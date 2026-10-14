#include "player.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mbb
{
namespace
{
constexpr int   kFade     = 96; // LIVE mode crossfade, samples (2 ms)
constexpr float kFadeStep = 1.0f / kFade;

// a rounded down to a multiple of b
double Floor(double a, double b)
{
    return std::floor(a / b) * b;
}

// Linear interpolation at a (wrapped) fractional frame position.
void ReadAt(const PlayBuf& b, double pos, float& l, float& r)
{
    pos -= Floor(pos, b.frames);
    uint32_t i = static_cast<uint32_t>(pos);
    if(i >= b.frames)
        i = 0;
    const uint32_t j = i + 1 < b.frames ? i + 1 : 0;
    const float    f = static_cast<float>(pos - i);
    const int16_t *a = b.data + 2 * i, *c = b.data + 2 * j;
    l = a[0] + f * (c[0] - a[0]);
    r = a[1] + f * (c[1] - a[1]);
}
} // namespace

void Player::Init(float sr)
{
    // SC Lag: y = x + b1 * (y - x), b1 = exp(log(0.001) / (lag * sr))
    lag_xf_   = static_cast<float>(std::exp(std::log(0.001) / (0.05 * sr)));
    lag_amp_  = static_cast<float>(std::exp(std::log(0.001) / (0.1 * sr)));
    vol_coef_ = static_cast<float>(1 - std::exp(-1 / (0.02 * sr)));
    // SC RLPF(18000, rq 1)
    const double pfreq = 18000 * 2 * 3.14159265358979323846 / sr;
    const double D     = std::tan(pfreq * 1.0 * 0.5);
    const double C     = (1 - D) / (1 + D);
    const double b1    = (1 + C) * std::cos(pfreq);
    b1_                = static_cast<float>(b1);
    b2_                = static_cast<float>(-C);
    a0_                = static_cast<float>((1 + C - b1) * 0.25);
}

void Player::TogglePlay()
{
    playing_ = !playing_;
    if(playing_)
    {
        // engine.amp(1); lattice_beats = -1; lattice:hard_restart()
        amp_target_ = 1;
        lattice_    = -1;
        to_tick_    = 0;
    }
    else
    {
        amp_target_ = 0; // engine.amp(0): the lattice keeps running
    }
}

int Player::ClaimFree()
{
    pending_ = -1;
    return active_ < 0 ? 0 : 1 - active_;
}

void Player::Publish(int idx, bool auto_play)
{
    const bool first = active_ < 0;
    pending_         = idx;
    if(first && auto_play && !playing_)
        TogglePlay();
}

void Player::Tick()
{
    if(pending_ >= 0)
    {
        // load_track: both readers move to the new buffer and keep their
        // position (scaled to the new tempo so they stay on the same beat).
        const int nb = pending_;
        pending_     = -1;
        if(active_ >= 0)
        {
            const PlayBuf& o = buf[active_];
            const PlayBuf& n = buf[nb];
            const double   r = n.fpb / o.fpb;
            pos_a_           = static_cast<uint32_t>(pos_a_ * r) % n.frames;
            pos_b_           = static_cast<uint32_t>(pos_b_ * r) % n.frames;
            // A different loop length would leave the bar count misaligned
            // (norns has one global length); restart the count instead.
            if(n.loop_beats != o.loop_beats)
                lattice_ = -1;
            if(n.fpb == o.fpb) // e.g. a new quarter (LIVE): 2 ms crossfade
            {
                swap_old_    = o.data;
                swap_frames_ = o.frames;
                swap_n_      = kFade;
            }
        }
        else
        {
            pos_a_ = pos_b_ = 0;
            a_or_b_         = 1;
            xf_             = 1;
            lattice_        = -1;
        }
        active_ = nb;
    }
    lattice_++;
    since_tick_ = 0;
    restarted_  = false;
    if(lattice_ % buf[active_].loop_beats == 0)
    {
        // engine.tozero(): flip, restart the newly selected reader
        a_or_b_ ^= 1;
        if(a_or_b_)
            pos_a_ = 0;
        else
            pos_b_ = 0;
        restarted_ = true;
    }
}

void Player::Quarter(const PlayBuf& b, int q, uint32_t& s0, uint32_t& s1)
{
    const double len = b.fpb * b.loop_beats / 4;
    s0               = static_cast<uint32_t>(q * len + 0.5);
    s1               = static_cast<uint32_t>((q + 1) * len + 0.5);
}

void Player::Splice(const PlayBuf& old, int16_t* dst, uint32_t s0, uint32_t s1, uint32_t n)
{
    constexpr uint32_t X   = 240;
    const uint32_t     end = std::min(s0 + n, old.frames);
    std::memcpy(dst, old.data, s0 * 4);
    // fade from old into new at s0, from new back into old at s1
    auto mix = [&](uint32_t at, bool to_new) {
        for(uint32_t i = 0; i < X && at + i < end; i++)
        {
            const float w = (i + 0.5f) / X;
            for(uint32_t k = 2 * (at + i); k < 2 * (at + i) + 2; k++)
            {
                const float o = old.data[k], nw = dst[k];
                dst[k]        = static_cast<int16_t>(std::lround(to_new ? o + w * (nw - o) : nw + w * (o - nw)));
            }
        }
    };
    mix(s0, true);
    if(end == old.frames)
        return;
    mix(s1, false);
    if(s1 + X < old.frames)
        std::memcpy(dst + 2 * (s1 + X), old.data + 2 * (s1 + X), (old.frames - s1 - X) * 4);
}

// LIVE: maps loop position p through quarter q's effect and reads the loop.
void Player::Live(const PlayBuf& b, uint32_t p, int q, float& l, float& r)
{
    const double fpb = b.fpb, ql = fpb * b.loop_beats / 4, q0 = q * ql;
    const double u    = std::max(0.0, p - q0); // position in the quarter
    double       x    = p;
    float        gain = 1;
    switch(fx_[q])
    {
        case kStutter:
            if(u >= ql / 2)
            {
                const double a = q0 + ql * 0.75, len = u < ql * 0.75 ? fpb / 2 : fpb / 4;
                x              = a + (p - a) - Floor(p - a, len);
            }
            break;
        case kReverse: x = 2 * Floor(p, fpb) + fpb - 1 - p; break;
        case kHalf:
        {
            const double s0 = q0 + Floor(u, 2 * fpb);
            x               = s0 + (p - s0) * 0.5;
            break;
        }
        case kTapeStop:
            x    = q0 + u - u * u / (2 * ql);
            gain = u < 0.75 * ql ? 1 : static_cast<float>((ql - u) / (0.25 * ql));
            break;
        default: break;
    }

    // Jumps (repeats, reversed chunks, loop restarts) are crossfaded: the old
    // read head keeps going while it fades out.
    const double n = b.frames;
    x -= Floor(x, n);
    double d = x - prev_;
    if(d > n / 2)
        d -= n;
    else if(d < -n / 2)
        d += n;
    if(settle_)
    {
        vel_    = d; // first step after a jump: learn the new speed
        settle_ = false;
    }
    else if(std::fabs(d - vel_) > 1.5)
    {
        fade_pos_ = prev_;
        fade_vel_ = vel_;
        fade_n_   = kFade;
        settle_   = true;
    }
    else
        vel_ = d;
    prev_ = x;

    ReadAt(b, x, l, r);
    if(fade_n_ > 0)
    {
        fade_pos_ += fade_vel_;
        float fl, fr;
        ReadAt(b, fade_pos_, fl, fr);
        const float w = fade_n_-- * kFadeStep;
        l += w * (fl - l);
        r += w * (fr - r);
    }
    live_gain_ += 0.02f * (gain - live_gain_);
    l *= live_gain_;
    r *= live_gain_;
}

void Player::Process(float* left, float* right, size_t n, float vol_target)
{
    constexpr float k = 1.0f / 32768.0f;
    for(size_t i = 0; i < n; i++)
    {
        float l = 0, r = 0;
        vol_ += vol_coef_ * (vol_target - vol_);
        if(active_ >= 0 || pending_ >= 0)
        {
            if(active_ < 0)
                to_tick_ = 0; // first buffer: start on this sample
            if(to_tick_ <= 0)
            {
                Tick();
                to_tick_ += buf[active_].fpb;
            }
            to_tick_ -= 1;
            if(since_tick_ != 0xffffffffu)
                since_tick_++;

            const PlayBuf& b = buf[active_];
            const uint32_t p = a_or_b_ ? pos_a_ : pos_b_; // the selected reader
            const int16_t* a = b.data + 2 * pos_a_;
            const int16_t* c = b.data + 2 * pos_b_;
            if(++pos_a_ >= b.frames)
                pos_a_ = 0;
            if(++pos_b_ >= b.frames)
                pos_b_ = 0;

            const float t = static_cast<float>(a_or_b_);
            xf_           = t + lag_xf_ * (xf_ - t);
            amp_          = amp_target_ + lag_amp_ * (amp_ - amp_target_);
            const float g = amp_ * vol_ * k;
            l             = (xf_ * a[0] + (1 - xf_) * c[0]) * g;
            r             = (xf_ * a[1] + (1 - xf_) * c[1]) * g;

            // LIVE: effects change on 16th notes; a quarter is loop_beats 16ths
            const int32_t q16 = static_cast<int32_t>(p / (b.fpb * 0.25));
            if(q16 != last_q_)
            {
                last_q_ = q16;
                for(int j = 0; j < 4; j++)
                    fx_[j] = req_[j];
            }
            const int  q  = std::min(3, static_cast<int>(q16 / std::max(1, b.loop_beats)));
            const bool on = fx_[q] != kClean;
            cur_q_        = static_cast<int8_t>(q);
            if(on || live_mix_ > 0)
            {
                if(live_mix_ == 0) // starting from the plain loop at p
                {
                    prev_      = p - 1.0;
                    vel_       = 1;
                    settle_    = false;
                    fade_n_    = 0;
                    live_gain_ = 1;
                }
                float ll, lr;
                Live(b, p, q, ll, lr);
                live_mix_ = on ? std::min(1.f, live_mix_ + kFadeStep)
                                : std::max(0.f, live_mix_ - kFadeStep);
                l += live_mix_ * (ll * g - l);
                r += live_mix_ * (lr * g - r);
            }
            if(swap_n_ > 0)
            {
                const float w = swap_n_-- * kFadeStep;
                if(p < swap_frames_)
                {
                    l += w * (swap_old_[2 * p] * g - l);
                    r += w * (swap_old_[2 * p + 1] * g - r);
                }
            }
        }
        if(aud_pos_ < aud_n_)
        {
            // short fades so a chop never clicks
            const uint32_t left = aud_n_ - aud_pos_;
            float          g    = vol_;
            if(aud_pos_ < 48)
                g *= aud_pos_ * (1.0f / 48);
            if(left < 240)
                g *= left * (1.0f / 240);
            l += aud_[2 * aud_pos_] * g;
            r += aud_[2 * aud_pos_ + 1] * g;
            aud_pos_++;
        }
        float* o[2]  = {left, right};
        float  in[2] = {l, r};
        for(int ch = 0; ch < 2; ch++)
        {
            const float y0 = a0_ * in[ch] + b1_ * y1_[ch] + b2_ * y2_[ch];
            o[ch][i]       = y0 + 2 * y1_[ch] + y2_[ch];
            y2_[ch]        = y1_[ch];
            y1_[ch]        = y0;
        }
    }
}

} // namespace mbb

#include "player.h"

#include <algorithm>
#include <cmath>

namespace mbb
{
namespace
{
constexpr int   kFade     = 96; // buffer swap crossfade, samples (2 ms)
constexpr float kFadeStep = 1.0f / kFade;
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
    return active_ == 0 ? 1 : 0;
}

void Player::Publish(int idx, bool auto_play, int sync, int beat)
{
    const bool first = active_ < 0;
    pending_         = idx;
    sync_            = sync;
    beat_            = beat;
    if(first && auto_play && !playing_)
        TogglePlay();
}

void Player::Tick()
{
    if(pending_ >= 0 && (active_ < 0 || sync_ <= 1 || (lattice_ + 1) % sync_ == 0))
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
            swap_pos_        = a_or_b_ ? pos_a_ : pos_b_;
            pos_a_           = static_cast<uint32_t>(pos_a_ * r) % n.frames;
            pos_b_           = static_cast<uint32_t>(pos_b_ * r) % n.frames;
            // A different loop length would leave the bar count misaligned
            // (norns has one global length); restart the count instead.
            if(n.loop_beats != o.loop_beats)
                lattice_ = -1;
            if(n.fpb == o.fpb) // e.g. focus quarters: 2 ms crossfade from the old buffer
            {
                swap_old_    = o.data;
                swap_frames_ = o.frames;
                swap_n_      = kFade;
            }
            if(beat_ >= 0) // play on from that beat (beat 0: tozero below)
            {
                lattice_ = beat_ - 1;
                pos_a_ = pos_b_ = static_cast<uint32_t>(std::lround(beat_ * n.fpb)) % n.frames;
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

void Player::Splice(int16_t* loop, uint32_t frames, const int16_t* v, uint32_t s0, uint32_t s1,
                    uint32_t n)
{
    constexpr uint32_t X    = 240;
    const uint32_t     have = std::min(s0 + n, frames);
    const bool         back = s1 + X <= have; // else the new audio is kept to its end
    const uint32_t     end  = back ? s1 + X : have;
    for(uint32_t i = s0; i < end; i++)
    {
        // fade in from the old audio at s0, back out to it at s1
        float w = 1;
        if(i < s0 + X)
            w = (i - s0 + 0.5f) / X;
        if(back && i >= s1)
            w = std::min(w, 1 - (i - s1 + 0.5f) / X);
        for(uint32_t k = 2 * i; k < 2 * i + 2; k++)
        {
            const int16_t nw = v[k - 2 * s0];
            const float   o  = loop[k];
            loop[k]          = w >= 1 ? nw : static_cast<int16_t>(std::lround(o + w * (nw - o)));
        }
    }
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

            const uint32_t q16 = static_cast<uint32_t>(p / (b.fpb * 0.25));
            cur_q_             = static_cast<int8_t>(std::min<uint32_t>(3, q16 / std::max(1, b.loop_beats)));
            if(swap_n_ > 0)
            {
                const float w = swap_n_-- * kFadeStep;
                if(swap_pos_ < swap_frames_)
                {
                    l += w * (swap_old_[2 * swap_pos_] * g - l);
                    r += w * (swap_old_[2 * swap_pos_ + 1] * g - r);
                }
                swap_pos_++;
            }
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

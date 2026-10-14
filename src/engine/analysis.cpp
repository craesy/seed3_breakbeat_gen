#include "analysis.h"
#include "dsp.h"

#include <cstring>

namespace mbb
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

// Minimal radix-2 FFT for N <= 256 (onset detection only).
constexpr uint32_t kMaxFft = 256;
float              g_re[kMaxFft], g_im[kMaxFft], g_win[kMaxFft], g_frame[kMaxFft];
float              g_cos[kMaxFft / 2], g_sin[kMaxFft / 2];
uint32_t           g_fft_n = 0;

void FftSetup(uint32_t n)
{
    if(g_fft_n == n)
        return;
    for(uint32_t i = 0; i < n; i++)
        g_win[i] = static_cast<float>(0.5 * (1.0 - std::cos(2.0 * kPi * i / n)));
    for(uint32_t i = 0; i < n / 2; i++)
    {
        g_cos[i] = static_cast<float>(std::cos(2.0 * kPi * i / n));
        g_sin[i] = static_cast<float>(-std::sin(2.0 * kPi * i / n));
    }
    g_fft_n = n;
}

void Fft(uint32_t n)
{
    for(uint32_t i = 1, j = 0; i < n; i++)
    {
        uint32_t bit = n >> 1;
        for(; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if(i < j)
        {
            float t = g_re[i];
            g_re[i] = g_re[j];
            g_re[j] = t;
            t       = g_im[i];
            g_im[i] = g_im[j];
            g_im[j] = t;
        }
    }
    for(uint32_t len = 2; len <= n; len <<= 1)
    {
        uint32_t half = len >> 1, tstep = n / len;
        for(uint32_t i = 0; i < n; i += len)
        {
            for(uint32_t k = 0; k < half; k++)
            {
                float wr = g_cos[k * tstep], wi = g_sin[k * tstep];
                float xr = g_re[i + k + half], xi = g_im[i + k + half];
                float tr = xr * wr - xi * wi;
                float ti = xr * wi + xi * wr;
                g_re[i + k + half] = g_re[i + k] - tr;
                g_im[i + k + half] = g_im[i + k] - ti;
                g_re[i + k] += tr;
                g_im[i + k] += ti;
            }
        }
    }
}

// aubio peak picker's (intentionally) first-order biquad, run forward then
// backward with fresh state each time.
void FiltFilt(float* x, int n)
{
    const float b0 = 0.15998789f, b1 = 0.31997577f, b2 = 0.15998789f;
    const float a1 = 0.23484048f, a2 = 0.0f;
    auto        run = [&](float* v) {
        float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        for(int i = 0; i < n; i++)
        {
            float x0 = v[i];
            float y  = b0 * x0;
            y += b1 * x1;
            y -= a1 * y1;
            y += b2 * x2;
            y -= a2 * y2;
            v[i] = y;
            x2   = x1;
            x1   = x0;
            y2   = y1;
            y1   = y;
        }
    };
    run(x);
    float tmp[16];
    for(int i = 0; i < n; i++)
        tmp[n - 1 - i] = x[i];
    run(tmp);
    for(int i = 0; i < n; i++)
        x[i] = tmp[n - 1 - i];
}

float Median7(const float* v)
{
    float s[7];
    std::memcpy(s, v, sizeof(s));
    for(int i = 1; i < 7; i++)
    {
        float k = s[i];
        int   j = i - 1;
        while(j >= 0 && s[j] > k)
        {
            s[j + 1] = s[j];
            j--;
        }
        s[j + 1] = k;
    }
    return s[3];
}

// Lua's tostring() keeps ~14 significant digits and aubioonset prints "%f";
// the onset times dnb.lua sees are rounded to microseconds.
double OnsetSeconds(uint32_t frames)
{
    return std::floor(static_cast<double>(frames) / kRate * 1e6 + 0.5) / 1e6;
}
} // namespace

bool Analyzer::Init(Arena& arena, uint32_t max_source_frames)
{
    max_hops_ = max_source_frames / 128 + 2;
    hfc_      = static_cast<float*>(arena.Alloc(max_hops_ * sizeof(float)));
    silent_   = static_cast<uint8_t*>(arena.Alloc(max_hops_));
    return hfc_ && silent_;
}

void Analyzer::ComputeNovelty(const Buf& src, uint32_t win, uint32_t hop, double silence_db)
{
    if(nov_src_ == src.d && nov_frames_ == src.n && nov_win_ == win && nov_hop_ == hop
       && nov_silence_ == silence_db)
        return; // the detection function does not depend on the threshold

    FftSetup(win);
    hops_ = src.n / hop + 1; // aubio also processes the final short block
    if(hops_ > max_hops_)
        hops_ = max_hops_;
    std::memset(g_frame, 0, sizeof(g_frame));
    for(uint32_t h = 0; h < hops_; h++)
    {
        // pvoc: slide the analysis buffer by one hop and append the new block
        std::memmove(g_frame, g_frame + hop, (win - hop) * sizeof(float));
        float*   blk   = g_frame + (win - hop);
        double   energy = 0.0;
        for(uint32_t i = 0; i < hop; i++)
        {
            uint32_t idx = h * hop + i;
            float    m   = 0.0f;
            if(idx < src.n)
            {
                const float* f = src.Frame(idx);
                m              = (f[0] + f[1]) * 0.5f; // aubio downmix
            }
            blk[i] = m;
            energy += static_cast<double>(m) * m;
        }
        double level = energy / hop;
        silent_[h]   = (level <= 0.0 || 10.0 * std::log10(level) < silence_db) ? 1 : 0;

        for(uint32_t i = 0; i < win; i++)
        {
            g_re[i] = g_frame[i] * g_win[i];
            g_im[i] = 0.0f;
        }
        Fft(win);
        float hfc = 0.0f;
        for(uint32_t k = 0; k <= win / 2; k++)
        {
            float mag = std::sqrt(g_re[k] * g_re[k] + g_im[k] * g_im[k]);
            mag       = std::log(mag + 1.0f); // hfc's default log compression (lambda 1)
            hfc += (k + 1) * mag;
        }
        hfc_[h] = hfc;
    }
    nov_src_     = src.d;
    nov_frames_  = src.n;
    nov_win_     = win;
    nov_hop_     = hop;
    nov_silence_ = silence_db;
}

uint32_t Analyzer::PickOnsets(uint32_t hop, double threshold, uint32_t minioi, uint32_t* out,
                              uint32_t max_out)
{
    const uint32_t delay = static_cast<uint32_t>(4.3 * hop);
    const float    thr   = static_cast<float>(threshold);
    float          keep[7] = {0}, peek[3] = {0}, proc[7];
    uint32_t       last_onset = 0, total = 0, n = 0;
    for(uint32_t h = 0; h < hops_; h++, total += hop)
    {
        std::memmove(keep, keep + 1, 6 * sizeof(float));
        keep[6] = hfc_[h];
        std::memcpy(proc, keep, sizeof(proc));
        FiltFilt(proc, 7);
        float mean = 0.0f;
        for(float v : proc)
            mean += v;
        mean /= 7.0f;
        float median = Median7(proc);
        peek[0]      = peek[1];
        peek[1]      = peek[2];
        peek[2]      = proc[5] - median - mean * thr;

        float isonset = 0.0f;
        if(peek[1] > peek[0] && peek[1] > peek[2] && peek[1] > 0.0f)
            isonset = 1.0f + 0.5f * (peek[0] - peek[2]) / (peek[0] - 2.0f * peek[1] + peek[2]);

        bool fired = false;
        if(isonset > 0.0f)
        {
            if(!silent_[h])
            {
                uint32_t new_onset
                    = total + static_cast<uint32_t>(std::floor(isonset * hop + 0.5f));
                if(last_onset + minioi < new_onset)
                {
                    if(!(last_onset > 0 && delay > new_onset))
                    {
                        last_onset = new_onset > delay ? new_onset : delay;
                        fired      = true;
                    }
                }
            }
        }
        else if(total <= delay && !silent_[h])
        {
            if(total == 0 || last_onset + minioi < total)
            {
                last_onset = total + delay;
                fired      = delay / hop > 0;
            }
        }
        if(fired && n < max_out)
            out[n++] = last_onset - delay;
    }
    return n;
}

uint32_t Analyzer::DetectOnsets(const Buf& src, uint32_t win, uint32_t hop, double threshold,
                                double minioi_sec, double silence_db, uint32_t* out,
                                uint32_t max_out)
{
    ComputeNovelty(src, win, hop, silence_db);
    const uint32_t minioi
        = static_cast<uint32_t>(std::floor(static_cast<float>(minioi_sec) * kRate + 0.5f));
    return PickOnsets(hop, threshold, minioi, out, max_out);
}

int Analyzer::DetectTempo(const Buf& src, double* exact)
{
    uint32_t n = DetectOnsets(src, 256, 128, 0.1, 0.05, -60.0, onsets_, kMaxOnsets);
    double   bpms_sum = 0, last = 0;
    uint32_t count    = 0;
    // two passes: mean/std, then trimmed mean (values strictly within 1.5 std)
    static double bpms[kMaxOnsets];
    for(uint32_t i = 0; i < n; i++)
    {
        double v = OnsetSeconds(onsets_[i]);
        if(v > 0)
        {
            double bpm = 60.0 / (v - last);
            while(bpm > 200)
                bpm /= 2;
            bpms[count++] = bpm;
            bpms_sum += bpm;
        }
        last = v;
    }
    if(count == 0)
        return 0;
    double mu  = bpms_sum / count;
    double var = 0;
    for(uint32_t i = 0; i < count; i++)
        var += (bpms[i] - mu) * (bpms[i] - mu);
    double   sd   = std::sqrt(var / count);
    double   tsum = 0;
    uint32_t tn   = 0;
    for(uint32_t i = 0; i < count; i++)
        if(bpms[i] > mu - sd * 1.5 && bpms[i] < mu + sd * 1.5)
        {
            tsum += bpms[i];
            tn++;
        }
    double t = tn ? tsum / tn : mu; // original would crash when tn == 0
    if(exact)
        *exact = t;
    return static_cast<int>(LuaRound(t));
}

void Analyzer::ClassifySlice(const Buf& src, Slice& s)
{
    const float* p = src.Frame(s.start);
    s.bd           = static_cast<float>(dsp::FilteredMeanNorm(p, s.len, false));
    s.sd           = static_cast<float>(dsp::FilteredMeanNorm(p, s.len, true));
    if(s.sd > 0.01f && s.sd > s.bd)
        s.cls = kSliceSnare;
    else if(s.bd > 0.02f)
        s.cls = kSliceKick;
    else
        s.cls = kSliceNone;
}

uint32_t Analyzer::SliceSource(const Buf& src, double tempo)
{
    n_slices_      = 0;
    n_onsets_      = 0;
    grid_fallback_ = false;
    equal_         = false;
    if(src.n == 0 || tempo <= 0)
        return 0;

    const double minioi    = 60.0 / tempo / 4.0;
    double       threshold = 1.0;
    while(n_onsets_ < 4 && threshold > 0)
    {
        n_onsets_ = DetectOnsets(src, 128, 128, threshold, minioi, -60.0, onsets_, kMaxOnsets);
        threshold_used_ = threshold;
        threshold -= 0.1;
    }

    if(n_onsets_ >= 2)
    {
        for(uint32_t k = 1; k < n_onsets_ && n_slices_ < kMaxSlices; k++)
        {
            Slice& s = slices_[n_slices_];
            s.start  = onsets_[k - 1];
            if(s.start >= src.n)
                break;
            s.len = (k == n_onsets_ - 1) ? src.n - s.start : onsets_[k] - onsets_[k - 1];
            if(s.start + s.len > src.n)
                s.len = src.n - s.start;
            if(s.len == 0)
                continue;
            ClassifySlice(src, s);
            n_slices_++;
        }
    }
    if(n_slices_ == 0)
    {
        // Fallback (the original crashes here): 16th-note grid.
        grid_fallback_       = true;
        const uint32_t step  = SecToFrames(minioi);
        for(uint32_t start = 0; start < src.n && n_slices_ < kMaxSlices && step; start += step)
        {
            Slice& s = slices_[n_slices_++];
            s.start  = start;
            s.len    = (src.n - start < step) ? src.n - start : step;
            ClassifySlice(src, s);
        }
    }
    return n_slices_;
}

uint32_t Analyzer::ChopEqual(const Buf& src, uint32_t n)
{
    n_slices_ = 0;
    equal_    = true;
    if(n > kMaxSlices)
        n = kMaxSlices;
    for(uint32_t k = 0; k < n; k++)
    {
        const uint32_t a = static_cast<uint32_t>(static_cast<uint64_t>(src.n) * k / n);
        const uint32_t b = static_cast<uint32_t>(static_cast<uint64_t>(src.n) * (k + 1) / n);
        if(b <= a)
            continue;
        Slice& s = slices_[n_slices_++];
        s.start  = a;
        s.len    = b - a;
        ClassifySlice(src, s);
    }
    return n_slices_;
}

} // namespace mbb

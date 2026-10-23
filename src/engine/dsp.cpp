#include "dsp.h"

#include <cstring>

namespace mbb
{
namespace dsp
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

inline float ClipF(float x)
{
    return x > 1.0f ? 1.0f : (x < -1.0f ? -1.0f : x);
}

// Single-precision sin for |x| < ~100: libm's double sin costs hundreds of
// cycles on the Cortex-M7. Error < 2e-7.
inline float SinF(float x)
{
    const float k = static_cast<float>(static_cast<int>(x * 0.15915494f + (x >= 0 ? 0.5f : -0.5f)));
    x = (x - k * 6.28125f) - k * 1.9353072e-3f; // 2*pi split for exact reduction
    if(x > 1.5707964f)
        x = 3.1415927f - x;
    else if(x < -1.5707964f)
        x = -3.1415927f - x;
    const float x2 = x * x;
    return x
           * (1.0f
              + x2
                    * (-1.6666667e-1f
                       + x2
                             * (8.3333333e-3f
                                + x2 * (-1.9841270e-4f + x2 * (2.7557319e-6f + x2 * -2.5052108e-8f)))));
}

// Sum of squared differences over n floats. Stops once it exceeds `bound`
// (sums only grow, so the full sum would exceed it too).
inline float SqDiff(const float* a, const float* b, uint32_t n, float bound)
{
    float    sum = 0.0f;
    uint32_t j   = 0;
    for(; j + 16 <= n; j += 16)
    {
        for(uint32_t k = j; k < j + 16; k++)
        {
            const float d = a[k] - b[k];
            sum += d * d;
        }
        if(sum > bound)
            return sum;
    }
    for(; j < n; j++)
    {
        const float d = a[j] - b[j];
        sum += d * d;
    }
    return sum;
}

// The first frame offset i in [0, count) minimising SqDiff(base + i frames,
// ref, n). A coarse pass (every 8th frame) picks a likely winner first so the
// exact pass can abandon most candidates early; the result is unchanged.
uint32_t BestMatch(const float* base, const float* ref, uint32_t n, uint32_t count)
{
    uint32_t guess = 0;
    float    coarse = 3.4e38f;
    for(uint32_t i = 0; i < count; i++)
    {
        const float* a = base + i * kChannels;
        float        s = 0.0f;
        for(uint32_t j = 0; j + 1 < n; j += 16)
        {
            const float d0 = a[j] - ref[j], d1 = a[j + 1] - ref[j + 1];
            s += d0 * d0 + d1 * d1;
        }
        if(s < coarse)
        {
            coarse = s;
            guess  = i;
        }
    }
    uint32_t best     = guess;
    float    best_err = SqDiff(base + guess * kChannels, ref, n, 3.4e38f);
    for(uint32_t i = 0; i < count; i++)
    {
        if(i == guess)
            continue;
        const float e = SqDiff(base + i * kChannels, ref, n, best_err);
        if(e < best_err || (e == best_err && i < best))
        {
            best_err = e;
            best     = i;
        }
    }
    return best;
}


// ---------------------------------------------------------------- biquad
struct Biquad
{
    double b0, b1, b2, a1, a2;
    double x1[2] = {0, 0}, x2[2] = {0, 0}, y1[2] = {0, 0}, y2[2] = {0, 0};

    void Design(FilterType type, double freq)
    {
        const double w0    = 2.0 * kPi * freq / kRate;
        const double cw    = std::cos(w0);
        const double alpha = std::sin(w0) / (2.0 * 0.70710678118654752);
        const double a0    = 1.0 + alpha;
        if(type == FilterType::LowPass)
        {
            b0 = (1.0 - cw) / 2.0;
            b1 = 1.0 - cw;
            b2 = (1.0 - cw) / 2.0;
        }
        else
        {
            b0 = (1.0 + cw) / 2.0;
            b1 = -(1.0 + cw);
            b2 = (1.0 + cw) / 2.0;
        }
        b0 /= a0;
        b1 /= a0;
        b2 /= a0;
        a1 = -2.0 * cw / a0;
        a2 = (1.0 - alpha) / a0;
    }
    inline double Tick(int c, double x)
    {
        double y = b0 * x + b1 * x1[c] + b2 * x2[c] - a1 * y1[c] - a2 * y2[c];
        x2[c]    = x1[c];
        x1[c]    = x;
        y2[c]    = y1[c];
        y1[c]    = y;
        return y;
    }
};

// ------------------------------------------------------------- resampler
// Kaiser-windowed sinc with a linearly interpolated table, approximating
// sox's default linear-phase `rate` (passband ~95% of Nyquist).
#ifndef MBB_RS_ZEROS
#define MBB_RS_ZEROS 64
#endif
#ifndef MBB_RS_BETA
#define MBB_RS_BETA 12.0
#endif
#ifndef MBB_RS_FC
#define MBB_RS_FC 0.958
#endif
#ifndef MBB_RS_OVER
#define MBB_RS_OVER 128
#endif
constexpr int    kZeros   = MBB_RS_ZEROS;
constexpr int    kOver    = MBB_RS_OVER;
constexpr double kBandEdge = MBB_RS_FC; // sinc cutoff relative to the lower Nyquist
constexpr int kTabSize = kZeros * kOver + 2;
float         g_kernel[kTabSize];
bool          g_kernel_ready = false;

double BesselI0(double x)
{
    double sum = 1.0, term = 1.0, q = x * x / 4.0;
    for(int k = 1; k < 40; k++)
    {
        term *= q / (static_cast<double>(k) * k);
        sum += term;
        if(term < sum * 1e-12)
            break;
    }
    return sum;
}

void BuildKernel()
{
    if(g_kernel_ready)
        return;
    const double beta = MBB_RS_BETA;
    const double norm = BesselI0(beta);
    for(int i = 0; i < kTabSize; i++)
    {
        double t = static_cast<double>(i) / kOver; // in zero crossings
        double s = t == 0.0 ? 1.0 : std::sin(kPi * t) / (kPi * t);
        double r = t / kZeros;
        double w = r >= 1.0 ? 0.0 : BesselI0(beta * std::sqrt(1.0 - r * r)) / norm;
        g_kernel[i] = static_cast<float>(s * w);
    }
    g_kernel_ready = true;
}

// Computes one stereo output frame at input position `center`. Taps step
// through the kernel table in single precision (`ck` = cutoff * kOver).
inline void ResamplePoint(const Buf& in, double center, double cutoff, float ck, float& l, float& r)
{
    const double half = kZeros / cutoff;
    int64_t      i0   = static_cast<int64_t>(std::ceil(center - half));
    int64_t      i1   = static_cast<int64_t>(std::floor(center + half));
    if(i0 < 0)
        i0 = 0;
    if(i1 > static_cast<int64_t>(in.n) - 1)
        i1 = static_cast<int64_t>(in.n) - 1;
    float sl = 0.0f, sr = 0.0f;
    if(i1 >= i0)
    {
        const float  p0  = static_cast<float>((center - static_cast<double>(i0)) * cutoff * kOver);
        const float  lim = static_cast<float>(kZeros * kOver);
        const float* f   = in.d + i0 * kChannels;
        const int    n   = static_cast<int>(i1 - i0) + 1;
        for(int t = 0; t < n; t++, f += kChannels)
        {
            const float pos = std::fabs(p0 - static_cast<float>(t) * ck);
            if(pos >= lim)
                continue;
            const int   k = static_cast<int>(pos);
            const float w = g_kernel[k] + (g_kernel[k + 1] - g_kernel[k]) * (pos - static_cast<float>(k));
            sl += f[0] * w;
            sr += f[1] * w;
        }
    }
    l = sl * static_cast<float>(cutoff);
    r = sr * static_cast<float>(cutoff);
}

// pow() is avoided throughout: its tables cost ~6 KB of the Seed's 128 KB
// flash. These forms are what sox itself uses (dB_to_linear is exp-based).
constexpr double kLn10 = 2.30258509299404568402;
constexpr double kLn2  = 0.69314718055994530942;

// Exact for |k| <= 22, like pow(10, k).
double Pow10(int k)
{
    double r = 1;
    for(int i = 0; i < (k < 0 ? -k : k); i++)
        r *= 10;
    return k < 0 ? 1 / r : r;
}

// sox prints the pitch's tempo factor with "%g" (6 significant digits).
double RoundSig6(double x)
{
    if(x == 0.0)
        return 0.0;
    double e     = std::floor(std::log10(std::fabs(x)));
    double scale = Pow10(5 - static_cast<int>(e));
    return std::nearbyint(x * scale) / scale;
}

// Scratch for stretch/tempo (static: no stack use, and outside DTCM).
constexpr uint32_t kStretchSeg = 960;
float              g_st_ibuf[kStretchSeg * kChannels];
double             g_st_obuf[kStretchSeg * kChannels];

constexpr uint32_t kTempoSeg     = 3936;
constexpr uint32_t kTempoSearch  = 704;
constexpr uint32_t kTempoOverlap = 576;
float              g_tp_win[(kTempoSeg + kTempoSearch + 8) * kChannels];
float              g_tp_overlap[kTempoOverlap * kChannels];
float              g_tp_tmp[kTempoOverlap * kChannels];
double             g_st_coefs[kStretchSeg];

} // namespace

void Clip(Buf& b)
{
    float* p = b.d;
    for(uint32_t i = 0; i < b.n * kChannels; i++)
        p[i] = ClipF(p[i]);
}

void Copy(const Buf& in, Buf& out)
{
    uint32_t n = in.n < out.cap ? in.n : out.cap;
    if(out.d != in.d)
        std::memmove(out.d, in.d, static_cast<size_t>(n) * kChannels * sizeof(float));
    out.n = n;
}

void Gain(Buf& b, double db)
{
    if(db == 0.0)
        return;
    float g = static_cast<float>(std::exp(db * kLn10 * 0.05));
    for(uint32_t i = 0; i < b.n * kChannels; i++)
        b.d[i] = ClipF(b.d[i] * g);
}

void Reverse(Buf& b)
{
    if(b.n < 2)
        return;
    for(uint32_t i = 0, j = b.n - 1; i < j; i++, j--)
    {
        float* a = b.Frame(i);
        float* c = b.Frame(j);
        float  t0 = a[0], t1 = a[1];
        a[0] = c[0];
        a[1] = c[1];
        c[0] = t0;
        c[1] = t1;
    }
}

void Trim(Buf& b, uint32_t frames)
{
    if(frames < b.n)
        b.n = frames;
}

void PadEnd(Buf& b, uint32_t frames)
{
    uint32_t n = b.n + frames;
    if(n > b.cap)
        n = b.cap;
    std::memset(b.Frame(b.n), 0, static_cast<size_t>(n - b.n) * kChannels * sizeof(float));
    b.n = n;
}

void Mix2(const Buf& a, const Buf& b, Buf& out, uint32_t trim_frames)
{
    uint32_t n = a.n > b.n ? a.n : b.n;
    if(n > trim_frames)
        n = trim_frames;
    if(n > out.cap)
        n = out.cap;
    for(uint32_t i = 0; i < n; i++)
    {
        float* o = out.Frame(i);
        for(int c = 0; c < kChannels; c++)
        {
            float x = (i < a.n ? a.Frame(i)[c] : 0.0f) * 0.5f
                      + (i < b.n ? b.Frame(i)[c] : 0.0f) * 0.5f;
            o[c] = ClipF(x);
        }
    }
    out.n = n;
}

void Filter(Buf& b, FilterType type, double freq)
{
    Biquad q;
    q.Design(type, freq);
    for(uint32_t i = 0; i < b.n; i++)
    {
        float* f = b.Frame(i);
        for(int c = 0; c < kChannels; c++)
            f[c] = ClipF(static_cast<float>(q.Tick(c, f[c])));
    }
}

double FilteredMeanNorm(const float* frames, uint32_t n, bool band)
{
    // band=false: `lowpass 200`; band=true: `lowpass 400 highpass 200`.
    Biquad lp, hp;
    lp.Design(FilterType::LowPass, band ? 400.0 : 200.0);
    hp.Design(FilterType::HighPass, 200.0);
    double sum = 0.0;
    for(uint32_t i = 0; i < n; i++)
    {
        for(int c = 0; c < kChannels; c++)
        {
            double y = lp.Tick(c, frames[i * kChannels + c]);
            if(band)
            {
                // sox clips to full scale between chained effects.
                y = hp.Tick(c, y > 1.0 ? 1.0 : (y < -1.0 ? -1.0 : y));
            }
            double a = std::fabs(y);
            sum += a > 1.0 ? 1.0 : a;
        }
    }
    return n ? sum / (static_cast<double>(n) * kChannels) : 0.0;
}

void Contrast(Buf& b)
{
    const float amount = 75.0f / 750.0f;
    for(uint32_t i = 0; i < b.n * kChannels; i++)
    {
        const float d = b.d[i] * 1.5707964f;
        b.d[i]        = SinF(d + amount * SinF(d * 4.0f));
    }
}

double MeanNorm(const Buf& b)
{
    double sum = 0.0;
    for(uint32_t i = 0; i < b.n * kChannels; i++)
        sum += std::fabs(b.d[i]);
    return b.n ? sum / (static_cast<double>(b.n) * kChannels) : 0.0;
}

void SilentEnd(const Buf& in, Buf& out, double silence_sec)
{
    uint32_t e = SecToFrames(in.Seconds() - silence_sec);
    if(e > in.n)
        e = in.n;
    if(e > out.cap)
        e = out.cap;
    const uint32_t fade = SecToFrames(0.005);
    for(uint32_t s = 0; s < e; s++)
    {
        const float* x    = in.Frame(s);
        float*       y    = out.Frame(s);
        float        gain = 1.0f;
        if(s + fade >= e)
        {
            double idx = static_cast<double>(e - s) / fade;
            if(idx > 1.0)
                idx = 1.0;
            gain = static_cast<float>(std::exp(-(1.0 - idx) * 5.0 * kLn10));
        }
        y[0] = x[0] * gain;
        y[1] = x[1] * gain;
    }
    out.n = e;
    PadEnd(out, SecToFrames(silence_sec));
}

uint32_t SpliceOverlap(double excess_sec)
{
    uint32_t ov = excess_sec > 0 ? 2 * SecToFrames(excess_sec)
                                 : static_cast<uint32_t>(kRate * 0.01 + 0.5);
    ov += 4;
    if(ov < 16)
        ov = 16;
    return ov & ~7u;
}

uint32_t SpliceSearch(double leeway_sec)
{
    return leeway_sec > 0 ? 2 * SecToFrames(leeway_sec)
                          : static_cast<uint32_t>(kRate * 0.01 + 0.5);
}

bool SpliceInto(Buf& a, const Buf& b, uint32_t ov, uint32_t search)
{
    const uint32_t p = a.n;
    if(p < ov)
        return false;
    if(b.n <= ov + search)
    {
        // sox never fills its splice buffer, so only a[0, p-ov) comes out.
        a.n = p - ov;
        return true;
    }
    const float* fa       = a.Frame(p - ov);
    const uint32_t best = BestMatch(b.d, fa, ov * kChannels, search);
    float*       xa   = a.Frame(p - ov);
    const float* xb   = b.Frame(best);
    const double step = kPi / ov;
    const double cs = std::cos(step), sn = std::sin(step);
    double       co = 1.0, si = 0.0; // cos/sin(i * step) by rotation
    for(uint32_t i = 0; i < ov; i++)
    {
        double fin = 0.5 - 0.5 * co;
        double nc  = co * cs - si * sn;
        si         = si * cs + co * sn;
        co         = nc;
        for(int c = 0; c < kChannels; c++)
        {
            double v = xa[i * kChannels + c] * (1.0 - fin) + xb[i * kChannels + c] * fin;
            xa[i * kChannels + c] = ClipF(static_cast<float>(v));
        }
    }
    uint32_t src  = best + ov;
    uint32_t dst  = p;
    uint32_t tail = b.n - src;
    if(dst + tail > a.cap)
        tail = a.cap - dst;
    std::memcpy(a.Frame(dst), b.Frame(src), static_cast<size_t>(tail) * kChannels * sizeof(float));
    a.n = dst + tail;
    return true;
}

void Stretch(const Buf& in, Buf& out, double factor)
{
    out.n = 0;
    if(factor == 1.0)
    {
        Copy(in, out);
        return;
    }
    const uint32_t seg = kStretchSeg;
    uint32_t       ishift, oshift;
    if(factor < 1.0)
    {
        ishift = static_cast<uint32_t>(1.0 * seg);
        oshift = static_cast<uint32_t>(factor * ishift);
    }
    else
    {
        oshift = static_cast<uint32_t>(0.8 * seg);
        ishift = static_cast<uint32_t>(oshift / factor);
    }
    if(ishift == 0)
        ishift = 1;
    const double   fading = factor < 1.0 ? 0.0 : 1.0 - 0.8;
    const uint32_t ov     = static_cast<uint32_t>(fading * seg);
    double*        coefs  = g_st_coefs;
    if(ov > 1)
    {
        double slope = 1.0 / (ov - 1);
        coefs[0]     = 1.0;
        for(uint32_t i = 1; i < ov - 1; i++)
            coefs[i] = slope * (ov - i - 1);
        coefs[ov - 1] = 0.0;
    }
    else if(ov == 1)
        coefs[0] = 1.0;

    uint32_t index  = seg / 2;
    uint32_t oindex = index;
    std::memset(g_st_ibuf, 0, sizeof(g_st_ibuf));
    std::memset(g_st_obuf, 0, sizeof(g_st_obuf));

    auto combine = [&]() {
        for(uint32_t i = 0; i < seg; i++)
        {
            double c;
            if(i < ov)
                c = coefs[ov - 1 - i];
            else if(i < seg - ov)
                c = 1.0;
            else
                c = coefs[i - seg + ov];
            g_st_obuf[i * 2] += c * g_st_ibuf[i * 2];
            g_st_obuf[i * 2 + 1] += c * g_st_ibuf[i * 2 + 1];
        }
    };
    auto emit = [&](uint32_t from, uint32_t to) -> bool {
        for(uint32_t i = from; i < to; i++)
        {
            if(out.n >= out.cap)
                return false;
            float* o = out.Frame(out.n++);
            o[0]     = ClipF(static_cast<float>(g_st_obuf[i * 2]));
            o[1]     = ClipF(static_cast<float>(g_st_obuf[i * 2 + 1]));
        }
        return true;
    };

    uint32_t iin = 0;
    while(iin < in.n)
    {
        uint32_t tocopy = seg - index;
        if(tocopy > in.n - iin)
            tocopy = in.n - iin;
        std::memcpy(&g_st_ibuf[index * 2], in.Frame(iin), tocopy * kChannels * sizeof(float));
        iin += tocopy;
        index += tocopy;
        if(index == seg)
        {
            combine();
            std::memmove(g_st_ibuf, &g_st_ibuf[ishift * 2], (seg - ishift) * kChannels * sizeof(float));
            index -= ishift;
            if(!emit(oindex, oshift))
                return;
            oindex = 0;
            std::memmove(g_st_obuf, &g_st_obuf[oshift * 2], (seg - oshift) * kChannels * sizeof(double));
            for(uint32_t i = seg - oshift; i < seg; i++)
                g_st_obuf[i * 2] = g_st_obuf[i * 2 + 1] = 0.0;
        }
    }
    // drain (input state)
    for(uint32_t i = index; i < seg; i++)
        g_st_ibuf[i * 2] = g_st_ibuf[i * 2 + 1] = 0.0f;
    combine();
    if(oindex < index)
        emit(oindex, index);
}

void Tempo(const Buf& in, Buf& out, double factor, uint32_t max_out)
{
    out.n = 0;
    if(factor == 1.0)
    {
        Copy(in, out);
        if(max_out)
            Trim(out, max_out);
        return;
    }
    // Default profile: segment 82 ms, search 82/5.587 ms, overlap 82/6.833 ms.
    const double   seg_ms = 82.0, search_ms = 82.0 / 5.587, ov_ms = 82.0 / 6.833;
    const uint32_t seg    = static_cast<uint32_t>(kRate * seg_ms / 1000 + .5);
    const uint32_t search = static_cast<uint32_t>(kRate * search_ms / 1000 + .5);
    double         ovd    = kRate * ov_ms / 1000 + 4.5;
    uint32_t       ov     = static_cast<uint32_t>(ovd > 16 ? ovd : 16);
    ov &= ~7u;
    if(ov * 2 > seg)
        ov -= 8;
    // These match the static scratch sizes above for 48 kHz.
    if(seg != kTempoSeg || search != kTempoSearch || ov != kTempoOverlap)
        return;

    const uint32_t pre    = search / 2;
    uint64_t       target = static_cast<uint64_t>(in.n / factor + .5);
    if(max_out && target > max_out)
        target = max_out;
    if(target > out.cap)
        target = out.cap;

    uint64_t       segs = 0, skip_total = 0;
    const uint32_t win  = search + seg;
    while(out.n < target)
    {
        // Load the window [skip_total, skip_total + win) of the virtual
        // stream (pre zeros, input, then zeros).
        for(uint32_t k = 0; k < win; k++)
        {
            int64_t src = static_cast<int64_t>(skip_total + k) - pre;
            float*  w   = &g_tp_win[k * 2];
            if(src >= 0 && src < static_cast<int64_t>(in.n))
            {
                const float* f = in.Frame(static_cast<uint32_t>(src));
                w[0]           = f[0];
                w[1]           = f[1];
            }
            else
                w[0] = w[1] = 0.0f;
        }
        uint32_t offset;
        float*   tmp = g_tp_tmp;
        if(segs == 0)
        {
            offset = search / 2;
            std::memcpy(tmp, &g_tp_win[offset * 2], ov * kChannels * sizeof(float));
        }
        else
        {
            offset = BestMatch(g_tp_win, g_tp_overlap, ov * kChannels, search);
            const float step = 1.0f / static_cast<float>(ov);
            for(uint32_t i = 0; i < ov; i++)
            {
                float fin  = step * static_cast<float>(i);
                float fout = 1.0f - fin;
                for(int c = 0; c < kChannels; c++)
                    tmp[i * 2 + c] = g_tp_overlap[i * 2 + c] * fout
                                     + g_tp_win[(offset + i) * 2 + c] * fin;
            }
        }
        auto write = [&](const float* src, uint32_t frames) {
            for(uint32_t i = 0; i < frames && out.n < out.cap; i++)
            {
                float* o = out.Frame(out.n++);
                o[0]     = src[i * 2];
                o[1]     = src[i * 2 + 1];
            }
        };
        write(tmp, ov);
        write(&g_tp_win[(offset + ov) * 2], seg - 2 * ov);
        std::memcpy(g_tp_overlap, &g_tp_win[(offset + seg - ov) * 2], ov * kChannels * sizeof(float));
        uint64_t skip = static_cast<uint64_t>(factor * (++segs * (seg - ov)) + 0.5);
        skip_total    = skip;
        if(out.n >= out.cap)
            break;
    }
    if(out.n > target)
        out.n = static_cast<uint32_t>(target);
    Clip(out);
}

void Resample(const Buf& in, Buf& out, double step, uint32_t out_frames)
{
    BuildKernel();
    if(out_frames > out.cap)
        out_frames = out.cap;
    const double cutoff = (step > 1.0 ? 1.0 / step : 1.0) * kBandEdge;
    const float  ck     = static_cast<float>(cutoff * kOver);
    for(uint32_t j = 0; j < out_frames; j++)
    {
        float l, r;
        ResamplePoint(in, j * step, cutoff, ck, l, r);
        float* o = out.Frame(j);
        o[0]     = ClipF(l);
        o[1]     = ClipF(r);
    }
    out.n = out_frames;
}

void ResampleToI16(const Buf& in, int16_t* out, double step, uint32_t out_frames)
{
    BuildKernel();
    const double cutoff = (step > 1.0 ? 1.0 / step : 1.0) * kBandEdge;
    const float  ck     = static_cast<float>(cutoff * kOver);
    for(uint32_t j = 0; j < out_frames; j++)
    {
        float l, r;
        ResamplePoint(in, j * step, cutoff, ck, l, r);
        out[j * 2]     = ToI16(l);
        out[j * 2 + 1] = ToI16(r);
    }
}

void Speed(const Buf& in, Buf& out, double factor)
{
    if(factor == 1.0)
    {
        Copy(in, out);
        return;
    }
    uint32_t n = static_cast<uint32_t>(in.n / factor + 0.5);
    Resample(in, out, factor, n);
}

void Pitch(const Buf& in, Buf& out, Buf& tmp, double cents, double trim_sec)
{
    if(cents == 0.0)
    {
        // tempo factor 1 and an unchanged rate are both null effects
        Copy(in, out);
        if(trim_sec >= 0)
            Trim(out, SecToFrames(trim_sec));
        return;
    }
    const double tf    = RoundSig6(std::exp(-cents / 1200.0 * kLn2));
    uint32_t     len_t = static_cast<uint32_t>(in.n / tf + .5);
    if(trim_sec >= 0)
    {
        const uint32_t cut = static_cast<uint32_t>(trim_sec * (kRate / tf) + .5);
        if(cut < len_t)
            len_t = cut;
    }
    const uint32_t out_len = static_cast<uint32_t>(len_t * tf + .5);
    Tempo(in, tmp, tf, len_t);
    if(tmp.n > len_t)
        tmp.n = len_t;
    Resample(tmp, out, 1.0 / tf, out_len);
}

// ------------------------------------------------------------------ reverb
bool Reverb::Init(Arena& arena)
{
    static const uint32_t comb_len[8]    = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
    static const uint32_t allpass_len[4] = {225, 341, 441, 556};
    const double          r              = kRate / 44100.0;
    const double          scale          = 100.0 / 100 * .9 + .1;
    for(int ch = 0; ch < 2; ch++)
    {
        for(int arr = 0; arr < 2; arr++)
        {
            double offset = arr; // stereo depth 100% -> 1 for the 2nd array
            Array& a      = arrays_[ch][arr];
            for(int i = 0; i < 8; i++, offset = -offset)
            {
                Filter& f = a.comb[i];
                f.size    = static_cast<uint32_t>(scale * r * (comb_len[i] + 12 * offset) + .5);
                f.buf     = static_cast<float*>(arena.Alloc(f.size * sizeof(float)));
                if(!f.buf)
                    return false;
            }
            for(int i = 0; i < 4; i++, offset = -offset)
            {
                Filter& f = a.allpass[i];
                f.size    = static_cast<uint32_t>(r * (allpass_len[i] + 12 * offset) + .5);
                f.buf     = static_cast<float*>(arena.Alloc(f.size * sizeof(float)));
                if(!f.buf)
                    return false;
            }
        }
    }
    const double reverberance = 50, hf_damping = 50, wet_gain_db = 0;
    const double a = -1 / std::log(1 - .3);
    const double b = 100 / (std::log(1 - .98) * a + 1);
    feedback_      = static_cast<float>(1 - std::exp((reverberance - b) / (a * b)));
    damping_       = static_cast<float>(hf_damping / 100 * .3 + .2);
    gain_          = static_cast<float>(std::exp(wet_gain_db * kLn10 * 0.05) * .015);
    return true;
}

void Reverb::Reset()
{
    for(auto& ch : arrays_)
        for(auto& a : ch)
        {
            for(auto& f : a.comb)
            {
                std::memset(f.buf, 0, f.size * sizeof(float));
                f.pos   = 0;
                f.store = 0;
            }
            for(auto& f : a.allpass)
            {
                std::memset(f.buf, 0, f.size * sizeof(float));
                f.pos   = 0;
                f.store = 0;
            }
        }
}

float Reverb::Run(Array& a, float in)
{
    float out = 0;
    for(int i = 7; i >= 0; i--)
    {
        Filter& f      = a.comb[i];
        float   output = f.buf[f.pos];
        f.store        = output + (f.store - output) * damping_;
        f.buf[f.pos]   = in + f.store * feedback_;
        f.pos          = f.pos == 0 ? f.size - 1 : f.pos - 1;
        out += output;
    }
    for(int i = 3; i >= 0; i--)
    {
        Filter& f      = a.allpass[i];
        float   output = f.buf[f.pos];
        f.buf[f.pos]   = out + output * .5f;
        f.pos          = f.pos == 0 ? f.size - 1 : f.pos - 1;
        out            = output - out;
    }
    return out * gain_;
}

void Reverb::Process(Buf& b)
{
    Reset();
    for(uint32_t i = 0; i < b.n; i++)
    {
        float* f      = b.Frame(i);
        float  dry[2] = {f[0], f[1]};
        float  wet[2][2];
        for(int c = 0; c < 2; c++)
            for(int w = 0; w < 2; w++)
                wet[c][w] = Run(arrays_[c][w], dry[c]);
        for(int w = 0; w < 2; w++)
            f[w] = ClipF(dry[w] + .5f * (wet[0][w] + wet[1][w]));
    }
}

} // namespace dsp
} // namespace mbb

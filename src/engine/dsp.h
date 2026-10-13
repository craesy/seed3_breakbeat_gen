// Re-implementations of the SoX effects that makebreakbeat's dnb.lua uses.
// Each function documents the sox command it stands in for. Buffers are
// 48 kHz stereo interleaved float; outputs are clamped to their capacity.
#pragma once

#include "mbb_core.h"

namespace mbb
{
namespace dsp
{
// sox clips to [-1, 1] when it writes every intermediate file.
void Clip(Buf& b);
void Copy(const Buf& in, Buf& out);
// `gain <db>`
void Gain(Buf& b, double db);
// `reverse`
void Reverse(Buf& b);
// `trim 0 <frames>`
void Trim(Buf& b, uint32_t frames);
// `pad 0 <frames>`
void PadEnd(Buf& b, uint32_t frames);
// `sox -m a b out trim 0 <frames>`: 1/2 gain per input, longest length.
void Mix2(const Buf& a, const Buf& b, Buf& out, uint32_t trim_frames);

enum class FilterType
{
    LowPass,
    HighPass
};
// `lowpass <f>` / `highpass <f>`: RBJ two-pole, Q = 0.7071.
void Filter(Buf& b, FilterType type, double freq);
// Streaming variant used for slice classification (no buffer needed):
// returns sox's "Mean norm" of the filtered (and clipped) slice.
double FilteredMeanNorm(const float* frames, uint32_t n, bool band);
// `contrast` (default amount 75).
void Contrast(Buf& b);
// sox `stat` "Mean norm".
double MeanNorm(const Buf& b);

// dnb.lua audio.silent_end(): `fade 0 <len-sl> 0.005` + `sl` seconds of
// silence appended.
void SilentEnd(const Buf& in, Buf& out, double silence_sec);

// `splice <len(a)>[,excess,leeway]` joining a and b; result left in `a`.
// overlap/search in frames as sox computes them. Returns false when sox
// would fail (a shorter than the overlap), leaving `a` untouched.
bool SpliceInto(Buf& a, const Buf& b, uint32_t overlap, uint32_t search);
// sox defaults: `splice p` -> 480/480, `splice p,0.005,0.001` -> 480/96.
uint32_t SpliceOverlap(double excess_sec);
uint32_t SpliceSearch(double leeway_sec);

// `stretch <factor>` (factor > 1 in all dnb.lua uses).
void Stretch(const Buf& in, Buf& out, double factor);

// `tempo <factor>` with sox's default profile and linear search.
// Only the first `max_out` frames are produced (0 = all).
void Tempo(const Buf& in, Buf& out, double factor, uint32_t max_out = 0);

// Band-limited resampling. `step` = input frames per output frame.
void Resample(const Buf& in, Buf& out, double step, uint32_t out_frames);
// Same, writing directly into interleaved int16 (final output stage).
void ResampleToI16(const Buf& in, int16_t* out, double step, uint32_t out_frames);

// `speed <factor>`: output length n / factor.
void Speed(const Buf& in, Buf& out, double factor);
// `pitch <cents> [trim 0 <trim_sec>]`: tempo by 1/2^(c/1200), then rate back.
// pitch only declares a new rate; sox appends the `rate` effect after all
// user effects, so a following trim cuts the tempo output (at 48k/factor)
// before resampling. `trim_sec` < 0 means no trim. `tmp` is scratch.
void Pitch(const Buf& in, Buf& out, Buf& tmp, double cents, double trim_sec = -1);

// `reverb` with default settings (50/50/100/100/0/0), stereo input.
class Reverb
{
  public:
    bool Init(Arena& arena);
    // Processes b in place (sox reverb output = dry + wet).
    void Process(Buf& b);

  private:
    struct Filter
    {
        float*   buf;
        uint32_t size;
        uint32_t pos;
        float    store;
    };
    struct Array
    {
        Filter comb[8];
        Filter allpass[4];
    };
    void  Reset();
    float Run(Array& a, float in);
    Array arrays_[2][2]; // [input channel][output channel]
    float feedback_ = 0, damping_ = 0, gain_ = 0;
};

// Converts float to int16 with clipping.
inline int16_t ToI16(float x)
{
    float v = x * 32767.0f;
    if(v > 32767.0f)
        v = 32767.0f;
    if(v < -32768.0f)
        v = -32768.0f;
    return static_cast<int16_t>(v >= 0 ? v + 0.5f : v - 0.5f);
}

} // namespace dsp
} // namespace mbb

// The makebreakbeat engine: source/drum loading, analysis and dnb.lua's
// Beat:generate() loop, rendering to interleaved int16 stereo.
#pragma once

#include "mbb_core.h"
#include "analysis.h"
#include "dsp.h"
#include "wav.h"

namespace mbb
{
struct Params
{
    // Length of the generated file in beats. makebreakbeat.lua renders
    // break_beats + 1 so the loop has a tail for its 50 ms crossfade.
    int    beats     = 17;
    double out_tempo = 0; // 0 = keep the source tempo
    // Probabilities in percent (makebreakbeat.lua defaults).
    double p_reverse = 10, p_stutter = 20, p_pitch = 5, p_trunc = 2, p_deviation = 10,
           p_kick = 50, p_snare = 20, p_half = 1, p_reverb = 5, p_stretch = 2;
    double   kick_db  = -6;
    double   snare_db = -6;
    uint64_t seed     = 1;
    // Chop selection (not in the original): use[k] == 0 leaves chop k out of
    // the beat. nullptr (or nothing selected) = every chop, as in dnb.lua.
    const uint8_t* use = nullptr;
    // LIVE mode (not in the original): generate the part of a loop that
    // starts this many beats in. Chops and the bar-position rules (kicks,
    // snares, stutters) carry on from there instead of from the top.
    double start_beat = 0;
};

struct Limits
{
    double source_sec   = 30; // longest source sample kept
    double joined_sec   = 40; // working loop buffer at the source tempo
    double work_sec     = 8;  // per-slice effect buffers
    double drum_sec     = 4;  // kick.wav / snare.wav
};

struct GenResult
{
    bool     ok           = false;
    uint32_t frames       = 0; // frames written to the int16 output
    int      beats        = 0; // beats actually generated (after clamping)
    int      iterations   = 0;
    double   source_tempo = 0;
    double   out_tempo    = 0;
};

class Engine
{
  public:
    static size_t RequiredBytes(const Limits& lim = Limits());
    bool          Init(void* mem, size_t bytes, const Limits& lim = Limits());

    // Loads the break. legacy_rate replicates `sox -r 48000 in out` (the
    // samples of a non-48 kHz file are reinterpreted, i.e. sped up/down).
    LoadResult LoadSource(Reader& r, bool legacy_rate);
    // Loads kick.wav / snare.wav. Legacy mode replicates dnb.lua's
    // `sox -r <rate> -c <source channels> drum.wav` reinterpretation.
    LoadResult LoadDrum(Reader& r, bool kick, bool legacy_rate, bool legacy_channels);
    // input_tempo > 0 skips detection. equal_chops > 0 chops the break into
    // that many equal pieces instead of at its onsets (see Chop()).
    bool Analyse(double input_tempo = 0, uint32_t equal_chops = 0);
    // Re-chops the analysed break: 0 = at its onsets (dnb.lua), otherwise n
    // equal chops, at most one per 16th note. Returns the number of chops.
    uint32_t Chop(uint32_t equal_chops);

    // Largest beat count that fits the buffers for an output tempo.
    int MaxBeats(double out_tempo, uint32_t out_cap_frames) const;

    GenResult Generate(const Params& p, int16_t* out, uint32_t out_cap_frames,
                       const Hooks* hooks = nullptr);

    double          tempo() const { return tempo_; }
    double          tempo_exact() const { return tempo_exact_; }
    const Analyzer& analyzer() const { return analyzer_; }
    const Buf&      source() const { return src_; }
    bool            has_kick() const { return kick_.n > 0; }
    bool            has_snare() const { return snare_.n > 0; }

  private:
    static constexpr int kWork = 6;
    struct Cache
    {
        static constexpr uint32_t kSize = 2048;
        uint64_t                  keys[kSize];
        int32_t                   vals[kSize];
        uint8_t                   used[kSize];
        void                      Clear();
        bool                      Find(uint64_t k, int32_t& v) const;
        void                      Put(uint64_t k, int32_t v);
    };

    Buf* Take(); // borrow a free work buffer
    void Give(Buf* b);
    // dnb.lua audio.stretch(): stretch by new/len*1.05 if longer, then trim.
    // Returns false when sox would have failed (no output).
    bool StretchTo(Buf*& cur, double new_sec);
    void Stutter(Buf*& cur, int count, int gain, int gainpitch);
    // Mixes the (gain-adjusted) kick/snare under the slice.
    void MixDrum(Buf*& cur, const Buf& drum, double db);
    void Log(const Hooks* h, const char* msg);
    void IndexSlices();

    Arena    arena_;
    Limits   lim_;
    Buf      src_, joined_, kick_, snare_;
    Buf      work_[kWork];
    bool     busy_[kWork] = {};
    dsp::Reverb reverb_;
    Analyzer analyzer_;
    Cache    cache_;
    Rng      rng_;
    uint16_t src_channels_ = 2;
    double   tempo_        = 0;
    double   tempo_exact_  = 0;
    bool     ready_        = false;
    uint16_t kicks_[Analyzer::kMaxSlices];
    uint16_t snares_[Analyzer::kMaxSlices];
    uint32_t n_kicks_ = 0, n_snares_ = 0;
    uint16_t order_[Analyzer::kMaxSlices]; // chops in use (Generate)
    uint16_t use_k_[Analyzer::kMaxSlices];
    uint16_t use_s_[Analyzer::kMaxSlices];
    uint32_t nonce_   = 0;
};

} // namespace mbb

// Source analysis: aubio-style HFC onset detection, dnb.lua's tempo estimate,
// onset slicing and kick/snare classification.
#pragma once

#include "mbb_core.h"

namespace mbb
{
enum SliceClass : uint8_t
{
    kSliceNone  = 0,
    kSliceKick  = 1,
    kSliceSnare = 2,
};

struct Slice
{
    uint32_t start; // frame offset into the source
    uint32_t len;   // frames
    float    bd;    // mean norm after `lowpass 200`
    float    sd;    // mean norm after `lowpass 400 highpass 200`
    uint8_t  cls;
};

class Analyzer
{
  public:
    static constexpr uint32_t kMaxOnsets = 1024;
    static constexpr uint32_t kMaxSlices = 512;

    bool Init(Arena& arena, uint32_t max_source_frames);

    // `aubioonset -O hfc -s -60 -t 0.1 -B 256 -H 128` + dnb.lua audio.tempo().
    // Returns 0 when no tempo could be estimated.
    int DetectTempo(const Buf& src, double* exact = nullptr);

    // Beat:new() onset loop (threshold 1.0 down by 0.1 until >= 4 onsets)
    // followed by Beat:onset_split(). Falls back to a 16th-note grid when
    // fewer than 2 onsets are found.
    uint32_t SliceSource(const Buf& src, double tempo);
    // Replaces the slices with n equal chops of the whole source (classified
    // the same way). Not part of the original.
    uint32_t ChopEqual(const Buf& src, uint32_t n);

    const Slice*    slices() const { return slices_; }
    uint32_t        num_slices() const { return n_slices_; }
    const uint32_t* onsets() const { return onsets_; }
    uint32_t        num_onsets() const { return n_onsets_; }
    double          threshold_used() const { return threshold_used_; }
    bool            used_grid_fallback() const { return grid_fallback_; }
    bool            equal_chops() const { return equal_; }
    void            InvalidateSource() { nov_src_ = nullptr; } // new data in the same buffer

    // Low level: onset frames (as printed by aubioonset, in samples).
    uint32_t DetectOnsets(const Buf& src, uint32_t win, uint32_t hop, double threshold,
                          double minioi_sec, double silence_db, uint32_t* out,
                          uint32_t max_out);

  private:
    void ComputeNovelty(const Buf& src, uint32_t win, uint32_t hop, double silence_db);
    uint32_t PickOnsets(uint32_t hop, double threshold, uint32_t minioi, uint32_t* out,
                        uint32_t max_out);
    void ClassifySlice(const Buf& src, Slice& s);

    float*   hfc_    = nullptr;
    uint8_t* silent_ = nullptr;
    uint32_t max_hops_ = 0;
    uint32_t hops_     = 0;
    uint32_t nov_win_ = 0, nov_hop_ = 0, nov_frames_ = 0;
    const float* nov_src_ = nullptr;
    double   nov_silence_ = 0;

    uint32_t onsets_[kMaxOnsets];
    uint32_t n_onsets_ = 0;
    Slice    slices_[kMaxSlices];
    uint32_t n_slices_       = 0;
    double   threshold_used_ = 0;
    bool     grid_fallback_  = false;
    bool     equal_          = false;
};

} // namespace mbb

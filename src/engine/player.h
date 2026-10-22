// Loop player: a port of lib/Engine_Makebreakbeat.sc and the lattice logic of
// makebreakbeat.lua. Platform independent so it can be tested on a computer.
//
// SuperCollider original:
//   aOrB = ToggleFF.kr(t_trig)                 -- flipped by engine.tozero()
//   pos  = Phasor.ar(trig: aOrB, ...)          -- reader A restarts when aOrB -> 1
//   pos2 = Phasor.ar(trig: 1 - aOrB, ...)      -- reader B restarts when aOrB -> 0
//   out  = (Lag.ar(aOrB, 0.05) crossfade of A/B) * Lag.kr(amp)
//   RLPF.ar(out, 18000, 1)
// makebreakbeat.lua ticks a lattice every quarter note and calls tozero()
// whenever lattice_beats % break_beats == 0. K3 sets amp and hard-restarts
// the lattice (lattice_beats = -1), load_track() swaps the buffer while the
// readers keep their positions.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mbb
{
struct PlayBuf
{
    const int16_t* data       = nullptr; // interleaved stereo
    uint32_t       frames     = 0;
    double         fpb        = 0;  // frames per beat
    int            loop_beats = 16; // tozero every this many beats
    int            tempo      = 0;
};

class Player
{
  public:
    void Init(float sample_rate);

    // ---- audio thread
    void Process(float* left, float* right, size_t n, float vol_target);

    // ---- control (on an MCU call these with interrupts blocked)
    // K3: play/stop. Starting restarts the loop at once, like norns.
    void TogglePlay();
    // Returns a buffer that is neither playing nor queued and cancels any
    // queued swap, so it is safe to fill while Process() keeps running.
    int ClaimFree();
    // Queues `idx` to replace the playing buffer on the next beat (with
    // sync > 1: the next beat that is a multiple of `sync` beats from the
    // last loop-length change). The very first buffer starts immediately (and
    // starts playback if auto_play). Normally both readers keep their place;
    // with beat >= 0 the new buffer plays from that beat (0 = from the top).
    void Publish(int idx, bool auto_play, int sync = 1, int beat = -1);

    // ---- Focus mode (not part of the original), see README "Controls".
    // Quarter (0..3) of the loop being played, -1 before the first beat.
    int playing_quarter() const { return cur_q_; }
    // Frames [s0, s1) of quarter q (0..3) of a loop.
    static void Quarter(const PlayBuf& b, int q, uint32_t& s0, uint32_t& s1);
    // Replaces frames [s0, s1) of `loop` (`frames` long) in place with the
    // first frames of `v` (n frames, v[0] is frame s0), crossfaded (5 ms) in
    // at s0 and back out over the 5 ms after s1. If `v` ends before that
    // (a loop without a tail), it is kept up to its end.
    static void Splice(int16_t* loop, uint32_t frames, const int16_t* v, uint32_t s0, uint32_t s1,
                       uint32_t n);

    // 0, 1: loops (ClaimFree); 2, 3: free for the caller (focus: one quarter)
    PlayBuf buf[4];

    int  active() const { return active_; }
    int  pending() const { return pending_; }
    bool playing() const { return playing_; }
    // For a beat LED: samples since the last beat and whether it restarted the loop.
    uint32_t since_tick() const { return since_tick_; }
    bool     last_tick_restarted() const { return restarted_; }
    // Reader state, for tests.
    uint32_t pos_a() const { return pos_a_; }
    uint32_t pos_b() const { return pos_b_; }
    int      a_or_b() const { return a_or_b_; }
    int      lattice() const { return lattice_; }

  private:
    void Tick();
    volatile int8_t cur_q_       = -1;
    const int16_t*  swap_old_    = nullptr; // replaced buffer, faded out after a swap
    uint32_t        swap_frames_ = 0, swap_pos_ = 0;
    int             swap_n_      = 0;

    volatile int   active_     = -1;
    volatile int   pending_    = -1;
    volatile int   sync_       = 1;
    volatile int   beat_       = -1;
    volatile bool  playing_    = false;
    volatile float amp_target_ = 0;

    double   to_tick_    = 0;
    int      lattice_    = -1;
    int      a_or_b_     = 1; // the Synth is created with t_trig = 1
    uint32_t pos_a_      = 0, pos_b_ = 0;
    float    xf_         = 1, amp_ = 0, vol_ = 1;
    float    lag_xf_     = 0, lag_amp_ = 0, vol_coef_ = 0;
    float    a0_ = 0, b1_ = 0, b2_ = 0, y1_[2] = {}, y2_[2] = {};
    uint32_t since_tick_ = 0xffffffffu;
    bool     restarted_  = false;
};

} // namespace mbb

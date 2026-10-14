// Core types for the makebreakbeat engine port.
// Portable C++17, no platform dependencies. All audio is 48 kHz stereo
// interleaved float, matching the norns version (which resamples to 48 kHz).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cmath>

namespace mbb
{
constexpr uint32_t kRate     = 48000;
constexpr int      kChannels = 2;

// sox converts a time in seconds to samples with round-half-up.
inline uint32_t SecToFrames(double sec)
{
    if(!(sec > 0.0))
        return 0;
    return static_cast<uint32_t>(sec * kRate + 0.5);
}
inline double FramesToSec(uint32_t frames)
{
    return static_cast<double>(frames) / kRate;
}

// Lua's math.round from dnb.lua: floor(x + 0.5).
inline double LuaRound(double x)
{
    return std::floor(x + 0.5);
}

// A stereo interleaved float buffer with fixed capacity (in frames).
struct Buf
{
    float*   d   = nullptr;
    uint32_t n   = 0;
    uint32_t cap = 0;

    float*       Frame(uint32_t i) { return d + static_cast<size_t>(i) * kChannels; }
    const float* Frame(uint32_t i) const
    {
        return d + static_cast<size_t>(i) * kChannels;
    }
    double Seconds() const { return FramesToSec(n); }
};

// Bump allocator over one big block (SDRAM on the Seed, malloc on a host).
class Arena
{
  public:
    void Init(void* base, size_t size)
    {
        base_ = static_cast<uint8_t*>(base);
        size_ = size;
        used_ = 0;
    }
    void* Alloc(size_t bytes, size_t align = 32)
    {
        size_t start = (used_ + align - 1) & ~(align - 1);
        if(start + bytes > size_)
            return nullptr;
        used_ = start + bytes;
        return base_ + start;
    }
    size_t Mark() const { return used_; }
    void   Reset(size_t mark) { used_ = mark; }
    size_t Used() const { return used_; }
    size_t Remaining() const { return size_ - used_; }
    size_t Size() const { return size_; }

    // Allocates a buffer, returns false if the arena is exhausted.
    bool AllocBuf(Buf& b, uint32_t frames)
    {
        void* p = Alloc(static_cast<size_t>(frames) * kChannels * sizeof(float));
        if(!p)
            return false;
        b.d   = static_cast<float*>(p);
        b.n   = 0;
        b.cap = frames;
        return true;
    }

  private:
    uint8_t* base_ = nullptr;
    size_t   size_ = 0;
    size_t   used_ = 0;
};

// PCG32 random generator, standing in for Lua's math.random.
class Rng
{
  public:
    void Seed(uint64_t seed)
    {
        state_ = 0;
        inc_   = (seed << 1u) | 1u;
        Next();
        state_ += 0x853c49e6748fea9bULL ^ seed;
        Next();
    }
    uint32_t Next()
    {
        uint64_t old = state_;
        state_       = old * 6364136223846793005ULL + inc_;
        uint32_t xs  = static_cast<uint32_t>(((old >> 18u) ^ old) >> 27u);
        uint32_t rot = static_cast<uint32_t>(old >> 59u);
        return (xs >> rot) | (xs << ((-rot) & 31));
    }
    // math.random(): uniform in [0, 1)
    double Uniform() { return (Next() >> 8) * (1.0 / 16777216.0); }
    // math.random(lo, hi): uniform integer in [lo, hi]
    int Range(int lo, int hi)
    {
        if(hi <= lo)
            return lo;
        uint32_t span = static_cast<uint32_t>(hi - lo) + 1u;
        return lo + static_cast<int>(Next() % span);
    }

  private:
    uint64_t state_ = 0x4d595df4d0f33173ULL;
    uint64_t inc_   = 1442695040888963407ULL;
};

// Optional progress/log hooks so the firmware can blink LEDs and log over USB.
struct Hooks
{
    void (*log)(void* ctx, const char* msg)          = nullptr;
    void (*progress)(void* ctx, float fraction)      = nullptr;
    void* ctx                                        = nullptr;
    // Generate() gives up (ok = false) if this turns true while it runs.
    const volatile bool* stop                        = nullptr;
};

} // namespace mbb

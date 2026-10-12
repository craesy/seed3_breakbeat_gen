// WAV reading/writing through small platform-neutral I/O interfaces.
#pragma once

#include "mbb_core.h"

namespace mbb
{
// Byte source. The firmware implements this on top of FatFs, the host on stdio.
class Reader
{
  public:
    virtual ~Reader() {}
    // Reads up to `bytes`; returns the number of bytes read (0 at EOF/error).
    virtual uint32_t Read(void* dst, uint32_t bytes) = 0;
    virtual bool     Seek(uint32_t pos)              = 0;
};

class Writer
{
  public:
    virtual ~Writer() {}
    virtual bool Write(const void* src, uint32_t bytes) = 0;
    virtual bool Seek(uint32_t pos)                     = 0;
};

struct WavInfo
{
    uint32_t rate        = 0;
    uint16_t channels    = 0;
    uint16_t bits        = 0;
    bool     is_float    = false;
    uint32_t data_offset = 0;
    uint32_t data_bytes  = 0;
    uint32_t frames      = 0;
};

// Parses RIFF/WAVE chunks (fmt + data, skipping anything else).
// Supports PCM 8/16/24/32-bit, IEEE float 32/64-bit, WAVE_FORMAT_EXTENSIBLE.
bool ParseWav(Reader& r, WavInfo& info);

struct LoadOptions
{
    // sox `-r <rate>` placed before an input file overrides the header rate
    // (the samples are reinterpreted, not resampled). 0 = use header.
    uint32_t rate_override = 0;
    // sox `-c <n>` before an input reinterprets the interleaving. 0 = header.
    uint16_t channels_override = 0;
};

struct LoadResult
{
    bool     ok              = false;
    uint32_t effective_rate  = 0; // rate the samples should be treated as
    uint16_t source_channels = 0; // channel count from the header
    uint16_t used_channels   = 0; // channel count used for de-interleaving
    bool     truncated       = false;
};

// Decodes the WAV into stereo float (mono is duplicated, >2 channels keeps
// the first two). Frames beyond out.cap are dropped (result.truncated).
LoadResult LoadWav(Reader& r, Buf& out, const LoadOptions& opt = LoadOptions());

// Writes a 16-bit PCM stereo 48 kHz WAV file.
bool WriteWav16(Writer& w, const int16_t* interleaved, uint32_t frames);

} // namespace mbb

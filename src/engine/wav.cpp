#include "wav.h"

#include <cstring>

namespace mbb
{
namespace
{
uint16_t Le16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
uint32_t Le32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
           | (static_cast<uint32_t>(p[2]) << 16)
           | (static_cast<uint32_t>(p[3]) << 24);
}
bool ReadExact(Reader& r, void* dst, uint32_t bytes)
{
    return r.Read(dst, bytes) == bytes;
}

float DecodeSample(const uint8_t* p, uint16_t bits, bool is_float)
{
    if(is_float)
    {
        if(bits == 32)
        {
            float f;
            uint32_t u = Le32(p);
            std::memcpy(&f, &u, 4);
            return f;
        }
        uint64_t u = static_cast<uint64_t>(Le32(p))
                     | (static_cast<uint64_t>(Le32(p + 4)) << 32);
        double d;
        std::memcpy(&d, &u, 8);
        return static_cast<float>(d);
    }
    switch(bits)
    {
        case 8: return (static_cast<int>(p[0]) - 128) * (1.0f / 128.0f);
        case 16:
            return static_cast<int16_t>(Le16(p)) * (1.0f / 32768.0f);
        case 24:
        {
            int32_t v = static_cast<int32_t>((p[0] << 8) | (p[1] << 16)
                                             | (static_cast<uint32_t>(p[2]) << 24));
            return (v >> 8) * (1.0f / 8388608.0f);
        }
        case 32:
            return static_cast<int32_t>(Le32(p)) * (1.0f / 2147483648.0f);
        default: return 0.0f;
    }
}
} // namespace

bool ParseWav(Reader& r, WavInfo& info)
{
    uint8_t hdr[12];
    if(!r.Seek(0) || !ReadExact(r, hdr, 12))
        return false;
    if(std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0)
        return false;

    bool     have_fmt = false;
    uint32_t pos      = 12;
    for(int guard = 0; guard < 64; guard++)
    {
        uint8_t ch[8];
        if(!r.Seek(pos) || !ReadExact(r, ch, 8))
            return false;
        uint32_t size = Le32(ch + 4);
        if(std::memcmp(ch, "fmt ", 4) == 0)
        {
            uint8_t  fmt[40] = {0};
            uint32_t n       = size < sizeof(fmt) ? size : sizeof(fmt);
            if(n < 16 || !ReadExact(r, fmt, n))
                return false;
            uint16_t tag  = Le16(fmt);
            info.channels = Le16(fmt + 2);
            info.rate     = Le32(fmt + 4);
            info.bits     = Le16(fmt + 14);
            if(tag == 0xFFFE && n >= 26)
                tag = Le16(fmt + 24); // sub-format GUID starts with the tag
            if(tag == 1)
                info.is_float = false;
            else if(tag == 3)
                info.is_float = true;
            else
                return false;
            if(info.is_float && info.bits != 32 && info.bits != 64)
                return false;
            if(!info.is_float && info.bits != 8 && info.bits != 16
               && info.bits != 24 && info.bits != 32)
                return false;
            if(info.channels == 0 || info.rate == 0)
                return false;
            have_fmt = true;
        }
        else if(std::memcmp(ch, "data", 4) == 0)
        {
            if(!have_fmt)
                return false;
            info.data_offset = pos + 8;
            info.data_bytes  = size;
            info.frames = size / (static_cast<uint32_t>(info.bits / 8) * info.channels);
            return true;
        }
        pos += 8 + size + (size & 1u);
    }
    return false;
}

LoadResult LoadWav(Reader& r, Buf& out, const LoadOptions& opt)
{
    LoadResult res;
    WavInfo    info;
    out.n = 0;
    if(!ParseWav(r, info))
        return res;

    res.source_channels = info.channels;
    res.used_channels = opt.channels_override ? opt.channels_override : info.channels;
    res.effective_rate = opt.rate_override ? opt.rate_override : info.rate;

    const uint32_t sample_bytes = info.bits / 8;
    const uint32_t frame_bytes  = sample_bytes * res.used_channels;
    const uint32_t total_frames = info.data_bytes / frame_bytes;
    const uint32_t frames = total_frames < out.cap ? total_frames : out.cap;
    res.truncated = frames < total_frames;

    uint8_t        buf[2048];
    const uint32_t block_frames = sizeof(buf) / frame_bytes;
    if(block_frames == 0 || !r.Seek(info.data_offset))
        return res;

    uint32_t done = 0;
    while(done < frames)
    {
        uint32_t want = frames - done;
        if(want > block_frames)
            want = block_frames;
        uint32_t got = r.Read(buf, want * frame_bytes) / frame_bytes;
        if(got == 0)
            break;
        for(uint32_t i = 0; i < got; i++)
        {
            const uint8_t* p = buf + i * frame_bytes;
            float          l = DecodeSample(p, info.bits, info.is_float);
            float          rr
                = res.used_channels > 1
                      ? DecodeSample(p + sample_bytes, info.bits, info.is_float)
                      : l;
            float* f = out.Frame(done + i);
            f[0]     = l;
            f[1]     = rr;
        }
        done += got;
    }
    out.n  = done;
    res.ok = done > 0 || frames == 0;
    return res;
}

bool WriteWav16(Writer& w, const int16_t* data, uint32_t frames)
{
    const uint32_t data_bytes = frames * 4u;
    uint8_t        h[44];
    auto           put32 = [&](int off, uint32_t v) {
        h[off]     = static_cast<uint8_t>(v);
        h[off + 1] = static_cast<uint8_t>(v >> 8);
        h[off + 2] = static_cast<uint8_t>(v >> 16);
        h[off + 3] = static_cast<uint8_t>(v >> 24);
    };
    auto put16 = [&](int off, uint16_t v) {
        h[off]     = static_cast<uint8_t>(v);
        h[off + 1] = static_cast<uint8_t>(v >> 8);
    };
    std::memcpy(h, "RIFF", 4);
    put32(4, 36 + data_bytes);
    std::memcpy(h + 8, "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);
    put16(22, 2);
    put32(24, kRate);
    put32(28, kRate * 4);
    put16(32, 4);
    put16(34, 16);
    std::memcpy(h + 36, "data", 4);
    put32(40, data_bytes);
    if(!w.Write(h, sizeof(h)))
        return false;
    // Little-endian targets only (Cortex-M7 and typical hosts).
    const uint8_t* p      = reinterpret_cast<const uint8_t*>(data);
    uint32_t       remain = data_bytes;
    while(remain)
    {
        uint32_t n = remain > 4096 ? 4096 : remain;
        if(!w.Write(p, n))
            return false;
        p += n;
        remain -= n;
    }
    return true;
}

} // namespace mbb

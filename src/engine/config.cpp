#include "config.h"

#include <cstring>

namespace mbb
{
namespace
{
bool IsSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

bool ParseNumber(const char* s, const char* e, double& out)
{
    while(s < e && IsSpace(*s))
        s++;
    while(e > s && IsSpace(e[-1]))
        e--;
    if(s == e)
        return false;
    bool neg = false;
    if(*s == '+' || *s == '-')
        neg = *s++ == '-';
    double v = 0, scale = 1;
    bool   digits = false, frac = false;
    for(; s < e; s++)
    {
        if(*s >= '0' && *s <= '9')
        {
            digits = true;
            if(frac)
            {
                scale /= 10;
                v += (*s - '0') * scale;
            }
            else
                v = v * 10 + (*s - '0');
        }
        else if(*s == '.' && !frac)
            frac = true;
        else
            return false;
    }
    if(!digits)
        return false;
    out = neg ? -v : v;
    return true;
}

bool KeyIs(const char* k, uint32_t klen, const char* name)
{
    return std::strlen(name) == klen && std::strncmp(k, name, klen) == 0;
}

bool ParseBool(const char* s, const char* e, bool& out)
{
    while(s < e && IsSpace(*s))
        s++;
    while(e > s && IsSpace(e[-1]))
        e--;
    uint32_t n = static_cast<uint32_t>(e - s);
    if(KeyIs(s, n, "1") || KeyIs(s, n, "yes") || KeyIs(s, n, "true") || KeyIs(s, n, "on"))
        out = true;
    else if(KeyIs(s, n, "0") || KeyIs(s, n, "no") || KeyIs(s, n, "false") || KeyIs(s, n, "off"))
        out = false;
    else
        return false;
    return true;
}

bool Apply(const char* k, uint32_t kl, const char* v, const char* ve, Settings& s)
{
    struct NumKey
    {
        const char* name;
        double*     dst;
        double      lo, hi;
    };
    const NumKey nums[] = {
        {"tempo", &s.tempo, 0, 400},
        {"input_tempo", &s.input_tempo, 0, 400},
        {"p_reverse", &s.gen.p_reverse, 0, 100},
        {"p_stutter", &s.gen.p_stutter, 0, 100},
        {"p_pitch", &s.gen.p_pitch, 0, 100},
        {"p_trunc", &s.gen.p_trunc, 0, 100},
        {"p_deviation", &s.gen.p_deviation, 0, 100},
        {"p_kick", &s.gen.p_kick, 0, 100},
        {"p_snare", &s.gen.p_snare, 0, 100},
        {"p_half", &s.gen.p_half, 0, 100},
        {"p_reverb", &s.gen.p_reverb, 0, 100},
        {"p_stretch", &s.gen.p_stretch, 0, 100},
        {"kick_db", &s.gen.kick_db, -96, 12},
        {"snare_db", &s.gen.snare_db, -96, 12},
    };
    for(const NumKey& n : nums)
        if(KeyIs(k, kl, n.name))
        {
            double x;
            if(!ParseNumber(v, ve, x))
                return false;
            *n.dst = x < n.lo ? n.lo : (x > n.hi ? n.hi : x);
            return true;
        }

    struct BoolKey
    {
        const char* name;
        bool*       dst;
    };
    const BoolKey bools[] = {
        {"legacy_rate", &s.legacy_rate},
        {"legacy_channels", &s.legacy_channels},
        {"use_pots", &s.use_pots},
        {"save", &s.save},
        {"auto_generate", &s.auto_generate},
        {"auto_play", &s.auto_play},
    };
    for(const BoolKey& b : bools)
        if(KeyIs(k, kl, b.name))
            return ParseBool(v, ve, *b.dst);

    double x;
    if(KeyIs(k, kl, "beats"))
    {
        if(!ParseNumber(v, ve, x) || x < 1)
            return false;
        s.beats = x > 256 ? 256 : static_cast<int>(x);
        return true;
    }
    if(KeyIs(k, kl, "seed"))
    {
        if(!ParseNumber(v, ve, x) || x < 0)
            return false;
        s.seed = x > 4294967295.0 ? 4294967295u : static_cast<uint32_t>(x);
        return true;
    }
    if(KeyIs(k, kl, "chop"))
    {
        if(!ParseNumber(v, ve, x) || x < 0 || x > 512)
            return false;
        s.chop = static_cast<uint32_t>(x);
        return true;
    }
    if(KeyIs(k, kl, "sample"))
    {
        while(v < ve && IsSpace(*v))
            v++;
        while(ve > v && IsSpace(ve[-1]))
            ve--;
        uint32_t n = static_cast<uint32_t>(ve - v);
        if(n >= sizeof(s.sample))
            return false;
        std::memcpy(s.sample, v, n);
        s.sample[n] = 0;
        return true;
    }
    return false;
}
} // namespace

int ParseConfig(const char* text, uint32_t len, Settings& s, int* bad_line)
{
    int         bad  = 0;
    int         line = 0;
    const char* p    = text;
    const char* end  = text + len;
    if(bad_line)
        *bad_line = 0;
    while(p < end)
    {
        line++;
        const char* eol = p;
        while(eol < end && *eol != '\n')
            eol++;
        const char* stop = p;
        while(stop < eol && *stop != '#' && *stop != ';')
            stop++;
        const char* k = p;
        while(k < stop && IsSpace(*k))
            k++;
        if(k < stop)
        {
            const char* eq = k;
            while(eq < stop && *eq != '=')
                eq++;
            const char* ke = eq;
            while(ke > k && IsSpace(ke[-1]))
                ke--;
            if(eq == stop || !Apply(k, static_cast<uint32_t>(ke - k), eq + 1, stop, s))
            {
                if(!bad && bad_line)
                    *bad_line = line;
                bad++;
            }
        }
        p = eol + 1;
    }
    return bad;
}

void ApplyPots(Params& p, const float pots[4])
{
    struct PotMap
    {
        double Params::*p;
        double          max;
    };
    static const PotMap kMap[4][3] = {
        {{&Params::p_deviation, 100}, {&Params::p_kick, 100}, {&Params::p_snare, 100}},
        {{&Params::p_stutter, 100}, {&Params::p_trunc, 50}, {nullptr, 0}},
        {{&Params::p_reverse, 80}, {&Params::p_pitch, 60}, {&Params::p_half, 40}},
        {{&Params::p_reverb, 60}, {&Params::p_stretch, 40}, {nullptr, 0}},
    };
    for(int f = 0; f < 4; f++)
    {
        const double x = pots[f] < 0 ? 0 : (pots[f] > 1 ? 1 : pots[f]);
        for(const PotMap& m : kMap[f])
            if(m.p)
            {
                const double c = p.*m.p, hi = m.max > c ? m.max : c;
                p.*m.p         = c + (hi - c) * x;
            }
    }
}

uint32_t DefaultConfig(char* buf, uint32_t cap)
{
    static const char kText[]
        = "# makebreakbeat for Daisy Seed3 - settings\n"
          "# key = value, '#' starts a comment. Delete this file to restore defaults.\n"
          "\n"
          "beats = 16          # loop length in beats\n"
          "tempo = 0           # output BPM (0 = keep the break's tempo)\n"
          "input_tempo = 0     # break BPM (0 = detect automatically)\n"
          "sample =            # break to load first, e.g. amen_resampled.wav\n"
          "chop = 0            # 0 = chop at each hit; 8/16/32 = equal chops\n"
          "\n"
          "# probabilities in percent (makebreakbeat defaults). In focus mode the\n"
          "# pots vary them for D16 (fully left = these, right = more):\n"
          "# D19 deviation/kick/snare, D20 stutter/trunc, D21 reverse/pitch/half,\n"
          "# D22 reverb/stretch\n"
          "p_deviation = 10\n"
          "p_reverse = 10\n"
          "p_stutter = 20\n"
          "p_pitch = 5\n"
          "p_trunc = 2\n"
          "p_half = 1\n"
          "p_reverb = 5\n"
          "p_stretch = 2\n"
          "p_kick = 50\n"
          "p_snare = 20\n"
          "kick_db = -6\n"
          "snare_db = -6\n"
          "\n"
          "seed = 0            # 0 = new random beat every time; N = beat k uses seed N+k-1\n"
          "save = 1            # write every beat to /MBB/OUT\n"
          "auto_generate = 1   # generate as soon as a break is loaded\n"

          "\n"
          "# 1 = reproduce the original's sox quirks: non-48 kHz\n"
          "# breaks are re-labelled (pitch/speed shift) and kick/snare take the\n"
          "# break's channel count (a mono snare plays at double speed).\n"
          "legacy_rate = 1\n"
          "legacy_channels = 1\n";
    uint32_t n = sizeof(kText) - 1;
    if(n > cap)
        n = cap;
    std::memcpy(buf, kText, n);
    return n;
}

} // namespace mbb

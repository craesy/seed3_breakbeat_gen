// Command-line front end for the engine (host builds), mirroring dnb.lua's
// options so the port can be auditioned and A/B-tested on a computer.
//   mbb -i break.wav -o out.wav [-b beats] [-t bpm] [--seed n] [--kick-file k.wav]
//       [--snare-file s.wav] [--input-tempo bpm] [--reverse p] [--stutter p] ...
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../src/engine/config.h"
#include "../src/engine/engine.h"
#include "../tests/host_io.h"

using namespace mbb;

static void Usage()
{
    std::puts(
        "usage: mbb -i in.wav -o out.wav [options]\n"
        "  -b N               beats to generate (default 16)\n"
        "  -t BPM             output tempo (default: the break's tempo)\n"
        "  --input-tempo BPM  break tempo (default: detect)\n"
        "  --seed N           random seed (default 1)\n"
        "  --kick-file F, --snare-file F, --kick-mix DB, --snare-mix DB\n"
        "  --deviation --reverse --stutter --pitch --trunc --half --reverb\n"
        "  --stretch --kick --snare   probabilities in percent\n"
        "  --no-legacy        resample/remix instead of sox -r/-c reinterpretation\n"
        "  --chop N           chop into N equal pieces (default: at each hit)\n"
        "  --use LIST         only use these chops, e.g. 1,3,5-8 (see -v)\n"
        "  --only TYPES       only use kick/snare/other chops, e.g. kick,snare\n"
        "  -v                 print analysis details"
        "  --print-config     print the default /MBB/config.txt and exit");
}

int main(int argc, char** argv)
{
    const char *in = nullptr, *out = nullptr, *kick = nullptr, *snare = nullptr;
    Params      p;
    p.beats            = 16;
    double input_tempo = 0;
    bool   legacy = true, verbose = false;
    int         chop = 0;
    std::string use_list, only;
    for(int i = 1; i < argc; i++)
    {
        std::string a = argv[i];
        if(a == "--print-config")
        {
            static char text[4096];
            std::fwrite(text, 1, DefaultConfig(text, sizeof(text)), stdout);
            return 0;
        }
        const char* v = i + 1 < argc ? argv[i + 1] : nullptr;
        auto        num = [&](double& dst) {
            if(v)
            {
                dst = std::atof(v);
                i++;
            }
        };
        if(a == "-i" && v)
            in = argv[++i];
        else if(a == "-o" && v)
            out = argv[++i];
        else if(a == "-b" && v)
            p.beats = std::atoi(argv[++i]);
        else if(a == "-t")
            num(p.out_tempo);
        else if(a == "--input-tempo")
            num(input_tempo);
        else if(a == "--seed" && v)
            p.seed = std::strtoull(argv[++i], nullptr, 10);
        else if(a == "--kick-file" && v)
            kick = argv[++i];
        else if(a == "--snare-file" && v)
            snare = argv[++i];
        else if(a == "--kick-mix")
            num(p.kick_db);
        else if(a == "--snare-mix")
            num(p.snare_db);
        else if(a == "--deviation")
            num(p.p_deviation);
        else if(a == "--reverse")
            num(p.p_reverse);
        else if(a == "--stutter")
            num(p.p_stutter);
        else if(a == "--pitch")
            num(p.p_pitch);
        else if(a == "--trunc")
            num(p.p_trunc);
        else if(a == "--half")
            num(p.p_half);
        else if(a == "--reverb")
            num(p.p_reverb);
        else if(a == "--stretch")
            num(p.p_stretch);
        else if(a == "--kick")
            num(p.p_kick);
        else if(a == "--snare")
            num(p.p_snare);
        else if(a == "--no-legacy")
            legacy = false;
        else if(a == "--chop" && v)
            chop = std::atoi(argv[++i]);
        else if(a == "--use" && v)
            use_list = argv[++i];
        else if(a == "--only" && v)
            only = argv[++i];
        else if(a == "-v")
            verbose = true;
        else
        {
            Usage();
            return 2;
        }
    }
    if(!in || !out)
    {
        Usage();
        return 2;
    }

    static Engine     e;
    std::vector<char> mem(Engine::RequiredBytes());
    if(!e.Init(mem.data(), mem.size()))
        return 1;
    FileReader r(in);
    if(!r.ok() || !e.LoadSource(r, legacy).ok)
    {
        std::fprintf(stderr, "cannot read %s\n", in);
        return 1;
    }
    if(kick)
    {
        FileReader k(kick);
        if(!k.ok() || !e.LoadDrum(k, true, legacy, legacy).ok)
            std::fprintf(stderr, "warning: cannot read %s\n", kick);
    }
    if(snare)
    {
        FileReader s(snare);
        if(!s.ok() || !e.LoadDrum(s, false, legacy, legacy).ok)
            std::fprintf(stderr, "warning: cannot read %s\n", snare);
    }
    if(!e.Analyse(input_tempo, chop > 0 ? chop : 0))
    {
        std::fprintf(stderr, "analysis failed\n");
        return 1;
    }
    if(verbose)
    {
        std::printf("tempo %.0f (%.3f), %u onsets, %u slices%s\n", e.tempo(), e.tempo_exact(),
                    e.analyzer().num_onsets(), e.analyzer().num_slices(),
                    e.analyzer().used_grid_fallback() ? " (grid fallback)" : "");
        for(uint32_t i = 0; i < e.analyzer().num_slices(); i++)
        {
            const Slice& s = e.analyzer().slices()[i];
            std::printf("  slice %2u %.6f +%.6f bd %.4f sd %.4f %s\n", i + 1, FramesToSec(s.start),
                        FramesToSec(s.len), s.bd, s.sd,
                        s.cls == kSliceKick ? "KICK" : (s.cls == kSliceSnare ? "SNARE" : ""));
        }
    }
    const uint32_t       n_chops = e.analyzer().num_slices();
    std::vector<uint8_t> use(n_chops, 1);
    if(!use_list.empty())
    {
        std::fill(use.begin(), use.end(), 0);
        for(const char* s = use_list.c_str(); *s;)
        {
            char* q;
            long  a = std::strtol(s, &q, 10), b = a;
            if(*q == '-')
                b = std::strtol(q + 1, &q, 10);
            for(long k = a; k <= b; k++)
                if(k >= 1 && k <= static_cast<long>(n_chops))
                    use[k - 1] = 1;
            s = *q ? q + 1 : q;
        }
    }
    if(!only.empty())
        for(uint32_t k = 0; k < n_chops; k++)
        {
            const uint8_t c = e.analyzer().slices()[k].cls;
            const char*   n = c == kSliceKick ? "kick" : (c == kSliceSnare ? "snare" : "other");
            if(only.find(n) == std::string::npos)
                use[k] = 0;
        }
    p.use = use.data();
    const uint32_t       cap = SecToFrames(120);
    std::vector<int16_t> buf(static_cast<size_t>(cap) * 2);
    Hooks h;
    h.log = [](void*, const char* m) { std::printf("  %s\n", m); };
    GenResult            g = e.Generate(p, buf.data(), cap, verbose ? &h : nullptr);
    if(!g.ok)
    {
        std::fprintf(stderr, "generation failed\n");
        return 1;
    }
    FileWriter w(out);
    if(!w.ok() || !WriteWav16(w, buf.data(), g.frames))
    {
        std::fprintf(stderr, "cannot write %s\n", out);
        return 1;
    }
    std::printf("generated %d beats into '%s' @ %.0f bpm (%u frames, %d iterations)\n", g.beats, out,
                g.out_tempo, g.frames, g.iterations);
    return 0;
}

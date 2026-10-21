// Host test-suite for the makebreakbeat port.
//   1. analysis vs. ground truth captured from aubioonset/sox/lua
//   2. effect A/B against a reference sox binary (MBB_SOX, optional)
//   3. generation: lengths, NaNs, determinism, timing; writes example WAVs
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../src/engine/config.h"
#include "../src/engine/engine.h"
#include "../src/engine/player.h"
#include "host_io.h"

using namespace mbb;

static int g_fail = 0;
#define CHECK(cond, ...)                         \
    do                                           \
    {                                            \
        if(!(cond))                              \
        {                                        \
            g_fail++;                            \
            std::printf("  FAIL: " __VA_ARGS__); \
            std::printf("\n");                   \
        }                                        \
    } while(0)

static std::string g_assets, g_out, g_sox;

static bool WriteWavF32(const char* path, const Buf& b)
{
    FILE* f = std::fopen(path, "wb");
    if(!f)
        return false;
    uint32_t data = b.n * 8, riff = 36 + data, fmt = 16, rate = kRate, br = kRate * 8;
    uint16_t tag = 3, ch = 2, align = 8, bits = 32;
    std::fwrite("RIFF", 1, 4, f);
    std::fwrite(&riff, 4, 1, f);
    std::fwrite("WAVEfmt ", 1, 8, f);
    std::fwrite(&fmt, 4, 1, f);
    std::fwrite(&tag, 2, 1, f);
    std::fwrite(&ch, 2, 1, f);
    std::fwrite(&rate, 4, 1, f);
    std::fwrite(&br, 4, 1, f);
    std::fwrite(&align, 2, 1, f);
    std::fwrite(&bits, 2, 1, f);
    std::fwrite("data", 1, 4, f);
    std::fwrite(&data, 4, 1, f);
    std::fwrite(b.d, 8, b.n, f);
    std::fclose(f);
    return true;
}

static bool Load(const std::string& path, Buf& b, const LoadOptions& o = LoadOptions())
{
    FileReader r(path.c_str());
    return r.ok() && LoadWav(r, b, o).ok;
}

struct Pool
{
    std::vector<float> mem;
    Arena              arena;
    explicit Pool(size_t bytes) : mem(bytes / 4 + 64) { arena.Init(mem.data(), bytes); }
    Buf Make(double sec)
    {
        Buf b;
        arena.AllocBuf(b, SecToFrames(sec));
        return b;
    }
};

// ------------------------------------------------------------- 1. analysis
static void TestAnalysis(Engine& e)
{
    std::printf("[analysis]\n");
    const double t_onsets[] = {0,        .168667,  .338104,  .507542,  .676125,  .844562, 1.032896,
                               1.184521, 1.353521, 1.521854, 1.612021, 1.698875, 1.783458, 1.862042,
                               2.029771, 2.119917, 2.216021, 2.291271, 2.368854, 2.536625, 2.628187};
    const double s_onsets[] = {0,        .167667,  .336542,  .505417,  .674417,  .845229, 1.181042,
                               1.351750, 1.519854, 1.858771, 2.026813, 2.365833, 2.534625};
    const uint8_t classes[] = {1, 0, 2, 1, 0, 1, 0, 2, 2, 2, 2, 2};
    const double  bd[] = {.0578, .0138, .0231, .0593, .0137, .0382, .0136, .0267, .0125, .0265, .0121, .0191};
    const double  sd[] = {.0474, .0119, .0315, .0477, .0119, .0352, .0119, .0414, .0173, .0412, .0169, .0288};

    // The tempo pass is re-run here through the public low-level API.
    static uint32_t on[1024];
    Analyzer&       a = const_cast<Analyzer&>(e.analyzer());
    uint32_t        n = a.DetectOnsets(e.source(), 256, 128, 0.1, 0.05, -60, on, 1024);
    CHECK(n == 21, "tempo pass: %u onsets, expected 21", n);
    int worst = 0;
    for(uint32_t i = 0; i < n && i < 21; i++)
    {
        int d = std::abs(static_cast<int>(on[i]) - static_cast<int>(t_onsets[i] * kRate + .5));
        worst = d > worst ? d : worst;
    }
    std::printf("  tempo pass: %u onsets, worst error %d samples\n", n, worst);
    CHECK(worst <= 2, "tempo-pass onset error %d samples", worst);

    std::printf("  tempo %.0f (exact %.3f, reference 177 / 177.018)\n", e.tempo(), e.tempo_exact());
    CHECK(e.tempo() == 177, "tempo %.1f != 177", e.tempo());

    n = e.analyzer().num_onsets();
    CHECK(n == 13, "slice pass: %u onsets, expected 13", n);
    worst = 0;
    for(uint32_t i = 0; i < n && i < 13; i++)
    {
        int d = std::abs(static_cast<int>(e.analyzer().onsets()[i])
                         - static_cast<int>(s_onsets[i] * kRate + .5));
        worst = d > worst ? d : worst;
    }
    std::printf("  slice pass: %u onsets (threshold %.1f), worst error %d samples\n", n,
                e.analyzer().threshold_used(), worst);
    CHECK(worst <= 2, "slice onset error %d samples", worst);

    const Slice* s = e.analyzer().slices();
    CHECK(e.analyzer().num_slices() == 12, "%u slices", e.analyzer().num_slices());
    double worst_m = 0;
    for(uint32_t i = 0; i < e.analyzer().num_slices() && i < 12; i++)
    {
        CHECK(s[i].cls == classes[i], "slice %u class %d expected %d", i + 1, s[i].cls, classes[i]);
        double d1 = std::fabs(s[i].bd - bd[i]), d2 = std::fabs(s[i].sd - sd[i]);
        worst_m   = d1 > worst_m ? d1 : worst_m;
        worst_m   = d2 > worst_m ? d2 : worst_m;
    }
    std::printf("  slice classes ok, worst bd/sd metric error %.5f\n", worst_m);
    CHECK(worst_m < 0.0006, "bd/sd metric error %.5f", worst_m);
}

// ------------------------------------------------------------- 2. sox A/B
static bool RunSox(const std::string& args)
{
    std::string cmd = g_sox + " " + args + " 2>/dev/null";
    return std::system(cmd.c_str()) == 0;
}

static double Snr(const Buf& ref, const Buf& got, int* len_diff)
{
    *len_diff = static_cast<int>(got.n) - static_cast<int>(ref.n);
    uint32_t n = ref.n < got.n ? ref.n : got.n;
    double   s = 0, e = 0;
    for(uint32_t i = 0; i < n * 2; i++)
    {
        s += static_cast<double>(ref.d[i]) * ref.d[i];
        double d = static_cast<double>(ref.d[i]) - got.d[i];
        e += d * d;
    }
    if(e == 0)
        return 200;
    return 10 * std::log10(s / e);
}

static void Compare(const char* name, const Buf& ref, const Buf& got, double min_snr, int max_len_diff = 0)
{
    int    ld;
    double snr = Snr(ref, got, &ld);
    std::printf("  %-34s len %7u vs %7u (%+d)  SNR %6.1f dB\n", name, ref.n, got.n, ld, snr);
    CHECK(std::abs(ld) <= max_len_diff, "%s length differs by %d", name, ld);
    CHECK(snr >= min_snr, "%s SNR %.1f < %.1f", name, snr, min_snr);
}

static void TestSoxEffects(Engine& e)
{
    std::printf("[sox A/B: %s]\n", g_sox.c_str());
    Pool        pool(64u << 20);
    const Buf&  src = e.source();
    const Slice* sl = e.analyzer().slices();
    Buf a = pool.Make(2), b = pool.Make(2), ref = pool.Make(8), got = pool.Make(8), tmp = pool.Make(8);
    // a: slice 3 (snare, ~0.17 s); b: slices 4-5 (~0.34 s)
    Buf va{const_cast<float*>(src.Frame(sl[2].start)), sl[2].len, sl[2].len};
    Buf vb{const_cast<float*>(src.Frame(sl[3].start)), sl[3].len + sl[4].len, 0};
    dsp::Copy(va, a);
    dsp::Copy(vb, b);
    const std::string A = g_out + "/ab_a.wav", B = g_out + "/ab_b.wav", R = g_out + "/ab_ref.wav";
    WriteWavF32(A.c_str(), a);
    WriteWavF32(B.c_str(), b);
    const std::string F = " -e floating-point -b 32 ";
    auto sox1 = [&](const std::string& in, const std::string& fx) {
        return RunSox(in + F + R + " " + fx) && Load(R, ref);
    };

    if(sox1(A, "reverse"))
    {
        dsp::Copy(a, got);
        dsp::Reverse(got);
        Compare("reverse", ref, got, 140);
    }
    if(sox1(A, "gain -6"))
    {
        dsp::Copy(a, got);
        dsp::Gain(got, -6);
        Compare("gain -6", ref, got, 120);
    }
    const double factors[] = {1.37, 2.1, 4.2};
    for(double f : factors)
        if(sox1(B, "stretch " + std::to_string(f)))
        {
            dsp::Stretch(b, got, f);
            Compare(("stretch " + std::to_string(f).substr(0, 4)).c_str(), ref, got, 100);
        }
    const double tempos[] = {0.9, 0.79, 1.26};
    for(double f : tempos)
        if(sox1(B, "tempo " + std::to_string(f)))
        {
            dsp::Tempo(b, got, f);
            Compare(("tempo " + std::to_string(f).substr(0, 4)).c_str(), ref, got, 100);
        }
    const int cents[] = {200, 400, 800, -100, 100};
    for(int c : cents)
        if(sox1(B, "pitch " + std::to_string(c)))
        {
            dsp::Pitch(b, got, tmp, c);
            Compare(("pitch " + std::to_string(c)).c_str(), ref, got, 50);
        }
    // The stutter's `gain g pitch p trim 0 <1/16 + 5 ms>` on a short slice.
    const double   rep_sec = 60.0 / 177 / 4 + 0.005;
    const int      short_cents[] = {100, 200, 300, -100, 700};
    for(int c : short_cents)
    {
        char fx[96];
        std::snprintf(fx, sizeof(fx), "gain -4 pitch %d trim 0 %.15g", c, rep_sec);
        if(sox1(A, fx))
        {
            Buf g = pool.Make(2);
            dsp::Copy(a, g);
            dsp::Gain(g, -4);
            dsp::Pitch(g, got, tmp, c, rep_sec);
            Compare(("short " + std::string(fx).substr(8, 9)).c_str(), ref, got, 50);
        }
    }
    if(sox1(B, "speed 0.5"))
    {
        dsp::Speed(b, got, 0.5);
        Compare("speed 0.5", ref, got, 60);
    }
    if(sox1(B, "speed 1.0847457627119"))
    {
        dsp::Speed(b, got, 1.0847457627119);
        Compare("speed 1.0847 (177->192 bpm)", ref, got, 60);
    }
    if(sox1(A, "pad 0 0.17 reverb"))
    {
        dsp::Copy(a, got);
        dsp::PadEnd(got, SecToFrames(0.17));
        dsp::Reverb* rv = new dsp::Reverb;
        Pool    p2(1 << 20);
        rv->Init(p2.arena);
        rv->Process(got);
        delete rv;
        Compare("pad 0 0.17 reverb", ref, got, 100);
    }
    {
        double sl8 = 60.0 / 177 / 8;
        double e8  = b.Seconds() - sl8;
        char   fx[96];
        std::snprintf(fx, sizeof(fx), "fade 0 %.6f 0.005 pad 0 %.6f", e8, sl8);
        if(sox1(B, fx))
        {
            dsp::SilentEnd(b, got, sl8);
            Compare("silent_end (fade+pad)", ref, got, 100);
        }
    }
    if(sox1(B, "highpass 80 contrast"))
    {
        dsp::Copy(b, got);
        dsp::Filter(got, dsp::FilterType::HighPass, 80);
        dsp::Contrast(got);
        Compare("highpass 80 contrast", ref, got, 100);
    }
    {
        char pos[64];
        std::snprintf(pos, sizeof(pos), "%.6f", a.Seconds());
        if(RunSox(A + " " + B + F + R + " splice " + pos + ",0.005,0.001") && Load(R, ref))
        {
            dsp::Copy(a, got);
            dsp::SpliceInto(got, b, dsp::SpliceOverlap(0.005), dsp::SpliceSearch(0.001));
            Compare("splice p,0.005,0.001", ref, got, 100);
        }
        if(RunSox(A + " " + B + F + R + " splice " + pos) && Load(R, ref))
        {
            dsp::Copy(a, got);
            dsp::SpliceInto(got, b, dsp::SpliceOverlap(0), dsp::SpliceSearch(0));
            Compare("splice p (defaults)", ref, got, 100);
        }
    }
    std::remove(A.c_str());
    std::remove(B.c_str());
    std::remove(R.c_str());
}

// ------------------------------------------------------------- 3. generate
static void TestGenerate(Engine& e)
{
    std::printf("[generate]\n");
    const uint32_t       cap = SecToFrames(40);
    std::vector<int16_t> out(cap * 2), out2(cap * 2);

    struct Case
    {
        const char* name;
        int         beats;
        double      tempo;
        uint64_t    seed;
        double      scale; // multiplies every effect probability
    } cases[] = {
        {"default_s1", 17, 0, 1, 1},       {"default_s2", 17, 0, 2, 1},
        {"default_s3", 17, 0, 3, 1},       {"tempo165_s4", 17, 165, 4, 1},
        {"tempo190_s5", 33, 190, 5, 1},    {"chaos_s6", 17, 0, 6, 4},
        {"chaos_s7", 33, 0, 7, 5},         {"long_s8", 65, 174, 8, 1},
    };
    for(const Case& c : cases)
    {
        Params p;
        p.beats     = c.beats;
        p.out_tempo = c.tempo;
        p.seed      = c.seed;
        p.p_reverse *= c.scale;
        p.p_stutter *= c.scale;
        p.p_pitch *= c.scale;
        p.p_trunc *= c.scale;
        p.p_half *= c.scale;
        p.p_reverb *= c.scale;
        p.p_stretch *= c.scale;
        auto      t0 = std::chrono::steady_clock::now();
        GenResult r  = e.Generate(p, out.data(), cap);
        double    ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        double    tempo   = c.tempo > 0 ? c.tempo : e.tempo();
        uint32_t  expect  = static_cast<uint32_t>(60.0 / tempo * r.beats * kRate + .5);
        int       peak    = 0;
        double    rms     = 0;
        for(uint32_t i = 0; i < r.frames * 2; i++)
        {
            int v = std::abs(static_cast<int>(out[i]));
            peak  = v > peak ? v : peak;
            rms += static_cast<double>(out[i]) * out[i];
        }
        rms = r.frames ? std::sqrt(rms / (r.frames * 2)) / 32768 : 0;
        std::printf("  %-12s %2d beats @ %5.1f bpm: %7u frames (expect %7u) iter %2d peak %5d rms %.3f  %6.1f ms\n",
                    c.name, r.beats, tempo, r.frames, expect, r.iterations, peak, rms, ms);
        CHECK(r.ok, "%s failed", c.name);
        CHECK(std::abs(static_cast<int>(r.frames) - static_cast<int>(expect)) <= 2,
              "%s length %u != %u", c.name, r.frames, expect);
        CHECK(rms > 0.02, "%s is (nearly) silent", c.name);

        GenResult r2 = e.Generate(p, out2.data(), cap);
        CHECK(r2.frames == r.frames
                  && std::memcmp(out.data(), out2.data(), r.frames * 4) == 0,
              "%s is not deterministic", c.name);

        std::string path = g_out + "/" + c.name + ".wav";
        FileWriter  w(path.c_str());
        WriteWav16(w, out.data(), r.frames);
    }
}

struct ChopTrace
{
    std::vector<int> used; // 0-based chop of every iteration
};
static void TraceLog(void* ctx, const char* msg)
{
    int i, s;
    if(std::sscanf(msg, "i%d s%d", &i, &s) == 2)
        static_cast<ChopTrace*>(ctx)->used.push_back(s - 1);
}

static void TestChop(Engine& e)
{
    std::printf("[chop]\n");
    const uint32_t       cap = SecToFrames(40);
    std::vector<int16_t> a(cap * 2), b(cap * 2);
    std::vector<uint8_t> use(Analyzer::kMaxSlices, 1);
    Params               p;
    p.beats = 17;
    p.seed  = 3;
    auto expect = [&](const GenResult& r) {
        return static_cast<uint32_t>(60.0 / e.tempo() * r.beats * kRate + .5);
    };

    // every chop selected == no selection == dnb.lua
    GenResult r0 = e.Generate(p, a.data(), cap);
    p.use        = use.data();
    GenResult r1 = e.Generate(p, b.data(), cap);
    CHECK(r0.ok && r1.frames == r0.frames && std::memcmp(a.data(), b.data(), r0.frames * 4) == 0,
          "all chops on changed the output");

    // kicks only: every iteration must come from a kick chop
    const Slice* s = e.analyzer().slices();
    for(uint32_t k = 0; k < e.analyzer().num_slices(); k++)
        use[k] = s[k].cls == kSliceKick;
    ChopTrace t;
    Hooks     h;
    h.log = TraceLog;
    h.ctx = &t;
    GenResult r2 = e.Generate(p, b.data(), cap, &h);
    bool      only_kicks = !t.used.empty();
    for(int v : t.used)
        only_kicks = only_kicks && use[v];
    CHECK(r2.ok && only_kicks, "kick-only selection used another chop");
    CHECK(std::abs(static_cast<int>(r2.frames) - static_cast<int>(expect(r2))) <= 2, "kick-only length %u",
          r2.frames);

    // a single chop still fills the loop
    std::fill(use.begin(), use.end(), 0);
    use[1]       = 1;
    GenResult r3 = e.Generate(p, b.data(), cap);
    CHECK(r3.ok && std::abs(static_cast<int>(r3.frames) - static_cast<int>(expect(r3))) <= 2,
          "single chop length %u", r3.frames);

    // equal chops
    std::fill(use.begin(), use.end(), 1);
    CHECK(e.Chop(16) == 16 && e.analyzer().equal_chops(), "16 equal chops");
    uint32_t end = 0;
    bool     contiguous = true;
    for(uint32_t k = 0; k < 16; k++)
    {
        contiguous = contiguous && s[k].start == end;
        end        = s[k].start + s[k].len;
    }
    CHECK(contiguous && end == e.source().n, "equal chops do not tile the break");
    for(uint32_t k = 0; k < 16; k++)
        use[k] = k % 4 == 0;
    t.used.clear();
    GenResult r4 = e.Generate(p, b.data(), cap, &h);
    bool      sel = !t.used.empty();
    for(int v : t.used)
        sel = sel && v % 4 == 0;
    CHECK(r4.ok && sel && std::abs(static_cast<int>(r4.frames) - static_cast<int>(expect(r4))) <= 2,
          "equal chop selection (len %u)", r4.frames);
    const uint32_t capped = e.Chop(64); // a 2.7 s break at 177 bpm has 32 16ths
    CHECK(capped == 32, "64 chops capped to %u", capped);
    p.use        = nullptr;
    GenResult r5 = e.Generate(p, b.data(), cap);
    CHECK(r5.ok && std::abs(static_cast<int>(r5.frames) - static_cast<int>(expect(r5))) <= 2,
          "16th chops length %u", r5.frames);

    // back to the hits: identical to the first run
    CHECK(e.Chop(0) == 12 && !e.analyzer().equal_chops(), "auto chops not restored");
    GenResult r6 = e.Generate(p, b.data(), cap);
    CHECK(r6.frames == r0.frames && std::memcmp(a.data(), b.data(), r0.frames * 4) == 0,
          "Chop(0) did not restore the original");
    // a new break of the same length must not reuse the old one's analysis
    CHECK(e.Analyse(0, 16) && e.tempo() == 177, "equal-chop analysis");
    std::vector<float> zeros(e.source().n * 2, 0.f);
    Buf                silent;
    silent.d = zeros.data();
    silent.n = silent.cap = e.source().n;
    const std::string sp = g_out + "/silent.wav";
    WriteWavF32(sp.c_str(), silent);
    FileReader quiet(sp.c_str());
    CHECK(e.LoadSource(quiet, true).ok, "load silent break");
    const bool ok = e.Analyse(0, 16);
    CHECK(!ok || e.tempo() != 177, "stale analysis reused (%.1f bpm)", e.tempo());
    FileReader again((g_assets + "/amen_resampled.wav").c_str());
    CHECK(e.LoadSource(again, true).ok && e.Analyse(0) && e.tempo() == 177, "reload amen");
    std::printf("chop: ok\n");
}

static void TestConfig()
{
    std::printf("[config]\n");
    Settings   s;
    const char txt[] = "beats = 32\n tempo=172.5 # comment\np_stutter = 150\nlegacy_rate = no\n"
                       "sample = my break.wav  \nbogus = 1\nseed = 42\n";
    int        bad_line;
    int        bad = ParseConfig(txt, sizeof(txt) - 1, s, &bad_line);
    CHECK(bad == 1 && bad_line == 6, "bad=%d line=%d", bad, bad_line);
    CHECK(s.beats == 32 && s.tempo == 172.5 && s.gen.p_stutter == 100 && !s.legacy_rate
              && std::strcmp(s.sample, "my break.wav") == 0 && s.seed == 42,
          "config values");
    char     buf[4096];
    uint32_t n = DefaultConfig(buf, sizeof(buf));
    Settings d;
    CHECK(ParseConfig(buf, n, d) == 0, "default config does not parse cleanly");
    std::printf("  ok\n");
}

// Engine_Makebreakbeat.sc + lattice semantics (see player.h).
static void TestPlayer()
{
    const uint32_t       fpb = 100, beats = 4, frames = fpb * (beats + 1); // generated with beats+1
    std::vector<int16_t> mem(frames * 2, 16384), mem2(frames * 4, -16384);
    Player               p;
    p.Init(48000.f);
    std::vector<float> l(frames * 20), r(frames * 20);
    auto run = [&](uint32_t n) { p.Process(l.data(), r.data(), n, 1.f); };

    run(64); // nothing loaded: silence
    CHECK(l[63] == 0 && !p.playing(), "idle player not silent");

    PlayBuf& b = p.buf[p.ClaimFree()];
    b          = {mem.data(), frames, double(fpb), int(beats), 120};
    p.Publish(0, true); // first buffer + auto_play starts at once
    CHECK(p.playing(), "auto_play did not start");
    run(1);
    // lattice 0: tozero flips aOrB 1 -> 0 and restarts reader B
    CHECK(p.active() == 0 && p.lattice() == 0 && p.a_or_b() == 0 && p.pos_b() == 1 && p.pos_a() == 1,
          "first tick: lattice %d aOrB %d A %u B %u", p.lattice(), p.a_or_b(), p.pos_a(), p.pos_b());
    run(fpb * beats - 1); // beats 1..3 do not restart
    CHECK(p.lattice() == 3 && p.a_or_b() == 0 && p.pos_b() == fpb * beats, "lattice %d B %u", p.lattice(),
          p.pos_b());
    run(1); // beat 4: flip back to A, A restarts, B keeps running into the tail
    CHECK(p.lattice() == 4 && p.a_or_b() == 1 && p.pos_a() == 1 && p.pos_b() == fpb * beats + 1,
          "loop restart: aOrB %d A %u B %u", p.a_or_b(), p.pos_a(), p.pos_b());
    run(48000 - frames); // ~1 s: constant buffer -> output settles to 0.5 (RLPF has unity DC gain)
    CHECK(std::fabs(l[48000 - frames - 1] - 0.5f) < 1e-3f, "steady output %f", l[48000 - frames - 1]);

    // swap on the next beat; positions scaled by tempo ratio, bar count reset on new length
    const int t = p.ClaimFree();
    CHECK(t == 1, "free buffer %d", t);
    p.buf[1] = {mem2.data(), frames * 2, double(fpb * 2), int(beats), 60};
    p.Publish(1, true);
    const uint32_t to_beat = fpb - (48000 - frames + beats * fpb) % fpb; // samples to next tick
    run(to_beat - 1);
    CHECK(p.active() == 0, "swapped before the beat");
    const uint32_t pa = p.pos_a(), pb = p.pos_b();
    run(1);
    // (if this beat is also a loop start, the newly selected reader restarts at 0)
    const bool     rs = p.last_tick_restarted();
    const uint32_t ea = rs && p.a_or_b() == 1 ? 1 : (pa * 2) % (frames * 2) + 1;
    const uint32_t eb = rs && p.a_or_b() == 0 ? 1 : (pb * 2) % (frames * 2) + 1;
    CHECK(p.active() == 1 && p.pos_a() == ea && p.pos_b() == eb, "swap positions A %u->%u B %u->%u", pa,
          p.pos_a(), pb, p.pos_b());
    run(9600); // 200 ms later the output is the new buffer
    CHECK(std::fabs(l[9599] + 0.5f) < 1e-3f, "after swap %f", l[9599]);

    // K3 stop: amp lags to 0 in ~0.1 s (-60 dB) but the lattice keeps running
    p.TogglePlay();
    const int lat = p.lattice();
    run(9600);
    CHECK(!p.playing() && std::fabs(l[9599]) < 1e-3f && p.lattice() > lat, "stop: %f lattice %d", l[9599],
          p.lattice());
    // K3 start: hard restart -> tick on the next sample, lattice 0 -> tozero
    const int ab = p.a_or_b();
    p.TogglePlay();
    run(1);
    CHECK(p.playing() && p.lattice() == 0 && p.a_or_b() == 1 - ab, "restart lattice %d", p.lattice());
    // 50 ms crossfade (SC Lag 0.05: -60 dB after 2400 samples)
    p.buf[0] = {mem.data(), frames, double(fpb * 2), int(beats), 60};
    p.Publish(p.ClaimFree(), true);
    run(fpb * 2 * beats + 2400 + 4800);
    CHECK(std::fabs(l[fpb * 2 * beats + 2400 + 4799] - 0.5f) < 2e-3f, "crossfaded to %f",
          l[fpb * 2 * beats + 2400 + 4799]);
    // chop audition: plays once over silence, then stops
    p.TogglePlay();
    run(9600);
    std::vector<float> chop(2 * 1000, 0.25f);
    p.Audition(chop.data(), 1000);
    run(1200);
    CHECK(std::fabs(l[500] - 0.25f) < 1e-3f && std::fabs(l[1100]) < 1e-3f, "audition %f / %f", l[500],
          l[1100]);
    Player q; // no loop loaded yet (auto_generate = 0)
    q.Init(48000.f);
    q.Audition(chop.data(), 1000);
    q.Process(l.data(), r.data(), 600, 1.f);
    CHECK(std::fabs(r[500] - 0.25f) < 1e-2f, "audition without a loop %f", r[500]);
    std::printf("player: ok\n");
}

// LIVE mode. The test loop is a ramp whose L + R add up to the frame index,
// so the output decodes back to the position being read.
static void TestLive(Engine& e)
{
    std::printf("[live]\n");
    const uint32_t       fpb = 3600, beats = 16, frames = fpb * (beats + 1), ql = 4 * fpb;
    std::vector<int16_t> ramp(frames * 2), ramp2(5 * fpb * 2);
    for(uint32_t i = 0; i < frames; i++)
    {
        ramp[2 * i]     = static_cast<int16_t>(i / 2);
        ramp[2 * i + 1] = static_cast<int16_t>((i + 1) / 2);
    }
    for(uint32_t i = 0; i < 5 * fpb; i++) // another version of a quarter: 30000 + i
    {
        ramp2[2 * i]     = static_cast<int16_t>((30000 + i) / 2);
        ramp2[2 * i + 1] = static_cast<int16_t>((30001 + i) / 2);
    }
    Player p;
    p.Init(48000.f);
    p.buf[p.ClaimFree()] = {ramp.data(), frames, double(fpb), int(beats), 200};
    p.Publish(0, true);
    std::vector<double>   dec; // decoded read position
    std::vector<uint32_t> trn; // transport position in the playing buffer
    std::vector<int>      act; // playing buffer
    auto run = [&](uint32_t n) {
        dec.clear(), trn.clear(), act.clear();
        for(uint32_t i = 0; i < n; i++)
        {
            float l, r;
            p.Process(&l, &r, 1, 1.f);
            const uint32_t s = p.a_or_b() ? p.pos_a() : p.pos_b(), bf = p.buf[p.active()].frames;
            dec.push_back((static_cast<double>(l) + r) * 32768.0);
            trn.push_back((s + bf - 1) % bf);
            act.push_back(p.active());
        }
    };
    auto to = [&](uint32_t pos) { // run until the transport is at pos
        while(trn.empty() || trn.back() != pos)
            run(1);
    };
    // index of the first sample playing buffer `b`
    auto start_of = [&](int b) {
        size_t i = 0;
        while(i < act.size() && act[i] != b)
            i++;
        return i;
    };
    // decoded == offset + transport from sample a on (until n), after the crossfades
    auto reads = [&](size_t a, size_t n, double offset) {
        int bad = 0;
        for(size_t i = a + 7200; i < std::min(n, dec.size()); i++)
            bad += std::fabs(dec[i] - offset - trn[i]) > 3; // the readers' crossfade may be 1 frame apart
        if(bad)
            std::printf("  (%u: %.2f vs %.0f + %u)\n", unsigned(a + 7200), dec[a + 7200], offset, trn[a + 7200]);
        return bad;
    };

    // the quarter being played (for the quarter LEDs)
    int wrong = 0;
    for(uint32_t i = 0; i < 2 * beats * fpb; i++)
    {
        run(1);
        wrong += p.playing_quarter() != static_cast<int>(std::min(3u, trn.back() / ql));
    }
    CHECK(wrong == 0, "playing_quarter wrong for %d samples", wrong);

    // Solo: quarter 2 on its own, from the next bar (half way through beat 6)
    uint32_t s0, s1;
    Player::Quarter(p.buf[0], 1, s0, s1);
    CHECK(s0 == ql && s1 == 2 * ql, "quarter 2 is %u..%u", s0, s1);
    to(5 * fpb + fpb / 2);
    p.buf[2] = {ramp.data() + 2 * s0, 5 * fpb, double(fpb), 4, 200};
    p.Publish(2, false, 4);
    run(10 * fpb);
    size_t i0 = start_of(2);
    CHECK(i0 > 0 && i0 < act.size() && trn[i0 - 1] == 8 * fpb - 1 && trn[i0] == 0,
          "solo started after %u (want %u)", i0 > 0 && i0 < act.size() ? trn[i0 - 1] : 0, 8 * fpb - 1);
    CHECK(reads(i0, i0 + ql, s0) == 0 && reads(i0 + ql, i0 + 2 * ql, s0) == 0 && trn[i0 + ql] == 0,
          "solo does not loop quarter 2");

    // another version: on the next beat, at the same place
    to(fpb + fpb / 2);
    p.buf[3] = {ramp2.data(), 5 * fpb, double(fpb), 4, 200};
    p.Publish(3, false);
    run(2 * fpb);
    i0 = start_of(3);
    CHECK(i0 > 0 && i0 < act.size() && trn[i0 - 1] == 2 * fpb - 1 && trn[i0] == 2 * fpb,
          "version switched at %u", i0 < act.size() ? trn[i0] : 0);
    CHECK(reads(i0 + 200 - 7200, act.size(), 30000) == 0, "version 2 not playing");

    // back to the whole loop: at the end of the solo, from the top
    p.Publish(0, false, 4);
    run(6 * fpb);
    i0 = start_of(0);
    CHECK(i0 > 0 && i0 < act.size() && trn[i0 - 1] == 4 * fpb - 1 && trn[i0] == 0,
          "loop resumed after %u", i0 > 0 && i0 < act.size() ? trn[i0 - 1] : 0);
    CHECK(reads(i0, act.size(), 0) == 0, "loop not playing from the top");

    // Regenerating a quarter: a new 16-beat loop at 160 bpm, then quarters
    // 2 and 4 made again and spliced in.
    const uint32_t       cap = SecToFrames(12);
    std::vector<int16_t> beat(cap * 2), part(cap * 2), out(cap * 2);
    Params               gp;
    gp.seed      = 11;
    gp.out_tempo = 160;
    GenResult g  = e.Generate(gp, beat.data(), cap);
    CHECK(g.ok, "live: generate");
    PlayBuf old = {beat.data(), g.frames, 48000.0 * 60.0 / g.out_tempo, g.beats - 1, 160};
    Params  qp  = gp;
    qp.beats    = old.loop_beats / 4 + 2;
    qp.seed     = 12;
    qp.start_beat = 0;
    GenResult a = e.Generate(qp, part.data(), cap);
    qp.start_beat = 4;
    GenResult b = e.Generate(qp, part.data() + cap, cap / 2);
    CHECK(a.ok && b.ok && std::memcmp(part.data(), part.data() + cap, old.fpb * 4) != 0,
          "start_beat did not change the first chop");
    // background versions can be given up part way
    volatile bool stop = true;
    Hooks         sh;
    sh.stop = &stop;
    CHECK(!e.Generate(qp, part.data(), cap, &sh).ok, "stop flag ignored");
    CHECK(e.Generate(qp, part.data(), cap).ok, "generate after a stop failed");
    float max_step = 0;
    for(int q : {1, 3})
    {
        uint32_t s0, s1;
        Player::Quarter(old, q, s0, s1);
        CHECK(s0 == static_cast<uint32_t>(q * 4 * old.fpb + .5), "quarter %d starts at %u", q, s0);
        qp.start_beat = q * old.loop_beats / 4.0;
        qp.seed       = 20 + q;
        GenResult r   = e.Generate(qp, out.data() + 2 * s0, cap - s0);
        CHECK(r.ok && s0 + r.frames >= s1 + 240, "quarter %d: %u frames", q, r.frames);
        std::vector<int16_t> fresh(out.begin() + 2 * s0, out.begin() + 2 * (s0 + r.frames));
        Player::Splice(old, out.data(), s0, s1, r.frames);
        bool ok = true;
        for(uint32_t i = 0; i < old.frames; i++)
        {
            const bool in = i >= s0 && i < s1, from_new = (i >= s0 + 240 && i < s1) || (q == 3 && i >= s0 + 240);
            const bool from_old = !in && (i < s0 || i >= s1 + 240) && q != 3;
            for(int c = 0; c < 2; c++)
            {
                if(from_new)
                    ok = ok && out[2 * i + c] == fresh[2 * (i - s0) + c];
                if(from_old || i < s0)
                    ok = ok && out[2 * i + c] == old.data[2 * i + c];
            }
        }
        CHECK(ok, "quarter %d: splice kept the wrong audio", q);
        for(uint32_t i = 1; i < old.frames; i++)
            for(int c = 0; c < 2; c++)
            {
                const float d = std::abs(out[2 * i + c] - out[2 * i - 2 + c]);
                const bool  x = (i >= s0 && i < s0 + 241) || (i >= s1 && i < s1 + 241);
                if(x)
                    max_step = std::max(max_step, d);
            }
    }
    float own = 0;
    for(const std::vector<int16_t>* v : {&beat, &out})
        for(uint32_t i = 1; i < old.frames; i++)
            own = std::max(own, static_cast<float>(std::abs((*v)[2 * i] - (*v)[2 * i - 2])));
    std::printf("  splice: largest step at a seam %.0f (audio %.0f)\n", max_step, own);
    CHECK(max_step <= own, "splice seam step %.0f > %.0f", max_step, own);

    // A real beat with quarters soloed, switched between versions and kept
    // or dropped at random, as the firmware does: no clicks, NaNs or
    // overloads. Clicks are measured as the largest step between samples.
    const uint32_t       big = SecToFrames(20);
    std::vector<int16_t> mem[2] = {std::vector<int16_t>(big * 2), std::vector<int16_t>(big * 2)};
    std::copy(beat.begin(), beat.begin() + 2 * old.frames, mem[0].begin());
    const uint32_t       total = static_cast<uint32_t>(old.fpb * 128);
    std::vector<int16_t> demo(total * 2);
    Player               q;
    q.Init(48000.f);
    q.buf[0]      = old;
    q.buf[0].data = mem[0].data();
    q.Publish(0, true);
    uint32_t rng = 12345;
    auto     rnd = [&](uint32_t n) {
        rng = rng * 1664525u + 1013904223u;
        return (rng >> 8) % n;
    };
    const int      Q = old.loop_beats / 4;
    const uint32_t slot_len = static_cast<uint32_t>((Q + 2) * old.fpb) + 64;
    int            solo = -1, full = 0, pool = 1, n_slots = 0, shown = 0, solos = 0, switches = 0, kept = 0;
    uint32_t       ss0 = 0, ss1 = 0, base = 0, slot_frames[8] = {};
    auto           slot = [&](int k) {
        PlayBuf b    = q.buf[full];
        b.loop_beats = Q;
        if(k == 0)
        {
            b.data   = mem[full].data() + 2 * ss0;
            b.frames = std::min(static_cast<uint32_t>((Q + 1) * b.fpb), b.frames - ss0);
        }
        else
        {
            b.data   = mem[pool].data() + 2 * (base + (k - 1) * slot_len);
            b.frames = slot_frames[k];
        }
        return b;
    };
    float    step[2] = {0, 0}, prev[2] = {0, 0}, peak = 0;
    bool     finite  = true;
    uint32_t next    = static_cast<uint32_t>(old.fpb * 32); // two plain loops first
    for(uint32_t i = 0; i < total; i++)
    {
        if(i == next)
        {
            const int act_b = q.active();
            if(solo < 0 && q.pending() < 0 && act_b < 2)
            {
                solo = rnd(4), full = act_b, pool = 1 - act_b;
                Player::Quarter(q.buf[full], solo, ss0, ss1);
                base    = q.buf[full].frames;
                n_slots = 1 + std::min<int>(7, (big - base) / slot_len);
                for(int k = 1; k < n_slots; k++)
                {
                    qp.start_beat  = solo * Q;
                    qp.seed        = 100 + solos * 8 + k;
                    GenResult r    = e.Generate(qp, mem[pool].data() + 2 * (base + (k - 1) * slot_len), slot_len);
                    slot_frames[k] = r.frames;
                    CHECK(r.ok && r.frames >= (Q + 1) * old.fpb, "version %d: %u frames", k, r.frames);
                }
                q.buf[2] = slot(0);
                q.Publish(2, false, 4);
                shown = 0;
                solos++;
            }
            else if(solo >= 0)
            {
                const int what = rnd(5);
                if(what < 3)
                {
                    shown        = rnd(n_slots);
                    const bool in = act_b >= 2;
                    const int  ab = in && act_b == 2 ? 3 : 2;
                    q.buf[ab]     = slot(shown);
                    q.Publish(ab, false, in ? 1 : 4);
                    switches++;
                }
                else if(what == 3 && shown > 0)
                {
                    int16_t* dst = mem[pool].data();
                    const uint32_t m = std::min(slot_frames[shown], q.buf[full].frames - ss0);
                    std::memcpy(dst + 2 * ss0, slot(shown).data, m * 4);
                    Player::Splice(q.buf[full], dst, ss0, ss1, m);
                    q.buf[pool]      = q.buf[full];
                    q.buf[pool].data = dst;
                    q.Publish(pool, false, 4);
                    solo = -1;
                    kept++;
                }
                else
                {
                    q.Publish(full, false, 4);
                    solo = -1;
                }
            }
            next += static_cast<uint32_t>(old.fpb * (0.3 + rnd(100) * 0.02));
        }
        float l, r;
        q.Process(&l, &r, 1, 1.f);
        const float o[2] = {l, r};
        for(int ch = 0; ch < 2; ch++)
        {
            finite = finite && std::isfinite(o[ch]);
            peak   = std::max(peak, std::fabs(o[ch]));
            if(i > 48000)
                step[i >= old.fpb * 32] = std::max(step[i >= old.fpb * 32], std::fabs(o[ch] - prev[ch]));
            prev[ch]         = o[ch];
            demo[2 * i + ch] = static_cast<int16_t>(std::lround(std::max(-1.f, std::min(1.f, o[ch])) * 32767));
        }
    }
    const float limit = std::max(step[0], own / 32768) + 0.03f;
    std::printf("  random: %d solos, %d version changes, %d kept: largest step %.3f (plain loop %.3f), peak %.3f\n",
                solos, switches, kept, step[1], step[0], peak);
    CHECK(finite && peak < 1.2f && step[1] <= limit && solos >= 4 && switches >= 8 && kept >= 2,
          "random: step %.3f > %.3f or peak %.3f", step[1], limit, peak);
    std::string path = g_out + "/live_demo.wav";
    FileWriter  w(path.c_str());
    WriteWav16(w, demo.data(), total);
    std::printf("live: ok\n");
}

int main(int argc, char** argv)
{
    g_assets = argc > 1 ? argv[1] : "../makebreakbeat/lib";
    g_out    = argc > 2 ? argv[2] : "tests/out";
    if(const char* s = std::getenv("MBB_SOX"))
        g_sox = s;

    static Engine     e;
    const size_t      bytes = Engine::RequiredBytes();
    std::vector<char> mem(bytes);
    std::printf("engine arena: %.1f MB\n", bytes / 1048576.0);
    if(!e.Init(mem.data(), bytes))
    {
        std::printf("engine init failed\n");
        return 1;
    }
    FileReader src((g_assets + "/amen_resampled.wav").c_str());
    LoadResult lr = e.LoadSource(src, true);
    CHECK(lr.ok, "load source");
    FileReader kick((g_assets + "/kick.wav").c_str()), snare((g_assets + "/snare.wav").c_str());
    CHECK(e.LoadDrum(kick, true, true, true).ok, "load kick");
    CHECK(e.LoadDrum(snare, false, true, true).ok, "load snare");
    auto t0 = std::chrono::steady_clock::now();
    CHECK(e.Analyse(0), "analyse");
    std::printf("analysis took %.1f ms\n",
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());

    TestConfig();
    TestPlayer();
    TestAnalysis(e);
    if(!g_sox.empty())
        TestSoxEffects(e);
    else
        std::printf("[sox A/B skipped: set MBB_SOX]\n");
    TestGenerate(e);
    TestChop(e);
    TestLive(e);

    std::printf(g_fail ? "\n%d FAILURE(S)\n" : "\nALL TESTS PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}

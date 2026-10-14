#include "engine.h"

#include <cstdio>
#include <cstring>

namespace mbb
{
namespace
{
// `sox file -n stat` prints "Length (seconds)" with %f; dnb.lua does all of
// its arithmetic on that 6-decimal value.
double Len6(const Buf& b)
{
    return std::floor(static_cast<double>(b.n) * 1e6 / kRate + 0.5) / 1e6;
}

// dnb.lua caches effect results by file name (e.g. "<slice>kick.wavpitch.wav")
// and only draws the random parameter when the file does not exist yet. The
// chain of applied effects is hashed to emulate that.
uint64_t Fnv(uint64_t h, const char* s)
{
    for(; *s; s++)
    {
        h ^= static_cast<uint8_t>(*s);
        h *= 0x100000001b3ULL;
    }
    return h;
}
uint64_t Fnv(uint64_t h, uint32_t v)
{
    for(int i = 0; i < 4; i++)
    {
        h ^= (v >> (8 * i)) & 0xff;
        h *= 0x100000001b3ULL;
    }
    return h;
}
constexpr uint64_t kFnvBasis = 0xcbf29ce484222325ULL;
} // namespace

// ---------------------------------------------------------------- cache
void Engine::Cache::Clear()
{
    std::memset(used, 0, sizeof(used));
}

bool Engine::Cache::Find(uint64_t k, int32_t& v) const
{
    for(uint32_t i = 0, idx = static_cast<uint32_t>(k) & (kSize - 1); i < kSize;
        i++, idx = (idx + 1) & (kSize - 1))
    {
        if(!used[idx])
            return false;
        if(keys[idx] == k)
        {
            v = vals[idx];
            return true;
        }
    }
    return false;
}

void Engine::Cache::Put(uint64_t k, int32_t v)
{
    for(uint32_t i = 0, idx = static_cast<uint32_t>(k) & (kSize - 1); i < kSize;
        i++, idx = (idx + 1) & (kSize - 1))
    {
        if(!used[idx] || keys[idx] == k)
        {
            used[idx] = 1;
            keys[idx] = k;
            vals[idx] = v;
            return;
        }
    }
}

// ---------------------------------------------------------------- setup
size_t Engine::RequiredBytes(const Limits& lim)
{
    const size_t fb  = kChannels * sizeof(float);
    size_t       src = SecToFrames(lim.source_sec);
    size_t       n   = src * fb;
    n += SecToFrames(lim.joined_sec) * fb;
    n += 2 * SecToFrames(lim.drum_sec) * fb;
    n += kWork * SecToFrames(lim.work_sec) * fb;
    n += (src / 128 + 2) * 5; // onset detection function + silence flags
    n += 512 * 1024;          // reverb delay lines + alignment slack
    return n;
}

bool Engine::Init(void* mem, size_t bytes, const Limits& lim)
{
    arena_.Init(mem, bytes);
    lim_   = lim;
    ready_ = false;
    bool ok = arena_.AllocBuf(src_, SecToFrames(lim.source_sec))
              && arena_.AllocBuf(joined_, SecToFrames(lim.joined_sec))
              && arena_.AllocBuf(kick_, SecToFrames(lim.drum_sec))
              && arena_.AllocBuf(snare_, SecToFrames(lim.drum_sec));
    for(int i = 0; ok && i < kWork; i++)
    {
        ok      = arena_.AllocBuf(work_[i], SecToFrames(lim.work_sec));
        busy_[i] = false;
    }
    ok = ok && reverb_.Init(arena_) && analyzer_.Init(arena_, src_.cap);
    return ok;
}

LoadResult Engine::LoadSource(Reader& r, bool legacy_rate)
{
    ready_ = false;
    tempo_ = 0;
    src_.n = 0;
    analyzer_.InvalidateSource();
    LoadOptions opt;
    if(legacy_rate)
        opt.rate_override = kRate; // `sox -r 48000 in.wav out.wav`
    // Decode into the (larger) joined buffer first so a resample has room.
    LoadResult res = LoadWav(r, joined_, opt);
    if(!res.ok)
    {
        joined_.n = 0;
        return res;
    }
    uint32_t wanted = joined_.n;
    if(res.effective_rate == kRate)
    {
        dsp::Copy(joined_, src_);
    }
    else
    {
        const double step = static_cast<double>(res.effective_rate) / kRate;
        wanted            = static_cast<uint32_t>(joined_.n / step + 0.5);
        dsp::Resample(joined_, src_, step, wanted);
        res.effective_rate = kRate;
    }
    if(wanted > src_.n)
        res.truncated = true;
    dsp::Clip(src_);
    joined_.n     = 0;
    src_channels_ = res.source_channels;
    return res;
}

LoadResult Engine::LoadDrum(Reader& r, bool kick, bool legacy_rate, bool legacy_channels)
{
    Buf& dst = kick ? kick_ : snare_;
    dst.n    = 0;
    // dnb.lua: `sox -r <rate> -c <channels> kick.wav kick_merge.wav gain <db>`,
    // rate/channels taken from the (48 kHz) break.
    LoadOptions opt;
    if(legacy_rate)
        opt.rate_override = kRate;
    if(legacy_channels)
        opt.channels_override = src_channels_ ? src_channels_ : 2;
    Buf&       scratch = work_[0];
    LoadResult res     = LoadWav(r, scratch, opt);
    if(!res.ok)
    {
        scratch.n = 0;
        return res;
    }
    if(res.effective_rate == kRate)
        dsp::Copy(scratch, dst);
    else
    {
        const double step = static_cast<double>(res.effective_rate) / kRate;
        dsp::Resample(scratch, dst, step, static_cast<uint32_t>(scratch.n / step + 0.5));
        res.effective_rate = kRate;
    }
    dsp::Clip(dst);
    scratch.n = 0;
    return res;
}

bool Engine::Analyse(double input_tempo, uint32_t equal_chops)
{
    ready_ = false;
    if(src_.n == 0)
        return false;
    if(input_tempo > 0)
    {
        tempo_       = input_tempo;
        tempo_exact_ = input_tempo;
    }
    else
    {
        double exact = 0;
        int    t     = analyzer_.DetectTempo(src_, &exact);
        // The original errors out without a tempo; fall back to 120 BPM.
        tempo_       = t > 0 ? t : 120;
        tempo_exact_ = t > 0 ? exact : 120;
    }
    ready_ = Chop(equal_chops) > 0;
    return ready_;
}

uint32_t Engine::Chop(uint32_t equal_chops)
{
    if(src_.n == 0 || tempo_ <= 0)
        return 0;
    if(equal_chops == 0)
        analyzer_.SliceSource(src_, tempo_);
    else
    {
        // At most one chop per 16th note (the grid Generate keeps them on).
        const uint32_t max_n
            = static_cast<uint32_t>(FramesToSec(src_.n) / (60.0 / tempo_ / 4) + 0.5);
        analyzer_.ChopEqual(src_, equal_chops < max_n ? equal_chops : (max_n ? max_n : 1));
    }
    IndexSlices();
    ready_ = analyzer_.num_slices() > 0;
    return analyzer_.num_slices();
}

void Engine::IndexSlices()
{
    n_kicks_ = n_snares_ = 0;
    const Slice* s       = analyzer_.slices();
    for(uint32_t i = 0; i < analyzer_.num_slices(); i++)
    {
        if(s[i].cls == kSliceKick)
            kicks_[n_kicks_++] = static_cast<uint16_t>(i);
        else if(s[i].cls == kSliceSnare)
            snares_[n_snares_++] = static_cast<uint16_t>(i);
    }
}

int Engine::MaxBeats(double out_tempo, uint32_t out_cap_frames) const
{
    const double t    = tempo_ > 0 ? tempo_ : 120;
    const double nt   = out_tempo > 0 ? out_tempo : t;
    // joined must hold the trimmed result (at the source tempo) ...
    int b1 = static_cast<int>(std::floor((FramesToSec(joined_.cap) - 0.05) / (60.0 / t)));
    // ... and the output buffer the tempo-changed result.
    int b2 = static_cast<int>(std::floor((FramesToSec(out_cap_frames) - 0.01) / (60.0 / nt)));
    int b  = b1 < b2 ? b1 : b2;
    return b < 1 ? 1 : b;
}

// ---------------------------------------------------------------- helpers
Buf* Engine::Take()
{
    for(int i = 0; i < kWork; i++)
        if(!busy_[i])
        {
            busy_[i]   = true;
            work_[i].n = 0;
            return &work_[i];
        }
    return nullptr;
}

void Engine::Give(Buf* b)
{
    for(int i = 0; i < kWork; i++)
        if(&work_[i] == b)
            busy_[i] = false;
}

void Engine::Log(const Hooks* h, const char* msg)
{
    if(h && h->log)
        h->log(h->ctx, msg);
}

bool Engine::StretchTo(Buf*& cur, double new_sec)
{
    const double dur = Len6(*cur);
    if(new_sec > dur)
    {
        if(dur <= 0)
            return false; // `stretch inf` fails and no file is written
        Buf* o = Take();
        dsp::Stretch(*cur, *o, new_sec / dur * 1.05);
        Give(cur);
        cur = o;
    }
    if(new_sec >= 0)
        dsp::Trim(*cur, SecToFrames(new_sec));
    else
    {
        // `trim 0 -x` keeps all but the last x seconds (empty if x > length)
        uint32_t cut = SecToFrames(-new_sec);
        cur->n       = cut >= cur->n ? 0 : cur->n - cut;
    }
    return true;
}

void Engine::MixDrum(Buf*& cur, const Buf& drum, double db)
{
    Buf* g = Take();
    Buf* o = Take();
    // Only the first len(v) frames of the drum survive `trim 0 len(v)`.
    Buf part = drum;
    if(part.n > cur->n)
        part.n = cur->n;
    dsp::Copy(part, *g);
    dsp::Gain(*g, db);
    dsp::Mix2(*g, *cur, *o, cur->n);
    Give(g);
    Give(cur);
    cur = o;
}

// audio.stutter(fname, fname2, tempo, count, 1/16, gain, gainpitch)
void Engine::Stutter(Buf*& cur, int count, int gain, int gainpitch)
{
    const double bs = 60.0 / tempo_ / 4.0; // the `beat` global is nil -> 1/16

    // audio.quantize(fname, foo1, tempo, 1/16, 0)
    Buf*         foo1  = Take();
    const double dur   = Len6(*cur);
    const double beats = dur / bs;
    const double ideal = LuaRound(beats);
    if(ideal * bs + 0 > beats * bs + 0)
        dsp::Stretch(*cur, *foo1, ideal / beats * 1.05);
    else
        dsp::Copy(*cur, *foo1);
    dsp::Trim(*foo1, SecToFrames(ideal * bs));
    Give(cur);

    const double   rep_sec = bs + 0.005;
    const uint32_t rep_len = SecToFrames(rep_sec);
    const uint32_t ov      = dsp::SpliceOverlap(0);
    const uint32_t search  = dsp::SpliceSearch(0);
    Buf*           acc     = Take();
    Buf*           g       = Take();
    Buf*           rep     = Take();
    Buf*           tmp     = Take();

    dsp::Copy(*foo1, *acc);
    dsp::Gain(*acc, gain > 0 ? -2.0 * count : -2.0);
    dsp::Trim(*acc, rep_len);

    double pitch_amt = 0;
    for(int i = 2; i <= count; i++)
    {
        double ga = gain > 0 ? -2.0 * (count - i) : (gain < 0 ? -2.0 * i : -2.0);
        pitch_amt += 100.0 * gainpitch;
        dsp::Copy(*foo1, *g);
        dsp::Gain(*g, ga);
        dsp::Pitch(*g, *rep, *tmp, pitch_amt, rep_sec);
        dsp::SpliceInto(*acc, *rep, ov, search);
    }
    Give(foo1);
    Give(g);
    Give(rep);
    Give(tmp);
    cur = acc;
}

// ---------------------------------------------------------------- generate
// Port of Beat:generate() from dnb.lua (makebreakbeat by @infinitedigits).
GenResult Engine::Generate(const Params& p, int16_t* out, uint32_t out_cap_frames,
                           const Hooks* hooks)
{
    GenResult res;
    if(!ready_ || analyzer_.num_slices() == 0 || !out || out_cap_frames == 0)
        return res;
    for(int i = 0; i < kWork; i++)
        busy_[i] = false;

    const double T     = tempo_;
    const double new_t = p.out_tempo > 0 ? p.out_tempo : T;
    int          beats = p.beats < 1 ? 1 : p.beats;
    const int    maxb  = MaxBeats(new_t, out_cap_frames);
    if(beats > maxb)
        beats = maxb;
    res.beats        = beats;
    res.source_tempo = T;
    res.out_tempo    = new_t;

    rng_.Seed(p.seed);
    cache_.Clear();
    nonce_ = 0;

    // The LFO tables are drawn in this order. math.lfo() ignores its time
    // argument, so each LFO is a per-generation constant in [0, 1].
    rng_.Range(32, 64); // global LFO period (global_lfo is never enabled)
    double phase[7];
    for(double& ph : phase)
        ph = rng_.Range(1, 100);
    auto lfo = [&](double ph) {
        return (std::sin(2 * 3.14159265 / (beats / 2.0) + ph) + 1) / 2;
    };
    const double lfo_reverse = lfo(phase[0]);
    const double lfo_stutter = lfo(phase[1]);
    const double lfo_pitch   = lfo(phase[2]);
    const double lfo_trunc   = lfo(phase[3]);
    // phase[4] belongs to the (unused) deviation LFO
    const double lfo_half    = lfo(phase[5]);
    const double lfo_stretch = lfo(phase[6]);
    const double p_global    = 1;

    const uint32_t n_slices     = analyzer_.num_slices();
    const Slice*   slices       = analyzer_.slices();
    // Chops in use. With every chop selected this is the identity, so the
    // random sequence (and the result) is exactly dnb.lua's.
    uint32_t n_use = 0, n_k = 0, n_s = 0;
    for(uint32_t k = 0; k < n_slices; k++)
        if(!p.use || p.use[k])
            order_[n_use++] = static_cast<uint16_t>(k);
    const bool subset = n_use > 0 && n_use < n_slices;
    if(!subset)
        for(n_use = 0; n_use < n_slices; n_use++)
            order_[n_use] = static_cast<uint16_t>(n_use);
    for(uint32_t k = 0; k < n_kicks_; k++)
        if(!subset || p.use[kicks_[k]])
            use_k_[n_k++] = kicks_[k];
    for(uint32_t k = 0; k < n_snares_; k++)
        if(!subset || p.use[snares_[k]])
            use_s_[n_s++] = snares_[k];
    // A selection can be made of short chops: they go on a 16th-note grid
    // (each one at least one step long), with more iterations and padding.
    const bool   custom   = subset || analyzer_.equal_chops();
    const int    max_iter = custom ? beats * 8 : beats * 3;
    const double   eighth       = 60.0 / T / 2.0;
    const double   grid         = custom ? eighth / 2 : eighth;
    const double   final_length = 60.0 / T * beats;
    joined_.n                   = 0;
    double current_beat         = 0;
    // start_beat: the chop that would be playing there (chops are laid out
    // back to back) and the position in eighths
    const double start8 = p.start_beat * 2;
    uint32_t     i0     = 0;
    if(p.start_beat > 0)
    {
        double total = 0;
        for(uint32_t k = 0; k < n_use; k++)
            total += slices[order_[k]].len;
        double pos = p.start_beat * 60 / T * kRate;
        pos -= std::floor(pos / total) * total;
        while(i0 + 1 < n_use && pos >= slices[order_[i0]].len)
            pos -= slices[order_[i0++]].len;
    }

    for(int i = 1; i <= max_iter; i++)
    {
        res.iterations = i;
        if(hooks && hooks->progress)
            hooks->progress(hooks->ctx, static_cast<float>(Len6(joined_) / final_length));
        if(hooks && hooks->stop && *hooks->stop)
            return res;

        uint32_t vi = order_[(i - 1 + i0) % n_use];
        if(rng_.Uniform() < p_global * p.p_deviation / 100)
            vi = order_[rng_.Range(1, n_use) - 1];
        const long rb = static_cast<long>(LuaRound(current_beat + start8));
        if(rb % 8 == 0 && rng_.Uniform() < p.p_kick / 100 && n_k > 0)
            vi = use_k_[rng_.Range(1, n_k) - 1];
        if(rb % 12 == 0 && rng_.Uniform() < p.p_snare / 100 && n_s > 0)
            vi = use_s_[rng_.Range(1, n_s) - 1];

        char trace[192];
        int  tl   = 0;
        auto note = [&](const char* fmt, int a, int b) {
            if(hooks && hooks->log && tl < static_cast<int>(sizeof(trace)) - 32)
                tl += std::snprintf(trace + tl, sizeof(trace) - tl, fmt, a, b);
        };
        note("i%d s%d", i, static_cast<int>(vi) + 1);

        const Slice&  sl  = slices[vi];
        const uint8_t cls = sl.cls; // onset_is_kick/snare[v_original]
        uint64_t      key = Fnv(kFnvBasis, vi);

        Buf* cur = Take();
        Buf  view;
        view.d = const_cast<float*>(src_.Frame(sl.start));
        view.n = sl.len;
        dsp::Copy(view, *cur);

        if(cls == kSliceKick && kick_.n)
        {
            MixDrum(cur, kick_, p.kick_db);
            key = Fnv(key, "kick");
            note(" kick", 0, 0);
        }
        if(cls == kSliceSnare && snare_.n)
        {
            MixDrum(cur, snare_, p.snare_db);
            key = Fnv(key, "snare");
            note(" snare", 0, 0);
        }

        if(rng_.Uniform() < p_global * p.p_pitch / 100 * lfo_pitch * 2)
        {
            key = Fnv(key, "pitch");
            int32_t m;
            if(!cache_.Find(key, m))
            {
                m = rng_.Range(1, 4);
                cache_.Put(key, m);
            }
            note(" pitch%d", 200 * m, 0);
            Buf* o = Take();
            Buf* t = Take();
            dsp::Pitch(*cur, *o, *t, 200.0 * m);
            Give(t);
            Give(cur);
            cur = o;
        }
        if(rng_.Uniform() < p_global * p.p_half / 100 * lfo_half * 2)
        {
            key    = Fnv(key, "half");
            note(" half", 0, 0);
            Buf* o = Take();
            dsp::Speed(*cur, *o, 0.5);
            Give(cur);
            cur = o;
        }
        if(rng_.Uniform() < p_global * p.p_reverse / 100 * lfo_reverse * 2 / 2)
        {
            key = Fnv(key, "reverse");
            note(" rev", 0, 0);
            dsp::Reverse(*cur);
        }
        if(rng_.Uniform() < p_global * p.p_stretch / 100 * lfo_stretch * 2 / 2)
        {
            key = Fnv(key, "stretch");
            int32_t m;
            if(!cache_.Find(key, m))
            {
                m = rng_.Range(1, 2);
                cache_.Put(key, m);
            }
            note(" stretch%d", 2 * m, 0);
            if(!StretchTo(cur, Len6(*cur) * 2 * m))
                cur->n = 0;
        }

        // Each branch draws its random number before testing the beat.
        const double p_stut = p_global * p.p_stutter / 100 * lfo_stutter * 2;
        auto gainpitch = [&]() { return rng_.Range(1, 10) < 8 ? 0 : rng_.Range(-1, 1); };
        int  count = 0, gain = 0, gp = 0;
        if(rng_.Uniform() < p_stut && rb % 8 == 2)
        {
            count = 12;
            gain  = 1;
            gp    = gainpitch();
        }
        else if(rng_.Uniform() < p_stut && rb % 8 == 4)
        {
            count = 8;
            gain  = rng_.Range(-1, 5);
            gp    = gainpitch();
        }
        else if(rng_.Uniform() < p_stut && rb % 8 == 6)
        {
            count = 4;
            gain  = rng_.Range(-2, 6);
            gp    = gainpitch();
        }
        else if(rng_.Uniform() < p_stut && rb % 8 == 7)
        {
            count = 2;
            gain  = rng_.Range(-3, 7);
            gp    = gainpitch();
        }
        if(count)
        {
            note(" stut%d/%d", count, gain);
            note("/%d", gp, 0);
            Stutter(cur, count, gain, gp);
            key = Fnv(Fnv(key, "stutter"), ++nonce_); // a fresh random file name
        }

        if(rng_.Uniform() < p_global * p.p_trunc / 100 * lfo_trunc * 2
           && Len6(*cur) > 60.0 / T / 4)
        {
            key    = Fnv(key, "trunc");
            note(" trunc", 0, 0);
            Buf* o = Take();
            dsp::SilentEnd(*cur, *o, 60.0 / T / 8);
            Give(cur);
            cur = o;
        }
        if(rng_.Uniform() < p_global * p.p_reverb / 100 && cls != kSliceNone)
        {
            const double vdur = Len6(*cur);
            key               = Fnv(key, "reverb");
            int32_t m;
            if(!cache_.Find(key, m))
            {
                m = rng_.Range(1, 3);
                cache_.Put(key, m);
            }
            note(" reverb%d", m, 0);
            dsp::PadEnd(*cur, SecToFrames(vdur * m / 2));
            reverb_.Process(*cur);
        }
        if(rng_.Uniform() < p_global * p.p_reverse / 100 * lfo_reverse * 2 / 2)
        {
            key = Fnv(key, "reverse");
            note(" rev", 0, 0);
            dsp::Reverse(*cur);
        }

        // Keep the loop on the eighth-note grid.
        const double lv   = Len6(*cur);
        const double base = i == 1 ? 0 : Len6(joined_);
        const double nb   = (base + lv) / grid;
        double       steps = LuaRound(nb);
        if(custom && steps < LuaRound(base / grid) + 1)
            steps = LuaRound(base / grid) + 1;
        const double diff = (steps - nb) * grid;
        if(i == 1)
        {
            if(StretchTo(cur, lv + diff + 0.01))
                dsp::Copy(*cur, joined_);
        }
        else
        {
            if(StretchTo(cur, lv + diff + 0.01))
                dsp::SpliceInto(joined_, *cur, dsp::SpliceOverlap(0.005), dsp::SpliceSearch(0.001));
        }
        Give(cur);
        note(" -> %d", static_cast<int>(joined_.n), 0);
        if(hooks && hooks->log)
            hooks->log(hooks->ctx, trace);

        current_beat = Len6(joined_) / eighth;
        if(Len6(joined_) >= 60.0 / T * beats + 0.005 || joined_.n >= joined_.cap)
            break;
    }

    // `sox joined out trim 0 <final_length> highpass 80 contrast`
    dsp::Trim(joined_, SecToFrames(final_length));
    if(custom && joined_.n < SecToFrames(final_length))
        dsp::PadEnd(joined_, SecToFrames(final_length) - joined_.n);
    dsp::Filter(joined_, dsp::FilterType::HighPass, 80);
    dsp::Contrast(joined_);
    dsp::Clip(joined_);

    uint32_t frames;
    if(new_t != T)
    {
        // `speed <new/old>`
        const double factor = new_t / T;
        frames              = static_cast<uint32_t>(joined_.n / factor + 0.5);
        if(frames > out_cap_frames)
            frames = out_cap_frames;
        dsp::ResampleToI16(joined_, out, factor, frames);
    }
    else
    {
        frames = joined_.n < out_cap_frames ? joined_.n : out_cap_frames;
        for(uint32_t i = 0; i < frames * kChannels; i++)
            out[i] = dsp::ToI16(joined_.d[i]);
    }
    if(hooks && hooks->progress)
        hooks->progress(hooks->ctx, 1.0f);
    Log(hooks, "generate: done");
    res.frames = frames;
    res.ok     = frames > 0;
    return res;
}

} // namespace mbb

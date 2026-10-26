// makebreakbeat for the Daisy Seed3.
//
// Port of schollz/makebreakbeat (norns): the beat generator runs natively
// (src/engine, a C++ reimplementation of lib/dnb.lua and the sox/aubio
// commands it shells out to) and the player reproduces
// lib/Engine_Makebreakbeat.sc plus the lattice logic of makebreakbeat.lua.
//
// Controls (all on press, see README "Controls"):
//   D15  focus: loop the quarter of the loop that is playing (seamlessly);
//        press again to play the whole loop again, with any new version in it
//   D16  in focus: make a new version of that quarter (the pots set how)
//   D17  new beat from the same break (not in focus)
//   D18  next break in /MBB/SAMPLES and a new beat from it (not in focus)
//   D19..D22 pots, for D16: shuffle, stutter, pitch, space (fully left = config)
//   D23, D24, D25, D28 LEDs: quarter playing, or in focus (blinks while D16 works)
//
// SD card: /MBB/config.txt, /MBB/SAMPLES/*.wav, /MBB/kick.wav,
// /MBB/snare.wav, generated beats are written to /MBB/OUT/<bpm>_<n>.wav.

#include "daisy_seed.h"
#include "ff_gen_drv.h"
#include "util/sd_diskio.h"
#include "util/bsp_sd_diskio.h"
#include "usbd_def.h"
#include "util/scopedirqblocker.h"

#include "config.h"
#include "engine.h"
#include "player.h"
#include "wav.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

extern SD_HandleTypeDef hsd1; // libDaisy's SD handle (per/sdmmc.cpp)

using namespace daisy;

extern "C" USBD_HandleTypeDef hUsbDeviceFS;

// libDaisy's System::Init links in the USB host and UART drivers through
// these two symbols. Neither is used here, so stand-ins keep ~5 KB of drivers
// out of the 128 KB flash (the host handle stays empty, so its interrupt
// handler is never called).
extern "C"
{
    HCD_HandleTypeDef hhcd_USB_OTG_HS;
    void              HAL_HCD_IRQHandler(HCD_HandleTypeDef*) {}
    void              dsy_uart_global_init() {}
}

namespace
{
// ------------------------------------------------------------------ memory
// SDRAM (64 MB): engine arena + two int16 stereo play buffers.
constexpr size_t   kArenaBytes = 49u * 1000u * 1000u; // Engine::RequiredBytes() ~= 48.96 MB
constexpr uint32_t kPlayFrames = 2160000;             // 45 s per play buffer

DSY_SDRAM_BSS alignas(32) uint8_t g_arena[kArenaBytes];
DSY_SDRAM_BSS alignas(32) int16_t g_play_mem[2][kPlayFrames * 2];

// AXI SRAM: everything the SD card DMA touches (IDMA cannot reach DTCM,
// where the stack lives).
alignas(32) uint8_t g_bounce[32768];
FIL                 g_rfile, g_wfile;
DIR                 g_dir;
FILINFO             g_fno;
char                g_cfg_text[8192];

DaisySeed      hw;
SdmmcHandler   sdmmc;
FATFS          g_fs; // FatFs work area (sector window is DMA'd: keep in SRAM)
char           g_sd_path[4];
mbb::Engine    engine;
mbb::Settings  settings;
Switch         buttons[4];

// ------------------------------------------------------------ USB log
// Minimal USB-serial logger (libDaisy's Logger also links its UART backend,
// which does not fit in flash). Never blocks when no terminal is reading.
char g_log_buf[2][160];
int  g_log_k       = 0;
bool g_log_reading = true;

void Log(const char* fmt, ...)
{
    if(hUsbDeviceFS.dev_state != USBD_STATE_CONFIGURED)
        return;
    char*   b = g_log_buf[g_log_k];
    va_list va;
    va_start(va, fmt);
    int n = std::vsnprintf(b, sizeof(g_log_buf[0]) - 2, fmt, va);
    va_end(va);
    if(n < 0)
        return;
    if(n > static_cast<int>(sizeof(g_log_buf[0])) - 3)
        n = sizeof(g_log_buf[0]) - 3;
    b[n++] = '\r';
    b[n++] = '\n';
    // Ping-pong buffers: a buffer is only reused after the next transfer
    // was accepted, i.e. after its own transfer completed.
    const uint32_t t0 = System::GetNow();
    while(hw.usb_handle.TransmitInternal(reinterpret_cast<uint8_t*>(b), n)
          != UsbHandle::Result::OK)
    {
        if(!g_log_reading || System::GetNow() - t0 > 20)
        {
            g_log_reading = false;
            return;
        }
    }
    g_log_reading = true;
    g_log_k ^= 1;
}

constexpr const char* kRoot    = "/MBB";
constexpr const char* kSamples = "/MBB/SAMPLES";
constexpr const char* kOut     = "/MBB/OUT";
constexpr const char* kConfig  = "/MBB/config.txt";

// -------------------------------------------------------------- FatFs I/O
// Reads go through an aligned window so FatFs only ever DMAs whole sectors
// into the start of g_bounce (32-byte aligned, cache maintenance safe).
class SdReader : public mbb::Reader
{
  public:
    bool Open(const char* path)
    {
        Close();
        res_     = f_open(&g_rfile, path, FA_READ);
        ok_      = res_ == FR_OK;
        pos_     = 0;
        win_pos_ = 0;
        win_len_ = 0;
        return ok_;
    }
    void Close()
    {
        if(ok_)
            f_close(&g_rfile);
        ok_ = false;
    }
    ~SdReader() override { Close(); }
    uint32_t Size() const { return ok_ ? f_size(&g_rfile) : 0; }
    // First FatFs error from Open or a read (FR_OK = none). The WAV loader
    // accepts a short read, so check this to catch a file cut off by the card.
    FRESULT error() const { return res_; }

    uint32_t Read(void* dst, uint32_t bytes) override
    {
        uint8_t* d     = static_cast<uint8_t*>(dst);
        uint32_t total = 0;
        while(ok_ && bytes)
        {
            if(pos_ < win_pos_ || pos_ >= win_pos_ + win_len_)
            {
                if(!Fill() || pos_ >= win_pos_ + win_len_)
                    break;
            }
            const uint32_t off = pos_ - win_pos_;
            uint32_t       n   = win_len_ - off;
            if(n > bytes)
                n = bytes;
            std::memcpy(d, g_bounce + off, n);
            d += n;
            pos_ += n;
            total += n;
            bytes -= n;
        }
        return total;
    }
    bool Seek(uint32_t pos) override
    {
        pos_ = pos;
        return ok_ && pos <= f_size(&g_rfile);
    }

  private:
    bool Fill()
    {
        win_pos_ = pos_ & ~511u;
        win_len_ = 0;
        UINT got = 0;
        res_     = f_lseek(&g_rfile, win_pos_);
        if(res_ == FR_OK)
            res_ = f_read(&g_rfile, g_bounce, sizeof(g_bounce), &got);
        if(res_ != FR_OK)
            return false;
        win_len_ = got;
        return true;
    }
    FRESULT  res_     = FR_OK;
    bool     ok_      = false;
    uint32_t pos_     = 0;
    uint32_t win_pos_ = 0;
    uint32_t win_len_ = 0;
};

// Writes are collected in g_bounce and flushed at sector-aligned file
// positions for the same reason.
class SdWriter : public mbb::Writer
{
  public:
    bool Open(const char* path, BYTE mode = FA_CREATE_ALWAYS | FA_WRITE)
    {
        res_ = f_open(&g_wfile, path, mode);
        ok_  = res_ == FR_OK;
        len_ = 0;
        err_ = !ok_;
        return ok_;
    }
    // Returns false if anything failed since Open(). After a failed write the
    // file is not closed: that would retry the writes, and each retry can
    // block for 30 s in the SD driver. The caller remounts instead.
    bool Close()
    {
        if(!ok_)
            return false;
        Flush();
        ok_ = false;
        if(!err_)
            Check(f_close(&g_wfile));
        return !err_;
    }
    int error() const { return res_; }
    ~SdWriter() override { Close(); }

    bool Write(const void* src, uint32_t bytes) override
    {
        const uint8_t* s = static_cast<const uint8_t*>(src);
        while(ok_ && !err_ && bytes)
        {
            uint32_t n = sizeof(g_bounce) - len_;
            if(n > bytes)
                n = bytes;
            std::memcpy(g_bounce + len_, s, n);
            len_ += n;
            s += n;
            bytes -= n;
            if(len_ == sizeof(g_bounce))
                Flush();
        }
        return ok_ && !err_;
    }
    bool Seek(uint32_t pos) override
    {
        Flush();
        if(!err_)
            Check(f_lseek(&g_wfile, pos));
        return !err_;
    }

  private:
    void Check(FRESULT r)
    {
        if(r != FR_OK)
        {
            res_ = r;
            err_ = true;
        }
    }
    bool Put(const uint8_t* p, uint32_t n)
    {
        UINT put = 0;
        Check(f_write(&g_wfile, p, n, &put));
        if(!err_ && put != n)
            Check(FR_DENIED); // card full
        return !err_;
    }
    void Flush()
    {
        if(!ok_ || err_ || len_ == 0)
            return;
        // Complete a partial sector first so the bulk write starts aligned.
        const uint32_t head = (512u - (f_tell(&g_wfile) & 511u)) & 511u;
        if(head && len_ > head)
        {
            if(!Put(g_bounce, head))
                return;
            std::memmove(g_bounce, g_bounce + head, len_ - head);
            len_ -= head;
        }
        Put(g_bounce, len_);
        len_ = 0;
    }
    bool     ok_  = false;
    bool     err_ = false;
    FRESULT  res_ = FR_OK;
    uint32_t len_ = 0;
};

// ------------------------------------------------------------ audio player
mbb::Player g_player; // src/engine/player.h: Engine_Makebreakbeat.sc + lattice

enum class UiState : uint8_t
{
    Boot,
    Idle,
    Busy,
    Error
};

volatile UiState g_ui = UiState::Boot;
GPIO             g_qled[4];       // D23, D24, D25, D28: quarter being played
constexpr bool   kQledLow = true; // LEDs wired 3V3 -> LED -> resistor -> pin: low = lit
constexpr bool   kPots    = true; // false: pots not read (D16 uses config.txt as is)
volatile bool    g_pots_ready = false;
float            g_pot_s[4]   = {}; // smoothed pots, 0..1

// Button presses, in order (the audio interrupt queues them, main() runs
// them; generating blocks main()). Low bits: button, high bits: the quarter
// playing at the press.
enum Ev : uint8_t
{
    kFocus, // D15
    kRegen, // D16
    kNew,   // D17
    kNext   // D18
};
volatile uint8_t g_q[16];
volatile uint8_t g_q_w = 0, g_q_r = 0;
volatile int8_t  g_focus_q = -1;    // quarter in focus
volatile bool    g_regen   = false; // a new version of it is being made

void PollButtons()
{
    for(int i = 0; i < 4; i++)
    {
        buttons[i].Debounce();
        if(buttons[i].RisingEdge() && static_cast<uint8_t>(g_q_w - g_q_r) < sizeof(g_q))
        {
            const int q     = std::max(0, g_player.playing_quarter());
            g_q[g_q_w & 15] = static_cast<uint8_t>(i | q << 4);
            g_q_w           = g_q_w + 1;
        }
    }
}

// True if a press of a button in `mask` (bit = Ev) is waiting.
bool Pending(unsigned mask)
{
    for(uint8_t i = g_q_r; i != g_q_w; i++)
        if((mask >> (g_q[i & 15] & 3)) & 1)
            return true;
    return false;
}

void UpdateLed()
{
    const uint32_t now = System::GetNow();
    bool           on;
    switch(g_ui)
    {
        case UiState::Error: on = (now / 60) & 1; break;
        case UiState::Boot:
        case UiState::Busy: on = (now / 150) & 1; break;
        default: // flash on every beat, longer on the "one"
            on = g_player.active() >= 0
                 && g_player.since_tick() < (g_player.last_tick_restarted() ? 7200u : 1920u);
    }
    hw.SetLed(on);

    const int fq = g_focus_q;
    const int q  = fq >= 0 ? fq : g_player.playing_quarter();
    for(int i = 0; i < 4; i++)
    {
        const bool lit = i == q && !(fq >= 0 && g_regen && (now / 100) & 1);
        g_qled[i].Write(lit != kQledLow);
    }
}

void AudioCallback(AudioHandle::InputBuffer, AudioHandle::OutputBuffer out, size_t size)
{
    PollButtons();
    if(g_pots_ready)
        for(int f = 0; f < 4; f++)
            g_pot_s[f] += 0.05f * (hw.adc.GetFloat(f) - g_pot_s[f]); // ~20 ms
    g_player.Process(out[0], out[1], size, 1.f);
    UpdateLed();
}

// ------------------------------------------------------------- SD helpers
constexpr int kMaxSamples = 64;
constexpr int kNameLen    = 64;
char          g_samples[kMaxSamples][kNameLen];
int           g_n_samples  = 0;
int           g_cur_sample = -1;
bool          g_break_ok   = false;

struct OutFile
{
    uint16_t tempo, n;
};
constexpr int kMaxOuts = 512;
OutFile       g_outs[kMaxOuts];
int           g_n_outs = 0;
uint32_t      g_gen_count = 0;

bool HasWavExt(const char* name)
{
    const size_t n = std::strlen(name);
    return n > 4 && strcasecmp(name + n - 4, ".wav") == 0;
}

// FatFs is linked straight to libDaisy's SD driver: FatFSInterface would
// also pull in the USB host stack, which does not fit in 128 KB of flash.
void InitSd()
{
    SdmmcHandler::Config sd_cfg;
    sd_cfg.Defaults();
    sd_cfg.speed = SdmmcHandler::Speed::MEDIUM_SLOW;
    sd_cfg.width = SdmmcHandler::BusWidth::BITS_4;
    sdmmc.Init(sd_cfg);
    FATFS_LinkDriver(&SD_Driver, g_sd_path);
}

// The first mount starts the card (once, through FatFs). reinit restarts the
// controller and card first, e.g. after an error: FatFs never re-initialises
// a drive by itself. Starting a working card more than once breaks reads.
bool MountSd(bool reinit)
{
    if(reinit)
    {
        f_mount(nullptr, g_sd_path, 0);
        HAL_SD_DeInit(&hsd1);
        BSP_SD_Init();
    }
    const FRESULT r = f_mount(&g_fs, g_sd_path, 1);
    if(r != FR_OK)
        Log("SD mount failed (%d) - is a FAT32 card inserted?", r);
    return r == FR_OK;
}

// A failed write can leave the card mid-transfer and FatFs with sectors it
// cannot write back (which would also break reading): start over, and stop
// saving so the buttons are never held up by the card again.
void WriteFailed(const char* path, int err)
{
    Log("cannot save %s (FatFs error %d, SD error 0x%lx) - saving off until restart", path, err,
        static_cast<unsigned long>(hsd1.ErrorCode));
    settings.save = false;
    if(!MountSd(true))
        Log("SD card stopped answering - unplug the Seed to reset it");
}

// Returns true if r had no FatFs error. A missing file is left to the caller;
// any other error is logged and the card restarted for the next attempt.
bool ReadOk(const char* path, SdReader& r)
{
    const FRESULT e = r.error();
    if(e == FR_OK)
        return true;
    if(e != FR_NO_FILE)
    {
        Log("SD error reading %s (FatFs error %d, SD error 0x%lx)", path, e,
            static_cast<unsigned long>(hsd1.ErrorCode));
        r.Close();
        if(!MountSd(true))
            Log("SD card stopped answering - unplug the Seed to reset it");
    }
    return false;
}

void LoadConfig()
{
    f_mkdir(kRoot);
    f_mkdir(kSamples);
    f_mkdir(kOut);

    SdReader rd;
    if(!rd.Open(kConfig))
    {
        const uint32_t n = mbb::DefaultConfig(g_cfg_text, sizeof(g_cfg_text));
        SdWriter       w;
        if(w.Open(kConfig, FA_CREATE_NEW | FA_WRITE) && w.Write(g_cfg_text, n) && w.Close())
            Log("wrote default %s", kConfig);
        else
            WriteFailed(kConfig, w.error());
        if(!rd.Open(kConfig))
            return; // defaults stay in effect
    }
    uint32_t len = rd.Size();
    if(len >= sizeof(g_cfg_text))
        len = sizeof(g_cfg_text) - 1;
    len               = rd.Read(g_cfg_text, len);
    g_cfg_text[len]   = 0;
    int       bad_ln  = 0;
    const int bad     = mbb::ParseConfig(g_cfg_text, len, settings, &bad_ln);
    if(bad)
        Log("config: %d unknown line(s), first at line %d", bad, bad_ln);
    Log("config: beats %d tempo %d seed %u", settings.beats, static_cast<int>(settings.tempo),
        static_cast<unsigned>(settings.seed));
}

void ScanSamples()
{
    if(f_opendir(&g_dir, kSamples) != FR_OK)
        return; // keep the old list (card not readable)
    g_n_samples = 0;
    while(g_n_samples < kMaxSamples && f_readdir(&g_dir, &g_fno) == FR_OK && g_fno.fname[0])
    {
        const char* n = g_fno.fname;
        // Skip folders, hidden files and macOS "._name.wav" resource forks.
        if((g_fno.fattrib & (AM_DIR | AM_HID | AM_SYS)) || n[0] == '.' || !HasWavExt(n)
           || std::strlen(n) >= kNameLen)
            continue;
        std::strcpy(g_samples[g_n_samples++], n);
    }
    f_closedir(&g_dir);
    for(int i = 1; i < g_n_samples; i++) // insertion sort, case-insensitive
        for(int j = i; j > 0 && strcasecmp(g_samples[j - 1], g_samples[j]) > 0; j--)
        {
            char t[kNameLen];
            std::strcpy(t, g_samples[j]);
            std::strcpy(g_samples[j], g_samples[j - 1]);
            std::strcpy(g_samples[j - 1], t);
        }
}

bool ParseOutName(const char* name, OutFile& o)
{
    // "<tempo>_<n>.wav"
    auto num = [&](uint32_t& v) {
        const char* s = name;
        v             = 0;
        while(*name >= '0' && *name <= '9' && v < 100000)
            v = v * 10 + static_cast<uint32_t>(*name++ - '0');
        return name != s;
    };
    uint32_t t = 0, n = 0;
    if(!num(t) || *name++ != '_' || !num(n) || strcasecmp(name, ".wav") != 0 || t == 0
       || t > 999 || n == 0 || n > 65535)
        return false;
    o.tempo = static_cast<uint16_t>(t);
    o.n     = static_cast<uint16_t>(n);
    return true;
}

bool OutLess(const OutFile& a, const OutFile& b)
{
    return a.tempo != b.tempo ? a.tempo < b.tempo : a.n < b.n;
}

void ScanOutputs()
{
    g_n_outs = 0;
    if(f_opendir(&g_dir, kOut) != FR_OK)
        return;
    while(g_n_outs < kMaxOuts && f_readdir(&g_dir, &g_fno) == FR_OK && g_fno.fname[0])
    {
        OutFile o;
        if(!(g_fno.fattrib & AM_DIR) && g_fno.fname[0] != '.' && ParseOutName(g_fno.fname, o))
            g_outs[g_n_outs++] = o;
    }
    f_closedir(&g_dir);
    for(int i = 1; i < g_n_outs; i++)
        for(int j = i; j > 0 && OutLess(g_outs[j], g_outs[j - 1]); j--)
        {
            const OutFile t = g_outs[j];
            g_outs[j]       = g_outs[j - 1];
            g_outs[j - 1]   = t;
        }
}

// ---------------------------------------------------------------- actions
void LogHook(void*, const char* msg)
{
    Log("  %s", msg);
}

// A generation gives up when one of these buttons is pressed (bit = Ev).
volatile bool g_stop    = false;
unsigned      g_stop_on = 0;

void ProgressHook(void* ctx, float f)
{
    g_stop = Pending(g_stop_on);
    if(!ctx)
        return;
    int&      last = *static_cast<int*>(ctx);
    const int pct  = static_cast<int>(f * 100);
    if(pct >= last + 25 || (pct == 100 && last != 100))
    {
        last = pct;
        Log("  progress %d%%", pct);
    }
}

void LoadDrums()
{
    for(int k = 0; k < 2; k++)
    {
        const char* name = k == 0 ? "kick" : "snare";
        char        path[24];
        std::snprintf(path, sizeof(path), "%s/%s.wav", kRoot, name);
        SdReader   r;
        const bool ok = r.Open(path)
                        && engine.LoadDrum(r, k == 0, settings.legacy_rate,
                                           settings.legacy_channels)
                               .ok;
        if(ReadOk(path, r) && ok)
            Log("%s.wav loaded", name);
        else
            Log("no %s (%s mixing disabled)", path, name);
    }
}

bool LoadBreak(int idx)
{
    if(g_n_samples == 0)
    {
        Log("no breaks in %s", kSamples);
        return false;
    }
    idx = ((idx % g_n_samples) + g_n_samples) % g_n_samples;
    char path[96];
    std::snprintf(path, sizeof(path), "%s/%s", kSamples, g_samples[idx]);
    Log("loading %s", path);
    const uint32_t t0 = System::GetNow();
    SdReader       r;
    if(!r.Open(path))
    {
        ReadOk(path, r);
        Log("cannot open %s (FatFs error %d)", path, r.error()); // the current break stays loaded
        return false;
    }
    g_break_ok   = false;
    g_cur_sample = idx;
    const mbb::LoadResult lr = engine.LoadSource(r, settings.legacy_rate);
    if(!ReadOk(path, r))
        return false;
    r.Close();
    if(!lr.ok)
    {
        Log("not a supported WAV: %s", path);
        return false;
    }
    if(lr.truncated)
        Log("break longer than 30 s, using the first 30 s");
    // Drums are resampled/re-labelled relative to the break, so reload them.
    LoadDrums();
    if(!engine.Analyse(settings.input_tempo, settings.chop))
    {
        Log("analysis failed");
        return false;
    }
    const mbb::Analyzer& a = engine.analyzer();
    Log("%s: %d bpm (x10 %d), %u onsets, %u slices%s, %u ms", g_samples[idx],
                 static_cast<int>(engine.tempo()),
                 static_cast<int>(engine.tempo_exact() * 10 + 0.5),
                 static_cast<unsigned>(a.num_onsets()), static_cast<unsigned>(a.num_slices()),
                 a.used_grid_fallback() ? " (grid)" : "",
                 static_cast<unsigned>(System::GetNow() - t0));
    g_break_ok = true;
    return true;
}

bool SaveBeat(const mbb::PlayBuf& b)
{
    ScanOutputs();
    // makebreakbeat.lua: first free <tempo>_<i>.wav
    uint16_t n = 1;
    for(int i = 0; i < g_n_outs; i++)
        if(g_outs[i].tempo == b.tempo && g_outs[i].n == n)
            n++;
    char path[64];
    // (the scan stops at kMaxOuts files, so make sure nothing is overwritten)
    do
        std::snprintf(path, sizeof(path), "%s/%d_%u.wav", kOut, b.tempo, static_cast<unsigned>(n));
    while(f_stat(path, &g_fno) == FR_OK && ++n != 0);
    const uint32_t t0 = System::GetNow();
    SdWriter       w;
    if(!w.Open(path) || !mbb::WriteWav16(w, b.data, b.frames) || !w.Close())
    {
        WriteFailed(path, w.error());
        return false;
    }
    Log("saved %s (%u ms)", path, static_cast<unsigned>(System::GetNow() - t0));
    return true;
}

// The player's buffer hand-over must not race the audio interrupt.
int ClaimFreeBuffer()
{
    ScopedIrqBlocker blk;
    return g_player.ClaimFree();
}

void WaitSwap()
{
    while(g_player.pending() >= 0)
        System::Delay(1);
    System::Delay(3); // and its 2 ms crossfade
}

// Pot f, 0..1 with a small dead zone at both ends.
float Pot(int f)
{
    if(!g_pots_ready)
        return 0.f; // config.txt as it is
    return std::min(1.f, std::max(0.f, (g_pot_s[f] - 0.02f) / 0.96f));
}

// Generator settings: config (with `pots`: scaled by the pots) and seed.
mbb::Params GenParams(bool pots)
{
    mbb::Params p = settings.gen;
    if(pots)
    {
        const float x[4] = {Pot(0), Pot(1), Pot(2), Pot(3)};
        mbb::ApplyPots(p, x); // see config.h
    }
    if(settings.seed)
        p.seed = static_cast<uint64_t>(settings.seed) + g_gen_count;
    else
        p.seed = (static_cast<uint64_t>(Random::GetValue()) << 32) ^ System::GetUs();
    g_gen_count++;
    return p;
}

// Focus mode: prints the pots (0..100) and the probabilities they give D16,
// on entering (`now`) and whenever a pot moves by 2 or more.
void ShowPots(bool now)
{
    static int      shown[4] = {-100, -100, -100, -100};
    static uint32_t last     = 0;
    const uint32_t  t        = System::GetNow();
    if(!g_pots_ready || (!now && t - last < 150))
        return;
    float x[4];
    int   v[4];
    bool  moved = now;
    for(int f = 0; f < 4; f++)
    {
        x[f]  = Pot(f);
        v[f]  = static_cast<int>(x[f] * 100 + 0.5f);
        moved = moved || std::abs(v[f] - shown[f]) >= 2;
    }
    if(!moved)
        return;
    std::copy(v, v + 4, shown);
    last          = t;
    mbb::Params p = settings.gen;
    mbb::ApplyPots(p, x);
    auto r = [](double d) { return static_cast<int>(d + 0.5); };
    Log("pots D19 %d D20 %d D21 %d D22 %d: dev %d kick %d snare %d, stutter %d trunc %d, "
        "reverse %d pitch %d half %d, reverb %d stretch %d",
        v[0], v[1], v[2], v[3], r(p.p_deviation), r(p.p_kick), r(p.p_snare), r(p.p_stutter),
        r(p.p_trunc), r(p.p_reverse), r(p.p_pitch), r(p.p_half), r(p.p_reverb), r(p.p_stretch));
}

// D17: a new loop from the break loaded, played from the next beat.
bool Generate()
{
    if(!g_break_ok)
    {
        Log("no break loaded");
        return false;
    }
    g_ui = UiState::Busy;
    mbb::Params p = GenParams(false);
    // makebreakbeat.lua passes floor(clock tempo) and break_beats + 1.
    p.beats     = std::max(1, settings.beats) + 1;
    p.out_tempo = settings.tempo > 0 ? std::floor(settings.tempo) : 0;

    const int      target = ClaimFreeBuffer();
    mbb::PlayBuf&  b      = g_player.buf[target];
    int16_t* const mem    = g_play_mem[target];
    Log("generating %d beats @ %d bpm from %s (seed %lu)", p.beats - 1,
        static_cast<int>(p.out_tempo > 0 ? p.out_tempo : engine.tempo()), g_samples[g_cur_sample],
        static_cast<unsigned long>(p.seed & 0xffffffffu));
    int        last = -100;
    mbb::Hooks h;
    h.log       = LogHook;
    h.progress  = ProgressHook;
    h.ctx       = &last;
    h.stop      = &g_stop;
    g_stop_on   = 1u << kNew | 1u << kNext; // pressed again: start over
    g_stop      = false;
    const uint32_t       t0 = System::GetNow();
    const mbb::GenResult r  = engine.Generate(p, mem, kPlayFrames, &h);
    g_ui                    = UiState::Idle;
    if(g_stop)
        return false;
    if(!r.ok)
    {
        Log("generation failed");
        return false;
    }
    b.data       = mem;
    b.frames     = r.frames;
    b.tempo      = static_cast<int>(std::floor(r.out_tempo));
    b.fpb        = 48000.0 * 60.0 / r.out_tempo;
    b.loop_beats = r.beats > 1 ? r.beats - 1 : 1;
    Log("generated %d beats (%lu frames, %d iterations) in %lu ms", b.loop_beats,
        static_cast<unsigned long>(r.frames), r.iterations,
        static_cast<unsigned long>(System::GetNow() - t0));
    {
        ScopedIrqBlocker blk;
        g_player.Publish(target, true);
    }
    if(settings.save)
        SaveBeat(b);
    return true;
}

// D18: the next break (rescanned: the card may have new files), new loop.
void NextBreak(int steps)
{
    char cur[kNameLen] = "";
    if(g_cur_sample >= 0)
        std::strcpy(cur, g_samples[g_cur_sample]);
    ScanSamples();
    g_cur_sample = 0;
    for(int i = 0; i < g_n_samples; i++)
        if(strcasecmp(g_samples[i], cur) == 0)
            g_cur_sample = i;
    g_ui = UiState::Busy;
    if(!LoadBreak(g_cur_sample + steps))
    {
        g_ui = UiState::Error;
        return;
    }
    g_ui = UiState::Idle;
    Generate();
}

// Focus (D15): quarter q of the loop in player buffer `a` loops on its own
// (player buffers 2 and 3). New versions of it (D16) are made into two
// slots of the other loop buffer, taking turns, so the one playing is never
// overwritten. Leaving splices the version playing into the loop.
struct Focus
{
    mbb::PlayBuf full;
    int          a, q, Q, shown, made; // shown: slot playing, -1 = the quarter as it was
    uint32_t     s0, s1, slot;
} g_f;

// Quarter buffer: the quarter as it was (k < 0) or version slot k.
mbb::PlayBuf FocusBuf(int k, uint32_t frames)
{
    mbb::PlayBuf b = g_f.full;
    b.loop_beats   = g_f.Q;
    b.data         = k < 0 ? g_f.full.data + 2 * g_f.s0 : g_play_mem[1 - g_f.a] + 2 * k * g_f.slot;
    b.frames       = frames;
    return b;
}

void StartFocus(int q)
{
    WaitSwap();
    const int a = g_player.active();
    if(a < 0 || a > 1)
    {
        Log("no loop playing");
        return;
    }
    const mbb::PlayBuf& full = g_player.buf[a];
    if(full.loop_beats % 4)
    {
        Log("focus: %d beats do not split into quarters", full.loop_beats);
        return;
    }
    g_f.full  = full;
    g_f.a     = a;
    g_f.q     = q;
    g_f.Q     = full.loop_beats / 4;
    g_f.shown = -1;
    g_f.made  = 0;
    mbb::Player::Quarter(full, q, g_f.s0, g_f.s1);
    // a beat over for the loop's tail and the splice; 12.5 ms over MaxBeats' margin
    g_f.slot = static_cast<uint32_t>(std::ceil((g_f.Q + 1) * full.fpb)) + 600;
    const uint32_t n = std::min(static_cast<uint32_t>((g_f.Q + 1) * full.fpb), full.frames - g_f.s0);
    {
        ScopedIrqBlocker blk;
        // carry on seamlessly from the next beat if it is in quarter q, else
        // (pressed on its last beat) go back to the start of it
        const int nb    = (g_player.lattice() + 1) % full.loop_beats;
        g_player.buf[2] = FocusBuf(-1, n);
        g_player.Publish(2, false, 1, nb / g_f.Q == q ? nb - q * g_f.Q : 0);
        g_focus_q = static_cast<int8_t>(q);
    }
    Log("focus on quarter %d (%d beats)", q + 1, g_f.Q);
    ShowPots(true);
}

void EndFocus()
{
    WaitSwap();
    const int a = g_f.a;
    if(g_f.shown >= 0) // the loop buffer is not being read meanwhile
    {
        const mbb::PlayBuf& v = g_player.buf[g_player.active()];
        mbb::Player::Splice(g_play_mem[a], g_f.full.frames, v.data, g_f.s0, g_f.s1, v.frames);
    }
    {
        ScopedIrqBlocker blk;
        // carry on with the next beat of the loop (after the quarter's last
        // beat, the beat after the quarter)
        const int Q = g_f.Q, nb = (g_player.lattice() + 1) % Q;
        g_player.Publish(a, false, 1, nb ? g_f.q * Q + nb : (g_f.q + 1) * Q % (4 * Q));
    }
    WaitSwap();
    g_focus_q = -1;
    Log(g_f.shown >= 0 ? "back to the loop, with the new quarter %d" : "back to the loop",
        g_f.q + 1);
}

// D16: a new version of the quarter in focus, played from the next beat.
void Regen()
{
    WaitSwap(); // the slot about to be overwritten may still be playing
    const int k = g_f.shown == 0 ? 1 : 0;
    if(static_cast<uint64_t>(k + 1) * g_f.slot > kPlayFrames)
    {
        Log("quarter too long");
        return;
    }
    mbb::Params p = GenParams(true);
    p.beats       = g_f.Q + 1;
    p.start_beat  = g_f.q * g_f.Q;
    p.out_tempo   = 2880000.0 / g_f.full.fpb;
    if(std::fabs(p.out_tempo - engine.tempo()) < 1e-6)
        p.out_tempo = 0; // the break's own tempo: no resampling
    int pv[4];
    for(int f = 0; f < 4; f++)
        pv[f] = static_cast<int>(Pot(f) * 100 + 0.5f);
    mbb::Hooks h;
    h.progress = ProgressHook;
    h.stop     = &g_stop;
    g_stop_on  = 1u << kFocus; // leaving focus: give up
    g_stop     = false;
    g_regen    = true;
    const uint32_t       t0 = System::GetNow();
    const mbb::GenResult r  = engine.Generate(p, g_play_mem[1 - g_f.a] + 2 * k * g_f.slot, g_f.slot, &h);
    g_regen                 = false;
    if(g_stop)
        return;
    if(!r.ok || r.frames + 2 < (g_f.Q + 1) * g_f.full.fpb)
    {
        Log("quarter %d: generation failed", g_f.q + 1);
        return;
    }
    {
        ScopedIrqBlocker blk;
        const int b     = g_player.active() == 2 ? 3 : 2;
        g_player.buf[b] = FocusBuf(k, r.frames);
        g_player.Publish(b, false);
    }
    g_f.shown = k;
    Log("quarter %d version %d (pots %d %d %d %d) in %lu ms", g_f.q + 1, ++g_f.made, pv[0], pv[1],
        pv[2], pv[3], static_cast<unsigned long>(System::GetNow() - t0));
}

// One button press (`steps` presses of it in a row).
void Run(int ev, int q, int steps)
{
    const bool focus = g_focus_q >= 0;
    switch(ev)
    {
        case kFocus:
            if(focus)
                EndFocus();
            else
                StartFocus(q);
            break;
        case kRegen:
            if(focus)
                Regen();
            else
                Log("D16: press D15 to focus on a quarter first");
            break;
        default:
            if(focus)
                Log("D%d: press D15 to leave focus first", 15 + ev);
            else if(ev == kNew)
                Generate();
            else
                NextBreak(steps);
    }
}

[[noreturn]] void Fatal(const char* msg)
{
    g_ui = UiState::Error;
    while(true)
    {
        Log("%s", msg);
        System::Delay(2000);
    }
}

} // namespace

int main(void)
{
    hw.Init(true); // 480 MHz
    hw.usb_handle.Init(UsbHandle::FS_INTERNAL);

    const uint8_t pins[4] = {15, 16, 17, 18};
    for(int i = 0; i < 4; i++)
        buttons[i].Init(hw.GetPin(pins[i]), 1000.f);
    const Pin led_pins[4] = {seed::D23, seed::D24, seed::D25, seed::D28};
    for(int i = 0; i < 4; i++)
    {
        g_qled[i].Init(led_pins[i], GPIO::Mode::OUTPUT);
        g_qled[i].Write(kQledLow); // off
    }

    g_player.Init(48000.f);
    hw.SetAudioBlockSize(48);
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);
    hw.StartAudio(AudioCallback);

    System::Delay(500); // let the USB log attach
    Log("makebreakbeat for Daisy Seed%s",
                 hw.CheckBoardVersion() == DaisySeed::BoardVersion::DAISY_SEED_3 ? "3" : "");

    if(mbb::Engine::RequiredBytes() > kArenaBytes)
        Fatal("engine arena too small");
    if(!engine.Init(g_arena, kArenaBytes))
        Fatal("engine init failed");

    InitSd();
    for(bool retry = false; !MountSd(retry); retry = true)
    {
        g_ui = UiState::Error;
        System::Delay(2000);
    }
    g_ui = UiState::Boot;
    LoadConfig();

    if(kPots)
    {
        AdcChannelConfig pots[4];
        const Pin        pot_pins[4] = {seed::D19, seed::D20, seed::D21, seed::D22};
        for(int i = 0; i < 4; i++)
            pots[i].InitSingle(pot_pins[i], AdcChannelConfig::SPEED_810CYCLES_5);
        hw.adc.Init(pots, 4);
        hw.adc.Start();
        g_pots_ready = true;
    }

    // Find the first break: config "sample", else the first file.
    while(true)
    {
        ScanSamples();
        if(g_n_samples > 0)
            break;
        g_ui = UiState::Error;
        Log("put .wav breaks into %s", kSamples);
        System::Delay(2000);
    }
    int first = 0;
    for(int i = 0; i < g_n_samples; i++)
        if(settings.sample[0] && strcasecmp(g_samples[i], settings.sample) == 0)
            first = i;
    for(int tries = 0; tries < g_n_samples && !LoadBreak(first + tries); tries++) {}
    g_ui = g_break_ok ? UiState::Idle : UiState::Error;

    if(g_break_ok && settings.auto_generate)
        Generate();

    while(true)
    {
        if(g_q_r != g_q_w)
        {
            const uint8_t e = g_q[g_q_r & 15];
            g_q_r           = g_q_r + 1;
            // presses of D16..D18 made meanwhile count once (D18: steps)
            int steps = 1;
            while((e & 3) != kFocus && g_q_r != g_q_w && (g_q[g_q_r & 15] & 3) == (e & 3))
            {
                g_q_r = g_q_r + 1;
                steps++;
            }
            Run(e & 3, e >> 4, steps);
        }
        if(g_focus_q >= 0)
            ShowPots(false);
        System::Delay(1);
    }
}

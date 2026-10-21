// makebreakbeat for the Daisy Seed3.
//
// Port of schollz/makebreakbeat (norns): the beat generator runs natively
// (src/engine, a C++ reimplementation of lib/dnb.lua and the sox/aubio
// commands it shells out to) and the player reproduces
// lib/Engine_Makebreakbeat.sc plus the lattice logic of makebreakbeat.lua.
//
// Controls (same wiring as the test project), short / long (>= 0.6 s) press:
//   D15  generate a new beat (norns K2) / enter CHOP mode
//   D16  play / stop (norns K3)
//   D17  next break in /MBB/SAMPLES / enter LIVE mode
//   D18  previous / next saved beat in /MBB/OUT (norns E)
//   D23, D24, D25, D28 LEDs: lit while quarter 1..4 of the loop plays
//   D19..D22 pots (config use_pots = 1): tempo, beats, chaos, volume (off: kPots)
// CHOP mode (not in the original) picks the chops beats are built from:
//   D15  generate from the selected chops / leave without generating
//   D16  play the current chop (on press) / long: chop at hits, 8, 16, 32
//   D17  next / previous chop (and play it)
//   D18  current chop on-off / select all, kicks, snares, kicks+snares, other
// LIVE mode (not in the original) works on quarters of the loop:
//   D15..D18 tap: solo quarter 1..4 (it loops on its own from the next bar);
//            tap again: back to the whole loop with the version picked
//   D19..D22 pots: while quarter 1..4 is soloed, pick a version of it (fully
//            left = as it was; the others are made in RAM in the background
//            from the moment a loop plays, ~5-6 per quarter for 16 beats;
//            a pot only takes over once it is turned)
//   D15 long: generate a whole new beat          D16 long: play / stop
//   D17 long: leave LIVE                         D18 long: undo / redo
//            (while soloing: back to the loop as it was)
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

volatile UiState g_ui         = UiState::Boot;
GPIO             g_qled[4];         // D23, D24, D25, D28: quarter being played
constexpr bool   kQledLow = true;   // LEDs wired 3V3 -> LED -> resistor -> pin: low = lit
volatile bool    g_pots_ready = false;
// false: pots not read at all (use_pots is ignored, LIVE has no versions)
constexpr bool kPots = true;
// LIVE solo, see StartSolo(); set by main(), read by the audio interrupt
volatile int8_t  g_solo_q    = -1;    // quarter soloed
volatile uint8_t g_solo_n    = 1;     // versions on its pot (0 = as it was)
volatile uint8_t g_solo_req  = 0;     // version the pot asks for
volatile bool    g_solo_wait = false; // that version is still being made (LED blinks)
float            g_solo_hold = 0;     // pot when the solo started (pickup)
bool             g_solo_live = false;
// Versions of the quarters, see BankFit()
constexpr int kSlots = 28;
struct Bank
{
    bool     ok;
    double   fpb;
    int      beats, mem, n, epoch; // loop beats, buffer, slots, layout count
    uint32_t base, slot;
    int8_t   q[kSlots];     // quarter of each slot (-1: the next one soloed)
    uint8_t  state[kSlots]; // 0: to make, 1: ready, 2: failed
    uint32_t frames[kSlots];
} g_bank;

// Button events latched by the audio interrupt (generation blocks main()).
volatile int      g_ev_generate = 0, g_ev_sample = 0, g_ev_out = 0, g_ev_chop = 0, g_ev_undo = 0;
volatile uint8_t  g_ev_quarter  = 0; // LIVE: quarters tapped (bits)
volatile uint32_t g_ev_out_ms   = 0;
uint32_t          g_btn_down[4] = {};

// CHOP mode: button events go through a small queue (order matters).
enum ChopEv : uint8_t
{
    kAudition,
    kNext,
    kPrev,
    kToggle,
    kFilter,
    kGrid
};
volatile bool    g_chop_mode = false, g_chop_led = false;
volatile uint8_t g_cq[16];
volatile bool    g_live_mode   = false;
float            g_pot_hold[4] = {}; // LIVE: pots frozen for generate/volume
float            g_pot_s[4]    = {}; // smoothed pots
volatile uint8_t g_cq_w = 0, g_cq_r = 0;

void PushChop(uint8_t ev)
{
    if(static_cast<uint8_t>(g_cq_w - g_cq_r) < sizeof(g_cq))
    {
        g_cq[g_cq_w & 15] = ev;
        g_cq_w            = g_cq_w + 1;
    }
}

float Pot(int i)
{
    if(!g_pots_ready)
        return 0.5f;
    return g_live_mode ? g_pot_hold[i] : g_pot_s[i];
}

void PollButtons()
{
    const uint32_t now = System::GetNow();
    for(int i = 0; i < 4; i++)
    {
        buttons[i].Debounce();
        if(buttons[i].RisingEdge())
        {
            g_btn_down[i] = now;
            if(i == 1 && !g_live_mode)
            {
                if(g_chop_mode)
                    PushChop(kAudition);
                else
                    g_player.TogglePlay(); // runs in the audio interrupt already
            }
        }
        if(buttons[i].FallingEdge())
        {
            const bool longp = now - g_btn_down[i] >= 600;
            if(g_live_mode)
            {
                if(!longp)
                    g_ev_quarter = static_cast<uint8_t>(g_ev_quarter | (1 << i));
                else if(i == 0)
                    g_ev_generate = g_ev_generate + 1;
                else if(i == 1)
                    g_player.TogglePlay();
                else if(i == 3)
                    g_ev_undo = g_ev_undo + 1;
                else
                    g_live_mode = false;
            }
            else if(i == 2 && longp && !g_chop_mode)
            {
                for(int f = 0; f < 4; f++)
                    g_pot_hold[f] = Pot(f);
                g_live_mode = true;
            }
            else if(i == 0)
            {
                if(longp)
                    g_ev_chop = g_ev_chop + 1;
                else
                    g_ev_generate = g_ev_generate + 1;
            }
            else if(g_chop_mode)
            {
                if(i == 1 && longp)
                    PushChop(kGrid);
                else if(i == 2)
                    PushChop(longp ? kPrev : kNext);
                else if(i == 3)
                    PushChop(longp ? kFilter : kToggle);
            }
            else if(i == 2)
                g_ev_sample = g_ev_sample + 1;
            else if(i == 3)
            {
                g_ev_out    = g_ev_out + (longp ? 1 : -1);
                g_ev_out_ms = now;
            }
        }
    }
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
        default:
            if(g_chop_mode) // lit = chop on, short blip every second
                on = g_chop_led != ((now % 1000) < 80);
            else if(g_player.active() < 0)
                on = false;
            else if(g_player.playing()) // flash on every beat, longer on the "one"; inverted in LIVE
                on = g_live_mode
                     != (g_player.since_tick() < (g_player.last_tick_restarted() ? 7200u : 1920u));
            else
                on = true; // a beat is loaded and stopped
    }
    hw.SetLed(on);

    // Quarter LEDs: the quarter of the loop being played, while playing; in
    // a solo the soloed one (blinking while its version is being made)
    const int sq = g_solo_q;
    const int q  = sq >= 0 ? sq : g_player.playing() ? g_player.playing_quarter() : -1;
    for(int i = 0; i < 4; i++)
    {
        const bool lit = i == q && !(sq >= 0 && g_solo_wait && (now / 100) & 1);
        g_qled[i].Write(lit != kQledLow);
    }
}

void AudioCallback(AudioHandle::InputBuffer, AudioHandle::OutputBuffer out, size_t size)
{
    PollButtons();
    float vol = 1;
    if(g_pots_ready)
    {
        for(int f = 0; f < 4; f++)
        {
            g_pot_s[f] += 0.05f * (hw.adc.GetFloat(f) - g_pot_s[f]); // ~20 ms
            if(f != g_solo_q || !g_live_mode)
                continue;
            // LIVE solo: the pot picks one of n versions (with hysteresis),
            // once it has been turned (an unconnected input just drifts)
            const float p = g_pot_s[f], n = g_solo_n, z = g_solo_req;
            if(!g_solo_live)
            {
                if(std::fabs(p - g_solo_hold) <= 0.05f)
                    continue;
                g_solo_live = true;
            }
            else if(p > z / n - 0.03f && p < (z + 1) / n + 0.03f)
                continue;
            g_solo_req = static_cast<uint8_t>(std::min(n - 1, p * n));
        }
        const float v = Pot(3);
        if(settings.use_pots)
            vol = v * v;
    }
    g_player.Process(out[0], out[1], size, vol);
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
OutFile       g_cur_out{0, 0};
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
    if(!kPots)
        settings.use_pots = 0;
    Log("config: beats %d tempo %d seed %u pots %d", settings.beats,
                 static_cast<int>(settings.tempo), static_cast<unsigned>(settings.seed),
                 settings.use_pots ? 1 : 0);
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

void ProgressHook(void* ctx, float f)
{
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

// ------------------------------------------------------------- CHOP mode
uint8_t  g_use[mbb::Analyzer::kMaxSlices];
uint32_t g_cursor = 0, g_filter = 0, g_grid = 0;
bool     g_was_playing = false;

uint32_t CountUse()
{
    uint32_t c = 0;
    for(uint32_t k = 0; k < engine.analyzer().num_slices(); k++)
        c += g_use[k];
    return c;
}

void ResetChops()
{
    std::memset(g_use, 1, sizeof(g_use));
    g_cursor = g_filter = 0;
}

void ShowChop(bool play)
{
    const mbb::Slice& s = engine.analyzer().slices()[g_cursor];
    g_chop_led          = g_use[g_cursor];
    if(play)
    {
        ScopedIrqBlocker blk;
        g_player.Audition(engine.source().Frame(s.start), s.len);
    }
    static const char* const kCls[] = {"other", "KICK", "SNARE"};
    Log("chop %u/%u %s %u ms %s", static_cast<unsigned>(g_cursor + 1),
        static_cast<unsigned>(engine.analyzer().num_slices()), kCls[s.cls],
        static_cast<unsigned>(s.len / 48), g_use[g_cursor] ? "ON" : "off");
}

void LogChops()
{
    Log("%u chops (%s), %u on", static_cast<unsigned>(engine.analyzer().num_slices()),
        g_grid ? "equal" : "at hits", static_cast<unsigned>(CountUse()));
}

void ChopEvent(uint8_t ev)
{
    const uint32_t n = engine.analyzer().num_slices();
    switch(ev)
    {
        case kNext: g_cursor = (g_cursor + 1) % n; break;
        case kPrev: g_cursor = (g_cursor + n - 1) % n; break;
        case kToggle:
            if(g_use[g_cursor] && CountUse() == 1)
                Log("keep at least one chop");
            else
                g_use[g_cursor] ^= 1;
            ShowChop(false);
            return;
        case kFilter:
        {
            // all, kicks, kicks+snares, snares, other (bit = slice class)
            static const uint8_t kMasks[] = {7, 2, 6, 4, 1};
            static const char* const kNames[] = {"all", "kicks", "kicks+snares", "snares", "other"};
            for(int tries = 0; tries < 5; tries++)
            {
                g_filter = (g_filter + 1) % 5;
                for(uint32_t k = 0; k < n; k++)
                    g_use[k] = (kMasks[g_filter] >> engine.analyzer().slices()[k].cls) & 1;
                if(CountUse())
                    break;
            }
            Log("select %s", kNames[g_filter]);
            LogChops();
            ShowChop(false);
            return;
        }
        case kGrid:
        {
            static const uint8_t kGrids[] = {0, 8, 16, 32};
            uint32_t             gi       = 0;
            while(gi < 4 && kGrids[gi] != g_grid)
                gi++;
            g_grid = kGrids[(gi + 1) % 4];
            {
                ScopedIrqBlocker blk;
                g_player.StopAudition();
            }
            engine.Chop(g_grid);
            ResetChops();
            LogChops();
            break;
        }
        default: break;
    }
    ShowChop(true);
}

void EnterChop()
{
    if(!g_break_ok)
    {
        Log("no break loaded");
        return;
    }
    {
        ScopedIrqBlocker blk;
        g_was_playing = g_player.playing();
        if(g_was_playing)
            g_player.TogglePlay();
        g_cq_r      = g_cq_w; // drop stale events
        g_chop_mode = true;
    }
    Log("CHOP mode");
    LogChops();
    ShowChop(true);
}

void LeaveChop(bool resume)
{
    ScopedIrqBlocker blk;
    g_chop_mode = false;
    g_player.StopAudition();
    if(resume && g_was_playing && !g_player.playing())
        g_player.TogglePlay();
}

bool LoadBreak(int idx)
{
    {
        ScopedIrqBlocker blk;
        g_player.StopAudition(); // it reads the source buffer
    }
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
    ResetChops();
    if(!engine.Analyse(settings.input_tempo, g_grid))
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
    g_bank.ok  = false; // new versions from this break
    return true;
}

// The player's buffer hand-over must not race the audio interrupt.
int ClaimFreeBuffer()
{
    ScopedIrqBlocker blk;
    const int f            = g_player.ClaimFree();
    g_player.buf[f].frames = 0; // being overwritten: nothing to undo to
    g_bank.ok              = false;
    return f;
}

void Publish(int target)
{
    ScopedIrqBlocker blk;
    g_player.Publish(target, settings.auto_play);
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
    g_cur_out.tempo = static_cast<uint16_t>(b.tempo);
    g_cur_out.n     = n;
    Log("saved %s (%u ms)", path, static_cast<unsigned>(System::GetNow() - t0));
    return true;
}

// Generator settings for a whole beat or a LIVE quarter: config, chaos pot,
// chop selection and seed.
mbb::Params GenParams()
{
    mbb::Params p = settings.gen;
    if(settings.use_pots)
    {
        const double chaos = Pot(2) * 2; // 0..2 x the configured probabilities
        for(double* q : {&p.p_deviation, &p.p_reverse, &p.p_stutter, &p.p_pitch, &p.p_trunc,
                         &p.p_half, &p.p_reverb, &p.p_stretch})
            *q = std::min(100.0, *q * chaos);
    }
    const uint32_t n_on = CountUse(), n_all = engine.analyzer().num_slices();
    if(n_on < n_all)
    {
        p.use = g_use;
        Log("using %u/%u chops", static_cast<unsigned>(n_on), static_cast<unsigned>(n_all));
    }
    if(settings.seed)
        p.seed = static_cast<uint64_t>(settings.seed) + g_gen_count;
    else
        p.seed = (static_cast<uint64_t>(Random::GetValue()) << 32) ^ System::GetUs();
    g_gen_count++;
    return p;
}

bool Generate()
{
    if(!g_break_ok)
    {
        Log("no break loaded");
        return false;
    }
    g_ui = UiState::Busy;

    int    beats = settings.beats;
    double tempo = settings.tempo;
    if(settings.use_pots)
    {
        static const int kBeats[] = {4, 8, 12, 16, 24, 32, 48, 64};
        const float      pt       = Pot(0);
        tempo = pt < 0.02f ? 0 : std::floor(60 + (pt - 0.02f) / 0.98f * 140); // 0 = break tempo
        beats = kBeats[std::min(7, static_cast<int>(Pot(1) * 8))];
    }
    if(beats < 1)
        beats = 1;
    mbb::Params p = GenParams();
    // makebreakbeat.lua passes floor(clock tempo) and break_beats + 1.
    p.beats     = beats + 1;
    p.out_tempo = tempo > 0 ? std::floor(tempo) : 0;

    const int      target = ClaimFreeBuffer();
    mbb::PlayBuf&  b      = g_player.buf[target];
    int16_t* const mem    = g_play_mem[target];
    Log("generating %d beats @ %d bpm from %s (seed %lu)", beats,
                 static_cast<int>(p.out_tempo > 0 ? p.out_tempo : engine.tempo()),
                 g_samples[g_cur_sample], static_cast<unsigned long>(p.seed & 0xffffffffu));
    int        last = -100;
    mbb::Hooks h;
    h.log                  = LogHook;
    h.progress             = ProgressHook;
    h.ctx                  = &last;
    const uint32_t       t0 = System::GetNow();
    const mbb::GenResult r  = engine.Generate(p, mem, kPlayFrames, &h);
    if(!r.ok)
    {
        Log("generation failed");
        g_ui = UiState::Idle;
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
    Publish(target);
    if(settings.save)
        SaveBeat(b);
    g_ui = UiState::Idle;
    return true;
}

void WaitSwap()
{
    while(g_player.pending() >= 0)
        System::Delay(1);
    System::Delay(3); // and its 2 ms crossfade
}

int TakeEvent(volatile int& ev)
{
    ScopedIrqBlocker blk;
    const int        v = ev;
    ev                 = 0;
    return v;
}

// LIVE versions. As soon as a loop plays, new versions of each of its
// quarters are made in the background, one at a time (new chops, same tempo
// and length, carrying on from where the quarter starts). They are kept in the
// tail of a loop buffer (`mem`), behind the longest loop either buffer holds,
// so the undo loop and kept versions never overwrite them. Up to 7 per
// quarter; if fewer than 4 fit (long loops), a soloed quarter takes them all.
// A new beat, break, or a loop of another tempo or length starts over.

int16_t* SlotMem(int i)
{
    return g_play_mem[g_bank.mem] + 2 * (g_bank.base + i * g_bank.slot);
}

// Lays the bank out for the loop playing, unless it already fits it.
bool BankFit()
{
    const int a = g_player.active();
    if(a < 0 || a > 1 || !g_break_ok)
        return false;
    const mbb::PlayBuf& b = g_player.buf[a];
    if(!g_bank.ok || g_bank.fpb != b.fpb || g_bank.beats != b.loop_beats || b.frames > g_bank.base)
    {
        g_bank.epoch++;
        g_bank.ok    = true;
        g_bank.fpb   = b.fpb;
        g_bank.beats = b.loop_beats;
        g_bank.mem   = 1 - a;
        g_bank.base  = std::max(g_player.buf[0].frames, g_player.buf[1].frames);
        g_bank.slot  = static_cast<uint32_t>(std::ceil((b.loop_beats / 4 + 1) * b.fpb)) + 64;
        int n        = std::min<int>(kSlots, (kPlayFrames - g_bank.base) / g_bank.slot);
        if(b.loop_beats % 4)
            n = 0;
        for(int i = 0; i < n; i++)
        {
            g_bank.q[i]     = static_cast<int8_t>(n < 4 ? -1 : i % 4);
            g_bank.state[i] = 0;
        }
        g_bank.n = n;
        Log("versions: room for %d", n);
    }
    return g_bank.n > 0;
}

// Slot of version k (1..) of quarter q, -1 if there is none.
int SlotOf(int q, int k)
{
    for(int i = 0; i < g_bank.n; i++)
        if(g_bank.q[i] == q && --k == 0)
            return i;
    return -1;
}

// LIVE solo. Quarter q of the loop (buffer `full`) loops on its own (player
// buffers 2 and 3, from the next bar) and its pot picks a version. Ending the
// solo either swaps back to `full` or splices the version picked into the
// other loop buffer (`pool`) and swaps to that, so undo goes back to the loop
// as it was.
struct Solo
{
    mbb::PlayBuf full;
    int          a, pool, beats, sync, shown;
    uint32_t     s0, s1;
} g_s;

mbb::PlayBuf SoloBuf(int k)
{
    mbb::PlayBuf b = g_s.full;
    b.loop_beats   = g_s.beats;
    b.data         = g_s.full.data + 2 * g_s.s0;
    b.frames       = std::min(static_cast<uint32_t>((g_s.beats + 1) * b.fpb), b.frames - g_s.s0);
    const int i    = SlotOf(g_solo_q, k);
    if(i >= 0)
    {
        b.data   = SlotMem(i);
        b.frames = g_bank.frames[i];
    }
    return b;
}

void StartSolo(int q)
{
    WaitSwap();
    const int a = g_player.active();
    if(!g_break_ok || a < 0 || a > 1)
        return;
    g_s.full = g_player.buf[a];
    if(g_s.full.loop_beats % 4)
    {
        Log("solo: %d beats do not split into quarters", g_s.full.loop_beats);
        return;
    }
    BankFit();
    int n = 1;
    for(int i = 0; i < g_bank.n; i++)
    {
        if(g_bank.n < 4 && g_bank.q[i] != q) // long loops: this quarter takes every slot
        {
            g_bank.q[i]     = static_cast<int8_t>(q);
            g_bank.state[i] = 0;
            g_bank.epoch++;
        }
        n += g_bank.q[i] == q;
    }
    if(!g_pots_ready)
        n = 1;
    g_s.a     = a;
    g_s.pool  = 1 - a;
    g_s.beats = g_s.full.loop_beats / 4;
    g_s.sync  = g_s.beats % 4 ? g_s.beats : 4; // swaps in and out on a bar (or a shorter quarter)
    g_s.shown = 0;
    mbb::Player::Quarter(g_s.full, q, g_s.s0, g_s.s1);
    {
        ScopedIrqBlocker blk;
        g_player.buf[2] = SoloBuf(0);
        g_player.Publish(2, false, g_s.sync);
        g_solo_hold = g_pot_s[q];
        g_solo_live = false;
        g_solo_req  = 0;
        g_solo_wait = false;
        g_solo_n    = static_cast<uint8_t>(n);
        g_solo_q    = static_cast<int8_t>(q);
    }
    Log("solo quarter %d (%d versions)", q + 1, n - 1);
}

void EndSolo(bool keep)
{
    const int k = keep ? g_s.shown : 0;
    if(k > 0)
    {
        int16_t* const     dst = g_play_mem[g_s.pool];
        const mbb::PlayBuf v   = SoloBuf(k);
        const uint32_t     m   = std::min(v.frames, g_s.full.frames - g_s.s0);
        std::memcpy(dst + 2 * g_s.s0, v.data, m * 4);
        mbb::Player::Splice(g_s.full, dst, g_s.s0, g_s.s1, m);
        ScopedIrqBlocker blk;
        g_player.buf[g_s.pool]      = g_s.full;
        g_player.buf[g_s.pool].data = dst;
        g_player.Publish(g_s.pool, false, g_s.sync);
    }
    else
    {
        ScopedIrqBlocker blk;
        g_player.Publish(g_s.a, false, g_s.sync);
    }
    WaitSwap();
    if(k > 0)
        g_bank.state[SlotOf(g_solo_q, k)] = 0; // it is in the loop now: make another
    g_solo_q    = -1;
    g_solo_wait = false;
    Log(k > 0 ? "solo: kept version %d" : "solo: back to the loop", k);
}

// Swaps to the version the pot asks for (on the next beat), if it is ready.
void SoloSwitch()
{
    const int k  = g_solo_req, i = SlotOf(g_solo_q, k);
    const int st = k == 0 ? 1 : i < 0 ? 2 : g_bank.state[i];
    g_solo_wait  = st == 0;
    if(st != 1 || k == g_s.shown)
        return;
    ScopedIrqBlocker blk;
    const int  act = g_player.active();
    const bool in  = act >= 2; // else still waiting for the bar
    const int  b   = act == 2 ? 3 : 2;
    g_player.buf[b] = SoloBuf(k);
    g_player.Publish(b, false, in ? 1 : g_s.sync);
    g_s.shown = k;
}

// LIVE quarter taps and solo controls; also run while a version is being made.
void LivePoll()
{
    uint8_t taps;
    {
        ScopedIrqBlocker blk;
        taps = g_ev_quarter;
        // while soloing, another quarter's tap ends the solo and is kept to start its own
        g_ev_quarter = g_solo_q >= 0 ? static_cast<uint8_t>(taps & ~(1 << g_solo_q)) : 0;
    }
    if(g_solo_q >= 0)
    {
        if(TakeEvent(g_ev_undo) > 0)
            EndSolo(false);
        else if(taps || !g_live_mode)
            EndSolo(true);
        else
            SoloSwitch();
        return;
    }
    for(int q = 0; q < 4 && g_live_mode; q++)
        if((taps >> q) & 1)
        {
            StartSolo(q);
            return;
        }
}

// Next version to make: the one the soloed quarter's pot asks for, else
// that quarter's first missing one, else the first missing one.
int NextSlot()
{
    const int want = g_solo_q >= 0 ? SlotOf(g_solo_q, g_solo_req) : -1;
    if(want >= 0 && !g_bank.state[want])
        return want;
    int first = -1;
    for(int i = 0; i < g_bank.n; i++)
        if(!g_bank.state[i] && g_bank.q[i] >= 0)
        {
            if(g_bank.q[i] == g_solo_q)
                return i;
            if(first < 0)
                first = i;
        }
    return first;
}

// Button work that background versions give way to.
bool OtherWork()
{
    return g_ev_generate || g_ev_sample || g_ev_out || g_ev_chop || g_cq_r != g_cq_w
           || (g_solo_q < 0 && g_ev_undo);
}

volatile bool g_bg_stop = false;
int           g_making = -1, g_make_epoch = 0;

void BankHook(void*, float)
{
    LivePoll();
    g_bg_stop = OtherWork() || g_make_epoch != g_bank.epoch
                || g_bank.q[NextSlot()] != g_bank.q[g_making]; // a soloed quarter's come first
}

// Makes one missing version.
void BankWork()
{
    if(!g_pots_ready || g_chop_mode || OtherWork() || (g_solo_q < 0 && !BankFit()))
        return;
    const int i = NextSlot();
    if(i < 0)
        return;
    const int   q = g_bank.q[i], qb = g_bank.beats / 4;
    mbb::Params p = GenParams();
    p.beats       = qb + 2; // a beat over, for the splice crossfade
    p.start_beat  = q * qb;
    p.out_tempo   = 2880000.0 / g_bank.fpb;
    if(std::fabs(p.out_tempo - engine.tempo()) < 1e-6)
        p.out_tempo = 0; // the break's own tempo: no resampling
    mbb::Hooks h;
    h.progress   = BankHook;
    h.stop       = &g_bg_stop;
    g_making     = i;
    g_make_epoch = g_bank.epoch;
    g_bg_stop    = false;
    const uint32_t       t0 = System::GetNow();
    const mbb::GenResult r  = engine.Generate(p, SlotMem(i), g_bank.slot, &h);
    if(g_bg_stop || g_make_epoch != g_bank.epoch)
        return; // given up, or the bank changed meanwhile
    const bool ok     = r.ok && r.frames >= (qb + 1) * g_bank.fpb;
    g_bank.frames[i]  = r.frames;
    g_bank.state[i]   = ok ? 1 : 2;
    int v             = 0;
    for(int j = 0; j <= i; j++)
        v += g_bank.q[j] == q;
    Log(ok ? "quarter %d version %d in %lu ms" : "quarter %d version %d failed", q + 1, v,
        static_cast<unsigned long>(System::GetNow() - t0));
}

// LIVE: swaps back to the other buffer (the loop before the last change).
void Undo()
{
    WaitSwap();
    const int a = g_player.active();
    if(a < 0 || a > 1 || g_player.buf[1 - a].frames == 0)
    {
        Log("nothing to undo");
        return;
    }
    Publish(1 - a);
    Log("undo");
}

// Loads /MBB/OUT/<tempo>_<n>.wav (16-bit stereo 48 kHz, as written above).
bool LoadOutput(const OutFile& o)
{
    char path[64];
    std::snprintf(path, sizeof(path), "%s/%u_%u.wav", kOut, static_cast<unsigned>(o.tempo),
                  static_cast<unsigned>(o.n));
    Log("loading %s", path);
    SdReader     r;
    mbb::WavInfo info;
    if(!r.Open(path) || !mbb::ParseWav(r, info) || info.bits != 16 || info.channels != 2
       || info.is_float || !r.Seek(info.data_offset))
    {
        if(ReadOk(path, r))
            Log("%s: not a 16-bit stereo WAV", path);
        else
            Log("cannot open %s (FatFs error %d)", path, r.error());
        return false;
    }
    g_ui             = UiState::Busy;
    const int      target = ClaimFreeBuffer();
    mbb::PlayBuf&  b      = g_player.buf[target];
    int16_t* const mem    = g_play_mem[target];
    uint32_t       frames = info.frames < kPlayFrames ? info.frames : kPlayFrames;
    frames                = r.Read(mem, frames * 4) / 4;
    g_ui             = UiState::Idle;
    if(frames == 0 || !ReadOk(path, r))
        return false;
    b.data       = mem;
    b.frames     = frames;
    b.tempo      = o.tempo;
    b.fpb        = 48000.0 * 60.0 / o.tempo;
    const int nb = static_cast<int>(std::lround(frames / b.fpb)) - 1;
    b.loop_beats = nb > 1 ? nb : 1;
    Publish(target);
    g_cur_out = o;
    Log("playing %s (%d beats)", path, b.loop_beats);
    return true;
}

void StepOutput(int steps)
{
    ScanOutputs();
    if(g_n_outs == 0)
    {
        Log("no saved beats in %s", kOut);
        return;
    }
    int idx = g_n_outs - 1;
    for(int i = 0; i < g_n_outs; i++)
        if(g_outs[i].tempo == g_cur_out.tempo && g_outs[i].n == g_cur_out.n)
            idx = i;
    idx = ((idx + steps) % g_n_outs + g_n_outs) % g_n_outs;
    LoadOutput(g_outs[idx]);
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
    g_grid = settings.chop;

    // Read the pots (if on) even with use_pots = 0: LIVE mode uses them for
    // its effects (use_pots only turns tempo/beats/chaos/volume on/off).
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

    ScanOutputs();
    if(g_break_ok && settings.auto_generate)
        Generate();

    bool     shown_live   = false;
    int      shown_pot[4] = {};
    uint32_t pot_ms       = 0;
    while(true)
    {
        if(g_live_mode != shown_live)
        {
            shown_live = g_live_mode;
            Log(shown_live ? "LIVE mode" : "LIVE mode off");
            shown_pot[0] = -100; // print the pots
        }
        if(shown_live && g_pots_ready && System::GetNow() - pot_ms >= 250)
        {
            // pot positions 0..100, whenever one moves
            int  pv[4];
            bool moved = false;
            for(int f = 0; f < 4; f++)
            {
                pv[f] = static_cast<int>(g_pot_s[f] * 100 + 0.5f);
                moved = moved || std::abs(pv[f] - shown_pot[f]) >= 4;
            }
            if(moved)
            {
                pot_ms = System::GetNow();
                std::memcpy(shown_pot, pv, sizeof(pv));
                Log("pots D19 %d D20 %d D21 %d D22 %d", pv[0], pv[1], pv[2], pv[3]);
            }
        }
        LivePoll();
        if(g_solo_q < 0 && (TakeEvent(g_ev_undo) & 1))
            Undo();
        // chop edits first, so D15 acts on the latest selection
        while(g_cq_r != g_cq_w)
        {
            const uint8_t ev = g_cq[g_cq_r & 15];
            g_cq_r           = g_cq_r + 1;
            if(g_chop_mode)
                ChopEvent(ev);
        }
        if(TakeEvent(g_ev_chop) & 1)
        {
            if(g_chop_mode)
                LeaveChop(true);
            else
                EnterChop();
        }
        if(g_solo_q >= 0 && (g_ev_generate || g_ev_sample || g_ev_out))
            EndSolo(true); // these load a new loop
        if(TakeEvent(g_ev_generate) > 0)
        {
            if(g_chop_mode)
            {
                LeaveChop(false);
                if(Generate())
                {
                    ScopedIrqBlocker blk;
                    if(!g_player.playing())
                        g_player.TogglePlay();
                }
                else
                    LeaveChop(true);
            }
            else
                Generate();
        }

        const int ds = TakeEvent(g_ev_sample);
        if(ds != 0)
        {
            // Rescan (cards may have new files) and step from the current name.
            char cur[kNameLen] = "";
            if(g_cur_sample >= 0)
                std::strcpy(cur, g_samples[g_cur_sample]);
            ScanSamples();
            g_cur_sample = 0;
            for(int i = 0; i < g_n_samples; i++)
                if(strcasecmp(g_samples[i], cur) == 0)
                    g_cur_sample = i;
            g_ui = UiState::Busy;
            if(LoadBreak(g_cur_sample + ds))
            {
                g_ui = UiState::Idle;
                if(settings.auto_generate)
                    Generate();
            }
            else
                g_ui = UiState::Error;
        }

        // Like the norns encoder: wait until the presses stop, then load.
        if(g_ev_out != 0 && System::GetNow() - g_ev_out_ms > 400)
            StepOutput(TakeEvent(g_ev_out));

        BankWork();
        System::Delay(1);
    }
}

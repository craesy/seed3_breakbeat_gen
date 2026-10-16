// SD card write test for the Daisy Seed, wired as for makebreakbeat.
//
// Tries several bus settings, from slowest to fastest. Each one mounts the
// card, reads /MBB/config.txt, writes /MBB/SDTEST.BIN (small, then 32 KB),
// reads it back and deletes it. Every step is logged over USB serial with the
// FatFs result and the STM32 SD error code. The test starts once a serial
// terminal is open; press RESET to run it again.
#include "daisy_seed.h"
#include "ff_gen_drv.h"
#include "util/sd_diskio.h"
#include "util/bsp_sd_diskio.h"

#include <cstring>

using namespace daisy;

extern SD_HandleTypeDef hsd1;

namespace
{
DaisySeed    hw;
SdmmcHandler sdmmc;
FATFS        fs;
FIL          fil;
char         sd_path[4];
uint8_t      buf[32768] __attribute__((aligned(32)));

constexpr const char* kTestFile = "/MBB/SDTEST.BIN";
constexpr const char  kSmall[]  = "makebreakbeat SD write test\r\n";

struct Setup
{
    SdmmcHandler::BusWidth width;
    SdmmcHandler::Speed    speed;
    bool                   pullups;
};

const Setup kSetups[] = {
    {SdmmcHandler::BusWidth::BITS_1, SdmmcHandler::Speed::SLOW, true},
    {SdmmcHandler::BusWidth::BITS_1, SdmmcHandler::Speed::MEDIUM_SLOW, true},
    {SdmmcHandler::BusWidth::BITS_4, SdmmcHandler::Speed::SLOW, true},
    {SdmmcHandler::BusWidth::BITS_4, SdmmcHandler::Speed::MEDIUM_SLOW, true},
    {SdmmcHandler::BusWidth::BITS_4, SdmmcHandler::Speed::MEDIUM_SLOW, false},
    {SdmmcHandler::BusWidth::BITS_4, SdmmcHandler::Speed::STANDARD, true},
};
constexpr int kNumSetups = sizeof(kSetups) / sizeof(kSetups[0]);

void PullUps()
{
    GPIOC->PUPDR = (GPIOC->PUPDR & ~(0xffu << 16)) | (0x55u << 16); // PC8-PC11: DAT0-3
    GPIOD->PUPDR = (GPIOD->PUPDR & ~(3u << 4)) | (1u << 4);         // PD2: CMD
}

const char* StateName(uint32_t s)
{
    static const char* const names[] = {"idle",      "ready",     "ident",
                                        "standby",   "transfer",  "sending",
                                        "receiving", "programming", "disconnected"};
    return s < 9 ? names[s] : "no answer";
}

void CardState()
{
    hsd1.ErrorCode = HAL_SD_ERROR_NONE;
    const uint32_t s = HAL_SD_GetCardState(&hsd1);
    hw.PrintLine("    card now: %s (state %lu, SD error 0x%08lx)",
                 hsd1.ErrorCode ? "no answer" : StateName(s),
                 static_cast<unsigned long>(s),
                 static_cast<unsigned long>(hsd1.ErrorCode));
}

// Returns 0 = write ok, 1 = card init/mount failed, 2 = read failed,
// 3 = write failed.
int Run(int k)
{
    const Setup& s = kSetups[k];
    f_mount(nullptr, sd_path, 0);
    if(hsd1.Instance)
        HAL_SD_DeInit(&hsd1); // back to reset: clock off, pins released

    SdmmcHandler::Config cfg;
    cfg.Defaults();
    cfg.width = s.width;
    cfg.speed = s.speed;
    sdmmc.Init(cfg);
    const uint32_t khz = HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_SDMMC)
                         / (2u * hsd1.Init.ClockDiv) / 1000u;
    hw.PrintLine("");
    hw.PrintLine("--- test %d of %d: %d-bit, %lu kHz, pull-ups %s", k + 1, kNumSetups,
                 s.width == SdmmcHandler::BusWidth::BITS_1 ? 1 : 4,
                 static_cast<unsigned long>(khz), s.pullups ? "on" : "off");

    // The first init configures the pins (without pull-ups); add them, then
    // initialise the card again with the pull-ups in place.
    BSP_SD_Init();
    if(s.pullups)
        PullUps();
    uint32_t t = System::GetNow();
    if(BSP_SD_Init() != MSD_OK)
    {
        hw.PrintLine("    card init FAILED (SD error 0x%08lx)",
                     static_cast<unsigned long>(hsd1.ErrorCode));
        return 1;
    }
    hw.PrintLine("    card init ok: %s, %lu MB, %s speed, class %u (%lu ms)",
                 hsd1.SdCard.CardType == CARD_SDHC_SDXC ? "SDHC/SDXC" : "SDSC",
                 static_cast<unsigned long>(
                     (static_cast<uint64_t>(hsd1.SdCard.LogBlockNbr)
                      * hsd1.SdCard.LogBlockSize)
                     >> 20),
                 hsd1.SdCard.CardSpeed == CARD_HIGH_SPEED ? "high" : "normal",
                 static_cast<unsigned>(hsd1.SdCard.Class),
                 static_cast<unsigned long>(System::GetNow() - t));

    FRESULT r = f_mount(&fs, sd_path, 1);
    if(r != FR_OK)
    {
        hw.PrintLine("    mount FAILED: FatFs %d, SD error 0x%08lx", r,
                     static_cast<unsigned long>(hsd1.ErrorCode));
        return 1;
    }

    UINT n = 0;
    r      = f_open(&fil, "/MBB/config.txt", FA_READ);
    if(r == FR_OK)
    {
        r = f_read(&fil, buf, sizeof(buf), &n);
        f_close(&fil);
    }
    hw.PrintLine("    read /MBB/config.txt: %s (FatFs %d, %u bytes)",
                 r == FR_OK ? "ok" : "FAILED", r, n);
    if(r != FR_OK && r != FR_NO_FILE && r != FR_NO_PATH)
        return 2;

    // Small write: f_sync makes FatFs write the directory, FAT and data
    // sectors one block at a time.
    hw.PrintLine("    writing %s ...", kTestFile);
    hsd1.ErrorCode = HAL_SD_ERROR_NONE;
    t              = System::GetNow();
    r              = f_open(&fil, kTestFile, FA_CREATE_ALWAYS | FA_WRITE);
    if(r == FR_OK)
        r = f_write(&fil, kSmall, sizeof(kSmall) - 1, &n);
    if(r == FR_OK)
        r = f_sync(&fil);
    hw.PrintLine("    small write: %s (FatFs %d, SD error 0x%08lx, %lu ms)",
                 r == FR_OK ? "ok" : "FAILED", r,
                 static_cast<unsigned long>(hsd1.ErrorCode),
                 static_cast<unsigned long>(System::GetNow() - t));
    if(r != FR_OK)
    {
        CardState(); // don't close: FatFs would retry the writes
        return 3;
    }

    // 32 KB in one go: a multi-block write, like saving a beat.
    for(uint32_t i = 0; i < sizeof(buf); i++)
        buf[i] = static_cast<uint8_t>(i * 7 + (i >> 8));
    t = System::GetNow();
    r = f_write(&fil, buf, sizeof(buf), &n);
    if(r == FR_OK && n != sizeof(buf))
        r = FR_DENIED;
    if(r == FR_OK)
        r = f_close(&fil);
    hw.PrintLine("    32 KB write: %s (FatFs %d, SD error 0x%08lx, %lu ms)",
                 r == FR_OK ? "ok" : "FAILED", r,
                 static_cast<unsigned long>(hsd1.ErrorCode),
                 static_cast<unsigned long>(System::GetNow() - t));
    if(r != FR_OK)
    {
        CardState();
        return 3;
    }

    // Read it back.
    std::memset(buf, 0, sizeof(buf));
    bool ok = f_open(&fil, kTestFile, FA_READ) == FR_OK
              && f_read(&fil, buf, sizeof(kSmall) - 1, &n) == FR_OK
              && std::memcmp(buf, kSmall, sizeof(kSmall) - 1) == 0
              && f_read(&fil, buf, sizeof(buf), &n) == FR_OK && n == sizeof(buf);
    for(uint32_t i = 0; ok && i < sizeof(buf); i++)
        ok = buf[i] == static_cast<uint8_t>(i * 7 + (i >> 8));
    f_close(&fil);
    f_unlink(kTestFile);
    hw.PrintLine("    read back: %s", ok ? "ok" : "MISMATCH");
    return ok ? 0 : 3;
}
} // namespace

int main(void)
{
    hw.Init(true);
    hw.StartLog(true); // waits for a serial terminal
    hw.PrintLine("SD card write test (makebreakbeat wiring: D1-D6)");
    FATFS_LinkDriver(&SD_Driver, sd_path);

    static const char* const kResult[]
        = {"WRITE OK", "card did not start", "read failed", "WRITE FAILED"};
    int res[kNumSetups];
    for(int k = 0; k < kNumSetups; k++)
        res[k] = Run(k);

    hw.PrintLine("");
    hw.PrintLine("=== summary");
    bool any_ok = false;
    for(int k = 0; k < kNumSetups; k++)
    {
        hw.PrintLine("  test %d: %s", k + 1, kResult[res[k]]);
        any_ok = any_ok || res[k] == 0;
    }
    hw.PrintLine("done - press RESET to run again");

    // LED: slow blink = some setting can write, fast blink = none can.
    while(true)
    {
        hw.SetLed((System::GetNow() / (any_ok ? 500 : 100)) & 1);
        System::Delay(10);
    }
}

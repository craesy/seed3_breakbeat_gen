// /MBB/config.txt parser: `key = value` lines, `#` or `;` comments.
#pragma once

#include "engine.h"

namespace mbb
{
struct Settings
{
    Params gen;                  // probabilities, drum levels, seed
    int    beats         = 16;   // loop length in beats (norns "beats")
    double tempo         = 0;    // output BPM, 0 = keep the break's tempo
    double input_tempo   = 0;    // break BPM, 0 = detect
    bool   legacy_rate     = true; // replicate `sox -r 48000` reinterpretation
    bool   legacy_channels = true; // replicate `-c <n>` on kick/snare
    bool   use_pots      = false; // pots on D19-D22 set tempo/beats/chaos/volume
    uint32_t seed        = 0;    // 0 = new random seed for every generation
    bool   save          = true; // write each generated loop to /MBB/OUT
    bool   auto_generate = true; // generate as soon as a break is loaded
    bool   auto_play     = true;  // start playing after the first beat
    char   sample[64]    = "";   // break to load first (file name in SAMPLES)
    uint32_t chop        = 0;    // 0 = chop at the hits, N = N equal chops
};

// Applies every recognised key to `s`. Returns the number of lines that
// could not be understood; `bad_line` receives the first such line number.
int ParseConfig(const char* text, uint32_t len, Settings& s, int* bad_line = nullptr);

// Writes a fully commented default config into `buf`. Returns its length.
uint32_t DefaultConfig(char* buf, uint32_t cap);

} // namespace mbb

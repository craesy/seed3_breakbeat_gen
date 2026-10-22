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
    bool   use_pots      = false; // ignored (older firmware), still parsed
    uint32_t seed        = 0;    // 0 = new random seed for every generation
    bool   save          = true; // write each generated loop to /MBB/OUT
    bool   auto_generate = true; // generate as soon as a break is loaded
    bool   auto_play     = true;  // ignored (older firmware), still parsed
    char   sample[64]    = "";   // break to load first (file name in SAMPLES)
    uint32_t chop        = 0;    // 0 = chop at the hits, N = N equal chops
};

// Applies every recognised key to `s`. Returns the number of lines that
// could not be understood; `bad_line` receives the first such line number.
int ParseConfig(const char* text, uint32_t len, Settings& s, int* bad_line = nullptr);

// Focus mode, D16: pot f (0..1, D19..D22) moves a group of probabilities
// from the configured values (fully left) linearly up to a maximum (fully
// right; a value configured higher than that stays as it is):
//   D19 shuffle: p_deviation 100, p_kick 100, p_snare 100
//   D20 stutter: p_stutter 100, p_trunc 50
//   D21 pitch:   p_reverse 80, p_pitch 60, p_half 40
//   D22 space:   p_reverb 60, p_stretch 40
void ApplyPots(Params& p, const float pots[4]);

// Writes a fully commented default config into `buf`. Returns its length.
uint32_t DefaultConfig(char* buf, uint32_t cap);

} // namespace mbb

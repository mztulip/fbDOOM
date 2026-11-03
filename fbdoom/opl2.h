//
// Small YM3812 (OPL2) emulator for the music, written for the MStar Kronus
// (WIWA HD95): two 256-entry tables (log-sine and exponent, 1 KB) as in the
// real chip and about 1 KB of state, so it stays in the L1 cache. The large
// random-access tables of fmopl (about 136 KB) evicted the game's lines from
// the cache shared by both VPEs, and the extra scattered DRAM traffic
// starved the display (GOP).
//
// Only what Doom's OPL music uses: 9 melodic channels, 4 waveforms,
// feedback, FM/additive, KSL, KSR, tremolo, vibrato. No rhythm mode, no
// timers, no CSM.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//

#ifndef OPL2_H
#define OPL2_H

#include <stdint.h>

typedef struct opl2_s opl2_t;

opl2_t *OPL2_Create(unsigned int rate);
void OPL2_Destroy(opl2_t *chip);
void OPL2_Write(opl2_t *chip, int reg, int value);
void OPL2_Render(opl2_t *chip, int16_t *buf, int samples);

#endif

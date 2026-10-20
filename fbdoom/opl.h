//
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//     OPL interface (the part of Chocolate Doom's opl/opl.h used by
//     i_oplmusic.c), implemented by opl_fmopl.c.
//

#ifndef OPL_OPL_H
#define OPL_OPL_H

#include <inttypes.h>

typedef void (*opl_callback_t)(void *data);

typedef enum
{
    OPL_INIT_NONE,
    OPL_INIT_OPL2,
    OPL_INIT_OPL3,
} opl_init_result_t;

#define OPL_NUM_OPERATORS   21
#define OPL_NUM_VOICES      9

#define OPL_REG_WAVEFORM_ENABLE   0x01
#define OPL_REG_TIMER1            0x02
#define OPL_REG_TIMER2            0x03
#define OPL_REG_TIMER_CTRL        0x04
#define OPL_REG_FM_MODE           0x08

// Operator registers (21 of each):

#define OPL_REGS_TREMOLO          0x20
#define OPL_REGS_LEVEL            0x40
#define OPL_REGS_ATTACK           0x60
#define OPL_REGS_SUSTAIN          0x80
#define OPL_REGS_WAVEFORM         0xE0

// Voice registers (9 of each):

#define OPL_REGS_FREQ_1           0xA0
#define OPL_REGS_FREQ_2           0xB0
#define OPL_REGS_FEEDBACK         0xC0

opl_init_result_t OPL_Init(unsigned int port_base);
void OPL_Shutdown(void);
void OPL_SetSampleRate(unsigned int rate);
void OPL_WriteRegister(int reg, int value);

// Timer callbacks, in microseconds of played music.

void OPL_SetCallback(uint64_t us, opl_callback_t callback, void *data);
void OPL_AdjustCallbacks(float factor);
void OPL_ClearCallbacks(void);

// Keep the callbacks and the mixer away while changing the music state.

void OPL_Lock(void);
void OPL_Unlock(void);

void OPL_SetPaused(int paused);

// opl_fmopl.c: add 'frames' stereo frames of music at 2 * OPL_FM_RATE Hz
// to acc (interleaved left/right). Called by the sound mixer thread.

#define OPL_FM_RATE 24000

void OPL_MixMusic(int32_t *acc, int frames);

#endif

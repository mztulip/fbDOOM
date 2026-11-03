//
// OPL music without SDL (MStar Kronus, WIWA HD95): the opl.h interface on
// the small OPL2 emulator of opl2.c, or with -fmopl on the fmopl YM3812
// emulator (the first one used, kept for comparison: its large tables
// disturb the display). There is no audio callback of its own:
// the sound effects mixer thread (i_mstarsound.c) calls OPL_MixMusic() for
// every block. It runs the music timer callbacks of i_oplmusic.c at the
// right sample, renders the chip at OPL_FM_RATE and doubles the rate to the
// mixer's. Music time is counted in rendered samples.
//
// fmopl at 24 kHz costs about 8 % of the 552 MHz CPU with all 9 voices on;
// Nuked OPL3 (Chocolate Doom 3) needed about 70 %.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//

#include <pthread.h>
#include <string.h>

#include "fmopl.h"
#include "m_argv.h"
#include "opl.h"
#include "opl2.h"

#define OPL_CLOCK	3579545
#define MAX_CALLBACKS	64
#define MAX_BLOCK	1024		// OPL samples per OPL_MixMusic call

typedef struct
{
    opl_callback_t callback;
    void *data;
    uint64_t time;		// us
} opl_timer_t;

static opl_timer_t timers[MAX_CALLBACKS];
static int num_timers;

// Recursive: the callbacks run with it held and call OPL_WriteRegister and
// OPL_SetCallback, which lock it too.
static pthread_mutex_t opl_mutex;
static FM_OPL *fm;		// with -fmopl
static opl2_t *chip;		// otherwise
static int running;		// one of them is set up
static uint64_t samples;	// rendered since OPL_Init
static uint64_t now_us;
static int paused;
static int32_t last;		// previous sample, for the 2x interpolation
static int32_t dc_in, dc_out;	// DC blocker: previous input, output << 4

static void SetTime(void)
{
    now_us = samples * 1000000 / OPL_FM_RATE;
}

static int NextTimer(void)
{
    int i, best = -1;

    for (i = 0; i < num_timers; ++i)
    {
        if (best < 0 || timers[i].time < timers[best].time)
        {
            best = i;
        }
    }
    return best;
}

void OPL_WriteRegister(int reg, int value)
{
    if (!running)
    {
        return;
    }
    pthread_mutex_lock(&opl_mutex);
    if (fm != NULL)
    {
        OPLWrite(fm, 0, reg);
        OPLWrite(fm, 1, value);
    }
    else if (chip != NULL)
    {
        OPL2_Write(chip, reg, value);
    }
    pthread_mutex_unlock(&opl_mutex);
}

static void InitRegisters(void)
{
    int r;

    // as Doom (and Chocolate Doom's OPL_InitRegisters) does it
    for (r = OPL_REGS_LEVEL; r <= OPL_REGS_LEVEL + OPL_NUM_OPERATORS; ++r)
    {
        OPL_WriteRegister(r, 0x3f);
    }
    for (r = OPL_REGS_ATTACK; r <= OPL_REGS_WAVEFORM + OPL_NUM_OPERATORS; ++r)
    {
        OPL_WriteRegister(r, 0x00);
    }
    for (r = 1; r < OPL_REGS_LEVEL; ++r)
    {
        OPL_WriteRegister(r, 0x00);
    }
    OPL_WriteRegister(OPL_REG_TIMER_CTRL, 0x60);
    OPL_WriteRegister(OPL_REG_TIMER_CTRL, 0x80);
    OPL_WriteRegister(OPL_REG_WAVEFORM_ENABLE, 0x20);
}

opl_init_result_t OPL_Init(unsigned int port_base)
{
    pthread_mutexattr_t attr;

    (void) port_base;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&opl_mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    if (M_CheckParm("-fmopl") > 0)
    {
        fm = OPLCreate(OPL_CLOCK, OPL_FM_RATE);
    }
    else
    {
        chip = OPL2_Create(OPL_FM_RATE);
    }
    if (fm == NULL && chip == NULL)
    {
        return OPL_INIT_NONE;
    }
    running = 1;
    num_timers = 0;
    samples = 0;
    now_us = 0;
    paused = 0;
    last = 0;
    dc_in = dc_out = 0;
    InitRegisters();
    return OPL_INIT_OPL2;
}

void OPL_Shutdown(void)
{
    if (!running)
    {
        return;
    }
    pthread_mutex_lock(&opl_mutex);
    running = 0;			// OPL_MixMusic stops here
    num_timers = 0;
    pthread_mutex_unlock(&opl_mutex);
    if (fm != NULL)
    {
        OPLDestroy(fm);
        fm = NULL;
    }
    OPL2_Destroy(chip);
    chip = NULL;
}

void OPL_SetSampleRate(unsigned int rate)
{
    (void) rate;			// always OPL_FM_RATE, doubled by OPL_MixMusic
}

void OPL_SetCallback(uint64_t us, opl_callback_t callback, void *data)
{
    pthread_mutex_lock(&opl_mutex);
    if (num_timers < MAX_CALLBACKS)
    {
        timers[num_timers].callback = callback;
        timers[num_timers].data = data;
        timers[num_timers].time = now_us + us;
        ++num_timers;
    }
    pthread_mutex_unlock(&opl_mutex);
}

void OPL_AdjustCallbacks(float factor)
{
    int i;

    // tempo change: scale the time left to every pending callback
    pthread_mutex_lock(&opl_mutex);
    for (i = 0; i < num_timers; ++i)
    {
        timers[i].time = now_us + (uint64_t) ((timers[i].time - now_us) * factor);
    }
    pthread_mutex_unlock(&opl_mutex);
}

void OPL_ClearCallbacks(void)
{
    pthread_mutex_lock(&opl_mutex);
    num_timers = 0;
    pthread_mutex_unlock(&opl_mutex);
}

void OPL_Lock(void)
{
    pthread_mutex_lock(&opl_mutex);
}

void OPL_Unlock(void)
{
    pthread_mutex_unlock(&opl_mutex);
}

void OPL_SetPaused(int p)
{
    pthread_mutex_lock(&opl_mutex);
    paused = p;
    pthread_mutex_unlock(&opl_mutex);
}

void OPL_MixMusic(int32_t *acc, int frames)
{
    static int16_t buf[MAX_BLOCK];
    int need = frames / 2, done = 0, gain, i;

    if (!running || need > MAX_BLOCK)
    {
        return;
    }
    pthread_mutex_lock(&opl_mutex);
    if (!running || paused)
    {
        pthread_mutex_unlock(&opl_mutex);
        return;
    }
    while (done < need)
    {
        int t, n = need - done;

        // run every callback that is due; they may schedule new ones
        while ((t = NextTimer()) >= 0 && timers[t].time <= now_us)
        {
            opl_timer_t due = timers[t];

            timers[t] = timers[--num_timers];
            due.callback(due.data);
        }
        if (t >= 0)
        {
            // samples until the next callback, at least one
            uint64_t until = ((timers[t].time - now_us) * OPL_FM_RATE + 999999) / 1000000;

            if (until < (uint64_t) n)
            {
                n = until > 0 ? (int) until : 1;
            }
        }
        if (fm != NULL)
        {
            YM3812UpdateOne(fm, buf + done, n);
        }
        else
        {
            OPL2_Render(chip, buf + done, n);
        }
        done += n;
        samples += n;
        SetTime();
    }
    gain = fm != NULL ? 1 : 2;		// opl2.c is 6 dB quieter than fmopl
    pthread_mutex_unlock(&opl_mutex);

    for (i = 0; i < need; ++i)
    {
        int32_t x = buf[i] * gain, s, mid;

        // DC blocker (about 7.5 Hz): the half-wave and rectified waveforms
        // add an offset that only eats into the headroom
        dc_out += ((x - dc_in) << 4) - (dc_out >> 9);
        dc_in = x;
        s = dc_out >> 4;
        mid = (last + s) / 2;

        acc[4 * i] += mid;
        acc[4 * i + 1] += mid;
        acc[4 * i + 2] += s;
        acc[4 * i + 3] += s;
        last = s;
    }
}

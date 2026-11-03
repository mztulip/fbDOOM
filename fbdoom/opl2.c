//
// Small YM3812 (OPL2) emulator, see opl2.h.
//
// The operator works in the log domain like the chip: a 10-bit phase picks
// a quarter-wave log-sine value, the envelope attenuation (0.1875 dB steps,
// 8 log units each) is added and the exponent table turns the sum back into
// a linear 13-bit value. The chip runs at clock / 72 = 49716 Hz; here phase,
// envelope and LFO steps are scaled to the output rate.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "opl2.h"

#define CHIP_RATE	49716		// 3579545 Hz / 72
#define ENV_FRAC	16		// fraction bits of the envelope
#define ENV_MAX		(511 << ENV_FRAC)
#define ENV_QUIET	(384 << ENV_FRAC)	// from here on the output is 0
#define SILENT		0x1fff		// log level that gives 0

enum { EG_OFF, EG_ATTACK, EG_DECAY, EG_SUSTAIN, EG_RELEASE };

typedef struct
{
    uint32_t phase;		// top 10 bits are the wave phase
    uint32_t inc;		// per output sample, vibrato applied
    uint32_t env;		// attenuation, ENV_FRAC fraction bits
    uint32_t env_inc;		// for the current state
    uint32_t sl;		// sustain level, same format as env
    int att;			// TL + KSL, envelope units
    int out, prev;		// last two outputs (feedback)
    uint8_t state;
    uint8_t reg20, reg40, reg60, reg80, ws;
    uint8_t wave;		// ws, or 0 while waveform select is off
} op_t;

typedef struct
{
    op_t op[2];			// modulator, carrier
    int fnum, block, key, fb, con;
} channel_t;

struct opl2_s
{
    channel_t ch[9];
    uint32_t rate_inc[64];	// envelope step per output sample by rate
    uint32_t phase_scale;	// 16.16: chip phase units -> ours
    uint32_t chip_time, chip_step;	// chip samples, 16.16
    uint32_t lfo_tick;		// chip_time >> 22: the LFOs step every 64
    int vibpos, trem;
    int wse, nts, dam, dvb;
    int reg;			// address latch (index 0 writes)
};

static uint16_t logsin[256];
static uint16_t exptab[256];

static const uint8_t mult_x2[16] = {
    1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30
};
static const uint8_t ksl_rom[16] = {
    0, 32, 40, 45, 48, 51, 53, 55, 56, 58, 59, 60, 61, 62, 63, 64
};
static const uint8_t ksl_shift[4] = { 8, 1, 2, 0 };

// register offset 0x00-0x15 -> operator index (channel * 2 + op), -1 = none
static const int8_t slot_of_reg[22] = {
     0,  2,  4,  1,  3,  5, -1, -1,
     6,  8, 10,  7,  9, 11, -1, -1,
    12, 14, 16, 13, 15, 17
};

static void InitTables(void)
{
    int i;

    if (exptab[255] != 0)
    {
        return;
    }
    for (i = 0; i < 256; ++i)
    {
        double s = sin((i + 0.5) * M_PI / 512.0);

        logsin[i] = (uint16_t) lrint(-log2(s) * 256.0);
        exptab[i] = (uint16_t) lrint(pow(2.0, (255 - i) / 256.0) * 1024.0);
    }
}

static op_t *Op(opl2_t *chip, int i)
{
    return &chip->ch[i >> 1].op[i & 1];
}

static int KeyScale(opl2_t *chip, channel_t *ch, op_t *op)
{
    int kc = (ch->block << 1) | ((ch->fnum >> (chip->nts ? 8 : 9)) & 1);

    return (op->reg20 & 0x10) ? kc : kc >> 2;
}

static uint32_t RateInc(opl2_t *chip, channel_t *ch, op_t *op, int rate)
{
    int r;

    if (rate == 0)
    {
        return 0;
    }
    r = rate * 4 + KeyScale(chip, ch, op);
    return chip->rate_inc[r > 63 ? 63 : r];
}

static void SetEnvInc(opl2_t *chip, channel_t *ch, op_t *op)
{
    switch (op->state)
    {
        case EG_ATTACK:
            op->env_inc = RateInc(chip, ch, op, op->reg60 >> 4);
            break;
        case EG_DECAY:
            op->env_inc = RateInc(chip, ch, op, op->reg60 & 15);
            break;
        case EG_SUSTAIN:
            // percussive sounds (EG type 0) keep falling at the release rate
            op->env_inc = (op->reg20 & 0x20) ? 0
                        : RateInc(chip, ch, op, op->reg80 & 15);
            break;
        case EG_RELEASE:
            op->env_inc = RateInc(chip, ch, op, op->reg80 & 15);
            break;
        default:
            op->env_inc = 0;
            break;
    }
}

static void UpdateAtt(channel_t *ch, op_t *op)
{
    int ksl = (ksl_rom[ch->fnum >> 6] << 2) - ((8 - ch->block) << 5);

    if (ksl < 0)
    {
        ksl = 0;
    }
    op->att = ((op->reg40 & 0x3f) << 2) + (ksl >> ksl_shift[op->reg40 >> 6]);
}

static void UpdateInc(opl2_t *chip, channel_t *ch, op_t *op)
{
    int fnum = ch->fnum;
    uint32_t base;

    if (op->reg20 & 0x40)
    {
        // vibrato: 8 steps, depth from the top 3 bits of F-number
        int range = (fnum >> 7) & 7;

        if (!chip->dvb)
        {
            range >>= 1;
        }
        if ((chip->vibpos & 3) == 0)
        {
            range = 0;
        }
        else if (chip->vibpos & 1)
        {
            range >>= 1;
        }
        fnum += (chip->vibpos & 4) ? -range : range;
    }
    // chip phase step per chip sample, 19-bit phase accumulator
    base = ((((uint32_t) fnum << ch->block) >> 1) * mult_x2[op->reg20 & 15]) >> 1;
    op->inc = (uint32_t) (((uint64_t) base * chip->phase_scale) >> 16);
}

static void UpdateChannel(opl2_t *chip, channel_t *ch)
{
    int i;

    for (i = 0; i < 2; ++i)
    {
        UpdateAtt(ch, &ch->op[i]);
        UpdateInc(chip, ch, &ch->op[i]);
        SetEnvInc(chip, ch, &ch->op[i]);
    }
}

static void KeyOn(opl2_t *chip, channel_t *ch, op_t *op)
{
    op->phase = 0;
    if ((op->reg60 >> 4) == 15)
    {
        op->env = 0;			// rate 15 attack is instant
        op->state = EG_DECAY;
    }
    else
    {
        op->state = EG_ATTACK;
    }
    SetEnvInc(chip, ch, op);
}

static void KeyOff(opl2_t *chip, channel_t *ch, op_t *op)
{
    if (op->state != EG_OFF)
    {
        op->state = EG_RELEASE;
        SetEnvInc(chip, ch, op);
    }
}

opl2_t *OPL2_Create(unsigned int rate)
{
    opl2_t *chip;
    double ratio = (double) CHIP_RATE / rate;
    int r, i;

    InitTables();
    chip = calloc(1, sizeof(*chip));
    if (chip == NULL)
    {
        return NULL;
    }
    // envelope step per chip sample: (4 + rate & 3) << (rate >> 2) in units
    // of 2^-15 (rate 60: 4 per sample, 512 steps in 2.6 ms)
    for (r = 4; r < 64; ++r)
    {
        int rr = r > 60 ? 60 : r;
        double step = (4 + (rr & 3)) * pow(2.0, (rr >> 2) - 15);

        chip->rate_inc[r] = (uint32_t) (step * ratio * (1 << ENV_FRAC) + 0.5);
    }
    chip->phase_scale = (uint32_t) (ratio * (1 << 13) * 65536.0 + 0.5);
    chip->chip_step = (uint32_t) (ratio * 65536.0 + 0.5);
    for (i = 0; i < 18; ++i)
    {
        op_t *op = Op(chip, i);

        op->env = ENV_MAX;
        op->sl = ENV_MAX;
        op->state = EG_OFF;
    }
    return chip;
}

void OPL2_Destroy(opl2_t *chip)
{
    free(chip);
}

void OPL2_Write(opl2_t *chip, int reg, int value)
{
    channel_t *ch;
    op_t *op;
    int n;

    reg &= 0xff;
    value &= 0xff;
    if (reg >= 0x20 && reg < 0xa0)
    {
        n = slot_of_reg[(reg & 0x1f) < 22 ? reg & 0x1f : 6];
        if ((reg & 0x1f) >= 22 || n < 0)
        {
            return;
        }
        op = Op(chip, n);
        ch = &chip->ch[n >> 1];
        switch (reg & 0xe0)
        {
            case 0x20:
                op->reg20 = value;
                UpdateInc(chip, ch, op);
                SetEnvInc(chip, ch, op);
                break;
            case 0x40:
                op->reg40 = value;
                UpdateAtt(ch, op);
                break;
            case 0x60:
                op->reg60 = value;
                SetEnvInc(chip, ch, op);
                break;
            case 0x80:
                op->reg80 = value;
                // SL 15 is 93 dB, the others 3 dB (16 envelope units) each
                n = value >> 4;
                op->sl = (uint32_t) (n == 15 ? 31 : n) << (4 + ENV_FRAC);
                SetEnvInc(chip, ch, op);
                break;
        }
        return;
    }
    if (reg >= 0xe0 && reg < 0xf6)
    {
        n = slot_of_reg[reg & 0x1f];
        if (n >= 0)
        {
            op = Op(chip, n);
            op->ws = value & 3;
            op->wave = chip->wse ? op->ws : 0;
        }
        return;
    }
    if ((reg & 0xf0) == 0xa0 || ((reg & 0xf0) == 0xb0 && reg != 0xbd)
     || (reg & 0xf0) == 0xc0)
    {
        if ((reg & 15) > 8)
        {
            return;
        }
        ch = &chip->ch[reg & 15];
        switch (reg & 0xf0)
        {
            case 0xa0:
                ch->fnum = (ch->fnum & 0x300) | value;
                UpdateChannel(chip, ch);
                break;
            case 0xb0:
                ch->fnum = (ch->fnum & 0xff) | ((value & 3) << 8);
                ch->block = (value >> 2) & 7;
                UpdateChannel(chip, ch);
                if ((value & 0x20) && !ch->key)
                {
                    KeyOn(chip, ch, &ch->op[0]);
                    KeyOn(chip, ch, &ch->op[1]);
                }
                else if (!(value & 0x20) && ch->key)
                {
                    KeyOff(chip, ch, &ch->op[0]);
                    KeyOff(chip, ch, &ch->op[1]);
                }
                ch->key = (value & 0x20) != 0;
                break;
            case 0xc0:
                ch->fb = (value >> 1) & 7;
                ch->con = value & 1;
                break;
        }
        return;
    }
    switch (reg)
    {
        case 0x01:
            chip->wse = (value & 0x20) != 0;
            for (n = 0; n < 18; ++n)
            {
                op = Op(chip, n);
                op->wave = chip->wse ? op->ws : 0;
            }
            break;
        case 0x08:
            chip->nts = (value & 0x40) != 0;
            break;
        case 0xbd:
            chip->dam = (value & 0x80) != 0;
            chip->dvb = (value & 0x40) != 0;
            break;
    }
}

static void StepEnvelope(op_t *op)
{
    switch (op->state)
    {
        case EG_ATTACK:
        {
            // exponential approach to 0 dB
            uint32_t dec = (uint32_t) (((uint64_t) (op->env + (1 << ENV_FRAC))
                                        * op->env_inc) >> (ENV_FRAC + 3));

            if (dec >= op->env)
            {
                op->env = 0;
                op->state = EG_DECAY;
                op->env_inc = 0;	// set by the caller, see OpCalc
            }
            else
            {
                op->env -= dec;
            }
            break;
        }
        case EG_DECAY:
            op->env += op->env_inc;
            if (op->env >= op->sl)
            {
                op->env = op->sl;
                op->state = EG_SUSTAIN;
                op->env_inc = 0;
            }
            break;
        case EG_SUSTAIN:
        case EG_RELEASE:
            op->env += op->env_inc;
            if (op->env >= ENV_QUIET)
            {
                op->env = ENV_MAX;
                op->state = EG_OFF;
                op->env_inc = 0;
            }
            break;
    }
}

static int OpCalc(opl2_t *chip, channel_t *ch, op_t *op, int mod)
{
    unsigned int p, level;
    int state = op->state, neg = 0, out;

    if (state == EG_OFF)
    {
        return 0;
    }
    StepEnvelope(op);
    if (op->state != state)
    {
        SetEnvInc(chip, ch, op);
    }

    p = ((op->phase >> 22) + mod) & 0x3ff;
    op->phase += op->inc;

    switch (op->wave)
    {
        case 0:
            neg = p & 0x200;
            level = logsin[(p & 0x100) ? (~p & 0xff) : (p & 0xff)];
            break;
        case 1:
            level = (p & 0x200) ? SILENT
                  : logsin[(p & 0x100) ? (~p & 0xff) : (p & 0xff)];
            break;
        case 2:
            level = logsin[(p & 0x100) ? (~p & 0xff) : (p & 0xff)];
            break;
        default:
            level = (p & 0x100) ? SILENT : logsin[p & 0xff];
            break;
    }

    {
        int env = (op->env >> ENV_FRAC) + op->att
                + ((op->reg20 & 0x80) ? chip->trem : 0);

        level += (env > 511 ? 511 : env) << 3;
    }
    if (level >= SILENT)
    {
        return 0;
    }
    out = (exptab[level & 0xff] << 1) >> (level >> 8);
    return neg ? -out : out;
}

static void StepLFO(opl2_t *chip)
{
    uint32_t t = chip->chip_time >> 16;
    int vibpos = (t >> 10) & 7, pos = (t >> 6) % 210, i;

    // tremolo: 210-step triangle every 64 chip samples (3.7 Hz),
    // 4.8 dB or 1 dB
    pos = pos < 105 ? pos : 210 - pos;
    chip->trem = pos >> (chip->dam ? 2 : 4);

    // vibrato: 8 steps every 1024 chip samples (6.1 Hz)
    if (vibpos != chip->vibpos)
    {
        chip->vibpos = vibpos;
        for (i = 0; i < 18; ++i)
        {
            op_t *op = Op(chip, i);

            if (op->reg20 & 0x40)
            {
                UpdateInc(chip, &chip->ch[i >> 1], op);
            }
        }
    }
}

void OPL2_Render(opl2_t *chip, int16_t *buf, int samples)
{
    int i, c;

    for (i = 0; i < samples; ++i)
    {
        int mix = 0;

        chip->chip_time += chip->chip_step;
        if ((chip->chip_time >> 22) != chip->lfo_tick)
        {
            chip->lfo_tick = chip->chip_time >> 22;
            StepLFO(chip);
        }
        for (c = 0; c < 9; ++c)
        {
            channel_t *ch = &chip->ch[c];
            op_t *m = &ch->op[0], *car = &ch->op[1];
            int fbmod = 0, mo;

            if (m->state == EG_OFF && car->state == EG_OFF)
            {
                continue;
            }
            if (ch->fb != 0)
            {
                fbmod = (m->prev + m->out) >> (9 - ch->fb);
            }
            mo = OpCalc(chip, ch, m, fbmod);
            m->prev = m->out;
            m->out = mo;
            if (ch->con)
            {
                mix += mo + OpCalc(chip, ch, car, 0);
            }
            else
            {
                mix += OpCalc(chip, ch, car, mo);
            }
        }
        buf[i] = mix > 32767 ? 32767 : mix < -32768 ? -32768 : mix;
    }
}

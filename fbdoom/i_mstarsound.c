//
// Sound effects for the MStar Kronus (MSD7818, WIWA HD95 set-top box):
// a software mixer (NUM_CHANNELS voices, 8-bit DMX lumps resampled to
// MIX_RATE stereo) running in its own thread and feeding the DEC DSP of the
// SoC through mstar_audio.h (stock DSP firmware, xPCM decoder, /dev/mem).
// Build with MSTAR_SOUND=1, see the Makefile and README.md.
//

#include <pthread.h>
#include <stdint.h>
#include <string.h>

#define MAUDIO_QUIET
#define MAUDIO_LEAD	12288		// ~64 ms of latency at 48 kHz stereo
#include "mstar_audio.h"

#include "deh_str.h"
#include "i_sound.h"
#include "i_system.h"
#include "m_misc.h"
#include "w_wad.h"
#include "z_zone.h"
#include "doomtype.h"

#define NUM_CHANNELS	8
#define MIX_RATE	48000
#define MIX_FRAMES	512		// frames per mixing block (~10.7 ms)

typedef struct
{
    const uint8_t *data;	// unsigned 8-bit samples, NULL = free
    uint32_t len;		// samples
    uint32_t pos;		// 16.16 fixed point position
    uint32_t step;		// 16.16 fixed point step per output frame
    int left, right;		// 0..255
} channel_t;

static channel_t channels[NUM_CHANNELS];
static pthread_mutex_t mix_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t mix_thread;
static volatile int mix_running;
static boolean use_sfx_prefix;
static boolean sound_initialized;

static void *MixThread(void *arg)
{
    static int16_t out[MIX_FRAMES * 2];
    int i, c;

    (void) arg;

    while (mix_running)
    {
        int32_t acc[MIX_FRAMES * 2];

        memset(acc, 0, sizeof(acc));
        pthread_mutex_lock(&mix_lock);
        for (c = 0; c < NUM_CHANNELS; ++c)
        {
            channel_t *ch = &channels[c];

            if (ch->data == NULL)
            {
                continue;
            }
            for (i = 0; i < MIX_FRAMES; ++i)
            {
                uint32_t idx = ch->pos >> 16;
                int s;

                if (idx >= ch->len)
                {
                    ch->data = NULL;
                    break;
                }
                s = ((int) ch->data[idx] - 128) << 8;		// 16-bit range
                acc[2 * i] += (s * ch->left) >> 8;
                acc[2 * i + 1] += (s * ch->right) >> 8;
                ch->pos += ch->step;
            }
        }
        pthread_mutex_unlock(&mix_lock);

        for (i = 0; i < MIX_FRAMES * 2; ++i)
        {
            int32_t v = acc[i] / 2;	// headroom for several voices

            out[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
        }
        maudio_write(out, MIX_FRAMES);	// blocks, paced to real time
    }
    return NULL;
}

static void GetSfxLumpName(sfxinfo_t *sfx, char *buf, size_t buf_len)
{
    if (sfx->link != NULL)
    {
        sfx = sfx->link;
    }
    if (use_sfx_prefix)
    {
        M_snprintf(buf, buf_len, "ds%s", DEH_String(sfx->name));
    }
    else
    {
        M_StringCopy(buf, DEH_String(sfx->name), buf_len);
    }
}

static int I_MStar_GetSfxLumpNum(sfxinfo_t *sfx)
{
    char namebuf[9];

    GetSfxLumpName(sfx, namebuf, sizeof(namebuf));
    return W_GetNumForName(namebuf);
}

static void SetParams(channel_t *ch, int vol, int sep)
{
    int left = ((254 - sep) * vol) / 127;
    int right = (sep * vol) / 127;

    ch->left = left < 0 ? 0 : left > 255 ? 255 : left;
    ch->right = right < 0 ? 0 : right > 255 ? 255 : right;
}

static void I_MStar_UpdateSoundParams(int handle, int vol, int sep)
{
    if (!sound_initialized || handle < 0 || handle >= NUM_CHANNELS)
    {
        return;
    }
    pthread_mutex_lock(&mix_lock);
    SetParams(&channels[handle], vol, sep);
    pthread_mutex_unlock(&mix_lock);
}

// DMX sound lump: u16 format (3), u16 rate, u32 length, then the samples;
// like DMX (and Chocolate Doom) skip the first and last 16 samples.
static int I_MStar_StartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep)
{
    const byte *data;
    unsigned int lumplen, length, rate;
    channel_t *ch;

    if (!sound_initialized || channel < 0 || channel >= NUM_CHANNELS)
    {
        return -1;
    }
    if (sfxinfo->lumpnum < 0)
    {
        sfxinfo->lumpnum = I_MStar_GetSfxLumpNum(sfxinfo);
    }
    data = W_CacheLumpNum(sfxinfo->lumpnum, PU_STATIC);	// kept cached
    lumplen = W_LumpLength(sfxinfo->lumpnum);
    if (lumplen < 8 || data[0] != 0x03 || data[1] != 0x00)
    {
        return -1;
    }
    rate = (data[3] << 8) | data[2];
    length = (data[7] << 24) | (data[6] << 16) | (data[5] << 8) | data[4];
    if (length > lumplen - 8 || length <= 48 || rate == 0)
    {
        return -1;
    }

    ch = &channels[channel];
    pthread_mutex_lock(&mix_lock);
    ch->data = data + 8 + 16;
    ch->len = length - 32;
    ch->pos = 0;
    ch->step = (uint32_t) (((uint64_t) rate << 16) / MIX_RATE);
    SetParams(ch, vol, sep);
    pthread_mutex_unlock(&mix_lock);
    return channel;
}

static void I_MStar_StopSound(int handle)
{
    if (!sound_initialized || handle < 0 || handle >= NUM_CHANNELS)
    {
        return;
    }
    pthread_mutex_lock(&mix_lock);
    channels[handle].data = NULL;
    pthread_mutex_unlock(&mix_lock);
}

static boolean I_MStar_SoundIsPlaying(int handle)
{
    if (!sound_initialized || handle < 0 || handle >= NUM_CHANNELS)
    {
        return false;
    }
    return channels[handle].data != NULL;
}

static void I_MStar_UpdateSound(void)
{
    // mixing happens in MixThread
}

static void I_MStar_PrecacheSounds(sfxinfo_t *sounds, int num_sounds)
{
    // lumps are cached on first use
}

static void I_MStar_ShutdownSound(void)
{
    if (!sound_initialized)
    {
        return;
    }
    mix_running = 0;
    pthread_join(mix_thread, NULL);
    maudio_close();
    sound_initialized = false;
}

static boolean I_MStar_InitSound(boolean _use_sfx_prefix)
{
    use_sfx_prefix = _use_sfx_prefix;
    memset(channels, 0, sizeof(channels));

    printf("I_MStar_InitSound: starting the DSPs (MStar xPCM, %d Hz)\n", MIX_RATE);
    if (!maudio_open(MIX_RATE, 2))
    {
        fprintf(stderr, "I_MStar_InitSound: audio init failed (root? /dev/mem?)\n");
        return false;
    }
    mix_running = 1;
    if (pthread_create(&mix_thread, NULL, MixThread, NULL) != 0)
    {
        maudio_close();
        return false;
    }
    sound_initialized = true;
    return true;
}

static snddevice_t sound_mstar_devices[] =
{
    SNDDEVICE_SB,
    SNDDEVICE_PAS,
    SNDDEVICE_GUS,
    SNDDEVICE_WAVEBLASTER,
    SNDDEVICE_SOUNDCANVAS,
    SNDDEVICE_AWE32,
};

sound_module_t sound_mstar_module =
{
    sound_mstar_devices,
    arrlen(sound_mstar_devices),
    I_MStar_InitSound,
    I_MStar_ShutdownSound,
    I_MStar_GetSfxLumpNum,
    I_MStar_UpdateSound,
    I_MStar_UpdateSoundParams,
    I_MStar_StartSound,
    I_MStar_StopSound,
    I_MStar_SoundIsPlaying,
    I_MStar_PrecacheSounds,
};

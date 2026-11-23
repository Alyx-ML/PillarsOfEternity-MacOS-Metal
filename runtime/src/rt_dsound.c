/*
 * DSOUND: DirectSound 8 (DirectSoundCreate, DirectSoundCreate8,
 * DirectSoundEnumerateW, IDirectSound8 and IDirectSoundBuffer8) on
 * CoreAudio.
 *
 * The emulated system has one playback device: the default output device
 * of macOS. DirectSoundEnumerateW reports "Primary Sound Driver" (NULL
 * GUID) and then the default device with the runtime GUID DEVICE_GUID.
 *
 * Secondary buffers: the buffer data is a guest heap block. Lock gives
 * pointers into that block, as on Windows (software buffers). Each
 * secondary buffer has its own default output audio unit with a float32
 * input in the sample rate of the buffer format. The render callback reads
 * the buffer data from guest memory, converts the samples to float32,
 * changes the rate for SetFrequency (linear interpolation), applies the
 * volume and the pan, and moves the play cursor. A looping buffer starts
 * again at offset 0 at the end. A buffer without looping stops at the end
 * and its play cursor goes to 0 (UNCONFIRMED: the cursor value of Windows
 * after the end).
 *
 * The play cursor is the offset of the next frame that the render callback
 * reads. The write cursor is the play cursor plus the frames of the last
 * render call, so data from the write cursor on is not read yet.
 *
 * The primary buffer keeps the format (SetFormat), the volume and the pan.
 * Its volume and pan apply to all buffers of the device. The primary format
 * does not change the output: each audio unit converts its buffer format
 * for the device. Lock and GetCurrentPosition of the primary buffer
 * (DSSCL_WRITEPRIMARY) are not supported: they stop the process.
 *
 * Differences from Windows:
 * - The runtime has no hardware buffers. DSBCAPS_LOCHARDWARE gives
 *   DSERR_INVALIDCALL (UNCONFIRMED: the error code of Windows).
 * - DSBCAPS_CTRL3D is accepted, but the runtime does not implement
 *   IDirectSound3DBuffer and IDirectSound3DListener: QueryInterface for
 *   them stops the process. Without these interfaces, a 3D buffer keeps the
 *   initial 3D state of Windows: the source is at the listener position, so
 *   the buffer plays centered without distance attenuation. FMOD in the
 *   player sets DSBCAPS_CTRL3D and does not query the 3D interfaces (the
 *   executable does not contain their interface IDs).
 * - Deferred buffers (DSBCAPS_LOCDEFER), effects (SetFX with effects) and
 *   IDirectSoundNotify are not supported: they stop the process with a
 *   message.
 * - The buffers are not muted when the application loses the focus.
 * - GetSpeakerConfig reports stereo (wide geometry).
 */
#include "rt.h"
#include "rt_com.h"
#include "rt_com_thunks.h"
#include "rt_thread.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define DS_OK 0u
#define DSERR_ALLOCATED 0x8878000Au
#define DSERR_CONTROLUNAVAIL 0x8878001Eu
#define DSERR_INVALIDPARAM 0x80070057u
#define DSERR_INVALIDCALL 0x88780032u
#define DSERR_PRIOLEVELNEEDED 0x88780046u
#define DSERR_OUTOFMEMORY 0x8007000Eu
#define DSERR_BADFORMAT 0x88780064u
#define DSERR_NODRIVER 0x88780078u
#define DSERR_ALREADYINITIALIZED 0x88780082u
#define DSERR_NOAGGREGATION 0x80040110u

#define DSSCL_NORMAL 1u
#define DSSCL_PRIORITY 2u
#define DSSCL_WRITEPRIMARY 4u

#define DSBCAPS_PRIMARYBUFFER 0x00000001u
#define DSBCAPS_STATIC 0x00000002u
#define DSBCAPS_LOCHARDWARE 0x00000004u
#define DSBCAPS_LOCSOFTWARE 0x00000008u
#define DSBCAPS_CTRL3D 0x00000010u
#define DSBCAPS_CTRLFREQUENCY 0x00000020u
#define DSBCAPS_CTRLPAN 0x00000040u
#define DSBCAPS_CTRLVOLUME 0x00000080u
#define DSBCAPS_CTRLPOSITIONNOTIFY 0x00000100u
#define DSBCAPS_CTRLFX 0x00000200u
#define DSBCAPS_STICKYFOCUS 0x00004000u
#define DSBCAPS_GLOBALFOCUS 0x00008000u
#define DSBCAPS_GETCURRENTPOSITION2 0x00010000u
#define DSBCAPS_MUTE3DATMAXDISTANCE 0x00020000u
#define DSBCAPS_LOCDEFER 0x00040000u
#define DSBCAPS_TRUEPLAYPOSITION 0x00080000u
#define DSBCAPS_KNOWN 0x000FC3FFu

#define DSBPLAY_LOOPING 0x00000001u
#define DSBLOCK_FROMWRITECURSOR 0x00000001u
#define DSBLOCK_ENTIREBUFFER 0x00000002u
#define DSBSTATUS_PLAYING 0x00000001u
#define DSBSTATUS_LOOPING 0x00000004u

#define DSBSIZE_MIN 4u
#define DSBSIZE_MAX 0x0FFFFFFFu
#define DSBFREQUENCY_MIN 100u
#define DSBFREQUENCY_MAX 200000u
#define DSBVOLUME_MIN (-10000)
#define DSBPAN_LEFT (-10000)
#define DSBPAN_RIGHT 10000

/* DSCAPS flags */
#define DSCAPS_PRIMARYMONO 0x00000001u
#define DSCAPS_PRIMARYSTEREO 0x00000002u
#define DSCAPS_PRIMARY8BIT 0x00000004u
#define DSCAPS_PRIMARY16BIT 0x00000008u
#define DSCAPS_CONTINUOUSRATE 0x00000010u
#define DSCAPS_SECONDARYMONO 0x00000100u
#define DSCAPS_SECONDARYSTEREO 0x00000200u
#define DSCAPS_SECONDARY8BIT 0x00000400u
#define DSCAPS_SECONDARY16BIT 0x00000800u

#define DSSPEAKER_STEREO 4u
#define DSSPEAKER_GEOMETRY_WIDE 20u
#define DS_UNCERTIFIED 1u

#define WAVE_FORMAT_PCM 1u
#define WAVE_FORMAT_IEEE_FLOAT 3u
#define WAVE_FORMAT_EXTENSIBLE 0xFFFEu
#define FMT_MAX 40u

static const rt_guid IID_IDirectSound = { 0x279AFA83u, 0x4981, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } };
static const rt_guid IID_IDirectSound8 = { 0xC50A7E93u, 0xF395, 0x4834, { 0x9E, 0xF6, 0x7F, 0xA9, 0x9D, 0xE5, 0x09, 0x66 } };
static const rt_guid IID_IDirectSoundBuffer = { 0x279AFA85u, 0x4981, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } };
static const rt_guid IID_IDirectSoundBuffer8 = { 0x6825A449u, 0x7524, 0x4D82, { 0x92, 0x0F, 0x50, 0xE3, 0x6A, 0xB3, 0xAB, 0x1E } };
static const rt_guid DSDEVID_DefaultPlayback = { 0xDEF00000u, 0x9C6D, 0x47ED, { 0xAA, 0xF1, 0x4D, 0xDA, 0x8F, 0x2B, 0x5C, 0x03 } };
static const rt_guid DSDEVID_DefaultVoicePlayback = { 0xDEF00002u, 0x9C6D, 0x47ED, { 0xAA, 0xF1, 0x4D, 0xDA, 0x8F, 0x2B, 0x5C, 0x03 } };
/* The runtime GUID of the default output device. */
static const rt_guid DEVICE_GUID = { 0x5D6B1C2Au, 0x7F3E, 0x4B8A, { 0x9C, 0x41, 0x2E, 0x6D, 0x8F, 0x10, 0xA3, 0x57 } };

typedef struct wfmt {
    uint32_t tag, channels, rate, bits, align, mask;
    bool is_float;
} wfmt;

/* Buffer data, shared by the duplicates of a buffer (DuplicateSoundBuffer). */
typedef struct dsmem {
    uint32_t guest, size;
    uint32_t refs; /* g_lock */
} dsmem;

typedef struct dsdev {
    uint32_t level;
    uint32_t speaker;
    /* the primary buffer */
    uint8_t pfmt[FMT_MAX];
    uint32_t pfmt_size;
    int32_t pvolume, ppan;
    bool pplaying;
} dsdev;

typedef struct dsbuf {
    rt_com_object *devobj; /* one reference */
    dsdev *dev;
    bool primary;
    uint32_t flags;
    /* secondary buffers */
    dsmem *mem;
    wfmt f;
    uint8_t fmt[FMT_MAX];
    uint32_t fmt_size;
    uint32_t out_channels;
    AudioComponentInstance unit;
    bool unit_started;
    /* g_lock */
    bool playing, looping;
    uint32_t frame, frac; /* play cursor: frame + frac / 2^32 */
    uint32_t freq;        /* 0: the rate of the format */
    int32_t volume, pan;
    uint32_t last_frames;
} dsbuf;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void dlock(void)
{
    if (pthread_mutex_lock(&g_lock) != 0) rt_fatal("DSOUND: lock failed");
}

static void dunlock(void)
{
    if (pthread_mutex_unlock(&g_lock) != 0) rt_fatal("DSOUND: unlock failed");
}

static void device_destroy(rt_com_object *o);
static void buffer_destroy(rt_com_object *o);

static const rt_com_iface device_ifaces[] = { { &IID_IDirectSound, 0 }, { &IID_IDirectSound8, 0 } };
static const rt_com_iface buffer_ifaces[] = { { &IID_IDirectSoundBuffer, 0 }, { &IID_IDirectSoundBuffer8, 0 } };
static const rt_com_class device_class = { "IDirectSound8", 1, { &rt_vt_DirectSound8 }, device_ifaces, 2, device_destroy };
static const rt_com_class buffer_class = { "IDirectSoundBuffer8", 1, { &rt_vt_DirectSoundBuffer8 }, buffer_ifaces, 2, buffer_destroy };

/* ---------------------------------------------------------------- device */

static AudioObjectID default_output(void)
{
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain };
    AudioObjectID id = kAudioObjectUnknown;
    UInt32 size = sizeof id;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, &id) != noErr) return kAudioObjectUnknown;
    return id;
}

/* The name of device id in UTF-16 (at most 255 characters). Returns the
   number of characters, or -1 when the device has no name. */
static int output_name(AudioObjectID id, uint16_t out[256])
{
    AudioObjectPropertyAddress a = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFStringRef name = NULL;
    UInt32 size = sizeof name;
    CFIndex n;
    if (AudioObjectGetPropertyData(id, &a, 0, NULL, &size, &name) != noErr || name == NULL) return -1;
    n = CFStringGetLength(name);
    if (n > 255) n = 255;
    CFStringGetCharacters(name, CFRangeMake(0, n), out);
    CFRelease(name);
    return (int)n;
}

/* ---------------------------------------------------------------- formats */

/* WAVEFORMATEX (or WAVEFORMATEXTENSIBLE) at g. Copies the structure to raw
   (*raw_size bytes). Returns DS_OK, DSERR_INVALIDPARAM or DSERR_BADFORMAT. */
static uint32_t read_format(uint32_t g, wfmt *f, uint8_t raw[FMT_MAX], uint32_t *raw_size)
{
    uint32_t valid;
    if (g == 0 || !rt_va_is_committed(g, 16)) return DSERR_INVALIDPARAM;
    f->tag = rd16(g);
    f->channels = rd16(g + 2);
    f->rate = rd32(g + 4);
    f->align = rd16(g + 12);
    f->bits = rd16(g + 14);
    f->mask = 0;
    f->is_float = false;
    *raw_size = 18;
    if (f->tag == WAVE_FORMAT_EXTENSIBLE)
    {
        /* cbSize 16, wValidBitsPerSample 18, dwChannelMask 20, SubFormat 24. */
        static const uint8_t pcm[16] = { 1, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71 };
        static const uint8_t flt[16] = { 3, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71 };
        if (!rt_va_is_committed(g, 40) || rd16(g + 16) < 22) return DSERR_BADFORMAT;
        valid = rd16(g + 18);
        f->mask = rd32(g + 20);
        if (memcmp(G2H(g + 24), pcm, 16) == 0)
            f->is_float = false;
        else if (memcmp(G2H(g + 24), flt, 16) == 0)
            f->is_float = true;
        else
            return DSERR_BADFORMAT;
        if (valid != 0 && valid != f->bits) return DSERR_BADFORMAT;
        *raw_size = 40;
    }
    else if (f->tag == WAVE_FORMAT_IEEE_FLOAT)
        f->is_float = true;
    else if (f->tag != WAVE_FORMAT_PCM)
        return DSERR_BADFORMAT;
    if (f->is_float ? f->bits != 32 : (f->bits != 8 && f->bits != 16 && f->bits != 24 && f->bits != 32)) return DSERR_BADFORMAT;
    if (f->channels == 0 || f->channels > 8u) return DSERR_BADFORMAT;
    if (f->rate < DSBFREQUENCY_MIN || f->rate > DSBFREQUENCY_MAX) return DSERR_BADFORMAT;
    if (f->align != f->channels * f->bits / 8u) return DSERR_BADFORMAT;
    if (rd32(g + 8) != f->rate * f->align) return DSERR_BADFORMAT;
    if (f->mask != 0 && (uint32_t)__builtin_popcount(f->mask) != f->channels) return DSERR_BADFORMAT;
    memcpy(raw, G2H(g), *raw_size);
    if (*raw_size == 18) memset(raw + 16, 0, 2); /* cbSize: no extra bytes */
    return DS_OK;
}

/* The Windows default speaker positions for a format without a mask. */
static uint32_t default_mask(uint32_t channels)
{
    switch (channels)
    {
    case 1: return 0x4;
    case 2: return 0x3;
    case 3: return 0x7;
    case 4: return 0x33;
    case 5: return 0x37;
    case 6: return 0x3F;
    case 7: return 0x13F;
    default: return 0x63F;
    }
}

static float sample_at(const uint8_t *p, const wfmt *f)
{
    switch (f->bits)
    {
    case 8: return ((float)p[0] - 128.0f) * (1.0f / 128.0f);
    case 16:
    {
        int16_t v;
        memcpy(&v, p, 2);
        return (float)v * (1.0f / 32768.0f);
    }
    case 24:
    {
        int32_t v = (int32_t)(((uint32_t)p[0] << 8) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 24)) >> 8;
        return (float)v * (1.0f / 8388608.0f);
    }
    default:
        if (f->is_float)
        {
            float v;
            memcpy(&v, p, 4);
            return v;
        }
        else
        {
            int32_t v;
            memcpy(&v, p, 4);
            return (float)v * (1.0f / 2147483648.0f);
        }
    }
}

/* ---------------------------------------------------------------- render */

static float gain_of(int32_t millibels)
{
    if (millibels <= DSBVOLUME_MIN) return 0.0f;
    return powf(10.0f, (float)millibels / 2000.0f);
}

static atomic_bool muted;

void rt_audio_set_muted(void)
{
    atomic_store(&muted, true);
}

bool rt_audio_muted(void)
{
    return atomic_load(&muted);
}

static OSStatus render(void *ctx, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames,
                       AudioBufferList *io)
{
    dsbuf *b = ctx;
    float *out = io->mBuffers[0].mData;
    uint32_t oc = b->out_channels, ch = b->f.channels, align = b->f.align, n = frames, i, c;
    uint32_t nframes = b->mem->size / align, rate;
    const uint8_t *data = G2H(b->mem->guest);
    float g, gl, gr;
    uint64_t step;
    int32_t pan;
    (void)flags;
    (void)ts;
    (void)bus;
    if (io->mBuffers[0].mDataByteSize < n * oc * 4u) n = io->mBuffers[0].mDataByteSize / (oc * 4u);
    dlock();
    b->last_frames = n;
    if (!b->playing)
    {
        dunlock();
        memset(out, 0, (size_t)n * oc * 4u);
        return noErr;
    }
    g = gain_of(b->volume + b->dev->pvolume);
    pan = b->pan + b->dev->ppan;
    if (pan < DSBPAN_LEFT) pan = DSBPAN_LEFT;
    if (pan > DSBPAN_RIGHT) pan = DSBPAN_RIGHT;
    gl = g * (pan > 0 ? gain_of(-pan) : 1.0f);
    gr = g * (pan < 0 ? gain_of(pan) : 1.0f);
    rate = b->freq != 0 ? b->freq : b->f.rate;
    step = ((uint64_t)rate << 32) / b->f.rate;
    for (i = 0; i < n; i++)
    {
        uint32_t f0 = b->frame, f1 = f0 + 1u;
        float t = (float)b->frac * (1.0f / 4294967296.0f), s[8];
        uint64_t p;
        if (!b->playing)
        {
            memset(out + (size_t)i * oc, 0, (size_t)(n - i) * oc * 4u);
            break;
        }
        if (f1 >= nframes) f1 = b->looping ? 0u : f0;
        for (c = 0; c < ch; c++)
        {
            float s0 = sample_at(data + (size_t)f0 * align + c * (b->f.bits / 8u), &b->f);
            if (b->frac != 0)
            {
                float s1 = sample_at(data + (size_t)f1 * align + c * (b->f.bits / 8u), &b->f);
                s0 += (s1 - s0) * t;
            }
            s[c] = s0;
        }
        if (ch == 1)
        {
            out[(size_t)i * 2u] = s[0] * gl;
            out[(size_t)i * 2u + 1u] = s[0] * gr;
        }
        else if (ch == 2)
        {
            out[(size_t)i * 2u] = s[0] * gl;
            out[(size_t)i * 2u + 1u] = s[1] * gr;
        }
        else
            for (c = 0; c < ch; c++) out[(size_t)i * oc + c] = s[c] * g;
        p = (uint64_t)b->frac + step;
        b->frame += (uint32_t)(p >> 32);
        b->frac = (uint32_t)p;
        if (b->frame >= nframes)
        {
            if (b->looping)
                b->frame %= nframes;
            else
            {
                b->playing = false;
                b->frame = 0;
                b->frac = 0;
            }
        }
    }
    dunlock();
    if (atomic_load(&muted)) memset(out, 0, (size_t)n * oc * 4u);
    return noErr;
}

/* The audio unit of a secondary buffer. Returns DS_OK, DSERR_NODRIVER or
   DSERR_BADFORMAT. */
static uint32_t open_unit(dsbuf *b)
{
    AudioComponentDescription desc = { kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0 };
    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    AudioStreamBasicDescription asbd;
    AURenderCallbackStruct cb = { render, b };
    AudioChannelLayout layout;
    OSStatus st;
    if (comp == NULL) return DSERR_NODRIVER;
    if (AudioComponentInstanceNew(comp, &b->unit) != noErr) return DSERR_NODRIVER;
    b->out_channels = b->f.channels == 1 ? 2u : b->f.channels;
    memset(&asbd, 0, sizeof asbd);
    asbd.mSampleRate = b->f.rate;
    asbd.mFormatID = kAudioFormatLinearPCM;
    asbd.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    asbd.mBytesPerPacket = b->out_channels * 4u;
    asbd.mFramesPerPacket = 1;
    asbd.mBytesPerFrame = b->out_channels * 4u;
    asbd.mChannelsPerFrame = b->out_channels;
    asbd.mBitsPerChannel = 32;
    st = AudioUnitSetProperty(b->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &asbd, sizeof asbd);
    if (st == noErr && b->out_channels > 2)
    {
        memset(&layout, 0, sizeof layout);
        layout.mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelBitmap;
        layout.mChannelBitmap = b->f.mask != 0 ? b->f.mask : default_mask(b->f.channels);
        st = AudioUnitSetProperty(b->unit, kAudioUnitProperty_AudioChannelLayout, kAudioUnitScope_Input, 0, &layout, sizeof layout);
    }
    if (st == noErr) st = AudioUnitSetProperty(b->unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
    if (st == noErr) st = AudioUnitInitialize(b->unit);
    if (st != noErr)
    {
        rt_log("DSOUND: the audio unit does not accept the format (%u Hz, %u channels): OSStatus %d", b->f.rate, b->f.channels, (int)st);
        AudioComponentInstanceDispose(b->unit);
        b->unit = NULL;
        return DSERR_BADFORMAT;
    }
    return DS_OK;
}

/* ---------------------------------------------------------------- objects */

static void mem_release(dsmem *m)
{
    bool last;
    dlock();
    last = --m->refs == 0;
    dunlock();
    if (!last) return;
    rt_heap_free(m->guest);
    free(m);
}

static void device_destroy(rt_com_object *o)
{
    free(o->data);
}

static void buffer_destroy(rt_com_object *o)
{
    dsbuf *b = o->data;
    if (b->unit != NULL)
    {
        if (b->unit_started && AudioOutputUnitStop(b->unit) != noErr) rt_log("DSOUND: AudioOutputUnitStop failed");
        if (AudioUnitUninitialize(b->unit) != noErr) rt_log("DSOUND: AudioUnitUninitialize failed");
        if (AudioComponentInstanceDispose(b->unit) != noErr) rt_log("DSOUND: AudioComponentInstanceDispose failed");
    }
    if (b->mem != NULL) mem_release(b->mem);
    rt_com_release(b->devobj);
    free(b);
}

static dsdev *dev_this(_cpu *cpu)
{
    return rt_com_this(ARG(0), 0, &device_class)->data;
}

static dsbuf *buf_this(_cpu *cpu)
{
    return rt_com_this(ARG(0), 0, &buffer_class)->data;
}

/* A new buffer object of device devobj. On success, writes the interface
   pointer to ppbuf. b becomes owned by the object. Returns DS_OK or
   DSERR_OUTOFMEMORY (then b and its parts are released). */
static uint32_t publish_buffer(rt_com_object *devobj, dsbuf *b, uint32_t ppbuf)
{
    rt_com_object *o;
    rt_com_addref(devobj);
    b->devobj = devobj;
    b->dev = devobj->data;
    o = rt_com_create(&buffer_class, b);
    if (o == NULL)
    {
        if (b->unit != NULL) AudioComponentInstanceDispose(b->unit);
        if (b->mem != NULL) mem_release(b->mem);
        rt_com_release(devobj);
        free(b);
        return DSERR_OUTOFMEMORY;
    }
    wr32(ppbuf, rt_com_ptr(o, 0));
    return DS_OK;
}

/* ---------------------------------------------------------------- DirectSoundCreate */

static void create_device(_cpu *cpu, const char *fn)
{
    uint32_t gp = ARG(0), ppds = ARG(1), outer = ARG(2);
    rt_com_object *o;
    dsdev *d;
    if (ppds == 0)
    {
        RET(DSERR_INVALIDPARAM);
        return;
    }
    wr32(ppds, 0);
    if (outer != 0)
    {
        RET(DSERR_NOAGGREGATION);
        return;
    }
    if (gp != 0)
    {
        rt_guid g = rt_guid_read(gp);
        if (!rt_guid_eq(&g, &DSDEVID_DefaultPlayback) && !rt_guid_eq(&g, &DSDEVID_DefaultVoicePlayback) && !rt_guid_eq(&g, &DEVICE_GUID))
        {
            char text[39];
            rt_guid_format(&g, text);
            rt_log("DSOUND: %s for device %s, which the emulated system does not have", fn, text);
            RET(DSERR_NODRIVER);
            return;
        }
    }
    if (default_output() == kAudioObjectUnknown)
    {
        rt_log("DSOUND: %s: macOS has no default output device", fn);
        RET(DSERR_NODRIVER);
        return;
    }
    d = calloc(1, sizeof *d);
    if (d == NULL) rt_fatal("out of host memory");
    d->speaker = (DSSPEAKER_GEOMETRY_WIDE << 16) | DSSPEAKER_STEREO;
    /* The initial primary format: PCM, 22050 Hz, 8 bits, stereo
       (UNCONFIRMED: the initial format of Windows). */
    d->pfmt_size = 18;
    {
        uint8_t *p = d->pfmt;
        uint16_t tag = WAVE_FORMAT_PCM, ch = 2, align = 2, bits = 8, cb = 0;
        uint32_t rate = 22050, avg = 44100;
        memcpy(p, &tag, 2);
        memcpy(p + 2, &ch, 2);
        memcpy(p + 4, &rate, 4);
        memcpy(p + 8, &avg, 4);
        memcpy(p + 12, &align, 2);
        memcpy(p + 14, &bits, 2);
        memcpy(p + 16, &cb, 2);
    }
    o = rt_com_create(&device_class, d);
    if (o == NULL)
    {
        free(d);
        RET(DSERR_OUTOFMEMORY);
        return;
    }
    rt_log("DSOUND: %s: device object created", fn);
    wr32(ppds, rt_com_ptr(o, 0));
    RET(DS_OK);
}

/* HRESULT DirectSoundCreate(LPCGUID device, LPDIRECTSOUND *ds, LPUNKNOWN outer) */
void rt_DirectSoundCreate(_cpu *cpu)
{
    create_device(cpu, "DirectSoundCreate");
}

/* HRESULT DirectSoundCreate8(LPCGUID device, LPDIRECTSOUND8 *ds, LPUNKNOWN outer) */
void rt_DirectSoundCreate8(_cpu *cpu)
{
    create_device(cpu, "DirectSoundCreate8");
}

/* HRESULT DirectSoundEnumerateW(LPDSENUMCALLBACKW cb, LPVOID context)
   BOOL CALLBACK cb(LPGUID guid, LPCWSTR description, LPCWSTR module, LPVOID context) */
void rt_DirectSoundEnumerateW(_cpu *cpu)
{
    static const char primary[] = "Primary Sound Driver";
    uint32_t cb = ARG(0), ctx = ARG(1), blk, guid, s_primary, s_name, s_empty, i;
    uint16_t name[256];
    AudioObjectID id;
    int n;
    if (cb == 0)
    {
        RET(DSERR_INVALIDPARAM);
        return;
    }
    id = default_output();
    n = id == kAudioObjectUnknown ? -1 : output_name(id, name);
    if (id != kAudioObjectUnknown && n < 0)
        rt_fatal("DSOUND: the default output device of macOS has no name (kAudioObjectPropertyName failed)");
    /* GUID, "Primary Sound Driver", the device name, "" */
    blk = rt_heap_calloc(1, 16u + 2u * (uint32_t)sizeof primary + 2u * 256u + 2u);
    if (blk == 0)
    {
        RET(DSERR_OUTOFMEMORY);
        return;
    }
    guid = blk;
    s_primary = blk + 16u;
    s_name = s_primary + 2u * (uint32_t)sizeof primary;
    s_empty = s_name + 2u * 256u;
    rt_guid_write(guid, &DEVICE_GUID);
    for (i = 0; i < sizeof primary; i++) wr16(s_primary + 2u * i, (uint8_t)primary[i]);
    for (i = 0; n > 0 && i < (uint32_t)n; i++) wr16(s_name + 2u * i, name[i]);
    if (id != kAudioObjectUnknown && rt_call_cb(cpu, cb, 4, 0, s_primary, s_empty, ctx) != 0)
        (void)rt_call_cb(cpu, cb, 4, guid, s_name, s_empty, ctx);
    rt_heap_free(blk);
    RET(DS_OK);
}

/* ---------------------------------------------------------------- IDirectSound8 */

/* HRESULT CreateSoundBuffer(this, LPCDSBUFFERDESC desc, LPDIRECTSOUNDBUFFER *buf, LPUNKNOWN outer) */
void rt_com_DirectSound8_CreateSoundBuffer(_cpu *cpu)
{
    uint32_t desc = ARG(1), ppbuf = ARG(2), outer = ARG(3), size, flags, bytes, fmt, r;
    rt_com_object *devobj = rt_com_this(ARG(0), 0, &device_class);
    dsbuf *b;
    if (desc == 0 || ppbuf == 0 || !rt_va_is_committed(desc, 20))
    {
        RET(DSERR_INVALIDPARAM);
        return;
    }
    wr32(ppbuf, 0);
    if (outer != 0)
    {
        RET(DSERR_NOAGGREGATION);
        return;
    }
    size = rd32(desc);
    flags = rd32(desc + 4);
    bytes = rd32(desc + 8);
    fmt = rd32(desc + 16);
    if ((size != 20 && size != 36) || (flags & ~DSBCAPS_KNOWN) != 0 ||
        (flags & (DSBCAPS_LOCHARDWARE | DSBCAPS_LOCSOFTWARE)) == (DSBCAPS_LOCHARDWARE | DSBCAPS_LOCSOFTWARE))
    {
        RET(DSERR_INVALIDPARAM);
        return;
    }
    /* A 3D buffer cannot have a pan control (DirectSound documentation). */
    if ((flags & (DSBCAPS_CTRL3D | DSBCAPS_CTRLPAN)) == (DSBCAPS_CTRL3D | DSBCAPS_CTRLPAN))
    {
        RET(DSERR_INVALIDPARAM);
        return;
    }
    if (flags & DSBCAPS_LOCDEFER) rt_fatal("DSOUND: CreateSoundBuffer with DSBCAPS_LOCDEFER (not supported by the runtime)");
    if (flags & DSBCAPS_LOCHARDWARE)
    {
        rt_log("DSOUND: CreateSoundBuffer with DSBCAPS_LOCHARDWARE: the emulated device has no hardware buffers");
        RET(DSERR_INVALIDCALL);
        return;
    }
    b = calloc(1, sizeof *b);
    if (b == NULL) rt_fatal("out of host memory");
    b->flags = flags;
    if (flags & DSBCAPS_PRIMARYBUFFER)
    {
        if (bytes != 0 || fmt != 0 || (flags & (DSBCAPS_CTRLFREQUENCY | DSBCAPS_CTRLFX | DSBCAPS_CTRLPOSITIONNOTIFY | DSBCAPS_STATIC)) != 0)
        {
            free(b);
            RET(DSERR_INVALIDPARAM);
            return;
        }
        b->primary = true;
        r = publish_buffer(devobj, b, ppbuf);
        if (r == DS_OK) rt_log("DSOUND: primary buffer, flags 0x%08x", flags);
        RET(r);
        return;
    }
    if (bytes < DSBSIZE_MIN || bytes > DSBSIZE_MAX)
    {
        free(b);
        RET(DSERR_INVALIDPARAM);
        return;
    }
    r = read_format(fmt, &b->f, b->fmt, &b->fmt_size);
    if (r == DS_OK && bytes < b->f.align) r = DSERR_INVALIDPARAM;
    /* A 3D buffer must be mono (DirectSound documentation; UNCONFIRMED:
       the error code of Windows). */
    if (r == DS_OK && (flags & DSBCAPS_CTRL3D) && b->f.channels != 1) r = DSERR_INVALIDPARAM;
    if (r != DS_OK)
    {
        free(b);
        RET(r);
        return;
    }
    b->mem = calloc(1, sizeof *b->mem);
    if (b->mem == NULL) rt_fatal("out of host memory");
    b->mem->size = bytes;
    b->mem->refs = 1;
    /* 8-bit PCM silence is 0x80; the other formats use 0. */
    b->mem->guest = rt_heap_calloc(1, bytes);
    if (b->mem->guest == 0)
    {
        free(b->mem);
        free(b);
        RET(DSERR_OUTOFMEMORY);
        return;
    }
    if (b->f.bits == 8) memset(G2H(b->mem->guest), 0x80, bytes);
    r = open_unit(b);
    if (r != DS_OK)
    {
        mem_release(b->mem);
        free(b);
        RET(r);
        return;
    }
    r = publish_buffer(devobj, b, ppbuf);
    if (r == DS_OK)
        rt_log("DSOUND: secondary buffer of %u bytes: %u Hz, %u bits%s, %u channels, flags 0x%08x", bytes, b->f.rate, b->f.bits,
               b->f.is_float ? " float" : "", b->f.channels, flags);
    RET(r);
}

/* HRESULT GetCaps(this, LPDSCAPS caps) */
void rt_com_DirectSound8_GetCaps(_cpu *cpu)
{
    uint32_t caps = ARG(1), i;
    (void)dev_this(cpu);
    if (caps == 0 || !rt_va_is_committed(caps, 96) || rd32(caps) != 96)
    {
        RET(DSERR_INVALIDPARAM);
        return;
    }
    for (i = 4; i < 96; i += 4) wr32(caps + i, 0);
    wr32(caps + 4, DSCAPS_PRIMARYMONO | DSCAPS_PRIMARYSTEREO | DSCAPS_PRIMARY8BIT | DSCAPS_PRIMARY16BIT | DSCAPS_CONTINUOUSRATE |
                       DSCAPS_SECONDARYMONO | DSCAPS_SECONDARYSTEREO | DSCAPS_SECONDARY8BIT | DSCAPS_SECONDARY16BIT);
    wr32(caps + 8, DSBFREQUENCY_MIN);
    wr32(caps + 12, DSBFREQUENCY_MAX);
    wr32(caps + 16, 1); /* dwPrimaryBuffers */
    RET(DS_OK);
}

/* HRESULT DuplicateSoundBuffer(this, LPDIRECTSOUNDBUFFER orig, LPDIRECTSOUNDBUFFER *dup) */
void rt_com_DirectSound8_DuplicateSoundBuffer(_cpu *cpu)
{
    uint32_t orig = ARG(1), ppdup = ARG(2), r;
    rt_com_object *devobj = rt_com_this(ARG(0), 0, &device_class);
    dsbuf *s, *b;
    if (orig == 0 || ppdup == 0)
    {
        RET(DSERR_INVALIDPARAM);
        return;
    }
    wr32(ppdup, 0);
    s = rt_com_this(orig, 0, &buffer_class)->data;
    if (s->primary)
    {
        RET(DSERR_INVALIDCALL);
        return;
    }
    b = calloc(1, sizeof *b);
    if (b == NULL) rt_fatal("out of host memory");
    dlock();
    b->flags = s->flags;
    b->f = s->f;
    memcpy(b->fmt, s->fmt, sizeof b->fmt);
    b->fmt_size = s->fmt_size;
    b->freq = s->freq;
    b->volume = s->volume;
    b->pan = s->pan;
    b->mem = s->mem;
    b->mem->refs++;
    dunlock();
    r = open_unit(b);
    if (r != DS_OK)
    {
        mem_release(b->mem);
        free(b);
        RET(r);
        return;
    }
    RET(publish_buffer(devobj, b, ppdup));
}

/* HRESULT SetCooperativeLevel(this, HWND hwnd, DWORD level) */
void rt_com_DirectSound8_SetCooperativeLevel(_cpu *cpu)
{
    uint32_t level = ARG(2);
    dsdev *d = dev_this(cpu);
    if (level < DSSCL_NORMAL || level > DSSCL_WRITEPRIMARY)
    {
        RET(DSERR_INVALIDPARAM);
        return;
    }
    dlock();
    d->level = level;
    dunlock();
    RET(DS_OK);
}

/* HRESULT Compact(this) */
void rt_com_DirectSound8_Compact(_cpu *cpu)
{
    dsdev *d = dev_this(cpu);
    RET(d->level >= DSSCL_PRIORITY ? DS_OK : DSERR_PRIOLEVELNEEDED);
}

/* HRESULT GetSpeakerConfig(this, LPDWORD config) */
void rt_com_DirectSound8_GetSpeakerConfig(_cpu *cpu)
{
    uint32_t p = ARG(1);
    dsdev *d = dev_this(cpu);
    if (p == 0)
    {
        RET(DSERR_INVALIDPARAM);
        return;
    }
    wr32(p, d->speaker);
    RET(DS_OK);
}

/* HRESULT SetSpeakerConfig(this, DWORD config) */
void rt_com_DirectSound8_SetSpeakerConfig(_cpu *cpu)
{
    dsdev *d = dev_this(cpu);
    d->speaker = ARG(1);
    RET(DS_OK);
}

/* HRESULT Initialize(this, LPCGUID device) */
void rt_com_DirectSound8_Initialize(_cpu *cpu)
{
    (void)dev_this(cpu);
    RET(DSERR_ALREADYINITIALIZED);
}

/* HRESULT VerifyCertification(this, LPDWORD certified) */
void rt_com_DirectSound8_VerifyCertification(_cpu *cpu)
{
    uint32_t p = ARG(1);
    (void)dev_this(cpu);
    if (p == 0)
    {
        RET(DSERR_INVALIDPARAM);
        return;
    }
    wr32(p, DS_UNCERTIFIED);
    RET(DS_OK);
}

/* ---------------------------------------------------------------- IDirectSoundBuffer8 */

/* HRESULT GetCaps(this, LPDSBCAPS caps) */
void rt_com_DirectSoundBuffer8_GetCaps(_cpu *cpu)
{
    uint32_t caps = ARG(1), bytes;
    dsbuf *b = buf_this(cpu);
    if (caps == 0 || !rt_va_is_committed(caps, 20) || rd32(caps) != 20)
    {
        RET(DSERR_INVALIDPARAM);
        return;
    }
    if (b->primary)
    {
        uint32_t rate, align;
        dlock();
        memcpy(&rate, b->dev->pfmt + 4, 4);
        align = (uint32_t)b->dev->pfmt[12] | ((uint32_t)b->dev->pfmt[13] << 8);
        dunlock();
        bytes = rate * align / 10u; /* 100 ms (UNCONFIRMED: the primary size of Windows) */
    }
    else
        bytes = b->mem->size;
    wr32(caps + 4, b->flags | (b->primary ? 0u : DSBCAPS_LOCSOFTWARE));
    wr32(caps + 8, bytes);
    wr32(caps + 12, 0);
    wr32(caps + 16, 0);
    RET(DS_OK);
}

/* HRESULT GetCurrentPosition(this, LPDWORD play, LPDWORD write) */
void rt_com_DirectSoundBuffer8_GetCurrentPosition(_cpu *cpu)
{
    uint32_t pplay = ARG(1), pwrite = ARG(2), play, write;
    dsbuf *b = buf_this(cpu);
    if (b->primary) rt_fatal("DSOUND: GetCurrentPosition of the primary buffer (DSSCL_WRITEPRIMARY is not supported by the runtime)");
    dlock();
    play = b->frame * b->f.align;
    write = play;
    if (b->playing)
    {
        uint32_t rate = b->freq != 0 ? b->freq : b->f.rate;
        uint64_t ahead = ((uint64_t)b->last_frames * rate + b->f.rate - 1u) / b->f.rate;
        write = (uint32_t)(((uint64_t)play + ahead * b->f.align) % b->mem->size);
    }
    dunlock();
    if (pplay != 0) wr32(pplay, play);
    if (pwrite != 0) wr32(pwrite, write);
    RET(DS_OK);
}

/* HRESULT GetFormat(this, LPWAVEFORMATEX fmt, DWORD size, LPDWORD written) */
void rt_com_DirectSoundBuffer8_GetFormat(_cpu *cpu)
{
    uint32_t pfmt = ARG(1), size = ARG(2), pwritten = ARG(3), n;
    uint8_t raw[FMT_MAX];
    dsbuf *b = buf_this(cpu);
    dlock();
    if (b->primary)
    {
        n = b->dev->pfmt_size;
        memcpy(raw, b->dev->pfmt, n);
    }
    else
    {
        n = b->fmt_size;
        memcpy(raw, b->fmt, n);
    }
    dunlock();
    if (pfmt == 0)
    {
        if (pwritten == 0)
        {
            RET(DSERR_INVALIDPARAM);
            return;
        }
        wr32(pwritten, n);
        RET(DS_OK);
        return;
    }
    if (size < n)
    {
        RET(DSERR_INVALIDPARAM);
        return;
    }
    memcpy(G2H(pfmt), raw, n);
    if (pwritten != 0) wr32(pwritten, n);
    RET(DS_OK);
}

/* HRESULT GetVolume(this, LPLONG volume) */
void rt_com_DirectSoundBuffer8_GetVolume(_cpu *cpu)
{
    uint32_t p = ARG(1);
    dsbuf *b = buf_this(cpu);
    if (p == 0)
    {
        RET(DSERR_INVALIDPARAM);
        return;
    }
    if (!(b->flags & DSBCAPS_CTRLVOLUME))
    {
        RET(DSERR_CONTROLUNAVAIL);
        return;
    }
    dlock();
    wr32(p, (uint32_t)(b->primary ? b->dev->pvolume : b->volume));
    dunlock();
    RET(DS_OK);
}

/* HRESULT GetPan(this, LPLONG pan) */
void rt_com_DirectSoundBuffer8_GetPan(_cpu *cpu)
{
    uint32_t p = ARG(1);
    dsbuf *b = buf_this(cpu);
    if (p == 0)
    {
        RET(DSERR_INVALIDPARAM);
        return;
    }
    if (!(b->flags & DSBCAPS_CTRLPAN))
    {
        RET(DSERR_CONTROLUNAVAIL);
        return;
    }
    dlock();
    wr32(p, (uint32_t)(b->primary ? b->dev->ppan : b->pan));
    dunlock();
    RET(DS_OK);
}

/* HRESULT GetFrequency(this, LPDWORD frequency) */
void rt_com_DirectSoundBuffer8_GetFrequency(_cpu *cpu)
{
    uint32_t p = ARG(1);
    dsbuf *b = buf_this(cpu);
    if (p == 0)
    {
        RET(DSERR_INVALIDPARAM);
        return;
    }
    if (b->primary || !(b->flags & DSBCAPS_CTRLFREQUENCY))
    {
        RET(DSERR_CONTROLUNAVAIL);
        return;
    }
    dlock();
    wr32(p, b->freq != 0 ? b->freq : b->f.rate);
    dunlock();
    RET(DS_OK);
}

/* HRESULT GetStatus(this, LPDWORD status) */
void rt_com_DirectSoundBuffer8_GetStatus(_cpu *cpu)
{
    uint32_t p = ARG(1), s = 0;
    dsbuf *b = buf_this(cpu);
    if (p == 0)
    {
        RET(DSERR_INVALIDPARAM);
        return;
    }
    dlock();
    if (b->primary)
        s = b->dev->pplaying ? DSBSTATUS_PLAYING | DSBSTATUS_LOOPING : 0u;
    else if (b->playing)
        s = DSBSTATUS_PLAYING | (b->looping ? DSBSTATUS_LOOPING : 0u);
    dunlock();
    wr32(p, s);
    RET(DS_OK);
}

/* HRESULT Initialize(this, LPDIRECTSOUND ds, LPCDSBUFFERDESC desc) */
void rt_com_DirectSoundBuffer8_Initialize(_cpu *cpu)
{
    (void)buf_this(cpu);
    RET(DSERR_ALREADYINITIALIZED);
}

/* HRESULT Lock(this, DWORD offset, DWORD bytes, LPVOID *p1, LPDWORD n1, LPVOID *p2, LPDWORD n2, DWORD flags) */
void rt_com_DirectSoundBuffer8_Lock(_cpu *cpu)
{
    uint32_t offset = ARG(1), bytes = ARG(2), pp1 = ARG(3), pn1 = ARG(4), pp2 = ARG(5), pn2 = ARG(6), flags = ARG(7);
    uint32_t size, n1, n2;
    dsbuf *b = buf_this(cpu);
    if (b->primary) rt_fatal("DSOUND: Lock of the primary buffer (DSSCL_WRITEPRIMARY is not supported by the runtime)");
    size = b->mem->size;
    if (flags & DSBLOCK_ENTIREBUFFER) bytes = size;
    if (flags & DSBLOCK_FROMWRITECURSOR)
    {
        uint32_t w;
        dlock();
        w = b->frame * b->f.align;
        if (b->playing)
        {
            uint32_t rate = b->freq != 0 ? b->freq : b->f.rate;
            uint64_t ahead = ((uint64_t)b->last_frames * rate + b->f.rate - 1u) / b->f.rate;
            w = (uint32_t)(((uint64_t)w + ahead * b->f.align) % size);
        }
        dunlock();
        offset = w;
    }
    if (pp1 == 0 || pn1 == 0 || offset >= size || bytes == 0 || bytes > size || (pp2 != 0 && pn2 == 0))
    {
        RET(DSERR_INVALIDPARAM);
        return;
    }
    n1 = size - offset;
    if (n1 > bytes) n1 = bytes;
    n2 = bytes - n1;
    if (n2 != 0 && pp2 == 0)
    {
        RET(DSERR_INVALIDPARAM);
        return;
    }
    wr32(pp1, b->mem->guest + offset);
    wr32(pn1, n1);
    if (pp2 != 0)
    {
        wr32(pp2, n2 != 0 ? b->mem->guest : 0u);
        wr32(pn2, n2);
    }
    RET(DS_OK);
}

/* HRESULT Play(this, DWORD reserved, DWORD priority, DWORD flags) */
void rt_com_DirectSoundBuffer8_Play(_cpu *cpu)
{
    uint32_t reserved = ARG(1), flags = ARG(3);
    dsbuf *b = buf_this(cpu);
    bool start;
    if (reserved != 0 || (flags & ~DSBPLAY_LOOPING) != 0)
    {
        RET(DSERR_INVALIDPARAM);
        return;
    }
    if (b->primary)
    {
        if (!(flags & DSBPLAY_LOOPING))
        {
            RET(DSERR_INVALIDPARAM);
            return;
        }
        dlock();
        b->dev->pplaying = true;
        dunlock();
        RET(DS_OK);
        return;
    }
    dlock();
    b->playing = true;
    b->looping = (flags & DSBPLAY_LOOPING) != 0;
    start = !b->unit_started;
    b->unit_started = true;
    dunlock();
    if (start)
    {
        OSStatus st = AudioOutputUnitStart(b->unit);
        if (st != noErr) rt_fatal("DSOUND: AudioOutputUnitStart failed (OSStatus %d)", (int)st);
    }
    RET(DS_OK);
}

/* HRESULT SetCurrentPosition(this, DWORD position) */
void rt_com_DirectSoundBuffer8_SetCurrentPosition(_cpu *cpu)
{
    uint32_t pos = ARG(1);
    dsbuf *b = buf_this(cpu);
    if (b->primary)
    {
        RET(DSERR_INVALIDCALL);
        return;
    }
    if (pos >= b->mem->size)
    {
        RET(DSERR_INVALIDPARAM);
        return;
    }
    dlock();
    b->frame = pos / b->f.align;
    b->frac = 0;
    dunlock();
    RET(DS_OK);
}

/* HRESULT SetFormat(this, LPCWAVEFORMATEX fmt) */
void rt_com_DirectSoundBuffer8_SetFormat(_cpu *cpu)
{
    uint32_t r, n;
    uint8_t raw[FMT_MAX];
    wfmt f;
    dsbuf *b = buf_this(cpu);
    if (!b->primary)
    {
        RET(DSERR_INVALIDCALL);
        return;
    }
    if (b->dev->level < DSSCL_PRIORITY)
    {
        RET(DSERR_PRIOLEVELNEEDED);
        return;
    }
    r = read_format(ARG(1), &f, raw, &n);
    if (r == DS_OK)
    {
        dlock();
        memcpy(b->dev->pfmt, raw, n);
        b->dev->pfmt_size = n;
        dunlock();
        rt_log("DSOUND: primary format %u Hz, %u bits%s, %u channels", f.rate, f.bits, f.is_float ? " float" : "", f.channels);
    }
    RET(r);
}

/* HRESULT SetVolume(this, LONG volume) */
void rt_com_DirectSoundBuffer8_SetVolume(_cpu *cpu)
{
    int32_t v = (int32_t)ARG(1);
    dsbuf *b = buf_this(cpu);
    if (!(b->flags & DSBCAPS_CTRLVOLUME))
    {
        RET(DSERR_CONTROLUNAVAIL);
        return;
    }
    if (v < DSBVOLUME_MIN || v > 0)
    {
        RET(DSERR_INVALIDPARAM);
        return;
    }
    dlock();
    if (b->primary)
        b->dev->pvolume = v;
    else
        b->volume = v;
    dunlock();
    RET(DS_OK);
}

/* HRESULT SetPan(this, LONG pan) */
void rt_com_DirectSoundBuffer8_SetPan(_cpu *cpu)
{
    int32_t v = (int32_t)ARG(1);
    dsbuf *b = buf_this(cpu);
    if (!(b->flags & DSBCAPS_CTRLPAN))
    {
        RET(DSERR_CONTROLUNAVAIL);
        return;
    }
    if (v < DSBPAN_LEFT || v > DSBPAN_RIGHT)
    {
        RET(DSERR_INVALIDPARAM);
        return;
    }
    dlock();
    if (b->primary)
        b->dev->ppan = v;
    else
        b->pan = v;
    dunlock();
    RET(DS_OK);
}

/* HRESULT SetFrequency(this, DWORD frequency) */
void rt_com_DirectSoundBuffer8_SetFrequency(_cpu *cpu)
{
    uint32_t v = ARG(1);
    dsbuf *b = buf_this(cpu);
    if (b->primary || !(b->flags & DSBCAPS_CTRLFREQUENCY))
    {
        RET(DSERR_CONTROLUNAVAIL);
        return;
    }
    if (v != 0 && (v < DSBFREQUENCY_MIN || v > DSBFREQUENCY_MAX))
    {
        RET(DSERR_INVALIDPARAM);
        return;
    }
    dlock();
    b->freq = v;
    dunlock();
    RET(DS_OK);
}

/* HRESULT Stop(this) */
void rt_com_DirectSoundBuffer8_Stop(_cpu *cpu)
{
    dsbuf *b = buf_this(cpu);
    dlock();
    if (b->primary)
        b->dev->pplaying = false;
    else
        b->playing = false;
    dunlock();
    RET(DS_OK);
}

/* HRESULT Unlock(this, LPVOID p1, DWORD n1, LPVOID p2, DWORD n2) */
void rt_com_DirectSoundBuffer8_Unlock(_cpu *cpu)
{
    uint32_t p1 = ARG(1), n1 = ARG(2), p2 = ARG(3), n2 = ARG(4), base, size;
    dsbuf *b = buf_this(cpu);
    if (b->primary) rt_fatal("DSOUND: Unlock of the primary buffer (DSSCL_WRITEPRIMARY is not supported by the runtime)");
    base = b->mem->guest;
    size = b->mem->size;
    if (p1 < base || p1 - base > size || n1 > size - (p1 - base) || (p2 != 0 && (p2 != base || n2 > size)))
    {
        RET(DSERR_INVALIDPARAM);
        return;
    }
    RET(DS_OK);
}

/* HRESULT Restore(this) */
void rt_com_DirectSoundBuffer8_Restore(_cpu *cpu)
{
    (void)buf_this(cpu);
    RET(DS_OK);
}

/* HRESULT SetFX(this, DWORD count, LPDSEFFECTDESC effects, LPDWORD results) */
void rt_com_DirectSoundBuffer8_SetFX(_cpu *cpu)
{
    uint32_t count = ARG(1);
    dsbuf *b = buf_this(cpu);
    if (b->primary || !(b->flags & DSBCAPS_CTRLFX))
    {
        RET(DSERR_CONTROLUNAVAIL);
        return;
    }
    if (count != 0) rt_fatal("DSOUND: SetFX with %u effects (DirectSound effects are not supported by the runtime)", count);
    RET(DS_OK);
}

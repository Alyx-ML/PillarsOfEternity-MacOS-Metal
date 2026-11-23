/*
 * WINMM waveform audio: waveOut and waveIn.
 *
 * Device 0 (and WAVE_MAPPER) is the default output or input device of
 * macOS.
 *
 * waveOut: a default output audio unit (CoreAudio) plays the guest
 * format; the unit converts the sample format, the sample rate and the
 * channels for the device. The render callback reads the data of the
 * queued WAVEHDR blocks from guest memory and plays silence when no block
 * is queued. waveOutGetPosition gives the bytes that the unit took from
 * the blocks.
 *
 * waveIn: an AudioQueue input records in the guest format and fills the
 * blocks of waveInAddBuffer in order. Data without a free block is lost,
 * as on Windows. macOS asks the user for the microphone permission; the
 * application bundle must have NSMicrophoneUsageDescription.
 *
 * Callbacks (WOM_*, WIM_*): one runtime thread for each open device calls
 * the guest callback function, sets the event or posts the window
 * message. CALLBACK_THREAD is not supported (USER32 has no thread
 * messages for other threads).
 */
#include "rt.h"
#include "rt_object.h"
#include "rt_text.h"
#include "rt_thread.h"
#include "rt_user32.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define MMSYSERR_NOERROR 0u
#define MMSYSERR_ERROR 1u
#define MMSYSERR_BADDEVICEID 2u
#define MMSYSERR_ALLOCATED 4u
#define MMSYSERR_INVALHANDLE 5u
#define MMSYSERR_NODRIVER 6u
#define MMSYSERR_NOMEM 7u
#define MMSYSERR_INVALFLAG 10u
#define MMSYSERR_INVALPARAM 11u
#define WAVERR_BADFORMAT 32u
#define WAVERR_STILLPLAYING 33u
#define WAVERR_UNPREPARED 34u

#define WAVE_MAPPER 0xFFFFFFFFu
#define WAVE_FORMAT_QUERY 0x0001u
#define WAVE_ALLOWSYNC 0x0002u
#define WAVE_MAPPED 0x0004u
#define WAVE_FORMAT_DIRECT 0x0008u
#define CALLBACK_TYPEMASK 0x00070000u
#define CALLBACK_NULL 0x00000000u
#define CALLBACK_WINDOW 0x00010000u
#define CALLBACK_THREAD 0x00020000u
#define CALLBACK_FUNCTION 0x00030000u
#define CALLBACK_EVENT 0x00050000u

#define WOM_OPEN 0x3BBu
#define WOM_CLOSE 0x3BCu
#define WOM_DONE 0x3BDu
#define WIM_OPEN 0x3BEu
#define WIM_CLOSE 0x3BFu
#define WIM_DATA 0x3C0u

#define WHDR_DONE 0x01u
#define WHDR_PREPARED 0x02u
#define WHDR_BEGINLOOP 0x04u
#define WHDR_ENDLOOP 0x08u
#define WHDR_INQUEUE 0x10u

#define TIME_MS 0x0001u
#define TIME_SAMPLES 0x0002u
#define TIME_BYTES 0x0004u

#define WAVE_FORMAT_PCM 1u
#define WAVE_FORMAT_IEEE_FLOAT 3u
#define WAVE_FORMAT_EXTENSIBLE 0xFFFEu

#define MAX_OPEN 16
#define MAX_QUEUE 256
#define IN_BUFFERS 4
#define CALLBACK_STACK (256u * 1024u)

/* WAVEHDR: lpData 0, dwBufferLength 4, dwBytesRecorded 8, dwUser 12,
   dwFlags 16, dwLoops 20, lpNext 24, reserved 28. */
#define HDR_SIZE 32u

typedef struct wfmt {
    uint32_t channels, rate, bits, align, mask;
    bool is_float;
} wfmt;

typedef struct note {
    uint32_t msg, p1;
} note;

typedef struct wdev {
    bool used, input;
    uint32_t handle;
    uint32_t cb_type, cb, inst;
    wfmt f;
    /* output */
    AudioComponentInstance unit;
    bool running;
    /* input */
    AudioQueueRef aq;
    AudioQueueBufferRef aqbuf[IN_BUFFERS];
    bool recording;
    /* the queued blocks (guest addresses) */
    uint32_t q[MAX_QUEUE];
    int qhead, qcount;
    uint32_t offset; /* output: bytes of the first block that were played */
    uint64_t position; /* bytes played or recorded */
    /* callbacks */
    note notes[MAX_QUEUE + 4];
    int nhead, ncount;
    bool stop;      /* the callback thread ends after the notes */
    bool cb_done;   /* the callback thread ended */
    uint32_t handled; /* the number of notes handled */
    uint32_t posted;  /* the number of notes posted */
} wdev;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static wdev g_devs[MAX_OPEN];
static uint16_t g_gen[MAX_OPEN];

static void wlock(void)
{
    if (pthread_mutex_lock(&g_lock) != 0) rt_fatal("WINMM: lock failed");
}

static void wunlock(void)
{
    if (pthread_mutex_unlock(&g_lock) != 0) rt_fatal("WINMM: unlock failed");
}

static void wbroadcast(void)
{
    if (pthread_cond_broadcast(&g_cond) != 0) rt_fatal("WINMM: condition broadcast failed");
}

static void wwait(void)
{
    if (pthread_cond_wait(&g_cond, &g_lock) != 0) rt_fatal("WINMM: condition wait failed");
}

/* ---------------------------------------------------------------- devices */

static AudioObjectID default_device(bool input)
{
    AudioObjectPropertyAddress a = { input ? kAudioHardwarePropertyDefaultInputDevice : kAudioHardwarePropertyDefaultOutputDevice,
                                     kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectID id = kAudioObjectUnknown;
    UInt32 size = sizeof id;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, &id) != noErr) return kAudioObjectUnknown;
    return id;
}

/* The name of the default device in UTF-16 (at most 31 characters, as
   szPname). Returns the number of characters. */
static int device_name(bool input, uint16_t out[32])
{
    AudioObjectID id = default_device(input);
    AudioObjectPropertyAddress a = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFStringRef name = NULL;
    UInt32 size = sizeof name;
    CFIndex n;
    if (id == kAudioObjectUnknown || AudioObjectGetPropertyData(id, &a, 0, NULL, &size, &name) != noErr || name == NULL)
    {
        static const char def[] = "Default device";
        int i;
        for (i = 0; def[i] != 0; i++) out[i] = (uint8_t)def[i];
        return i;
    }
    n = CFStringGetLength(name);
    if (n > 31) n = 31;
    CFStringGetCharacters(name, CFRangeMake(0, n), out);
    CFRelease(name);
    return (int)n;
}

/* WAVEFORMATEX (or WAVEFORMATEXTENSIBLE) at g. Returns MMSYSERR_NOERROR
   or WAVERR_BADFORMAT. */
static uint32_t read_format(uint32_t g, bool input, wfmt *f)
{
    uint32_t tag, valid;
    if (g == 0 || !rt_va_is_committed(g, 16)) return MMSYSERR_INVALPARAM;
    tag = rd16(g);
    f->channels = rd16(g + 2);
    f->rate = rd32(g + 4);
    f->align = rd16(g + 12);
    f->bits = rd16(g + 14);
    f->mask = 0;
    f->is_float = false;
    if (tag == WAVE_FORMAT_EXTENSIBLE)
    {
        /* cbSize 16, wValidBitsPerSample 18, dwChannelMask 20, SubFormat 24. */
        static const uint8_t pcm[16] = { 1, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71 };
        static const uint8_t flt[16] = { 3, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71 };
        if (!rt_va_is_committed(g, 40) || rd16(g + 16) < 22) return WAVERR_BADFORMAT;
        valid = rd16(g + 18);
        f->mask = rd32(g + 20);
        if (memcmp(G2H(g + 24), pcm, 16) == 0)
            f->is_float = false;
        else if (memcmp(G2H(g + 24), flt, 16) == 0)
            f->is_float = true;
        else
            return WAVERR_BADFORMAT;
        if (valid != 0 && valid != f->bits) return WAVERR_BADFORMAT;
    }
    else if (tag == WAVE_FORMAT_IEEE_FLOAT)
        f->is_float = true;
    else if (tag != WAVE_FORMAT_PCM)
        return WAVERR_BADFORMAT;
    if (f->is_float ? f->bits != 32 : (f->bits != 8 && f->bits != 16 && f->bits != 24 && f->bits != 32)) return WAVERR_BADFORMAT;
    if (f->channels == 0 || f->channels > (input ? 2u : 8u)) return WAVERR_BADFORMAT;
    if (f->rate < 1000 || f->rate > 384000) return WAVERR_BADFORMAT;
    if (f->align != f->channels * f->bits / 8u) return WAVERR_BADFORMAT;
    if (rd32(g + 8) != f->rate * f->align) return WAVERR_BADFORMAT;
    if (f->mask != 0 && (uint32_t)__builtin_popcount(f->mask) != f->channels) return WAVERR_BADFORMAT;
    return MMSYSERR_NOERROR;
}

static AudioStreamBasicDescription asbd_of(const wfmt *f)
{
    AudioStreamBasicDescription d;
    memset(&d, 0, sizeof d);
    d.mSampleRate = f->rate;
    d.mFormatID = kAudioFormatLinearPCM;
    d.mFormatFlags = kAudioFormatFlagIsPacked |
                     (f->is_float ? kAudioFormatFlagIsFloat : (f->bits > 8 ? kAudioFormatFlagIsSignedInteger : 0u));
    d.mBytesPerPacket = f->align;
    d.mFramesPerPacket = 1;
    d.mBytesPerFrame = f->align;
    d.mChannelsPerFrame = f->channels;
    d.mBitsPerChannel = f->bits;
    return d;
}

/* The Windows default speaker positions for a format without a mask. */
static uint32_t default_mask(uint32_t channels)
{
    switch (channels)
    {
    case 1: return 0x4;   /* front center */
    case 2: return 0x3;
    case 3: return 0x7;
    case 4: return 0x33;  /* quad */
    case 5: return 0x37;
    case 6: return 0x3F;  /* 5.1 */
    case 7: return 0x13F;
    default: return 0x63F; /* 7.1 */
    }
}

/* ---------------------------------------------------------------- callbacks */

/* Queue a callback note (lock held). */
static void post_note(wdev *d, uint32_t msg, uint32_t p1)
{
    int tail;
    if (d->cb_type == CALLBACK_NULL) return;
    if (d->ncount >= (int)(sizeof d->notes / sizeof d->notes[0])) rt_fatal("WINMM: the callback queue is full");
    tail = (d->nhead + d->ncount) % (int)(sizeof d->notes / sizeof d->notes[0]);
    d->notes[tail] = (note){ msg, p1 };
    d->ncount++;
    d->posted++;
    wbroadcast();
}

/* Wait until the callback thread handled all notes (lock held). */
static void wait_notes(wdev *d)
{
    while (d->cb_type != CALLBACK_NULL && d->handled != d->posted) wwait();
}

static void callback_body(void *ctx, _cpu *cpu)
{
    wdev *d = ctx;
    for (;;)
    {
        note n;
        uint32_t type, cb, inst, h;
        wlock();
        while (d->ncount == 0 && !d->stop) wwait();
        if (d->ncount == 0 && d->stop)
        {
            d->cb_done = true;
            wbroadcast();
            wunlock();
            return;
        }
        n = d->notes[d->nhead];
        d->nhead = (d->nhead + 1) % (int)(sizeof d->notes / sizeof d->notes[0]);
        d->ncount--;
        type = d->cb_type;
        cb = d->cb;
        inst = d->inst;
        h = d->handle;
        wunlock();
        switch (type)
        {
        case CALLBACK_FUNCTION:
            /* void CALLBACK waveOutProc(HWAVEOUT, UINT msg, DWORD_PTR instance,
               DWORD_PTR p1, DWORD_PTR p2) */
            rt_call_cb(cpu, cb, 5, h, n.msg, inst, n.p1, 0);
            break;
        case CALLBACK_EVENT:
        {
            rt_object *e = rt_handle_get(cb, OBJ_EVENT);
            if (e != NULL)
            {
                rt_obj_lock();
                e->signaled = true;
                rt_obj_signal(e, e->manual_reset ? 0u : 1u);
                rt_obj_unlock();
                rt_object_unref(e);
            }
            break;
        }
        case CALLBACK_WINDOW:
            /* MM_WOM_* and MM_WIM_* have the values of WOM_* and WIM_*. */
            rt_user32_post_message(cb, n.msg, h, n.p1);
            break;
        default: break;
        }
        wlock();
        d->handled++;
        wbroadcast();
        wunlock();
    }
}

/* ---------------------------------------------------------------- handles */

static wdev *dev_of(uint32_t h, bool input) /* lock held */
{
    uint32_t idx = (h >> 8) & 0xFFu;
    if ((h & 0xFFFF0000u) != (input ? 0x57490000u : 0x574F0000u) || idx >= MAX_OPEN) return NULL;
    if (!g_devs[idx].used || g_devs[idx].handle != h || g_devs[idx].input != input) return NULL;
    return &g_devs[idx];
}

/* Mark a block done (lock held). */
static void block_done(wdev *d, uint32_t hdr)
{
    uint32_t fl = rd32(hdr + 16);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    wr32(hdr + 16, (fl & ~WHDR_INQUEUE) | WHDR_DONE);
    post_note(d, d->input ? WIM_DATA : WOM_DONE, hdr);
}

/* ---------------------------------------------------------------- output */

static OSStatus render(void *ctx, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames,
                       AudioBufferList *io)
{
    wdev *d = ctx;
    uint8_t *out = io->mBuffers[0].mData;
    uint32_t need, got = 0, silence;
    (void)flags;
    (void)ts;
    (void)bus;
    need = frames * d->f.align;
    if (io->mBuffers[0].mDataByteSize < need) need = io->mBuffers[0].mDataByteSize;
    silence = (!d->f.is_float && d->f.bits == 8) ? 0x80u : 0u;
    wlock();
    while (got < need && d->qcount > 0)
    {
        uint32_t hdr = d->q[d->qhead], data = rd32(hdr), len = rd32(hdr + 4), n;
        if (d->offset < len)
        {
            n = len - d->offset;
            if (n > need - got) n = need - got;
            memcpy(out + got, G2H(data + d->offset), n);
            got += n;
            d->offset += n;
            d->position += n;
        }
        if (d->offset >= len)
        {
            d->qhead = (d->qhead + 1) % MAX_QUEUE;
            d->qcount--;
            d->offset = 0;
            block_done(d, hdr);
        }
    }
    wunlock();
    if (rt_audio_muted()) got = 0;
    if (got < need) memset(out + got, (int)silence, need - got);
    return noErr;
}

static uint32_t open_output(wdev *d)
{
    AudioComponentDescription desc = { kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0 };
    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    AudioStreamBasicDescription asbd = asbd_of(&d->f);
    AURenderCallbackStruct cb = { render, d };
    AudioChannelLayout layout;
    OSStatus st;
    if (comp == NULL) return MMSYSERR_NODRIVER;
    if (AudioComponentInstanceNew(comp, &d->unit) != noErr) return MMSYSERR_NODRIVER;
    st = AudioUnitSetProperty(d->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &asbd, sizeof asbd);
    if (st == noErr && d->f.channels > 2)
    {
        memset(&layout, 0, sizeof layout);
        layout.mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelBitmap;
        layout.mChannelBitmap = d->f.mask != 0 ? d->f.mask : default_mask(d->f.channels);
        st = AudioUnitSetProperty(d->unit, kAudioUnitProperty_AudioChannelLayout, kAudioUnitScope_Input, 0, &layout, sizeof layout);
    }
    if (st == noErr) st = AudioUnitSetProperty(d->unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
    if (st == noErr) st = AudioUnitInitialize(d->unit);
    if (st != noErr)
    {
        AudioComponentInstanceDispose(d->unit);
        d->unit = NULL;
        return WAVERR_BADFORMAT;
    }
    return MMSYSERR_NOERROR;
}

/* ---------------------------------------------------------------- input */

static void aq_input(void *ctx, AudioQueueRef q, AudioQueueBufferRef buf, const AudioTimeStamp *ts, UInt32 npackets,
                     const AudioStreamPacketDescription *pd)
{
    wdev *d = ctx;
    const uint8_t *src = buf->mAudioData;
    uint32_t left = buf->mAudioDataByteSize;
    bool again;
    (void)ts;
    (void)npackets;
    (void)pd;
    wlock();
    while (left > 0 && d->qcount > 0)
    {
        uint32_t hdr = d->q[d->qhead], data = rd32(hdr), len = rd32(hdr + 4), rec = rd32(hdr + 8), n;
        n = len - rec;
        if (n > left) n = left;
        memcpy(G2H(data + rec), src, n);
        src += n;
        left -= n;
        rec += n;
        d->position += n;
        wr32(hdr + 8, rec);
        if (rec >= len)
        {
            d->qhead = (d->qhead + 1) % MAX_QUEUE;
            d->qcount--;
            block_done(d, hdr);
        }
    }
    again = d->recording;
    wunlock();
    if (again) AudioQueueEnqueueBuffer(q, buf, 0, NULL);
}

static uint32_t open_input(wdev *d)
{
    AudioStreamBasicDescription asbd = asbd_of(&d->f);
    uint32_t size = d->f.rate * d->f.align / 50u; /* 20 ms */
    int i;
    if (size < d->f.align) size = d->f.align;
    size -= size % d->f.align;
    if (AudioQueueNewInput(&asbd, aq_input, d, NULL, NULL, 0, &d->aq) != noErr) return WAVERR_BADFORMAT;
    for (i = 0; i < IN_BUFFERS; i++)
        if (AudioQueueAllocateBuffer(d->aq, size, &d->aqbuf[i]) != noErr)
        {
            AudioQueueDispose(d->aq, true);
            d->aq = NULL;
            return MMSYSERR_NOMEM;
        }
    return MMSYSERR_NOERROR;
}

/* ---------------------------------------------------------------- open and close */

static void wave_open(_cpu *cpu, bool input)
{
    uint32_t ph = ARG(0), id = ARG(1), fmt = ARG(2), cb = ARG(3), inst = ARG(4), flags = ARG(5), r, type;
    wfmt f;
    wdev *d = NULL;
    int i;
    if (flags & ~(WAVE_FORMAT_QUERY | WAVE_ALLOWSYNC | WAVE_MAPPED | WAVE_FORMAT_DIRECT | CALLBACK_TYPEMASK))
    {
        RET(MMSYSERR_INVALFLAG);
        return;
    }
    if (default_device(input) == kAudioObjectUnknown || (id != 0 && id != WAVE_MAPPER))
    {
        RET(MMSYSERR_BADDEVICEID);
        return;
    }
    r = read_format(fmt, input, &f);
    if (r != MMSYSERR_NOERROR || (flags & WAVE_FORMAT_QUERY))
    {
        RET(r);
        return;
    }
    if (ph == 0 || !rt_va_is_committed(ph, 4))
    {
        RET(MMSYSERR_INVALPARAM);
        return;
    }
    type = flags & CALLBACK_TYPEMASK;
    if (type == CALLBACK_THREAD) rt_fatal("waveOpen: CALLBACK_THREAD is not supported");
    if (type != CALLBACK_NULL && type != CALLBACK_WINDOW && type != CALLBACK_FUNCTION && type != CALLBACK_EVENT)
    {
        RET(MMSYSERR_INVALFLAG);
        return;
    }
    wlock();
    for (i = 0; i < MAX_OPEN && d == NULL; i++)
        if (!g_devs[i].used) d = &g_devs[i];
    if (d == NULL)
    {
        wunlock();
        RET(MMSYSERR_ALLOCATED);
        return;
    }
    i = (int)(d - g_devs);
    memset(d, 0, sizeof *d);
    d->used = true;
    d->input = input;
    g_gen[i]++;
    d->handle = (input ? 0x57490000u : 0x574F0000u) | ((uint32_t)i << 8) | (g_gen[i] & 0xFFu);
    d->cb_type = type;
    d->cb = cb;
    d->inst = inst;
    d->f = f;
    wunlock();
    r = input ? open_input(d) : open_output(d);
    if (r != MMSYSERR_NOERROR)
    {
        wlock();
        d->used = false;
        wunlock();
        RET(r);
        return;
    }
    if (type != CALLBACK_NULL) rt_thread_start_runtime(callback_body, d, CALLBACK_STACK);
    wr32(ph, d->handle);
    wlock();
    post_note(d, input ? WIM_OPEN : WOM_OPEN, 0);
    wait_notes(d);
    wunlock();
    RET(MMSYSERR_NOERROR);
}

static void wave_close(_cpu *cpu, bool input)
{
    wdev *d;
    wlock();
    d = dev_of(ARG(0), input);
    if (d == NULL)
    {
        wunlock();
        RET(MMSYSERR_INVALHANDLE);
        return;
    }
    if (d->qcount > 0)
    {
        wunlock();
        RET(WAVERR_STILLPLAYING);
        return;
    }
    d->recording = false;
    wunlock();
    if (input)
    {
        AudioQueueStop(d->aq, true);
        AudioQueueDispose(d->aq, true);
        d->aq = NULL;
    }
    else
    {
        if (d->running) AudioOutputUnitStop(d->unit);
        AudioUnitUninitialize(d->unit);
        AudioComponentInstanceDispose(d->unit);
        d->unit = NULL;
    }
    wlock();
    d->running = false;
    post_note(d, input ? WIM_CLOSE : WOM_CLOSE, 0);
    d->stop = true;
    wbroadcast();
    while (d->cb_type != CALLBACK_NULL && !d->cb_done) wwait();
    d->used = false;
    wunlock();
    RET(MMSYSERR_NOERROR);
}

/* ---------------------------------------------------------------- blocks */

static uint32_t hdr_check(uint32_t hdr, uint32_t size)
{
    if (hdr == 0 || size < HDR_SIZE || !rt_va_is_committed(hdr, HDR_SIZE)) return MMSYSERR_INVALPARAM;
    if (rd32(hdr + 4) != 0 && !rt_va_is_committed(rd32(hdr), rd32(hdr + 4))) return MMSYSERR_INVALPARAM;
    return MMSYSERR_NOERROR;
}

static void prepare(_cpu *cpu, bool input, bool unprepare)
{
    uint32_t hdr = ARG(1), r = hdr_check(hdr, ARG(2)), fl;
    wlock();
    if (dev_of(ARG(0), input) == NULL)
    {
        wunlock();
        RET(MMSYSERR_INVALHANDLE);
        return;
    }
    if (r != MMSYSERR_NOERROR)
    {
        wunlock();
        RET(r);
        return;
    }
    fl = rd32(hdr + 16);
    if (unprepare)
    {
        if (fl & WHDR_INQUEUE)
        {
            wunlock();
            RET(WAVERR_STILLPLAYING);
            return;
        }
        wr32(hdr + 16, fl & ~WHDR_PREPARED);
    }
    else
        wr32(hdr + 16, fl | WHDR_PREPARED);
    wunlock();
    RET(MMSYSERR_NOERROR);
}

static void add_block(_cpu *cpu, bool input)
{
    uint32_t hdr = ARG(1), r = hdr_check(hdr, ARG(2)), fl;
    wdev *d;
    bool start = false;
    wlock();
    d = dev_of(ARG(0), input);
    if (d == NULL)
    {
        wunlock();
        RET(MMSYSERR_INVALHANDLE);
        return;
    }
    if (r != MMSYSERR_NOERROR)
    {
        wunlock();
        RET(r);
        return;
    }
    fl = rd32(hdr + 16);
    if (!(fl & WHDR_PREPARED))
    {
        wunlock();
        RET(WAVERR_UNPREPARED);
        return;
    }
    if (fl & WHDR_INQUEUE)
    {
        wunlock();
        RET(WAVERR_STILLPLAYING);
        return;
    }
    if (!input && (fl & (WHDR_BEGINLOOP | WHDR_ENDLOOP))) rt_fatal("waveOutWrite: looped blocks are not supported");
    if (d->qcount >= MAX_QUEUE) rt_fatal("WINMM: more than %d queued blocks", MAX_QUEUE);
    if (input) wr32(hdr + 8, 0);
    wr32(hdr + 16, (fl & ~WHDR_DONE) | WHDR_INQUEUE);
    d->q[(d->qhead + d->qcount) % MAX_QUEUE] = hdr;
    d->qcount++;
    if (!input && !d->running)
    {
        d->running = true;
        start = true;
    }
    wunlock();
    if (start && AudioOutputUnitStart(d->unit) != noErr)
    {
        wlock();
        d->running = false;
        wunlock();
        rt_fatal("waveOutWrite: the CoreAudio output unit could not start");
    }
    RET(MMSYSERR_NOERROR);
}

/* Stop and return all blocks (waveOutReset, waveInReset). */
static void reset(_cpu *cpu, bool input)
{
    wdev *d;
    wlock();
    d = dev_of(ARG(0), input);
    if (d == NULL)
    {
        wunlock();
        RET(MMSYSERR_INVALHANDLE);
        return;
    }
    d->recording = false;
    wunlock();
    if (input)
        AudioQueueStop(d->aq, true);
    else if (d->running)
        AudioOutputUnitStop(d->unit);
    wlock();
    d->running = false;
    while (d->qcount > 0)
    {
        uint32_t hdr = d->q[d->qhead];
        d->qhead = (d->qhead + 1) % MAX_QUEUE;
        d->qcount--;
        block_done(d, hdr);
    }
    d->offset = 0;
    d->position = 0;
    wait_notes(d);
    wunlock();
    RET(MMSYSERR_NOERROR);
}

/* ---------------------------------------------------------------- functions */

/* UINT waveOutGetNumDevs(void) */
void rt_waveOutGetNumDevs(_cpu *cpu) { RET(default_device(false) != kAudioObjectUnknown ? 1u : 0u); }
/* UINT waveInGetNumDevs(void) */
void rt_waveInGetNumDevs(_cpu *cpu) { RET(default_device(true) != kAudioObjectUnknown ? 1u : 0u); }

/* WAVEOUTCAPS / WAVEINCAPS: wMid 0, wPid 2, vDriverVersion 4, szPname 8
   (32 characters), then dwFormats, wChannels, wReserved1 (and dwSupport
   for output). */
static void get_caps(_cpu *cpu, bool input, bool wide)
{
    uint32_t id = ARG(0), p = ARG(1), size = ARG(2), name_bytes = wide ? 64u : 32u, full = 8u + name_bytes + 8u + (input ? 0u : 4u);
    uint8_t tmp[96];
    uint16_t name[32];
    int n, i;
    if (default_device(input) == kAudioObjectUnknown || (id != 0 && id != WAVE_MAPPER))
    {
        RET(MMSYSERR_BADDEVICEID);
        return;
    }
    if (p == 0 || !rt_va_is_committed(p, size))
    {
        RET(MMSYSERR_INVALPARAM);
        return;
    }
    memset(tmp, 0, sizeof tmp);
    tmp[0] = 1; /* MM_MICROSOFT */
    tmp[2] = input ? 0x65 : 0x66; /* MM_MSFT_GENERIC_WAVEIN, MM_MSFT_GENERIC_WAVEOUT */
    tmp[4] = 0;
    tmp[5] = 6; /* driver version 6.0 */
    n = device_name(input, name);
    for (i = 0; i < n; i++)
    {
        if (wide)
        {
            tmp[8 + 2 * i] = (uint8_t)name[i];
            tmp[9 + 2 * i] = (uint8_t)(name[i] >> 8);
        }
        else
        {
            int c = rt_unicode_to_cp1252(name[i]);
            tmp[8 + i] = (uint8_t)(c < 0 ? '?' : c);
        }
    }
    {
        uint32_t o = 8u + name_bytes;
        uint32_t formats = 0x000FFFFFu; /* all WAVE_FORMAT_* rates of 11, 22, 44 and 96 kHz */
        memcpy(tmp + o, &formats, 4);
        tmp[o + 4] = 2; /* wChannels */
        if (!input)
        {
            uint32_t support = 0x0004u | 0x0008u; /* WAVECAPS_VOLUME, WAVECAPS_LRVOLUME */
            memcpy(tmp + o + 8, &support, 4);
        }
    }
    for (i = 0; i < (int)(size < full ? size : full); i++) wr8(p + (uint32_t)i, tmp[i]);
    RET(MMSYSERR_NOERROR);
}

void rt_waveOutGetDevCapsA(_cpu *cpu) { get_caps(cpu, false, false); }
void rt_waveOutGetDevCapsW(_cpu *cpu) { get_caps(cpu, false, true); }
void rt_waveInGetDevCapsA(_cpu *cpu) { get_caps(cpu, true, false); }
void rt_waveInGetDevCapsW(_cpu *cpu) { get_caps(cpu, true, true); }

/* MMRESULT waveOutOpen(LPHWAVEOUT, UINT id, LPCWAVEFORMATEX, DWORD_PTR cb,
   DWORD_PTR instance, DWORD flags) */
void rt_waveOutOpen(_cpu *cpu) { wave_open(cpu, false); }
/* MMRESULT waveInOpen(LPHWAVEIN, UINT id, LPCWAVEFORMATEX, DWORD_PTR cb,
   DWORD_PTR instance, DWORD flags) */
void rt_waveInOpen(_cpu *cpu) { wave_open(cpu, true); }
void rt_waveOutClose(_cpu *cpu) { wave_close(cpu, false); }
void rt_waveInClose(_cpu *cpu) { wave_close(cpu, true); }
void rt_waveOutPrepareHeader(_cpu *cpu) { prepare(cpu, false, false); }
void rt_waveOutUnprepareHeader(_cpu *cpu) { prepare(cpu, false, true); }
void rt_waveInPrepareHeader(_cpu *cpu) { prepare(cpu, true, false); }
void rt_waveInUnprepareHeader(_cpu *cpu) { prepare(cpu, true, true); }
void rt_waveOutWrite(_cpu *cpu) { add_block(cpu, false); }
void rt_waveInAddBuffer(_cpu *cpu) { add_block(cpu, true); }
void rt_waveOutReset(_cpu *cpu) { reset(cpu, false); }
void rt_waveInReset(_cpu *cpu) { reset(cpu, true); }

/* MMRESULT waveInStart(HWAVEIN) */
void rt_waveInStart(_cpu *cpu)
{
    wdev *d;
    int i;
    wlock();
    d = dev_of(ARG(0), true);
    if (d == NULL)
    {
        wunlock();
        RET(MMSYSERR_INVALHANDLE);
        return;
    }
    if (d->recording)
    {
        wunlock();
        RET(MMSYSERR_NOERROR);
        return;
    }
    d->recording = true;
    wunlock();
    for (i = 0; i < IN_BUFFERS; i++)
        if (AudioQueueEnqueueBuffer(d->aq, d->aqbuf[i], 0, NULL) != noErr) rt_fatal("waveInStart: AudioQueueEnqueueBuffer failed");
    if (AudioQueueStart(d->aq, NULL) != noErr)
    {
        wlock();
        d->recording = false;
        wunlock();
        RET(MMSYSERR_ERROR);
        return;
    }
    RET(MMSYSERR_NOERROR);
}

/* MMRESULT waveOutGetPosition(HWAVEOUT, LPMMTIME, UINT size) */
void rt_waveOutGetPosition(_cpu *cpu)
{
    uint32_t mt = ARG(1), size = ARG(2), type;
    uint64_t pos;
    wfmt f;
    wdev *d;
    if (mt == 0 || size < 8 || !rt_va_is_committed(mt, size))
    {
        RET(MMSYSERR_INVALPARAM);
        return;
    }
    wlock();
    d = dev_of(ARG(0), false);
    if (d == NULL)
    {
        wunlock();
        RET(MMSYSERR_INVALHANDLE);
        return;
    }
    pos = d->position;
    f = d->f;
    wunlock();
    type = rd32(mt);
    switch (type)
    {
    case TIME_SAMPLES: wr32(mt + 4, (uint32_t)(pos / f.align)); break;
    case TIME_MS: wr32(mt + 4, (uint32_t)(pos / f.align * 1000u / f.rate)); break;
    default:
        /* Other time formats: the bytes, as Windows does for a format that
           the device does not support. */
        wr32(mt, TIME_BYTES);
        wr32(mt + 4, (uint32_t)pos);
        break;
    }
    RET(MMSYSERR_NOERROR);
}

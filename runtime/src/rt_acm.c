/*
 * MSACM32: audio compression manager streams.
 *
 * The runtime has the decoders of the codecs that Windows installs:
 *   - Microsoft ADPCM (format tag 2), IMA ADPCM (0x11), CCITT A-law (6)
 *     and u-law (7): decoders in this file;
 *   - GSM 6.10 (0x31) and MPEG Layer 3 (0x55): CoreAudio AudioConverter;
 *   - the PCM converter: 8 and 16 bits, mono and stereo, at the same
 *     sample rate.
 * Each decoder makes 16-bit PCM with the channels and the sample rate of
 * the source. Encoders, filters and asynchronous streams are not
 * supported.
 *
 * MSACM32 has no acmStreamClose import in this program, so the streams
 * stay open until the process ends.
 */
#include "rt.h"
#include "rt_thread.h"

#include <AudioToolbox/AudioToolbox.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define MMSYSERR_NOERROR 0u
#define MMSYSERR_INVALHANDLE 5u
#define MMSYSERR_NOMEM 7u
#define MMSYSERR_INVALFLAG 10u
#define MMSYSERR_INVALPARAM 11u
#define ACMERR_NOTPOSSIBLE 512u
#define ACMERR_UNPREPARED 514u

#define ACM_FORMATSUGGESTF_WFORMATTAG 0x00010000u
#define ACM_FORMATSUGGESTF_NCHANNELS 0x00020000u
#define ACM_FORMATSUGGESTF_NSAMPLESPERSEC 0x00040000u
#define ACM_FORMATSUGGESTF_WBITSPERSAMPLE 0x00080000u

#define ACM_STREAMOPENF_QUERY 0x00000001u
#define ACM_STREAMOPENF_ASYNC 0x00000002u
#define ACM_STREAMOPENF_NONREALTIME 0x00000004u

#define ACM_STREAMSIZEF_SOURCE 0x00000000u
#define ACM_STREAMSIZEF_DESTINATION 0x00000001u

#define ACM_STREAMCONVERTF_BLOCKALIGN 0x00000004u
#define ACM_STREAMCONVERTF_START 0x00000010u
#define ACM_STREAMCONVERTF_END 0x00000020u

#define STATUSF_DONE 0x00010000u
#define STATUSF_PREPARED 0x00020000u
#define STATUSF_INQUEUE 0x00100000u

#define TAG_PCM 1u
#define TAG_MSADPCM 2u
#define TAG_ALAW 6u
#define TAG_MULAW 7u
#define TAG_IMAADPCM 0x11u
#define TAG_GSM610 0x31u
#define TAG_MP3 0x55u

/* ACMSTREAMHEADER: cbStruct 0, fdwStatus 4, dwUser 8, pbSrc 12,
   cbSrcLength 16, cbSrcLengthUsed 20, dwSrcUser 24, pbDst 28, cbDstLength
   32, cbDstLengthUsed 36, dwDstUser 40, dwReservedDriver[10] 44. */
#define ASH_SIZE 84u

typedef struct afmt {
    uint32_t tag, channels, rate, avg, align, bits;
    /* MS ADPCM */
    uint32_t spb; /* samples per block */
    int16_t coef[32][2];
    uint32_t ncoef;
} afmt;

typedef struct stream {
    uint32_t handle;
    afmt src, dst;
    AudioConverterRef conv; /* GSM, MP3 */
    /* the input of an AudioConverter call */
    const uint8_t *in;
    uint32_t in_left, in_used;
    AudioStreamPacketDescription pd;
    bool end;
} stream;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static stream **g_streams;
static uint32_t g_nstreams;

static void alock(void)
{
    if (pthread_mutex_lock(&g_lock) != 0) rt_fatal("MSACM32: lock failed");
}

static void aunlock(void)
{
    if (pthread_mutex_unlock(&g_lock) != 0) rt_fatal("MSACM32: unlock failed");
}

/* ---------------------------------------------------------------- formats */

static bool read_fmt(uint32_t g, afmt *f)
{
    uint32_t extra = 0, i;
    if (g == 0 || !rt_va_is_committed(g, 16)) return false;
    memset(f, 0, sizeof *f);
    f->tag = rd16(g);
    f->channels = rd16(g + 2);
    f->rate = rd32(g + 4);
    f->avg = rd32(g + 8);
    f->align = rd16(g + 12);
    f->bits = rd16(g + 14);
    if (f->tag != TAG_PCM)
    {
        if (!rt_va_is_committed(g, 18)) return false;
        extra = rd16(g + 16);
        if (!rt_va_is_committed(g, 18 + extra)) return false;
    }
    if (f->channels == 0 || f->channels > 2 || f->rate == 0 || f->rate > 192000 || f->align == 0) return false;
    switch (f->tag)
    {
    case TAG_PCM:
        return (f->bits == 8 || f->bits == 16) && f->align == f->channels * f->bits / 8u;
    case TAG_MSADPCM:
        /* wSamplesPerBlock 18, wNumCoef 20, aCoef 22 (pairs of int16). */
        if (extra < 4 || f->bits != 4) return false;
        f->spb = rd16(g + 18);
        f->ncoef = rd16(g + 20);
        if (f->ncoef < 7 || f->ncoef > 32 || extra < 4 + 4 * f->ncoef) return false;
        for (i = 0; i < f->ncoef; i++)
        {
            f->coef[i][0] = (int16_t)rd16(g + 22 + 4 * i);
            f->coef[i][1] = (int16_t)rd16(g + 24 + 4 * i);
        }
        return f->align > 7u * f->channels && f->spb == (f->align - 7u * f->channels) * 8u / (4u * f->channels) + 2u;
    case TAG_IMAADPCM:
        if (extra < 2 || f->bits != 4) return false;
        f->spb = rd16(g + 18);
        return f->align > 4u * f->channels && (f->align % (4u * f->channels)) == 0 &&
               f->spb == (f->align - 4u * f->channels) * 8u / (4u * f->channels) + 1u;
    case TAG_ALAW:
    case TAG_MULAW:
        return f->bits == 8 && f->align == f->channels;
    case TAG_GSM610:
        /* wSamplesPerBlock 18 */
        if (extra < 2 || f->channels != 1 || f->align != 65) return false;
        f->spb = rd16(g + 18);
        return f->spb == 320;
    case TAG_MP3:
        return true;
    default:
        return false;
    }
}

static void pcm16_of(const afmt *src, afmt *dst)
{
    memset(dst, 0, sizeof *dst);
    dst->tag = TAG_PCM;
    dst->channels = src->channels;
    dst->rate = src->rate;
    dst->bits = 16;
    dst->align = 2u * src->channels;
    dst->avg = dst->align * dst->rate;
}

static void write_pcm_fmt(uint32_t g, uint32_t size, const afmt *f)
{
    wr16(g, TAG_PCM);
    wr16(g + 2, (uint16_t)f->channels);
    wr32(g + 4, f->rate);
    wr32(g + 8, f->avg);
    wr16(g + 12, (uint16_t)f->align);
    wr16(g + 14, (uint16_t)f->bits);
    if (size >= 18) wr16(g + 16, 0);
}

/* MMRESULT acmFormatSuggest(HACMDRIVER, LPWAVEFORMATEX src, LPWAVEFORMATEX
   dst, DWORD dst_size, DWORD flags) */
void rt_acmFormatSuggest(_cpu *cpu)
{
    uint32_t had = ARG(0), src = ARG(1), dst = ARG(2), size = ARG(3), flags = ARG(4);
    afmt s, d;
    if (had != 0) rt_fatal("acmFormatSuggest: a driver handle is not supported");
    if (flags & ~(ACM_FORMATSUGGESTF_WFORMATTAG | ACM_FORMATSUGGESTF_NCHANNELS | ACM_FORMATSUGGESTF_NSAMPLESPERSEC |
                  ACM_FORMATSUGGESTF_WBITSPERSAMPLE))
    {
        RET(MMSYSERR_INVALFLAG);
        return;
    }
    if (dst == 0 || size < 16 || !rt_va_is_committed(dst, size))
    {
        RET(MMSYSERR_INVALPARAM);
        return;
    }
    if (!read_fmt(src, &s))
    {
        RET(ACMERR_NOTPOSSIBLE);
        return;
    }
    pcm16_of(&s, &d);
    if (s.tag == TAG_PCM) d.bits = s.bits;
    if ((flags & ACM_FORMATSUGGESTF_WFORMATTAG) && rd16(dst) != TAG_PCM)
    {
        RET(ACMERR_NOTPOSSIBLE); /* no encoders */
        return;
    }
    if (flags & ACM_FORMATSUGGESTF_NCHANNELS)
    {
        d.channels = rd16(dst + 2);
        if ((d.channels != 1 && d.channels != 2) || (s.tag != TAG_PCM && d.channels != s.channels))
        {
            RET(ACMERR_NOTPOSSIBLE);
            return;
        }
    }
    if ((flags & ACM_FORMATSUGGESTF_NSAMPLESPERSEC) && rd32(dst + 4) != s.rate)
    {
        RET(ACMERR_NOTPOSSIBLE); /* no sample rate conversion */
        return;
    }
    if (flags & ACM_FORMATSUGGESTF_WBITSPERSAMPLE)
    {
        d.bits = rd16(dst + 14);
        if ((d.bits != 8 && d.bits != 16) || (s.tag != TAG_PCM && d.bits != 16))
        {
            RET(ACMERR_NOTPOSSIBLE);
            return;
        }
    }
    d.align = d.channels * d.bits / 8u;
    d.avg = d.align * d.rate;
    write_pcm_fmt(dst, size, &d);
    RET(MMSYSERR_NOERROR);
}

/* ---------------------------------------------------------------- AudioConverter codecs */

static OSStatus conv_input(AudioConverterRef c, UInt32 *npackets, AudioBufferList *io, AudioStreamPacketDescription **pd,
                           void *ctx);

/* The length of the MPEG audio frame at p (0: not a frame header). */
static uint32_t mp3_frame(const uint8_t *p, uint32_t n, uint32_t *samples)
{
    static const uint16_t br1[16] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
    static const uint16_t br2[16] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 };
    static const uint32_t sr[3] = { 44100, 48000, 32000 };
    uint32_t ver, layer, bi, si, pad, rate, br;
    if (n < 4 || p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return 0;
    ver = (p[1] >> 3) & 3; /* 3: MPEG 1, 2: MPEG 2, 0: MPEG 2.5 */
    layer = (p[1] >> 1) & 3; /* 1: layer 3 */
    bi = p[2] >> 4;
    si = (p[2] >> 2) & 3;
    pad = (p[2] >> 1) & 1;
    if (ver == 1 || layer != 1 || bi == 0 || bi == 15 || si == 3) return 0;
    rate = sr[si] >> (ver == 3 ? 0 : ver == 2 ? 1 : 2);
    br = (ver == 3 ? br1[bi] : br2[bi]) * 1000u;
    *samples = ver == 3 ? 1152u : 576u;
    return (ver == 3 ? 144u : 72u) * br / rate + pad;
}

/* MPEG audio Layer 3 frame sizes at a sample rate: MPEG 1 has 1152 samples
   in each frame and 32 to 320 kbit/s; MPEG 2 and 2.5 have 576 samples and
   8 to 160 kbit/s. */
static uint32_t mp3_frame_samples(uint32_t rate)
{
    return rate >= 32000 ? 1152u : 576u;
}

static uint32_t mp3_min_frame(uint32_t rate)
{
    uint32_t n = rate >= 32000 ? 144u * 32000u / rate : 72u * 8000u / rate;
    return n == 0 ? 1u : n;
}

static uint32_t mp3_max_frame(uint32_t rate)
{
    return (rate >= 32000 ? 144u * 320000u / rate : 72u * 160000u / rate) + 1u;
}

static bool open_converter(stream *s)
{
    AudioStreamBasicDescription in, out;
    memset(&in, 0, sizeof in);
    memset(&out, 0, sizeof out);
    in.mSampleRate = s->src.rate;
    in.mChannelsPerFrame = s->src.channels;
    if (s->src.tag == TAG_GSM610)
    {
        in.mFormatID = kAudioFormatMicrosoftGSM;
        in.mBytesPerPacket = 65;
        in.mFramesPerPacket = 320;
    }
    else
    {
        in.mFormatID = kAudioFormatMPEGLayer3;
        in.mFramesPerPacket = mp3_frame_samples(s->src.rate);
    }
    out.mSampleRate = s->src.rate;
    out.mFormatID = kAudioFormatLinearPCM;
    out.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    out.mChannelsPerFrame = s->src.channels;
    out.mBitsPerChannel = 16;
    out.mBytesPerFrame = 2u * s->src.channels;
    out.mBytesPerPacket = out.mBytesPerFrame;
    out.mFramesPerPacket = 1;
    return AudioConverterNew(&in, &out, &s->conv) == noErr;
}

#define NO_MORE_INPUT ((OSStatus)0x52544E44) /* "RTND" */

static OSStatus conv_input(AudioConverterRef c, UInt32 *npackets, AudioBufferList *io, AudioStreamPacketDescription **pd,
                           void *ctx)
{
    stream *s = ctx;
    uint32_t len, samples = 0;
    (void)c;
    if (s->in_left == 0)
    {
        *npackets = 0;
        return s->end ? noErr : NO_MORE_INPUT;
    }
    if (s->src.tag == TAG_GSM610)
    {
        if (s->in_left < 65)
        {
            *npackets = 0;
            return s->end ? noErr : NO_MORE_INPUT;
        }
        len = 65;
    }
    else
    {
        /* Skip to the next frame header. */
        while (s->in_left >= 4 && mp3_frame(s->in, s->in_left, &samples) == 0)
        {
            s->in++;
            s->in_left--;
            s->in_used++;
        }
        len = s->in_left >= 4 ? mp3_frame(s->in, s->in_left, &samples) : 0;
        if (len == 0 || len > s->in_left)
        {
            *npackets = 0;
            return s->end ? noErr : NO_MORE_INPUT;
        }
    }
    io->mNumberBuffers = 1;
    io->mBuffers[0].mData = (void *)s->in;
    io->mBuffers[0].mDataByteSize = len;
    io->mBuffers[0].mNumberChannels = s->src.channels;
    if (pd != NULL)
    {
        s->pd.mStartOffset = 0;
        s->pd.mVariableFramesInPacket = 0;
        s->pd.mDataByteSize = len;
        *pd = &s->pd;
    }
    *npackets = 1;
    s->in += len;
    s->in_left -= len;
    s->in_used += len;
    return noErr;
}

/* ---------------------------------------------------------------- decoders */

static int16_t clamp16(int32_t v)
{
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

static void put16(uint8_t *p, int16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)((uint16_t)v >> 8);
}

static int16_t get16(const uint8_t *p)
{
    return (int16_t)(p[0] | (p[1] << 8));
}

static void msadpcm_block(const afmt *f, const uint8_t *in, uint8_t *out)
{
    static const int32_t adapt[16] = { 230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230 };
    uint32_t ch = f->channels, c, i;
    int32_t c1[2], c2[2], delta[2], s1[2], s2[2];
    const uint8_t *p = in;
    uint8_t *o = out;
    for (c = 0; c < ch; c++)
    {
        uint32_t pi = *p++;
        if (pi >= f->ncoef) pi = 0;
        c1[c] = f->coef[pi][0];
        c2[c] = f->coef[pi][1];
    }
    for (c = 0; c < ch; c++, p += 2) delta[c] = get16(p);
    for (c = 0; c < ch; c++, p += 2) s1[c] = get16(p);
    for (c = 0; c < ch; c++, p += 2) s2[c] = get16(p);
    for (c = 0; c < ch; c++, o += 2) put16(o, (int16_t)s2[c]);
    for (c = 0; c < ch; c++, o += 2) put16(o, (int16_t)s1[c]);
    for (i = 0; i < (f->spb - 2u) * ch; i++)
    {
        uint32_t nib = (i & 1u) ? (p[i >> 1] & 0x0Fu) : (p[i >> 1] >> 4);
        uint32_t cc = ch == 2 ? (i & 1u) : 0u;
        int32_t sn = (nib & 8u) ? (int32_t)nib - 16 : (int32_t)nib;
        int32_t pred = (s1[cc] * c1[cc] + s2[cc] * c2[cc]) / 256 + sn * delta[cc];
        int16_t v = clamp16(pred);
        s2[cc] = s1[cc];
        s1[cc] = v;
        delta[cc] = adapt[nib] * delta[cc] / 256;
        if (delta[cc] < 16) delta[cc] = 16;
        put16(o, v);
        o += 2;
    }
}

static const int16_t ima_steps[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,    31,
    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,   130,   143,
    157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,   544,   598,   658,
    724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,
    3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static void imaadpcm_block(const afmt *f, const uint8_t *in, uint8_t *out)
{
    static const int idx_adj[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };
    uint32_t ch = f->channels, c, k;
    int32_t pred[2], idx[2];
    const uint8_t *p = in;
    for (c = 0; c < ch; c++, p += 4)
    {
        pred[c] = get16(p);
        idx[c] = p[2] > 88 ? 88 : p[2];
        put16(out + 2u * c, (int16_t)pred[c]);
    }
    /* Then groups of 4 bytes (8 samples) for each channel in turn. */
    for (k = 0; k < (f->spb - 1u) / 8u; k++)
        for (c = 0; c < ch; c++)
        {
            uint32_t j;
            for (j = 0; j < 8; j++)
            {
                uint32_t nib = (j & 1u) ? (p[j >> 1] >> 4) : (p[j >> 1] & 0x0Fu);
                int32_t step = ima_steps[idx[c]], diff = step >> 3;
                uint32_t sample = 1u + k * 8u + j;
                if (nib & 4u) diff += step;
                if (nib & 2u) diff += step >> 1;
                if (nib & 1u) diff += step >> 2;
                pred[c] = clamp16(pred[c] + ((nib & 8u) ? -diff : diff));
                idx[c] += idx_adj[nib & 7u];
                if (idx[c] < 0) idx[c] = 0;
                if (idx[c] > 88) idx[c] = 88;
                put16(out + 2u * (sample * ch + c), (int16_t)pred[c]);
            }
            p += 4;
        }
}

static int16_t alaw(uint8_t a)
{
    int32_t t, seg;
    a ^= 0x55;
    t = (a & 0x0F) << 4;
    seg = (a & 0x70) >> 4;
    if (seg == 0)
        t += 8;
    else
        t = (t + 0x108) << (seg - 1);
    return (int16_t)((a & 0x80) ? t : -t);
}

static int16_t mulaw(uint8_t u)
{
    int32_t t;
    u = (uint8_t)~u;
    t = ((u & 0x0F) << 3) + 0x84;
    t <<= (u & 0x70) >> 4;
    return (int16_t)((u & 0x80) ? (0x84 - t) : (t - 0x84));
}

/* ---------------------------------------------------------------- streams */

static stream *stream_of(uint32_t h) /* lock held */
{
    uint32_t i = h - 0x41430000u;
    if ((h & 0xFFFF0000u) != 0x41430000u || i == 0 || i > g_nstreams) return NULL;
    return g_streams[i - 1];
}

static bool supported_pair(const afmt *s, const afmt *d)
{
    if (d->tag != TAG_PCM || d->rate != s->rate) return false;
    if (s->tag == TAG_PCM) return true; /* 8/16 bits, mono/stereo */
    return d->bits == 16 && d->channels == s->channels;
}

/* MMRESULT acmStreamOpen(LPHACMSTREAM, HACMDRIVER, LPWAVEFORMATEX src,
   LPWAVEFORMATEX dst, LPWAVEFILTER, DWORD_PTR cb, DWORD_PTR instance, DWORD
   flags) */
void rt_acmStreamOpen(_cpu *cpu)
{
    uint32_t ph = ARG(0), flags = ARG(7);
    afmt s, d;
    stream *st;
    if (ARG(1) != 0) rt_fatal("acmStreamOpen: a driver handle is not supported");
    if (ARG(4) != 0) rt_fatal("acmStreamOpen: filters are not supported");
    if (flags & ACM_STREAMOPENF_ASYNC) rt_fatal("acmStreamOpen: asynchronous streams are not supported");
    if (flags & ~(ACM_STREAMOPENF_QUERY | ACM_STREAMOPENF_NONREALTIME))
    {
        RET(MMSYSERR_INVALFLAG);
        return;
    }
    if (!read_fmt(ARG(2), &s) || !read_fmt(ARG(3), &d) || !supported_pair(&s, &d))
    {
        RET(ACMERR_NOTPOSSIBLE);
        return;
    }
    st = calloc(1, sizeof *st);
    if (st == NULL) rt_fatal("out of host memory (ACM stream)");
    st->src = s;
    st->dst = d;
    if ((s.tag == TAG_GSM610 || s.tag == TAG_MP3) && !open_converter(st))
    {
        free(st);
        RET(ACMERR_NOTPOSSIBLE);
        return;
    }
    if (flags & ACM_STREAMOPENF_QUERY)
    {
        if (st->conv != NULL) AudioConverterDispose(st->conv);
        free(st);
        RET(MMSYSERR_NOERROR);
        return;
    }
    if (ph == 0 || !rt_va_is_committed(ph, 4))
    {
        if (st->conv != NULL) AudioConverterDispose(st->conv);
        free(st);
        RET(MMSYSERR_INVALPARAM);
        return;
    }
    alock();
    if (g_nstreams >= 0xFFFFu) rt_fatal("MSACM32: more than 65535 streams");
    {
        stream **n = realloc(g_streams, (g_nstreams + 1u) * sizeof *n);
        if (n == NULL) rt_fatal("out of host memory (ACM streams)");
        g_streams = n;
    }
    g_streams[g_nstreams++] = st;
    st->handle = 0x41430000u | g_nstreams;
    aunlock();
    wr32(ph, st->handle);
    RET(MMSYSERR_NOERROR);
}

/* The output bytes for n input bytes (an upper bound). */
static uint64_t out_for_in(const stream *s, uint64_t n)
{
    switch (s->src.tag)
    {
    case TAG_PCM: return n / s->src.align * s->dst.align;
    case TAG_ALAW:
    case TAG_MULAW: return n / s->src.align * s->dst.align;
    case TAG_MSADPCM:
    case TAG_IMAADPCM:
    case TAG_GSM610: return n / s->src.align * s->src.spb * s->dst.align;
    default:
    {
        /* MP3: the frames of the smallest size (MPEG 1: 32 kbit/s; MPEG 2
           and 2.5: 8 kbit/s), and two frames that the decoder keeps. */
        uint64_t frames = n / mp3_min_frame(s->src.rate) + 2u;
        return frames * mp3_frame_samples(s->src.rate) * s->dst.align;
    }
    }
}

/* The input bytes for n output bytes. */
static uint64_t in_for_out(const stream *s, uint64_t n)
{
    switch (s->src.tag)
    {
    case TAG_PCM:
    case TAG_ALAW:
    case TAG_MULAW: return n / s->dst.align * s->src.align;
    case TAG_MSADPCM:
    case TAG_IMAADPCM:
    case TAG_GSM610: return (n / s->dst.align + s->src.spb - 1u) / s->src.spb * s->src.align;
    default:
    {
        uint64_t spf = mp3_frame_samples(s->src.rate), frames = (n / s->dst.align + spf - 1u) / spf;
        return frames * mp3_max_frame(s->src.rate);
    }
    }
}

/* MMRESULT acmStreamSize(HACMSTREAM, DWORD in, LPDWORD out, DWORD flags) */
void rt_acmStreamSize(_cpu *cpu)
{
    uint32_t n = ARG(1), pout = ARG(2), flags = ARG(3);
    stream *s;
    uint64_t r;
    alock();
    s = stream_of(ARG(0));
    aunlock();
    if (s == NULL)
    {
        RET(MMSYSERR_INVALHANDLE);
        return;
    }
    if (pout == 0 || flags > ACM_STREAMSIZEF_DESTINATION)
    {
        RET(flags > ACM_STREAMSIZEF_DESTINATION ? MMSYSERR_INVALFLAG : MMSYSERR_INVALPARAM);
        return;
    }
    r = flags == ACM_STREAMSIZEF_SOURCE ? out_for_in(s, n) : in_for_out(s, n);
    if (r > 0xFFFFFFFFu) r = 0xFFFFFFFFu;
    wr32(pout, (uint32_t)r);
    RET(r == 0 ? ACMERR_NOTPOSSIBLE : MMSYSERR_NOERROR);
}

static uint32_t header_arg(uint32_t ash)
{
    if (ash == 0 || !rt_va_is_committed(ash, ASH_SIZE) || rd32(ash) < ASH_SIZE) return MMSYSERR_INVALPARAM;
    return MMSYSERR_NOERROR;
}

/* MMRESULT acmStreamPrepareHeader(HACMSTREAM, LPACMSTREAMHEADER, DWORD) */
void rt_acmStreamPrepareHeader(_cpu *cpu)
{
    uint32_t ash = ARG(1), r = header_arg(ash);
    stream *s;
    alock();
    s = stream_of(ARG(0));
    aunlock();
    if (s == NULL)
    {
        RET(MMSYSERR_INVALHANDLE);
        return;
    }
    if (r != MMSYSERR_NOERROR || ARG(2) != 0)
    {
        RET(r != MMSYSERR_NOERROR ? r : MMSYSERR_INVALFLAG);
        return;
    }
    if (!rt_va_is_committed(rd32(ash + 12), rd32(ash + 16)) || !rt_va_is_committed(rd32(ash + 28), rd32(ash + 32)))
    {
        RET(MMSYSERR_INVALPARAM);
        return;
    }
    /* The driver area keeps the lengths of the prepare. */
    wr32(ash + 44, rd32(ash + 16));
    wr32(ash + 48, rd32(ash + 32));
    wr32(ash + 4, (rd32(ash + 4) | STATUSF_PREPARED | STATUSF_DONE) & ~STATUSF_INQUEUE);
    RET(MMSYSERR_NOERROR);
}

/* MMRESULT acmStreamUnprepareHeader(HACMSTREAM, LPACMSTREAMHEADER, DWORD) */
void rt_acmStreamUnprepareHeader(_cpu *cpu)
{
    uint32_t ash = ARG(1), r = header_arg(ash);
    stream *s;
    alock();
    s = stream_of(ARG(0));
    aunlock();
    if (s == NULL)
    {
        RET(MMSYSERR_INVALHANDLE);
        return;
    }
    if (r != MMSYSERR_NOERROR || ARG(2) != 0)
    {
        RET(r != MMSYSERR_NOERROR ? r : MMSYSERR_INVALFLAG);
        return;
    }
    if (!(rd32(ash + 4) & STATUSF_PREPARED))
    {
        RET(ACMERR_UNPREPARED);
        return;
    }
    if (rd32(ash + 16) != rd32(ash + 44) || rd32(ash + 32) != rd32(ash + 48))
    {
        RET(MMSYSERR_INVALPARAM);
        return;
    }
    wr32(ash + 4, rd32(ash + 4) & ~STATUSF_PREPARED);
    RET(MMSYSERR_NOERROR);
}

/* MMRESULT acmStreamConvert(HACMSTREAM, LPACMSTREAMHEADER, DWORD flags) */
void rt_acmStreamConvert(_cpu *cpu)
{
    uint32_t ash = ARG(1), flags = ARG(2), r = header_arg(ash), src, dst, slen, dlen, used = 0, made = 0;
    const uint8_t *in;
    uint8_t *out;
    stream *s;
    alock();
    s = stream_of(ARG(0));
    aunlock();
    if (s == NULL)
    {
        RET(MMSYSERR_INVALHANDLE);
        return;
    }
    if (r != MMSYSERR_NOERROR)
    {
        RET(r);
        return;
    }
    if (flags & ~(ACM_STREAMCONVERTF_BLOCKALIGN | ACM_STREAMCONVERTF_START | ACM_STREAMCONVERTF_END))
    {
        RET(MMSYSERR_INVALFLAG);
        return;
    }
    if (!(rd32(ash + 4) & STATUSF_PREPARED))
    {
        RET(ACMERR_UNPREPARED);
        return;
    }
    src = rd32(ash + 12);
    slen = rd32(ash + 16);
    dst = rd32(ash + 28);
    dlen = rd32(ash + 32);
    if (!rt_va_is_committed(src, slen) || !rt_va_is_committed(dst, dlen))
    {
        RET(MMSYSERR_INVALPARAM);
        return;
    }
    in = (const uint8_t *)G2H(src);
    out = (uint8_t *)G2H(dst);
    wr32(ash + 4, (rd32(ash + 4) & ~STATUSF_DONE) | STATUSF_INQUEUE);
    switch (s->src.tag)
    {
    case TAG_PCM:
    {
        uint32_t frames = slen / s->src.align, fo = dlen / s->dst.align, i, c;
        if (frames > fo) frames = fo;
        for (i = 0; i < frames; i++)
        {
            int32_t v[2];
            for (c = 0; c < s->src.channels; c++)
                v[c] = s->src.bits == 8 ? ((int32_t)in[i * s->src.align + c] - 128) * 256 : get16(in + i * s->src.align + 2 * c);
            if (s->src.channels == 1) v[1] = v[0];
            if (s->src.channels == 2 && s->dst.channels == 1) v[0] = (v[0] + v[1]) / 2;
            for (c = 0; c < s->dst.channels; c++)
            {
                if (s->dst.bits == 8)
                    out[i * s->dst.align + c] = (uint8_t)((v[c] >> 8) + 128);
                else
                    put16(out + i * s->dst.align + 2 * c, (int16_t)v[c]);
            }
        }
        used = frames * s->src.align;
        made = frames * s->dst.align;
        break;
    }
    case TAG_ALAW:
    case TAG_MULAW:
    {
        uint32_t n = slen, i;
        if (n > dlen / 2u) n = dlen / 2u;
        n -= n % s->src.align;
        for (i = 0; i < n; i++) put16(out + 2 * i, s->src.tag == TAG_ALAW ? alaw(in[i]) : mulaw(in[i]));
        used = n;
        made = 2u * n;
        break;
    }
    case TAG_MSADPCM:
    case TAG_IMAADPCM:
    {
        uint32_t bsize = s->src.spb * s->dst.align;
        while (used + s->src.align <= slen && made + bsize <= dlen)
        {
            if (s->src.tag == TAG_MSADPCM)
                msadpcm_block(&s->src, in + used, out + made);
            else
                imaadpcm_block(&s->src, in + used, out + made);
            used += s->src.align;
            made += bsize;
        }
        if ((flags & ACM_STREAMCONVERTF_END) && used < slen && slen - used < s->src.align && made + bsize <= dlen &&
            !(flags & ACM_STREAMCONVERTF_BLOCKALIGN))
        {
            /* The last block is short: decode the samples that it has. */
            uint8_t blk[4096], tmp[2 * 2 * 8192];
            uint32_t have = slen - used, hdr = (s->src.tag == TAG_MSADPCM ? 7u : 4u) * s->src.channels, samples;
            if (s->src.align <= sizeof blk && bsize <= sizeof tmp && have > hdr)
            {
                memset(blk, 0, sizeof blk);
                memcpy(blk, in + used, have);
                if (s->src.tag == TAG_MSADPCM)
                {
                    msadpcm_block(&s->src, blk, tmp);
                    samples = 2u + (have - hdr) * 2u / s->src.channels;
                }
                else
                {
                    imaadpcm_block(&s->src, blk, tmp);
                    samples = 1u + (have - hdr) / (4u * s->src.channels) * 8u;
                }
                if (samples > s->src.spb) samples = s->src.spb;
                memcpy(out + made, tmp, samples * s->dst.align);
                made += samples * s->dst.align;
                used = slen;
            }
        }
        break;
    }
    default:
    {
        /* GSM 6.10, MP3: the AudioConverter keeps the decoder state. */
        UInt32 frames = dlen / s->dst.align;
        AudioBufferList bl;
        OSStatus st;
        if (flags & ACM_STREAMCONVERTF_START) AudioConverterReset(s->conv);
        s->in = in;
        s->in_left = (flags & ACM_STREAMCONVERTF_BLOCKALIGN) && s->src.tag == TAG_GSM610 ? slen - slen % 65u : slen;
        s->in_used = 0;
        s->end = (flags & ACM_STREAMCONVERTF_END) != 0;
        bl.mNumberBuffers = 1;
        bl.mBuffers[0].mData = out;
        bl.mBuffers[0].mDataByteSize = frames * s->dst.align;
        bl.mBuffers[0].mNumberChannels = s->dst.channels;
        st = frames > 0 ? AudioConverterFillComplexBuffer(s->conv, conv_input, s, &frames, &bl, NULL) : noErr;
        if (st != noErr && st != NO_MORE_INPUT)
        {
            wr32(ash + 20, 0);
            wr32(ash + 36, 0);
            wr32(ash + 4, (rd32(ash + 4) & ~STATUSF_INQUEUE) | STATUSF_DONE);
            RET(ACMERR_NOTPOSSIBLE);
            return;
        }
        used = s->in_used;
        made = frames * s->dst.align;
        s->in = NULL;
        break;
    }
    }
    wr32(ash + 20, used);
    wr32(ash + 36, made);
    wr32(ash + 4, (rd32(ash + 4) & ~STATUSF_INQUEUE) | STATUSF_DONE);
    RET(MMSYSERR_NOERROR);
}

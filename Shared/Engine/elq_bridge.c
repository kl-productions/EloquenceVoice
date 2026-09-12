/* The engine behind an iOS speech synthesis extension. See elq_bridge.h for
 * which thread calls what.
 *
 * Two facts about the engine shape this file. Samples and index replies are
 * delivered only while somebody asks eciSpeaking or eciSynchronize -- asking
 * is what drives the queue of results -- and an instance refuses a call that
 * arrives while another call on it is running. So one worker thread owns
 * every ECI call: it takes requests from the control thread, and polls while
 * an utterance is going.
 *
 * Samples cross from that thread to the render thread in a single-producer,
 * single-consumer ring: the worker (inside the callback) only moves `head',
 * the render thread only moves `tail'. A cancel cannot move `tail' itself, so
 * it leaves `flushTo' for the render thread to act on.
 */

#include "elq_bridge.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __APPLE__
#include <pthread/qos.h>
#endif

#include "eci.h"

enum { ENGINE_FRAME = 1024, MAX_SYNTHS = 16, MAX_LANGUAGES = 32, POLL_MS = 2 };

/* Inserted after every utterance. Its reply means every sample of that
   utterance has been handed over. */
#define END_INDEX 0x454c5121

/* 262,144 samples. When it is full the callback answers eciDataNotProcessed
   and the engine offers the same buffer again 30 ms later, so a long document
   is paced rather than cut short. */
#define RING_SIZE ((size_t)1 << 18)
#define RING_MASK (RING_SIZE - 1)

/* The one language that takes UTF-8 itself; the IBM nine take Windows-1252. */
#define LANGUAGE_POLISH 0x00110000u

typedef struct {
    ECIHand            h;
    ECIFilterHand      filter;
    void              *filterEntry;   /* the reader is handed its address */
    ECIFilterAttribute filterAttrib;
    uint32_t           language;
    short              frame[ENGINE_FRAME];
    float             *ring;
    atomic_size_t      head;
    atomic_size_t      tail;
    atomic_size_t      flushTo;       /* 0, or a head position plus one */
    atomic_int         aborting;
    atomic_uint        generation;
    atomic_uint        finishedGeneration;
} ElqSynth;

struct ElqHost {
    pthread_mutex_t     lock;
    pthread_cond_t      wake;
    pthread_t           worker;
    int                 workerStarted;

    /* What the control thread has asked for, under `lock'. */
    int                 quit;
    int                 cancelPending;
    char               *pendingDocument;
    uint32_t            pendingLanguage;
    ElqVoiceSettings    pendingVoice;
    unsigned            pendingRequest;

    /* A speak request is numbered when it is posted and marked served once
       the worker has started it, so the render thread never reports the
       previous utterance's end as the end of one still waiting. */
    atomic_uint         requested;
    atomic_uint         served;

    int32_t             sampleRate;
    ElqSynth           *synths[MAX_SYNTHS];   /* the worker's, once it runs */
    int                 count;
    _Atomic(ElqSynth *) current;
};

/* ---- the engine's callback, on the worker thread ---------------------- */

static int ECICALL on_message(ECIHand h, ECIMessage message, int param,
                              void *data)
{
    ElqSynth *s = data;
    size_t    head, used, count, i;

    (void)h;

    if (message == eciIndexReply) {
        if (param == END_INDEX)
            atomic_store(&s->finishedGeneration,
                         atomic_load(&s->generation));
        return eciDataProcessed;
    }
    if (message != eciWaveformBuffer)
        return eciDataProcessed;
    if (atomic_load(&s->aborting))
        return eciDataAbort;

    head = atomic_load(&s->head);
    used = (head - atomic_load(&s->tail)) & RING_MASK;
    count = (size_t)param;
    if (count > RING_MASK - used)
        return eciDataNotProcessed;

    for (i = 0; i < count; i++)
        s->ring[(head + i) & RING_MASK] = (float)s->frame[i] / 32768.0f;
    atomic_store(&s->head, (head + count) & RING_MASK);
    return eciDataProcessed;
}

/* ---- the render thread ----------------------------------------------- */

static size_t synth_render(ElqSynth *s, float *out, size_t frames,
                           int32_t *finished)
{
    /* Both generations are read before `head', so an end reported between
       the two reads cannot claim samples this call never saw. */
    unsigned gen = atomic_load(&s->generation);
    unsigned fin = atomic_load(&s->finishedGeneration);
    size_t   flush = atomic_exchange(&s->flushTo, 0);
    size_t   tail, head, avail, n, i;

    if (flush != 0)
        atomic_store(&s->tail, flush - 1);
    tail = atomic_load(&s->tail);
    head = atomic_load(&s->head);
    avail = (head - tail) & RING_MASK;
    n = avail < frames ? avail : frames;

    for (i = 0; i < n; i++)
        out[i] = s->ring[(tail + i) & RING_MASK];
    atomic_store(&s->tail, (tail + n) & RING_MASK);

    *finished = fin == gen && n == avail
             && atomic_load(&s->generation) == gen;
    return n;
}

/* ---- text ------------------------------------------------------------ */

static size_t put_utf8(char *out, uint32_t cp)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xc0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xe0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

/* One code point off the front, or U+FFFD for a byte that starts nothing. */
static uint32_t take_utf8(const unsigned char **at)
{
    const unsigned char *p = *at;
    uint32_t cp;
    int      extra, i;

    if (p[0] < 0x80) {
        *at = p + 1;
        return p[0];
    }
    if ((p[0] & 0xe0) == 0xc0) {
        cp = p[0] & 0x1f;
        extra = 1;
    } else if ((p[0] & 0xf0) == 0xe0) {
        cp = p[0] & 0x0f;
        extra = 2;
    } else if ((p[0] & 0xf8) == 0xf0) {
        cp = p[0] & 0x07;
        extra = 3;
    } else {
        *at = p + 1;
        return 0xfffd;
    }
    for (i = 1; i <= extra; i++) {
        if ((p[i] & 0xc0) != 0x80) {
            *at = p + i;
            return 0xfffd;
        }
        cp = (cp << 6) | (p[i] & 0x3f);
    }
    *at = p + extra + 1;
    return cp;
}

/* The byte Windows-1252 has for a code point, or -1. Curly quotes become
   straight ones, as IBM's own Speech Dispatcher module does, so a contraction
   is not read as a symbol. */
static int cp1252_byte(uint32_t cp)
{
    static const uint16_t high[32] = {
        0x20ac, 0,      0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
        0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017d, 0,
        0,      0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
        0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0,      0x017e, 0x0178,
    };
    int i;

    if (cp == 0x2018 || cp == 0x2019)
        return '\'';
    if (cp == 0x201c || cp == 0x201d)
        return '"';
    if (cp == 0x00a0)
        return ' ';
    if (cp < 0x80 || (cp >= 0xa0 && cp <= 0xff))
        return (int)cp;
    for (i = 0; i < 32; i++)
        if (high[i] != 0 && high[i] == cp)
            return 0x80 + i;
    return -1;
}

/* UTF-8 in, the text as this language's engine reads it out. */
static char *engine_text(uint32_t language, const char *utf8)
{
    const unsigned char *at = (const unsigned char *)utf8;
    size_t len = strlen(utf8);
    char  *out, *o;

    if ((language & 0xffff0000u) == LANGUAGE_POLISH) {
        out = malloc(len + 1);
        if (out)
            memcpy(out, utf8, len + 1);
        return out;
    }

    out = malloc(len + 1);
    if (!out)
        return 0;
    o = out;
    while (*at) {
        int b = cp1252_byte(take_utf8(&at));

        *o++ = (char)(b < 0 ? ' ' : b);
    }
    *o = 0;
    return out;
}

static uint32_t entity_code_point(const char *name, size_t len)
{
    if (len >= 2 && name[0] == '#') {
        char    *end;
        unsigned long v = (name[1] == 'x' || name[1] == 'X')
                        ? strtoul(name + 2, &end, 16)
                        : strtoul(name + 1, &end, 10);

        return end == name + len && v > 0 && v <= 0x10ffff ? (uint32_t)v : 0;
    }
    if (len == 3 && !memcmp(name, "amp", 3))  return '&';
    if (len == 2 && !memcmp(name, "lt", 2))   return '<';
    if (len == 2 && !memcmp(name, "gt", 2))   return '>';
    if (len == 4 && !memcmp(name, "quot", 4)) return '"';
    if (len == 4 && !memcmp(name, "apos", 4)) return '\'';
    return 0;
}

/* A document with its markup taken out and its entities put back, still
   UTF-8. Used when the SSML reader is off or produced nothing. A break
   becomes a sentence end, which is the pause the engine has. */
static char *strip_markup(const char *in)
{
    size_t      len = strlen(in);
    char       *out = malloc(len * 2 + 1);
    char       *o = out;
    const char *p = in;

    if (!out)
        return 0;
    while (*p) {
        if (*p == '<') {
            const char *end = strchr(p, '>');

            if (!end)
                break;
            if (!strncmp(p, "<break", 6)) {
                *o++ = '.';
                *o++ = ' ';
            } else {
                *o++ = ' ';
            }
            p = end + 1;
        } else if (*p == '&') {
            const char *end = strchr(p, ';');
            uint32_t    cp = end && end - p <= 12
                           ? entity_code_point(p + 1, (size_t)(end - p - 1))
                           : 0;

            if (cp) {
                o += put_utf8(o, cp);
                p = end + 1;
            } else {
                *o++ = *p++;
            }
        } else {
            *o++ = *p++;
        }
    }
    *o = 0;
    return out;
}

static int looks_like_markup(const char *text)
{
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
        text++;
    return *text == '<';
}

static int has_speakable(const char *text)
{
    for (; *text; text++)
        if (*text != ' ' && *text != '\t' && *text != '\r' && *text != '\n')
            return 1;
    return 0;
}

/* ---- making a document one IBM's reader will take --------------------- */

/* What IBM's reader was measured to need, against this engine:
 *   - <speak> with exactly version="1.0" and an xml:lang, or it answers
 *     nothing at all for the whole document;
 *   - prosody given as a change: it reads a bare "100%" as +100%, where
 *     SSML 1.1 means the default, and a bare rate of "1.5" as the slowest
 *     speed there is;
 *   - no say-as it has no reader for: "currency" and "characters" each empty
 *     the whole document, so a second attempt goes without say-as at all. */

static const char *language_locale(uint32_t language)
{
    switch (language & ~(uint32_t)eciUnicodeCodeSet) {
    case 0x00010001: return "en-GB";
    case 0x00020000: return "es-ES";
    case 0x00020001: return "es-MX";
    case 0x00030000: return "fr-FR";
    case 0x00030001: return "fr-CA";
    case 0x00040000: return "de-DE";
    case 0x00050000: return "it-IT";
    case 0x00080000: return "ja-JP";
    case 0x00110000: return "pl-PL";
    default:         return "en-US";
    }
}

/* Whether the tag at `p' is <name ...> (or </name ...> when `closing'). */
static int tag_is(const char *p, const char *name, int closing)
{
    size_t n = strlen(name);

    if (p[0] != '<')
        return 0;
    p += 1;
    if (closing) {
        if (*p != '/')
            return 0;
        p++;
    }
    return !strncmp(p, name, n)
        && (p[n] == '>' || p[n] == '/' || p[n] == ' ' || p[n] == '\t'
            || p[n] == '\r' || p[n] == '\n');
}

/* A prosody value as the change IBM's reader expects. `value' runs to `end'. */
static char *write_prosody_value(char *o, const char *value, const char *end,
                                 int isRate)
{
    const char *p = value;
    int         digits = 0, dot = 0, percent = 0;

    while (p < end && ((*p >= '0' && *p <= '9') || *p == '.')) {
        if (*p == '.')
            dot++;
        else
            digits++;
        p++;
    }
    if (p < end && *p == '%') {
        percent = 1;
        p++;
    }
    if (digits > 0 && dot <= 1 && p == end && (percent || isRate)) {
        double v = strtod(value, 0);
        double change = percent ? v - 100.0 : v * 100.0 - 100.0;

        if (change < -100.0)
            change = -100.0;
        if (change > 1000.0)
            change = 1000.0;
        return o + sprintf(o, "%+d%%", (int)(change < 0 ? change - 0.5
                                                          : change + 0.5));
    }
    memcpy(o, value, (size_t)(end - value));
    return o + (end - value);
}

/* Copies <prosody ...> from `p' to the '>' at `close', making rate, pitch and
   volume into changes. */
static char *write_prosody_tag(char *o, const char *p, const char *close)
{
    const char *q = p + 8;

    memcpy(o, p, 8);
    o += 8;
    while (q < close) {
        const char *name, *nameEnd, *value;
        char        quote;
        int         which;

        if (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n' || *q == '/') {
            *o++ = *q++;
            continue;
        }
        name = q;
        while (q < close && *q != '=' && *q != ' ' && *q != '\t'
               && *q != '\r' && *q != '\n' && *q != '/')
            q++;
        nameEnd = q;
        if (q + 1 >= close || *q != '=' || (q[1] != '"' && q[1] != '\'')) {
            memcpy(o, name, (size_t)(q - name));
            o += q - name;
            if (q < close && *q == '=')
                *o++ = *q++;
            continue;
        }
        quote = q[1];
        value = q + 2;
        q = value;
        while (q < close && *q != quote)
            q++;

        memcpy(o, name, (size_t)(nameEnd - name));
        o += nameEnd - name;
        *o++ = '=';
        *o++ = quote;
        which = nameEnd - name == 4 && !strncmp(name, "rate", 4)   ? 1
              : nameEnd - name == 5 && !strncmp(name, "pitch", 5)  ? 2
              : nameEnd - name == 6 && !strncmp(name, "volume", 6) ? 2
              : 0;
        if (which)
            o = write_prosody_value(o, value, q, which == 1);
        else {
            memcpy(o, value, (size_t)(q - value));
            o += q - value;
        }
        *o++ = quote;
        if (q < close)
            q++;
    }
    *o++ = '>';
    return o;
}

/* The document rewritten for IBM's reader, with backticks made apostrophes:
   in annotation mode a backtick starts a command. */
static char *document_for_reader(const char *in, uint32_t language,
                                 int dropSayAs)
{
    size_t      len = strlen(in);
    char       *out = malloc(len * 2 + 128);
    char       *o = out;
    const char *p = in;
    int         speakDone = 0;

    if (!out)
        return 0;
    while (*p) {
        const char *close;

        if (*p != '<') {
            *o++ = *p == '`' ? '\'' : *p;
            p++;
            continue;
        }
        close = strchr(p, '>');
        if (!close) {
            while (*p)
                *o++ = *p++ == '`' ? '\'' : p[-1];
            break;
        }
        if (!speakDone && tag_is(p, "speak", 0)) {
            o += sprintf(o, "<speak version=\"1.0\" xml:lang=\"%s\"%s",
                         language_locale(language),
                         close > p && close[-1] == '/' ? "/>" : ">");
            speakDone = 1;
        } else if (tag_is(p, "prosody", 0)) {
            o = write_prosody_tag(o, p, close);
        } else if (dropSayAs
                   && (tag_is(p, "say-as", 0) || tag_is(p, "say-as", 1))) {
            /* the tag goes and what it held stays */
        } else {
            memcpy(o, p, (size_t)(close - p + 1));
            o += close - p + 1;
        }
        p = close + 1;
    }
    *o = 0;
    return out;
}

/* ---- one engine instance per language, on the worker thread ----------- */

static void apply_voice(ElqSynth *s, const ElqVoiceSettings *v)
{
    static const int params[] = {
        eciSpeed, eciPitchBaseline, eciPitchFluctuation, eciHeadSize,
        eciRoughness, eciBreathiness, eciVolume,
    };
    const int values[] = {
        v->speed, v->pitch, v->pitchFluctuation, v->headSize,
        v->roughness, v->breathiness, v->volume,
    };
    int preset = v->preset >= 1 && v->preset <= ECI_PRESET_VOICES
               ? v->preset : 1;
    size_t i;

    eciCopyVoice(s->h, preset, 0);
    for (i = 0; i < sizeof params / sizeof params[0]; i++) {
        int most = params[i] == eciSpeed ? 250 : 100;

        if (values[i] >= 0)
            eciSetVoiceParam(s->h, 0, params[i],
                             values[i] > most ? most : values[i]);
    }
}

/* Stops handing samples over and waits for the engine to finish the message
   it is on, which it cannot abandon; eciSynchronize is also what delivers the
   callbacks that answer eciDataAbort. */
static void synth_cancel(ElqSynth *s)
{
    atomic_store(&s->aborting, 1);
    eciStop(s->h);
    eciSynchronize(s->h);
    atomic_store(&s->flushTo, atomic_load(&s->head) + 1);
}

/* Queues an utterance and answers whether there is anything to wait for. */
static int synth_speak(ElqSynth *s, const char *document,
                       const ElqVoiceSettings *v)
{
    unsigned gen = atomic_load(&s->generation) + 1;
    int      queued = 0;

    atomic_store(&s->generation, gen);
    atomic_store(&s->aborting, 0);
    apply_voice(s, v);

    /* IBM's reader takes the document as UTF-8 and answers with annotated
       text in the engine's own bytes. It writes into what it is given, so it
       gets a rewritten copy. */
    if (v->useSSMLReader && s->filter && looks_like_markup(document)) {
        int attempt;

        for (attempt = 0; attempt < 2 && !queued; attempt++) {
            char *copy = document_for_reader(document, s->language, attempt);
            char *read = 0;

            if (!copy)
                break;
            eciGetFilteredText(s->h, s->filter, copy, &read);
            if (read && has_speakable(read)) {
                eciSetParam(s->h, eciInputType, 1);
                queued = eciAddText(s->h, read);
            }
            free(copy);
        }
    }

    if (!queued) {
        char *plain = strip_markup(document);
        char *bytes = plain ? engine_text(s->language, plain) : 0;

        eciSetParam(s->h, eciInputType, 0);
        queued = bytes && has_speakable(bytes) && eciAddText(s->h, bytes);
        free(bytes);
        free(plain);
    }

    if (queued)
        queued = eciInsertIndex(s->h, END_INDEX) && eciSynthesize(s->h);
    if (!queued)
        atomic_store(&s->finishedGeneration, gen);
    return queued;
}

static void synth_destroy(ElqSynth *s)
{
    if (!s)
        return;
    if (s->h != NULL_ECI_HAND) {
        synth_cancel(s);
        if (s->filter)
            eciDeleteFilter(s->h, s->filter);
        eciDelete(s->h);
    }
    free(s->ring);
    free(s);
}

static ElqSynth *synth_create(uint32_t language, int32_t sampleRate)
{
    ElqSynth *s = calloc(1, sizeof *s);

    if (!s)
        return 0;
    s->language = language;
    s->ring = calloc(RING_SIZE, sizeof *s->ring);
    if (!s->ring)
        goto fail;

    s->h = eciNewEx((int)language);
    if (s->h == NULL_ECI_HAND)
        goto fail;
    if (sampleRate > 0 && eciSetParam(s->h, eciSampleRate, sampleRate) < 0)
        goto fail;

    eciRegisterCallback(s->h, on_message, s);
    if (!eciSetOutputBuffer(s->h, ENGINE_FRAME, s->frame))
        goto fail;

    /* The SSML reader has to be set up before any text or index is queued,
       or eciNewFilter refuses (docs/quirks.md). Registering answers 0 when it
       worked; activating's answer is ignored, as test/lib/dll.py does. */
    s->filterEntry = (void *)ssmlFilterGetObject;
    if (eciRegisterFilter(s->h, 0, &s->filterEntry, &s->filterAttrib, 1) == 0) {
        s->filter = eciNewFilter(s->h, 0, 1);
        if (s->filter)
            eciActivateFilter(s->h, s->filter);
    }
    return s;

fail:
    if (s->h != NULL_ECI_HAND)
        eciDelete(s->h);
    free(s->ring);
    free(s);
    return 0;
}

static ElqSynth *host_synth_for(ElqHost *host, uint32_t language)
{
    ElqSynth *s;
    int       i;

    for (i = 0; i < host->count; i++)
        if (host->synths[i]->language == language)
            return host->synths[i];
    if (host->count == MAX_SYNTHS)
        return 0;
    s = synth_create(language, host->sampleRate);
    if (s)
        host->synths[host->count++] = s;
    return s;
}

/* ---- the worker ------------------------------------------------------- */

/* With `lock' held: sleep until asked for something or POLL_MS passes. */
static void wait_a_moment(ElqHost *host)
{
    struct timespec until;

    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += POLL_MS * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    pthread_cond_timedwait(&host->wake, &host->lock, &until);
}

static void *worker_main(void *arg)
{
    ElqHost *host = arg;
    int      speaking = 0;

#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif

    pthread_mutex_lock(&host->lock);
    for (;;) {
        char            *document;
        uint32_t         language;
        ElqVoiceSettings voice;
        unsigned         request;
        int              cancel;
        ElqSynth        *s;

        if (speaking) {
            if (!host->quit && !host->cancelPending && !host->pendingDocument)
                wait_a_moment(host);
        } else {
            while (!host->quit && !host->cancelPending
                   && !host->pendingDocument)
                pthread_cond_wait(&host->wake, &host->lock);
        }
        if (host->quit)
            break;

        document = host->pendingDocument;
        language = host->pendingLanguage;
        voice = host->pendingVoice;
        request = host->pendingRequest;
        cancel = host->cancelPending || document != 0;
        host->pendingDocument = 0;
        host->cancelPending = 0;
        pthread_mutex_unlock(&host->lock);

        s = atomic_load(&host->current);
        if (cancel && s) {
            synth_cancel(s);
            speaking = 0;
        }
        if (document) {
            ElqSynth *next = host_synth_for(host, language);

            if (next) {
                atomic_store(&host->current, next);
                speaking = synth_speak(next, document, &voice);
            }
            atomic_store(&host->served, request);
            free(document);
        }
        if (speaking) {
            /* Asking is what delivers the samples and the end index. */
            s = atomic_load(&host->current);
            speaking = eciSpeaking(s->h);
        }

        pthread_mutex_lock(&host->lock);
    }
    pthread_mutex_unlock(&host->lock);
    return 0;
}

/* ---- the host --------------------------------------------------------- */

ElqHost *elq_host_create(int32_t sampleRate)
{
    unsigned int languages[MAX_LANGUAGES];
    int          n = 0;
    ElqHost     *host;
    ElqSynth    *first;

    if (eciGetAvailableLanguages(0, &n) || n < 1)
        return 0;
    if (n > MAX_LANGUAGES)
        n = MAX_LANGUAGES;
    eciGetAvailableLanguages(languages, &n);

    host = calloc(1, sizeof *host);
    if (!host)
        return 0;
    pthread_mutex_init(&host->lock, 0);
    pthread_cond_init(&host->wake, 0);
    host->sampleRate = sampleRate;

    /* Made here, before the worker exists, so that a sample rate the engine
       refuses fails now, while the extension can still pick another. */
    first = host_synth_for(host, languages[0]);
    if (!first) {
        elq_host_destroy(host);
        return 0;
    }
    atomic_store(&host->current, first);

    if (pthread_create(&host->worker, 0, worker_main, host) != 0) {
        elq_host_destroy(host);
        return 0;
    }
    host->workerStarted = 1;
    return host;
}

void elq_host_destroy(ElqHost *host)
{
    int i;

    if (!host)
        return;
    if (host->workerStarted) {
        pthread_mutex_lock(&host->lock);
        host->quit = 1;
        pthread_cond_signal(&host->wake);
        pthread_mutex_unlock(&host->lock);
        pthread_join(host->worker, 0);
    }
    atomic_store(&host->current, (ElqSynth *)0);
    for (i = 0; i < host->count; i++)
        synth_destroy(host->synths[i]);
    free(host->pendingDocument);
    pthread_cond_destroy(&host->wake);
    pthread_mutex_destroy(&host->lock);
    free(host);
}

/* Silences whatever is playing at once, without waiting for the worker. */
static void stop_current_now(ElqHost *host)
{
    ElqSynth *s = atomic_load(&host->current);

    if (s) {
        atomic_store(&s->aborting, 1);
        atomic_store(&s->flushTo, atomic_load(&s->head) + 1);
    }
}

int elq_host_speak(ElqHost *host, uint32_t language, const char *document,
                   const ElqVoiceSettings *voice)
{
    size_t   len;
    char    *copy;
    unsigned request;

    if (!host || !document || !voice)
        return 0;
    len = strlen(document);
    copy = malloc(len + 1);
    if (!copy)
        return 0;
    memcpy(copy, document, len + 1);

    stop_current_now(host);
    pthread_mutex_lock(&host->lock);
    request = atomic_load(&host->requested) + 1;
    atomic_store(&host->requested, request);
    free(host->pendingDocument);
    host->pendingDocument = copy;
    host->pendingLanguage = language;
    host->pendingVoice = *voice;
    host->pendingRequest = request;
    pthread_cond_signal(&host->wake);
    pthread_mutex_unlock(&host->lock);
    return 1;
}

void elq_host_cancel(ElqHost *host)
{
    if (!host)
        return;
    stop_current_now(host);
    pthread_mutex_lock(&host->lock);
    host->cancelPending = 1;
    free(host->pendingDocument);
    host->pendingDocument = 0;
    pthread_cond_signal(&host->wake);
    pthread_mutex_unlock(&host->lock);
}

size_t elq_host_render(ElqHost *host, float *out, size_t frames,
                       int32_t *finished)
{
    unsigned  requested = atomic_load(&host->requested);
    unsigned  served = atomic_load(&host->served);
    ElqSynth *s = atomic_load(&host->current);
    size_t    n;

    if (!s) {
        *finished = 0;
        return 0;
    }
    n = synth_render(s, out, frames, finished);
    if (requested != served)
        *finished = 0;
    return n;
}

/* Drives Shared/Engine/elq_bridge.c the way the iOS audio unit does -- a
 * control thread asking for speech and a render loop pulling samples -- on
 * any desktop the engine builds on. No iPhone needed.
 *
 *   cc -std=gnu11 -I<openevv>/include -IShared/Engine tests/bridge_test.c \
 *      Shared/Engine/elq_bridge.c <openevv>/build/libevv.a <eci_api.o> \
 *      -lpthread -lm -o bridge_test
 *   ./bridge_test [directory for wave files]
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "elq_bridge.h"

#ifdef _WIN32
#include <windows.h>
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
#else
#include <time.h>
static void sleep_ms(int ms)
{
    struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&t, 0);
}
#endif

enum { BLOCK = 512 };

static int         failures;
static const char *wavdir;

static ElqVoiceSettings voice(int preset, int ssml)
{
    ElqVoiceSettings v = { preset, -1, -1, -1, -1, -1, -1, -1, ssml };
    return v;
}

typedef struct {
    float *samples;
    long   count;
    long   room;
    int    finished;
} Take;

/* Pulls blocks until the bridge says finished, `stopAfter' samples have come
   (when not negative), or a wait of about `limitMs' with nothing arriving. */
static void pull(ElqHost *h, Take *t, int limitMs, long stopAfter)
{
    float block[BLOCK];
    int   idle = 0;

    for (;;) {
        int32_t fin = 0;
        size_t  n = elq_host_render(h, block, BLOCK, &fin);

        if (n) {
            if (t->count + (long)n > t->room) {
                t->room = (t->count + (long)n) * 2;
                t->samples = realloc(t->samples, (size_t)t->room * sizeof(float));
            }
            memcpy(t->samples + t->count, block, n * sizeof(float));
            t->count += (long)n;
            idle = 0;
        }
        if (fin) {
            t->finished = 1;
            return;
        }
        if (stopAfter >= 0 && t->count >= stopAfter)
            return;
        if (n < BLOCK) {
            sleep_ms(1);
            if (++idle > limitMs)
                return;
        }
    }
}

static void write_wav(const char *name, const Take *t, int rate)
{
    char     path[512];
    FILE    *f;
    uint32_t bytes = (uint32_t)t->count * 2;
    long     i;

    if (!wavdir)
        return;
    snprintf(path, sizeof path, "%s/%s", wavdir, name);
    f = fopen(path, "wb");
    if (!f)
        return;
    fwrite("RIFF", 1, 4, f);
    { uint32_t v = 36 + bytes; fwrite(&v, 4, 1, f); }
    fwrite("WAVEfmt ", 1, 8, f);
    { uint32_t v = 16; fwrite(&v, 4, 1, f); }
    { uint16_t v = 1; fwrite(&v, 2, 1, f); fwrite(&v, 2, 1, f); }
    { uint32_t v = (uint32_t)rate; fwrite(&v, 4, 1, f); v *= 2; fwrite(&v, 4, 1, f); }
    { uint16_t v = 2; fwrite(&v, 2, 1, f); v = 16; fwrite(&v, 2, 1, f); }
    fwrite("data", 1, 4, f);
    fwrite(&bytes, 4, 1, f);
    for (i = 0; i < t->count; i++) {
        float   x = t->samples[i] * 32768.0f;
        int16_t s = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
}

static void check(int ok, const char *what)
{
    printf("%s  %s\n", ok ? "pass" : "FAIL", what);
    if (!ok)
        failures++;
}

static long speak_all(ElqHost *h, const char *doc, ElqVoiceSettings v,
                      const char *wav, int rate, int *finished, int *queued)
{
    Take t = { 0 };
    int  q = elq_host_speak(h, 0x00010000, doc, &v);

    pull(h, &t, 5000, -1);
    if (queued)
        *queued = q;
    if (finished)
        *finished = t.finished;
    if (wav)
        write_wav(wav, &t, rate);
    free(t.samples);
    return t.count;
}

int main(int argc, char **argv)
{
    static const char *plain = "Hello from Eloquence.";
    static const char *ssml =
        "<speak version=\"1.0\" xml:lang=\"en-US\">Hello from "
        "<prosody rate=\"200%\">Eloquence</prosody>. That costs "
        "<say-as interpret-as=\"currency\">$5.25</say-as>.</speak>";
    ElqHost *h, *h22;
    long     base, n;
    int      fin, queued;

    /* Unbuffered, so a crash still leaves the checks that ran in the log. */
    setvbuf(stdout, 0, _IONBF, 0);
    wavdir = argc > 1 ? argv[1] : 0;

    h = elq_host_create(11025);
    check(h != 0, "a host is made at 11,025 Hz");
    if (!h)
        return 1;

    base = speak_all(h, plain, voice(1, 0), "plain.wav", 11025, &fin, 0);
    printf("      plain text: %ld samples\n", base);
    check(base > 5000 && fin, "plain text speaks and reports finished");

    n = speak_all(h, ssml, voice(1, 1), "ssml-reader.wav", 11025, &fin, 0);
    printf("      SSML through IBM's reader: %ld samples\n", n);
    check(n > 5000 && fin, "SSML through the reader speaks and finishes");

    n = speak_all(h, ssml, voice(1, 0), "ssml-stripped.wav", 11025, &fin, 0);
    printf("      SSML with tags stripped: %ld samples\n", n);
    check(n > 5000 && fin, "SSML with the reader off speaks and finishes");

    /* The reader is what carries VoiceOver's rate, so a fast prosody rate has
       to come out shorter with it than without it, whatever shape the
       document's <speak> tag and markup take. */
    {
        static const char *const fastDocs[] = {
            "<speak version=\"1.0\" xml:lang=\"en-US\"><prosody rate=\"200%\">Hello from Eloquence.</prosody></speak>",
            "<speak><prosody rate=\"200%\">Hello from Eloquence.</prosody></speak>",
            "<?xml version=\"1.0\"?><speak version=\"1.1\" xmlns=\"http://www.w3.org/2001/10/synthesis\" xml:lang=\"en-US\"><prosody rate=\"200%\" pitch=\"100%\" volume=\"100%\">Hello from Eloquence.</prosody></speak>",
            "<speak version=\"1.0\" xml:lang=\"en-US\"><prosody rate=\"200%\">That costs <say-as interpret-as=\"currency\">$5.25</say-as>.</prosody></speak>",
        };
        static const char *const names[] = {
            "the reader carries a prosody rate",
            "the reader carries a rate in a bare <speak>",
            "the reader carries a rate in a <speak> with a declaration and namespace",
            "the reader carries a rate past a say-as it cannot read",
        };
        size_t k;

        for (k = 0; k < sizeof fastDocs / sizeof fastDocs[0]; k++) {
            long withReader = speak_all(h, fastDocs[k], voice(1, 1), 0, 11025, &fin, 0);
            long without = speak_all(h, fastDocs[k], voice(1, 0), 0, 11025, 0, 0);

            printf("      reader %ld samples, stripped %ld\n", withReader, without);
            check(fin && withReader > 0 && withReader < without * 8 / 10, names[k]);
        }
    }

    /* SSML 1.1 reads a bare percentage as a multiple of the default, so 100%
       everywhere is the voice as it stands. IBM's reader takes a bare
       percentage as a change instead, which would double speed and pitch. */
    n = speak_all(h, "<speak><prosody rate=\"100%\" pitch=\"100%\" volume=\"100%\">Hello from Eloquence.</prosody></speak>",
                  voice(1, 1), "ssml-100-percent.wav", 11025, &fin, 0);
    printf("      rate, pitch and volume at 100%%: %ld samples, plain %ld\n", n, base);
    check(fin && n > base * 9 / 10 && n < base * 11 / 10,
          "100% prosody sounds like the voice unchanged");

    n = speak_all(h, "Don\xe2\x80\x99t \xe2\x80\x9cpanic\xe2\x80\x9d \xe2\x80\x94 caf\xc3\xa9 \xe2\x82\xac" "5.",
                  voice(2, 0), "utf8.wav", 11025, &fin, 0);
    check(n > 5000 && fin, "UTF-8 text with curly quotes, a dash and accents speaks");

    n = speak_all(h, "<speak version=\"1.0\"></speak>", voice(1, 1), 0, 11025, &fin, &queued);
    check(queued && fin && n == 0, "an empty document finishes with no samples");

    {
        ElqVoiceSettings fast = voice(1, 0);
        fast.speed = 250;
        n = speak_all(h, plain, fast, "fast.wav", 11025, &fin, 0);
        printf("      speed 250: %ld samples\n", n);
        check(n > 0 && n < base * 8 / 10 && fin, "speed 250 is shorter than the preset");
    }

    /* Cancel part way through a long utterance, then speak something short:
       nothing of the first may come out ahead of the second. */
    {
        static const char *longText =
            "This is a long sentence that keeps going so that there is "
            "plenty of it left to throw away when the cancel arrives, and "
            "then it goes on a good while longer still.";
        ElqVoiceSettings v = voice(1, 0);
        Take t = { 0 };
        long second, fresh;

        fresh = speak_all(h, "Second.", v, 0, 11025, 0, 0);
        elq_host_speak(h, 0x00010000, longText, &v);
        pull(h, &t, 5000, 4000);
        elq_host_cancel(h);
        free(t.samples);

        second = speak_all(h, "Second.", v, "after-cancel.wav", 11025, &fin, 0);
        printf("      \"Second.\" fresh %ld samples, after a cancel %ld\n",
               fresh, second);
        check(fin && second > 0 && second < fresh * 3 / 2,
              "after a cancel only the new utterance is heard");
    }

    /* Not pulling for a while lets the ring fill; the engine must wait rather
       than drop, so the whole long document still arrives. */
    {
        char             doc[8192] = "";
        ElqVoiceSettings v = voice(1, 0);
        Take             t = { 0 };
        int              i;

        for (i = 0; i < 40; i++)
            strcat(doc, "The quick brown fox jumps over the lazy dog. ");
        elq_host_speak(h, 0x00010000, doc, &v);
        sleep_ms(3000);
        pull(h, &t, 10000, -1);
        printf("      a long document read slowly: %ld samples (%.1f s)\n",
               t.count, t.count / 11025.0);
        check(t.finished && t.count > (1L << 18),
              "a document longer than the ring is paced, not cut");
        free(t.samples);
    }

    h22 = elq_host_create(22050);
    check(h22 != 0, "a host is made at 22,050 Hz");
    if (h22) {
        n = speak_all(h22, plain, voice(1, 0), "plain-22050.wav", 22050, &fin, 0);
        printf("      plain text at 22,050 Hz: %ld samples (%.2fx)\n",
               n, (double)n / (double)base);
        check(fin && n > base * 19 / 10 && n < base * 21 / 10,
              "22,050 Hz gives twice the samples for the same speech");
        elq_host_destroy(h22);
    }

    elq_host_destroy(h);
    printf("%s\n", failures ? "SOME CHECKS FAILED" : "all checks passed");
    return failures ? 1 : 0;
}

/* The OpenEVV engine shaped for an iOS speech synthesis extension.
 *
 * Three threads touch this and each has its own calls:
 *   - the extension's control thread: elq_host_speak, elq_host_cancel, which
 *     post a request and return at once
 *   - a worker thread of the bridge's own, which makes every engine call and
 *     fills a ring buffer
 *   - the real-time audio render thread: elq_host_render, which never locks
 *     or allocates
 */

#ifndef ELQ_BRIDGE_H
#define ELQ_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ElqHost ElqHost;

/* One voice as the extension wants it spoken. A value of -1 keeps whatever the
   preset has; everything else is the engine's own 0-100 scale, except speed,
   which runs 0-250. */
typedef struct {
    int32_t preset;            /* 1..8: Reed, Shelley, Sandy, Rocko, Glen, Flo,
                                  Grandma, Grandpa */
    int32_t speed;
    int32_t pitch;
    int32_t pitchFluctuation;
    int32_t headSize;
    int32_t roughness;
    int32_t breathiness;
    int32_t volume;
    int32_t useSSMLReader;     /* nonzero: IBM's SSML reader; zero: tags are
                                  stripped here and the text spoken plain */
} ElqVoiceSettings;

/* sampleRate is in hertz. Returns NULL if the engine has no languages. */
ElqHost *elq_host_create(int32_t sampleRate);
void     elq_host_destroy(ElqHost *host);

/* Silences anything playing and asks for the document (SSML or plain UTF-8)
   to be spoken. Returns 1 once the request is posted, 0 if it could not be.
   A document with nothing to say is reported finished by the render soon
   after. */
int  elq_host_speak(ElqHost *host, uint32_t language, const char *document,
                    const ElqVoiceSettings *voice);
void elq_host_cancel(ElqHost *host);

/* Real-time safe. Writes up to `frames` mono float samples and answers how
   many; `finished` is set once the current utterance has been fully rendered. */
size_t elq_host_render(ElqHost *host, float *out, size_t frames,
                       int32_t *finished);

#ifdef __cplusplus
}
#endif

#endif

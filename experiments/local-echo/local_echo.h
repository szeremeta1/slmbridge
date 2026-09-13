/* Experimental one-frame local feedback, independent of DSP sample skipping.
 * No claim about physical round-trip delay. Reference commits only after the
 * complete selected 8-kHz AudioSocket frame is accepted by the transport. */
#ifndef SLMBRIDGE_LOCAL_ECHO_H
#define SLMBRIDGE_LOCAL_ECHO_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

enum le_mode { LE_OFF = 0, LE_OBSERVE = 1, LE_MIX = 2 };
struct local_echo {
    enum le_mode mode;
    uint64_t received, committed, mixed, warmup, clipped;
    int pending, invalid;
    const char *reason;
    int16_t reference[160], candidate[160];
};

static int le_fail(struct local_echo *s, const char *reason)
{
    if (!s->invalid) s->reason = reason;
    s->invalid = 1;
    return -1;
}

static int le_config(const char *value)
{
    if (!value || !strcmp(value, "off")) return LE_OFF;
    if (!strcmp(value, "observe")) return LE_OBSERVE;
    if (!strcmp(value, "mix")) return LE_MIX;
    return -1;
}

/* Input bytes are native-endian signed-linear PCM, as in the bridge itself.
 * memcpy avoids requiring alignment of the AudioSocket payload buffer. */
static int le_receive(struct local_echo *s, void *bytes, size_t size)
{
    if (s->mode == LE_OFF) return 0;
    if (s->invalid) return -1;
    if (size != 320) return le_fail(s, "input_frame_size");
    if (s->pending || s->committed != s->received)
        return le_fail(s, "input_before_reference_commit");
    if (s->received == UINT64_MAX) return le_fail(s, "counter_overflow");
    if (!s->received) {
        s->warmup++;
    } else if (s->mode == LE_MIX) {
        for (size_t i = 0; i < 160; i++) {
            int16_t original;
            memcpy(&original, (uint8_t *)bytes + i * 2, 2);
            /* Fixed amplitude gain 1/100 = -40 dB. Round the added term to
             * nearest integer, ties away from zero; then saturate once. */
            int32_t ref = s->reference[i];
            int32_t echo = ref >= 0 ? (ref + 50) / 100 : -((-ref + 50) / 100);
            int32_t sum = (int32_t)original + echo;
            if (sum > INT16_MAX) { sum = INT16_MAX; s->clipped++; }
            if (sum < INT16_MIN) { sum = INT16_MIN; s->clipped++; }
            int16_t result = (int16_t)sum;
            memcpy((uint8_t *)bytes + i * 2, &result, 2);
        }
        s->mixed++;
    }
    s->received++;
    return 0;
}

/* Stage the exact post-resampler/ring/fill output, never a future DSP block.
 * A fallback emission without a preceding input invalidates this epoch. */
static int le_stage(struct local_echo *s, const int16_t *samples)
{
    if (s->mode == LE_OFF) return 0;
    if (s->invalid) return -1;
    if (s->pending || s->committed == UINT64_MAX ||
        s->received != s->committed + 1)
        return le_fail(s, "unpaired_output");
    memcpy(s->candidate, samples, sizeof s->candidate);
    s->pending = 1;
    return 0;
}

static int le_commit(struct local_echo *s)
{
    if (s->mode == LE_OFF) return 0;
    if (s->invalid) return -1;
    if (!s->pending || s->committed == UINT64_MAX ||
        s->received != s->committed + 1)
        return le_fail(s, "unpaired_commit");
    memcpy(s->reference, s->candidate, sizeof s->reference);
    s->pending = 0;
    s->committed++;
    return 0;
}

static void le_report(const struct local_echo *s)
{
    if (s->mode == LE_OFF) return;
    fprintf(stderr, "bridge_local_echo: mode=%s invalid=%d reason=%s "
            "received=%llu committed=%llu warmup=%llu mixed=%llu clipped=%llu pending=%d\n",
            s->mode == LE_MIX ? "mix" : "observe", s->invalid,
            s->reason ? s->reason : "none", (unsigned long long)s->received,
            (unsigned long long)s->committed, (unsigned long long)s->warmup,
            (unsigned long long)s->mixed, (unsigned long long)s->clipped, s->pending);
}
#endif

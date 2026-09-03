/*
 * slmbridge — slmodemd behind AudioSocket.
 *
 * Phase 4's performance path. spandsp's V.32bis is 251 lines of empty API
 * skeleton and its V.34 never reaches data mode, so 2400 bps is the ceiling of
 * anything fully open. The only working alternative is slmodemd's proprietary
 * DSP blob, which does have V.32, V.32bis, V.34, V.90 and V.92 — verified by
 * `nm` on dsplibs.o, not by marketing.
 *
 * WHAT THIS REPLACES, AND WHY THAT SOLVES D-MODEM'S BLOCKER
 *
 * Upstream D-Modem pairs slmodemd with `d-modem`, a PJSIP process that owns the
 * SIP leg. That process can only ever place calls — d-modem.c calls
 * pjsua_call_make_call() and has no on_incoming_call callback at all — which is
 * fatal for an ISP, because an ISP answers.
 *
 * Rather than patch PJSIP answer support into d-modem, this replaces d-modem
 * outright. Asterisk already answers the call and hands us the audio over
 * AudioSocket, so the "cannot answer" blocker simply stops existing. We also
 * inherit Asterisk's codec discipline, its identify/ACL, and one SIP stack on
 * the box instead of two competing for port 5060.
 *
 * THE 9600 Hz RESAMPLER, WHICH THIS ALSO DELETES
 *
 * modem.h:85 reads `#define MODEM_RATE 9600 /_ 8000 _/` and d-modem.c:216
 * initialises its pjmedia port at 9600 Hz. PJMEDIA therefore resamples
 * 9600 <-> 8000 on every frame in both directions — pure signal degradation
 * sitting directly in the modem path, and very plausibly why connect rates on
 * this path historically stuck around 14.4k.
 *
 * Asterisk hands us slin at exactly 8000 Hz. Building slmodemd with
 * MODEM_RATE 8000 makes the DSP native to that rate and removes every
 * resampler from the path. The blob ships dual-rate coefficient tables — 12
 * symbols suffixed _8000 next to 15 suffixed _9600, plus the literal string
 * "Sample rate 8000" — so this is a supported configuration, not a hack.
 *
 * PROTOCOL BETWEEN slmodemd AND US
 *
 * slmodemd creates a socketpair, forks, and execs its helper as
 *     helper <dial_string> <fd>
 * then does raw read()/write() of signed 16-bit little-endian PCM over it,
 * unframed (see mdm_device_read/write in modem_main.c). There is no header and
 * no rate negotiation — the rate is whatever slmodemd was compiled for.
 *
 * We are that helper. We inherit the AudioSocket TCP connection from the broker
 * through an environment variable, and relay:
 *
 *     slmodemd ──PCM──▶ [ring] ──20 ms frames──▶ AudioSocket ──▶ Asterisk
 *     slmodemd ◀──PCM──                          AudioSocket ◀── Asterisk
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define AS_HANGUP 0x00
#define AS_UUID   0x01
#define AS_DTMF   0x03
#define AS_AUDIO  0x10
#define AS_ERROR  0xFF

#define AS_RATE       8000                   /* Asterisk hands us slin at 8 kHz */
#define FRAME_SAMPLES 160                    /* 20 ms at 8 kHz */
#define FRAME_BYTES   (FRAME_SAMPLES * 2)
/* 8 frames = 160 ms, not 64 frames = 1.28 s.
 *
 * Measured: with a 64-frame ring the link came up and pinged cleanly but showed
 * 1,188-1,256 ms RTT on a 12000 bps carrier where the arithmetic says ~100 ms.
 * The ring had filled during the handshake burst and then simply stayed full,
 * adding its entire depth as constant one-way delay for the rest of the call.
 * Latency is harmless to a modem; it is not harmless to TCP.
 *
 * UNRESOLVED, and the honest state of this file. Shrinking the ring to 8 frames
 * made things worse, not better: RTT went from ~1.2 s to 3.1-6.1 s, with
 * overrun=45 on one helper and starved=50 on the other in the same call. That
 * asymmetry is the actual diagnosis -- the two slmodemd instances free-run on
 * their own DSP clocks and drift against this fixed 20 ms transmit deadline, so
 * one accumulates backlog while its partner runs dry, and dropping frames to
 * bound the backlog corrupts the data stream into V.42 retransmission.
 *
 * The correct fix is an elastic buffer that targets a small fixed occupancy and
 * absorbs drift by occasionally duplicating or dropping a single SAMPLE rather
 * than a whole 20 ms frame -- i.e. rate adaptation, not truncation. That is not
 * written yet. 16 frames is a middle setting chosen to keep the link usable for
 * ICMP while that work is outstanding; it is not a solution. */
#define RING_FRAMES_DEFAULT 16
#define RING_FRAMES_RESAMPLED 64   /* the 9600 path: see ring_frames_for() */
#define RING_FRAMES_MAX     64
#define RING_SAMPLES  (FRAME_SAMPLES * RING_FRAMES_MAX)

/* THE RING DEPTH IS NOW A DIAL, and the reason is that its old value was
 * compensation for a bug that has since been fixed.
 *
 * 16 frames -- 320 ms -- was "a middle setting chosen to keep the link usable
 * while that work is outstanding; it is not a solution", where the outstanding
 * work was the percent-level rate mismatch above. Shrinking it made things
 * WORSE at the time (RTT 1.2 s -> 3.1-6.1 s) because a smaller buffer runs dry
 * faster against a 20% mismatch.
 *
 * With the DSP on its native 9600 and the resampler feeding it 192 samples per
 * 20 ms, that mismatch is gone -- measured starved=0, slip-del=0, slip-ins=0 on
 * a live call. So the ring no longer has to absorb drift, only jitter, and 320
 * ms of it is pure one-way latency. V.34's Phase 2 INFO exchange is timed, and
 * a third of a second each way is the difference between completing and sitting
 * in "Repeated info0" forever.
 *
 * SLMBRIDGE_RING_FRAMES, default unchanged at 16. */
/* The default is larger on the RESAMPLED path, and only there.
 *
 * When the ring fills, the helper reads slmodemd's output into a buffer called
 * `waste` and discards it -- a hole punched in the signal we transmit, up to
 * 4 KB at a time. Measured on the 9600 experimental line: overrun=4..19 on most
 * calls at 16 frames, and exactly 0 at 64.
 *
 * ⚠️ Production overruns too (up to 13 seen at 8000) and is unaffected in
 * practice -- V.32bis at 2400 baud rides through a lost 20 ms and LAPM
 * retransmits, and those lines log ZERO hdlc frame errors across a day. So this
 * is not a proven cause of anything; it is a latent defect that the slow path
 * tolerates. V.34 at 3429 baud has far less margin, which is why the larger
 * ring is given to the resampled path and production is left byte-identical in
 * behaviour rather than "improved" on a hunch.
 *
 * Capacity is not latency: under SLMBRIDGE_TX_CLOCK=rx the ring drains one frame
 * per frame Asterisk delivers, so steady-state delay is SLMBRIDGE_RX_PREFILL, not
 * this number. Capacity only has to absorb a burst.
 *
 * SLMBRIDGE_RING_FRAMES overrides both. */

/* ------------------------------------------------------------------------
 * Configuration is by environment variable, and every variable has one
 * canonical name with the SLMBRIDGE_ prefix. bridge_env("X") reads
 * SLMBRIDGE_X and, when that is unset, LITENET_X: the deployment this was
 * written for configured the bridge under the older prefix for a year and
 * still does, and a rename that broke a running pool would be a poor way to
 * publish a program. The fallback is deliberate and permanent; nothing in
 * this file reads either spelling directly.
 * ---------------------------------------------------------------------- */
static const char *bridge_env(const char *name)
{
    static char key[64];
    snprintf(key, sizeof key, "SLMBRIDGE_%s", name);
    const char *v = getenv(key);
    if (v) return v;
    snprintf(key, sizeof key, "LITENET_%s", name);
    return getenv(key);
}

static int ring_frames_for(int dsp_rate)
{
    const char *v = bridge_env("RING_FRAMES");
    int dflt = (dsp_rate != 8000) ? RING_FRAMES_RESAMPLED : RING_FRAMES_DEFAULT;
    int n = (v && *v) ? atoi(v) : dflt;
    if (n < 2) n = 2;
    if (n > RING_FRAMES_MAX) n = RING_FRAMES_MAX;
    return n;
}

static void ts_add_ns(struct timespec *t, long ns)
{
    t->tv_nsec += ns;
    while (t->tv_nsec >= 1000000000L) { t->tv_nsec -= 1000000000L; t->tv_sec++; }
}

static int read_exact(int fd, void *buf, size_t n)
{
    uint8_t *p = buf; size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

/* ------------------------------------------------------------- resampler --
 * 8000 Hz on the AudioSocket side, 9600 Hz on the slmodemd side.
 *
 * WHY THIS EXISTS AGAIN, HAVING BEEN DELETED ON PURPOSE
 *
 * An earlier decision built slmodemd at MODEM_RATE 8000 specifically so no resampler would
 * exist anywhere, and that was right about the resampler it was deleting:
 * PJMEDIA converting 9600<->8000 at an arbitrary ratio inside a conference
 * bridge that was itself running at 16 kHz, i.e. two conversions and a wrong
 * intermediate rate. It was wrong about the cost of the 8000 build, which
 * measurement shows: at 8000 the V.8 datapump will not create, and V.34, V.90
 * and V.92 all route through V.8. The 8 kHz build is what caps a pool at
 * V.32bis (traced on the reference deployment, 2026-08).
 *
 * So the DSP goes back to its native rate and the conversion is done here,
 * once, properly. 9600/8000 is exactly 6/5 -- a rational resampler with five
 * or six polyphase branches, not an arbitrary-ratio interpolator -- and the
 * band that has to survive is 300-3400 Hz, nowhere near the 4 kHz corner.
 *
 * IT IS ALSO THE PACING FIX, WHICH IS THE PART WORTH NOTICING.
 *
 * slmodemd's DSP is paced by how fast this helper writes to it. A 9600 Hz
 * build fed 160 samples per 20 ms is being fed at 8000 Hz -- it is starved by
 * exactly 20%, forever. That is the "gross, percent-level" mismatch recorded
 * in the elastic-buffer note above, the one that made a one-sample-per-frame
 * correction saturate and still overrun. 9600/8000 = 1.2. Feeding 192 samples
 * per 20 ms instead of 160 is the same 20 ms of audio at the rate the DSP
 * actually keeps time in, and the ring should now sit still.
 *
 * Set SLMBRIDGE_DSP_RATE=8000 to bypass every line of this and get the old
 * straight-through path back, which is the rollback.
 */
/* THE TEST STOPPED AT 3400 Hz AND THE FILTER STOPPED WORKING AT 3500.
 *
 * The 32-tap Blackman design this replaces was measured flat "to within 0.1 dB"
 * across 300-3400 Hz, and it is. That band was the right target for V.32bis,
 * whose symbols stop at 3000 Hz. It is the wrong target for V.34: the 3429-baud
 * symbol rate that carries 31200 and 33600 sits on a 1959 Hz carrier with 12.5%
 * excess bandwidth, so its signal runs to 1959 + 3429*1.125/2 = 3888 Hz.
 * Measured round trip on the old design:
 *
 *     3400 Hz  -0.11 dB      3674 Hz  -1.85 dB      3888 Hz  -7.05 dB
 *
 * That is the top of the fastest V.34 constellation being quietly buried by the
 * component whose entire job is to be transparent -- the same defect the 12-tap
 * version had, moved up a kilohertz and left there because the test only ever
 * asserted to 3400. slmbridge_rstest.c now asserts to 3900 for that reason.
 *
 * WHY THIS COSTS TAPS AND CANNOT BE HAD BY MOVING THE CUTOFF
 *
 * The obvious cheap fix is to slide fc up from 4000 Hz and buy the passband
 * back for free, since the stopband was 75-108 dB down and nothing in a 35 dB
 * SNR telephone channel needs that. It was tried, it measures beautifully in a
 * magnitude plot, and it is WRONG in the up direction -- caught only because
 * the rewritten test injects a tone and looks for its image:
 *
 *     up-resampling 8000 -> 9600 with fc=4250 gave +1.41 dB at 3800 Hz
 *
 * Gain, from a lowpass. Reconstructing a 9600 Hz signal from 8000 Hz samples,
 * every component above 4000 Hz is by definition an image and not signal: input
 * at 3800 Hz mirrors to 4200 Hz, and an fc of 4250 passed it at full amplitude
 * straight into the DSP. So for the UP direction the corner is pinned at 4000 Hz
 * by physics, and the only lever on the transition is length. An earlier note
 * queued "the computed 64-tap resampler" and the instinct was right; the number
 * was low, because 64 taps at fc=4000 is still -0.92 dB (up) and -1.39 dB (down)
 * at 3888 Hz.
 *
 * The DOWN direction could legitimately relax -- decimating 9600 -> 8000 folds
 * about 4000 Hz, but the only thing between 4000 and 4800 Hz is the DSP's own
 * out-of-band emission, and real image energy does not begin until 9600-3900 =
 * 5700 Hz. Measured with SLMBRIDGE_RS_DUMP on a replayed call, the blob emits
 * -86 dBc between 4000 and 4400 Hz, so relaxing it would in fact have been
 * safe. It is kept at 4000 Hz anyway, for two reasons: the taps it would save
 * are worth less than not owing the next reader an assumption, and that -86 dBc
 * was captured during the V.8/INFO0 phases -- the replayed call never reached
 * the 3429-baud data phase, so it does not characterise emission when the
 * modulator is actually running at the top symbol rate.
 *
 * WHAT IT COSTS, AND WHY THAT IS AFFORDABLE HERE
 *
 * 96 taps/phase up and 112 down, Kaiser beta 5.0, fc 4000 Hz both ways:
 *
 *              ripple 300-3888   @3674     @3888    alias rejection
 *   UP   old       3.57 dB      -0.72 dB  -3.57 dB    -75.3 dB (>=4800)
 *   UP   new       0.06 dB      -0.00 dB  -0.16 dB    -69.9 dB (>=4800)
 *   DOWN old       3.92 dB      -1.14 dB  -3.92 dB    -83.1 dB (>=5700)
 *   DOWN new       0.08 dB      +0.00 dB  -0.20 dB    -74.5 dB (>=5700)
 *
 * Round trip at 3888 Hz goes from -7.05 dB to -0.36 dB. Stopband gives up a few
 * dB it had no use for.
 *
 * It is NOT free, and an earlier draft of this comment claimed it was close to
 * a wash because the old inner loop's per-input-sample memmove is gone (the
 * history is now double-written, so the filter window is always contiguous and
 * nothing is moved). Benchmarked on the box that runs it -- Xeon E5-2697A v4,
 * both directions, one line's worth of audio -- that was wrong:
 *
 *     legacy 32/32 tap   0.065% of one core per line
 *     this   96/112 tap  0.187% of one core per line
 *
 * 2.9x, not a wash. It is affordable rather than cheap: 6.0% of one core if all
 * 32 lines ever run it, against the 0.8 core-% the DSP costs per line and the
 * 7.3 the relay costs. Today exactly one line runs it. The memmove removal is
 * real and is why 3.5x the taps costs 2.9x the time instead of more.
 *
 * This does NOT claim a rate improvement. It removes a known distortion from
 * the band V.34's rate decision is made on; whether the far modem then picks a
 * higher symbol rate is a question for a real call, not for this file.
 *
 * SLMBRIDGE_RS_PROFILE=legacy restores the old 32-tap fc=4000 Blackman pair
 * exactly, which is the rollback. The new pair is the default because the only
 * caller of any of this is the 9600 experimental path -- the 32 production lines
 * run FAST_DSP_RATE=8000, where `resampling` is false and not one line of this
 * resampler executes.
 *
 * SLMBRIDGE_RS_DUMP=<prefix> writes the raw pre-resample streams to
 * <prefix>-as8000.raw and <prefix>-dsp9600.raw (s16le, no header). Off unless
 * set. Diagnostic only: it does a blocking write per frame and has no place on
 * a live line. */
#define RS_TAPS_MAX       128
#define RS_MAX_L          6

typedef struct {
    int   L, M;                                  /* interpolate L, decimate M */
    int   taps;                                  /* taps per polyphase branch */
    int   phase;                                 /* virtual position, [0,L) */
    float h[RS_TAPS_MAX * RS_MAX_L];             /* prototype, h[k*L + p] */
    float hist[2 * RS_TAPS_MAX];                 /* double-written ring */
    int   hpos;                                  /* next slot to overwrite */
} resamp_t;

/* Modified Bessel I0, series form. Converges in well under 60 terms for the
   beta range a window uses; the loop breaks on relative epsilon anyway. */
static double rs_i0(double x)
{
    double s = 1.0, t = 1.0;
    for (int k = 1; k < 60; k++) {
        double q = x / (2.0 * k);
        t *= q * q;
        s += t;
        if (t < 1e-16 * s) break;
    }
    return s;
}

/* Windowed-sinc prototype at the intermediate rate (in_rate * L = 48000 for
   both directions here). fc_hz is the -6 dB corner in real Hz; beta < 0 means
   Blackman, which is what `legacy` uses. Scaled so the whole filter sums to L,
   which makes each polyphase branch sum to ~1 and keeps DC gain at unity. */
static void rs_init(resamp_t *r, int L, int M, int taps, double fc_hz,
                    double beta)
{
    if (taps < 4) taps = 4;
    if (taps > RS_TAPS_MAX) taps = RS_TAPS_MAX;
    r->L = L; r->M = M; r->taps = taps; r->phase = 0; r->hpos = 0;
    memset(r->hist, 0, sizeof r->hist);
    memset(r->h, 0, sizeof r->h);

    const double inter = 8000.0 * 6.0;           /* == 9600 * 5 */
    const int    n     = taps * L;
    const double fc    = fc_hz / inter;
    const double mid   = (n - 1) / 2.0;
    const double i0b   = (beta >= 0.0) ? rs_i0(beta) : 0.0;
    double sum = 0.0;
    for (int i = 0; i < n; i++) {
        double x = i - mid;
        double sinc = (x == 0.0) ? 2.0 * fc
                                 : sin(2.0 * M_PI * fc * x) / (M_PI * x);
        double w;
        if (beta >= 0.0) {
            double t = (2.0 * i) / (double)(n - 1) - 1.0;   /* -1 .. +1 */
            double a = 1.0 - t * t;
            if (a < 0.0) a = 0.0;
            w = rs_i0(beta * sqrt(a)) / i0b;
        } else {
            w = 0.42 - 0.5 * cos(2.0 * M_PI * i / (n - 1))
                     + 0.08 * cos(4.0 * M_PI * i / (n - 1));
        }
        r->h[i] = (float)(sinc * w);
        sum += r->h[i];
    }
    for (int i = 0; i < n; i++) r->h[i] = (float)(r->h[i] * (double)L / sum);
}

/* Pick the pair of designs. See the block comment: UP and DOWN have different
   stopband edges (4800 vs 5700 Hz), so they are not the same filter. */
static void rs_init_pair(resamp_t *up, resamp_t *down, int dsp_rate)
{
    const int L_up = dsp_rate / 1600, M_up = 8000 / 1600;   /* 6, 5 */
    const char *p = bridge_env("RS_PROFILE");
    if (p && !strcmp(p, "legacy")) {
        rs_init(up,   L_up, M_up, 32, 4000.0, -1.0);
        rs_init(down, M_up, L_up, 32, 4000.0, -1.0);
        return;
    }
    /* SLMBRIDGE_RS_PROFILE=narrow -- deliberately band-limit the RECEIVE path.
     *
     * The flat 4000 Hz design below is correct as signal processing and may be
     * wrong as engineering. The caller's ATA transmits 10.7 dB down at 3750 Hz
     * (line-probe measurement on the reference deployment), so the top of the band
     * carries very little of his signal and proportionally more noise.
     * Reconstructing it flat hands that noise to the equalizer at full weight
     * and lets the rate selector believe in bandwidth that is not really there
     * -- which is consistent with the new filter buying 33,600 rate decisions
     * without buying reliability, and with the old 32-tap design (accidentally
     * ~7 dB down at 3888 Hz) doing better on connect rate in a paired A/B.
     *
     * So: same modern filter quality, corner moved down to SLMBRIDGE_RS_FC
     * (default 3800 Hz). UP only -- the DOWN
     * direction is our transmit, which is clean and should stay flat.
     * This is a hypothesis under test, not a shipped default. */
    if (p && !strcmp(p, "narrow")) {
        const char *f = bridge_env("RS_FC");
        /* 3800, not 3400. At 96 taps a 3400 corner is a brick wall -- -59 dB
         * by 3600 -- which would delete the top of the 3429-baud constellation
         * rather than de-emphasise it. 3800 measures -6.0 dB at 3800 Hz, which
         * is within a dB of what the legacy 32-tap design did at 3888 (-7 dB)
         * and is the response the paired A/B suggests is actually helping.
         * The point is to reproduce legacy's GENTLE ROLLOFF while keeping the
         * modern stopband, not to invent a new brick wall. */
        double fc = f ? atof(f) : 3800.0;
        if (fc < 3000.0 || fc > 4000.0) fc = 3800.0;   /* refuse nonsense */
        rs_init(up,   L_up, M_up,  96, fc,     5.0);
        rs_init(down, M_up, L_up, 112, 4000.0, 5.0);
        return;
    }
    rs_init(up,   L_up, M_up,  96, 4000.0, 5.0);
    rs_init(down, M_up, L_up, 112, 4000.0, 5.0);
}

/* Push nin samples, emit as many outputs as become available. Returns the
   count written. maxout must allow ceil(nin * L / M) + 1.
 *
 * The history is written twice, at hpos and hpos+taps, so the newest `taps`
 * samples are always contiguous at hist[hpos+1 .. hpos+taps] and the inner
 * loop needs no modulo and no per-sample memmove. The old version moved
 * (taps-1) floats for every input sample; at 48 taps and 32 lines that alone
 * would have been the dominant cost of the filter. */
static int rs_process(resamp_t *r, const int16_t *in, int nin,
                      int16_t *out, int maxout)
{
    const int taps = r->taps;
    int nout = 0;
    for (int i = 0; i < nin; i++) {
        r->hist[r->hpos] = r->hist[r->hpos + taps] = (float)in[i];
        if (++r->hpos >= taps) r->hpos = 0;
        const float *w = r->hist + r->hpos;      /* w[0]=oldest .. w[taps-1]=newest */

        while (r->phase < r->L) {
            if (nout >= maxout) { r->phase += r->M; continue; }  /* never overrun caller */
            const float *hp = r->h + r->phase;
            float acc = 0.0f;
            for (int k = 0; k < taps; k++)
                acc += hp[k * r->L] * w[taps - 1 - k];
            int v = (int)(acc + (acc >= 0.0f ? 0.5f : -0.5f));
            if (v >  32767) v =  32767;
            if (v < -32768) v = -32768;
            out[nout++] = (int16_t)v;
            r->phase += r->M;
        }
        r->phase -= r->L;
    }
    return nout;
}

/* Env-gated raw taps on both pre-resample streams, so the filter's inputs can
   be looked at instead of reasoned about. SLMBRIDGE_RS_DUMP=<prefix> writes
   <prefix>-as8000.raw (what Asterisk hands us) and <prefix>-dsp9600.raw (what
   the DSP hands us). s16le, no header. Opens on first use, never closes -- a
   diagnostic run is one call long. Blocking writes: bench only. */
static void rs_dump(int which, const int16_t *p, int n)
{
    static FILE *f[2];
    static int tried;
    if (!tried) {
        tried = 1;
        const char *pre = bridge_env("RS_DUMP");
        if (pre && *pre) {
            char path[512];
            snprintf(path, sizeof path, "%s-as8000.raw", pre);
            f[0] = fopen(path, "wb");
            snprintf(path, sizeof path, "%s-dsp9600.raw", pre);
            f[1] = fopen(path, "wb");
        }
    }
    if (f[which] && n > 0) fwrite(p, 2, (size_t)n, f[which]);
}

/* ---------------------------------------------------------------- helper --
 * Invoked by slmodemd as: slmbridge <dial_string> <socketpair_fd>
 *
 * ELASTIC BUFFER — why this exists
 *
 * slmodemd free-runs on its own DSP clock. AudioSocket runs on Asterisk's
 * 20 ms clock. The two are never exactly equal, so a fixed-size queue between
 * them either fills or empties, permanently, no matter how big it is. Measured:
 * one helper reporting overrun=45 while its partner reported starved=50 in the
 * same call, and shrinking the ring from 64 frames to 8 made round-trip time
 * WORSE (1.2 s -> 3.1-6.1 s) rather than better.
 *
 * That is the signature of a rate problem being treated as a size problem.
 * Dropping a whole 20 ms frame to bound the backlog destroys 160 consecutive
 * samples; V.42 retransmits, and TCP's window never opens. ICMP survived it
 * because one lost echo costs one packet.
 *
 * The fix is to correct the RATE, by one sample at a time. Each 20 ms tick we
 * consume 159, 160 or 161 samples from the ring to emit exactly 160, steering
 * occupancy toward a small target. A single inserted or deleted sample at
 * 8 kHz is 125 microseconds and is invisible to a demodulator's equaliser;
 * doing it at a zero crossing means it does not even introduce a step.
 *
 * This is the same technique a hardware ATA uses to reconcile its codec clock
 * with the network -- and notably it is the mechanism master prompt SS3.4 cites
 * as what kills V.90 ("ATAs reconcile drift by sample slip"). Fatal to 56k,
 * which needs exact codeword alignment; harmless to V.32bis, which does not.
 *
 * IT DID NOT WORK, AND THE REASON MATTERS.
 *
 * Enabled, the answer side logged slip-del=274 against tx=274 -- deleting a
 * sample on EVERY frame, i.e. the correction saturated -- and still recorded
 * overrun=18, meaning the ring hit its ceiling anyway. One sample per 20 ms is
 * a 0.06% correction. If it saturates and the buffer still overflows, the
 * mismatch is not drift; it is gross, percent-level, and sample slip is the
 * wrong instrument entirely.
 *
 * That reframes the problem: something in this path is not running at 8 kHz,
 * and finding out what is the next step rather than correcting harder. The
 * obvious suspect is slmodemd's own timing -- it is driven by how fast its
 * device delivers, and a socketpair delivers as fast as it is written, so its
 * DSP cadence may not be pinned to MODEM_RATE at all.
 *
 * Left in place, defaulting OFF, because the mechanism is right even though
 * this application of it was premature.
 */

#define TARGET_SAMPLES  (FRAME_SAMPLES * 2)   /* 40 ms of slack to absorb jitter */
#define HYSTERESIS      40                    /* 5 ms; do not chase small errors */

/* Pick the least-energy point in a window -- the closest thing to a zero
   crossing -- so an inserted or deleted sample introduces no step. */
static int quietest_index(const int16_t *s, int lo, int hi)
{
    int best = lo, bv = 32768;
    for (int i = lo; i < hi; i++) {
        int v = s[i] < 0 ? -s[i] : s[i];
        if (v < bv) { bv = v; best = i; }
    }
    return best;
}

/* Write one 20 ms AudioSocket AUDIO frame. Returns 0 on success, -1 on a short
   or failed write. */
static int emit_frame(int as_fd, const int16_t *out)
{
    uint8_t msg[3 + FRAME_BYTES];
    msg[0] = AS_AUDIO;
    msg[1] = (FRAME_BYTES >> 8) & 0xFF;
    msg[2] = FRAME_BYTES & 0xFF;
    memcpy(msg + 3, out, FRAME_BYTES);
    return write(as_fd, msg, sizeof msg) == (ssize_t)sizeof msg ? 0 : -1;
}

/* Pull exactly one 8 kHz frame from the ring. On underrun, hold the last level
   rather than slamming to zero -- 2 s of silence is AudioSocket's liveness
   timeout -- and count it. The ring is always in the 8 kHz domain (the pcm_fd
   reader has already down-resampled), so this is rate-independent. */
static void ring_pull_frame(int16_t *ring, size_t *ring_len, int16_t *out,
                            unsigned long *starved)
{
    if (*ring_len >= (size_t)FRAME_SAMPLES) {
        memcpy(out, ring, FRAME_BYTES);
        memmove(ring, ring + FRAME_SAMPLES, (*ring_len - FRAME_SAMPLES) * 2);
        *ring_len -= (size_t)FRAME_SAMPLES;
    } else {
        int16_t fill = *ring_len ? ring[*ring_len - 1] : 0;
        for (int i = 0; i < FRAME_SAMPLES; i++) out[i] = fill;
        *ring_len = 0;
        (*starved)++;
    }
}

static int helper_main(int pcm_fd, int as_fd)
{
    signal(SIGPIPE, SIG_IGN);
    int one = 1;
    setsockopt(as_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    fcntl(pcm_fd, F_SETFL, O_NONBLOCK);

    static int16_t ring[RING_SAMPLES];
    size_t ring_len = 0;                       /* in SAMPLES, not bytes */
    static uint8_t payload[65536];
    int16_t out[FRAME_SAMPLES];

    /* The DSP's own sample rate. 9600 is slmodemd's native rate and the only
       one at which V.8 -- and therefore V.34, V.90 and V.92 -- will create;
       8000 is the previous build and the rollback. */
    /* DEFAULTS TO 8000, which is the rate the pool is proven on, and 9600 is
       opt-in. Measurement establishes that 9600 is the only rate at which V.8 will
       create -- and therefore the only rate at which V.34 is reachable at all
       -- but reaching the V.34 datapump is not the same claim as completing a
       V.34 handshake, and the second one is NOT yet measured. Two synthetic
       rigs got as far as V.34 phase 2 and stalled in "Repeated info0", and
       neither rig is trustworthy enough to say whether that is the modem or
       the rig. Until a real call settles it, the default must be the rate that
       is known to carry subscribers. */
    const char *dr = bridge_env("DSP_RATE");
    const int dsp_rate = (dr && *dr) ? atoi(dr) : AS_RATE;
    const int resampling = (dsp_rate != AS_RATE);
    /* after dsp_rate: the resampled path gets a larger ring. See ring_frames_for(). */
    const size_t ring_cap = (size_t)ring_frames_for(dsp_rate) * FRAME_SAMPLES;
    static resamp_t up, down;
    if (resampling) {
        /* 8000 -> 9600 is 6/5 up; 9600 -> 8000 is 5/6 down. The two are not
           the same filter -- see rs_init_pair and the block comment above it. */
        rs_init_pair(&up, &down, dsp_rate);
    }
    static int16_t upbuf[8192], dnbuf[8192], insamp[4096];
    static uint8_t rawbuf[8192];
    size_t rawlen = 0;                         /* odd trailing byte lives here */
    unsigned long tx = 0, rx = 0, starved = 0, dropped = 0, dup = 0, overrun = 0;
    /* RX-frame arrival jitter. slmodemd is clocked by how fast audio arrives
       (SLMBRIDGE_TX_CLOCK=rx), so irregular delivery IS an irregular sample clock
       to the DSP. Asterisk applies NO jitter buffer on this path -- checked,
       nothing in the dialplan or pjsip.conf sets one -- so RTP jitter from the
       caller's ATA reaches the equalizer unsmoothed. V.32bis at 2400 baud does
       not care; a 3429-baud V.34 constellation might. Measured here rather
       than argued about: the DSP's own equalizer error separates outcomes
       completely (equerr>300 is 0 for 16) and the line probe says the
       FREQUENCY response is identical on good and bad calls, which leaves
       timing as the thing the probe cannot see. */
    unsigned long jb_n = 0, jb_late = 0;
    double jb_sum = 0.0, jb_sumsq = 0.0, jb_max = 0.0;
    struct timespec jb_prev = {0, 0};
    const char *why = "loop ended";

    /* Sample slip is OPT-IN and defaults OFF. It is written, it is correct in
       principle, and it made things measurably worse -- see the note at the top
       of this section. Enable with SLMBRIDGE_ELASTIC=1
       once the underlying rate question is settled. */
    const char *el = bridge_env("ELASTIC");
    const int elastic = el && *el == '1';

    /* ------------------------------------------------------------------ *
     * TX CLOCK -- the fix the rate measurements pointed at.
     *
     * slmodemd is sample-SYNCHRONOUS: its main loop (modem_main.c ~962-996)
     * reads `count` samples from this helper's socketpair, runs modem_process()
     * on exactly `count`, and writes exactly `count` back. It has no internal
     * sample clock -- its cadence is entirely how fast and how much we feed it.
     * We are its clock.
     *
     * The RX direction already honours that: Asterisk hands us one 20 ms slin
     * frame every 20 ms on its own PSTN-derived clock, and we write each one
     * straight to slmodemd, so the DSP advances at exactly Asterisk's rate.
     *
     * The bug was the TX direction. It drained the ring on an INDEPENDENT
     * CLOCK_MONOTONIC 20 ms timer, and CLOCK_MONOTONIC's 20 ms is not Asterisk's
     * 20 ms -- two crystals, drifting. slmodemd produces output at Asterisk's
     * rate (it is input-locked); we consumed it at the monotonic rate. The
     * difference accumulated in the ring until it starved or overran, and either
     * one drops or repeats a 20 ms chunk of the modem's TRANSMITTED signal.
     * V.32bis survives that; V.34's Phase 2 INFO exchange does not, and that is
     * the "Repeated info0" that stalls every faster train. Measured: at 8000
     * starved=153/call, at 9600 overrun=12/call -- a clock mismatch, not a rate.
     *
     * SLMBRIDGE_TX_CLOCK=rx removes the second clock. We emit exactly one frame to
     * Asterisk for each audio frame Asterisk hands us, so TX rate == RX rate ==
     * the ONE clock, structurally, and the ring only ever holds the pipeline
     * delay between feeding slmodemd and reading its reply. No drift to
     * accumulate means no starve/overrun to corrupt INFO0.
     *
     * DEFAULTS OFF. The timer path below is byte-for-byte the proven 14,400
     * path, so deploying this binary changes nothing until a line opts in. The
     * fast pool stays on the rate that carries subscribers until a real call
     * says otherwise -- the same discipline the 8000 build was held to. */
    const char *cm = bridge_env("TX_CLOCK");
    const int rx_clock = cm && !strcmp(cm, "rx");

    /* A small pre-fill so the first RX-triggered drains have slmodemd output to
       send instead of starving while the DSP spins up. 2 frames = 40 ms, which
       is also the whole steady-state latency this path adds. A dial because the
       right value is the pipeline depth and that is worth measuring, not
       guessing. Only meaningful in rx-clock mode. */
    if (rx_clock) {
        const char *pf = bridge_env("RX_PREFILL");
        int frames = (pf && *pf) ? atoi(pf) : 2;
        if (frames < 0) frames = 0;
        if ((size_t)frames > ring_cap / FRAME_SAMPLES) frames = (int)(ring_cap / FRAME_SAMPLES);
        ring_len = (size_t)frames * FRAME_SAMPLES;
        memset(ring, 0, ring_len * 2);
    }

    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    ts_add_ns(&next, 20L * 1000000L);

    for (;;) {
        struct timespec now;
        long wait_ms;
        if (rx_clock) {
            /* No monotonic deadline to chase -- Asterisk's frames are the
               clock. 200 ms is only a keepalive fallback for a silent trunk. */
            wait_ms = 200;
        } else {
            clock_gettime(CLOCK_MONOTONIC, &now);
            wait_ms = (next.tv_sec - now.tv_sec) * 1000L
                    + (next.tv_nsec - now.tv_nsec) / 1000000L;
            if (wait_ms < 0) wait_ms = 0;
        }

        int got_audio = 0;
        struct pollfd pfd[2];
        pfd[0].fd = as_fd;  pfd[0].events = POLLIN;
        pfd[1].fd = pcm_fd; pfd[1].events = POLLIN;
        int pr = poll(pfd, 2, (int)wait_ms);

        if (pr > 0) {
            if (pfd[0].revents & (POLLERR | POLLHUP)) { why = "POLLERR/POLLHUP on broker socket"; break; }
            if (pfd[1].revents & (POLLERR | POLLHUP)) { why = "POLLERR/POLLHUP on slmodemd socketpair"; break; }

            /* Asterisk -> slmodemd, straight through: slmodemd's reads are what
               pace its DSP, so this direction must not be re-timed. */
            if (pfd[0].revents & POLLIN) {
                uint8_t hdr[3];
                if (read_exact(as_fd, hdr, 3) < 0) { why = "EOF reading TLV header from broker"; break; }
                uint16_t len = (uint16_t)((hdr[1] << 8) | hdr[2]);
                if (len && read_exact(as_fd, payload, len) < 0) { why = "EOF reading TLV payload"; break; }
                if (hdr[0] == AS_AUDIO) {
                    if (!resampling) {
                        ssize_t w = write(pcm_fd, payload, len); (void)w;
                    } else {
                        /* 160 samples of 8 kHz become 192 of 9.6 kHz: the same
                           20 ms, at the rate the DSP keeps time in. Feeding it
                           160 is what starved it by 20% forever. */
                        int nin = (int)(len / 2);
                        if (nin > (int)(sizeof insamp / 2)) nin = (int)(sizeof insamp / 2);
                        memcpy(insamp, payload, (size_t)nin * 2);
                        rs_dump(0, insamp, nin);
                        int n = rs_process(&up, insamp, nin, upbuf,
                                           (int)(sizeof upbuf / 2));
                        ssize_t w = write(pcm_fd, upbuf, (size_t)n * 2); (void)w;
                    }
                    rx++;
                    got_audio = 1;
                    {   /* inter-arrival gap in ms; nominal is 20 */
                        struct timespec now;
                        clock_gettime(CLOCK_MONOTONIC, &now);
                        if (jb_prev.tv_sec) {
                            double ms = (now.tv_sec - jb_prev.tv_sec) * 1000.0
                                      + (now.tv_nsec - jb_prev.tv_nsec) / 1e6;
                            jb_n++; jb_sum += ms; jb_sumsq += ms * ms;
                            if (ms > jb_max) jb_max = ms;
                            if (ms > 40.0) jb_late++;      /* a whole frame late */
                        }
                        jb_prev = now;
                    }
                } else if (hdr[0] == AS_HANGUP || hdr[0] == AS_ERROR) {
                    why = "AudioSocket HANGUP/ERROR"; break;
                }
            }

            /* slmodemd -> ring */
            if (pfd[1].revents & POLLIN) {
                /* Room in the ring is in 8 kHz samples; at 9.6 kHz the DSP has
                   to hand us 6 for every 5 that fit, so bound the READ by what
                   the ring can hold AFTER conversion, not before. */
                size_t out_room = ring_cap - ring_len;
                size_t in_room  = resampling ? out_room * 6 / 5 : out_room;
                size_t room     = in_room * 2;
                if (room > sizeof rawbuf - rawlen) room = sizeof rawbuf - rawlen;
                if (out_room > 0 && room > 0) {
                    ssize_t r = read(pcm_fd, rawbuf + rawlen, room);
                    if (r > 0) {
                        rawlen += (size_t)r;
                        /* A read can end mid-sample. slmodemd and this helper
                           exchange raw unframed PCM with no header, so half a
                           sample carried into the next read leaves the stream
                           permanently one byte out of step and every sample
                           after it is two half-samples. Keep the odd byte. */
                        size_t nsamp = rawlen / 2;
                        if (nsamp > sizeof insamp / 2) nsamp = sizeof insamp / 2;
                        memcpy(insamp, rawbuf, nsamp * 2);
                        size_t used = nsamp * 2;
                        if (rawlen > used) memmove(rawbuf, rawbuf + used, rawlen - used);
                        rawlen -= used;

                        if (!resampling) {
                            size_t n = nsamp < out_room ? nsamp : out_room;
                            memcpy(ring + ring_len, insamp, n * 2);
                            ring_len += n;
                        } else {
                            rs_dump(1, insamp, (int)nsamp);
                            int n = rs_process(&down, insamp, (int)nsamp, dnbuf,
                                               (int)(sizeof dnbuf / 2));
                            if ((size_t)n > out_room) n = (int)out_room;
                            memcpy(ring + ring_len, dnbuf, (size_t)n * 2);
                            ring_len += (size_t)n;
                        }
                    }
                } else {
                    /* Should not happen once the elastic loop is steering, and
                       is counted so that it cannot happen silently. */
                    uint8_t waste[4096];
                    ssize_t r = read(pcm_fd, waste, sizeof waste);
                    if (r > 0) overrun++;
                }
            }
        }

        /* RX-CLOCKED TX. One frame out per audio frame in, so our transmit rate
           is Asterisk's receive rate exactly -- the single-clock path. The
           monotonic-timer block below never runs in this mode. */
        if (rx_clock) {
            if (got_audio || pr == 0) {
                int16_t rout[FRAME_SAMPLES];
                ring_pull_frame(ring, &ring_len, rout, &starved);
                if (emit_frame(as_fd, rout) < 0) { why = "short write to broker"; break; }
                tx++;
            }
            continue;
        }

        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > next.tv_sec ||
            (now.tv_sec == next.tv_sec && now.tv_nsec >= next.tv_nsec)) {

            /* Steer occupancy toward TARGET_SAMPLES by consuming one sample
               more or less than we emit. */
            int consume = FRAME_SAMPLES;
            long err = elastic ? (long)ring_len - TARGET_SAMPLES : 0;
            if (err > HYSTERESIS && ring_len >= (size_t)FRAME_SAMPLES + 1)
                consume = FRAME_SAMPLES + 1;          /* running long: delete one */
            else if (err < -HYSTERESIS && ring_len >= (size_t)FRAME_SAMPLES - 1)
                consume = FRAME_SAMPLES - 1;          /* running short: insert one */

            if (ring_len >= (size_t)consume) {
                if (consume == FRAME_SAMPLES) {
                    memcpy(out, ring, FRAME_SAMPLES * 2);
                } else if (consume == FRAME_SAMPLES + 1) {
                    int k = quietest_index(ring, 1, consume - 1);
                    memcpy(out, ring, (size_t)k * 2);
                    memcpy(out + k, ring + k + 1, (size_t)(FRAME_SAMPLES - k) * 2);
                    dropped++;
                } else {
                    int k = quietest_index(ring, 1, consume - 1);
                    memcpy(out, ring, (size_t)k * 2);
                    out[k] = ring[k];                 /* repeat one sample */
                    memcpy(out + k + 1, ring + k, (size_t)(FRAME_SAMPLES - k - 1) * 2);
                    dup++;
                }
                memmove(ring, ring + consume, (ring_len - (size_t)consume) * 2);
                ring_len -= (size_t)consume;
            } else {
                /* Genuinely nothing to send. Hold the last level rather than
                   slamming to zero, and never go silent -- 2 s of silence is
                   AudioSocket's liveness timeout.

                   ⚠️ MEASURED HAZARD, 2026-09-01. That justification is about
                   the liveness timeout only, and against THAT goal a constant
                   is fine. As a SIGNAL it is the worst available filler: a
                   whole frame of DC, all energy at 0 Hz, an amplitude step at
                   each edge, and nothing for a remote receiver's carrier or
                   timing recovery to track through.

                   On the V.34 experimental line, starvation is strongly
                   associated with the residual handshake failure -- 0.174
                   events/s on failures against 0.059 on calls that connect,
                   with every single failure above 0.083/s (Mann-Whitney
                   p < 0.00001, n=72). The absolute counts are tiny, 1-2 per
                   call, which is the point: what matters is WHERE the frame
                   lands. One DC frame in data mode is absorbed by error
                   control; the same frame during V.34 Phase 3/4 training
                   breaks the far end's acquisition.

                   PRODUCTION IS UNAFFECTED IN PRACTICE and is deliberately
                   left alone: V.32bis at 2400 baud rides through a lost 20 ms
                   and LAPM retransmits, and those lines log zero HDLC frame
                   errors across a day.

                   Replacing the fill with a repeat of the last good frame was
                   tried and is NOT a fix -- 13/16 vs 10/16 connects, Fisher
                   p = 0.433, and the build carrying it showed an unexplained
                   low-rate regression. Measured before trying it again. */
                int16_t fill = ring_len ? ring[ring_len - 1] : 0;
                for (int i = 0; i < FRAME_SAMPLES; i++) out[i] = fill;
                ring_len = 0;
                starved++;
            }

            uint8_t msg[3 + FRAME_BYTES];
            msg[0] = AS_AUDIO;
            msg[1] = (FRAME_BYTES >> 8) & 0xFF;
            msg[2] = FRAME_BYTES & 0xFF;
            memcpy(msg + 3, out, FRAME_BYTES);
            if (write(as_fd, msg, sizeof msg) != (ssize_t)sizeof msg) { why = "short write to broker"; break; }
            tx++;

            ts_add_ns(&next, 20L * 1000000L);
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec > next.tv_sec ||
                (now.tv_sec == next.tv_sec && now.tv_nsec > next.tv_nsec)) {
                next = now;
                ts_add_ns(&next, 20L * 1000000L);
            }
        }
    }

    fprintf(stderr, "slmbridge helper: exit (%s) tx=%lu rx=%lu starved=%lu "
            "slip-del=%lu slip-ins=%lu overrun=%lu rxgap-mean=%.1fms "
            "rxgap-sd=%.1fms rxgap-max=%.1fms rxgap-late=%lu\n",
            why, tx, rx, starved, dropped, dup, overrun,
            jb_n ? jb_sum / (double)jb_n : 0.0,
            jb_n > 1 ? sqrt(jb_sumsq / (double)jb_n
                            - (jb_sum / (double)jb_n) * (jb_sum / (double)jb_n)) : 0.0,
            jb_max, jb_late);
    return 0;
}

/* ---------------------------------------------------------------- broker --
 * Holds the Asterisk side and hands it to whichever helper slmodemd spawns for
 * the current call. slmodemd stays resident, so the helper cannot inherit the
 * call's socket -- it connects to us instead, and we relay. Both sides already
 * speak AudioSocket TLV, so this is a dumb byte pump; at 16 kB/s the extra copy
 * costs nothing measurable.
 */
static int relay(int a, int b)
{
    uint8_t buf[8192];
    struct pollfd p[2];
    p[0].fd = a; p[1].fd = b;
    for (;;) {
        p[0].events = p[1].events = POLLIN;
        if (poll(p, 2, 30000) <= 0) return -1;
        for (int i = 0; i < 2; i++) {
            if (p[i].revents & (POLLERR | POLLHUP)) return 0;
            if (p[i].revents & POLLIN) {
                ssize_t n = read(p[i].fd, buf, sizeof buf);
                if (n <= 0) return 0;
                int out = p[i].fd == a ? b : a;
                ssize_t off = 0;
                while (off < n) {
                    ssize_t w = write(out, buf + off, (size_t)(n - off));
                    if (w <= 0) return 0;
                    off += w;
                }
            }
        }
    }
}

static pid_t pppd_pid;

/* The AudioSocket listening socket, so relay_and_watch() can refuse a second
   caller while this broker is busy. File scope for the same reason pppd_pid is:
   the relay loop needs it and threading it through three call sites to say one
   thing is worse. -1 until main() has bound and listened. */
static int g_listen_fd = -1;
static char  connect_rate[32];
/* The OTHER carrier. See src/slmbridge-narrow.c for the full account: V.34
 * negotiates each direction independently, slmodemd's "CONNECT <n>" carries
 * its rx (UPSTREAM), and the downstream figure arrives on a separate
 * "TxRate: <n>" line gated on S70 bit 2 (AT&A).
 *
 * MIRRORED HERE DELIBERATELY, UNBUILT. This file is the PRODUCTION bridge and
 * its binary is pinned by md5 b0c8eecf4883c461a4ca65a5cc672d37; the change is
 * carried in the source so the two files do not drift, which is a failure this
 * project has paid for before. On the production pool today both directions
 * are V.32bis 14,400 and symmetric, so this reports the same number twice
 * until the fast pool actually carries V.34. */
static char  tx_rate[32];

/* Hand the modem TTY to pppd once the modem says CONNECT.
 *
 * The broker holds the TTY only to write AT commands and to read the result
 * code. The moment CONNECT appears the TTY becomes a data path and pppd must
 * own it exclusively -- so we close our descriptor before exec'ing, or two
 * readers race for every PPP frame and the link never comes up. */

/* ------------------------------------------------------------------------
 * Session files.
 *
 * modemd has written /run/litenet/sessions/<subscriber-ip> since Phase 3, and
 * two separate things read it: the NOC console joins on it to show a line's
 * rate, and retroproxy calls BpsFor(ip) to pick a compression profile.
 *
 * The fast pool never got this half of the contract. Every fast-pool call
 * therefore brought up a ppp interface with no session file, which produced
 * the console warning "N ppp interface(s) up but only 0 session file(s)" --
 * and, far worse than a warning, made retroproxy fall back to --default-bps
 * 14400 for a caller who had automoded DOWN. A subscriber who trained at 4,800
 * was being served pages budgeted for 14,400. The journal shows exactly that:
 * "slmbridge: CONNECT 4800".
 *
 * Same path, same format, same lifetime as modemd's: create when the carrier
 * trains, unlink when it goes away. A leaked file hands the NEXT subscriber on
 * that address somebody else's rate, which is why teardown matters as much as
 * creation.
 *
 * The rate written is the ACTUAL negotiated rate parsed from CONNECT, not the
 * line's configured maximum -- that is the entire value of doing this here.
 * ---------------------------------------------------------------------- */
static char session_path[128];

static void session_file_write(const char *rate)
{
    const char *ip = bridge_env("SESSION_IP");
    if (!ip || !*ip || !rate || !*rate) {
        fprintf(stderr, "slmbridge: no session file -- ip=[%s] rate=[%s]\n",
                ip ? ip : "(unset)", rate ? rate : "(null)");
        return;
    }

    if (mkdir("/run/litenet", 0755) != 0 && errno != EEXIST) return;
    if (mkdir("/run/litenet/sessions", 0755) != 0 && errno != EEXIST) return;

    snprintf(session_path, sizeof session_path, "/run/litenet/sessions/%s", ip);
    FILE *f = fopen(session_path, "w");
    if (!f) {
        fprintf(stderr, "slmbridge: could not write %s: %s\n", session_path, strerror(errno));
        session_path[0] = '\0';
        return;
    }
    /* First line is the bare rate and nothing else -- retroproxy and the
     * console both parse it that way, and modemd writes it that way. Anything
     * added later goes on a SECOND line.
     *
     * That contract had a trap in it: the ip-up hook digit-stripped the WHOLE
     * file, so a second line would have been concatenated onto the rate --
     * 33600 and 26400 becoming 3360026400, which no reader could reject. The
     * hook takes the first line only as of 2026-09-01. Check every reader
     * before adding a third line. */
    fprintf(f, "%d\n", atoi(rate));
    if (tx_rate[0])
        fprintf(f, "down=%d up=%d\n", atoi(tx_rate), atoi(rate));
    fclose(f);
    fprintf(stderr, "slmbridge: session file %s = %d%s\n", session_path,
            atoi(rate), tx_rate[0] ? " (with both carriers)" : "");
}

static void session_file_clear(void)
{
    if (session_path[0]) { unlink(session_path); session_path[0] = '\0'; }
}

static void hand_off_to_pppd(int *tfd, const char *tty)
{
    const char *args = bridge_env("PPPD");
    const char *netns = bridge_env("NETNS");
    if (!args || pppd_pid > 0)
        return;

    if (*tfd >= 0) { close(*tfd); *tfd = -1; }

    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        for (int fd = 3; fd < 256; fd++) close(fd);
        char *av[48];
        int i = 0;
        if (netns) {
            av[i++] = "/usr/sbin/ip"; av[i++] = "netns";
            av[i++] = "exec";         av[i++] = (char *)netns;
        }
        av[i++] = "/usr/sbin/pppd";
        av[i++] = (char *)tty;
        static char abuf[1024];
        snprintf(abuf, sizeof abuf, "%s", args);
        for (char *tok = strtok(abuf, " "); tok && i < 46; tok = strtok(NULL, " "))
            av[i++] = tok;
        av[i] = NULL;
        execv(av[0], av);
        _exit(127);
    }
    pppd_pid = pid;
    session_file_write(connect_rate);
    fprintf(stderr, "slmbridge: CONNECT %s -- pppd pid %d on %s%s%s\n",
            connect_rate[0] ? connect_rate : "?", pid, tty,
            netns ? " in netns " : "", netns ? netns : "");
}

/* Relay audio, and simultaneously watch the modem TTY for its result code so
   pppd can be started the instant the carrier is up.
 *
 * tfd is IN/OUT. hand_off_to_pppd() closes the TTY descriptor and sets it to
 * -1 the moment CONNECT arrives, and the caller has to see that. It used to be
 * passed by value, so broker_main kept a copy of a descriptor this function had
 * already closed -- and then wrote its end-of-call "ATH\r" into it. The write
 * failed with EBADF and the return value was discarded, so the modem was never
 * hung up: it stayed OFF-HOOK after every call that reached CONNECT, and the
 * next ATA was ignored. A fast line answered exactly ONE call per restart and
 * gave every caller after that eight seconds of silence and
 * "helper never connected -- is slmodemd running?".
 *
 * That was invisible for as long as it existed because slmodemd used to SIGABRT
 * at teardown (a guard patched into slmodemd's fd-set handling), which restarted the unit and
 * re-armed the line by accident. Fixing the crash removed the accidental
 * recovery and left this. Worse than a plain EBADF: fd numbers get reused, so
 * "ATH\r" could land in whatever descriptor took that number, and the close()
 * that followed was a double close. */
static int relay_and_watch(int a, int b, int *tfdp)
{
    uint8_t buf[8192];
    char line[256];
    size_t lp = 0;
    struct pollfd p[4];
    for (;;) {
        int tfd = *tfdp;
        int n = 0;
        p[0].fd = a; p[0].events = POLLIN; n++;
        p[1].fd = b; p[1].events = POLLIN; n++;
        p[2].fd = tfd; p[2].events = POLLIN;
        int nfds = (tfd >= 0) ? 3 : 2;

        /* Watch the listening socket DURING the call, so this broker refuses a
           second caller instead of parking them.

           The service loop is strictly serial -- accept, drive AT, relay for
           the length of the call, close -- and the listen backlog was 4, so a
           second connection completed its TCP handshake and sat in the queue
           with nobody reading it. Asterisk's connect() succeeded, so Asterisk
           answered the caller, and that caller heard nothing until the
           AudioSocket liveness timeout fired. Indistinguishable from a dead
           line.

           extensions.conf already describes this exposure and names this as
           the fix: "the proper fix is slmbridge refusing a second connection
           on a busy broker, which makes the broker the allocator and this loop
           merely a hint". Accepting and immediately closing is that refusal --
           the loser gets a closed connection at once and the dialplan's
           GROUP_COUNT race stops being able to strand anybody. */
        int li = -1;
        if (g_listen_fd >= 0) {
            li = nfds;
            p[nfds].fd = g_listen_fd; p[nfds].events = POLLIN; nfds++;
        }

        if (poll(p, nfds, 30000) <= 0) return -1;

        if (li >= 0 && (p[li].revents & POLLIN)) {
            int extra = accept(g_listen_fd, NULL, NULL);
            if (extra >= 0) {
                fprintf(stderr, "slmbridge: refused a second AudioSocket "
                                "connection -- this broker is already on a call\n");
                close(extra);
            }
        }

        if (tfd >= 0 && (p[2].revents & POLLIN)) {
            ssize_t r = read(tfd, buf, sizeof buf);
            for (ssize_t i = 0; i < r; i++) {
                char c = (char)buf[i];
                if (c == '\r' || c == '\n') {
                    line[lp] = '\0';
                    if (!strncmp(line, "TxRate:", 7)) {
                        const char *sp = line + 7;
                        while (*sp == ' ') sp++;
                        snprintf(tx_rate, sizeof tx_rate, "%s", sp);
                    }
                    if (!strncmp(line, "CONNECT", 7)) {
                        const char *sp = strchr(line, ' ');
                        snprintf(connect_rate, sizeof connect_rate, "%s",
                                 sp ? sp + 1 : "");
                        hand_off_to_pppd(tfdp, bridge_env("TTY"));
                        tfd = *tfdp;   /* now -1; the fd belongs to pppd */
                    }
                    lp = 0;
                } else if (lp < sizeof line - 1) {
                    line[lp++] = c;
                }
            }
        }
        for (int i = 0; i < 2; i++) {
            if (p[i].revents & (POLLERR | POLLHUP)) return 0;
            if (p[i].revents & POLLIN) {
                ssize_t r = read(p[i].fd, buf, sizeof buf);
                if (r <= 0) return 0;
                int out = p[i].fd == a ? b : a;
                ssize_t off = 0;
                while (off < r) {
                    ssize_t w = write(out, buf + off, (size_t)(r - off));
                    if (w <= 0) return 0;
                    off += w;
                }
            }
        }
    }
}

static int broker_main(int port, const char *unix_path, const char *tty)
{
    setenv("SLMBRIDGE_TTY", tty, 1);
    setenv("LITENET_TTY", tty, 1); /* the older spelling, for scripts that read it */
    signal(SIGPIPE, SIG_IGN);
    unlink(unix_path);
    int us = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un ua = {0};
    ua.sun_family = AF_UNIX;
    snprintf(ua.sun_path, sizeof ua.sun_path, "%s", unix_path);
    if (bind(us, (struct sockaddr *)&ua, sizeof ua) < 0) { perror("bind unix"); return 1; }
    /* slmodemd drops privileges to an unprivileged user before it forks the
       helper, so a root-owned 0755 socket is unreachable to it -- the helper
       fails with EACCES, exits, and slmodemd then FD_SETs the now-dead fd and
       aborts with "bit out of range 0 - FD_SETSIZE". The abort is a downstream
       symptom; this chmod is the actual fix. */
    if (chmod(unix_path, 0666) < 0) perror("chmod unix socket");
    listen(us, 4);

    int ts = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ts, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in ta = {0};
    ta.sin_family = AF_INET;
    ta.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ta.sin_port = htons((uint16_t)port);
    if (bind(ts, (struct sockaddr *)&ta, sizeof ta) < 0) { perror("bind tcp"); return 1; }
    /* Backlog 1, not 4. One broker serves one call, so a queue of four was
       four callers who could be accepted by the kernel and served by nobody.
       relay_and_watch() closes anything that arrives mid-call; the shallow
       backlog just narrows the window before it gets there. */
    listen(ts, 1);
    g_listen_fd = ts;

    fprintf(stderr, "slmbridge broker: AudioSocket on 127.0.0.1:%d, helper on %s, "
                    "modem tty %s\n", port, unix_path, tty);

    /* Configure the modem ONCE, now, before any call exists.
     *
     * Doing this per-call is fatal and was: slmodemd's AT parser needs a beat
     * between commands, four commands at 500 ms is two seconds, and two seconds
     * of sending Asterisk nothing is exactly AudioSocket's MAX_WAIT_TIMEOUT_MSEC.
     * Asterisk tore every call down before the modem could attach, and the
     * helper saw only an already-closed socket. A real modem is configured at
     * startup and answers instantly; so is this one. */
    {
        const char *at_init = bridge_env("AT_INIT");
        if (!at_init) at_init = "ATZ;ATX4;AT+MS=132,1,1200,14400;AT+MS?";
        int t = open(tty, O_RDWR | O_NOCTTY);
        if (t < 0) {
            fprintf(stderr, "slmbridge: cannot open %s: %s\n", tty, strerror(errno));
            return 1;
        }
        char list[512], buf[256];
        snprintf(list, sizeof list, "%s", at_init);
        for (char *tok = strtok(list, ";"); tok; tok = strtok(NULL, ";")) {
            snprintf(buf, sizeof buf, "%s\r", tok);
            ssize_t w = write(t, buf, strlen(buf)); (void)w;
            usleep(500000);
        }
        close(t);
        fprintf(stderr, "slmbridge: modem configured with '%s'\n", at_init);
    }

    for (;;) {
        int as_fd = accept(ts, NULL, NULL);
        if (as_fd < 0) { if (errno == EINTR) continue; break; }
        setsockopt(as_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        fprintf(stderr, "slmbridge: call in; answering modem via %s\n", tty);

        /* Drive slmodemd's AT layer. The init string is where V.90/V.92 get
           disabled: slmodemd defaults to `92,1,300,56000` (AT+MS?), and 56k
           cannot work over RTP -- there is no shared 8 kHz clock and the jitter
           buffer exists to drop and repeat packets. Worse, asking for it wastes
           handshake time and falls back *worse* than never asking.
           AT+MS=<dp_id>,<automode>,<min>,<max>, dp ids from modem_defs.h:
           122=V.22bis 132=V.32bis 34=V.34 90=V.90 92=V.92.
           ATX4 = full result codes, correct for an answering modem.
           ATW2 = CONNECT reports the line rate, not the 115200 serial rate. */
        /* One write, no sleeps. Anything slower than this races the 2000 ms
           AudioSocket liveness timeout. */
        const char *at_cmd = bridge_env("AT_CMD");
        if (!at_cmd) at_cmd = "ATA";
        int t = open(tty, O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (t >= 0) {
            char buf[64];
            int n = snprintf(buf, sizeof buf, "%s\r", at_cmd);
            ssize_t w = write(t, buf, (size_t)n); (void)w;
            fprintf(stderr, "slmbridge: sent '%s'\n", at_cmd);
        }
        pppd_pid = 0;
        connect_rate[0] = '\0';
        tx_rate[0] = '\0';

        struct pollfd wp = { .fd = us, .events = POLLIN };
        int hp = poll(&wp, 1, 8000);
        if (hp <= 0) {
            fprintf(stderr, "slmbridge: helper never connected -- is slmodemd running?\n");
            close(as_fd);
            if (t >= 0) close(t);
            continue;
        }
        int h_fd = accept(us, NULL, NULL);
        fprintf(stderr, "slmbridge: helper attached, relaying\n");
        relay_and_watch(as_fd, h_fd, &t);
        fprintf(stderr, "slmbridge: call ended\n");
        if (pppd_pid > 0) { kill(pppd_pid, SIGTERM); waitpid(pppd_pid, NULL, 0); pppd_pid = 0; }
        session_file_clear();
        close(h_fd);
        close(as_fd);

        /* PUT THE MODEM BACK ON HOOK. This is not tidiness, it is the whole
           reason a line can take a second call.

           On any call that reached CONNECT, hand_off_to_pppd() has closed t and
           set it to -1, so there is nothing left to write ATH to -- re-open the
           TTY and hang up on the fresh descriptor. slmodemd's AT parser is
           alive the whole time; what it will not do is honour ATA on a modem
           that is still off-hook, and it says nothing when it declines.

           Verified by hand on a line in that state: ATA alone was echoed and
           produced no result code, while ATH followed by ATA forked the audio
           helper immediately. */
        if (t < 0)
            t = open(tty, O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (t >= 0) {
            if (write(t, "ATH\r", 4) != 4)
                fprintf(stderr, "slmbridge: could not hang the modem up: %s "
                                "-- this line will refuse the next call\n",
                        strerror(errno));
            close(t);
        } else {
            fprintf(stderr, "slmbridge: could not re-open %s to hang up: %s "
                            "-- this line will refuse the next call\n",
                    tty, strerror(errno));
        }
        t = -1;

        /* slmodemd's AT layer needs a beat to process ATH before the next ATA
           arrives. 200 ms, not the 500 ms the init sequence uses: this happens
           after the caller has already hung up, so it costs nobody anything,
           and the next call is at minimum a fresh TCP connect away. */
        usleep(200000);
    }
    return 0;
}

/* Guarded so slmbridge_rstest.c can #include this file and test the SHIPPING
   resampler rather than a copy of it. A copy is how a filter gets fixed in the
   test and left broken in the binary. */
#ifndef RS_SELFTEST
int main(int argc, char **argv)
{
    /* slmodemd execs us as: <self> <dial_string> <fd>. Anything else and we
       were run by a human who wants the usage message. */
    const char *upath = bridge_env("ASOCK_PATH");
    if (!upath) upath = "/run/litenet/slm0.sock";

    if (argc >= 2 && !strcmp(argv[1], "--broker")) {
        int port = (argc > 2) ? atoi(argv[2]) : 9095;
        const char *tty = (argc > 3) ? argv[3] : "/dev/ttySL0";
        return broker_main(port, upath, tty);
    }

    /* slmodemd execs us as:
     *     execl(exec, exec, m->dial_string, fdstr, NULL)
     * On the ANSWER side there is no dial string, so m->dial_string is NULL and
     * execl terminates the argument list right there -- we are handed argc==1
     * and never see the fd at all. Requiring argc==3 meant the helper exited
     * immediately on every answered call, slmodemd then read from a socketpair
     * whose peer had gone, and aborted with an FD_SETSIZE message that pointed
     * nowhere near the cause.
     *
     * Take the fd from the LAST argument, whatever the arity, and say so
     * loudly when there isn't one. */
    if (argc >= 2) {
        int pcm_fd = atoi(argv[argc - 1]);
        int as_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un ua = {0};
        ua.sun_family = AF_UNIX;
        snprintf(ua.sun_path, sizeof ua.sun_path, "%s", upath);
        if (pcm_fd <= 0) {
            fprintf(stderr, "slmbridge helper: no usable fd in argv (argc=%d, "
                            "last='%s')\n", argc, argv[argc - 1]);
            return 1;
        }
        if (connect(as_fd, (struct sockaddr *)&ua, sizeof ua) < 0) {
            fprintf(stderr, "slmbridge helper: connect %s: %s\n",
                    upath, strerror(errno));
            return 1;
        }
        fprintf(stderr, "slmbridge helper: pcm fd %d, relaying via %s\n",
                pcm_fd, upath);
        return helper_main(pcm_fd, as_fd);
    }

    fprintf(stderr,
        "usage: slmbridge --broker [port] [tty]      run the AudioSocket broker\n"
        "       (slmodemd execs this as its --exec helper; do not run that by hand)\n");
    return 2;
}
#endif /* RS_SELFTEST */

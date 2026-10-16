/* Regression/fuzz harness for the helper's two untrusted inputs: the
 * AudioSocket TLV stream (via the broker) and the raw PCM stream from slmodemd.
 *
 * Includes the shipping slmbridge.c (RS_SELFTEST hides main) and runs the real
 * helper_main() in a forked child against two socketpairs, then throws random,
 * boundary and truncated TLV frames plus odd-sized PCM bursts at it. Build with
 * -fsanitize=address,undefined; any sanitizer report makes the child exit
 * non-zero, which fails the run. */
#define RS_SELFTEST
#include "../slmbridge.c"

static uint32_t rng = 0x2545F491u;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static int wr(int fd, const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n) { ssize_t w = write(fd, b, n); if (w <= 0) return -1; b += w; n -= (size_t)w; }
    return 0;
}

static void drain(int fd)
{
    uint8_t junk[65536];
    while (read(fd, junk, sizeof junk) > 0) {}
}

static int run_case(const char *dsp_rate, const char *clock, int iters)
{
    int as[2], pcm[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, as) || socketpair(AF_UNIX, SOCK_STREAM, 0, pcm)) return 1;
    setenv("SLMBRIDGE_DSP_RATE", dsp_rate, 1);
    if (clock) setenv("SLMBRIDGE_TX_CLOCK", clock, 1); else unsetenv("SLMBRIDGE_TX_CLOCK");
    pid_t pid = fork();
    if (pid == 0) {
        close(as[1]); close(pcm[1]);
        int fd = open("/dev/null", O_WRONLY); dup2(fd, 2);
        _exit(helper_main(pcm[0], as[0]));
    }
    close(as[0]); close(pcm[0]);
    fcntl(as[1], F_SETFL, O_NONBLOCK);
    fcntl(pcm[1], F_SETFL, O_NONBLOCK);
    static uint8_t buf[3 + 65535];
    for (int i = 0; i < iters; i++) {
        uint32_t kind = rnd() % 8;
        uint16_t len;
        switch (kind) {
        case 0: len = 65535; break;                 /* maximum length field */
        case 1: len = 0; break;                     /* empty frame */
        case 2: len = (uint16_t)(rnd() | 1); break; /* odd length */
        case 3: len = 320; break;                   /* nominal 20 ms */
        default: len = (uint16_t)(rnd() % 9000); break;
        }
        uint8_t type = (rnd() % 4) ? AS_AUDIO : (uint8_t)(rnd() % 0xFE + 1); /* never HANGUP/ERROR */
        if (type == AS_ERROR) type = AS_DTMF;
        buf[0] = type; buf[1] = (uint8_t)(len >> 8); buf[2] = (uint8_t)len;
        for (size_t k = 0; k < len; k++) buf[3 + k] = (uint8_t)rnd();
        /* blocking-ish write of the whole frame so the helper sees whole TLVs */
        fcntl(as[1], F_SETFL, 0);
        if (wr(as[1], buf, 3 + (size_t)len) < 0) break;
        fcntl(as[1], F_SETFL, O_NONBLOCK);
        /* slmodemd side: an odd-sized burst of random PCM, then drain both */
        size_t pn = rnd() % 6000;
        for (size_t k = 0; k < pn; k++) buf[k] = (uint8_t)rnd();
        ssize_t w = write(pcm[1], buf, pn); (void)w;
        drain(as[1]); drain(pcm[1]);
    }
    uint8_t hang[3] = { AS_HANGUP, 0, 0 };
    fcntl(as[1], F_SETFL, 0);
    wr(as[1], hang, 3);
    int st = 0;
    for (int t = 0; t < 500; t++) {          /* keep draining so the child never blocks */
        drain(as[1]); drain(pcm[1]);
        if (waitpid(pid, &st, WNOHANG) == pid) break;
        usleep(10000);
        if (t == 499) { kill(pid, SIGKILL); waitpid(pid, &st, 0); }
    }
    close(as[1]); close(pcm[1]);
    int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    printf("  dsp_rate=%-5s clock=%-5s iters=%d  %s\n", dsp_rate, clock ? clock : "timer", iters,
           ok ? "ok" : "FAIL");
    return !ok;
}

/* DSP_RATE is a free-form integer from the environment, and rs_init() sizes
 * its prototype as taps * (dsp_rate / 1600). Anything above 9600 overflowed
 * the fixed h[RS_TAPS_MAX * RS_MAX_L] table before validation existed. */
static int rate_validation(void)
{
    int fails = 0;
    const struct { int in, ok; } c[] = { {8000,1}, {9600,1}, {48000,0}, {11200,0}, {1000,0}, {0,0}, {-9600,0} };
    for (size_t i = 0; i < sizeof c / sizeof c[0]; i++) {
        int got = dsp_rate_supported(c[i].in);
        if (got != c[i].ok) { printf("  dsp_rate_supported(%d) = %d, want %d  FAIL\n", c[i].in, got, c[i].ok); fails++; }
    }
    static resamp_t up, down;
    rs_init_pair(&up, &down, 48000);     /* must clamp, not overflow, even if called directly */
    printf("  dsp rate validation %s\n", fails ? "FAIL" : "ok");
    return fails;
}

static int session_ip_validation(void)
{
    int fails = 0;
    const struct { const char *ip; int ok; } c[] = {
        {"192.0.2.10",1}, {"2001:db8::1",1}, {"../../etc/passwd",0}, {"a/b",0}, {"",0}, {".",0}, {"..",0},
    };
    for (size_t i = 0; i < sizeof c / sizeof c[0]; i++)
        if (session_name_ok(c[i].ip) != c[i].ok) { printf("  session_name_ok(%s) wrong  FAIL\n", c[i].ip); fails++; }
    printf("  session file name validation %s\n", fails ? "FAIL" : "ok");
    return fails;
}

int main(int argc, char **argv)
{
    int iters = argc > 1 ? atoi(argv[1]) : 300;
    signal(SIGPIPE, SIG_IGN);
    int fails = rate_validation() + session_ip_validation();
    fails += run_case("8000", NULL, iters);
    fails += run_case("8000", "rx", iters);
    fails += run_case("9600", NULL, iters);
    fails += run_case("9600", "rx", iters);
    printf(fails ? "fuzz_helper: FAIL\n" : "fuzz_helper: ok\n");
    return fails != 0;
}

/* Compare only test-owned AF_UNIX helper processes; no hardware. */
#define _GNU_SOURCE
#define RS_SELFTEST
#include "bridge.c"
#include <assert.h>

static void fixture_defaults(void)
{
    assert(setenv("SLMBRIDGE_RS_DUMP", "", 1) == 0);
    assert(setenv("SLMBRIDGE_RS_PROFILE", "", 1) == 0);
    assert(setenv("SLMBRIDGE_RS_FC", "3800", 1) == 0);
    assert(setenv("SLMBRIDGE_ELASTIC", "0", 1) == 0);
    assert(setenv("SLMBRIDGE_RING_FRAMES", "", 1) == 0);
    assert(setenv("SLMBRIDGE_RXGAP_LOG_MS", "0", 1) == 0);
    assert(setenv("SLMBRIDGE_RX_PREFILL", "4", 1) == 0);
}

static double mono_seconds(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
static void read_all(int fd, void *buffer, size_t bytes)
{
    char *p = buffer;
    while (bytes) {
        struct pollfd f = {.fd=fd, .events=POLLIN};
        assert(poll(&f, 1, 3000) > 0);
        ssize_t n = read(fd, p, bytes); assert(n > 0);
        p += n; bytes -= (size_t)n;
    }
}
static double trial(int enabled)
{
    int a[2], p[2], cap = 4096;
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, a));
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, p));
    assert(!setsockopt(a[1], SOL_SOCKET, SO_SNDBUF, &cap, sizeof cap));
    char filler[4096] = {0}; size_t filled = 0;
    for (;;) {
        ssize_t n = send(a[1], filler, sizeof filler, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n > 0) filled += (size_t)n;
        else { assert(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)); break; }
    }
    fixture_defaults();
    setenv("SLMBRIDGE_STREAM_IO", enabled ? "1" : "0", 1);
    setenv("SLMBRIDGE_DSP_RATE", "8000", 1);
    setenv("SLMBRIDGE_TX_CLOCK", "", 1);
    pid_t child = fork(); assert(child > -1);
    if (!child) {
        close(a[0]); close(p[0]); alarm(5);
        _exit(helper_main(p[1], a[1]));
    }
    close(a[1]); close(p[1]);
    int16_t samples[480] = {0};
    assert(write(p[0], samples, sizeof samples) == sizeof samples);
    assert(poll(NULL, 0, 350) == 0);
    while (filled) {
        size_t n = filled < sizeof filler ? filled : sizeof filler;
        read_all(a[0], filler, n); filled -= n;
    }
    unsigned char frame[323]; read_all(a[0], frame, sizeof frame);
    assert(frame[0] == 0x10 && frame[1] == 1 && frame[2] == 64);
    double first = mono_seconds();
    read_all(a[0], frame, sizeof frame);
    assert(frame[0] == 0x10 && frame[1] == 1 && frame[2] == 64);
    double gap = mono_seconds() - first;
    unsigned char hangup[3] = {0}; assert(write(a[0], hangup, 3) == 3);
    int status; assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(a[0]); close(p[0]);
    return gap;
}
int main(void)
{
    for (int n = 0; n < 3; n++) {
        double old_gap = trial(0), new_gap = trial(1);
        assert(new_gap >= .010); /* Reject the prior immediate extra frame. */
        printf("trial%d after350ms stall: legacy next-frame gap=%.3fms enabled gap=%.3fms\n",
               n, old_gap*1000, new_gap*1000);
    }
}

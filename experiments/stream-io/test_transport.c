#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
static ssize_t fake_send(int, const void *, size_t, int);
static ssize_t fake_recv(int, void *, size_t, int);
#define send fake_send
#define recv fake_recv
#include "bridge_stream_io.h"
#undef send
#undef recv
static uint8_t captured[131072], incoming[131072];
static size_t captured_n, input_n, input_pos, fragment, calls;
static int mode;
static ssize_t fake_send(int fd, const void *p, size_t n, int flags)
{
    (void)fd; assert(flags == (MSG_DONTWAIT | MSG_NOSIGNAL)); calls++;
    if (mode == 1) { errno = EAGAIN; return -1; }
    if (mode == 2) { errno = EINTR; return -1; }
    if (mode == 3) return 0;
    if (mode == 4) { errno = EPIPE; return -1; }
    if (mode == 5 && calls == 2) { errno = EAGAIN; return -1; }
    if (n > fragment) n = fragment;
    assert(captured_n + n <= sizeof captured);
    memcpy(captured + captured_n, p, n); captured_n += n;
    return (ssize_t)n;
}
static ssize_t fake_recv(int fd, void *p, size_t n, int flags)
{
    (void)fd; assert(flags == MSG_DONTWAIT); calls++;
    if (mode == 1) { errno = EAGAIN; return -1; }
    if (mode == 2) { errno = EINTR; return -1; }
    if (mode == 4) { errno = ECONNRESET; return -1; }
    if (input_pos == input_n) return 0;
    if (n > fragment) n = fragment;
    if (n > input_n - input_pos) n = input_n - input_pos;
    memcpy(p, incoming + input_pos, n); input_pos += n;
    return (ssize_t)n;
}
int main(void)
{
    uint8_t data[BIO_CAP], payload[65536];
    for (size_t i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i * 131 + i / 256);
    for (fragment = 1; fragment <= 323; fragment++) {
        struct bio_queue q = {0}; captured_n = calls = 0; mode = 0;
        assert(bio_put(&q, data, sizeof data) == 0);
        assert(bio_flush(&q, 2) == 1 && !q.len && !q.head);
        assert(captured_n == sizeof data && !memcmp(captured, data, sizeof data));
    }
    for (mode = 1; mode <= 4; mode++) {
        struct bio_queue q = {0}; captured_n = calls = 0;
        assert(!bio_put(&q, data, sizeof data));
        int r = bio_flush(&q, 2);
        assert(r == (mode <= 2 ? 0 : -1));
        assert(calls == 1 && q.len == sizeof data && !q.head && !captured_n);
        assert(errno == (mode == 1 ? EAGAIN : mode == 2 ? EINTR : mode == 3 ? EIO : EPIPE));
    }
    struct bio_queue q = {0}; captured_n = calls = 0; mode = 5; fragment = 7;
    assert(!bio_put(&q, data, 323)); assert(!bio_flush(&q, 2));
    assert(q.head == 7 && q.len == 316 && captured_n == 7);
    assert(bio_put(&q, data, 1) == -1 && errno == ENOBUFS);
    mode = 0; assert(bio_flush(&q, 2) == 1);
    assert(!memcmp(data, captured, 323));
    assert(bio_put(&q, data, BIO_CAP + 1) == -1 && errno == ENOBUFS);
    incoming[0] = 0x10; incoming[1] = 32; incoming[2] = 0;
    memcpy(incoming + 3, data, 8192); memcpy(incoming + 8195, incoming, 8195);
    for (fragment = 1; fragment <= 8195; fragment++) {
        struct bio_tlv t = {0}; input_pos = 0; input_n = 16390; calls = 0; mode = 0;
        assert(bio_tlv_read(&t, 2, payload, sizeof payload) == 1);
        assert(input_pos == 8195 && t.len == 8192 && !memcmp(payload, data, 8192));
        memset(&t, 0, sizeof t);
        assert(bio_tlv_read(&t, 2, payload, sizeof payload) == 1 && input_pos == 16390);
    }
    for (size_t cutoff = 0; cutoff < 8195; cutoff++) {
        struct bio_tlv t = {0}; input_pos = 0; input_n = cutoff; fragment = 8195; mode = 0;
        assert(bio_tlv_read(&t, 2, payload, sizeof payload) == (cutoff ? -1 : -2));
        if (cutoff) assert(errno == EPROTO);
    }
    for (mode = 1; mode <= 2; mode++) {
        struct bio_tlv t = {0}; input_pos = calls = 0; input_n = 8195;
        assert(!bio_tlv_read(&t, 2, payload, sizeof payload) && calls == 1 && !input_pos);
    }
    unsigned invalid[] = {0, 1, 319, 8193, 8194, 65535};
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++) {
        struct bio_tlv t = {0}; incoming[1] = invalid[i] >> 8; incoming[2] = invalid[i];
        input_n = 3; input_pos = 0; mode = 0;
        assert(bio_tlv_read(&t, 2, payload, sizeof payload) == -1 && errno == EPROTO);
    }
    for (int phase = 0; phase < 6; phase++) for (size_t room = 0; room < 10241; room++) {
        for (size_t odd = 0; odd <= 1; odd++) {
            size_t old_room = room * 6 / 5 * 2;
            size_t bytes = bio_read_room(old_room, odd, room, 1, 5, 6, phase);
            if (bytes) assert(bio_outputs((bytes + odd) / 2, 5, 6, phase) <= room);
            assert(bytes <= old_room);
        }
    }
    uint8_t external[BIO_PCM_CAP]; struct bio_queue fifo={.external=external,.external_cap=sizeof external};
    assert(bio_capacity(&fifo)==BIO_PCM_CAP && bio_space(&fifo)==BIO_PCM_CAP);
    assert(!bio_append(&fifo,data,8192));assert(!bio_append(&fifo,data,8192));
    assert(!memcmp(external,data,8192)&&!memcmp(external+8192,data,8192));
    fifo.head=8192;fifo.len=8192;
    for(int i=0;i<30;i++)assert(!bio_append(&fifo,data,8192));
    assert(fifo.len==31*8192 && !bio_append(&fifo,data,8192));
    assert(fifo.head==0 && fifo.len==BIO_PCM_CAP && bio_append(&fifo,data,1)==-1);
    for(size_t i=0;i<BIO_PCM_CAP;i++)assert(external[i]==data[i%8192]);
    long long last=bio_now_ms()-BIO_STALL_MS-1;
    assert(bio_progress_timeout(1,last,-1)==-2&&errno==ETIMEDOUT);
    last=bio_now_ms();assert(bio_progress_timeout(1,last,-1)>0);
    memset(&q,0,sizeof q);captured_n=0;fragment=323;mode=0;
    assert(!bio_put(&q,data,323));last=0;assert(bio_flush_progress(&q,2,&last)==1&&last>0);
    unsetenv("SLMBRIDGE_STREAM_IO"); assert(!bio_enabled());
    setenv("SLMBRIDGE_STREAM_IO", "0", 1); assert(!bio_enabled());
    setenv("SLMBRIDGE_STREAM_IO", "1", 1); assert(bio_enabled() == 1);
    setenv("SLMBRIDGE_STREAM_IO", "", 1); assert(bio_enabled() == -1);
    setenv("SLMBRIDGE_STREAM_IO", "01", 1); assert(bio_enabled() == -1);
    int16_t samples[160] = {0}; memset(&q, 0, sizeof q); assert(!bio_frame(&q, samples));
    assert(q.len == 323 && q.data[0] == 0x10 && q.data[1] == 1 && q.data[2] == 64);
    bio_report("unit", "expected fixture", 0, &q, &q, 0);
    puts("PASS exact write suffixes 1..323; bounded pending queue; EINTR/EAGAIN/zero/fatal; TLV fragments 1..8195, every truncated EOF; all downsampler phase/ring/odd bounds; strict flag");
    return 0;
}

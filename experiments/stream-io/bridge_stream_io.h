#ifndef BRIDGE_STREAM_IO_H
#define BRIDGE_STREAM_IO_H
/* Bounded socket transport only; no sample generation or signal processing. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <poll.h>
#include <time.h>

#define BIO_CAP 16384
#define BIO_PCM_CAP (256 * 1024)
#define BIO_STALL_MS 2000
struct bio_queue {
    uint8_t data[BIO_CAP];
    uint8_t *external;
    size_t external_cap;
    size_t head, len;
    unsigned long bytes, short_writes, waits, interrupts;
};
struct bio_tlv {
    uint8_t hdr[3];
    size_t header_used, payload_used;
    unsigned len;
};
static int bio_enabled(void)
{
    const char *s = getenv("SLMBRIDGE_STREAM_IO");
    if (!s || !strcmp(s, "0")) return 0;
    if (!strcmp(s, "1")) return 1;
    fprintf(stderr, "bridge_stream_io: INSTRUMENT ERROR invalid SLMBRIDGE_STREAM_IO (expected 0 or 1)\n");
    errno = EINVAL;
    return -1;
}
static long long bio_now_ms(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return -1;
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static uint8_t *bio_data(struct bio_queue *q) { return q->external ? q->external : q->data; }
static size_t bio_capacity(const struct bio_queue *q) { return q->external ? q->external_cap : sizeof q->data; }
static size_t bio_space(const struct bio_queue *q) { return bio_capacity(q) - q->len; }
/* PCM input may append complete converted frames; output frames and broker
 * batches still use bio_put's single-batch contract. No eviction or overwrite. */
static int bio_append(struct bio_queue *q, const void *data, size_t len)
{
    if (len > bio_space(q)) { errno = ENOBUFS; return -1; }
    if (q->head + q->len + len > bio_capacity(q)) {
        memmove(bio_data(q), bio_data(q) + q->head, q->len); q->head = 0;
    }
    memcpy(bio_data(q) + q->head + q->len, data, len); q->len += len;
    return 0;
}
/* Direction-aware helper watchdog. The caller records actual stream progress;
 * appending bytes or generating timer filler alone cannot extend a stall. */
static int bio_progress_timeout(int pending, long long progress_ms, int wait_ms)
{
    if (!pending) return wait_ms;
    long long now = bio_now_ms();
    if (now < 0) return -2;
    long long left = BIO_STALL_MS - (now - progress_ms);
    if (left <= 0) { errno = ETIMEDOUT; return -2; }
    return wait_ms < 0 || left < wait_ms ? (int)left : wait_ms;
}
static int bio_put(struct bio_queue *q, const void *data, size_t len)
{
    if (q->len || len > bio_capacity(q)) { errno = ENOBUFS; return -1; }
    memcpy(bio_data(q), data, len);
    q->head = 0; q->len = len;
    return 0;
}
/* 1 drained, 0 pending, -1 fatal. EINTR returns to poll instead of spinning.
 * MSG_DONTWAIT preserves descriptor flags; SIGPIPE cannot kill the process. */
static int bio_flush(struct bio_queue *q, int fd)
{
    while (q->len) {
        ssize_t n = send(fd, bio_data(q) + q->head, q->len, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n > 0) {
            if ((size_t)n > q->len) { errno = EIO; return -1; }
            if ((size_t)n < q->len) q->short_writes++;
            q->head += (size_t)n; q->len -= (size_t)n; q->bytes += (unsigned long)n;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            q->waits++; return 0;
        } else if (n < 0 && errno == EINTR) {
            q->interrupts++; return 0;
        } else {
            if (!n) errno = EIO;
            return -1;
        }
    }
    q->head = 0;
    return 1;
}
static int bio_flush_progress(struct bio_queue *q, int fd, long long *progress_ms)
{
    unsigned long before = q->bytes;
    int result = bio_flush(q, fd);
    if (progress_ms && q->bytes != before) {
        *progress_ms = bio_now_ms();
        if (*progress_ms < 0) return -1;
    }
    return result;
}
/* Exactly one TLV per invocation. 1 complete, 0 pending, -1 error, -2 clean
 * boundary EOF. Never reads into the following TLV or blocks on fragments. */
static int bio_tlv_read(struct bio_tlv *t, int fd, uint8_t *payload, size_t cap)
{
    for (;;) {
        void *dst;
        size_t left;
        if (t->header_used < 3) {
            dst = t->hdr + t->header_used; left = 3 - t->header_used;
        } else {
            if (t->payload_used == t->len) return 1;
            dst = payload + t->payload_used; left = t->len - t->payload_used;
        }
        ssize_t n = recv(fd, dst, left, MSG_DONTWAIT);
        if (n > 0) {
            if ((size_t)n > left) { errno = EIO; return -1; }
            if (t->header_used < 3) {
                t->header_used += (size_t)n;
                if (t->header_used == 3) {
                    t->len = ((unsigned)t->hdr[1] << 8) | t->hdr[2];
                    if (t->len > cap || (t->hdr[0] == 0x10 &&
                        (!t->len || (t->len & 1) || t->len > 8192))) {
                        errno = EPROTO; return -1;
                    }
                }
            } else t->payload_used += (size_t)n;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            return 0;
        } else {
            if (!n) {
                if (!t->header_used) return -2;
                errno = EPROTO;
            }
            return -1;
        }
    }
}
static int bio_frame(struct bio_queue *q, const int16_t *samples)
{
    uint8_t frame[323] = {0x10, 1, 64};
    memcpy(frame + 3, samples, 320);
    return bio_put(q, frame, sizeof frame);
}
/* Exact output count for the existing rational resampler, without advancing
 * its state. Its phase recurrence is phase += M per output, -= L per input. */
static size_t bio_outputs(size_t inputs, int L, int M, int phase)
{
    long count = (long)inputs * L - phase;
    return count > 0 ? (size_t)((count + M - 1) / M) : 0;
}
static size_t bio_read_room(size_t old_room, size_t rawlen, size_t out_room,
                           int resampling, int L, int M, int phase)
{
    size_t inputs = resampling ? (out_room * (size_t)M + (size_t)phase) / (size_t)L : out_room;
    size_t safe = inputs * 2;
    if (safe <= rawlen) return 0;
    safe -= rawlen;
    return old_room < safe ? old_room : safe;
}
static void bio_report(const char *role, const char *why, int error,
                       const struct bio_queue *a, const struct bio_queue *b,
                       size_t partial)
{
    fprintf(stderr, "bridge_stream_io: role=%s enabled=1 error=%d reason=%s "
        "to_first_pending=%zu to_second_pending=%zu partial_input=%zu "
        "to_first_bytes=%lu to_second_bytes=%lu short_writes=%lu waits=%lu interrupts=%lu\n",
        role, error, why, a->len, b->len, partial, a->bytes, b->bytes,
        a->short_writes + b->short_writes, a->waits + b->waits,
        a->interrupts + b->interrupts);
}
#endif

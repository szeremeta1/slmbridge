#!/usr/bin/env python3
"""Exact-source experimental transform; original off path is retained verbatim."""
import hashlib
from pathlib import Path

SOURCE_SHA256 = '9ddc2eae070d5082b59d67d7f955fd8979485e03f775ca9cdee2d16da3af6308'
def once(s, a, b):
    if s.count(a) != 1:
        raise ValueError(f'expected one anchor ({s.count(a)}): {a[:100]!r}')
    return s.replace(a, b)

def helper(s):
    h = s[s.index('static int helper_main('):s.index('/* ---------------------------------------------------------------- broker --')]
    h = once(h, 'helper_main(', 'helper_stream_main(')
    h = once(h, '    signal(SIGPIPE, SIG_IGN);', '''    static uint8_t pcm_fifo[BIO_PCM_CAP];
    struct bio_queue to_as = {0}, to_pcm = {.external=pcm_fifo, .external_cap=sizeof pcm_fifo};
    struct bio_tlv tlv = {0};
    int io_error = 0, tx_due = 0;
    long long progress_ms = bio_now_ms();
    if (progress_ms < 0) return 1;
    signal(SIGPIPE, SIG_IGN);''')
    h = once(h, '        int got_audio = 0;', '''        /* A blocked write acts as backpressure, never as another audio tick.
         * A fragmented TLV likewise cannot manufacture a fallback frame. */
        if (to_as.len || tlv.header_used || (rx_clock && to_pcm.len)) wait_ms = -1;
        int bounded_wait = bio_progress_timeout(to_as.len || to_pcm.len || tlv.header_used, progress_ms, (int)wait_ms);
        if (bounded_wait == -2) { io_error = errno; why = "stalled helper output"; break; }
        wait_ms = bounded_wait;
        int got_audio = 0;''')
    h = once(h, '''        pfd[0].fd = as_fd;  pfd[0].events = POLLIN;
        pfd[1].fd = pcm_fd; pfd[1].events = POLLIN;''', '''        size_t out_room_poll = ring_cap - ring_len;
        size_t legacy_room = (resampling ? out_room_poll * 6 / 5 : out_room_poll) * 2;
        if (legacy_room > sizeof rawbuf - rawlen) legacy_room = sizeof rawbuf - rawlen;
        size_t pcm_room = bio_read_room(legacy_room, rawlen, out_room_poll,
                                       resampling, down.L, down.M, down.phase);
        pfd[0].fd = as_fd;
        size_t input_reserve = resampling ? bio_outputs(4096, up.L, up.M, 0) * 2 : 8192;
        pfd[0].events = ((bio_space(&to_pcm) >= input_reserve && !to_as.len && !tx_due) ? POLLIN : 0)
                        | (to_as.len ? POLLOUT : 0);
        pfd[1].fd = pcm_fd;
        pfd[1].events = (pcm_room ? POLLIN : 0) | (to_pcm.len ? POLLOUT : 0);''')
    h = once(h, '''        if (pr > 0) {
            if (pfd[0].revents & (POLLERR | POLLHUP)) { why = "POLLERR/POLLHUP on broker socket"; break; }
            if (pfd[1].revents & (POLLERR | POLLHUP)) { why = "POLLERR/POLLHUP on slmodemd socketpair"; break; }''', '''        if (pr < 0) {
            if (errno == EINTR) continue;
            io_error = errno; why = "poll failed"; break;
        }
        if (pr > 0) {
            if (pfd[0].revents & POLLOUT) {
                if (bio_flush_progress(&to_as, as_fd, (rx_clock || !to_pcm.len) ? &progress_ms : NULL) < 0) { io_error = errno; why = "broker write failed"; break; }
                if (!to_as.len) {
                    tx++;
                    if (!rx_clock) {
                        ts_add_ns(&next, 20L * 1000000L);
                        clock_gettime(CLOCK_MONOTONIC, &now);
                        if (now.tv_sec > next.tv_sec ||
                            (now.tv_sec == next.tv_sec && now.tv_nsec > next.tv_nsec)) {
                            next = now; ts_add_ns(&next, 20L * 1000000L);
                        }
                    }
                }
            }
            if (pfd[1].revents & POLLOUT) {
                if (bio_flush_progress(&to_pcm, pcm_fd, &progress_ms) < 0) { io_error = errno; why = "PCM write failed"; break; }
            }
            if ((pfd[0].revents & (POLLERR | POLLHUP | POLLNVAL)) && !(pfd[0].revents & POLLIN)) {
                io_error = EPIPE; why = "broker socket closed"; break;
            }
            if ((pfd[1].revents & (POLLERR | POLLHUP | POLLNVAL)) && !(pfd[1].revents & POLLIN)) {
                io_error = EPIPE; why = "PCM socket closed"; break;
            }''')
    h = once(h, '''                uint8_t hdr[3];
                if (read_exact(as_fd, hdr, 3) < 0) { why = "EOF reading TLV header from broker"; break; }
                uint16_t len = (uint16_t)((hdr[1] << 8) | hdr[2]);
                if (len && read_exact(as_fd, payload, len) < 0) { why = "EOF reading TLV payload"; break; }
                if (hdr[0] == AS_AUDIO) {''', '''                size_t before_input = tlv.header_used + tlv.payload_used;
                int complete = bio_tlv_read(&tlv, as_fd, payload, sizeof payload);
                if (tlv.header_used + tlv.payload_used != before_input) progress_ms = bio_now_ms();
                if (complete < 0) {
                    io_error = complete == -2 ? 0 : errno;
                    why = complete == -2 ? "broker EOF" : "broker TLV input failed"; break;
                }
                uint8_t type = tlv.hdr[0];
                unsigned len = tlv.len;
                if (complete == 1 && type == AS_AUDIO) {
''')
    h = once(h, '                        ssize_t w = write(pcm_fd, payload, len); (void)w;', '''                        if (bio_append(&to_pcm, payload, len) < 0 || bio_flush_progress(&to_pcm, pcm_fd, &progress_ms) < 0) {
                            io_error = errno; why = "PCM write failed"; break;
                        }''')
    h = once(h, '''                        int n = rs_process(&up, insamp, nin, upbuf,
                                           (int)(sizeof upbuf / 2));
                        ssize_t w = write(pcm_fd, upbuf, (size_t)n * 2); (void)w;''', '''                        if (bio_outputs((size_t)nin, up.L, up.M, up.phase) > sizeof upbuf / 2) {
                            io_error = EOVERFLOW; why = "upsampler output capacity"; break;
                        }
                        int n = rs_process(&up, insamp, nin, upbuf,
                                           (int)(sizeof upbuf / 2));
                        if (bio_append(&to_pcm, upbuf, (size_t)n * 2) < 0 || bio_flush_progress(&to_pcm, pcm_fd, &progress_ms) < 0) {
                            io_error = errno; why = "PCM write failed"; break;
                        }''')
    h = once(h, '                    got_audio = 1;', '                    got_audio = 1;\n                    if (rx_clock) tx_due = 1;')
    h = once(h, '''                } else if (hdr[0] == AS_HANGUP || hdr[0] == AS_ERROR) {
                    why = "AudioSocket HANGUP/ERROR"; break;
                }
            }''', '''                } else if (complete == 1 && (type == AS_HANGUP || type == AS_ERROR)) {
                    memset(&tlv, 0, sizeof tlv);
                    why = "AudioSocket HANGUP/ERROR"; break;
                }
                if (complete == 1) memset(&tlv, 0, sizeof tlv);
            }''')
    h = once(h, '                if (out_room > 0 && room > 0) {', '''                room = bio_read_room(room, rawlen, out_room, resampling,
                                     down.L, down.M, down.phase);
                if (out_room > 0 && room > 0) {''')
    h = once(h, '                    ssize_t r = read(pcm_fd, rawbuf + rawlen, room);', '''                    ssize_t r = recv(pcm_fd, rawbuf + rawlen, room, MSG_DONTWAIT);
                    if (r > 0) progress_ms = bio_now_ms();
                    if (r == 0) { io_error = rawlen ? EPROTO : 0; why = "PCM EOF"; break; }
                    if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                        io_error = errno; why = "PCM read failed"; break;
                    }''')
    h = once(h, '                            if ((size_t)n > out_room) n = (int)out_room;', '''                            if ((size_t)n > out_room) {
                                io_error = EOVERFLOW; why = "downsampler output capacity"; break;
                            }''')
    h = once(h, '''                } else {
                    /* Should not happen once the elastic loop is steering, and
                       is counted so that it cannot happen silently. */
                    uint8_t waste[4096];
                    ssize_t r = read(pcm_fd, waste, sizeof waste);
                    if (r > 0) overrun++;
                }''', '''                } /* No room: omit POLLIN next time. Never drain/discard PCM. */''')
    h = once(h, '''            if (got_audio || pr == 0) {''', '''            (void)got_audio;
            if (!to_as.len && (tx_due || (!to_pcm.len && pr == 0 && !tlv.header_used))) {''')
    h = once(h, '''                if (emit_frame(as_fd, rout) < 0) { why = "short write to broker"; break; }
                tx++;''', '''                tx_due = 0;
                if (bio_frame(&to_as, rout) < 0 || bio_flush_progress(&to_as, as_fd, (rx_clock || !to_pcm.len) ? &progress_ms : NULL) < 0) {
                    io_error = errno; why = "broker frame write failed"; break;
                }
                if (!to_as.len) tx++;
            else continue; /* The original deadline advances after completion. */''')
    h = once(h, '''        if (now.tv_sec > next.tv_sec ||
            (now.tv_sec == next.tv_sec && now.tv_nsec >= next.tv_nsec)) {''', '''        if (!to_as.len && !tlv.header_used &&
            (now.tv_sec > next.tv_sec ||
             (now.tv_sec == next.tv_sec && now.tv_nsec >= next.tv_nsec))) {''')
    h = once(h, '''            if (write(as_fd, msg, sizeof msg) != (ssize_t)sizeof msg) { why = "short write to broker"; break; }
            tx++;''', '''            if (bio_put(&to_as, msg, sizeof msg) < 0 || bio_flush_progress(&to_as, as_fd, (rx_clock || !to_pcm.len) ? &progress_ms : NULL) < 0) {
                io_error = errno; why = "broker frame write failed"; break;
            }
            if (!to_as.len) tx++;
            else continue; /* The original deadline advances after completion. */''')
    h = once(h, '    return 0;\n}', '''    if (!io_error && (to_as.len || to_pcm.len || rawlen)) io_error = EPIPE;
    bio_report("helper", why, io_error, &to_as, &to_pcm, tlv.header_used + tlv.payload_used + rawlen);
    return io_error ? 1 : 0;
}''')
    return h

def broker(s):
    h = s[s.index('static int relay_and_watch('):s.index('static int broker_main(')]
    h = once(h, 'relay_and_watch(', 'relay_stream_and_watch(')
    h = once(h, '    uint8_t buf[8192];', '''    struct bio_queue q[2] = {{0}, {0}};
    int closed[2] = {0, 0};
    int io_error = 0;
    const char *why = "relay ended";
    uint8_t buf[8192];''')
    h = once(h, '''        p[0].fd = a; p[0].events = POLLIN; n++;
        p[1].fd = b; p[1].events = POLLIN; n++;''', '''        p[0].fd = a; p[1].fd = b;
        for (int i = 0; i < 2; i++) {
            p[i].events = (!closed[i] && !q[1-i].len ? POLLIN : 0)
                          | (q[i].len ? POLLOUT : 0);
            /* A half-closed fd with no requested work must not spin on HUP. */
            if (!p[i].events) p[i].fd = -1;
        }
        n = 2;''')
    h = once(h, '        if (poll(p, nfds, 30000) <= 0) return -1;', '''        int ready = poll(p, nfds, 30000);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) { io_error = ready == 0 ? ETIMEDOUT : errno; why = "relay poll failed"; break; }''')
    start = h.index('        for (int i = 0; i < 2; i++) {', h.index('        if (tfd >= 0'))
    end = h.index('\n    }\n}', start)
    h = h[:start] + '''        for (int i = 0; i < 2; i++) {
            int fd = i ? b : a;
            if (p[i].revents & POLLOUT) {
                if (bio_flush(&q[i], fd) < 0) { io_error = errno; why = "relay write failed"; goto stream_done; }
            }
            if ((p[i].revents & (POLLERR | POLLNVAL)) && !(p[i].revents & POLLIN)) {
                io_error = EIO; why = "relay socket error"; goto stream_done;
            }
            if (!closed[i] && !q[1-i].len && (p[i].revents & (POLLIN | POLLHUP))) {
                ssize_t r = recv(fd, buf, sizeof buf, MSG_DONTWAIT);
                if (!r) {
                    closed[i] = 1;
                    /* EOF follows all accepted bytes from this source; they
                     * have already drained because POLLIN required no queue. */
                    if (shutdown(i ? a : b, SHUT_WR) < 0 && errno != ENOTCONN) {
                        io_error = errno; why = "relay half close failed"; goto stream_done;
                    }
                } else if (r > 0) {
                    if (bio_put(&q[1-i], buf, (size_t)r) < 0 ||
                        bio_flush(&q[1-i], i ? a : b) < 0) {
                        io_error = errno; why = "relay write failed"; goto stream_done;
                    }
                } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                    io_error = errno; why = "relay read failed"; goto stream_done;
                }
            }
        }
        if (closed[0] && closed[1]) { why = "relay EOF"; break; }''' + h[end:]
    h = once(h, '\n    }\n}', '''
    }
stream_done:
    if (!io_error && (q[0].len || q[1].len)) io_error = EPIPE;
    bio_report("broker", why, io_error, &q[0], &q[1], 0);
    return io_error ? -1 : 0;
}''')
    return h

def patch(s):
    if '/* BEGIN bounded socket transport */' in s: raise ValueError("already patched")
    h = helper(s)
    b = broker(s)
    header = Path(__file__).with_name('bridge_stream_io.h').read_text()
    s = once(s, '#define AS_HANGUP', '/* BEGIN bounded socket transport */\n' + header + '\n/* END bounded socket transport */\n\n#define AS_HANGUP')
    s = once(s, 'static int helper_main(int pcm_fd, int as_fd)\n{', h + '''static int helper_main(int pcm_fd, int as_fd)
{
    int stream_io = bio_enabled();
    if (stream_io < 0) return 1;
    if (stream_io) return helper_stream_main(pcm_fd, as_fd);''')
    s = once(s, 'static int relay_and_watch(int a, int b, int *tfdp)\n{', b + '''static int relay_and_watch(int a, int b, int *tfdp)
{
    int stream_io = bio_enabled();
    if (stream_io < 0) return -1;
    if (stream_io) return relay_stream_and_watch(a, b, tfdp);''')
    s = once(s, 'int main(int argc, char **argv)\n{', 'int main(int argc, char **argv)\n{\n    if (bio_enabled() < 0) return 1;')
    return s

if __name__ == '__main__':
    import argparse
    p = argparse.ArgumentParser()
    p.add_argument('source', type=Path)
    p.add_argument('output', type=Path)
    a = p.parse_args()
    data = a.source.read_bytes()
    if hashlib.sha256(data).hexdigest() != SOURCE_SHA256:
        raise SystemExit('source fingerprint differs from pinned public main')
    if a.output.exists(): raise SystemExit('output already exists')
    a.output.write_text(patch(data.decode()))

#!/usr/bin/env python3
"""Apply a default-off local-reference experiment to exact reviewed transport bytes."""
import argparse
import hashlib
from pathlib import Path

SOURCE_SHA256 = 'd354ab6a4ef5dd9a5685a80682ee05cfdb989274b2c0a6f07b877d311939597b'


def once(source, old, new):
    if source.count(old) != 1:
        raise ValueError('exact source anchor differs')
    return source.replace(old, new)


def transform(data):
    if hashlib.sha256(data).hexdigest() != SOURCE_SHA256:
        raise ValueError('reviewed transport source hash differs')
    source = data.decode()
    start = source.index('static int helper_stream_main(')
    end = source.index('static int helper_main(', start)
    helper = source[start:end]
    helper = once(helper, '    static uint8_t pcm_fifo[BIO_PCM_CAP];',
                  '    struct local_echo echo = {.mode = le_config(getenv("SLMBRIDGE_LOCAL_ECHO"))};\n'
                  '    static uint8_t pcm_fifo[BIO_PCM_CAP];')
    helper = once(helper, '                if (complete == 1 && type == AS_AUDIO) {',
                  '                if (complete == 1 && type == AS_AUDIO) {\n'
                  '                    (void)le_receive(&echo, payload, len);')
    helper = once(helper, '                ring_pull_frame(ring, &ring_len, rout, &starved);',
                  '                ring_pull_frame(ring, &ring_len, rout, &starved);\n'
                  '                (void)le_stage(&echo, rout);')
    helper = once(helper, '                    tx++;',
                  '                    tx++;\n                    (void)le_commit(&echo);')
    # Immediate RX-clock flush commits here. The timer path is unchanged and
    # cannot run when the reference is enabled: main enforces RX clocking.
    helper = once(helper, '                if (!to_as.len) tx++;',
                  '                if (!to_as.len) { tx++; (void)le_commit(&echo); }')
    helper = once(helper, '    bio_report("helper", why, io_error, &to_as, &to_pcm,',
                  '    le_report(&echo);\n    bio_report("helper", why, io_error, &to_as, &to_pcm,')
    source = source[:start] + helper + source[end:]
    source = once(source, '/* BEGIN bounded socket transport */',
                  Path(__file__).with_name('local_echo.h').read_text() +
                  '\n/* BEGIN bounded socket transport */')
    clock_read = 'bridge_env("TX_CLOCK")'
    source = once(source, '    if (bio_enabled() < 0) return 1;', f'''    if (bio_enabled() < 0) return 1;
    int local_echo_mode = le_config(getenv("SLMBRIDGE_LOCAL_ECHO"));
    const char *local_echo_clock = {clock_read};
    if (local_echo_mode < 0 || (local_echo_mode &&
        (bio_enabled() != 1 || !local_echo_clock || strcmp(local_echo_clock, "rx")))) {{
        fprintf(stderr, "bridge_local_echo: INSTRUMENT ERROR requires off or observe/mix with STREAM_IO=1 and TX_CLOCK=rx\\n");
        return 1;
    }}''')
    # A failed reference epoch is recorded permanently. It never changes the
    # bridge's existing fallback policy, blocks a frame, or silently restarts.
    return source


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    if a.source.is_symlink() or not a.source.is_file():
        raise SystemExit('regular input file required')
    original = a.source.read_bytes()
    candidate = transform(original)
    if a.source.read_bytes() != original:
        raise SystemExit('input changed during transform')
    with a.output.open('x') as f:
        f.write(candidate)
    print(hashlib.sha256(candidate.encode()).hexdigest())


if __name__ == '__main__':
    main()

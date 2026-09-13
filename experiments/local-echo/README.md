# Experimental local echo reference

This default-off experiment adds a fixed -40 dB copy of the previous fully
accepted 8 kHz output frame to incoming signed-linear samples before RX
upsampling. The reference is selected after TX resampling, ring removal and
underrun fill. Pending or partially written output is never eligible.

It requires the [bounded stream transport](../stream-io/README.md), exact
`SLMBRIDGE_STREAM_IO=1` and `SLMBRIDGE_TX_CLOCK=rx`. Set
`SLMBRIDGE_LOCAL_ECHO=observe` to check the reference lifecycle without changing
samples, or `mix` to add it. Unset or `off` preserves the original audio path.
Invalid values and incompatible clock/transport settings refuse before I/O.

The first input frame is unchanged warmup. Missing, duplicate or extra output
commits, non-320-byte input and fallback output permanently invalidate the
reference epoch. The bridge keeps its original transport/fallback behavior and
passes subsequent RX unchanged. It does not silently restart the experiment.
An invalid epoch must be retained as instrument uncertainty, never relabeled a
valid no-echo arm. Exit records report mode, validity, reason, received/committed
frames, warmup, mixing, clipping and pending state; counters freeze on invalidation.
Separate transport counters still describe whole-call traffic.

This prototype changes no noise source, modem watchdog, DSP echo canceller,
I/O delay or normal build. One frame is a sample-domain relationship, not proof
of a physical 20 ms echo or a correct modem delay setting. There are no physical
modem results or speed/reliability claims for this public candidate.

## Reproduce

First build the pinned stream experiment into a new private output directory.
Then, from the repository root:

```sh
python3 experiments/local-echo/validate.py \
  --source /private/stream-build/bridge.c \
  --output /private/new-local-echo-validation
```

The transformer accepts only source SHA256
`d354ab6a4ef5dd9a5685a80682ee05cfdb989274b2c0a6f07b877d311939597b`
and produces exact source SHA256
`1fb71c7cfd19114f71eee87ce0455b5d8cf9445508f4bc83af31918f963978a0`.
It never overwrites existing output. The validator seals inputs before/after,
checks untouched legacy helper/broker sections, compiles ASAN/UBSAN fixtures,
and runs generated-byte local AF_UNIX tests and guaranteed pre-I/O refusals.
No listener, serial port, modem or service is opened. Keep test binaries and
private manifests outside the repository; the normal Makefile stays unchanged.

Fixtures cover all signed reference values, rounding, saturation, unaligned
PCM, warmup, stale/future/partial references, 14 actual helper cases across
8/9.6 kHz and observe/mix, short/EINTR/EAGAIN writes, prefill and fallback
invalidation. Five real-main configuration errors must refuse. The legacy
transport suite runs on Linux and is explicitly skipped on macOS. This normalized standalone extraction passed on macOS arm64 (Apple clang 21)
and Linux x86-64 (GCC 13.3): reference sanitizer checks, all 14 helper cases and
five pre-I/O refusals. Linux also passed the full legacy transport suite; macOS
explicitly skipped it. Both input seals remained unchanged. The initial macOS
compile exposed a fixture-helper dependency missing from the standalone tree;
public-prefix fixture defaults were added locally to this test before the fresh
passing runs. No canonical bridge source changed.

Linux sanitizer executable SHA256:
`8f8573f0e24cc7732686c00bc86e64dacdc132c367398e1b664a10fc76d7369c`.
macOS sanitizer executable SHA256:
`6414109ca2a8ed8fa685cb778d9d627816734cb977e03786eb44df7312707abf`.
These test artifacts are not optimized service binaries or physical evidence.

A live trial still needs an exact optimized binary, settings verified on both
helper and broker, complete call-attributed reference/transport lifecycle,
interleaved arms, fresh bidirectional payloads and independent restoration.

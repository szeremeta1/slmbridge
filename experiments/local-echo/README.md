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

## Related physical observation, September 13

A separate narrow-profile integration of the same reference helper completed
two fixed observe-only hardware slots, with no echo mixed. A US Robotics modem
passed 393,216 fresh random bytes each way, DNS, certificate-checked HTTPS and
a 241-second PPP session. Its reference epoch covered 13,029 received/committed
frames. A StarTech modem failed to establish carrier, but its 1,600-frame
reference epoch remained valid. Both epochs had one warmup, no mixed/clipped
frames, no pending output and complete attributed transport delivery.

This is reference feasibility in that integration, not physical validation of
this public-profile build, a successful StarTech connection or evidence that
injected echo improves speed/reliability. The public-profile source and binary
hashes below retain their separate offline validation status. A subsequent
interleaved observe/mix comparison is needed before any treatment claim.

Two failed setup attempts exposed deployment checks that matter here. First,
root could hash a binary inside private build directories while the unprivileged
helper could not execute through them. Copy only the reviewed executable into
an appropriate serving directory and test execution as its actual identity.
Second, a launcher exported RX clock settings only after starting slmodemd;
its broker received them, but the later helper inherited slmodemd's earlier
environment. A union of process environments hid the difference. Supply and
verify the settings separately in both roles before a call. A broker showing
RX clocking does not prove that its separately exec-ed audio helper uses it.

## Reference evidence

`journal.py` checks one complete local-reference record against an independently
verified enabled-transport proof for the same journal. It binds the helper PID,
boot, executable, unit, stream and lifetime, then checks source order and counter
invariants. Invalid epochs retain their frozen counters and reason. Partial
transport delivery and extra whole-call output remain separate failures to
qualify. Fragmented reference records refuse; no continuation is guessed.
Actual executable hashes, process generations and mode readback remain the
controller's responsibility. A journal pathname alone is insufficient.
A final pending output preserves validity of the prior committed reference
already used for RX, while delivery and treatment qualification remain false.
Exit counters alone cannot locate that pending frame relative to intentional
teardown or the native call exposure.

Run `python3 experiments/local-echo/journal-test.py` for eleven offline test
methods, including real C-generated observe, mix, invalid and partial-reference
records. Attribution, ordering, overflow and transport faults are also checked.
A valid reference is not proof of complete audio delivery, physical echo delay
or a reliable call. This analyzer changes no generated bridge source or binary
and starts no physical experiment.

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

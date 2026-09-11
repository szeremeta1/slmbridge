# Public-source validation — 2026-09-10

All results below use the public source and generated-byte socket fixtures in
this experiment. They do not measure physical modem negotiation, held calls,
PPP payload throughput or a dropped-call rate. No service was changed.

## Identity

| Item | Value |
|---|---|
| Public base commit | `309d3a9809179d5d331d5adbd7dea7275200ed80` |
| Base `slmbridge.c` SHA-256 | `9ddc2eae070d5082b59d67d7f955fd8979485e03f775ca9cdee2d16da3af6308` |
| Generated candidate C SHA-256 | `d354ab6a4ef5dd9a5685a80682ee05cfdb989274b2c0a6f07b877d311939597b` |
| Candidate executable SHA-256 | `f116ecbb38ae89302031c60505f6e2c6e20ce9b5f84093bdc85ebb05d5b63b8c` |
| Toolchain | GCC 13.3.0, Ubuntu package 13.3.0-6ubuntu2~24.04.1, Linux x86-64 |
| Candidate compile flags | `-O2 -pthread -lm` |

The normalized public-only transformation generates the exact reviewed C, and
its executable matches the separately tested public candidate byte-for-byte.
The original off-path helper/broker bodies and resampler function are asserted
unchanged apart from dispatch. This identifies the tested build; it does not
identify or validate any deployed executable.

## Exact stream and failure tests

The component suite passed every write fragment from 1 to 323 bytes, including
EINTR, EAGAIN, zero progress, fatal errors and exact suffix resumption. It
checks fixed FIFO append/compaction/capacity, overflow refusal, watchdog bounds,
every TLV fragment from 1 to 8,195 bytes, every truncated EOF and invalid AUDIO
lengths. Phase/ring/odd-byte capacity combinations are checked, and actual
resampler output counts match the derived bound across all six phases, block
lengths 0–48 and boundary lengths 4,095/4,096 with canaries intact.

Actual helper-function tests passed 32 complete input/output frames per case
at 8 and 9.6 kHz, with original off and enabled paths, default/legacy/narrow
filter profiles, one-byte TLV fragments, seven-byte writes and injected
EINTR/EAGAIN. The expected bytes use the same existing filter implementation.
An actual broker-function fixture transfers 128 KiB in each direction exactly,
through 4 KiB requested socket buffers with short/error injection and duplex
half-close. Separate actual helper fixtures hold each output direction full
for 350 ms: each retains exact bytes and finishes with one received and one
transmitted frame, zero pending bytes, and no added fallback frame.

Actual helper zero-write, EPIPE, odd PCM EOF and odd AUDIO cases fail visibly.
All four builder/source tests pass, including source/transformation drift,
symlink, existing-output and duplicate-patch refusal. The full Linux build
command in the README reruns these checks and the paced/timer cases below.

## Paced cyclic backpressure

The fixture begins with the ring prefilled and both PCM socket directions
full. A synchronous test peer waits for write readiness before reading any
input. Real incoming AUDIO frames arrive every 20 ms, so an unpaced burst
cannot hide a progress-watchdog bug. No synthetic output credits are added.

Requested-small socket buffers accepted 8,128 bytes per direction; default
buffers accepted 180,224. Each exact completion below checked both generated
byte streams and ended with zero error, pending bytes, partial input,
starvation, sample slips and overruns.

| AUDIO payload | DSP rate | Socket buffers | Output frames | Elapsed | Outcome |
|---:|---:|---|---:|---:|---|
| 320 bytes | 8,000 Hz | Requested 4 KiB | 26 | 0.504 s | Exact completion |
| 320 bytes | 9,600 Hz | Requested 4 KiB | 22 | 0.425 s | Exact completion |
| 320 bytes | 8,000 Hz | Default | 436 | 8.709 s | Exact completion |
| 320 bytes | 9,600 Hz | Default | 363 | 7.248 s | Exact completion |
| 8,192 bytes | 8,000 Hz | Requested 4 KiB | 26 | 0.534 s | Exact completion |
| 8,192 bytes | 9,600 Hz | Requested 4 KiB | 22 | 0.453 s | Exact completion |
| 8,192 bytes | 8,000 Hz | Default | 32 | 2.632 s | Explicit timeout; 262,144 PCM bytes pending |
| 8,192 bytes | 9,600 Hz | Default | 26 | 2.513 s | Explicit timeout; 255,592 PCM bytes pending |

The last two cases are a supported containment result, **not delivery
successes**. Both returned status 1 with helper `error=110` (`ETIMEDOUT`) and
retained pending-byte accounting. Additional frames written by the fixture
remained in the kernel; they were not counted as received by the helper.
Repeated maximum-size input TLVs retain the old one-output-frame-per-TLV rule
and are not sample-balanced. No universal full-backlog progress claim follows
from the 256 KiB capacity.

The long ordinary-frame cases explain why the watchdog must observe reverse
PCM/real input progress: there can legitimately be several seconds without an
accepted PCM write while buffered output drains. A timeout on that write
counter alone incorrectly failed an earlier candidate. The tested candidate
uses the progress-aware helper policy documented in the README.

## Timer behavior after an output stall

A test-owned outgoing AudioSocket buffer stays full for 350 ms. After the
first complete frame is accepted, the next-frame gaps were:

| Trial | Original off path | Enabled path |
|---|---:|---:|
| 1 | 20.023 ms | 20.081 ms |
| 2 | 20.240 ms | 20.061 ms |
| 3 | 20.079 ms | 20.074 ms |

An earlier candidate advanced its deadline at enqueue and produced an immediate
second frame after completion. Advancing at complete delivery restores the
existing timer behavior. These measurements are local scheduling evidence,
not a physical line clock or end-to-end latency measurement.

## Limits of this record

No test launches bridge main, a listener, TTY, modem, DSP or live service.
Missing terminal logs after forced termination remain unknown. Terminal zero
pending bytes does not establish an empty FIFO throughout a call. Physical
speed, reliability, retrains and sustained payload still need independent
measurement before any promotion. Raw manifests remain outside this branch;
this file preserves the inspectable, normalized results and exact build IDs.

## Reproduction hardening checkpoint

A fresh isolated Linux run after the builder/fixture hardening passes six
builder tests, the complete component and integration suites, eight paced
cycles and three timer comparisons. All nine supplied build/fixture files and
the public source retain their initial hashes; copied fixtures and both
generated sources also pass their final checks. Every compiler/test command
has a 90-second deadline. No runtime source, transformation or header changed:
the generated C and executable still have the exact hashes in the Identity
table. The earlier four-test record above is retained as historical evidence.

The four ordinary-frame cases complete exactly (26/22 frames with small
buffers; 436/363 with default buffers), in 0.505/0.425 and 8.709/7.248 seconds
respectively for 8/9.6 kHz. The maximum-frame small-buffer cases also complete
exactly. Both maximum-frame/default-buffer cases still fail explicitly with
ETIMEDOUT, after 2.631/2.513 seconds; these remain containment results, not
delivery successes. The three enabled timer gaps are 20.058, 20.072 and
20.076 ms; the corresponding legacy observations are 20.028, 20.017 and
20.019 ms. Public fixture controls are now explicitly fixed, including default
filter and timer settings, rather than being inherited from the caller.

The raw build-manifest SHA-256 is
`a61da87b154c22b1ef92984b59e120ad728c627c8f0563a0e79411428c3bd861`.
It remains a local provenance artifact. This additional run is generated-byte
AF_UNIX evidence only and adds no physical speed or reliability result.

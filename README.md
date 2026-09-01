# slmbridge

A multi-line answering soft-modem bridge for Asterisk over AudioSocket.

> **AI-generated code — use at your own risk.** The entirety of `slmbridge`
> (including `slmbridge.c`, `slmbridge_rstest.c`, and this documentation) was
> written by a generative AI system, not audited line-by-line by a human
> engineer with domain expertise in telephony, DSP, or C systems programming
> beyond the review that produced this disclaimer. It has not undergone a
> professional security audit and has not been tested against adversarial or
> malformed input.
>
> This code interacts with subprocess execution, raw sockets, and file
> descriptors passed across process boundaries — categories of code where
> bugs can have consequences beyond a crash (resource exhaustion, unexpected
> subprocess behavior, mishandling of untrusted network input). It is
> provided strictly "as is," with no warranty of correctness, safety,
> security, or fitness for any purpose. **By using, deploying, modifying, or
> distributing this software, you assume all risk of any flaws, defects,
> vulnerabilities, data loss, service disruption, or other harm that may
> result — direct or indirect.** Do not deploy this in a production or
> security-sensitive environment without independent review by someone
> qualified to evaluate it. See [LICENSE](LICENSE) for the full legal
> disclaimer of warranty and liability.

`slmbridge` lets `slmodemd` — Smart Link's Linux softmodem daemon — answer
incoming calls delivered to
[Asterisk](https://www.asterisk.org/) as
[AudioSocket](https://github.com/CyCoreSystems/audiosocket) streams, and run
several lines of it at once on a single machine.

## The problem

`slmodemd`'s DSP is the only open, working fast-modem implementation around
(V.32, V.32bis, V.34, V.90, V.92). The reference project that reaches it over
SIP, [D-Modem](https://github.com/strozfriedberg/d-modem), pairs it with a
PJSIP process that can only *place* calls — its source has no incoming-call
handler at all. For anything that needs to *answer* the phone, that's fatal.

`slmbridge` replaces that PJSIP process outright instead of patching answer
support into it. Asterisk already answers the call, already owns the SIP
stack, and already hands audio to arbitrary programs over AudioSocket, so
"cannot answer" simply stops being a problem to solve.

It also fixes two problems that sit underneath that one:

- **The 9,600 Hz / 8,000 Hz mismatch.** `slmodemd`'s DSP defaults to a
  9,600 Hz media rate; AudioSocket delivers 8,000 Hz. Left alone, that's a
  resampler sitting directly in the modem's signal path on every frame in
  both directions. `slmbridge` runs a 6:5 polyphase resampler at the boundary
  instead, and the DSP itself can be built natively for 8,000 Hz if you have
  the coefficient tables to support it.

  The self-test asserts flatness across **300–3,900 Hz**, not 300–3,400. That
  detail is the whole point: an earlier version asserted 300–3,400 and passed
  while the filter it was testing was 7 dB down at 3,888 Hz — the top of the
  3,429-baud V.34 constellation. A test whose passband stops below the signal
  band cannot see the defect it exists to catch. If a future rate needs more
  than 3,900 Hz, move that number first and let it fail.

  Three filter profiles are selectable at runtime with `LITENET_RS_PROFILE`:
  the default 96/112-tap Kaiser pair (flat to 3,900), `legacy` (the original
  32-tap Blackman pair, kept as an exact rollback), and `narrow`, which
  deliberately band-limits the *receive* direction to `LITENET_RS_FC`
  (default 3,800 Hz). `narrow` is an experiment, not a recommendation — it
  intentionally fails the transparency check, which is why the self-test
  reports it rather than asserting on it.
- **Clock drift.** `slmodemd` has no clock of its own — it advances at
  whatever rate its device delivers audio. An implementation that reads at
  one clock and writes at an independent one lets the two drift apart, which
  shows up as accumulating latency and eventually dropped/repeated frames.
  `slmbridge` slaves its transmit clock to its receive clock, making the two
  one by construction rather than by periodic correction.

## How it works

`slmbridge` is two small processes, not one:

- **The broker** listens for AudioSocket connections from Asterisk and forks
  a `slmodemd` instance per call.
- **The helper** is what `slmodemd` execs as its `--exec` answer-side helper.
  It inherits the AudioSocket connection from the broker and relays raw PCM
  between `slmodemd`'s socketpair and AudioSocket's 20 ms framing, resampling
  as it goes.

```
Asterisk ──AudioSocket (TCP)──▶ broker ──forks──▶ slmodemd
                                   │                  │
                                   └──inherited fd──▶ helper (this binary)
                                                       │
                                          raw PCM over socketpair
```

Because `slmodemd` hardcodes device 0 internally, running more than one
instance needs each one told apart — `slmbridge` handles the multi-line
bookkeeping so a pool of lines can run concurrently on one host without
fighting over the same device node.

## Building

```sh
make          # builds slmbridge
make check    # builds and runs the resampler self-test
```

Requires a C compiler and `pthread`/`libm`. No other dependencies — this
binary does no DSP of its own and links against nothing GPL or proprietary;
it talks to `slmodemd` as a subprocess over a pipe, not as a library.

You'll separately need a working `slmodemd` build (from Smart Link's
`sl-modem-daemon` sources) and a SIP/Asterisk setup that exposes calls to it
over AudioSocket via `app_audiosocket`/`res_audiosocket`. `slmodemd`'s DSP
component itself ships under a proprietary license from Smart Link and is
not included here — see their own licensing terms before building or
distributing it.

## Known limitation: what it transmits when the ring runs dry

When there is no `slmodemd` output ready to send, `slmbridge` fills the frame
with a **constant** — the last sample value, or zero — and counts it as
`starved` in the exit line. That is deliberate and it is about AudioSocket's
2-second liveness timeout, which a constant satisfies. As a *signal* it is the
worst available filler: a whole frame of DC, all energy at 0 Hz, an amplitude
step at each edge, and nothing for the far end's carrier or timing recovery to
track through.

Measured upstream on a V.34 line, starvation is strongly associated with
handshake failure — 0.174 events/second on calls that failed against 0.059 on
calls that connected, with every failure above 0.083/s (Mann-Whitney
p < 0.00001, n = 72). The absolute counts are small, 1–2 per call, which is the
point: what matters is *where* the frame lands. One DC frame in data mode is
absorbed by error control; the same frame during V.34 Phase 3/4 training breaks
the far end's acquisition.

**For V.32bis this is not a practical problem** and the code is deliberately
left alone: at 2400 baud a lost 20 ms is ridden through and LAPM retransmits,
and lines running that way log zero HDLC frame errors across a day.

Replacing the fill with a repeat of the last good frame is the obvious idea. It
was tried and it is **not** a fix — 13/16 versus 10/16 connects (Fisher
p = 0.433), and the build carrying it showed an unexplained regression to lower
connect speeds. If you are going to attempt this, start from those numbers
rather than from the intuition.

Watch `starved=` in the helper's exit line. If it is more than one or two per
call, the ring is being drained faster than `slmodemd` fills it, and
`LITENET_RX_PREFILL` is the dial — at the cost of path delay, which on a V.34
link is itself expensive.

## Attribution

`slmbridge` doesn't link against or vendor any of the following — it just
talks to them as a subprocess or across a call — but it exists to plug the
gap between them, and none of this works without their prior art:

| Project | Author | License |
|---|---|---|
| `slmodemd` | Smart Link Ltd (Debian `sl-modem-daemon` packaging) | BSD-3-Clause (packaging); DSP itself is Smart Link's proprietary blob |
| [D-Modem](https://github.com/strozfriedberg/d-modem) | Stroz Friedberg / Aon plc | GPL-2.0 |
| [AudioSocket](https://github.com/CyCoreSystems/audiosocket) | Seán C McCord, CyCore Systems | GPL-2.0 |
| Asterisk | Sangoma / Digium and the Asterisk project | GPL-2.0 |

`slmbridge` itself was extracted from [LiteNet Connectivity Solutions'](https://dialup.litenet.tel) dial-up
ISP stack, where it's what lets a real 14.4k-class modem call in and get
answered by software instead of hardware.

## License

MIT — see [LICENSE](LICENSE). This covers `slmbridge`'s own code only; the
third-party components it interoperates with carry their own licenses as
listed above.

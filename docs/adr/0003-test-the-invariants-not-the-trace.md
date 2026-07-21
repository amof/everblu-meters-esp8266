# Test the radio against invariants and payload, not against a recorded trace

The library was rewritten from scratch after `87bdae3`, which is the last commit
known to have actually read a meter. That commit is therefore the only oracle we
have for behaviour on the air — but it is an oracle for *what goes out*, not for
*how it gets there*. The old driver kept the TX FIFO full by watching the free-byte
count carried in the SPI status byte and wrote the master request after a blind
`delay(130)`; the rewrite polls TXBYTES explicitly through `waitTxFifoFree()`
(see ADR-0001). Both uphold the same invariant by different mechanisms, so their
SPI traces differ on nearly every line by design.

A test asserting "the new trace matches the known-good trace" would therefore be
entirely red while the code was entirely correct, and would have to be
regenerated after every legitimate timing change — at which point it asserts
nothing. We assert three things instead:

1. **Payload equivalence.** Filter the trace down to bytes written to the TX
   FIFO and concatenate them. Old and new must be byte-identical: N chunks of
   wake-up preamble followed by the 39-byte encoded master request. This is the
   part of the old code that is genuinely still authoritative.
2. **Invariants over the trace.** The FIFO is loaded before `STX`; TXBYTES never
   reaches zero between `STX` and the last request byte; no write can carry the
   FIFO past 64 bytes; `SFTX`/`SFRX` are issued only from IDLE, underflow or
   overflow; `MDMCFG2`, `PKTCTRL0` and `IOCFG0` are restored whichever step
   fails.
3. **Protocol vectors.** The frames published on the Maison Simon wiki, archived
   under `docs/cyble/`, pin frame construction to ground truth independent of
   either codebase.

## Considered options

**Compiling the old driver as a live second test target** was rejected. It would
need the old `everblu_meters.h`, its `T_BOOL`/`echo_debug`/wiringPi shims and a
duplicate register header dragged back into the tree and kept compiling forever,
in exchange for a byte stream that is fixed by the protocol and cannot drift. The
stream is instead generated from the old code once and checked in as a fixture,
with this ADR recording where it came from.

**Trace equality** is covered above. **Testing only the pure functions in
`utils.cpp`** is necessary but not sufficient: they were almost certainly not
what was broken, and a green suite that never exercises the CC1101 sequence would
be worse than none, because it would be believed.

## Consequences

The tests are to compile the real, unmodified `cc1101.cpp` and
`everblu_cyble.cpp` against fake `Arduino.h`/`SPI.h` primitives, with a stateful
CC1101 model behind them. Nothing is made virtual and no seam is introduced for
the tests' benefit: for a radio driver the correctness *is* the exact register
values and strobe order, so the thing under test must be the thing that gets
flashed.

Only part of that exists. Tier 3 is built, and `cc1101.cpp` is compiled natively
against a chip model that covers the register file, the identity registers and
how GDO0 follows IOCFG0 — enough for the wiring check of ADR-0005, which never
leaves IDLE. Tiers 1 and 2 remain unwritten.

`everblu_cyble.cpp` is in the native build, but only its scheduling logic is
exercised: what `readIfDue()` decides *before* the radio is touched needs a
clock and an EEPROM, not a FIFO model, so it was testable well ahead of the
rest. Those tests are worth having early because the decision they cover is
mostly invisible — its inputs are the day of the week and the hour, so a fault
in it reproduces for a few hours a week and cannot be reproduced on demand at
all. One shipped as exactly that: a tick outside the wakeup window was counted
as a read attempt, so every Sunday logged and republished a refusal once a
minute for thirteen hours.

Completing them means the model must also track FIFO occupancy and the
IDLE→TX→TXFIFO_UNDERFLOW transition. A model that merely answers status reads
plausibly would let the underflow bug in ADR-0001 pass green, which is the one
failure this suite exists to prevent.

The TX-side invariants have been checked verbatim against the datasheet (§15.4
for underflow and SFTX, §20 for overflow corrupting FIFO content, Table 42 for
where the flush strobes are legal), so tier 2 is not merely folklore there. The
RX side has had no such check: the two-stage sync detection, the mid-frame rate
switch and the 4x oversampling are still only our reading, and a test built on a
misreading confirms it rather than catches it.

Building tier 3 already turned one RX assumption up. `decode_4bitpbit_serial`
seeds its bit polarity from the very first sample of the buffer with a bit count
of zero, so it swallows one bit of whatever the buffer starts with and finishes
one byte early: a synthetic frame round-trips byte-for-byte except its first and
last. On the air that transient presumably lands in the sync pattern rather than
the data, which would explain why it has never mattered — but that is inference,
not evidence, and prefixing the stream with idle bits does not reproduce the
conditions, it trips the decoder's end-of-frame early return instead.

Settling it needs a real oversampled buffer captured off a live meter. Until
then the decoder test is a characterisation test over the 17 bytes that do
round-trip, labelled as such in the source so nobody mistakes it for a proof of
correctness.

That buffer cannot be checked in. A capture is raw oversampled bits and looks
opaque, but it decodes to the meter's radio address, its serial and thirteen
months of consumption history: enough for anyone in range to interrogate that
meter over the air, and enough to infer when the household was empty. In a
public repository that is a disclosure, not a fixture. Captures are therefore
gitignored under `test/fixtures/`, and no committed test reads one — a capture is
kept, if at all, as a purely local diagnostic artefact of whoever took it.

The committed suite instead covers the two things that can be tested without the
wire: field construction and validation, on *decoded* frames built from arbitrary
values with genuine Kermit checksums (`test_full_response_*`), and the decoder
itself as a characterisation round-trip over a synthetic stream. What it cannot
cover is the wire *timing*: the measurement that sized `RADIAN_FRAME_SIZE` — 11.4
bit periods per byte, against the 11 originally assumed — is recorded here in
prose rather than asserted in CI, because reproducing it needs a real oversampled
capture, which `scripts/decode_capture.py` turns back into bytes from the MQTT
topic. Synthesising a substitute *capture* was considered and rejected: raw bits
re-encoded through our own encoder would confirm this codebase's assumptions about
the wire rather than test them, which is the specific failure this ADR exists to
avoid. Synthetic *decoded* frames carry no such circularity — they exercise the
checksum and the offsets, not the sampling — which is why those are committed and
a synthetic capture is not.

One on-hardware read against a live meter therefore remains the final gate, and
no amount of green on the desktop substitutes for it.

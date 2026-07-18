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
   overflow; `MDMCFG2` and `PKTCTRL0` are restored whichever step fails.
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

The tests compile the real, unmodified `cc1101.cpp` and `everblu_cyble.cpp`
against fake `Arduino.h`/`SPI.h` primitives, with a stateful CC1101 model behind
them. Nothing is made virtual and no seam is introduced for the tests' benefit:
for a radio driver the correctness *is* the exact register values and strobe
order, so the thing under test must be the thing that gets flashed.

The model must track FIFO occupancy and the IDLE→TX→TXFIFO_UNDERFLOW transition.
A model that merely answers status reads plausibly would let the underflow bug in
ADR-0001 pass green, which is the one failure this suite exists to prevent.

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

Settling it needs a real oversampled buffer captured off a live meter, checked in
as a fixture. Until then the decoder test is a characterisation test over the
17 bytes that do round-trip, labelled as such in the source so nobody mistakes it
for a proof of correctness.

One on-hardware read against a live meter therefore remains the final gate, and
no amount of green on the desktop substitutes for it.

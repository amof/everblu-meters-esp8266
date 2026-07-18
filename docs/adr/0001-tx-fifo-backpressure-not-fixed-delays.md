# Feed the CC1101 TX FIFO by backpressure, never by fixed delays

The wake-up preamble and master request are sent in infinite packet length mode
with no hardware preamble or sync word, so the carrier exists only for as long
as the TX FIFO has bytes in it. If the FIFO runs dry mid-transmission the CC1101
enters TXFIFO_UNDERFLOW, and the datasheet (§15.4) is explicit that the only way
out is an SFTX strobe and that *writing to the TX FIFO after it has underflowed
will not restart TX mode* — so the whole exchange is silently lost. We therefore
gate every FIFO write on `waitTxFifoFree()`, which polls TXBYTES, rather than
sleeping for the nominal drain time.

## Considered options

Refilling on a fixed `delay(30)` per 8-byte chunk looks correct and is much
simpler: 8 bytes at 2400 bit/s drain in 26.6 ms, so 30 ms "obviously" keeps up.
It does not. Matching the drain rate guarantees the FIFO is empty for ~3.4 ms
before every refill, which is exactly the underflow condition. This was tried
and it broke the radio completely; the bug was hard to find because the failure
is silent and happens before any data reaches the air.

## Consequences

The FIFO is deliberately kept nearly full rather than nearly empty. This costs a
few SPI status reads per chunk, and in exchange it buys ~180 ms of slack, which
also absorbs the timing jitter introduced when the ESP8266 SDK services WiFi
inside `delay()`.

Two follow-on constraints come from the same place and must not be "simplified"
either: the TX FIFO has to be loaded *before* the STX strobe, or the chip
underflows immediately on entering TX; and the master request may only be
written once the FIFO has 39 free bytes, because a TX FIFO overflow corrupts the
FIFO content (§10.1) and would put a malformed request on the air.

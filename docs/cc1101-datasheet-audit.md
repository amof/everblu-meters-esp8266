# CC1101 datasheet audit and test bring-up — working notes

Status of an audit of the rewritten CC1101 driver against the datasheet
(`cc1101.pdf`, SWRS061I), plus the desktop test harness built alongside it.
Written to be picked up later. Not an ADR: the decisions live in
[ADR-0003](adr/0003-test-the-invariants-not-the-trace.md), this is the state of
play.

Last worked on: 2026-07-18. Nothing here is committed yet.

## Why this was done

The README declares a blocking bug: the ESP cannot complete an exchange with the
meter. The library was rewritten from scratch after `87bdae3`, which is the last
commit known to have actually read a meter — so every difference from it is a
suspect. The goal was regression-proofing the CC1101 *sequencing*, with
pure-logic tests as scaffolding.

## Prime suspect: the GDO0 wait was inverted

`IOCFG0 = 0x06` makes GDO0 assert HIGH when the sync word is received and
de-assert at end of packet. The pin is therefore LOW when the code starts
waiting.

| Commit | Code | Effect |
|---|---|---|
| `87bdae3` (working) | `while (digitalRead(GDO0) == FALSE)` | waits for HIGH = sync received |
| `19f6aa5` (rewrite) | `while (... == voltageLevel)`, called `LOW`/`HIGH` | stage 1 still correct |
| `b60429f` ("refactor: few changes") | `while (... != voltageLevel)` | both call sites inverted |
| `022db51` ("does not need an argument") | `while (... == HIGH)` hardcoded | inversion baked in |

Because GDO0 is already LOW on entry, the wait returned `true` immediately
without waiting for anything, and stage 2 read the FIFO before the sync word had
arrived.

**Fixed**, as `waitForGdo0Assert()`. The rename is deliberate — the old name
never said which direction, which is what let a refactor flip it unnoticed.

This is high confidence but **still a hypothesis**: it has not been confirmed
against a live meter. There may be more than one fault.

## Other findings

Fixed:

- **`status_state` masked four bits, should be three.** Table 23: bit 7 is
  CHIP_RDYn, bits 6:4 are STATE. Masking `0x0F` folds CHIP_RDYn into the state,
  so every `waitForState` fails whenever the crystal is not yet stable. Five
  accessors in `cc1101.cpp`. Worked in practice only by luck.
- **`reset()` held CSn low for 10 us**; 19.1.1 asks for at least 40 us on both
  edges. Now 45 us either side.
- **`rssiDbm` was `uint8_t`** while `rssiTo2dbm()` returns `int8_t`, so -74 dBm
  logged as 182. Now signed, format `%d`.
- **Dead code**: `if (freq0 > 255)` where `freq0` is a `byte`.

Neither the mask nor the reset timing was likely causing the field failure — do
not read those fixes as fixes to the symptom.

Left alone deliberately:

- **The RX FIFO is emptied mid-packet.** Datasheet 20: *"the RX FIFO should
  never be emptied before the last byte of the packet is received"*, and for
  packets over 64 bytes read `RXBYTES.NUM_RXBYTES - 1`. The meter response is
  ~744 oversampled bytes and `readFifoData()` reads all `n`. The documented
  failure is a duplicated byte, which in a 4x oversampled stream would corrupt
  the decode. **The old working code did exactly the same**, so this is latent
  rather than a regression, and changing it would alter behaviour that has field
  evidence behind it. Revisit if GDO0 does not fully fix the decode.
- **`PATABLE[0] = 0x60` is 0 dBm at 433 MHz**, not the +10 dBm (`0xC0`) the wiki
  cites for the reference master. `PA_Test[]` holding `0xC0` sits unused. This
  was *not* changed by the rewrite — `87bdae3` has the identical `PA[]` — so it
  is not a regression, but 10 dB is worth having if range is marginal.

## Verified correct

Checked against the datasheet and found right, so they need not be re-checked:

- `MDMCFG4 = 0xF6` -> 58 kHz RX filter BW
- `MDMCFG3 = 0x83` -> 2398 bps (nominal 2400)
- `MDMCFG4 = 0xF8` -> 9.59 kbps, exactly 4x — **this is the oversampling**
- `DEVIATN = 0x15` -> 5157 Hz (spec 5 kHz)
- `MCSM0 = 0x18` -> FS_AUTOCAL = 01, calibrate on IDLE->TX/RX
- `FIFOTHR = 0x47`, `IOCFG0 = 0x06` comment accurate
- the read-twice errata in `readStatusReg()`
- the RX register sequences match `87bdae3` register-for-register
- ADR-0001's underflow reasoning: 15.4 quoted verbatim and correct

Citation corrections made: the overflow-corrupts-FIFO rule is **section 20**,
not 10.1 (10.1 is the Chip Status Byte); Table 42 is per-strobe (SFTX in IDLE or
TXFIFO_UNDERFLOW, SFRX in IDLE or RXFIFO_OVERFLOW), not one combined list.

Also worth knowing: **FIFO_BYTES_AVAILABLE saturates at 15** ("15 or more"), so
the free-byte nibble of the chip status byte can never report more than 15.
Anything needing a real count must read TXBYTES/RXBYTES, as `waitTxFifoFree()`
does — which is why that nibble is not kept anywhere.

## Test harness

`test/` holds tier 3 of ADR-0003. Six tests, all passing, ~1 s.

Two of them check `crc_kermit` against frames published on the Maison Simon wiki
(`DADC` for the master request, `909E` for the meter ack) — ground truth
independent of both codebases. Others pin the serial framing: 19 bytes -> 30
on-wire at 12 bits per byte, which independently explains why `TX_BUFFER_SIZE`
is 39 (9 sync + 30).

`test_decoder_round_trips_frame_body` is a **characterisation test, not a
correctness test**, and is labelled as such in the source. `decode_4bitpbit_serial`
swallows one bit of whatever starts its buffer and finishes a byte early, so a
synthetic frame round-trips except its first and last byte. On the air that
transient presumably lands in the sync pattern — that is inference, not
evidence. Prefixing idle bits does not reproduce it, it trips the decoder's
end-of-frame early return instead.

### Running it

```sh
pio test -e native      # desktop tests
pio run  -e esp8266     # firmware
```

The native env needs a host compiler. One is installed
(`BrechtSanders.WinLibs.POSIX.UCRT.LLVM`, GCC 14.2.0) but was not on PATH in the
session where this was written:

```
%LOCALAPPDATA%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT.LLVM_*\mingw64\bin
```

Reading the datasheet needs poppler, unpacked at `poppler-26.02.0/` (gitignored):

```sh
./poppler-26.02.0/Library/bin/pdftotext.exe -layout cc1101.pdf out.txt
```

## Next

1. **Flash and test.** Everything above is desk work. Whether the meter answers
   is the only thing that settles the GDO0 hypothesis.
2. **Capture a real RX buffer** if a read succeeds, and check it in as a
   fixture. This is the single highest-value artefact available: it would settle
   the decoder question outright and turn the characterisation test into a real
   one.
3. **Tier 1** — freeze the `87bdae3` TX byte stream as a fixture and assert the
   rewrite produces it byte-identically.
4. **Tier 2** — the stateful CC1101 model (FIFO occupancy, IDLE->TX->underflow)
   behind fake Arduino/SPI primitives, asserting the invariants rather than a
   recorded trace.

Tiers 1 and 2 are where the sequencing risk actually sits. Everything built so
far is scaffolding.

## Uncommitted state

Working tree at time of writing, nothing staged:

- `src/cc1101.cpp`, `include/cc1101.h`, `src/everblu_cyble.cpp` — the fixes above
- `docs/adr/0003-*.md` — new
- `docs/adr/0001-*.md`, `include/cc1101.h` — citation fixes
- `CONTEXT.md` — added **Master ack** (documented in the protocol, never sent by
  this reader nor by the one it descends from)
- `platformio.ini` — `[env:native]`
- `test/` — new
- `.gitignore` — `poppler-*/`

The GDO0 fix and the rest are mixed together in one working tree. If you want to
bisect, split it: GDO0 alone in the first commit, the rest in a second.
`cc1101.pdf` (2.6 MB) is still untracked and needs a decision.

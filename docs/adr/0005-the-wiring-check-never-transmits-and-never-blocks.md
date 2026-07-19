# The wiring check never transmits, and never blocks a reading

A reader that is assembled wrong and a reader whose meter never answered look
identical from Home Assistant: both end at `no_response`, which names the meter
as the problem. The failure that motivates this is GDO0 — a single wire, on a
pin the firmware only assumes it is on, exercised by no register read. Commit
`aabe852` ("fix: gdo0 was inverted") and the defensive docstring on
`waitForGdo0Assert` are both scar tissue from that wire. The wiring check exists
to tell the two cases apart on first assembly, before anyone goes looking for a
radio problem that is really a jumper problem.

It is deliberately weak in two ways, and the two are the same decision seen
twice.

**It proves connectivity, not function.** The check confirms that SPI reaches
the chip, by reading PARTNUM and VERSION, and that GDO0 reaches the pin the
firmware believes it does, by writing `IOCFG0 = 0x2F` (drive GDO0 constant low)
then `0x6F` (the same with `GDOx_INV` set, so constant high) and reading the pin
back each time. The chip stays in IDLE, the synthesiser never runs, and nothing
is radiated. Both directions are needed because the pin is configured
`INPUT_PULLUP`: a disconnected GDO0 floats high, so only the drive-low step
detects a missing wire, and only the drive-high step detects a pin shorted to
ground. Nothing here says anything about the crystal, the PA or the antenna.

**It therefore must not gate anything.** A check that infers a physical fact
from indirect evidence, on hardware variants we have not seen, will eventually
be wrong. If it were allowed to refuse readings, its first false positive would
present as "my reader stopped finding the meter", with the diagnostic itself as
the cause and a firmware change as the only escape. A wiring check that can
brick the device it is checking is worse than the fault it diagnoses. It
publishes a verdict and logs a line at the top of each interrogation while
failing; it never prevents an interrogation or a sweep.

## Considered options

**Transmitting a wake-up preamble and watching GDO0 assert** would prove far
more — the synthesiser, the PA and GDO0 in one shot. It was rejected because
the check is for first assembly: on a bench, possibly with no antenna attached,
possibly outside the meter's wakeup window. Transmitting into an unterminated
output is exactly what a first-time builder should not be asked to do, and a
check that only works in the field is not a bench check.

**Folding the verdict into the status topic** was rejected because status is a
latest-event value and wiring fitness is sticky. Writing a wiring verdict there
would destroy the last interrogation outcome every time the check ran.

**Publishing `offline` on a failed check** was rejected as contrary to the
definition of availability: only the absence of a reader makes it unavailable.
A reader with a dead GDO0 is present and answering, and greying out its entities
would hide the log topic at the moment it is most needed.

**Blocking sweeps only** — refusing the expensive path but allowing single
interrogations — remains the reasonable next step, and is worth revisiting once
there is field evidence the check does not false-positive. It is not the right
default on day one.

## Consequences

The verdict is ordered rather than a set. A failed SPI test short-circuits: with
SPI dead, GDO0 is untestable rather than failing, since the pin would only be
read through its pull-up. So a single value with a first failing step is honest
where one entity per layer would not be.

`IOCFG0` must be restored to `0x06` on every exit path, including the failing
ones. Missing that restore breaks every subsequent interrogation by way of the
diagnostic meant to protect them — the same class of fault as `aabe852`. This
joins the register-restoration invariants listed in ADR-0003.

PARTNUM and VERSION are published as attributes of the wiring entity rather than
as an entity or a device `hw_version`. Their only real use is as the evidence
behind a `spi_failed` verdict, so they belong next to it; keeping them out of
the device block also keeps the discovery payloads compile-time constants.

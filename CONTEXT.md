# EverBlu Meters

Reads water consumption from Itron EverBlu Cyble Enhanced meters over the RADIAN
protocol at 433 MHz, using a CC1101 transceiver driven by an ESP8266, and
publishes the result to Home Assistant over MQTT.

## Language

### The exchange

**Wake-up preamble**:
A continuous ~2 second carrier of alternating bits at 2400 bit/s that rouses the
meter from sleep. Must be uninterrupted to be recognised.
_Avoid_: WUP, wakeup buffer

**Master request**:
The frame the reader sends to address one specific meter, identified by its year
and serial number and protected by a Kermit CRC.
_Avoid_: interrogation, query, txbuffer

**Meter ack**:
The short frame the meter returns to acknowledge a master request, before it
sends any consumption data.

**Meter response**:
The long frame carrying the meter's consumption data. Its payload positions are
fixed offsets, not a parsed structure.
_Avoid_: meter report, meter data

**Reading**:
One completed exchange yielding a current index. The meter counts these itself
and reports the total, which is how the fork detects a successful decode.

### On the wire

**Sync pattern**:
The bit sequence separating silence from payload. It is detected in two stages:
its start at 2.4 kbit/s, then its end, after which the radio switches rate.
_Avoid_: preamble (that is the wake-up preamble, a different thing)

**Oversampling**:
The meter's payload is captured at four samples per transmitted bit, so a raw
receive buffer is always four times the decoded frame size.

**Radian frame size**:
The on-wire length of a frame, larger than its decoded length because every byte
carries one start bit and two stop bits.

### The meter

**Current index**:
The meter's cumulative water consumption in litres.
_Avoid_: liters, consumption, total

**Battery lifetime**:
Remaining battery life reported by the meter, in months.

**Wakeup window**:
The hours during which the meter will answer a master request; outside it the
meter is deaf. Documented as Monday to Saturday 06:00–18:00, and reported by
each meter about itself as a start and a stop hour.

**Working frequency**:
The frequency the CC1101 must be *programmed* to for this particular board to
reach the meter. The meter's carrier is fixed at 433.82 MHz; this value differs
from it by the module's crystal error, so it is a property of the reader
hardware, not of the meter.
_Avoid_: meter frequency (it is not the meter's)

**Sweep**:
Trying working frequencies outwards from a centre until the meter answers. A
full sweep is the provisioning path; a re-centring sweep is the much narrower
one used to track crystal drift around an already-known value.
_Avoid_: scan

**Meter profile**:
What the reader has learned about its meter — working frequency and wakeup
window — persisted in EEPROM so a sweep happens once rather than every read.

**Provisioned**:
State of a reader that holds a valid meter profile, and can therefore read
directly instead of sweeping.

**Interrogation**:
One deliberate attempt to obtain a reading, whether scheduled or triggered by
hand. Distinct from a sweep, which is the search for a working frequency that an
interrogation may have to fall back on.

**Reading hour**:
The local hour at which the reader interrogates the meter automatically, once a
day. User configuration, not something learned from the meter.

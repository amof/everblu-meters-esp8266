# The frequency sweep calibrates our crystal, it does not discover the meter

The Cyble meter's carrier is fixed by specification at 433.82 MHz (25 kHz
channel, 5 kHz deviation, 2400 baud). We sweep anyway because the CC1101 does
not radiate the frequency it is programmed with: the reference implementation's
author reports two CC1101 boards given identical settings where one reached the
meter and the other did not. The sweep therefore corrects *our* module's crystal
error, and the value it finds is a property of the reader hardware — which is
why it is safe to cache per device, and why it must be re-validated rather than
trusted forever.

Source: the Maison Simon wiki, archived under `docs/cyble/`.

## Consequences

**The cached working frequency is a hint, not a constant.** Crystal error moves
with temperature — 10 ppm is 4.3 kHz at 433 MHz — and the reader typically lives
in an unheated meter box. A read therefore tries the stored value, then a
narrow ±6 kHz re-centring sweep, and only then a full sweep, rewriting the
stored value whenever the winning frequency has moved.

**Step size is 2 kHz.** This is what the reference implementation recommends.
An earlier 500 Hz step produced 261 candidate frequencies; at ~2.2 s of
transmission each, a full miss was ten minutes of near-continuous carrier.

**Sweeping outside the wakeup window is guaranteed to fail**, since the meter is
deaf Sundays and outside 06:00–18:00. Reads are gated on the clock being
NTP-synchronised for exactly this reason: an unsynchronised clock must block a
read rather than permit a sweep that cannot succeed.

**Reads are not triggered by MQTT connection.** `onConnectionEstablished` fires
on every reconnection, so driving the radio from it let a flapping broker
trigger repeated multi-minute transmissions. The read loop is started once and
thereafter reschedules itself.

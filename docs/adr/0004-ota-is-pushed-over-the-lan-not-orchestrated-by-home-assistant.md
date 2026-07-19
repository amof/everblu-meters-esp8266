# Firmware updates are pushed over the LAN, not orchestrated by Home Assistant

Every other thing this device does, it does through Home Assistant: seven
discovered entities, two command buttons, a schedule written from a text entity.
So the obvious shape for updates was an MQTT `update` entity — the reader pulls
a firmware image from a URL that Home Assistant serves out of `/config/www`, and
an Install button in the dashboard triggers it. We chose the opposite: the
laptop pushes an image straight to the device with `espota`, and Home Assistant
is not involved at all.

The deciding argument is that a pull-based design buys reach we can obtain more
cheaply. It exists to reach devices the uploader cannot address, or to schedule
installs for later. The image has to be compiled on a machine with the toolchain
regardless — there is no version of this where an update begins somewhere other
than in front of a compiler — so routing it through Home Assistant adds a file
store, a URL to configure, a version-comparison protocol and an HTTP client to
the firmware, none of which shorten the path from "I changed some code" to "the
device is running it".

The reach argument is not hypothetical here, and this decision nearly went the
other way because of it. The uploader is a container on a server on a different
network; the reader is on an isolated IoT VLAN behind OPNsense. They are not
peers on one LAN. Push still won because making the return path work turned out
to be two pieces of configuration — pin the callback port, publish it from the
container — against a permanent HTTP client in the firmware. Had the network
been genuinely one-way, pull would have been correct.

A pull design is also strictly more dangerous in the failure case we care about.
It points a device in a cellar at a URL and flashes whatever comes back, whereas
`espota` requires someone to be deliberately uploading, authenticated, right
now.

We rejected a device-hosted web upload form for the same reason we rejected the
pull design — it is machinery for getting the image somewhere it already is —
and rejected building from CI so the device could pull from a GitHub release,
because HTTPS on an ESP8266 means either a pinned CA certificate that expires or
no verification at all.

## Consequences

**The first flash and every recovery must be over USB.** OTA cannot install
itself, and a bad image is only recoverable with a cable. `env:esp8266` stays
the USB environment and `env:esp8266_ota` extends it, so the two differ in
transport and nothing else.

**There is no Install button in Home Assistant.** A future reader will notice
the asymmetry with every other feature. That is deliberate, and this file is
why. The firmware version *is* reported, as `sw_version` on the device — an
entity is not needed for that, so the visibility argument for a pull design
survives without the machinery.

**The image travels in the opposite direction to the command.** The uploader
sends a UDP invitation, and the device then opens a TCP connection *back* to the
source address it saw, on `--host_port`. Every hop in that return path has to
permit a connection the device originates: firewall rules between VLANs, and a
published port when the uploader is containerised or NATed. `--host_port` must
be pinned, since espota otherwise picks randomly from 10000-60000 and no rule
can match it. This is why a device that answers ping can still fail to flash —
ping only ever exercises the other direction.

**The OTA password is duplicated.** The firmware carries it in
`include/secrets.h`; the uploader passes it from `platformio_local.ini`. Both
are gitignored with a committed `.example` beside them. Two programs
authenticate to each other, so there is no single place for the secret, and
`EspMQTTClient::enableOTA(NULL)` quietly falling back to the MQTT password makes
leaving it unset worse than merely insecure.

**One identifier now has three jobs.** `MQTT_CLIENT_NAME` is the broker client
ID, the mDNS hostname, and the `upload_port` you flash to, because
`enableOTA()` passes it to `ArduinoOTA.setHostname()`. Changing it changes where
uploads go.

**Updates are reachable during a sweep, for free.** `ArduinoOTA.handle()` is
pumped from `mqtt.loop()`, which the sweep already calls between frequency
attempts to keep the MQTT keepalive alive. Without that the device would be
un-flashable for up to ten minutes at a time, and `espota` would report only an
unexplained timeout.

**The radio must be parked before the reboot.** The CC1101 has no reset line
wired, so it does not restart when the ESP does; an update landing mid-carrier
would leave it transmitting through the flash write, the reboot and the eboot
copy — tens of seconds of unmodulated 433 MHz from an unattended device, well
into ISM duty-cycle territory. `ArduinoOTA.onStart` calls `radioIdle()` for
this. The between-attempts pump happens to fire only when the radio is already
idle, so the guard is belt-and-braces today; it exists so that stops being
something the next person has to notice.

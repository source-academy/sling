# Sling

SVML interpreter link &rarr; Sinter link &rarr; Slink &rarr; Sling.

A sling is also something used to launch small objects... so, we use a sling to
launch small programs to devices.

This repository contains an MQTT-based protocol that allows compiled SVML
programs to be sent to devices to run, plus daemons that implement the protocol
for various platforms.

The initial use case is for Source Academy users to be able to write Source
programs in the Source Academy frontend and seamlessly run those programs on
embedded devices, but ultimately the daemons just receive a compiled binary and
pass it to a runtime, so this can be used for anything, really.

Note that because the protocol merely specifies the MQTT topics that devices
will receive programs from and publish output to, there is little code shared
between the daemons (aside from Sinter itself), which use their respective
platforms' MQTT and TLS libraries where possible.

See [Sling.md](./Sling.md) for the protocol.

## Directory layout

- `deps`: Dependencies, including Sinter itself.
- `linux`: Linux daemon. This is written with embedded Linux in mind, but will of course work on any Linux device.
- `esp32`: ESP32 daemon.
- `client-js`: Client for the browser.

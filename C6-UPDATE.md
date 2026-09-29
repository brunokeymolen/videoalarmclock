# Updating the ESP32-C6 Wi-Fi co-processor (optional)

> [!CAUTION]
> **Advice, not a required step, and entirely at your own risk.**
>
> I had to update the C6 on one of my own devices. For convenience and
> later reuse I left the code and this page in the repository. The clock
> will almost certainly work fine without it; at most, FTP uploads may
> get somewhat faster.
>
> The hardware, and the firmware it ships with, are
> **[Waveshare](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-4b.htm)'s**.
> Nothing here is from or endorsed by Waveshare or Espressif, and it has
> been done on exactly **one** board. Check the
> [Waveshare documentation](https://www.waveshare.com/wiki/ESP32-P4-WIFI6-Touch-LCD-4B)
> for your board first, and if you are unsure, don't. The [disclaimer](DISCLAIMER.md) applies in full: no
> responsibility is taken for a damaged device.

---

## What the C6 is

The ESP32-P4 that runs the clock has no radio. Wi-Fi comes from a second
chip on the board, an **ESP32-C6**, which the P4 talks to over SDIO
using Espressif's [esp_hosted](https://github.com/espressif/esp-hosted-mcu).
The C6 has its own flash and its own firmware, and flashing the clock
never touches it: it keeps whatever firmware Waveshare put on it at the
factory.

The clock is built against **esp_hosted 3.0.7**. The C6 firmware on the
board this was tested on was older, too old even to report its own
version. esp_hosted handles that combination in a *back-compatible*
mode: Wi-Fi, NTP and FTP all work, just over an older, less efficient
transfer mode.

---

## How to check yours

The version shows in the boot log. With the board on USB, from the
repository:

```sh
sources/tools/idf.sh -p /dev/ttyACM0 monitor     # Ctrl+] to leave
```

Any serial terminal at **115200 baud** works too; press the board's
reset button to see the boot from the start. Look for this line, about
three seconds in:

```
eh_init_evt: esp-hosted fw versions: host=3.0.7 coprocessor=...
```

| What it says | Meaning |
| --- | --- |
| `coprocessor=3.0.7 (match)` | Already up to date. Nothing to do. |
| `coprocessor=0.0.0` followed by `major version mismatch — OTA coprocessor from host` | Factory firmware in back-compatible mode. **Wi-Fi works.** This page applies, but is optional. |
| Anything else | Not what this was tested with. Do not use this page; see the [Waveshare documentation](https://www.waveshare.com/wiki/ESP32-P4-WIFI6-Touch-LCD-4B). |

---

## When to do it

**Usually never.** Leave the C6 alone if Wi-Fi works for you, and it
almost certainly does.

Only consider it when **all** of these hold:

- the boot log shows the `0.0.0` / `major version mismatch` case above,
- you have a reason: FTP uploads are painfully slow, for example,
  and you accept it may not help,
- you are comfortable building firmware from [`sources/`](sources/README.md)
  (Docker, the board on USB),
- you accept that a board with **no working Wi-Fi** is a possible
  outcome, and that recovering from it is not covered here.

---

## What can go wrong

The update is sent to the C6 over the same link Wi-Fi uses, with
esp_hosted's own co-processor OTA calls. The C6 writes the new image
next to the old one and switches to it only after checking it.

- **Interrupted or rejected before the end** (cable pulled, power lost,
  image refused): the C6 keeps running its old firmware. Run it again.
- **Switched, but the new firmware cannot talk to the P4**: the clock
  has no Wi-Fi. It still keeps time on its RTC and plays alarms from the
  SD card, but it will not sync or accept uploads. This page cannot undo
  that, because it needs the very link that is gone. Recovery means
  flashing the C6 directly; see the
  [Waveshare documentation](https://www.waveshare.com/wiki/ESP32-P4-WIFI6-Touch-LCD-4B)
  for the board.

There is no downgrade path here back to the factory C6 firmware.

---

## Doing it

You need what [`sources/README.md`](sources/README.md#what-you-need)
lists: Docker, and the board on USB by its **UART** port. Use a stable
power source, and do not unplug the board while it runs.

**1. Build and run the updater.**

```sh
sources/tools/c6-update.sh              # or: -p /dev/ttyACM0
```

This builds two things and replaces the clock firmware for the moment:

- `sources/firmware/c6_cp`: the C6 firmware, esp_hosted 3.0.7 built
  for the ESP32-C6 in streaming mode, the mode the clock already uses.
- `sources/firmware/c6_updater`: a small P4 program that carries that
  image, checks the C6's version, and sends the image over only if the
  C6 is older than 3.x. It does not touch your Wi-Fi settings or alarms.

It then opens the serial monitor. The transfer takes about fifteen
seconds; the board restarts once, and then repeats a `RESULT` line:

| `RESULT:` | Meaning |
| --- | --- |
| `UPDATED` | Done. The C6 runs 3.0.7. |
| `NOTHING TO DO` | It already did. Nothing was written. |
| `FAILED - ... old firmware` | Nothing changed. Safe to put the clock back as it was. |
| `FAILED - no link to the C6` | The updater could not reach the C6. Nothing was written. |

Leave the monitor with **Ctrl+]**. The updater only ever tries once per
power-up, so it will not keep rewriting the C6.

**2. Put the clock back.**

```sh
sources/tools/flash.sh
```

This builds the clock from source and installs it the way the updater
went on, which leaves your Wi-Fi network and alarms in place.
`firmware/flash.sh` (a released image) works as well, but it writes the
whole image from the start of the flash and may reset those settings.

**3. Check.** In the boot log, the line should now read:

```
eh_init_evt: esp-hosted fw versions: host=3.0.7 coprocessor=3.0.7 (match)
```

followed a few seconds later by `NN20CLOCK_NET: connected`.

---

## What it was tested on

One Waveshare ESP32-P4-WIFI6-Touch-LCD-4B (*Onboard Camera Interface*
version), ESP32-P4 revision v1.3, whose C6 came with factory firmware
that did not report a version. Updated to esp_hosted 3.0.7 in streaming
mode; Wi-Fi, NTP and the clock's normal operation were checked
afterwards.

---

## For developers

- The C6 image's esp_hosted version is pinned in
  `sources/firmware/c6_cp/main/idf_component.yml`. The clock itself
  takes whatever esp_hosted the component manager resolves (see
  `sources/firmware/nn20clock/dependencies.lock`). If the clock ever
  moves to a new esp_hosted major version, this pin, and the updater's
  `CURRENT_MAJOR`, have to follow.
- The C6 is built in **streaming** mode, not esp_hosted 3.x's default
  **SW_AGGR**. SW_AGGR is faster but needs a patched ESP-IDF, and the
  clock's container uses a stock one.
- The update is the esp_hosted co-processor OTA chain: begin → write →
  end → activate. Factory C6 firmware has no `activate` and switches in
  `end`, so an `activate` failure in the log is expected there.

## Links

- Waveshare: [product page](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-4b.htm) ·
  [wiki](https://www.waveshare.com/wiki/ESP32-P4-WIFI6-Touch-LCD-4B)
- Espressif: [esp-hosted-mcu](https://github.com/espressif/esp-hosted-mcu) ·
  [esp_hosted on the component registry](https://components.espressif.com/components/espressif/esp_hosted)

# Video Alarm Clock — the source

Everything needed to **build, flash, monitor and release** the firmware
is in this directory. Nothing above it is required to build, and nothing
here is required to install a released clock — [the top of this
repository](../README.md) has the browser installer, `firmware/flash.sh`
and the video tooling, and that is still the fast way to get a working
device.

You need **Docker** and the board on a USB cable. The ESP-IDF toolchain
lives in a container defined in [`.devcontainer/esp32`](.devcontainer/esp32),
so nothing but Docker is installed on your machine.

```sh
sources/tools/test.sh      # build and run the tests on your computer - no board
sources/tools/flash.sh     # build the firmware and install it on the board
```

Both work from anywhere; they find this directory themselves. The rest
of this file explains what they do, what to do when they do not work,
and how a release is cut.

---

## What you need

**Hardware**

- A **Waveshare ESP32-P4-WIFI6-Touch-LCD-4B** — 720×720 MIPI-DSI panel,
  GT911 touch, ES8311 audio codec, 32 MB flash, 32 MB PSRAM. Tested on
  the *Onboard Camera Interface* version, carrying an **ESP32-P4
  revision v1.3**. The firmware is built for revisions v1.00–v1.99;
  other v1.x parts should work but have not been tried. A board the
  image refuses says so plainly while flashing: *"requires chip revision
  in range … this chip is revision …"*.
- A **USB-C cable**, into the board's **UART** port — not the OTG one.

**On your computer**

- **Docker**, and your user able to run it (`docker ps` must work
  without `sudo`).
- Access to the board's serial port. On Linux that means being in the
  `dialout` group:

  ```sh
  sudo usermod -aG dialout "$USER"     # then log out and back in
  ```

- A C compiler and CMake, only for the host-side tests
  (`tools/test.sh`). Any distribution's `build-essential` and `cmake`
  will do.
- `gh`, logged in, only to publish a release.

Linux is what this is developed and tested on. macOS should work for the
build; passing a USB serial device into a Linux container from macOS
does not, so flashing from macOS needs `esptool` on the host instead —
`firmware/flash.sh` at the top of the repository does exactly that.

---

## 1. Build

The first command builds the ESP-IDF Docker image (about a 3 GB
download) and fetches the managed components — LVGL, the ST7703 panel
driver, the GT911 touch driver, `esp_hosted`/`esp_wifi_remote` for the
Wi-Fi co-processor. That takes a while. Everything after it is fast.

```sh
sources/tools/idf.sh set-target esp32p4      # once, and after a fullclean
sources/tools/idf.sh build
```

`tools/idf.sh` runs any `idf.py` command inside the container against
[`firmware/nn20clock`](firmware/nn20clock), as your own user, so nothing
in the tree ends up owned by root:

```sh
tools/idf.sh size            # where the flash went
tools/idf.sh size-components
tools/idf.sh menuconfig      # the NN20Clock menu: NTP server, timezone, OTA URL
tools/idf.sh fullclean
```

**After editing `sdkconfig.defaults`, delete the generated `sdkconfig`
and set the target again** — otherwise the old values survive:

```sh
rm -f sources/firmware/nn20clock/sdkconfig
sources/tools/idf.sh set-target esp32p4
sources/tools/idf.sh build
```

`NN20CLOCK_IDF_PROJECT` points the wrapper at another project in this
tree (that is how the on-target tests are built), and
`NN20CLOCK_IDF_RAW=1` runs a command in the container instead of an
`idf.py` subcommand, which is how `flash.sh --image` reaches `esptool`.

### In an editor

[`.devcontainer/esp32/devcontainer.json`](.devcontainer/esp32/devcontainer.json)
is the same image, for VS Code's *Dev Containers: Reopen in Container* —
open **this** directory as the workspace folder and it will be found. It
passes USB through, so `idf.py build` and `idf.py flash` work from the
integrated terminal exactly as `tools/idf.sh` does from outside.

One difference, and it matters for anything you intend to publish: VS
Code mounts the folder you opened, so a build inside the devcontainer
cannot see `.git` at the top of the repository and ESP-IDF stamps the
image with its fallback version instead of the tag. `tools/idf.sh`
mounts the repository for exactly that reason — see
[`tools/lib/paths.sh`](tools/lib/paths.sh). Use the wrappers for
anything that goes on a board or into a release; the devcontainer is
there because an editor with working code navigation is worth having.

---

## 2. Install it on the device

Plug the board in by its **UART** port and:

```sh
sources/tools/flash.sh
```

That sets the target if this is a fresh tree, builds, finds the serial
port, writes the image, and leaves you in the serial monitor (**Ctrl-]**
to leave). It ends with the board resetting into the clock face.

```sh
tools/flash.sh -p /dev/ttyACM0     # say which port
tools/flash.sh --no-monitor        # flash and exit
tools/flash.sh --erase             # wipe the flash first, then install
tools/flash.sh --image dist/v1.2.0/assets/nn20clock-v1.2.0-full.bin
```

`--erase` also wipes NVS, which is where the alarms and the Wi-Fi
credentials live. It is what you reach for when a board will not boot,
not part of a normal update.

`--image` writes a merged full-flash image at offset 0 instead of
building — that is how a staged release gets onto a board before it is
published.

By hand, if you would rather:

```sh
tools/idf.sh -p /dev/ttyACM0 flash
tools/idf.sh -p /dev/ttyACM0 erase-flash
```

**If it cannot open the port**, you are not in `dialout` (see above), or
something else is holding it — a monitor in another terminal is the
usual cause, and `fuser -v /dev/ttyACM0` will say so.

---

## 3. Watch it run

```sh
sources/tools/idf.sh -p /dev/ttyACM0 monitor
```

**Ctrl-]** leaves it. It needs a real terminal: from a script it fails
with *"Monitor requires standard input to be attached to TTY"*.

A healthy boot prints `Video Alarm Clock <version>` — taken from the
image, not from a string kept up to date by hand — then the chip
revision, the flash size and the free PSRAM and internal heap, and
brings up the workers, storage, the display, the timer, the manager and
the network in that order. After that an `alive, <n>s, internal heap
<n> KB` line every ten seconds: that number is what to watch when
something leaks, and its absence is a silent reset.

A panic prints a backtrace of addresses. To turn it into function names
and line numbers, the monitor does it for you; after the fact, the ELF
is in `firmware/nn20clock/build/nn20clock.elf`.

---

## 4. Tests

```sh
sources/tools/test.sh           # every suite, on your computer, in seconds
sources/tools/test.sh -R timer  # just the suites matching "timer"
```

Everything that is not hardware compiles and runs on a desktop — the
alarm firing rules, the AVI parser, the state machine, the storage
layer, the UI dispatch. That is the everyday loop, and it is where new
behaviour should be pinned.

The same suite sources also build into an application that runs **on the
board**, for the rare question that is genuinely target-specific
(FreeRTOS behaviour, PSRAM, alignment, real codegen):

```sh
NN20CLOCK_IDF_PROJECT=test/target sources/tools/idf.sh set-target esp32p4   # once
sources/tools/idf-test.sh                                                   # build, flash, watch
```

It prints its result over serial and ends in `PASS:` or `FAIL:`. **The
board keeps running whatever was flashed last**, so put the application
back afterwards with `tools/flash.sh`.

---

## 5. Cut a release

```sh
sources/release-scripts/prepare-release.sh v1.2.0 "What changed, in one line"
sources/tools/flash.sh --image dist/v1.2.0/assets/nn20clock-v1.2.0-full.bin
sources/release-scripts/publish-release.sh v1.2.0
```

`prepare-release.sh` tags `HEAD`, builds from scratch, checks the image
against the tag and the partition table, and stages
`sources/dist/<tag>/`. It publishes nothing, and it refuses to build on
a tree with any uncommitted or untracked file in it.

`publish-release.sh` commits `latest.json` at the top of the repository,
pushes it and the tag, creates the GitHub release with the two images
and `SHA256SUMS`, and then downloads the published image to check its
SHA-256 against the manifest.

That is what makes **gear icon → About → Check for updates** offer the
new version on every clock in the field, and what
[`firmware/flash.sh`](../firmware/flash.sh) downloads for a new board.
The browser installer needs nothing from you either:
[`.github/workflows/pages.yml`](../.github/workflows/pages.yml) runs on
the release event and rebuilds the page around the image that release
carries.

**[`release-scripts/README.md`](release-scripts/README.md) is the full
account** — what each script refuses, the order the two steps happen in
and why, and what to do when one fails halfway.

---

## What is in here

| | |
|---|---|
| `components/nn20clock_platform/` | the ESP-IDF / host build seam |
| `components/nn20clock_worker/` | task worker (FreeRTOS / pthreads) |
| `components/nn20clock_reqpool/` | lock-free request slots for async posts |
| `components/nn20clock_workers/` | the application threads |
| `components/nn20clock_time/` | shared date/time types |
| `components/nn20clock_alarm/` | the alarm model and when it fires |
| `components/nn20clock_media/` | what counts as media, and which names are safe |
| `components/nn20clock_avi/` | reading an AVI: headers, and walking the frames |
| `components/nn20clock_ringbuf/` | the audio/video ring buffer |
| `components/nn20clock_timer/` | the tick, local time, and alarm scheduling |
| `components/nn20clock_timefmt/` | time → the text a screen draws |
| `components/nn20clock_brightness/` | the brightness curve |
| `components/nn20clock_storage/` | config, credentials and alarms, flash-backed via NVS |
| `components/nn20clock_ui/` | UiBase: vtable, commands, UiWorker dispatch |
| `components/nn20clock_manager/` | the state machine and screen selection |
| `components/nn20clock_ota/` | over-the-air update |
| `components/nn20clock_app/` | composition root |
| `components/nn20clock_fonts/` | **firmware only** — the generated LVGL faces |
| `components/nn20clock_display/` | **firmware only** — ST7703 + LVGL + GT911 touch + PWM backlight |
| `components/nn20clock_sd/` | **firmware only** — the SD card |
| `components/nn20clock_net/` | **firmware only** — Wi-Fi on the C6, scanning, and SNTP |
| `components/nn20clock_ftp/` | **firmware only** — the FTP server |
| `components/nn20clock_es8311/` | **firmware only** — vendored codec driver; see its `VENDORED.md` |
| `components/nn20clock_audio/` | **firmware only** — the ES8311 codec, I2S, and the built-in tone |
| `components/nn20clock_player/` | **firmware only** — decoding and playing an alarm |
| `components/nn20clock_*_ui/` | **firmware only** — the screens |
| `firmware/nn20clock/` | the ESP32-P4 application: partition table and configuration |
| `test/` | the suites, host and on-target — see [`test/README.md`](test/README.md) |
| `tools/` | the build, test, flash and font wrappers |
| `release-scripts/` | building and publishing a version |
| `.devcontainer/esp32/` | the ESP-IDF container everything runs in |
| `CMakeLists.txt` | the host build, for the tests |

`nn20clock` is the internal name this started life under and is what the
code is called throughout; *Video Alarm Clock* is the product. Comments
occasionally cite "design §N", a design document that is not published —
the code and its comments stand on their own.

---

## Notes from the hardware

All of these were found by failing on a real board, and will bite anyone
writing their own firmware for this display:

- **The LCD backlight is active LOW.** Driving GPIO 26 high turns it
  *off*, and the panel is then black while DSI, LVGL and the flush path
  all report success.
- **The MIPI-DSI panel copies draw buffers asynchronously.**
  `esp_lcd_panel_draw_bitmap()` returns before the DMA is done. Calling
  `lv_display_flush_ready()` straight after it corrupts the next frame —
  signal completion from the `on_color_trans_done` callback instead.
- **How far the backlight dims depends on the PWM frequency, not the
  panel.** At the 5 kHz this started on, the screen went black in the
  low forties; at 240 Hz it dims smoothly into the teens. A floor is
  enforced on both the stored value and the one applied at boot, because
  a screen you cannot read is a device you cannot recover through its
  own settings screen. Change `BACKLIGHT_LEDC_HZ` and those floors have
  to be re-measured.
- **The audio codec shares an I2C bus with the touch panel.** ES8311 and
  GT911 are both on port 0. The registry's `espressif/es8311` is
  legacy-I2C-only and its constructor aborts the firmware before
  `app_main` if `i2c_master` is also linked, so the driver is vendored
  as `components/nn20clock_es8311/` with its I2C layer moved over.
- **The bootloader does not fit under the default partition-table
  offset.** PSRAM XIP plus GigaDevice flash support push it past the
  0x6000 ceiling that 0x8000 implies; the table lives at 0x10000
  instead.

Two rules the code holds to, worth knowing before changing it:

- **There are no mutexes, and there should never be one.** Each worker
  owns its data, and only callbacks on that worker touch it. Public
  functions copy their arguments into a request and post it — the queue
  is the synchronisation. A component that needs a lock has state that
  escaped its worker.
- **LVGL is called from exactly one thread**, the UiWorker, which is why
  `CONFIG_LV_OS_NONE=y`. A screen's constructor must not touch LVGL;
  widgets are created in `show()`. Anything slow — a Wi-Fi scan, an SD
  read — runs elsewhere and posts its result back, because that thread
  also draws.

---

## Wi-Fi credentials never come from a build

There is no build-time SSID or password, and there must not be one:
Kconfig strings become `.rodata`, so a real network name and passphrase
would travel inside every image built from this tree — including the
ones published as release assets, where `grep` finds them and nothing
can take them back. The network is chosen on the device (gear icon →
Wi-Fi) and kept in NVS.

`prepare-release.sh` checks the release build's generated config for
credential options that have come back, and refuses to stage the release
if it finds any.

---

## Licence

Copyright © 2026 Bruno Keymolen.

Video Alarm Clock is free software: you can redistribute it and/or
modify it under the terms of the **GNU General Public License** as
published by the Free Software Foundation, either **version 3** of the
License, or (at your option) any later version — see
[`../LICENSE`](../LICENSE). It comes with **no warranty**, and it is a
hobby alarm clock: do not rely on it as your only alarm for anything
that matters.

Two parts here carry their own terms, both compatible with the above:

- `components/nn20clock_es8311/` is Espressif's ES8311 driver,
  Apache-2.0, vendored with its I2C layer changed — see its `LICENSE`
  and `VENDORED.md`.
- `components/nn20clock_fonts/fonts/*.c` are generated from **DejaVu
  Sans Bold** under the DejaVu Fonts License; the notice is in
  [`components/nn20clock_fonts/README.md`](components/nn20clock_fonts/README.md).

The enclosure, above this directory, stays public domain under CC0 1.0
([`../enclosure/LICENSE`](../enclosure/LICENSE)).

The managed components fetched at build time (LVGL, the panel and touch
drivers, `esp_hosted`, `esp_wifi_remote`) are not in this tree and keep
their own licences, as does ESP-IDF itself.

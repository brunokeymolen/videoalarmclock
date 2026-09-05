# Firmware

`flash.sh` is the one-time install, over a USB cable. After it the clock
updates itself — gear icon → **About** → **Check for updates**.

```sh
./flash.sh                       # newest release, port autodetected
./flash.sh --version v0.2.0      # a specific release
./flash.sh --port /dev/ttyACM0   # if you have several serial devices
./flash.sh --file ./some.bin     # a file you already downloaded
```

The only thing it needs on your machine is esptool:

```sh
pip install --user esptool
```

## Which boards this has run on

The firmware is built for **ESP32-P4 revision v1.00 and up**. The board
it has been developed and tested on carries a **revision v1.3** part —
that is the only revision it has actually run on, so treat other v1.x
parts as "should work, unverified".

A chip outside the built range does not fail quietly. The flash refuses
with the revision in the message:

```
requires chip revision in range [...] (this chip is revision ...)
```

If you hit that, or if a different revision works fine, it is worth
saying so.

## What it does

1. Asks GitHub for the newest release, unless you named one.
2. Downloads `nn20clock-<version>-full.bin`.
3. Checks it against the release's published `SHA256SUMS`, and refuses
   to flash a download that does not match.
4. Writes it to the board at offset `0` and resets.

## The two images on a release, and which is which

| Asset | For |
| --- | --- |
| `nn20clock-<version>-full.bin` | **a board that has never run this firmware.** Bootloader, partition table, OTA selector and application, merged into one file that goes at offset `0`. This is what `flash.sh` downloads. |
| `nn20clock-<version>.bin` | **the application alone.** This is what a clock already in the field downloads for itself when you press Install. Flashing it by hand onto a fresh board leaves you with a board that has no bootloader. |

They are separate because the jobs are different. The four pieces of a
first install have four different offsets and one of them has moved
before, so they are merged into a single file that cannot be got wrong.

## Flashing by hand

If you would rather not use the script — a machine with esptool but not
this repository, say:

```sh
esptool.py --chip esp32p4 -p /dev/ttyACM0 -b 921600 \
    --before default_reset --after hard_reset \
    write_flash --flash_mode dio --flash_size 32MB --flash_freq 80m \
    0x0 nn20clock-v0.2.0-full.bin
```

Verify what you downloaded first:

```sh
sha256sum -c SHA256SUMS
```

## Starting completely clean

`--erase` wipes the whole flash before writing, **including every stored
alarm, the Wi-Fi network, and the brightness and volume settings**. It is
worth it only when the board is misbehaving in a way that smells like
stale settings.

```sh
./flash.sh --erase
```

## Rollback

An update that does not reach a running clock is reverted automatically.
The previous firmware is still in the other half of the flash, and the
bootloader returns to it at the next reset unless the new one starts
cleanly. That is why there is no "downgrade" button: the safety net is
automatic, and the manual path is this script.

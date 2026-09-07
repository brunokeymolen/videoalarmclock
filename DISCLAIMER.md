# Disclaimer

This is a hobby project, published because it might be useful to
somebody else. It is offered as is, with no warranty of any kind and no
liability accepted for anything that follows from using it. The binding
version of that is the **No Liability** section of [`LICENSE`](LICENSE);
this file is the plain-language account of what it means in practice.

## It is not a dependable alarm

Nothing here is certified, redundant or tested to any standard. It is a
microcontroller, an SD card and a few thousand lines of hobby firmware,
and any part of that can fail quietly.

**Do not rely on it as your only alarm for anything that matters** — a
flight, an exam, a shift, a dose of medication. Alarms survive a power
cut, which is a design goal rather than a guarantee, and there is no
promise that an alarm will sound, that it will sound at the right time,
or that the clock is showing the right time at all. A clock with no
network runs on its own RTC and drifts.

## Flashing is at your own risk

Writing firmware to a board can go wrong, and the board is yours. The
installer checks a published checksum before it writes, which catches a
corrupted download and nothing else. A failed or interrupted flash can
leave a board that will not boot until it is flashed again, and
`--erase` removes everything on it. No responsibility is taken for a
damaged, bricked or wiped device, whichever port you used.

The firmware is built for ESP32-P4 revisions v1.00–v1.99 and has
actually run on v1.3. Other revisions are untested.

## It is not secure, and does not pretend to be

Two things are documented plainly in the [README](README.md#2-put-it-on-your-wi-fi)
because they are deliberate, not oversights:

- The Wi-Fi passphrase is stored in plain flash. Anyone with the board
  and a cable can read it back.
- The upload server has no username or password. What limits it is that
  it runs only while the Media screen is open.

Put the clock on a network where that is acceptable. No responsibility
is taken for anything reached through it.

## What you play on it is yours

The tools in [`video/`](video/) convert a file you give them. One of
them can also download from YouTube, which may breach YouTube's terms of
service, and the material is usually somebody else's work.

**Deciding what you have the right to use is entirely your
responsibility.** The expectation is ordinary fair use — your own
footage, public-domain film, something you made, something licensed for
it. No responsibility is accepted for what anyone converts, uploads,
stores or plays, or for any infringement, claim or loss arising from it.
The YouTube support is a convenience for demonstrating the tool, not an
invitation to use it that way.

## No affiliation

Not affiliated with, endorsed by or supported by Waveshare, Espressif,
YouTube or Google. Hardware and product names belong to their owners and
are used only to say which parts this runs on.

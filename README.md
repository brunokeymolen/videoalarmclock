<p align="center">
  <img src="img/c.jpg" alt="Video Alarm Clock side view" width="180">
  &nbsp;&nbsp;
  <img src="img/d.jpg" alt="Video Alarm Clock" width="250">
  &nbsp;&nbsp;
  <img src="img/b.jpg" alt="Video Alarm Clock detail view" width="180">
</p>

<h1 align="center">Video Alarm Clock</h1>

<p align="center">
  A bedside clock that wakes you with your own video instead of a beep.
</p>

<p align="center">
  <strong>720×720 touch screen</strong> ·
  <strong>built-in speaker</strong> ·
  <strong>power-cut-safe alarms</strong> ·
  <strong>Wi-Fi updates</strong>
</p>

---

Video Alarm Clock runs on a [Waveshare ESP32-P4 touch display](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-4b.htm) and plays
videos from a microSD card. Flash it once, put it on Wi-Fi, add your
media, and manage alarms from the device itself.

This repository has **the installation files and procedure**. It has the three things you need
to own one:

| Directory | What it gives you |
| --- | --- |
| [`firmware/`](firmware/) | The firmware binaries and one-command installer |
| [`video/`](video/) | A container that converts and uploads video files |
| [`enclosure/`](enclosure/) | The printable bedside stand |

Once the firmware is on, the clock updates itself over Wi-Fi: gear icon
→ **About** → **Check for updates**. You should only need the cable
once.

---

## What you need

**Hardware**

- A **Waveshare ESP32-P4-WIFI6-Touch-LCD-4B** — 720×720 MIPI-DSI panel,
  GT911 touch, ES8311 audio codec, 32 MB flash, 32 MB PSRAM. 
  see: [https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-4b.htm]

  **The board this has been tested on carries an ESP32-P4 revision
  v1.3.** The firmware is built for revisions v1.00 through v1.99, so
  other v1.x parts should work, but v1.3 is the only one it has actually
  run on. If
  you have a different revision and it works — or does not — that is
  worth reporting. A board the firmware refuses says so plainly while
  flashing: *"requires chip revision in range ... this chip is revision
  ..."*.
- A **USB-C cable** to the board's UART port.
- A **microSD card**, formatted **FAT32**. Video lives here, not in
  flash; 32 GB is plenty, 128 GB is generous.

**On your computer**

- Linux or macOS for the firmware install below. Converting video also
  works on **Windows**, with Docker Desktop — step by step in
  [`video/README.md`](video/README.md#on-windows-step-by-step).
- `esptool`, only if you install the firmware from a terminal:
  `pip install --user esptool`. The browser installer in step 1 needs
  nothing at all.
- Docker, if you want the video container. Otherwise `ffmpeg` directly,
  plus `yt-dlp` only if you want the YouTube shortcut — see
  [`video/README.md`](video/README.md).

---

## 1. Install the firmware

Plug the board into your computer by its **UART** port — not the OTG
one. Then either press this:
<br><br>

<div align="center">
  <a href="https://brunokeymolen.github.io/videoalarmclock/">
    <img src="https://img.shields.io/badge/Install%20from%20your%20browser-2ea44f?style=for-the-badge&logo=espressif&logoColor=white" alt="Install the firmware from your browser">
  </a>
</div>

<br><br>
It flashes the board from the page it opens, with nothing to install
first. It needs Chrome, Edge or Firefox 151+ on a desktop — Safari and
iOS have no Web Serial at all and cannot do this.

Or from a terminal, which works everywhere:

```sh
cd firmware
./flash.sh
```

On Linux the board appears as `/dev/ttyACM0`.

That downloads the newest release, checks it against the published
checksum, and writes it. It takes about half a minute and ends with the
board resetting into the clock face.

Pick a specific version, or a specific port, if you need to:

```sh
./flash.sh --version v0.2.0
./flash.sh --port /dev/ttyACM0
```

<details>
<summary>If <code>flash.sh</code> says it cannot open the port</summary>

On Linux the serial port belongs to the `dialout` group:

```sh
sudo usermod -aG dialout "$USER"
```

Log out and back in — group membership does not apply to a session
that already exists.
</details>

<details>
<summary>If it says something else is holding the port</summary>

A serial monitor open in another terminal is the usual cause:

```sh
fuser -v /dev/ttyACM0     # find it
```
</details>

## 2. Put it on your Wi-Fi

On the device: **gear icon → Wi-Fi**. It scans, you pick a network and
type the passphrase.

The clock works without this — it just runs on its own RTC and the time
will be wrong until you set it by hand (**gear → Time**). With a network
it sets itself from NTP within a few seconds, and it is what over-the-air
updates need.

> The passphrase is stored in plain flash on the device. Anyone with
> physical access and a serial cable can read it back. That is the same
> exposure as any ESP32 project that stores Wi-Fi credentials.

## 3. Put video on it

Insert the microSD card and copy video onto it.


**Any video file works** — phone footage, a camcorder transfer, a film
you own, something you made. The clock plays one specific format, so it
has to be converted first, and `videoclock` does that and uploads the
result:

```sh
cd video
./videoclock holiday.mov wakeup.avi 192.168.0.201
```

Downloading from YouTube is a **convenient demo**, not the point of the
tool — the same command takes a URL instead of a filename:

```sh
./videoclock "https://youtu.be/VIDEO_ID" wakeup.avi 192.168.0.201
```

Trim a long source while converting, which you usually want — these
files run about 50 MB per minute:

```sh
START=00:01:30 DURATION=00:00:45 ./videoclock holiday.mov wakeup.avi
```

The clock's address is shown on its **gear → Media** screen, in blue at
the top. **That screen must be open while you upload** — see below.

Without the address it just leaves `wakeup.avi` in the current
directory, and you copy it on the card yourself.

On **Windows** the `videoclock` wrapper does not run; you install Docker
Desktop and call the container directly, which is written out step by
step in
[`video/README.md`](video/README.md#on-windows-step-by-step).

Full details, including running without Docker:
[`video/README.md`](video/README.md).

### The two rules the clock enforces

**Names are 8.3.** Up to eight of `A-Z a-z 0-9 _ -`, then `.avi`. The
firmware builds its filesystem with long filenames switched off, so a
longer name cannot exist on the card at all — an upload would fail, or
land under a mangled name that no longer matches the alarm pointing at
it. The script refuses a bad name before doing any work.

The same limit applies to folder names — see [Folders](#folders).

**The format is fixed.** 720×720 MJPEG video, PCM stereo audio at
44.1 kHz, in an AVI container. There is no other decoder in the
firmware, so this is not a preference — anything else is refused by the
player, which names the codec it was handed.

### Uploads only work while the Media screen is open

The clock runs its FTP server **only while gear → Media is on screen**.
Leave that screen and the server stops, mid-transfer if necessary.

There is no username or password. FTP sends credentials in clear text,
so a password would be theatre rather than protection; what limits the
exposure is time, and opening that screen is a deliberate act at the
device.

## 4. Set an alarm

**Clock face → alarm icon.** Each alarm has a time, the days it repeats,
and which video it plays. Alarms survive a power cut.

Tapping the sound row opens a browser for the card. Besides the videos
themselves it offers the **built-in tone**, and **`<random>`** — a
different video every time the alarm rings. See [Folders](#folders) for how
browsing works and what `<random>` draws from.

If the video an alarm points at has been deleted, or the folder it draws
from is empty, **the alarm still rings** — it falls back to the built-in
tone. An alarm never goes silent.

## 5. Print the stand

[`enclosure/`](enclosure/) has the STL and the OpenSCAD source it was
generated from. It holds the board at a 15° tilt for a bedside table.

---

## Folders

Everything about organising the card in folders. Skip it if you have a
handful of videos — the card's top level works fine on its own.

The top level of the card is the root folder, and folders can nest:

```text
/                 the root folder
/MORNING          a folder
/WEEKEND/KIDS     a nested folder
```

**Folder names follow the same 8.3 rule as video names** — up to eight
of `A-Z a-z 0-9 _ -`, and no extension. `morning` works, `weekend films`
does not. Names come back in upper case, so a folder you make as
`morning` shows up as `MORNING`.

### Browsing on the clock

The alarm sound picker and **gear → Play** both browse one folder at a
time. Folders are the filled, coloured rows with a folder icon; videos
are the plain ones. Tap a folder to go into it, tap `..` at the top to
come back out.

In the alarm picker, the arrow in the corner returns to the alarm — it
does not go up a level, so you cannot leave the screen by accident while
browsing.

### `<random>`

A different video every time, drawn from **the folder you are standing
in** and only that folder. `<random>` inside `MORNING` never reaches into
`MORNING/KIDS`. It appears once there are at least two videos in the
folder to choose between.

It means something slightly different in the two places it appears:

| Where | What it does |
| --- | --- |
| **Alarm sound picker** | A different video each morning. The alarm row afterwards reads `MORNING / <random>`, or `Root / <random>` for the top level, so you can see which folder it draws from without opening the picker. |
| **gear → Play** | Keeps drawing a new video every time one ends, until the sleep timer stops it. Set the sleep timer first — the two sit next to each other because they are one decision. See [The sleep timer](#the-sleep-timer). |

### Openers: the videos that go first

Sometimes a folder is not quite a shuffle. There is a title card that
should open the evening, or three parts of one holiday that only make
sense in order, and everything else in the folder can come in any order
at all.

**Put a number on the front of the file name.** `<random>` plays the
numbered videos first, in the order of their numbers, and only then
starts shuffling the rest.

```
1INTRO.AVI      plays first
2HOLIDAY.AVI    then this
15SUMMER.AVI    then this
AFRICA.AVI      and now it shuffles: these three, in no order,
COFFEE.AVI      for as long as playback goes on
MIDSLEEP.AVI
```

The rules are short:

| | |
| --- | --- |
| **The number goes at the front** | `1MYSTUFF.AVI` is an opener. `PART2.AVI` is not — that 2 is part of the name, not a position. |
| **It is a number, not text** | `9` plays before `15`. Sorting by name would have put 15 first. |
| **`01` is the same as `1`** | Pad the numbers if you like them lining up on your PC. The clock does not care. |
| **Openers open, once** | Once the numbered ones have played, the shuffle starts and does not go back to them. A title card in the middle of the evening is not a title card. |
| **Every `<random>` starts at the top** | Each morning the alarm rings, and each time you press **Play**, the openers run again from the first one. |

A folder where every video is numbered is simply a playlist. There is
nothing left to shuffle, so it plays 1, 2, 3 and then starts again at 1,
for as long as playback goes on. A folder with openers and only one
other video alternates between them, rather than repeating that one
video all night.

None of this needs setting up on the clock. Rename the file on your PC,
or upload it under a numbered name, and `<random>` picks it up the next
time it draws. Renaming is easiest over FTP — see
[Managing folders with FileZilla](#managing-folders-with-filezilla).

### The sleep timer

**gear → Play** has a sleep timer above the list, and it decides how
long playback runs — not just when to cut it short.

| Setting | What happens |
| --- | --- |
| **whole video** | Plays the video once and stops. The default. |
| **15 min**, 30, 60, 90 | Plays for that long, then stops wherever it has got to. |

A duration means exactly that. A 90-minute film with **15 min** set stops
a quarter of an hour in. A three-minute clip with **30 min** set plays
again and again until the half hour is up — and if you picked
`<random>`, it draws a different video each time instead of repeating
the same one. Pair `<random>` with a timer and you get an evening's
worth without choosing anything else.

It applies to a video you picked as well as to `<random>`; the only
setting that plays something once is **whole video**.

### Uploading straight into a folder

`videoclock` puts the converted video wherever you say, creating the
folder if it is not there — so there is no upload-then-move step.
`FOLDER=` goes in front of the command, the same way `START=` and
`DURATION=` do.

**Linux and macOS**, from a video file:

```sh
FOLDER=MORNING ./videoclock holiday.mov wakeup.avi 192.168.0.201
```

and from a YouTube URL:

```sh
FOLDER=MORNING ./videoclock "https://youtu.be/VIDEO_ID" wakeup.avi 192.168.0.201
```

**Windows**, where `videoclock` does not run and you call the container
directly — `FOLDER` becomes another `-e`, next to the ones already
there. From a video file:

```powershell
docker run --rm -i -e HOME=/tmp -e FOLDER=MORNING -v "${PWD}:/out" videoalarmclock-video file2clock.sh holiday.mov wakeup.avi 192.168.0.201
```

and from a YouTube URL:

```powershell
docker run --rm -i -e HOME=/tmp -e FOLDER=MORNING -v "${PWD}:/out" videoalarmclock-video yt2clock.sh "https://youtu.be/VIDEO_ID" wakeup.avi 192.168.0.201
```

It combines with the trim, which is usually what you want on a long
source:

```sh
START=00:01:30 DURATION=00:00:45 FOLDER=MORNING \
    ./videoclock holiday.mov wakeup.avi 192.168.0.201
```

Nested folders work — `FOLDER=WEEKEND/KIDS` — and each level is created
as needed. Folder names follow the 8.3 rule above, and the script checks
yours before it spends ten minutes converting.

Leave `FOLDER` out and the video lands at the top level of the card, as
it always did. **The Media screen must be open on the clock either way.**

### Managing folders with FileZilla

If you would rather drag files than type commands, any FTP client works
while the Media screen is open. [FileZilla](https://filezilla-project.org/)
is the one this was tested with, and it is free on Windows, macOS and
Linux.

Connect with **File → Site Manager → New Site**:

| | |
| --- | --- |
| Protocol | FTP |
| Host | the clock's address, from **gear → Media** |
| Port | 21 |
| Encryption | *Only use plain FTP (insecure)* |
| Logon Type | Anonymous |

There is no username or password, by design — see
[Uploads only work while the Media screen is open](#uploads-only-work-while-the-media-screen-is-open).
Once connected you get the card in the right-hand pane and can make
folders, rename, drag videos in and out, and delete, all by hand.

Three things behave differently from an ordinary FTP server, and none of
them is a fault:

- **The Media screen must stay open.** Leave it and the server stops,
  mid-transfer if one is running. FileZilla will report a lost
  connection.
- **Deleting a folder only works when it is empty.** FileZilla offers to
  delete a folder and everything in it; the clock refuses the folder
  part until you have emptied it.
- **Dragging onto a name that already exists fails** rather than
  overwriting. Delete the old one first, or pick another name.

FileZilla's raw-command box — **Server → Enter custom command** — is
also how you send `SITE HIDE`, further down.

### Making and moving folders over FTP

If you prefer the command line, or want to script it. Any FTP client
will do this while the Media screen is open:

```sh
CLOCK=192.168.0.201

curl -Q "MKD MORNING" ftp://$CLOCK/          # make a folder
curl -T wakeup.avi ftp://$CLOCK/MORNING/     # upload into it
curl ftp://$CLOCK/MORNING/                   # see what is in it
curl -Q "RMD MORNING" ftp://$CLOCK/          # remove it, if empty
```

Moving a video that is already on the card — instant, whatever its size,
because nothing is copied:

```sh
curl -Q "-RNFR wakeup.avi" -Q "-RNTO MORNING/wakeup.avi" ftp://$CLOCK/
```

Two things the clock will not do, on purpose:

- **It will not delete a folder that still has anything in it.** Empty
  it first. One mistyped path should not clear your card.
- **It will not overwrite when renaming or moving.** If something is
  already there under that name, the move is refused — the card holds
  the only copy of your videos.

Folders can nest, but keep it shallow: the clock shows one folder at a
time on a small screen.

**If you move a video, fix the alarm that uses it.** An alarm remembers
where a video is, not just its name, so an alarm pointing at
`WAKEUP.AVI` does not follow it into `MORNING/`. It falls back to the
built-in tone rather than going silent, but you will want to set it
again.

### Advanced: hiding things you do not want to see

Plug the card into a PC or a Mac and it comes back with folders you
never made — a trash can, a search index. The clock hides the ones it
knows about (`TRASH-~1`, `SYSTEM~1`, `RECYCL~1`, and a few more) so they
do not clutter the picker.

For anything it does not know about, there is a command:

```sh
curl -Q "SITE HIDE TRASH-~1" ftp://$CLOCK/     # stop listing it
curl -Q "SITE UNHIDE TRASH-~1" ftp://$CLOCK/   # list it again
```

This sets the FAT *hidden* attribute, the same one Windows uses, so it
sticks and your PC will respect it too.

`SITE` is FTP's slot for commands a particular server invents — there is
no standard way to change a file attribute over FTP — so this is the
clock's own. Clients with a raw-command box can send it just as well as
`curl` — in FileZilla that is **Server → Enter custom command**, with
the card open.

**Hidden means not listed, not locked.** A hidden file can still be
downloaded, renamed and deleted by name; the clock simply stops offering
it. That is also how you undo it if you hide the wrong thing.

The clock never hides anything on its own except the known names above,
and it will not hide a folder just because its name looks mangled: `~1`
is what FAT does to *any* long name, so a folder you created from your
PC as "Morning Films" arrives as `MORNIN~1` and looks exactly like junk.
Hiding those would lose your own work.

## Updating later

**Gear icon → About → Check for updates.** If there is a newer version
the button becomes **Install**; a progress bar runs for about ten
seconds, and then **Restart now** puts you on it.

Nothing polls and nothing updates by itself. Both steps are a button you
press, because a clock that reboots into different software while its
owner is asleep is a worse clock.

An update that fails to start properly is rolled back automatically — the
previous version is still in the other half of the flash, and the
bootloader returns to it if the new one does not reach a running clock.

You should never need the USB cable again. If you ever do — a board that
will not boot at all — `firmware/flash.sh` still works, and
`--erase` clears everything including alarms and Wi-Fi.

---

## When something is wrong

| Symptom | Cause | Fix |
| --- | --- | --- |
| `flash.sh`: no serial port found | Board not attached, or attached by the wrong port | Use the **UART** port, not the OTG one. `ls /dev/ttyACM* /dev/ttyUSB*` |
| `flash.sh`: cannot open the port | Not in `dialout` | `sudo usermod -aG dialout "$USER"`, then log out and back in |
| `flash.sh`: esptool is not installed | — | `pip install --user esptool` |
| Screen stays black after flashing | Board is not powered enough, or the panel ribbon is loose | Try a different USB port or a powered hub |
| Time is wrong and stays wrong | No network, so no NTP | gear → Wi-Fi. Or set it by hand: gear → Time |
| Upload refused / connection times out | The Media screen is not open | On the device: gear → Media, and leave it open |
| The clock refuses a video | Not 720×720 MJPEG + PCM in AVI | Convert it with `video/videoclock`; the error names the codec it got |
| No media listed, card is fine | The card is empty, or the videos are in a folder | Tap a folder row to go into it; `..` goes back out |
| A folder cannot be created | The name is longer than eight characters | Rename it — folder names follow the same 8.3 rule as videos |
| A folder will not delete | It still has something in it | Delete what is inside first; the clock refuses to delete a folder recursively |
| A move or rename is refused | Something already has that name | The clock never overwrites; pick another name, or delete the other one first |
| FileZilla: "could not connect" or a dropped transfer | The Media screen is not open, or was left during the transfer | gear → Media, and leave it open. Use plain FTP, anonymous — the clock has no TLS and no login |
| An alarm plays the tone instead of its video | Its video was deleted, moved, or its folder is empty | Set the sound again: alarm → sound row |
| A folder is on the card but the clock does not list it | It is hidden, or it is one of the desktop leftovers the clock skips | [Folders → Advanced](#advanced-hiding-things-you-do-not-want-to-see); `SITE UNHIDE` brings it back |
| About: "Could not reach the update server" | No network | gear → Wi-Fi |
| About: "The update server answered with nonsense" | No release published yet | Check the [releases page](../../releases) |
| An update installs, then the old version is back | The new firmware did not start cleanly and was rolled back | Report it — that is a bug worth hearing about |

---

## Licence and copyright

© 2026 Bruno Keymolen. The repository is under two licences.

**The stand is free, with no conditions at all.** [`enclosure/`](enclosure/)
— the STL and the OpenSCAD source — is public domain under CC0 1.0
([`enclosure/LICENSE`](enclosure/LICENSE)). Print it, change it, sell the
prints. No permission and no credit needed.

**Everything else is free for noncommercial use.** The firmware, the
scripts and the documentation are under the custom noncommercial terms in
[LICENSE](LICENSE). Install it, experiment, use it at home, change it,
pass it on — all of that is permitted. Selling
it, selling clocks with it on them, or running a business on it needs a
written agreement first. Ask, by opening an issue; the answer is not
automatically no.

**There is no warranty, for the clock or for what you play on it.** It
is a hobby project: do not rely on it as your only alarm for anything
that matters, and deciding what you have the right to convert and play
is your responsibility, not the author's. [`DISCLAIMER.md`](DISCLAIMER.md)
sets that out properly and is worth the two minutes.

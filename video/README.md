# Making videos for the Video Alarm Clock

Convert a video to the one format the clock plays, and upload it — in
one command.

**Any video file works.** Phone footage, a camcorder transfer, a DVD
rip, a film you own, something you made. That is the general case.

Fetching from YouTube is a **convenient demo** layered on top of it, not
what this is for: the same command takes a URL instead of a filename,
and everything after the download is identical.

## With Docker (recommended)

Nothing to install but Docker itself. `videoclock` builds the image the
first time and runs everything inside it.

```sh
./videoclock <video-file|youtube-url> <name.avi> [clock-ip]

./videoclock holiday.mov          wakeup.avi
./videoclock ~/Videos/sunrise.mp4 sunrise.avi 192.168.0.201
./videoclock "https://youtu.be/PIb6AZdTr-A" clg308.avi 192.168.0.201
```

Which of the two it runs is decided by whether the first argument names
a file that exists — a file always wins, so a mistyped path is reported
as one instead of being handed to yt-dlp to fail obscurely. The source
may live anywhere; a directory outside the current one is mounted into
the container read-only.

Without the third argument it leaves the `.avi` in the current
directory; copy it to `/` (root) on the SD card yourself.

### Trimming

These files run about **65 MB per minute**, so taking a slice of a long
source is usually what you want. `START` and `DURATION` are passed to
ffmpeg *before* it opens the file, so it seeks rather than decoding
everything and throwing most of it away:

```sh
START=00:01:30 DURATION=00:00:45 ./videoclock holiday.mov wakeup.avi
```

Either can be given alone: `START` on its own runs to the end,
`DURATION` on its own takes from the beginning.

### Keeping the YouTube half working

YouTube changes how it hands out download URLs often enough that a
yt-dlp more than a few weeks old stops working — and it fails with an
extraction error that reads like the video was taken down rather than
like a stale tool. When a download suddenly fails, this is the first
thing to try:

```sh
./videoclock --update
```

That rebuilds the image without its cache, which picks up a current
yt-dlp and a current Deno. It has no bearing on converting a local file,
which does not use yt-dlp at all.

If the rebuild does not fix it, check whether yt-dlp was actually out of
date before assuming it: the failure below — a missing or too-old
JavaScript runtime — wears the same costume, and an already-current
yt-dlp cannot be cured by updating it.

## On Windows, step by step

`videoclock` is a shell script, and Windows has no shell to run it in.
So on Windows you run the container directly: the same image, the same
two scripts inside it, typed out instead of wrapped.

**Docker Desktop is the only thing you install.** No WSL distribution to
pick, no ffmpeg, no yt-dlp, no Python — all of that lives inside the
container. Everything below is typed into **PowerShell**.

### Step 1 — Install Docker Desktop

Skip this if `docker version` already answers.

1. Download **Docker Desktop for Windows** from
   <https://www.docker.com/products/docker-desktop/> and run the
   installer.
2. Leave **Use WSL 2 instead of Hyper-V** ticked. The installer sets that
   backend up by itself; you do not have to install Ubuntu or any other
   Linux.
3. Reboot when it asks. It will ask.
4. Start **Docker Desktop** from the Start menu and wait until its window
   reads **Engine running** at the bottom left.

Docker Desktop has to be running *every* time you convert a video —
the commands below fail with `error during connect` if it is not. To stop
having to remember: Settings → General → **Start Docker Desktop when you
sign in**.

### Step 2 — Get this repository

On the repository page: green **Code** button → **Download ZIP**, then
right-click the downloaded file → **Extract All**. The folder you want is
`video`.

With git installed, `git clone <repo-url>` does the same thing.

### Step 3 — Build the image, once

Open the `video` folder in File Explorer, then right-click on empty space
→ **Open in Terminal** (Windows 11), or hold **Shift** and right-click →
**Open PowerShell window here** (Windows 10). The prompt should end in
`\video>`.

```powershell
docker build -t videoalarmclock-video .
```

That takes a few minutes and downloads a few hundred MB. It is also the
only step that has to happen in the `video` folder: afterwards the image
is on your machine and you convert from wherever your videos are.

### Step 4 — Convert a file you already have

Open the folder that holds the video in File Explorer and start a
terminal there the same way as in step 3 — the conversion can only see
the folder you are standing in.

```powershell
docker run --rm -i -e HOME=/tmp -v "${PWD}:/out" videoalarmclock-video file2clock.sh holiday.mov wakeup.avi
```

`holiday.mov` is your file. `wakeup.avi` is the name the clock will list
it under, and it must be **8.3** — up to eight of `A-Z a-z 0-9 _ -`, then
`.avi`. The script checks that before doing any work, because a longer
name cannot exist on the clock's card at all.

`wakeup.avi` appears in that same folder when the encode finishes.

To convert *and* upload in one go, put the clock's address at the end —
but read step 7 first, because the clock only accepts uploads while one
particular screen is open:

```powershell
docker run --rm -i -e HOME=/tmp -v "${PWD}:/out" videoalarmclock-video file2clock.sh holiday.mov wakeup.avi 192.168.0.201
```

### Step 5 — Or fetch from YouTube

The same command with `yt2clock.sh` instead of `file2clock.sh`, and a URL
instead of a filename. Nothing has to be in the folder beforehand; the
`.avi` still lands there.

```powershell
docker run --rm -i -e HOME=/tmp -v "${PWD}:/out" videoalarmclock-video yt2clock.sh "https://youtu.be/VIDEO_ID" wakeup.avi
```

**Keep the quotes around the URL.** A YouTube link copied from the
address bar usually contains `&`, and PowerShell treats a bare `&` as
punctuation of its own and refuses the whole line.

If a download fails with something that reads like the video has been
taken down, the usual cause is a stale yt-dlp rather than a missing
video. Rebuild in the `video` folder to pick up a current one — this is
the Windows spelling of `./videoclock --update`:

```powershell
docker build --no-cache -t videoalarmclock-video .
```

### Step 6 — Trim, because these files are large

720×720 MJPEG runs about **65 MB per minute**, so a four-minute music
video is roughly 260 MB. Two extra `-e` options cut the source down, and
they work for both scripts:

```powershell
docker run --rm -i -e HOME=/tmp -e START=00:01:30 -e DURATION=00:00:45 -v "${PWD}:/out" videoalarmclock-video file2clock.sh holiday.mov wakeup.avi
```

Either can be given alone: `START` on its own runs to the end of the
video, `DURATION` on its own takes from the beginning.

### Step 7 — Get it onto the clock

**Over the network.** On the device, open **gear icon → Media** and leave
it there. The address to use is in blue at the top of that screen. Then
run the command from step 4 or 5 with that address as the last argument.
The clock runs its FTP server *only* while that screen is showing, so
leaving it stops the transfer mid-file.

The first upload may raise a Windows Defender Firewall prompt for Docker
Desktop — allow it on **private** networks. Nothing has to be opened
inbound: the upload uses passive FTP, so the container makes both
connections outward to the clock.

**Or by hand.** Put the microSD card in your PC, make a folder called
`clock` at the top level of the card if it is not there already, and copy
the `.avi` into it. Flat — the clock does not look in subdirectories.

### The parts of that command

Worth knowing if you want to change something:

| | |
| --- | --- |
| `--rm` | delete the container when it exits; the `.avi` is not inside it |
| `-i` | keep input attached, so you see ffmpeg's progress |
| `-e HOME=/tmp` | yt-dlp writes a cache to `$HOME`, which does not exist in the container otherwise |
| `-v "${PWD}:/out"` | show the container the folder you are standing in. This is how your video gets in and the `.avi` gets out |
| `videoalarmclock-video` | the image built in step 3 |
| `file2clock.sh` / `yt2clock.sh` | which of the two jobs to do |

The quotes around `"${PWD}:/out"` matter — without them a path
containing a space breaks the option in half. In **Command Prompt**
rather than PowerShell, that part is `-v "%cd%:/out"` instead.

The Linux wrapper also passes `--user`, which is left out here on
purpose: it exists so the `.avi` does not come back owned by root, and
Windows bind mounts do not carry Linux ownership in the first place.

### When something goes wrong on Windows

| What you see | Cause | Fix |
| --- | --- | --- |
| `docker : The term 'docker' is not recognized` | Docker Desktop is not installed, or PowerShell was open before it was | Install it; close and reopen PowerShell |
| `error during connect ... docker_engine` | Docker Desktop is installed but not running | Start it from the Start menu, wait for **Engine running** |
| `docker: invalid reference format` | The quotes around `"${PWD}:/out"` were dropped, or an option was mistyped | Re-paste the whole line |
| `'wakeup.avi' is not an 8.3 name` | The name is too long, or has characters outside `A-Z a-z 0-9 _ -` | Rename it: eight characters at most, then `.avi` |
| `'wakeup.avi' already exists here` | A previous run left one; the script refuses to overwrite | Delete it or pick another name |
| `'holiday.mov' is not a file` | The video is not in the folder the terminal is standing in | Start the terminal in the folder that holds the video |
| `upload failed - is the Media screen open?` | It is not, or it was closed mid-transfer | On the device: gear → Media, leave it open, run it again |
| A YouTube download fails as if the video were gone | Stale yt-dlp, or a stale Deno inside the image | `docker build --no-cache -t videoalarmclock-video .` in the `video` folder, which refreshes both |

## Without Docker

The two scripts the container runs work directly if you install the
tools yourself. They take the same arguments as `videoclock`.

```sh
./file2clock.sh holiday.mov clip.avi 192.168.0.201     # needs ffmpeg
./yt2clock.sh "https://youtu.be/PIb6AZdTr-A" clg308.avi # + yt-dlp
```

`file2clock.sh` needs only **ffmpeg**. `yt2clock.sh` additionally needs
**yt-dlp** and a JavaScript runtime.

---

## Installing the tools, if you are not using Docker

### ffmpeg — needed for everything

Debian / Ubuntu:

```sh
sudo apt update && sudo apt install -y ffmpeg
```

Fedora: `sudo dnf install ffmpeg` · Arch: `sudo pacman -S ffmpeg` ·
macOS: `brew install ffmpeg`

Check it: `ffmpeg -version`

### yt-dlp — only for the YouTube shortcut

Skip this if you are converting files you already have.

**Do not install this from apt.** The packaged version is usually months
old, and YouTube changes often enough that a stale yt-dlp simply stops
working — normally with an extraction error that looks like a broken
video rather than a broken tool.

Take the official binary instead:

```sh
sudo curl -L https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp \
    -o /usr/local/bin/yt-dlp
sudo chmod a+rx /usr/local/bin/yt-dlp
```

macOS: `brew install yt-dlp`

Check it: `yt-dlp --version` (the version *is* a date, e.g. `2025.08.11`)

**Keep it updated.** When a download suddenly fails, this is the first
thing to try:

```sh
sudo yt-dlp -U
```

### A JavaScript runtime — again, only for YouTube

YouTube computes its download URLs in JavaScript, so yt-dlp has to run
some. It enables **Deno** by default and nothing else.

Without one you get a warning about runtimes followed by:

```
ERROR: [youtube] <id>: This video is not available
```

which reads like the video has been taken down rather than like a
missing dependency. It is not the video.

**The version matters, and this is the trap.** yt-dlp does not accept
just any runtime — it accepts these, and discards anything older
without treating it as a runtime at all:

| Runtime | Minimum |
| --- | --- |
| Deno | 2.3.0 |
| Node | 22.0.0 |
| Bun | 1.2.11 |

Debian and Ubuntu's `nodejs` package is **v18**, four major versions
below the line. Installing it does not help and is worse than having
nothing, because the failure is identical to the one above — a warning
about runtimes, then `This video is not available` — while `node` is
plainly right there on your PATH.

The script checks the version as well as the name, and says which
runtime it found and what would be new enough, rather than letting
YouTube take the blame.

If you have none, or only an old one:

```sh
curl -fsSL https://deno.land/install.sh | sh
```

Deno is the one yt-dlp enables without a flag, so it is the least
surprising choice. A current Node works too — `nvm install --lts`, which
is comfortably past 22.

**This is a moving target.** YouTube changes how it does this, and
yt-dlp follows; something that worked last week can stop without
anything on your machine changing. When that happens the order is:
`sudo yt-dlp -U` first, then check you still have a runtime.

The script honours `YTDLP` and `FFMPEG` if your binaries live elsewhere:

```sh
YTDLP=~/bin/yt-dlp ./yt2clock.sh "https://youtu.be/..." clip.avi
```

---

## The rules the script enforces, and why

**The name must be 8.3** — up to eight of `A-Z a-z 0-9 _ -`, then
`.avi`. The firmware builds FATFS with long filenames disabled, so a
longer name cannot exist on the card at all. An upload would either fail
or land under a mangled name that no longer matches the alarm pointing
at it, which is a bad thing to discover at 7am.

**The format is fixed.** 720×720 MJPEG video, PCM stereo audio at
44.1 kHz, in an AVI container. There is no other decoder in the
firmware, so this is not a preference — anything else will be refused by
the player with a message naming the codec it was handed.

**The frame is cropped, not letterboxed.**
`force_original_aspect_ratio=increase` followed by `crop=720:720` fills
the square panel and cuts the overflow. Letterboxing a 16:9 video would
put black bars on a clock face.

---

## Two things about size and speed

**These files are large.** 720×720 MJPEG at `-q:v 5` measures about
**55 KB per frame**. The scripts encode at 20 fps, so that is roughly
1.1 MB/s — about **65 MB per minute**, or 260 MB for a four-minute music
video. Fine on a 128 GB card, but not what people expect from a "video
file", and the reason `START` and `DURATION` exist.

(Real footage varies either side of that: a still, dark scene compresses
far better than a moving, detailed one. It is a per-frame cost, so
halving the frame rate halves the file.)

**There is headroom.** Playback reads about 1.1 MB/s at these settings,
against roughly **7.1 MB/s** from the card — a margin of about 6x. So
raising the quality (a lower `-q:v`) is reasonable.

The binding limit is not the card but the player thread, where decoding
and the audio write happen one after the other. If you push the settings
and playback starts to stutter, that is what you have run into; step
back down.

---

## Uploading

The third argument uploads over FTP. **The clock only listens while its
media screen is open** — on the device: gear icon → **Media**. Open it
before you run the script and leave it open until the transfer finishes;
closing that screen stops the server and cuts the upload.

That screen also shows the address to use, in blue at the top, and the
connection appears in its Connections list while the transfer runs.

There is no username or password. FTP sends credentials in clear text,
so a password would be theatre; what limits the exposure is that the
server is only up while somebody is standing at the clock with that
screen open.

By hand, without the script:

```sh
curl -T clip.avi ftp://<clock-ip>/          # upload
curl ftp://<clock-ip>/                      # list
curl -Q "DELE clip.avi" ftp://<clock-ip>/   # delete
```

---

## The ffmpeg line, if you want to run it yourself

`file2clock.sh` is a wrapper around exactly this, plus the name check
and the upload. On a machine with neither Docker nor these scripts, this
is the whole recipe:

```sh
ffmpeg -i input.mp4 \
  -vf "scale=720:720:force_original_aspect_ratio=increase,crop=720:720" \
  -c:v mjpeg -q:v 5 -r 20 \
  -c:a pcm_s16le -ar 44100 -ac 2 \
  clip720.avi
```

To take only part of a long video — worth doing, given the size — put
`-ss` (start) and `-t` (duration) **before** `-i`, which is what
`START` and `DURATION` do:

```sh
ffmpeg -ss 00:01:30 -t 00:02:00 -i input.mp4 \
  -vf "scale=720:720:force_original_aspect_ratio=increase,crop=720:720" \
  -c:v mjpeg -q:v 5 -r 20 \
  -c:a pcm_s16le -ar 44100 -ac 2 \
  clip720.avi
```

---

## Copyright

Downloading from YouTube may breach its terms of service, and the videos
are usually somebody else's work. This script is for material you have
the right to use — your own footage, public-domain film, or anything
licensed for it. What you point it at is your call.

No responsibility is taken for what anyone converts or plays — see
[`DISCLAIMER.md`](../DISCLAIMER.md). These scripts are under the
noncommercial terms in [`LICENSE`](../LICENSE).

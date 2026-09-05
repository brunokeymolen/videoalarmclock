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
directory; copy it to `/clock` on the SD card yourself.

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
yt-dlp. It has no bearing on converting a local file, which does not use
yt-dlp at all.

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

The script detects Deno, Node or Bun and passes the right flag, so any
of the three will do. If you have none:

```sh
curl -fsSL https://deno.land/install.sh | sh
```

Deno is the one yt-dlp supports without a flag, so it is the least
surprising choice. Node works too — `nvm install --lts` if you use nvm.

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

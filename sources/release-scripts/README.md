# Cutting a release

Two scripts, run in order. The first builds and stages; the second
publishes. They are separate because publishing is public and permanent
and building is neither, and because the gap between them is where a
release gets tested.

```sh
sources/release-scripts/prepare-release.sh v1.2.0 "What changed, in one line"
sources/tools/flash.sh --image dist/v1.2.0/assets/nn20clock-v1.2.0-full.bin
sources/release-scripts/publish-release.sh v1.2.0
```

Run them from anywhere; both find the repository themselves.

## What a release is

Two things reach a clock, and they are not interchangeable:

| | Goes | Why not the other way |
| --- | --- | --- |
| `latest.json` | **committed** at the top of this repository | The clock reads it from the branch at `CONFIG_NN20CLOCK_OTA_MANIFEST_URL`. As a release asset no clock could find it. |
| `nn20clock-<tag>.bin`, `-full.bin`, `SHA256SUMS` | **uploaded** as GitHub release assets | Binaries in git stay in its history forever. `firmware/flash.sh` downloads them from the release. |

Which repository, which branch and which URL are not written down in
these scripts: they are read back out of
`CONFIG_NN20CLOCK_OTA_MANIFEST_URL` in the build being released, and
checked against this checkout's `origin`. A release is always published
where that firmware actually looks.

## `prepare-release.sh <version> ["one line of notes"]`

Tags `HEAD` if the tag does not exist yet, builds in its own
`build-release/` directory, and stages `sources/dist/<tag>/`. It
publishes nothing. It refuses:

- a version that is not `vMAJOR.MINOR.PATCH` — the firmware parses that
  and treats anything else as a release that cannot name itself;
- **any** uncommitted or untracked file, anywhere in the repository —
  the installers and the video tooling included. Modified files make the
  image differ from the tag; untracked ones are in the image but not in
  the tag at all, and nothing afterwards reveals it;
- a tag that exists somewhere other than `HEAD`;
- an image whose stamped version is not the tag — read back out of the
  binary's application descriptor, not taken on trust;
- **a build that carries Wi-Fi credentials.** They used to reach the
  image through a git-ignored `sdkconfig.defaults.local`, as `.rodata`
  that `grep` finds; the mechanism was removed on 2026-09-06 and the
  network is chosen on the device. This is now a tripwire on the config
  the release was built from, kept because that failure is silent and a
  published asset cannot be taken back;
- an application too large for the `ota_0` slot in `partitions.csv`.

```
sources/dist/v1.2.0/
├── README.md      the same instructions, with this tag filled in
├── release.json   what publish-release.sh reads
├── latest.json    COMMIT at the top of this repository
└── assets/        UPLOAD as release assets
    ├── nn20clock-v1.2.0.bin        the app alone, over the air
    ├── nn20clock-v1.2.0-full.bin   everything merged, offset 0
    └── SHA256SUMS
```

`sources/dist/` is git-ignored. `--reuse-build` skips the compile when
`build-release/` already holds an image stamped with this tag — for
re-staging after a failed publish, not for a normal release.

**Between the two scripts is the moment to put `-full.bin` on a board
and watch it run**, which is the whole reason they are two scripts:

```sh
sources/tools/flash.sh --image dist/v1.2.0/assets/nn20clock-v1.2.0-full.bin
```

## `publish-release.sh <version>`

1. Commits `latest.json` at the top of the repository and pushes it.
2. Pushes the tag.
3. Creates the GitHub release **on that tag** and uploads the two images
   and `SHA256SUMS`.
4. Downloads the published image and checks its SHA-256 against the
   manifest — the only step that proves a clock could complete this
   update.

Before touching anything it checks that the staged manifest, the staged
image and the version stamped inside that image all agree; that the tag
exists, points at the commit the release was built from, and is an
ancestor of `HEAD`; that this is the right repository, on the right
branch, clean, and level with `origin`; and that `gh` is logged in.

| Option | |
| --- | --- |
| `--dry-run` | print the plan and stop |
| `--yes` | do not ask |
| `--allow-private` | publish although the repository is private |
| `--no-verify` | skip step 4 |

### The order, and what it costs

The source and the manifest are in the same repository now, and that
settles the order rather than leaving it to taste:

- the image is stamped from `git describe`, so **the tag exists before
  the build**;
- the manifest carries the SHA-256 of that image, so **it is written
  after it**.

So the tag sits on the commit the firmware was built from, and the
`latest.json` commit lands one commit later, touching nothing that
compiles. `git checkout <tag>` rebuilds this exact image; what it gives
you is the *previous* release's `latest.json`, which is a file the build
does not read. The alternative — moving the tag onto the manifest commit
— buys a tidier `git checkout` and risks moving a tag somebody has
already fetched. It was not taken.

The tag is pushed **before** `gh release create`, because that command
hangs the release on a tag that already exists and invents one at the
default branch if it does not.

### The window

Committing the manifest first means that from the push until the assets
finish uploading, the manifest names an image that 404s. It is reachable
only by someone pressing **Install** in that minute; the firmware reports
a failed download and stays on the version it has. Step 4 is what says
the window has closed.

### The repository has to be public

The clock fetches the manifest and the image anonymously — it has no
GitHub credentials and no way to acquire any — so both answer 404 while
the repository is private, and the About screen reports that the update
server answered with nonsense. Everything else about the release
succeeds, which is what makes it worth failing on:

```sh
gh repo edit brunokeymolen/videoalarmclock \
    --visibility public --accept-visibility-change-consequences
```

`--allow-private` stages the release anyway, for flipping visibility
afterwards. It skips step 4, because there is nothing anonymous to
check.

### If it fails halfway

Re-run it. A manifest that is already committed and pushed is detected
and skipped, a tag already on `origin` is checked rather than pushed
again, and an existing release has its assets uploaded with `--clobber`
rather than being recreated. What cannot be undone from here is a pushed
commit, a pushed tag or a published release; all three are public the
moment they exist.

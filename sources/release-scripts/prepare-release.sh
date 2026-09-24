#!/usr/bin/env bash
#
# prepare-release.sh - build a version and stage everything a release
# needs. Publishes nothing.
#
#   sources/release-scripts/prepare-release.sh v0.2.0 "Over-the-air updates"
#   sources/release-scripts/publish-release.sh v0.2.0    # the other half
#
# The source lives in sources/ and the installers live at the top of
# this same repository, so a release no longer crosses from a private
# tree to a public one: it is built here and published here. What a
# release still is, exactly, is three files staged in
# sources/dist/<tag>/:
#
#   latest.json          COMMITTED at the top of this repository. The
#                        clock reads it at
#                        CONFIG_NN20CLOCK_OTA_MANIFEST_URL and nothing
#                        else; it carries the SHA-256 that decides
#                        whether an image is installed.
#   assets/*.bin         UPLOADED as GitHub release assets. Binaries
#                        never go in the repository - git keeps them
#                        forever, and firmware/flash.sh downloads them
#                        from the release.
#   assets/SHA256SUMS    so a person flashing by hand can check a
#                        download without the manifest, which is for
#                        the device rather than for them.
#
# THE THREE THINGS IT REFUSES TO GET WRONG
#
# 1. A version that is not what was built. PROJECT_VER comes from `git
#    describe`, so a dirty tree produces an image stamped "-dirty" that
#    claims to be a release and then fails its own comparison. The
#    version stamped into the image is read back out of it and checked
#    against the tag, after the build, from the bytes.
#
# 2. Wi-Fi credentials in a public binary. They used to reach the image
#    through a git-ignored sdkconfig.defaults.local, as .rodata that
#    grep finds; that mechanism was removed on 2026-09-06 and the
#    network is chosen on the device instead. What is left here is a
#    tripwire: the release build's own generated config is checked for
#    any credential option that has come back, because the cost of
#    being wrong is a public asset that cannot be unpublished.
#
# 3. Publishing where the firmware is not looking. The owner, the
#    repository and the image URL are all derived from the manifest URL
#    compiled into this very build, not from a constant here - and the
#    repository it names is checked against this checkout's own origin
#    before anything is published.
#
# THE TAG
#
# It is made here, at HEAD, before the build, because ESP-IDF stamps
# the image from `git describe` and that stamp is what the clock
# compares against the manifest. So the tag marks the source that
# produced the image. publish-release.sh pushes it and hangs the
# GitHub release on it; the latest.json commit lands immediately after
# it, which means `git checkout <tag>` gives you the source of that
# release and the manifest of the one before. That is the honest way
# round: the tag can only be older than the manifest that describes the
# image it built.
#
# Options:
#   --reuse-build   skip the compile if build-release already holds an
#                   image stamped with this exact tag. For re-staging
#                   after a failed publish, not for a normal release.
#
set -euo pipefail

SCRIPT_NAME="prepare-release"
# SOURCE_ROOT is sources/ - the build, the components and dist/ are all
# relative to it. REPO_ROOT is the repository that holds it, which is
# also the repository the release is published to.
SOURCE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_ROOT="$(git -C "$SOURCE_ROOT" rev-parse --show-toplevel)"
# shellcheck source=lib/release-common.sh
. "$SOURCE_ROOT/release-scripts/lib/release-common.sh"
# shellcheck source=../tools/lib/paths.sh
. "$SOURCE_ROOT/tools/lib/paths.sh"
cd "$SOURCE_ROOT"

PROJECT_DIR="firmware/nn20clock"
BUILD_SUBDIR="build-release"           # relative to PROJECT_DIR, git-ignored
BUILD="$PROJECT_DIR/$BUILD_SUBDIR"

REUSE_BUILD=0
VERSION=""
NOTES=""

usage() {
    # The comment block at the top of this file, to the first line that
    # is not one. A line range would go stale the moment anything is
    # added to it - and it did.
    awk 'NR > 2 && /^#/ { sub(/^#[ ]?/, ""); print; next }
         NR > 2         { exit }' "$0"
    exit 2
}

while [ $# -gt 0 ]; do
    case "$1" in
        --reuse-build) REUSE_BUILD=1; shift ;;
        -h|--help)     usage ;;
        -*)            die "unknown option '$1' (try --help)" ;;
        *)
            if   [ -z "$VERSION" ]; then VERSION="$1"
            elif [ -z "$NOTES" ];   then NOTES="$1"
            else die "unexpected argument '$1'"
            fi
            shift ;;
    esac
done

[ -n "$VERSION" ] || usage
TAG="$(normalize_version "$VERSION")"
require_version_format "$TAG"

APP="$(app_asset "$TAG")"
FULL="$(full_asset "$TAG")"
OUT="dist/$TAG"

# ------------------------------------------------------------- guards --

step "checking the tree"

# Anything at all, tracked or not. `git describe --dirty` ignores
# untracked files, so a build with an uncommitted component in it is
# stamped with a clean tag and looks like a release - while the tag it
# names cannot rebuild the image. That is the failure worth refusing:
# it is invisible afterwards.
# Checked over the whole repository, not just sources/: the installers,
# the video tooling and the manifest live above it and a release commits
# into the same tree.
if [ -n "$(git -C "$REPO_ROOT" status --porcelain)" ]; then
    say
    git -C "$REPO_ROOT" status --short
    say
    die "the working tree is not clean.

   Every file above has to be committed (or removed) first. A release
   is a tag somebody can check out and rebuild; modified files make the
   image differ from the tag, and untracked ones are in the image but
   not in the tag at all - which no later inspection reveals."
fi

# The tags decide the version string, and a checkout that has never
# fetched them has an incomplete set: `git describe` then names an older
# tag, and the image is stamped with a version that was already
# published. Cheap to fetch, and offline is not a reason to stop - the
# checks below still run on whatever is here.
if ! git -C "$REPO_ROOT" fetch --quiet --tags origin 2>/dev/null; then
    warn "could not fetch tags from origin; working with the local set"
fi

if git rev-parse -q --verify "refs/tags/$TAG" >/dev/null; then
    TAGGED="$(git rev-parse "refs/tags/$TAG^{commit}")"
    HEAD_SHA="$(git rev-parse HEAD)"
    if [ "$TAGGED" != "$HEAD_SHA" ]; then
        die "$TAG already exists and points at $(git rev-parse --short "$TAGGED"), not HEAD.

   Release that commit instead:   git checkout $TAG
   Or pick the next version.      Moving a published tag is not an
                                  option - the clocks that already have
                                  it will never fetch it again."
    fi
    say "tag $TAG is already here, at HEAD"
    [ -n "$NOTES" ] || NOTES="$(git tag -l --format='%(contents:subject)' "$TAG")"
else
    say "tag $TAG does not exist; creating it at HEAD $(git rev-parse --short HEAD)"
    git tag -a "$TAG" -m "${NOTES:-$TAG}"
fi

[ -n "$NOTES" ] || NOTES="$TAG"

# More than one version tag on this commit, and `git describe` picks
# one of them - not necessarily this one. The image would then be
# stamped with the other version, which the check after the build
# catches, six minutes later. Caught here instead.
HEAD_TAGS="$(git tag --points-at HEAD | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' || true)"
if [ "$(printf '%s\n' "$HEAD_TAGS" | grep -c .)" -gt 1 ]; then
    say
    printf '    %s\n' $HEAD_TAGS
    say
    die "HEAD carries more than one version tag.

   ESP-IDF stamps the image with \`git describe\`, which picks one of
   them, and it is not necessarily $TAG. Delete the ones that are not
   this release:

     git tag -d <the others>"
fi

# The About screen has room for one line and the firmware truncates to
# NN20CLOCK_OTA_NOTES_MAX. Better to say so now than to discover it on
# a 720x720 panel.
if [ "${#NOTES}" -gt 95 ]; then
    warn "the notes are ${#NOTES} characters; the clock shows the first 95"
fi

# A release older than one already tagged would produce a manifest that
# every up-to-date clock correctly ignores - which looks exactly like a
# broken update.
NEWEST="$(git tag -l 'v[0-9]*.[0-9]*.[0-9]*' | sort -V | tail -1)"
if [ "$NEWEST" != "$TAG" ]; then
    warn "$NEWEST is a higher version than $TAG.

   Publishing $TAG makes the manifest name an older release, and a
   clock running $NEWEST will not offer it. That is the firmware
   working as intended, so make sure it is what you meant."
fi

# ------------------------------------------------------------- build --

BIN="$BUILD/nn20clock.bin"
NEED_BUILD=1
if [ "$REUSE_BUILD" = "1" ] && [ -f "$BIN" ]; then
    if [ "$(stamped_version "$BIN" || true)" = "$TAG" ]; then
        say "reusing $BIN, already stamped $TAG"
        NEED_BUILD=0
    else
        warn "--reuse-build ignored: $BIN is not stamped $TAG"
    fi
fi

if [ "$NEED_BUILD" = "1" ]; then
    step "building $TAG"
    say "    its own build directory, configured from scratch, which"
    say "    takes a few minutes."
    #
    # From scratch, and not only for cleanliness. PROJECT_VER is worked
    # out by ESP-IDF at *configure* time, so a build directory left over
    # from the previous release is not reconfigured by a new tag: the
    # image would come out stamped with the old version, which the check
    # below then rejects with nothing to do about it. Deleting the
    # directory is what makes the tag reach the binary.
    #
    rm -rf "$BUILD"
    mkdir -p "$BUILD"
    # SDKCONFIG lives inside the build directory so the generated config
    # cannot be confused with the development one, and is thrown away
    # with it.
    #
    # There used to be a -D NN20CLOCK_RELEASE=1 here. Nothing has ever
    # read it - not the project CMakeLists, not a component, not a
    # Kconfig - so it was removed rather than left looking like a
    # release build differs from an ordinary one. It does not: what
    # makes this image a release is the tag it is stamped with and the
    # clean tree it came from, both checked above and below.
    tools/idf.sh -B "$BUILD_SUBDIR" \
                 -D "SDKCONFIG=$BUILD_SUBDIR/sdkconfig" \
                 build
fi

[ -f "$BIN" ] || die "$BIN was not produced"

# ------------------------------------------------- what was built, really --

step "checking the image"

STAMPED="$(stamped_version "$BIN" || true)"
[ -n "$STAMPED" ] || die "$BIN has no application descriptor. Not an app image?"
if [ "$STAMPED" != "$TAG" ]; then
    die "the image is stamped '$STAMPED', not '$TAG'.

   That string is what the clock compares against the manifest, so this
   would ship a release that immediately offers itself an update, or
   one it refuses. It comes from \`git describe\` in $PROJECT_DIR at
   configure time - check the tag is at HEAD, then run again without
   --reuse-build, which rebuilds from scratch."
fi
say "    version   $STAMPED  (read out of the image)"

# The credentials tripwire.
#
# Nothing sets these any more - the options were deleted from
# main/Kconfig.projbuild - so this normally finds nothing and costs
# nothing. It is here because the failure it catches is silent and
# permanent: a release asset is world-readable forever, and a
# reintroduced build-time credential would travel inside one with no
# other sign. If it ever fires, the answer is to take the option back
# out, not to publish anyway.
GENERATED="$BUILD/sdkconfig"
LEAKED=0
for key in CONFIG_NN20CLOCK_WIFI_SSID CONFIG_NN20CLOCK_WIFI_PASSWORD; do
    if [ -f "$GENERATED" ]; then
        value="$(sed -n "s/^$key=\"\\(.*\\)\"\$/\\1/p" "$GENERATED")"
        if [ -n "$value" ]; then
            warn "$key is set in $GENERATED"
            LEAKED=1
        fi
    fi
done
if [ -f "$PROJECT_DIR/sdkconfig.defaults.local" ]; then
    warn "$PROJECT_DIR/sdkconfig.defaults.local exists again"
    LEAKED=1
fi
if [ "$LEAKED" = "1" ]; then
    die "this build can carry Wi-Fi credentials and must not be published.

   A release asset is world-readable forever. Build-time credentials
   were removed on 2026-09-06 - the network is chosen on the device -
   so something has put them back. Take them out rather than shipping
   this image."
fi
say "    wi-fi     no build-time credentials"

# It has to fit the slot it will be written into, and the slot is in
# the partition table rather than in this script.
SLOT="$(awk -F',' '/^ota_0/ { gsub(/[ \t]/, "", $5); print $5 }' \
        "$PROJECT_DIR/partitions.csv")"
case "$SLOT" in
    *M) SLOT_BYTES=$(( ${SLOT%M} * 1024 * 1024 )) ;;
    *K) SLOT_BYTES=$(( ${SLOT%K} * 1024 )) ;;
    0x*) SLOT_BYTES=$(( SLOT )) ;;
    *)  SLOT_BYTES=0 ;;
esac
APP_SIZE="$(size_of "$BIN")"
if [ "$SLOT_BYTES" -gt 0 ] && [ "$APP_SIZE" -ge "$SLOT_BYTES" ]; then
    die "the application is $APP_SIZE bytes and ota_0 is $SLOT_BYTES ($SLOT).

   It would not fit the slot it is downloaded into. The device checks
   this too, and refuses before writing anything - but it checks after
   somebody pressed Install."
fi
say "    size      $APP_SIZE bytes of $SLOT_BYTES ($SLOT slot)"

# ------------------------------------------------------------ staging --

step "staging $OUT"

rm -rf "$OUT"
mkdir -p "$OUT/assets"

cp "$BIN" "$OUT/assets/$APP"

# Two images, because the two jobs are different. A clock in the field
# replaces its application and nothing else, so OTA downloads the app
# alone. A board that has never run this firmware needs the bootloader,
# the partition table and the OTA selector as well - four pieces at four
# offsets, one of which has moved already - so those are merged into one
# file that goes at offset 0 and cannot be got wrong.
#
# Merged from the build's own flash_args rather than from offsets
# repeated here, so it cannot drift from what `idf.py flash` would do.
say "    merging the full-flash image"
# The container's login shell announces the ESP-IDF environment on
# stderr whatever export.sh is told, so the whole thing is captured and
# shown only if it fails - where it is the only thing worth reading.
MERGE_LOG="$(mktemp)"
# The same mount as tools/idf.sh: the repository, with the build
# directory named relative to it. Mounting sources/ here instead would
# work for the merge and then differ from every other container in this
# tree, which is how a path stops matching what people expect.
BUILD_IN_CONTAINER="$(nn20clock_container_path "$REPO_ROOT" "$SOURCE_ROOT/$BUILD")"
if ! docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
        -v "$REPO_ROOT:/workspace" -w "$BUILD_IN_CONTAINER" \
        "${NN20CLOCK_IDF_IMAGE:-nn20clock-esp32-idf}" \
        bash -lc '. $IDF_PATH/export.sh >/dev/null 2>&1 && python -m esptool \
            --chip esp32p4 merge_bin -o merged-full.bin @flash_args' \
        > "$MERGE_LOG" 2>&1; then
    cat "$MERGE_LOG" >&2
    rm -f "$MERGE_LOG"
    die "merging the full-flash image failed"
fi
rm -f "$MERGE_LOG"
mv "$BUILD/merged-full.bin" "$OUT/assets/$FULL"

APP_SHA="$(sha256_of "$OUT/assets/$APP")"
FULL_SIZE="$(size_of "$OUT/assets/$FULL")"

( cd "$OUT/assets" && sha256sum "$APP" "$FULL" > SHA256SUMS )

# ----------------------------------------------------------- manifest --

# Where this release is published is not a constant here: it is read
# back out of the configuration the image was built with, so the scripts
# publish where this firmware actually looks.
MANIFEST_URL="$(manifest_url "$GENERATED")"
parse_manifest_url "$MANIFEST_URL"

URL="https://github.com/$MANIFEST_OWNER/$MANIFEST_REPO/releases/download/$TAG/$APP"

# json.tool rather than a heredoc, so a quote or a backslash in the
# notes produces an escaped string instead of a manifest the clock
# rejects as "not JSON".
TAG="$TAG" NOTES="$NOTES" URL="$URL" SIZE="$APP_SIZE" SHA="$APP_SHA" \
python3 - > "$OUT/latest.json" <<'PY'
import json, os
print(json.dumps({
    "version": os.environ["TAG"],
    "notes":   os.environ["NOTES"],
    "url":     os.environ["URL"],
    "size":    int(os.environ["SIZE"]),
    "sha256":  os.environ["SHA"],
}, indent=2))
PY

# The release these two scripts hand to each other. publish-release.sh
# reads it rather than being told again on the command line, so the
# thing that was built is the thing that gets published.
TAG="$TAG" OWNER="$MANIFEST_OWNER" REPO="$MANIFEST_REPO" \
BRANCH="$MANIFEST_BRANCH" PATH_IN_REPO="$MANIFEST_PATH" \
NOTES="$NOTES" COMMIT="$(git rev-parse HEAD)" \
python3 - > "$OUT/release.json" <<'PY'
import json, os
print(json.dumps({
    "tag":            os.environ["TAG"],
    "notes":          os.environ["NOTES"],
    "owner":          os.environ["OWNER"],
    "repo":           os.environ["REPO"],
    "branch":         os.environ["BRANCH"],
    "manifest_path":  os.environ["PATH_IN_REPO"],
    "source_commit":  os.environ["COMMIT"],
}, indent=2))
PY

# ------------------------------------------------------------- README --

NOTES_MD="$(printf '%s' "$NOTES" | sed 's/|/\\|/g')"

cat > "$OUT/README.md" <<EOF
# $TAG

Staged by \`sources/release-scripts/prepare-release.sh\`. Nothing has
been published yet.

| | |
| --- | --- |
| Tag | \`$TAG\` |
| Notes | $NOTES_MD |
| Source commit | \`$(git rev-parse --short HEAD)\` |
| App image | \`$APP\` — $APP_SIZE bytes |
| | \`$APP_SHA\` |
| Full image | \`$FULL\` — $FULL_SIZE bytes |
| Published to | \`$MANIFEST_OWNER/$MANIFEST_REPO\` (branch \`$MANIFEST_BRANCH\`) |
| Clock fetches | $MANIFEST_URL |

## Publish it

\`\`\`sh
sources/release-scripts/publish-release.sh $TAG
\`\`\`

That commits \`$MANIFEST_PATH\` at the top of this repository, pushes
it and the tag, creates the GitHub release **on that tag**, uploads the
two images and \`SHA256SUMS\`, and then checks that a clock can
actually fetch what the manifest promises. \`--dry-run\` prints the
plan and stops.

## What is in here

\`\`\`
sources/$OUT/
├── README.md      this file
├── release.json   what publish-release.sh reads
├── latest.json    COMMITTED as $MANIFEST_PATH — the clock reads this
└── assets/        UPLOADED as release assets — never committed
    ├── $APP
    ├── $FULL
    └── SHA256SUMS
\`\`\`

\`latest.json\` in a release asset would be a manifest no clock could
find: it is read from the branch, not from the release. The \`.bin\`
files in the repository would be binaries git keeps forever.

## By hand, if it comes to that

\`\`\`sh
cd $REPO_ROOT
cp $SOURCE_ROOT/$OUT/latest.json $MANIFEST_PATH
git add $MANIFEST_PATH && git commit -m "release $TAG" && git push
git push origin $TAG

gh release create $TAG \\
    $SOURCE_ROOT/$OUT/assets/$APP \\
    $SOURCE_ROOT/$OUT/assets/$FULL \\
    $SOURCE_ROOT/$OUT/assets/SHA256SUMS \\
    --repo $MANIFEST_OWNER/$MANIFEST_REPO \\
    --title $TAG
\`\`\`

Push the tag before creating the release: \`gh release create\` hangs
the release on a tag that already exists, and invents one at the default
branch if it does not.
EOF

# -------------------------------------------------------------- report --

step "staged $TAG"
say
say "  $OUT/latest.json      commit as $MANIFEST_PATH"
say "  $OUT/assets/$APP  $APP_SIZE bytes   over the air"
say "  $OUT/assets/$FULL  $FULL_SIZE bytes   first install, offset 0"
say "  $OUT/assets/SHA256SUMS"
say
if ! git -C "$REPO_ROOT" ls-remote --tags origin "refs/tags/$TAG" 2>/dev/null | grep -q .; then
    say "  The tag is local only, which is where it should be until this"
    say "  image has been on a board. publish-release.sh pushes it."
    say
fi
say "  Try it first - that is what the gap between the two scripts is"
say "  for:"
say
say "      sources/tools/flash.sh --image $OUT/assets/$FULL"
say
say "  Publish it:"
say
say "      sources/release-scripts/publish-release.sh $TAG"
say

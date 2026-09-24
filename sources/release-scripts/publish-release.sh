#!/usr/bin/env bash
#
# publish-release.sh - publish a staged release from this repository, in
# the order that makes it work.
#
#   sources/release-scripts/prepare-release.sh v0.2.0 "Over-the-air updates"
#   sources/release-scripts/publish-release.sh v0.2.0
#   sources/release-scripts/publish-release.sh v0.2.0 --dry-run   # plan only
#
# It reads sources/dist/<tag>/, which prepare-release.sh wrote, and does
# four things:
#
#   1. commits latest.json at the top of this repository and pushes it;
#   2. pushes the tag prepare-release.sh made, and creates the GitHub
#      release on it;
#   3. uploads the two images and SHA256SUMS to that release;
#   4. checks that a clock could actually complete this update -
#      manifest fetchable, image fetchable, SHA-256 of the published
#      image equal to what the manifest promises.
#
# THE ORDER, AND WHY IT IS THIS ONE
#
# The source and the manifest now live in the same repository, and that
# settles the order rather than leaving it to preference: the image is
# stamped from `git describe`, so the tag exists before the build, and
# the manifest carries the SHA-256 of that image, so it is written after
# it. The tag is therefore on the commit the firmware was built from and
# the manifest commit lands on top of it - one commit later, touching
# nothing that compiles. `git checkout <tag>` rebuilds this exact image;
# it gives you the previous release's latest.json, which is a file the
# build does not read.
#
# The tag is pushed before `gh release create`, because that command
# hangs the release on a tag that already exists and invents one at the
# default branch if it does not. Pushing it here is what makes the
# release point where this script means.
#
# The manifest goes out before the assets, which leaves a window - from
# the push until the uploads finish - where it names an image that 404s.
# It is reachable only by someone pressing Install inside that minute;
# the firmware reports a failed download and stays on the version it
# has. Step 4 is what says the window has closed.
#
# Nothing here is undoable by this script: a pushed commit, a pushed tag
# and a published release are public. It asks first unless --yes.
#
# Options:
#   --dry-run         print the plan and stop before changing anything
#   --yes             do not ask
#   --allow-private   publish even though the repository is private, and
#                     no clock can fetch it yet
#   --no-verify       skip the post-publish download check
#
set -euo pipefail

SCRIPT_NAME="publish-release"
# SOURCE_ROOT is sources/, which is where dist/ was staged. REPO_ROOT is
# the repository that holds it - and, now, the one being published to.
SOURCE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_ROOT="$(git -C "$SOURCE_ROOT" rev-parse --show-toplevel)"
# shellcheck source=lib/release-common.sh
. "$SOURCE_ROOT/release-scripts/lib/release-common.sh"
cd "$SOURCE_ROOT"

DRY_RUN=0
ASSUME_YES=0
VERIFY=1
ALLOW_PRIVATE=0
PRIVATE=0
VERSION=""

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
        --dry-run)   DRY_RUN=1; shift ;;
        --yes|-y)    ASSUME_YES=1; shift ;;
        --allow-private) ALLOW_PRIVATE=1; shift ;;
        --no-verify) VERIFY=0; shift ;;
        -h|--help)   usage ;;
        -*)          die "unknown option '$1' (try --help)" ;;
        *)           [ -z "$VERSION" ] || die "unexpected argument '$1'"
                     VERSION="$1"; shift ;;
    esac
done

[ -n "$VERSION" ] || usage
TAG="$(normalize_version "$VERSION")"
require_version_format "$TAG"

OUT="dist/$TAG"
APP="$(app_asset "$TAG")"
FULL="$(full_asset "$TAG")"

# --------------------------------------------------- what was staged --

step "reading $OUT"

[ -d "$OUT" ] || die "nothing is staged for $TAG.

     sources/release-scripts/prepare-release.sh $TAG \"one line of notes\""

for f in release.json latest.json "assets/$APP" "assets/$FULL" assets/SHA256SUMS; do
    [ -f "$OUT/$f" ] || die "$OUT/$f is missing. Re-run prepare-release.sh $TAG"
done

field() { python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))[sys.argv[2]])' "$1" "$2"; }

OWNER="$(field "$OUT/release.json" owner)"
REPO="$(field "$OUT/release.json" repo)"
BRANCH="$(field "$OUT/release.json" branch)"
MANIFEST_PATH="$(field "$OUT/release.json" manifest_path)"
NOTES="$(field "$OUT/release.json" notes)"
SOURCE_COMMIT="$(field "$OUT/release.json" source_commit)"

# What a clock will actually GET. Rebuilt from the same pieces the
# manifest URL was split into, so the two cannot disagree.
MANIFEST_URL="https://raw.githubusercontent.com/$OWNER/$REPO/$BRANCH/$MANIFEST_PATH"

# The manifest is the only thing the clock reads, so it is checked
# against the bytes it describes rather than trusted. A stale dist/ -
# rebuilt assets, manifest from the run before - is otherwise invisible
# until a device refuses the download for a checksum mismatch.
M_VERSION="$(field "$OUT/latest.json" version)"
M_SHA="$(field "$OUT/latest.json" sha256)"
M_SIZE="$(field "$OUT/latest.json" size)"
M_URL="$(field "$OUT/latest.json" url)"

[ "$M_VERSION" = "$TAG" ] || die "$OUT/latest.json names $M_VERSION, not $TAG"
ACTUAL_SHA="$(sha256_of "$OUT/assets/$APP")"
ACTUAL_SIZE="$(size_of "$OUT/assets/$APP")"
[ "$M_SHA" = "$ACTUAL_SHA" ] || die "the manifest and the staged image disagree.

   manifest  $M_SHA
   image     $ACTUAL_SHA

   Re-run prepare-release.sh $TAG - never edit latest.json by hand."
[ "$M_SIZE" = "$ACTUAL_SIZE" ] || die "the manifest says $M_SIZE bytes, the image is $ACTUAL_SIZE"

STAMPED="$(stamped_version "$OUT/assets/$APP" || true)"
[ "$STAMPED" = "$TAG" ] || die "the staged image is stamped '$STAMPED', not '$TAG'"

say "    manifest, image and stamped version agree on $TAG"

# ------------------------------------------------------ the tools -----

command -v gh >/dev/null 2>&1 || die "gh is not installed, and it is what creates the release.

     https://cli.github.com  -  then: gh auth login"
gh auth status >/dev/null 2>&1 || die "gh is not logged in.

     gh auth login"
command -v curl >/dev/null 2>&1 || die "curl is not installed, and it is what verifies"

# ----------------------------------------------------- this checkout --

step "checking $REPO_ROOT"

# One repository now: the source that was built, the manifest the clock
# reads, and the installers a person downloads are all in this tree.
pub() { git -C "$REPO_ROOT" "$@"; }

SLUG="$(pub remote get-url origin \
        | sed -E 's#^(git@github\.com:|https://github\.com/)##; s#\.git$##')"
[ "$SLUG" = "$OWNER/$REPO" ] || die "this checkout has origin $SLUG, but this release belongs to $OWNER/$REPO.

   $OWNER/$REPO is where the firmware looks - it is baked into the
   image as CONFIG_NN20CLOCK_OTA_MANIFEST_URL. A manifest committed
   anywhere else is one no clock will ever read."

CURRENT_BRANCH="$(pub rev-parse --abbrev-ref HEAD)"
[ "$CURRENT_BRANCH" = "$BRANCH" ] || die "this checkout is on '$CURRENT_BRANCH'; the clock reads '$BRANCH'.

     git checkout $BRANCH"

if [ -n "$(pub status --porcelain)" ]; then
    say
    pub status --short
    say
    die "the working tree has uncommitted changes.

   The release commit would carry them along, into a public repository,
   under the message 'release $TAG'. Deal with them first."
fi

# The tag prepare-release.sh made. It is what the image was stamped
# from, so a release cut from anything else would ship a version string
# that does not match the source it claims to be.
pub rev-parse -q --verify "refs/tags/$TAG^{commit}" >/dev/null || die "there is no tag $TAG in this repository.

   prepare-release.sh makes it, at the commit it builds. Without it
   there is nothing to hang the release on:

     sources/release-scripts/prepare-release.sh $TAG"

TAG_COMMIT="$(pub rev-parse "refs/tags/$TAG^{commit}")"
[ "$TAG_COMMIT" = "$SOURCE_COMMIT" ] || die "$TAG points at $(pub rev-parse --short "$TAG_COMMIT"), but $OUT was built from $(pub rev-parse --short "$SOURCE_COMMIT").

   The tag moved after the release was staged. Re-stage it so the
   image, the manifest and the tag are the same release:

     sources/release-scripts/prepare-release.sh $TAG"

if ! pub merge-base --is-ancestor "$TAG_COMMIT" HEAD; then
    die "$TAG is not an ancestor of HEAD.

   The manifest commit is made on top of HEAD and the release is cut
   from $TAG, so a tag off to one side would publish a manifest that
   describes an image built from a commit this branch does not have."
fi

say "    fetching origin"
pub fetch --quiet origin "$BRANCH"
LOCAL_HEAD="$(pub rev-parse HEAD)"
REMOTE_HEAD="$(pub rev-parse "origin/$BRANCH")"
if [ "$LOCAL_HEAD" != "$REMOTE_HEAD" ]; then
    if pub merge-base --is-ancestor HEAD "origin/$BRANCH"; then
        die "this checkout is behind origin/$BRANCH.

     git pull --ff-only"
    else
        die "this checkout has commits origin/$BRANCH does not.

   Push or drop them first - this script pushes, and it must not carry
   somebody else's unrelated work into the release commit.

     git log --oneline origin/$BRANCH..HEAD"
    fi
fi

# Already published? Every half is checked, because a half-published
# release is exactly the state a re-run has to be safe in.
RELEASE_EXISTS=0
if gh release view "$TAG" --repo "$OWNER/$REPO" >/dev/null 2>&1; then
    RELEASE_EXISTS=1
fi

# The tag may already be on the remote - a re-run after an interrupted
# publish, or a tag pushed by hand. That is fine as long as it is this
# tag: `gh release create` hangs the release on whatever is there, so a
# remote tag pointing somewhere else would publish a release cut from
# code this image was not built from.
TAG_PUSHED=0
REMOTE_TAG="$(pub ls-remote --tags origin "refs/tags/$TAG^{}" \
              | awk '{print $1}' | head -1)"
[ -n "$REMOTE_TAG" ] || REMOTE_TAG="$(pub ls-remote --tags origin "refs/tags/$TAG" \
              | awk '{print $1}' | head -1)"
if [ -n "$REMOTE_TAG" ]; then
    TAG_PUSHED=1
    [ "$REMOTE_TAG" = "$TAG_COMMIT" ] || die "$OWNER/$REPO already has the tag $TAG, at $(pub rev-parse --short "$REMOTE_TAG" 2>/dev/null || printf '%s' "$REMOTE_TAG"), and this release was built at $(pub rev-parse --short "$TAG_COMMIT").

   The release would be cut from the commit already tagged there, not
   from the one this image came from. A published tag must not be
   moved - the clocks that already fetched it would never see the
   change - so release the next version instead."
fi

# ---------------------------------------------- can anything see it? ---

# A private repository serves neither the manifest nor the release
# asset to a device: raw.githubusercontent.com and the download URL both
# answer 404 to a client with no credentials, and the clock has none. It
# is worth failing on rather than warning about, because everything
# afterwards succeeds - the commit, the release, the upload - and the
# only symptom is every clock in the field reporting that the update
# server answered with nonsense.
if [ "$(gh repo view "$OWNER/$REPO" --json isPrivate --jq .isPrivate 2>/dev/null)" = "true" ]; then
    if [ "$ALLOW_PRIVATE" = "1" ]; then
        warn "$OWNER/$REPO is private; publishing anyway (--allow-private).
   No clock can fetch this release until the repository is public."
        # Nothing anonymous can be fetched, so there is nothing the
        # verification step could check.
        VERIFY=0
        PRIVATE=1
    else
        die "$OWNER/$REPO is private.

   The clock fetches the manifest and the image anonymously over TLS,
   so both answer 404 while the repository is private - and nothing in
   this script or on the device says why. A release published now is
   invisible to every board in the field.

     gh repo edit $OWNER/$REPO --visibility public --accept-visibility-change-consequences

   Or, to stage the release now and make it visible later:

     sources/release-scripts/publish-release.sh $TAG --allow-private"
    fi
fi

# The installers, the video tooling and the enclosure are NOT touched
# here. They are ordinary files in this repository with their own
# history, and a release changes exactly two things: the manifest at the
# top, and the assets on the GitHub release. If firmware/flash.sh or
# video/ needs a change for a release, commit it before running
# prepare-release.sh - the clean-tree guard there is what makes sure it
# is part of the tag rather than an afterthought.

# ------------------------------------------------------------ the plan --

MANIFEST_COMMITTED=0
if [ -f "$REPO_ROOT/$MANIFEST_PATH" ] && cmp -s "$OUT/latest.json" "$REPO_ROOT/$MANIFEST_PATH"; then
    MANIFEST_COMMITTED=1
fi

step "the plan"
say
say "  release      $TAG — $NOTES"
say "  built from   $(pub rev-parse --short "$SOURCE_COMMIT") (tagged $TAG)"
say "  into         $OWNER/$REPO  (branch $BRANCH)"
say
if [ "$MANIFEST_COMMITTED" = "1" ]; then
    say "  1. $MANIFEST_PATH is already committed and pushed for $TAG — skipping"
else
    say "  1. commit $MANIFEST_PATH and push it"
fi
if [ "$TAG_PUSHED" = "1" ]; then
    say "  2. tag $TAG is already on origin — not pushed again"
else
    say "  2. push the tag $TAG"
fi
if [ "$RELEASE_EXISTS" = "1" ]; then
    say "  3. release $TAG already exists on GitHub — uploading any missing assets"
else
    say "  3. create release $TAG on that tag, with:"
fi
say "         $APP        $ACTUAL_SIZE bytes   over the air"
say "         $FULL   $(size_of "$OUT/assets/$FULL") bytes   first install"
say "         SHA256SUMS"
if [ "$VERIFY" = "1" ]; then
    say "  4. verify a clock can fetch and check what it names"
fi
say

if [ "$DRY_RUN" = "1" ]; then
    say "--dry-run: nothing was changed."
    exit 0
fi

if [ "$MANIFEST_COMMITTED" = "1" ] && [ "$RELEASE_EXISTS" = "1" ]; then
    warn "$TAG looks published already. Continuing only uploads assets."
fi

confirm "Publish $TAG to $OWNER/$REPO? This is public and permanent. [y/N]"

# ---------------------------------------------- 1. the manifest commit --

if [ "$MANIFEST_COMMITTED" = "0" ] || [ -n "$(pub status --porcelain)" ]; then
    step "committing $MANIFEST_PATH"
    mkdir -p "$(dirname "$REPO_ROOT/$MANIFEST_PATH")"
    cp "$OUT/latest.json" "$REPO_ROOT/$MANIFEST_PATH"
    pub add -- "$MANIFEST_PATH"
    if [ -n "$(pub status --porcelain)" ]; then
        pub commit -q -m "release $TAG

$NOTES

Built from $SOURCE_COMMIT, tagged $TAG."
        pub push --quiet origin "$BRANCH"
        say "    pushed $(pub rev-parse --short HEAD)"
    else
        say "    nothing changed; the manifest was already this"
    fi
else
    say "    manifest already committed at $(pub rev-parse --short HEAD)"
fi

# ----------------------------------------------------- 2. the tag ------

# Before the release, not after: `gh release create` hangs the release
# on a tag that is already there, and creates one at the default branch
# if it is not - which would cut the release from whatever HEAD happens
# to be rather than from the commit this image was built at.
if [ "$TAG_PUSHED" = "1" ]; then
    say "    tag $TAG is already on origin"
else
    step "pushing the tag $TAG"
    pub push --quiet origin "refs/tags/$TAG"
fi

# ------------------------------------------------- 3. the release ------

if [ "$RELEASE_EXISTS" = "1" ]; then
    step "uploading to the existing release $TAG"
    gh release upload "$TAG" \
        "$OUT/assets/$APP" "$OUT/assets/$FULL" "$OUT/assets/SHA256SUMS" \
        --repo "$OWNER/$REPO" --clobber
else
    step "creating release $TAG on $(pub rev-parse --short "$TAG_COMMIT")"
    gh release create "$TAG" \
        "$OUT/assets/$APP" "$OUT/assets/$FULL" "$OUT/assets/SHA256SUMS" \
        --repo "$OWNER/$REPO" \
        --title "$TAG" \
        --notes "$NOTES

Flash a new board with \`firmware/flash.sh\`. A clock already running
this firmware updates itself: gear icon → About → Check for updates."
fi

# -------------------------------------------------- 3. does it work ----

# Where it ended up, and what to do with it. Printed whether or not the
# checks below ran, because it is the same release either way.
summary() {
    say
    say "Published $TAG."
    say
    say "  release   https://github.com/$OWNER/$REPO/releases/tag/$TAG"
    say "  manifest  $MANIFEST_URL"
    say
    say "On a clock already in the field: gear icon → About → Check for"
    say "updates. It should offer $TAG. On a new board: firmware/flash.sh"
    say "downloads $FULL."
    say
}

if [ "$VERIFY" = "0" ]; then
    say
    if [ "$PRIVATE" = "1" ]; then
        say "Not verifying: $OWNER/$REPO is private, so nothing a clock"
        say "could do can be tried from here either."
    else
        say "Not verifying (--no-verify)."
    fi
    summary
    exit 0
fi

step "verifying"

CODE="$(curl -fsIL -o /dev/null -w '%{http_code}' "$M_URL" || true)"
[ "$CODE" = "200" ] || die "the image the manifest names answers $CODE:

     $M_URL

   The manifest is already committed, so a clock checking now would
   offer the update and then fail to download it. Fix the release
   assets, or revert the manifest commit."
say "    image        200"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# The check that matters. TLS and a 200 say the file arrived; only the
# hash says it is the file the clock will accept, and a mismatch there
# is what the device reports as "the download did not match its
# checksum" - after somebody pressed Install.
curl -fsSL -o "$WORK/app.bin" "$M_URL" || die "could not download $M_URL"
PUBLISHED_SHA="$(sha256_of "$WORK/app.bin")"
[ "$PUBLISHED_SHA" = "$M_SHA" ] || die "the published image does not match the manifest.

   manifest   $M_SHA
   published  $PUBLISHED_SHA

   Every clock that tries this update will refuse it. Re-upload the
   asset from $OUT/assets/$APP"
say "    sha-256      matches the manifest"

# raw.githubusercontent is a CDN and serves the previous manifest for a
# few minutes. Not an error, so it is reported rather than failed on.
if curl -fsSL -o "$WORK/latest.json" "$MANIFEST_URL" 2>/dev/null; then
    SERVED="$(field "$WORK/latest.json" version 2>/dev/null || echo '?')"
    if [ "$SERVED" = "$TAG" ]; then
        say "    manifest     serving $TAG"
    else
        warn "the manifest URL is still serving $SERVED, not $TAG.
   That is raw.githubusercontent's cache; it catches up within a few
   minutes. Check again with:

     curl -fsS $MANIFEST_URL"
    fi
else
    warn "could not fetch $MANIFEST_URL yet (CDN lag); check it shortly"
fi

summary

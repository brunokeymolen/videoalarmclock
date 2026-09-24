# release-common.sh - what prepare-release.sh and publish-release.sh
# both have to agree on. Sourced, never executed.
#
# The two halves of a release run at different times, often days apart,
# and they have to agree about three things or the release is silently
# wrong rather than loudly broken:
#
#   the version format   the clock parses vMAJOR.MINOR.PATCH and nothing
#                        else, and a version it cannot parse is one it
#                        will not install.
#   where it is published  the image URL, and the repository the manifest
#                        is committed to, both come from the manifest URL
#                        compiled into the firmware. A release published
#                        anywhere else is one no clock will ever see.
#   which image is which  the app alone goes over the air; the merged
#                        image goes on a fresh board over a cable.
#
# So each of those lives here once, and both scripts read it.

# ------------------------------------------------------------ output --

# SCRIPT_NAME is set by whichever script sourced this, so the prefix on
# an error names the thing the person actually ran.
: "${SCRIPT_NAME:=release}"

say()  { printf '%s\n' "$*"; }
step() { printf '\n==> %s\n' "$*"; }
warn() { printf '%s: %s\n' "$SCRIPT_NAME" "$*" >&2; }
die()  { printf '%s: %s\n' "$SCRIPT_NAME" "$*" >&2; exit 1; }

# A question that must be answered by a person. ASSUME_YES short-circuits
# it for a scripted run; anything but "yes" stops.
confirm() {
    local prompt="$1"
    if [ "${ASSUME_YES:-0}" = "1" ]; then
        say "$prompt yes (--yes)"
        return 0
    fi
    if [ ! -t 0 ]; then
        die "$prompt

   Nothing is reading the terminal. Re-run with --yes if that is what
   you mean."
    fi
    local answer=""
    read -r -p "$prompt " answer
    case "$answer" in
        y|Y|yes|YES) return 0 ;;
        *) die "stopped. Nothing was published." ;;
    esac
}

# ----------------------------------------------------------- version --

# "0.2.0" and "v0.2.0" are the same release. The tag, the manifest and
# the asset names all use the v form, so everything is normalised to it
# once, here, rather than each caller remembering.
normalize_version() {
    local v="$1"
    case "$v" in
        v*) printf '%s' "$v" ;;
        *)  printf 'v%s' "$v" ;;
    esac
}

require_version_format() {
    printf '%s' "$1" | grep -Eq '^v[0-9]+\.[0-9]+\.[0-9]+$' && return 0
    die "'$1' is not vMAJOR.MINOR.PATCH.

   nn20clock_ota_compare_versions() parses exactly that and treats
   anything else as a release that cannot name itself - which it then
   refuses to install. See components/nn20clock_ota/src/nn20clock_ota_version.c"
}

# ------------------------------------------------------ where it goes --

# The one URL the firmware fetches, taken from the build that is being
# released when there is one, and from the component's Kconfig default
# otherwise. Reading it rather than repeating it is the point: these
# scripts publish where the image looks, even if that default changes.
manifest_url() {
    local generated="$1"     # a generated sdkconfig, or "" for the default
    if [ -n "$generated" ] && [ -f "$generated" ]; then
        sed -n 's/^CONFIG_NN20CLOCK_OTA_MANIFEST_URL="\(.*\)"$/\1/p' \
            "$generated" | head -1
        return
    fi
    sed -n 's/^[[:space:]]*default "\(https:\/\/[^"]*\)"$/\1/p' \
        "$SOURCE_ROOT/components/nn20clock_ota/Kconfig" | head -1
}

# Split a raw.githubusercontent.com manifest URL into the pieces the
# rest of a release is built from: the repository that gets the commit,
# the branch the clock reads, and the path within it.
#
# Sets MANIFEST_OWNER, MANIFEST_REPO, MANIFEST_BRANCH, MANIFEST_PATH.
parse_manifest_url() {
    local url="$1"
    [ -n "$url" ] || die "the firmware has no manifest URL.

   CONFIG_NN20CLOCK_OTA_MANIFEST_URL is empty, which turns updates off:
   the About screen reports that this build has nowhere to check. There
   is nothing to publish a manifest to."

    local rx='^https://raw\.githubusercontent\.com/([^/]+)/([^/]+)/([^/]+)/(.+)$'
    [[ "$url" =~ $rx ]] || die "cannot read a repository out of the manifest URL:

     $url

   These scripts expect the raw.githubusercontent.com form, which is
   what says which repository to commit the manifest to. Publishing by
   hand is then the only option - dist/<tag>/README.md has the pieces."

    MANIFEST_OWNER="${BASH_REMATCH[1]}"
    MANIFEST_REPO="${BASH_REMATCH[2]}"
    MANIFEST_BRANCH="${BASH_REMATCH[3]}"
    MANIFEST_PATH="${BASH_REMATCH[4]}"
}

# ------------------------------------------------------------- images --

app_asset()  { printf 'nn20clock-%s.bin' "$1"; }
full_asset() { printf 'nn20clock-%s-full.bin' "$1"; }

# The version string ESP-IDF stamped into an application image.
#
# This is the number the clock compares against the manifest, and it
# comes from `git describe` at build time rather than from the argument
# any of this was invoked with - so it is the only honest answer to
# "what did we actually build". esp_app_desc_t sits at offset 32 of the
# image with its magic first and `version` 16 bytes into it.
stamped_version() {
    local bin="$1" magic
    magic="$(dd if="$bin" bs=1 skip=32 count=4 2>/dev/null \
             | od -An -tx4 | tr -d ' \n')"
    [ "$magic" = "abcd5432" ] || return 1
    dd if="$bin" bs=1 skip=48 count=32 2>/dev/null | tr -d '\000'
}

sha256_of() { sha256sum "$1" | cut -d' ' -f1; }
size_of()   { stat -c '%s' "$1"; }

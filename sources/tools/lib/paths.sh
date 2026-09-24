# paths.sh - what the container has mounted, and where. Sourced, never
# executed.
#
# The sources live in a subdirectory of the repository, and that one
# fact decides what gets mounted. Mounting sources/ alone looks tidy and
# is wrong: ESP-IDF derives PROJECT_VER from `git describe`, .git is at
# the top of the repository, and a build that cannot see it is stamped
# with ESP-IDF's fallback instead of the tag. That version string is
# what the clock compares against the update manifest, so getting it
# wrong is not cosmetic - release-scripts/prepare-release.sh reads it
# back out of the image and refuses to publish when it is not the tag.
#
# So the repository root is mounted at /workspace, and everything else
# here is about saying where a host path lands inside it.

# The directory mounted at /workspace: the repository root when there is
# one, and the sources otherwise - a tarball with no .git still builds,
# it just cannot stamp a version.
#
# Takes the sources directory; prints a host path.
nn20clock_mount_root() {
    local source_root="$1"
    git -C "$source_root" rev-parse --show-toplevel 2>/dev/null \
        || printf '%s' "$source_root"
}

# Where a host path inside the mount appears in the container.
#
# Takes the mount root and a host path; prints an absolute container
# path. Fails if the path is outside the mount, because a path the
# container cannot see is worth an error rather than a confusing one
# from whatever tool was handed it.
nn20clock_container_path() {
    local mount_root="$1" host_path="$2" rel
    rel="$(realpath --relative-base="$mount_root" -- "$host_path")" || return 1
    case "$rel" in
        /*) printf 'not inside %s: %s\n' "$mount_root" "$host_path" >&2
            return 1 ;;
        .)  printf '/workspace' ;;
        *)  printf '/workspace/%s' "$rel" ;;
    esac
}

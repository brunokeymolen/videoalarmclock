# Everything yt2clock.sh needs, so nothing has to be installed on the
# host but Docker.
#
# The awkward dependency is yt-dlp. YouTube changes how it hands out
# download URLs often enough that a yt-dlp more than a few weeks old
# simply stops working - and it fails with an extraction error that
# reads like the video was taken down rather than like a stale tool.
# Distribution packages are always too old, so the official binary is
# fetched here, and `./videoclock --update` rebuilds without the cache
# to get a newer one.
FROM debian:bookworm-slim

# ffmpeg does the conversion; curl uploads over FTP and fetches yt-dlp
# and deno; python3 is what yt-dlp runs on; unzip unpacks deno.
RUN apt-get update && apt-get install -y --no-install-recommends \
        ffmpeg \
        curl \
        ca-certificates \
        python3 \
        unzip \
    && rm -rf /var/lib/apt/lists/*

# The JavaScript runtime YouTube's URL signing needs. It cannot be
# Debian's nodejs package: that is v18, and yt-dlp requires Deno >= 2.3,
# Node >= 22 or Bun >= 1.2.11. An unsupported runtime is not treated as
# a runtime at all - extraction falls back to a path that does not work
# and reports "This video is not available", which reads like the video
# has been deleted rather than like a dependency four major versions too
# old. Deno is the one yt-dlp enables by default, so no flag is needed
# to reach it.
RUN set -eux; \
    case "$(dpkg --print-architecture)" in \
        amd64) DENO_ARCH=x86_64-unknown-linux-gnu ;; \
        arm64) DENO_ARCH=aarch64-unknown-linux-gnu ;; \
        *) echo "no deno build for $(dpkg --print-architecture)" >&2; exit 1 ;; \
    esac; \
    curl -fsSL -o /tmp/deno.zip \
        "https://github.com/denoland/deno/releases/latest/download/deno-${DENO_ARCH}.zip"; \
    unzip -q /tmp/deno.zip -d /usr/local/bin; \
    rm /tmp/deno.zip; \
    chmod a+rx /usr/local/bin/deno; \
    deno --version

RUN curl -fsSL \
        https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp \
        -o /usr/local/bin/yt-dlp \
    && chmod a+rx /usr/local/bin/yt-dlp \
    && yt-dlp --version

COPY yt2clock.sh file2clock.sh /usr/local/bin/
RUN chmod a+rx /usr/local/bin/yt2clock.sh /usr/local/bin/file2clock.sh

# Output lands in the caller's directory, which the wrapper mounts here.
WORKDIR /out

# No ENTRYPOINT: there are two scripts and the wrapper picks between
# them by looking at what it was given - a URL or a file on disk. Set a
# default command so that `docker run <image>` on its own still explains
# itself rather than sitting there.
CMD ["/usr/local/bin/yt2clock.sh"]

# Trace replay host: Mesa llvmpipe through EGL (surfaceless), GLES 3.1.
FROM debian:bookworm
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    g++ cmake ninja-build libegl-dev libgles-dev zlib1g-dev libegl-mesa0 libgl1-mesa-dri zstd ca-certificates \
 && rm -rf /var/lib/apt/lists/*

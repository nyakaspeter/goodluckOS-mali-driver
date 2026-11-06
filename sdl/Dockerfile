# syntax=docker/dockerfile:1
# SDL2 for goodluckOS with the "mali" fbdev video backend, for ARM's closed
# Mali-400 driver (r6p2 fbdev). Same version as the system SDL (2.32.10).
FROM debian:bookworm AS builder

RUN dpkg --add-architecture armhf && \
    apt-get update && \
    apt-get install -y --no-install-recommends \
        ca-certificates wget make pkg-config file \
        crossbuild-essential-armhf libasound2-dev:armhf libudev-dev:armhf && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /build
RUN wget -q https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz && \
    tar xzf SDL2-2.32.10.tar.gz

WORKDIR /build/SDL2-2.32.10
RUN ./configure --host=arm-linux-gnueabihf --prefix=/usr \
        CFLAGS="-O2 -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard" \
        --enable-video-mali --enable-video-opengles --enable-video-opengles2 \
        --disable-video-kmsdrm --disable-video-x11 --disable-video-wayland \
        --disable-video-vulkan --disable-video-opengl --disable-video-rpi \
        --disable-pulseaudio --disable-pipewire --disable-jack --disable-sndio \
        --disable-esd --disable-arts --disable-nas --enable-alsa && \
    grep -E "Video drivers|Input drivers|Audio drivers" -A1 config.log | head -6; \
    make -j"$(nproc)" && \
    mkdir -p /out && cp -L build/.libs/libSDL2-2.0.so.0 /out/ && \
    file /out/libSDL2-2.0.so.0 && \
    grep -c MALI include/SDL_config.h

FROM scratch AS export
COPY --from=builder /out/ /

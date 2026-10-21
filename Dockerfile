# syntax=docker/dockerfile:1.6
#
# Miyoo Mini cross toolchain (the Buildroot SDK OnionOS and MinUI build with:
# GCC 8.3, glibc 2.28, Cortex-A7 hard float). scripts/build-miyoo.sh builds
# this image once and runs `make miyoo` inside it with the source mounted.

FROM --platform=linux/amd64 debian:bookworm-slim

ARG TOOLCHAIN_URL=https://github.com/shauninman/miyoomini-toolchain-buildroot/releases/download/v0.0.3/miyoomini-toolchain.tar.xz

RUN apt-get update && apt-get install -y --no-install-recommends \
		ca-certificates curl xz-utils make file \
	&& rm -rf /var/lib/apt/lists/*

RUN curl -fL --retry 3 "$TOOLCHAIN_URL" | tar -xJ -C /opt \
	&& test -x /opt/miyoomini-toolchain/usr/bin/arm-linux-gnueabihf-gcc

ENV PATH=/opt/miyoomini-toolchain/usr/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
WORKDIR /src

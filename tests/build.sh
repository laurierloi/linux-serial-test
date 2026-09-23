#!/bin/sh
set -eu
revision=$(git rev-parse HEAD)
mkdir -p build
for architecture in host target; do
    case "$architecture" in
        host) compiler=gcc ;;
        target) compiler=arm-linux-gnueabihf-gcc ;;
    esac
    "$compiler" -std=gnu11 -O2 -Wall -Wextra -Werror -static -Wl,--build-id=sha1 \
        "-DSERIAL_TEST_VERSION=\"$revision\"" linux-serial-test.c -o "build/linux-serial-test-$architecture"
done

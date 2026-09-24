#!/bin/sh
# Run the exact static release executable against the PTY regression harness.
exec qemu-arm /src/build/linux-serial-test-target "$@"

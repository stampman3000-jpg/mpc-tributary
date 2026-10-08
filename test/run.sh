#!/bin/sh
# Builds and runs the probe's host test in a Linux container. The probe uses usbfs and sysfs
# headers that macOS does not have, so this cannot run natively on a Mac.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
docker run --rm -v "$here":/w -w /w gcc:12 sh -c '
  gcc -std=gnu11 -O1 -Wall -Wextra -Wno-unused-parameter -pthread -o /tmp/overprobe_test test/host_test.c &&
  /tmp/overprobe_test'

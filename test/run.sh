#!/bin/sh
# Builds and runs the probe's host tests in a Linux container. The probe uses usbfs and sysfs
# headers that macOS does not have, so this cannot run natively on a Mac. Four suites run in
# one chain, each built then run before the next: the probe decisions (host_test), the
# capture clock (capture_test), the project-state save/restore path (state_test), and the
# state-restore regression (tributary-regression). Any failed build or run stops the chain.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
CC="gcc -std=gnu11 -O1 -Wall -Wextra -Wno-unused-parameter -pthread"
docker run --rm -v "$here":/w -w /w gcc:12 sh -c "
  $CC -o /tmp/overprobe_test test/host_test.c src/capture.c -lm &&
  /tmp/overprobe_test &&
  $CC -o /tmp/capture_test test/capture_test.c src/capture.c -lm &&
  /tmp/capture_test &&
  $CC -o /tmp/state_test test/state_test.c src/capture.c -lm &&
  /tmp/state_test &&
  $CC -o /tmp/tributary_regression test/tributary-regression.c src/capture.c -lm &&
  /tmp/tributary_regression"

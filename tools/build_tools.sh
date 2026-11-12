#!/bin/sh
# Build the engine's tools and tests for this computer (cc). Same flags as fm1-base's host build: -ffp-contract=off.
set -e
cd "$(dirname "$0")/.."
CC="${CC:-cc}"
FL="-O2 -ffp-contract=off -std=c99 -Wall -Wextra -Wno-unused-function -DPLAT_HOST -Idsp -Ifm1-base/include"
mkdir -p build
SRC="dsp/organ.c dsp/tuning.c dsp/reverb.c tools/host_stub.c"
$CC $FL -o build/calibrate tools/calibrate.c $SRC -lm
$CC $FL -o build/engine_test tests/engine_test.c $SRC -lm
# KIST-P: the same organ, the plate in the room's place (dsp/plate.c gives reverb.h's interface)
$CC $FL -DKIST_PLATE=1 -o build/engine_test_p tests/engine_test.c dsp/organ.c dsp/tuning.c dsp/plate.c tools/host_stub.c -lm
echo "tools: build/calibrate build/engine_test build/engine_test_p"

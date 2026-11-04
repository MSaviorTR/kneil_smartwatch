#!/bin/sh
# Host tests (no ESP-IDF or hardware needed):
#   1. unit tests for the frame parser and calibration maths
#   2. contract test: frames from tools/sensor_mcu_emulator.py must parse
#      with the exact firmware parser
set -e
cd "$(dirname "$0")"
mkdir -p build

CFLAGS="-std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -I../../main"

cc $CFLAGS test_main.c ../../main/sensor_protocol.c ../../main/calibration.c -lm -o build/host_tests
cc $CFLAGS parse_stdin.c ../../main/sensor_protocol.c -lm -o build/parse_stdin

./build/host_tests

echo "contract: 200 emulator frames, 10% deliberately corrupted"
python3 ../../tools/sensor_mcu_emulator.py --stdout --rate 1000 --count 200 \
        --scenario spike --corrupt 0.1 | ./build/parse_stdin

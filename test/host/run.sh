#!/bin/sh
# Host tests (no ESP-IDF or hardware needed):
#   1. unit tests for the frame parser and calibration maths
#   2. unit tests for app_state (the data hub) with FreeRTOS/NVS stubs
#   3. contract test: frames from tools/sensor_mcu_emulator.py must parse
#      with the exact firmware parser
set -e
cd "$(dirname "$0")"
mkdir -p build

CFLAGS="-std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -I../../main"

cc $CFLAGS test_main.c ../../main/sensor_protocol.c ../../main/calibration.c -lm -o build/host_tests
cc $CFLAGS parse_stdin.c ../../main/sensor_protocol.c -lm -o build/parse_stdin

# app_state needs ESP-IDF headers: stand-ins live in stubs/ and ../sim/stubs/
cc $CFLAGS -std=gnu11 -Istubs -I../sim/stubs \
   -DCONFIG_GW_LINK_TIMEOUT_MS=5000 -DCONFIG_GW_K_ALERT_DEFAULT_CENTI_MM=600 \
   -DCONFIG_GW_K_ALERT_HYSTERESIS_CENTI_MM=30 -DCONFIG_GW_K_ALERT_HOLD_S=5 \
   test_app_state.c ../../main/app_state.c ../../main/calibration.c -lm -o build/app_state_tests

./build/host_tests
./build/app_state_tests

echo "contract: 200 emulator frames, 10% deliberately corrupted"
python3 ../../tools/sensor_mcu_emulator.py --stdout --rate 1000 --count 200 \
        --scenario spike --corrupt 0.1 | ./build/parse_stdin

#!/bin/sh
# Renders the watch UI on this computer and writes PNG screenshots to
# test/sim/out/. Needs only the system C compiler (no ESP-IDF, no watch).
# LVGL comes from managed_components/, so run `idf.py build` once first.
set -e
cd "$(dirname "$0")"

LVGL=../../managed_components/lvgl__lvgl
[ -d "$LVGL" ] || { echo "run 'idf.py build' once so LVGL is downloaded"; exit 1; }

mkdir -p build/obj out
CFLAGS="-std=gnu11 -O1 -w -DLV_CONF_INCLUDE_SIMPLE -D_POSIX_C_SOURCE=200809L -I. -I$LVGL -Istubs -I../../main -I../../main/ui -I../../components/kneil_proto \
        -DCONFIG_GW_LINK_UART_NUM=1 -DCONFIG_GW_LINK_RX_GPIO=44 -DCONFIG_GW_LINK_BAUD=115200"

# LVGL is compiled once into a static library and reused.
if [ ! -f build/liblvgl.a ]; then
    echo "compiling LVGL (first run only)..."
    export CFLAGS
    find "$LVGL/src" -name '*.c' | xargs -P 8 -n 1 sh -c \
        'obj="build/obj/$(echo "$1" | sed "s|.*/src/||; s|/|_|g; s|\\.c\$|.o|")"; cc $CFLAGS -c "$1" -o "$obj"' _
    ar rcs build/liblvgl.a build/obj/*.o
fi

cc $CFLAGS sim_main.c therapy_stub.c ../../main/calibration.c ../../main/kneil_state.c ../../components/kneil_proto/kneil_proto.c ../../main/ui/*.c build/liblvgl.a -lm -o build/ui_sim

for s in live alert demo uncal nodata kneil therapy; do
    echo "scenario: $s"
    ./build/ui_sim "$s" out
done

# PNGs on macOS (sips); elsewhere the BMPs are left as they are.
if command -v sips >/dev/null 2>&1; then
    for f in out/*.bmp; do
        sips -s format png "$f" --out "${f%.bmp}.png" >/dev/null && rm "$f"
    done
fi

echo "screenshots in $(pwd)/out"

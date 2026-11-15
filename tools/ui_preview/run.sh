#!/usr/bin/env bash
# Render the launcher screens on the PC (needs gcc, python3 and a built tree so that
# managed_components/lvgl__lvgl exists). Output: build_preview/*.png
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$HERE/../.."
LVGL="$ROOT/managed_components/lvgl__lvgl"
[ -d "$LVGL/src" ] || { echo "run 'idf.py build' once so LVGL is downloaded"; exit 1; }
OUT="$HERE/build_preview"; mkdir -p "$OUT/obj"
# host lv_conf.h from LVGL's template
sed -e '0,/^#if 0/s//#if 1/' \
    -e 's/#define LV_COLOR_DEPTH .*/#define LV_COLOR_DEPTH 16/' \
    -e 's/#define LV_USE_STDLIB_MALLOC .*/#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB/' \
    -e 's/#define LV_USE_STDLIB_STRING .*/#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB/' \
    -e 's/#define LV_USE_STDLIB_SPRINTF .*/#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB/' \
    -e 's/#define LV_FONT_MONTSERRAT_20 .*/#define LV_FONT_MONTSERRAT_20 1/' \
    -e 's/#define LV_FONT_MONTSERRAT_28 .*/#define LV_FONT_MONTSERRAT_28 1/' \
    "$LVGL/lv_conf_template.h" > "$OUT/lv_conf.h"
CFLAGS="-O1 -w -DLV_CONF_INCLUDE_SIMPLE -DUI_HOST -I$OUT -I$LVGL -I$LVGL/src -I$HERE/stubs -I$ROOT/main"
OBJS=()
while read -r f; do
    o="$OUT/obj/$(echo "${f#$LVGL/}" | tr '/' '_').o"
    [ "$o" -nt "$f" ] 2>/dev/null || gcc $CFLAGS -c "$f" -o "$o" &
    OBJS+=("$o")
    [ "$(jobs -r | wc -l)" -ge 4 ] && wait -n
done < <(find "$LVGL/src" -name '*.c' | sort)
wait
gcc $CFLAGS "$HERE/preview.c" "$ROOT/main/ui.c" "$ROOT/main/apps_logic.c" "${OBJS[@]}" -lm -o "$OUT/preview"
(cd "$OUT" && OUT_DIR="$OUT" ./preview)
python3 -I "$HERE/to_png.py" "$OUT"

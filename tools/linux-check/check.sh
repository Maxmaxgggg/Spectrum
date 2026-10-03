#!/bin/bash
# check.sh [БАЗА] — полная проверка без MSVC и без видеокарты:
#   тесты (GPU-тесты сами пропускаются), приложение, окно без экрана,
#   именование; с БАЗОЙ — ещё compare.sh (ядра по машинному коду и
#   эталонный дамп против коммита БАЗА).
# Журналы — в $WORK/logs.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
L="$WORK/logs"; mkdir -p "$L"

"$KIT/build-tests.sh" "$WORK/tests" > "$L/build-tests.log" 2>&1 \
    || { echo "СБОРКА ТЕСТОВ УПАЛА"; grep -m 20 -E 'error|ошибка' "$L/build-tests.log"; exit 1; }
"$WORK/tests/SpectrumTests" > "$L/tests.log" 2>&1 || true
grep -A3 "ПРОВАЛ" "$L/tests.log" | head -40 || true
echo "тесты: $(tail -1 "$L/tests.log")"

"$KIT/build-app.sh" "$WORK/app" > "$L/build-app.log" 2>&1 \
    || { echo "СБОРКА ПРИЛОЖЕНИЯ УПАЛА"; grep -m 20 -E 'error|ошибка' "$L/build-app.log"; exit 1; }
if "$KIT/gui-smoke.sh" "$WORK/app" > "$L/gui.log" 2>&1; then echo "окно: всё ok"
else echo "ОКНО: ПРОВАЛ"; cat "$L/gui.log"; fi

"$KIT/tidy.sh" > "$L/tidy.log" 2>&1 || true
echo "именование: предупреждений $(grep -c 'warning:' "$L/tidy.log" || true)"

if [ -n "$1" ]; then "$KIT/compare.sh" "$1"; fi

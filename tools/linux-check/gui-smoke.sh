#!/bin/bash
# gui-smoke.sh [КАТАЛОГ ПРИЛОЖЕНИЯ] — окно без экрана (QT_QPA_PLATFORM=offscreen):
# gui_smoke.cpp нажимает кнопки, как пользователь, и сверяет спектр на экране.
# Нужна сборка build-app.sh; объекты берутся оттуда, кроме main.cpp.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
APP=$(cd "${1:-$WORK/app}" && pwd)
S="$APP/src"
QT="$(pkg-config --cflags Qt5Widgets Qt5PrintSupport) -fPIC"
g++ -std=c++14 -O1 -fopenmp -include "$KIT/compat.h" $QT -I"$S" -I"$S/compute" -I"$S/storage" -I"$S/ui" \
    -I"$S/ui/icons" -I"$APP/gen" -w -c "$KIT/gui_smoke.cpp" -o "$APP/gui_smoke.o"
g++ -fopenmp "$APP/gui_smoke.o" $(ls "$APP"/obj/*.o | grep -v '/main.cpp.o$') "$WORK/qcustomplot.o" \
    -o "$APP/gui_smoke" $(pkg-config --libs Qt5Widgets Qt5PrintSupport) -lgmpxx -lgmp \
    -L"$CUDA/lib64" -lcudart -Wl,-rpath,"$CUDA/lib64"
cd "$APP" && QT_QPA_PLATFORM=offscreen timeout 180 ./gui_smoke

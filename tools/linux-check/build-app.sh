#!/bin/bash
# build-app.sh [КАТАЛОГ] — само приложение под Linux, по списку Spectrum.vcxproj.
# Интерфейс собирается без пути к заголовкам CUDA: он не должен от них
# зависеть. Объекты (кроме main.cpp) нужны ещё и gui-smoke.sh.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
OUT=$(mkdir -p "${1:-$WORK/app}" && cd "${1:-$WORK/app}" && pwd)
S="$OUT/src"
prepare_source "${SRC:-$REPO}" "$S"
GEN="$OUT/gen"; OBJ="$OUT/obj"; mkdir -p "$GEN" "$OBJ"
rm -f "$OBJ"/*.o

QT="$(pkg-config --cflags Qt5Widgets Qt5PrintSupport) -fPIC"
INC_UI="-I$S -I$S/compute -I$S/storage -I$S/ui -I$S/ui/icons -I$GEN"
INC="$INC_UI -I$CUDA/include"
CXX="g++ -std=c++14 -O1 -fopenmp -include $KIT/compat.h $QT"
CU="clang++ -include $KIT/compat_cu.h -x cuda --cuda-path=$CUDA --cuda-gpu-arch=sm_86 -std=c++14 -O1 -Wno-unknown-cuda-version $INC -w"
PROJ="$S/Spectrum.vcxproj"

for u in $(project_items "$PROJ" QtUic); do
    /usr/lib/qt5/bin/uic "$u" -o "$GEN/ui_$(basename "$u" .ui).h"
done
for r in $(project_items "$PROJ" QtRcc); do
    /usr/lib/qt5/bin/rcc "$r" -o "$GEN/qrc_$(basename "$r" .qrc).cpp"
    bg $CXX $INC_UI -w -c "$GEN/qrc_$(basename "$r" .qrc).cpp" -o "$OBJ/qrc_$(basename "$r" .qrc).o"
done
for h in $(project_items "$PROJ" QtMoc); do
    m="$GEN/moc_$(basename "$h" .h).cpp"
    /usr/lib/qt5/bin/moc $INC "$h" -o "$m"
    bg $CXX $INC -w -c "$m" -o "$OBJ/moc_$(basename "$h" .h).o"
done
# qcustomplot — 35 тыс. строк, не меняется: объект переиспользуется, пока
# совпадает исходник.
QCP="$WORK/qcustomplot.o"
for f in $(project_items "$PROJ" ClCompile); do
    rel=$(realpath --relative-to="$S" "$f")
    if [ "$rel" = qcustomplot.cpp ]; then
        if [ ! -f "$QCP" ] || ! cmp -s "$f" "$WORK/qcustomplot.cpp"; then
            $CXX $INC -w -c "$f" -o "$QCP" && cp "$f" "$WORK/qcustomplot.cpp"
        fi
        continue
    fi
    case "$rel" in
        compute/*|storage/*) flags="$INC" ;;
        *)                   flags="$INC_UI" ;;
    esac
    bg $CXX $flags -w -c "$f" -o "$OBJ/$(echo "$rel" | tr / _).o"
done
for f in $(project_items "$PROJ" CudaCompile); do
    bg $CU --cuda-host-only -c "$f" -o "$OBJ/$(basename "$f").o"
done
wait_all || { echo "СБОРКА ПРИЛОЖЕНИЯ УПАЛА"; exit 1; }
g++ -fopenmp "$OBJ"/*.o "$QCP" -o "$OUT/Spectrum" $(pkg-config --libs Qt5Widgets Qt5PrintSupport) \
    -lgmpxx -lgmp -L"$CUDA/lib64" -lcudart -Wl,-rpath,"$CUDA/lib64"
echo "собрано: $OUT/Spectrum"

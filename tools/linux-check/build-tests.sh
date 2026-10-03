#!/bin/bash
# build-tests.sh [КАТАЛОГ] — SpectrumTests под Linux: g++ для .cpp, clang для
# .cu (хостовая часть — в объект; устройство — в PTX и через ptxas sm_86 в
# cubin, рядом с объектами: по нему сравниваются ядра, см. compare.sh).
# Исходники — из $SRC (по умолчанию сам репозиторий).
#
# Запуск одного теста: ONLY=testBzCyclic КАТАЛОГ/SpectrumTests. Вставка
# разбора ONLY делается только в копии, в репозиторий она не попадает.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
OUT=$(mkdir -p "${1:-$WORK/tests}" && cd "${1:-$WORK/tests}" && pwd)
S="$OUT/src"
prepare_source "${SRC:-$REPO}" "$S"

python3 - "$S/tests/main.cpp" <<'PY'
import re, sys
p = sys.argv[1]; s = open(p, encoding='utf-8').read()
anchor = '    const QStringList args = app.arguments();\n'
names = re.findall(r'^static void (test\w+)\(\)', s, re.M)
calls = ''.join('        if (only == QLatin1String("%s")) %s();\n' % (n, n) for n in names)
s = s.replace(anchor, anchor + '    if (qEnvironmentVariableIsSet("ONLY")) {\n'
              '        const QString only = qEnvironmentVariable("ONLY");\n' + calls +
              '        g_out << "pass " << g_passed << " fail " << g_failed << Qt::endl;\n'
              '        return g_failed ? 1 : 0;\n    }\n', 1)
open(p, 'w', encoding='utf-8').write(s)
PY

OBJ="$OUT/obj"; mkdir -p "$OBJ"
QT="$(pkg-config --cflags Qt5Core) -fPIC"
INC="-I$S -I$S/compute -I$S/storage -I$S/tests -I$S/ui -I$CUDA/include"
CXX="g++ -std=c++14 -O2 -fopenmp -include $KIT/compat.h $QT $INC -w"
CU="clang++ -include $KIT/compat_cu.h -x cuda --cuda-path=$CUDA --cuda-gpu-arch=sm_86 -std=c++14 -O2 -Wno-unknown-cuda-version $INC -w"
PROJ="$S/tests/SpectrumTests.vcxproj"

objs=()
for h in $(project_items "$PROJ" QtMoc); do
    m="$OBJ/moc_$(basename "$h" .h).cpp"
    /usr/lib/qt5/bin/moc $INC "$h" -o "$m"
    bg $CXX -c "$m" -o "$m.o"; objs+=("$m.o")
done
for f in $(project_items "$PROJ" ClCompile); do
    o="$OBJ/$(realpath --relative-to="$S" "$f" | tr / _).o"
    bg $CXX -c "$f" -o "$o"; objs+=("$o")
done
for f in $(project_items "$PROJ" CudaCompile); do
    b="$OBJ/$(basename "$f")"
    bg $CU --cuda-host-only -c "$f" -o "$b.o"; objs+=("$b.o")
    bg bash -c "$CU --cuda-device-only -S '$f' -o '$b.ptx' && '$CUDA/bin/ptxas' -arch=sm_86 '$b.ptx' -o '$b.cubin'"
done
wait_all || { echo "СБОРКА ТЕСТОВ УПАЛА"; exit 1; }
g++ -fopenmp "${objs[@]}" -o "$OUT/SpectrumTests" $(pkg-config --libs Qt5Core) -lgmpxx -lgmp \
    -L"$CUDA/lib64" -lcudart -Wl,-rpath,"$CUDA/lib64"
echo "собрано: $OUT/SpectrumTests"

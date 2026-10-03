#!/bin/bash
# tidy.sh — проверка имён по .clang-tidy репозитория (правила — NAMING.md):
# clang-tidy по всем .cpp и хостовой части .cu. Печатает предупреждения;
# ноль строк — всё по правилам.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
T="$WORK/tidy"; S="$T/src"; GEN="$T/gen"
prepare_source "$REPO" "$S"
mkdir -p "$GEN"
for u in "$S"/ui/*.ui; do /usr/lib/qt5/bin/uic "$u" -o "$GEN/ui_$(basename "$u" .ui).h"; done
cp "$REPO/.clang-tidy" "$S/.clang-tidy"
OMP=$(dirname "$(gcc -print-file-name=include/omp.h)")
python3 - "$S" "$GEN" "$KIT" "$CUDA" "$OMP" <<'PY'
import glob, json, subprocess, sys
src, gen, kit, cuda, omp = sys.argv[1:6]
qt = subprocess.check_output(['pkg-config', '--cflags', 'Qt5Widgets', 'Qt5PrintSupport']).decode().strip()
inc = (f"-I{src} -I{src}/compute -I{src}/storage -I{src}/tests -I{src}/ui -I{src}/ui/icons -I{gen} "
       f"-I{cuda}/include -isystem {omp}")
cmds = []
for f in sorted(glob.glob(src + '/**/*.cpp', recursive=True)):
    if 'qcustomplot' in f: continue
    cmds.append({"directory": src, "file": f,
                 "command": f"clang++ -std=c++14 -fopenmp -fPIC -w {qt} {inc} -include {kit}/compat.h -c {f}"})
for f in sorted(glob.glob(src + '/compute/*.cu')):
    cmds.append({"directory": src, "file": f,
                 "command": f"clang++ -x cuda --cuda-path={cuda} --cuda-gpu-arch=sm_86 --cuda-host-only "
                            f"-std=c++14 -Wno-unknown-cuda-version -w -fPIC {qt} {inc} -include {kit}/compat_cu.h -c {f}"})
json.dump(cmds, open(src + '/compile_commands.json', 'w'), indent=1)
PY
rm -rf "$T/logs"; mkdir -p "$T/logs"
cd "$S"
python3 -c "import json;[print(c['file']) for c in json.load(open('compile_commands.json'))]" | \
    xargs -P "$(nproc)" -I{} sh -c 'clang-tidy -p . --header-filter="'"$S"'/((compute|storage|tests|ui)/|(constants|settings|types)\.h)" "{}" > "'"$T"'/logs/$(echo "{}" | md5sum | cut -c1-8).log" 2>&1'
cat "$T"/logs/*.log | grep -E 'warning:' | grep -v qcustomplot | sed "s#$S/##" | sort -u

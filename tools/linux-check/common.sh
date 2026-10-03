# Общее для скриптов проверки: пути, копия исходников с правками под Linux,
# списки файлов из проектов Visual Studio — набор файлов сборки берётся из
# них же, поэтому файл, забытый в .vcxproj, здесь тоже не соберётся.
set -e
KIT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=${REPO:-$(cd "$KIT/../.." && pwd)}
CUDA=${CUDA_ROOT:-/tmp/cuda}
WORK=${WORK:-/tmp/spectrum-check}

# prepare_source ИЗ КУДА — копия без .git. На Linux uint64_t — unsigned long,
# а quint64 — unsigned long long; на Windows это один тип. Чтобы сигнатуры
# совпали, как там, uint64_t заменяется везде (u64ll_ — в compat.h).
prepare_source() {
    mkdir -p "$2"
    rsync -a --delete --exclude .git --exclude '*.i' --exclude tools "$1/" "$2/"
    find "$2" \( -name '*.cpp' -o -name '*.h' -o -name '*.cu' -o -name '*.cuh' \) ! -name 'qcustomplot*' \
        -exec sed -i -E 's/\buint64_t\b/u64ll_/g' {} +
    # Нужно только старым коммитам (compare.sh): там было то, что GCC не
    # принимает, а MSVC — да.
    sed -i 's/unsigned __int128(random)/(unsigned __int128)(random)/' "$2/compute/leon.cpp" 2>/dev/null || true
    sed -i '/std::atomic_bool requestRunState = false;/d' "$2/compute/worker.h" 2>/dev/null || true
}

# project_items ПРОЕКТ ВИД — абсолютные пути элементов вида ВИД (ClCompile,
# QtMoc, CudaCompile, QtUic, QtRcc) из .vcxproj.
project_items() {
    python3 - "$1" "$2" <<'PY'
import os, re, sys
proj, kind = sys.argv[1], sys.argv[2]
base = os.path.dirname(os.path.abspath(proj))
text = open(proj, encoding='utf-8-sig').read()
for m in re.finditer(r'<%s Include="([^"]+)"' % kind, text):
    print(os.path.normpath(os.path.join(base, m.group(1).replace('\\', '/'))))
PY
}

# Параллельные задачи: bg КОМАНДА…, потом wait_all — падает, если упала
# хоть одна (простой wait ошибку фоновой задачи теряет).
PIDS=()
bg() { "$@" & PIDS+=($!); }
wait_all() {
    local failed=0
    for p in "${PIDS[@]}"; do wait "$p" || failed=1; done
    PIDS=()
    return $failed
}

#!/bin/bash
# compare.sh БАЗА — что изменилось относительно коммита (ветки, тега) БАЗА:
#   1) машинный код ядер — по функциям cubin (sm_86): совпало, отличается,
#      новое, пропало. Так видно, что правка не задела ядра, которых не
#      касалась, — запустить их здесь не на чем;
#   2) эталонный дамп спектров (SpectrumTests --dump) — побайтно.
# Собирает обе версии заново: базу в $WORK/base, текущую — в $WORK/tests.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
BASE=${1:?укажите коммит для сравнения}
B="$WORK/base"; mkdir -p "$B"
rm -rf "$B/repo"; mkdir -p "$B/repo"
git -C "$REPO" archive "$BASE" | tar -x -C "$B/repo"
SRC="$B/repo" "$KIT/build-tests.sh" "$B/tests" > "$B/build.log" 2>&1 \
    || { echo "БАЗА НЕ СОБРАЛАСЬ"; tail -30 "$B/build.log"; exit 1; }
"$KIT/build-tests.sh" "$WORK/tests" > "$WORK/tests-build.log" 2>&1 \
    || { echo "ТЕКУЩАЯ НЕ СОБРАЛАСЬ"; tail -30 "$WORK/tests-build.log"; exit 1; }

python3 - "$B/tests/obj" "$WORK/tests/obj" <<'PY'
import glob, os, re, subprocess, sys
def functions(path):
    out = subprocess.run(['readelf', '-S', '-W', path], capture_output=True, text=True).stdout
    data = open(path, 'rb').read()
    res = {}
    for line in out.splitlines():
        m = re.search(r'\]\s+\.text\.(\S+)\s+\S+\s+[0-9a-f]+\s+([0-9a-f]+)\s+([0-9a-f]+)', line)
        if m:
            off, size = int(m.group(2), 16), int(m.group(3), 16)
            res[m.group(1)] = data[off:off + size]
    return res
def demangle(names):
    if not names: return []
    return subprocess.run(['c++filt'], input='\n'.join(names), capture_output=True, text=True).stdout.splitlines()
base_dir, cur_dir = sys.argv[1], sys.argv[2]
for cubin in sorted(glob.glob(os.path.join(cur_dir, '*.cubin'))):
    name = os.path.basename(cubin)
    old_path = os.path.join(base_dir, name)
    if not os.path.exists(old_path):
        print('%s: в базе нет' % name); continue
    old, new = functions(old_path), functions(cubin)
    same    = [k for k in old if k in new and old[k] == new[k]]
    changed = [k for k in old if k in new and old[k] != new[k]]
    gone    = [k for k in old if k not in new]
    added   = [k for k in new if k not in old]
    # Ядро сменило имя (скажем, получило параметр шаблона), а код тот же —
    # такое сопоставляется по самому машинному коду.
    by_code = {}
    for k in added:
        by_code.setdefault(new[k], []).append(k)
    renamed = []
    for k in list(gone):
        if by_code.get(old[k]):
            renamed.append((k, by_code[old[k]].pop(0)))
            gone.remove(k)
    added = [k for k in added if all(k != r[1] for r in renamed)]
    print('%s: совпало %d, тот же код под новым именем %d, отличается %d, пропало %d, новых %d'
          % (name, len(same), len(renamed), len(changed), len(gone), len(added)))
    for title, names in (('отличается', changed), ('пропало', gone), ('новое', added)):
        shown = demangle(names)
        for d in shown[:20]:
            print('    %s: %s' % (title, d))
        if len(shown) > 20:
            print('    … ещё %d' % (len(shown) - 20))
PY

"$B/tests/SpectrumTests" --dump "$B/dump.txt" > /dev/null 2>&1 || true
"$WORK/tests/SpectrumTests" --dump "$WORK/dump.txt" > /dev/null 2>&1 || true
if cmp -s "$B/dump.txt" "$WORK/dump.txt"; then echo "эталонный дамп: совпадает"
else echo "эталонный дамп: ОТЛИЧАЕТСЯ"; diff "$B/dump.txt" "$WORK/dump.txt" | head -20; fi

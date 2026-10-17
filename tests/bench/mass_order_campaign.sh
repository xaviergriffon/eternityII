#!/usr/bin/env bash
# Exploration par masse (docs/conception/etude_arbre_expansion_profonde.md) : sur
# des clones neufs, explorer les racines d'une coupe par masse décroissante
# (sondes comprises) trouve-t-il la solution plus tôt que la production ?
# Protocole et règle écrits AVANT la mesure ; ce script les applique tels quels.
#
#   tests/bench/mass_order_campaign.sh <répertoire> [instances par cellule] [première graine]
#
# Attend tests/bench/bench_mass_order_100 et _144 (cible `make bench-mass-order`).
# Les instances tournent en parallèle (MO_JOBS, défaut nproc/2) ; les quatre bras
# d'une instance, eux, se suivent dans le même processus.
set -euo pipefail
WORK=${1:?répertoire de travail}
N=${2:-30}
FIRST=${3:-61}
JOBS=${MO_JOBS:-$(( $(nproc) / 2 ))}
HERE=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$WORK/clones" "$WORK/runs"
OUT="$WORK/mo.txt"
LIST="$WORK/instances.txt"
: > "$LIST"
for cell in "10 14" "10 13" "12 22" "12 20"; do
    set -- $cell
    n=$1; k=$2
    for seed in $(seq "$FIRST" $((FIRST + N - 1))); do
        tag="${n}_${k}_${seed}"
        [ -f "$WORK/clones/solution_$tag.txt" ] || python3 "$HERE/tools/gen_clone.py" --size "$n" \
            --inner-colours "$k" --seed "$seed" --hints 5 --out-dir "$WORK/clones" > /dev/null
        echo "$n $k $seed" >> "$LIST"
    done
done
run_one() {
    n=$1; k=$2; seed=$3; tag="${n}_${k}_${seed}"
    mkdir -p "$WORK/runs/$tag"
    "$HERE/tests/bench/bench_mass_order_$((n * n))" --pieces "$WORK/clones/pieces_$tag.csv" \
        --indices-file "$WORK/clones/indices_$tag.csv" --solution "$WORK/clones/solution_$tag.txt" \
        --workdir "$WORK/runs/$tag" --label "n${n}k${k}/$seed" --seed "$seed" 2>/dev/null | grep '^MO '
}
export -f run_one
export WORK HERE
xargs -P "$JOBS" -L 1 bash -c 'run_one "$@"' _ < "$LIST" | tee "$OUT"
python3 - "$OUT" <<'PY'
import sys, math, statistics as st
from collections import defaultdict
rows = defaultdict(list)
for line in open(sys.argv[1]):
    f = line.split()
    kv = dict(x.split('=', 1) for x in f[2:] if '=' in x)
    if 'erreur' in kv:
        continue
    rows[f[1].split('/')[0]].append(kv)
def sign_p(k, n):
    lo = min(k, n - k)
    return 1.0 if n == 0 else min(1.0, 2 * sum(math.comb(n, i) for i in range(lo + 1)) / 2 ** n)
def tally(r, key):
    w = sum(1 for x in r if x[key] == '1'); l = sum(1 for x in r if x[key] == '-1')
    return w, l, len(r) - w - l, sign_p(w, w + l)
def med_time(r, arm):
    return st.median(float(x[arm].split(':')[1]) for x in r)
def solved(r, arm):
    return sum(1 for x in r if x[arm].startswith('R'))
print("\ncellule | bras résolus A/B/C/D | temps médian A / D (s) | D contre A : gagne-perd-indécis, p | D contre C | B contre A | C contre A | verdict")
for cell in sorted(rows):
    r = rows[cell]
    da, dc, ba, ca = (tally(r, k) for k in ("DA", "DC", "BA", "CA"))
    ok = da[0] > da[1] and da[3] < 0.01 and dc[0] > dc[1] and dc[3] < 0.01
    verdict = "D ADOPTABLE" if ok else ("D PERD contre A" if da[1] > da[0] and da[3] < 0.01 else "non adoptable")
    print(f"{cell} | {solved(r,'A')}/{solved(r,'B')}/{solved(r,'C')}/{solved(r,'D')} sur {len(r)} | "
          f"{med_time(r,'A'):.3f} / {med_time(r,'D'):.3f} | {da[0]}-{da[1]}-{da[2]}, p={da[3]:.2g} | "
          f"{dc[0]}-{dc[1]}-{dc[2]}, p={dc[3]:.2g} | {ba[0]}-{ba[1]}-{ba[2]}, p={ba[3]:.2g} | "
          f"{ca[0]}-{ca[1]}-{ca[2]}, p={ca[3]:.2g} | {verdict}")
    print(f"   rang médian de la racine de la solution dans l'ordre D : "
          f"{st.median(int(x['rang_solution']) / int(x['racines']) for x in r if int(x['rang_solution']) >= 0):.3f} ; "
          f"sondes médianes {st.median(float(x['sondes_s']) for x in r):.3f} s")
PY

#!/usr/bin/env bash
# Campagne S5 (docs/conception/etude_arbre_expansion_profonde.md) : sur des clones
# à solution plantée, la racine de la solution est-elle mieux classée par sa masse
# estimée que par le hasard ? Protocole et règle écrits AVANT la mesure ; ce script
# les applique tels quels, sans paramètre de décision.
#
#   tests/bench/s5_campaign.sh <répertoire de travail> [instances par cellule] [sondes] [première graine]
#
# S5_PRIMARY=rang (défaut : masse à la profondeur primaire) ou rang_mi (mi-profondeur,
# score primaire de la campagne de confirmation) : le score sur lequel la règle décide.
# Attend tests/bench/bench_sample_100 et _144 (cible `make bench-s5`).
set -euo pipefail
WORK=${1:?répertoire de travail}
N=${2:-30}
PROBES=${3:-50}
FIRST=${4:-1}
export S5_PRIMARY=${S5_PRIMARY:-rang}
WORKERS=${S5_WORKERS:-$(nproc)}
HERE=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$WORK/clones"
OUT="$WORK/s5.txt"
: > "$OUT"
# Cellules calibrées du §6.4 de docs/conception/banc_resolution_clones.md.
for cell in "10 14" "10 13" "12 22" "12 20"; do
    set -- $cell
    n=$1; k=$2; parts=$((n * n))
    for seed in $(seq "$FIRST" $((FIRST + N - 1))); do
        tag="${n}_${k}_${seed}"
        if [ ! -f "$WORK/clones/solution_$tag.txt" ]; then
            python3 "$HERE/tools/gen_clone.py" --size "$n" --inner-colours "$k" --seed "$seed" --hints 5 \
                --out-dir "$WORK/clones" > /dev/null
        fi
        "$HERE/tests/bench/bench_sample_$parts" --pieces "$WORK/clones/pieces_$tag.csv" \
            --indices-file "$WORK/clones/indices_$tag.csv" --solution "$WORK/clones/solution_$tag.txt" \
            --label "n${n}k${k}/$seed" --probe "$PROBES" --seed "$seed" --workers "$WORKERS" 2>/dev/null \
            | grep '^S5 ' | tee -a "$OUT"
    done
done
python3 - "$OUT" <<'PY'
import os, sys, math, statistics as st
PRIMARY = os.environ.get("S5_PRIMARY", "rang")
RHO = {"rang": "spearman", "rang_mi": "spearman_mi"}[PRIMARY]
from collections import defaultdict
rows = defaultdict(list)
errors = defaultdict(int)
for line in open(sys.argv[1]):
    f = line.split()
    cell = f[1].split('/')[0]
    kv = dict(x.split('=', 1) for x in f[2:] if '=' in x)
    if 'erreur' in kv:
        errors[cell] += 1
        continue
    rows[cell].append(kv)
def sign_p(k, n):
    lo = min(k, n - k)
    return min(1.0, 2 * sum(math.comb(n, i) for i in range(lo + 1)) / 2 ** n)
print(f"\nscore primaire : {PRIMARY}")
print("cellule | instances | rang médian | sous 50 % | test des signes p | Spearman médian | verdict")
for cell in sorted(set(rows) | set(errors)):
    r = rows[cell]
    if not r:
        print(f"{cell} | 0 (+{errors[cell]} en erreur) | — | — | — | — | indécise")
        continue
    ranks = [float(x[PRIMARY]) for x in r]
    below = sum(1 for v in ranks if v < 0.5)
    above = sum(1 for v in ranks if v > 0.5)
    p = sign_p(below, below + above)
    med = st.median(ranks)
    rho = st.median(float(x[RHO]) for x in r)
    if med < 0.25 and p < 0.01 and below > above:
        verdict = "POSITIVE"
    elif p < 0.01 and above > below:
        verdict = "NÉGATIVE (classée PLUS BAS que le hasard)"
    else:
        verdict = "non positive"
    err = f" (+{errors[cell]} en erreur)" if errors[cell] else ""
    print(f"{cell} | {len(r)}{err} | {med:.3f} | {below}/{below + above} | {p:.2g} | {rho:.3f} | {verdict}")
    other = "rang_mi" if PRIMARY == "rang" else "rang"
    print(f"   secondaires : rang médian ({other}) {st.median(float(x[other]) for x in r):.3f}, "
          f"sur les solutions estimées {st.median(float(x['rang_sol']) for x in r):.3f}")
PY

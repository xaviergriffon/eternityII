#!/usr/bin/env bash
#
# Banc de mesure du débit de la boucle de recherche (autosearch(), src/core/etii_search.c).
#
# Principe : au lieu de compter les nœuds explorés pendant une durée fixe (mesure
# bruitée par la charge de la machine), on mesure le temps mural nécessaire pour
# explorer un nombre de nœuds FIXE (ETII_BENCH_NODES, voir static_variables.h et
# check_client_threads dans src/app/etii_client.c). En mode `test` la recherche
# est déterministe : à N fixé, le travail exploré est le même d'un run à l'autre,
# ce qui rend la mesure directement comparable entre deux runs ou deux versions
# du code.
#
# Usage :
#   tests/bench/bench_search.sh [--nodes N] [--reps R] [--pieces fichier.csv]
#                                [--out rapport.json] [--force]
#   tests/bench/bench_search.sh --baseline ancien_rapport.json [options ci-dessus]
#
set -euo pipefail

# Locale fixe : le séparateur décimal de `time`/awk/printf dépend de LC_NUMERIC
# (une locale telle que fr_FR utilise la virgule) — la comparaison numérique du
# script suppose le point décimal partout.
export LC_ALL=C

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

NODES=5000000
REPS=5
PIECES="data/pieces.csv"
OUT=""
BASELINE=""
FORCE=0
LOAD_THRESHOLD_PER_CPU="1.0"

usage() {
    cat <<'EOF'
Usage: bench_search.sh [options]

Options:
  --nodes N         Nombre de nœuds cible par run (défaut : 5000000)
  --reps R          Nombre de répétitions mesurées, hors chauffe (défaut : 5)
  --pieces FICHIER  Fichier de pièces (défaut : data/pieces.csv)
  --out FICHIER     Écrit le rapport JSON dans FICHIER (toujours affiché sur stdout)
  --baseline FICHIER Compare le nouveau rapport à un rapport JSON précédent
  --force           Ignore le refus sur machine chargée
  -h, --help        Affiche cette aide
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --nodes) NODES="$2"; shift 2 ;;
        --reps) REPS="$2"; shift 2 ;;
        --pieces) PIECES="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --baseline) BASELINE="$2"; shift 2 ;;
        --force) FORCE=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "option inconnue : $1" >&2; usage >&2; exit 1 ;;
    esac
done

if [[ -n "$BASELINE" && ! -f "$BASELINE" ]]; then
    echo "baseline introuvable : $BASELINE" >&2
    exit 1
fi

log() { echo "$@" >&2; }

# --- Détection machine : cœurs, charge, épinglage -------------------------

detect_ncpu() {
    if command -v nproc >/dev/null 2>&1; then
        nproc
    elif [[ "$(uname -s)" == "Darwin" ]]; then
        sysctl -n hw.ncpu
    else
        getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1
    fi
}

detect_load1() {
    if [[ -r /proc/loadavg ]]; then
        awk '{print $1}' /proc/loadavg
    else
        # macOS/BSD : `uptime` n'a pas de format stable sur toutes les versions,
        # on extrait le premier flottant qui suit "load average".
        uptime | sed -E 's/.*load averages?:? *([0-9]+([.,][0-9]+)?).*/\1/' | tr ',' '.'
    fi
}

NCPU=$(detect_ncpu)
LOAD1=$(detect_load1)

OVERLOADED=$(awk -v l="$LOAD1" -v n="$NCPU" -v t="$LOAD_THRESHOLD_PER_CPU" \
    'BEGIN { print (l > n * t) ? "1" : "0" }')

if [[ "$OVERLOADED" == "1" && $FORCE -ne 1 ]]; then
    log "Machine chargée : load average 1min=$LOAD1 pour $NCPU cœur(s)" \
        "(seuil ${LOAD_THRESHOLD_PER_CPU}×cœurs). Mesure refusée — relancez avec" \
        "--force pour outrepasser, ou attendez que la charge redescende."
    exit 2
fi

PIN_CMD=""
PIN_DESC="aucun épinglage : outil non disponible sur cette plateforme"
if command -v taskset >/dev/null 2>&1; then
    PIN_CMD="taskset -c 0"
    PIN_DESC="taskset -c 0"
elif [[ "$(uname -s)" == "Darwin" ]]; then
    PIN_DESC="aucun équivalent à taskset sur macOS — mesure non épinglée à un cœur"
fi

# --- Build release (sans ASan ni couverture) -------------------------------

log "Compilation release (make clean && make)..."
make clean >/dev/null 2>&1 || true
if ! make >/dev/null 2>&1; then
    log "échec de la compilation — voir 'make' pour le détail"
    exit 1
fi
if [[ ! -x ./eternityII ]]; then
    log "binaire ./eternityII introuvable après compilation"
    exit 1
fi

# --- Exécution d'un run -----------------------------------------------------

WORKDIR=$(mktemp -d "${TMPDIR:-/tmp}/bench_search.XXXXXX")
trap 'rm -rf "$WORKDIR"' EXIT

# Écrit dans $1 le temps réel (secondes) et dans $2 la sortie du run.
run_once() {
    local outlog="$1" timefile="$2"
    TIMEFORMAT='%R'
    if [[ -n "$PIN_CMD" ]]; then
        { time $PIN_CMD env ETII_BENCH_NODES="$NODES" ./eternityII test "$PIECES" \
            < /dev/null > "$outlog" 2>&1 ; } 2> "$timefile"
    else
        { time env ETII_BENCH_NODES="$NODES" ./eternityII test "$PIECES" \
            < /dev/null > "$outlog" 2>&1 ; } 2> "$timefile"
    fi
}

extract_nodes_reached() {
    grep -a -o 'nodes_reached=[0-9]*' "$1" | tail -1 | cut -d= -f2
}

# --- Chauffe (non comptée) --------------------------------------------------

log "Chauffe (non comptée)..."
run_once "$WORKDIR/warmup.log" "$WORKDIR/warmup.time"
warmup_nodes=$(extract_nodes_reached "$WORKDIR/warmup.log")
if [[ -z "$warmup_nodes" ]]; then
    log "la cible de $NODES nœuds n'a jamais été atteinte pendant la chauffe" \
        "— voir $WORKDIR/warmup.log (conservé pour diagnostic)"
    trap - EXIT
    exit 1
fi

# --- Répétitions mesurées ---------------------------------------------------

rates_file="$WORKDIR/rates"
nodes_file="$WORKDIR/nodes"
: > "$rates_file"
: > "$nodes_file"
runs_json="[]"

for ((i = 1; i <= REPS; i++)); do
    log "Run $i/$REPS..."
    outlog="$WORKDIR/run_$i.log"
    timefile="$WORKDIR/run_$i.time"
    run_once "$outlog" "$timefile"

    nodes_reached=$(extract_nodes_reached "$outlog")
    elapsed=$(tr -d '[:space:]' < "$timefile")
    if [[ -z "$nodes_reached" || -z "$elapsed" ]]; then
        log "run $i : cible non atteinte ou temps non mesuré — voir $outlog"
        exit 1
    fi

    rate=$(awk -v n="$nodes_reached" -v t="$elapsed" 'BEGIN { printf "%.6f", (t > 0) ? n / t : 0 }')
    echo "$rate" >> "$rates_file"
    echo "$nodes_reached" >> "$nodes_file"

    run_entry=$(printf '{"run":%d,"nodes_reached":%s,"elapsed_sec":%s,"nodes_per_sec":%s}' \
        "$i" "$nodes_reached" "$elapsed" "$rate")
    if [[ "$runs_json" == "[]" ]]; then
        runs_json="[$run_entry]"
    else
        runs_json="${runs_json%]},${run_entry}]"
    fi
    log "  nodes_reached=$nodes_reached elapsed=${elapsed}s nodes/s=$rate"
done

# --- Statistiques (médiane/min/max/écart-type relatif) ---------------------

# Lit des nombres (un par ligne) sur stdin, imprime : médiane min max ecart_type ecart_type_relatif_pct
compute_stats() {
    awk '
    { a[NR] = $1; sum += $1; n++ }
    END {
        if (n == 0) { print "0 0 0 0 0"; exit }
        for (i = 1; i <= n; i++)
            for (j = i + 1; j <= n; j++)
                if (a[j] < a[i]) { t = a[i]; a[i] = a[j]; a[j] = t }
        if (n % 2 == 1) med = a[(n + 1) / 2]
        else med = (a[n / 2] + a[n / 2 + 1]) / 2
        mean = sum / n
        sq = 0
        for (i = 1; i <= n; i++) sq += (a[i] - mean) ^ 2
        sd = (n > 1) ? sqrt(sq / (n - 1)) : 0
        rel = (mean != 0) ? (sd / mean * 100) : 0
        printf "%.6f %.6f %.6f %.6f %.6f\n", med, a[1], a[n], sd, rel
    }'
}

read -r rate_med rate_min rate_max rate_sd rate_relsd < <(compute_stats < "$rates_file")
read -r nodes_med nodes_min nodes_max nodes_sd nodes_relsd < <(compute_stats < "$nodes_file")

nodes_all_equal="true"
if [[ "$nodes_min" != "$nodes_max" ]]; then
    nodes_all_equal="false"
fi

# --- Rapport JSON ------------------------------------------------------------

git_commit=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)
git_branch=$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)
git_dirty="false"
[[ -n "$(git status --porcelain 2>/dev/null)" ]] && git_dirty="true"
host=$(hostname 2>/dev/null || echo unknown)
os=$(uname -s)
timestamp=$(date -u +%Y-%m-%dT%H:%M:%SZ)

report_file="$WORKDIR/report.json"
{
    printf '{\n'
    printf '  "timestamp": "%s",\n' "$timestamp"
    printf '  "host": "%s",\n' "$host"
    printf '  "os": "%s",\n' "$os"
    printf '  "git_commit": "%s",\n' "$git_commit"
    printf '  "git_branch": "%s",\n' "$git_branch"
    printf '  "git_dirty": %s,\n' "$git_dirty"
    printf '  "pieces_file": "%s",\n' "$PIECES"
    printf '  "nodes_target": %s,\n' "$NODES"
    printf '  "reps": %s,\n' "$REPS"
    printf '  "ncpu": %s,\n' "$NCPU"
    printf '  "load_avg_1min": %s,\n' "$LOAD1"
    printf '  "pin": "%s",\n' "$PIN_DESC"
    printf '  "runs": %s,\n' "$runs_json"
    printf '  "nodes_per_sec_median": %s,\n' "$rate_med"
    printf '  "nodes_per_sec_min": %s,\n' "$rate_min"
    printf '  "nodes_per_sec_max": %s,\n' "$rate_max"
    printf '  "nodes_per_sec_stddev_rel_pct": %s,\n' "$rate_relsd"
    printf '  "nodes_reached_median": %s,\n' "$nodes_med"
    printf '  "nodes_reached_min": %s,\n' "$nodes_min"
    printf '  "nodes_reached_max": %s,\n' "$nodes_max"
    printf '  "nodes_reached_all_equal": %s\n' "$nodes_all_equal"
    printf '}\n'
} > "$report_file"

cat "$report_file"
if [[ -n "$OUT" ]]; then
    cp "$report_file" "$OUT"
    log "Rapport écrit dans $OUT"
fi

# --- Résumé lisible + comparaison éventuelle --------------------------------

log ""
log "=== Résumé ==="
log "machine : $host ($os, $NCPU cœurs, load avg 1min=$LOAD1) — épinglage : $PIN_DESC"
log "git : $git_branch @ $git_commit$( [[ "$git_dirty" == "true" ]] && echo ' (modifications non commitées)' )"
log "cible : $NODES nœuds × $REPS répétitions (+ 1 chauffe non comptée), fichier : $PIECES"
log "nœuds atteints : médiane=$nodes_med min=$nodes_min max=$nodes_max (identiques : $nodes_all_equal)"
printf 'nœuds/s : médiane=%s min=%s max=%s écart-type relatif=%.2f%%\n' \
    "$rate_med" "$rate_min" "$rate_max" "$rate_relsd" >&2
if awk -v r="$rate_relsd" 'BEGIN { exit !(r > 5.0) }'; then
    log "ATTENTION : écart-type relatif > 5 % — mesure bruitée, les conclusions sont peu fiables"
    log "            (relancez sur une machine moins chargée, ou augmentez --reps/--nodes)"
fi

if [[ -n "$BASELINE" ]]; then
    json_field() {
        # Extraction volontairement simple (pas de dépendance à jq/python) :
        # le format ci-dessus est plat, chaque clé n'apparaît qu'une fois.
        grep -o "\"$2\"[[:space:]]*:[[:space:]]*[-0-9.eE]*" "$1" | head -1 | sed -E 's/.*:[[:space:]]*//'
    }
    base_rate_med=$(json_field "$BASELINE" nodes_per_sec_median)
    base_nodes_target=$(json_field "$BASELINE" nodes_target)
    base_commit=$(grep -o '"git_commit": *"[^"]*"' "$BASELINE" | head -1 | sed -E 's/.*: *"([^"]*)"/\1/')

    if [[ -z "$base_rate_med" ]]; then
        log ""
        log "baseline $BASELINE : impossible d'en extraire nodes_per_sec_median, comparaison ignorée"
    else
        delta=$(awk -v new="$rate_med" -v old="$base_rate_med" \
            'BEGIN { printf "%.2f", (old != 0) ? (new - old) / old * 100 : 0 }')
        log ""
        log "=== Comparaison à la baseline ($BASELINE, commit $base_commit) ==="
        log "nœuds/s médian : baseline=$base_rate_med  actuel=$rate_med  delta=${delta}%"
        if [[ "$base_nodes_target" != "$NODES" ]]; then
            log "note : la cible de nœuds diffère (baseline=$base_nodes_target, actuel=$NODES)" \
                "— le débit (nœuds/s) reste comparable, pas le nombre brut de nœuds atteints."
        fi
    fi
fi

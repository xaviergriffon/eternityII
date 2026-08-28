#!/usr/bin/env bash
#
# Banc de mesure du débit de la boucle de recherche (autosearch(), src/core/etii_search.c).
#
# Principe : au lieu de compter les nœuds explorés pendant une durée fixe (mesure
# bruitée par la charge de la machine), on mesure le temps mural nécessaire pour
# explorer un nombre de nœuds FIXE (ETII_BENCH_NODES, voir app_static_variables.h et
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
# Fonctions pures partagées avec tests/bench/test_bench_parse.sh
# (validation du temps écoulé — voir bench_parse_elapsed).
. "$SCRIPT_DIR/bench_lib.sh"
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

# Écrit dans $1 la sortie du run et dans $2 le temps réel (secondes).
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

# Nombre de tentatives avant d'abandonner un run dont `time` n'a pas produit un
# temps exploitable — voir bench_retry_valid_time (bench_lib.sh).
TIME_MAX_ATTEMPTS=3

# Doublure de run_once attendue par bench_retry_valid_time : exécute le run puis
# écrit le temps BRUT sur sa sortie standard. Les chemins passent par des
# variables plutôt que par des arguments, la fonction étant appelée par nom.
RUN_OUTLOG=""
RUN_TIMEFILE=""
run_once_emit_time() {
    local rc=0
    # `|| rc=$?` neutralise le `set -e` : un solveur qui échoue doit être
    # DIAGNOSTIQUÉ, pas confondu avec un temps illisible. `time` a de toute façon
    # écrit sa mesure, donc la validation passera et c'est le contrôle de
    # nodes_reached, juste après, qui arrêtera le banc avec le bon message.
    run_once "$RUN_OUTLOG" "$RUN_TIMEFILE" || rc=$?
    if [[ $rc -ne 0 ]]; then
        log "  le solveur s'est terminé en erreur (code $rc) — voir $RUN_OUTLOG"
    fi
    cat "$RUN_TIMEFILE"
}

# Exécute un run et écrit sur stdout son temps écoulé VALIDÉ (secondes),
# en rejouant le run si `time` a produit une valeur illisible.
# Renvoie 1 après TIME_MAX_ATTEMPTS tentatives : mieux vaut faire échouer le
# banc que publier un débit faux.
run_once_validated() {
    RUN_OUTLOG="$1"
    RUN_TIMEFILE="$2"
    bench_retry_valid_time "$TIME_MAX_ATTEMPTS" log "$3" run_once_emit_time
}

extract_field() {
    # $1 = fichier de log, $2 = nom du champ ("nodes_reached", "fc_attempts", ...)
    # dans la ligne "ETII_BENCH key=val key=val ..." (src/app/etii_client.c).
    grep -a -o "$2=[0-9]*" "$1" | tail -1 | cut -d= -f2
}

extract_nodes_reached() { extract_field "$1" nodes_reached; }

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
fc_rate_file="$WORKDIR/fc_rates"
max_result_file="$WORKDIR/max_results"
: > "$rates_file"
: > "$nodes_file"
: > "$fc_rate_file"
: > "$max_result_file"
runs_json="[]"
have_fc=0
have_maxresult=0

for ((i = 1; i <= REPS; i++)); do
    log "Run $i/$REPS..."
    outlog="$WORKDIR/run_$i.log"
    timefile="$WORKDIR/run_$i.time"
    if ! elapsed=$(run_once_validated "$outlog" "$timefile" "run $i"); then
        # On garde le répertoire de travail : le fichier de temps est la pièce
        # à conviction (le temps y est brut, avant toute normalisation).
        trap - EXIT
        exit 1
    fi

    nodes_reached=$(extract_nodes_reached "$outlog")
    if [[ -z "$nodes_reached" ]]; then
        log "run $i : cible de $NODES nœuds non atteinte — voir $outlog" \
            "(conservé pour diagnostic)"
        trap - EXIT
        exit 1
    fi

    rate=$(awk -v n="$nodes_reached" -v t="$elapsed" 'BEGIN { printf "%.6f", (t > 0) ? n / t : 0 }')
    echo "$rate" >> "$rates_file"
    echo "$nodes_reached" >> "$nodes_file"

    # Élagage INLINE par forward-check dans autosearch() (bt_forward_check,
    # src/core/etii_search.c) — absent des logs si le binaire est compilé avec
    # FORWARD_CHECK_K=0. Coût déjà inclus dans nodes/s ci-dessus ; ces champs
    # ne servent qu'à isoler le TAUX d'élagage (utile pour distinguer un gain de
    # débit dû à une boucle plus rapide d'un gain dû à un élagage différent).
    # Sans rapport avec le mode `pruner` séparé (réseau), non couvert par ce banc.
    fc_attempts=$(extract_field "$outlog" fc_attempts)
    fc_pruned=$(extract_field "$outlog" fc_pruned)
    fc_json=""
    if [[ -n "$fc_attempts" && -n "$fc_pruned" ]]; then
        have_fc=1
        fc_rate=$(awk -v a="$fc_attempts" -v p="$fc_pruned" 'BEGIN { printf "%.4f", (a > 0) ? p / a * 100 : 0 }')
        echo "$fc_rate" >> "$fc_rate_file"
        fc_json=$(printf ',"fc_attempts":%s,"fc_pruned":%s,"fc_prune_rate_pct":%s' \
            "$fc_attempts" "$fc_pruned" "$fc_rate")
    fi

    # max_result (profondeur maximale atteinte, etii_search.c) : nodes/s mesure
    # un DÉBIT de traitement, pas un progrès réel — un élagage qui coupe moins
    # ferait visiter plus de nœuds pour la même profondeur (arbre plus large),
    # auquel cas un débit plus élevé ne traduirait pas un vrai gain. À cible de
    # nœuds FIXE (comparaison avec --baseline ci-dessous), une profondeur
    # maximale comparable ou supérieure confirme que le débit gagné se traduit
    # en profondeur réelle. Absent des logs sur un binaire antérieur à cette
    # instrumentation : ne bloque jamais le banc, juste omis du rapport.
    max_result=$(extract_field "$outlog" max_result)
    max_result_json=""
    if [[ -n "$max_result" ]]; then
        have_maxresult=1
        echo "$max_result" >> "$max_result_file"
        max_result_json=$(printf ',"max_result":%s' "$max_result")
    fi

    run_entry=$(printf '{"run":%d,"nodes_reached":%s,"elapsed_sec":%s,"nodes_per_sec":%s%s%s}' \
        "$i" "$nodes_reached" "$elapsed" "$rate" "$fc_json" "$max_result_json")
    if [[ "$runs_json" == "[]" ]]; then
        runs_json="[$run_entry]"
    else
        runs_json="${runs_json%]},${run_entry}]"
    fi
    log "  nodes_reached=$nodes_reached elapsed=${elapsed}s nodes/s=$rate${fc_json:+ fc_prune_rate=${fc_rate}%}${max_result_json:+ max_result=${max_result}}"
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

fc_json_fields=""
fc_summary_line=""
if [[ $have_fc -eq 1 ]]; then
    read -r fc_rate_med fc_rate_min fc_rate_max _ _ < <(compute_stats < "$fc_rate_file")
    fc_json_fields=$(printf ',\n  "fc_prune_rate_pct_median": %s,\n  "fc_prune_rate_pct_min": %s,\n  "fc_prune_rate_pct_max": %s' \
        "$fc_rate_med" "$fc_rate_min" "$fc_rate_max")
    fc_summary_line="taux d'élagage forward-check : médiane=${fc_rate_med}% min=${fc_rate_min}% max=${fc_rate_max}%"
fi

max_result_json_fields=""
max_result_summary_line=""
if [[ $have_maxresult -eq 1 ]]; then
    read -r maxr_med maxr_min maxr_max _ _ < <(compute_stats < "$max_result_file")
    max_result_json_fields=$(printf ',\n  "max_result_median": %s,\n  "max_result_min": %s,\n  "max_result_max": %s' \
        "$maxr_med" "$maxr_min" "$maxr_max")
    max_result_summary_line="profondeur maximale atteinte (max_result) : médiane=$maxr_med min=$maxr_min max=$maxr_max"
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
    printf '  "nodes_reached_all_equal": %s%s%s\n' "$nodes_all_equal" "$fc_json_fields" "$max_result_json_fields"
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
if [[ -n "$fc_summary_line" ]]; then
    log "$fc_summary_line"
else
    log "élagage forward-check : absent des logs (binaire compilé avec FORWARD_CHECK_K=0 ?)"
fi
if [[ -n "$max_result_summary_line" ]]; then
    log "$max_result_summary_line"
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

        base_fc_rate=$(json_field "$BASELINE" fc_prune_rate_pct_median)
        if [[ -n "$base_fc_rate" && -n "$fc_summary_line" ]]; then
            log "taux d'élagage forward-check médian : baseline=${base_fc_rate}%  actuel=${fc_rate_med}%" \
                "— un écart signale un changement de comportement de l'élagage, pas seulement de débit."
        fi

        # max_result n'est comparable QUE si la cible de nœuds est identique : à
        # cible fixe, une profondeur atteinte comparable démontre qu'un débit
        # (nœuds/s) plus élevé se traduit en progrès réel et non en nœuds
        # « dilués » par un élagage plus faible qui explorerait un arbre plus
        # large pour la même profondeur. À cible différente la comparaison ne
        # dit rien (plus de nœuds -> plus de profondeur, indépendamment de
        # tout changement d'élagage) : on l'affiche à titre indicatif seulement.
        base_maxr=$(json_field "$BASELINE" max_result_median)
        if [[ -n "$base_maxr" && -n "$max_result_summary_line" ]]; then
            if [[ "$base_nodes_target" == "$NODES" ]]; then
                log "profondeur atteinte (max_result) à cible de nœuds IDENTIQUE : baseline=$base_maxr  actuel=$maxr_med" \
                    "— comparable/supérieure = le gain de débit est un vrai gain de progrès, pas un arbre plus large exploré plus vite."
            else
                log "profondeur atteinte (max_result) : baseline=$base_maxr (cible $base_nodes_target)  actuel=$maxr_med (cible $NODES)" \
                    "— cibles de nœuds différentes, comparaison non probante (indicatif uniquement)."
            fi
        fi
    fi
fi

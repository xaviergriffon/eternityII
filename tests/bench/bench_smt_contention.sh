#!/usr/bin/env bash
#
# Banc de contention SMT : mesure si caser DEUX processus de recherche
# mono-thread sur les deux threads matériels d'un MÊME cœur physique (SMT /
# Hyper-Threading) apporte un vrai débit agrégé, ou si la boucle chaude
# (autosearch(), forward-check sur map_bucket_packed — voir
# docs/autosearch_step.md) se contente de se disputer les ports d'exécution et
# les caches L1/L2 du cœur.
#
# C'est la mesure préalable à toute idée de « 2 threads par fork d'analyse » :
# passer de fork à thread ne change RIEN à ce résultat (la map est déjà
# partagée en COW entre forks, donc physiquement les mêmes lignes de cache
# qu'entre threads d'un même processus) — seul le PLACEMENT sur les cœurs
# matériels compte. Ce banc répond donc à la question indépendamment de toute
# implémentation.
#
# Principe : pour un groupe de N CPUs (typiquement 2, les deux threads
# matériels d'un même cœur physique), on compare :
#   - le débit CUMULÉ de N processus tournant SEULS l'un après l'autre,
#     chacun épinglé (taskset) sur un des CPUs du groupe (référence "aucune
#     contention") ;
#   - le débit CUMULÉ des mêmes N processus tournant EN MÊME TEMPS, chacun
#     épinglé sur son CPU du groupe (le scénario réel : N workers actifs en
#     permanence).
#
# Usage :
#   tests/bench/bench_smt_contention.sh --list-topology
#   tests/bench/bench_smt_contention.sh --cpus 0,1 [--cpus 0,2 ...]
#                                        [--nodes N] [--reps R] [--pieces f]
#                                        [--out rapport.json] [--force]
#
# Sans --cpus, le script tente de détecter automatiquement une paire de
# threads SMT frères (thread_siblings_list) pour le groupe "même cœur". Une
# machine sans SMT (Thread(s) per core: 1 — cas de la plupart des VMs cloud)
# n'a pas de paire "même cœur" : le script le signale et s'arrête — cette
# mesure-là doit tourner sur une machine SMT réelle.
#
set -euo pipefail
export LC_ALL=C

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
. "$SCRIPT_DIR/bench_lib.sh"
cd "$REPO_ROOT"

NODES=2000000
REPS=5
PIECES="data/pieces.csv"
OUT=""
FORCE=0
LOAD_THRESHOLD_PER_CPU="1.0"
declare -a CPU_GROUPS=()
LIST_TOPOLOGY=0

usage() {
    cat <<'EOF'
Usage: bench_smt_contention.sh [options]

Options:
  --cpus a,b[,c...]  Groupe de CPUs à tester ensemble (répétable). Un run
                      "solo" (référence) et un run "concurrent" sont faits
                      pour ce groupe.
  --nodes N          Nœuds cible par processus (défaut : 2000000 — plus petit
                      que bench_search.sh car ce banc lance plusieurs
                      processus par répétition)
  --reps R           Répétitions mesurées, hors chauffe (défaut : 5)
  --pieces FICHIER   Fichier de pièces (défaut : data/pieces.csv)
  --out FICHIER      Écrit le rapport JSON dans FICHIER
  --force            Ignore le refus sur machine chargée
  --list-topology    Affiche les cœurs physiques et leurs threads SMT, puis quitte
  -h, --help         Affiche cette aide
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --cpus) CPU_GROUPS+=("$2"); shift 2 ;;
        --nodes) NODES="$2"; shift 2 ;;
        --reps) REPS="$2"; shift 2 ;;
        --pieces) PIECES="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --force) FORCE=1; shift ;;
        --list-topology) LIST_TOPOLOGY=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "option inconnue : $1" >&2; usage >&2; exit 1 ;;
    esac
done

log() { echo "$@" >&2; }

if ! command -v taskset >/dev/null 2>&1; then
    log "taskset est requis par ce banc (épinglage CPU précis indispensable" \
        "à la mesure) et n'est pas disponible sur cette plateforme (macOS ?)." \
        "Ce banc doit tourner sous Linux."
    exit 1
fi

# --- Topologie : cœurs physiques -> threads matériels (SMT) ----------------
#
# thread_siblings_list liste, pour un CPU logique donné, tous les CPUs
# logiques qui partagent le même cœur physique (lui inclus). Sur une machine
# sans SMT, chaque liste ne contient qu'un seul CPU.
print_topology() {
    local d cpu list seen=","
    for d in /sys/devices/system/cpu/cpu[0-9]*; do
        cpu=$(basename "$d" | sed 's/cpu//')
        [[ -r "$d/topology/thread_siblings_list" ]] || continue
        list=$(cat "$d/topology/thread_siblings_list")
        [[ "$seen" == *",$list,"* ]] && continue
        seen="${seen}${list},"
        echo "cœur physique -> CPUs logiques : $list"
    done
}

if [[ $LIST_TOPOLOGY -eq 1 ]]; then
    print_topology
    exit 0
fi

# Renvoie sur stdout la première thread_siblings_list de taille > 1 (une
# vraie paire/groupe SMT), ou rien (et un code 1) si la machine n'en a pas.
detect_smt_group() {
    local d list
    for d in /sys/devices/system/cpu/cpu[0-9]*; do
        [[ -r "$d/topology/thread_siblings_list" ]] || continue
        list=$(cat "$d/topology/thread_siblings_list")
        if [[ "$list" == *,* || "$list" == *-* ]]; then
            # Le noyau formate tantôt "0,4" tantôt "0-1" : ce banc veut une
            # liste explicite séparée par des virgules dans les deux cas.
            if [[ "$list" == *-* && "$list" != *,* ]]; then
                local lo="${list%-*}" hi="${list#*-}"
                list=$(seq -s, "$lo" "$hi")
            fi
            echo "$list"
            return 0
        fi
    done
    return 1
}

if [[ ${#CPU_GROUPS[@]} -eq 0 ]]; then
    log "aucun --cpus fourni, détection automatique d'une paire SMT..."
    if smt_group=$(detect_smt_group); then
        log "groupe SMT détecté (même cœur physique) : $smt_group"
        CPU_GROUPS+=("$smt_group")
    else
        log "AUCUNE paire SMT détectée sur cette machine (Thread(s) per core: 1," \
            "cas courant des VMs cloud) — ce banc ne peut pas mesurer la contention" \
            "SMT ici. Relancez avec --cpus <a>,<b> où a,b sont deux threads" \
            "matériels du MÊME cœur physique (voir --list-topology sur la machine" \
            "cible), ou exécutez ce script sur une machine SMT réelle."
        exit 3
    fi
fi

# --- Détection machine : charge ---------------------------------------------

detect_ncpu() { command -v nproc >/dev/null 2>&1 && nproc || getconf _NPROCESSORS_ONLN; }
detect_load1() { awk '{print $1}' /proc/loadavg; }

NCPU=$(detect_ncpu)
LOAD1=$(detect_load1)
OVERLOADED=$(awk -v l="$LOAD1" -v n="$NCPU" -v t="$LOAD_THRESHOLD_PER_CPU" \
    'BEGIN { print (l > n * t) ? "1" : "0" }')
if [[ "$OVERLOADED" == "1" && $FORCE -ne 1 ]]; then
    log "Machine chargée : load average 1min=$LOAD1 pour $NCPU cœur(s)." \
        "Mesure refusée — relancez avec --force ou attendez que la charge redescende."
    exit 2
fi

# --- Build release ------------------------------------------------------------

log "Compilation release (make clean && make)..."
make clean >/dev/null 2>&1 || true
if ! make >/dev/null 2>&1; then
    log "échec de la compilation — voir 'make' pour le détail"
    exit 1
fi
[[ -x ./eternityII ]] || { log "binaire ./eternityII introuvable après compilation"; exit 1; }

WORKDIR=$(mktemp -d "${TMPDIR:-/tmp}/bench_smt.XXXXXX")
trap 'rm -rf "$WORKDIR"' EXIT

TIME_MAX_ATTEMPTS=3

# Médiane d'une colonne de nombres (un par ligne) lue depuis le fichier $1.
# Implémentation portable (pas d'extension gawk type asort) : mêmes bornes de
# fiabilité que compute_stats dans bench_search.sh.
median_of_file() {
    sort -n "$1" | awk '{a[NR]=$1; n=NR} END { print (n%2==1) ? a[(n+1)/2] : (a[n/2]+a[n/2+1])/2 }'
}

# Lance UN run pinné sur le CPU $1, journal de sortie $2. Écrit sur stdout le
# temps écoulé VALIDÉ (bench_parse_elapsed, bench_lib.sh), en rejouant le run
# jusqu'à TIME_MAX_ATTEMPTS fois si `time` produit une valeur illisible (bogue
# d'arrondi bash documenté dans bench_lib.sh). Renvoie 1 sans rien écrire si
# aucune tentative n'a produit de temps exploitable.
run_one_validated() {
    local cpu="$1" outlog="$2" attempt raw elapsed rc
    for ((attempt = 1; attempt <= TIME_MAX_ATTEMPTS; attempt++)); do
        rc=0
        TIMEFORMAT='%R'
        { time taskset -c "$cpu" env ETII_BENCH_NODES="$NODES" ./eternityII test "$PIECES" \
            < /dev/null > "$outlog" 2>&1 ; } 2> "$WORKDIR/rawtime" || rc=$?
        if [[ $rc -ne 0 ]]; then
            log "cpu $cpu : le solveur s'est terminé en erreur (code $rc) — voir $outlog"
            return 1
        fi
        raw=$(cat "$WORKDIR/rawtime")
        if elapsed=$(bench_parse_elapsed "$raw"); then
            printf '%s' "$elapsed"
            return 0
        fi
        log "cpu $cpu : temps écoulé illisible (« $raw »), tentative $attempt/$TIME_MAX_ATTEMPTS"
    done
    log "cpu $cpu : temps écoulé toujours illisible après $TIME_MAX_ATTEMPTS tentative(s)"
    return 1
}

# Lance les processus des CPUs de $1 (liste séparée par des virgules) EN MÊME
# TEMPS, chacun épinglé sur son CPU. Attend leur fin. Écrit le nombre total de
# nœuds explorés cumulés dans le fichier $2. Écrit sur stdout le temps mural
# global (du premier lancement à la fin du dernier à terminer — le facteur qui
# borne réellement le débit agrégé).
run_concurrent() {
    local cpulist="$1" total_nodes_file="$2"
    local -a cpus pids logs
    IFS=',' read -ra cpus <<< "$cpulist"
    pids=()
    logs=()
    local cpu i=0 start end
    start=$(date +%s.%N)
    for cpu in "${cpus[@]}"; do
        local outlog="$WORKDIR/conc_${cpulist//,/_}_${i}.log"
        logs+=("$outlog")
        ( taskset -c "$cpu" env ETII_BENCH_NODES="$NODES" ./eternityII test "$PIECES" \
            < /dev/null > "$outlog" 2>&1 ) &
        pids+=("$!")
        ((i++)) || true
    done
    local rc=0 pid
    for pid in "${pids[@]}"; do
        wait "$pid" || rc=1
    done
    end=$(date +%s.%N)
    if [[ $rc -ne 0 ]]; then
        log "un des processus concurrents a échoué — voir : ${logs[*]}"
        return 1
    fi
    local total=0 n outlog
    for outlog in "${logs[@]}"; do
        n=$(bench_extract_field "$(cat "$outlog")" nodes_reached)
        if [[ -z "$n" ]]; then
            log "cible de nœuds non atteinte dans $outlog"
            return 1
        fi
        total=$((total + n))
    done
    echo "$total" > "$total_nodes_file"
    awk -v s="$start" -v e="$end" 'BEGIN { printf "%.6f", e - s }'
}

# --- Boucle sur les groupes ---------------------------------------------------

report_entries="[]"

for cpulist in "${CPU_GROUPS[@]}"; do
    log ""
    log "=== Groupe CPUs : $cpulist ==="
    IFS=',' read -ra cpus <<< "$cpulist"

    # --- Référence "solo" : chaque CPU du groupe, seul, REPS fois -----------
    sum_solo=0
    n_cpus=${#cpus[@]}
    for cpu in "${cpus[@]}"; do
        log "-- solo cpu $cpu (chauffe)"
        run_one_validated "$cpu" "$WORKDIR/warm_$cpu.log" >/dev/null || {
            log "chauffe échouée pour cpu $cpu"; exit 1; }

        rates_file="$WORKDIR/solo_${cpu}_rates"
        : > "$rates_file"
        for ((r = 1; r <= REPS; r++)); do
            outlog="$WORKDIR/solo_${cpu}_$r.log"
            elapsed=$(run_one_validated "$cpu" "$outlog") || exit 1
            n=$(bench_extract_field "$(cat "$outlog")" nodes_reached)
            if [[ -z "$n" ]]; then
                log "solo cpu $cpu run $r : cible de nœuds non atteinte — voir $outlog"
                exit 1
            fi
            rate=$(awk -v n="$n" -v t="$elapsed" 'BEGIN { printf "%.6f", (t > 0) ? n / t : 0 }')
            echo "$rate" >> "$rates_file"
            log "   run $r/$REPS : nodes=$n elapsed=${elapsed}s nodes/s=$rate"
        done
        med=$(median_of_file "$rates_file")
        log "-- solo cpu $cpu : nodes/s médian = $med"
        sum_solo=$(awk -v s="$sum_solo" -v r="$med" 'BEGIN { printf "%.6f", s + r }')
    done

    # --- Concurrent : tous les CPUs du groupe en même temps, REPS fois ------
    log "-- concurrent (chauffe)"
    run_concurrent "$cpulist" "$WORKDIR/warm_conc_total" >/dev/null || {
        log "chauffe concurrente échouée pour le groupe $cpulist"; exit 1; }

    conc_rates_file="$WORKDIR/conc_${cpulist//,/_}_rates"
    : > "$conc_rates_file"
    for ((r = 1; r <= REPS; r++)); do
        total_file="$WORKDIR/conc_total_$r"
        elapsed=$(run_concurrent "$cpulist" "$total_file") || exit 1
        total_nodes=$(cat "$total_file")
        rate=$(awk -v n="$total_nodes" -v t="$elapsed" 'BEGIN { printf "%.6f", (t > 0) ? n / t : 0 }')
        echo "$rate" >> "$conc_rates_file"
        log "   run $r/$REPS : total_nodes=$total_nodes elapsed=${elapsed}s aggregate_nodes/s=$rate"
    done
    conc_med=$(median_of_file "$conc_rates_file")

    # solo_rate_avg : débit moyen d'UN SEUL worker sur UN des CPUs du groupe,
    # sans aucune contention (l'autre CPU du groupe est idle pendant cette
    # mesure) — c'est la référence "aujourd'hui : 1 fork/thread par cœur".
    solo_rate_avg=$(awk -v s="$sum_solo" -v n="$n_cpus" 'BEGIN { printf "%.6f", (n > 0) ? s / n : 0 }')

    # contention_efficiency : concurrent / solo_sum. 100 % = les N workers
    # tournant en même temps se partagent la machine SANS AUCUNE gêne mutuelle
    # (chacun garde son débit solo) — c'est le cas de deux cœurs PHYSIQUES
    # distincts, qui n'ont quasiment rien à se disputer. En dessous de 100 %,
    # les workers se gênent (ports d'exécution, L1/L2 partagés d'un même cœur
    # SMT) : c'est CE nombre qui répond à la question de contention.
    contention_efficiency_pct=$(awk -v c="$conc_med" -v s="$sum_solo" \
        'BEGIN { printf "%.2f", (s > 0) ? c / s * 100 : 0 }')

    # net_gain_vs_single_worker : concurrent / solo_rate_avg. LA métrique de
    # décision — « est-ce que caser N workers sur ce groupe de CPUs rapporte
    # plus que d'y laisser tourner UN SEUL worker aujourd'hui ? ». Sur deux
    # cœurs physiques distincts, la réponse attendue est ~N (aucune gêne, donc
    # tout le monde en profite). Sur deux threads SMT d'un même cœur physique,
    # une valeur nettement < N (ex. 1,1-1,4 pour N=2) signale que la majeure
    # partie de la capacité du second thread matériel est perdue en
    # contention — le nombre qui tranche la question initiale.
    net_gain_vs_single_worker=$(awk -v c="$conc_med" -v r="$solo_rate_avg" \
        'BEGIN { printf "%.4f", (r > 0) ? c / r : 0 }')

    log ""
    log "Groupe $cpulist ($n_cpus CPU) : solo/CPU (moyenne)=$solo_rate_avg nodes/s |" \
        "concurrent=$conc_med nodes/s | efficacité vs somme des solos=${contention_efficiency_pct}% |" \
        "gain net vs 1 seul worker sur ce groupe=${net_gain_vs_single_worker}x (idéal sans contention : ${n_cpus}x)"

    entry=$(printf '{"cpus":"%s","n_cpus":%s,"solo_nodes_per_sec_avg":%s,"concurrent_nodes_per_sec_median":%s,"contention_efficiency_pct":%s,"net_gain_vs_single_worker":%s}' \
        "$cpulist" "$n_cpus" "$solo_rate_avg" "$conc_med" "$contention_efficiency_pct" "$net_gain_vs_single_worker")
    if [[ "$report_entries" == "[]" ]]; then
        report_entries="[$entry]"
    else
        report_entries="${report_entries%]},${entry}]"
    fi
done

# --- Rapport JSON --------------------------------------------------------------

git_commit=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)
host=$(hostname 2>/dev/null || echo unknown)
timestamp=$(date -u +%Y-%m-%dT%H:%M:%SZ)

report_file="$WORKDIR/report.json"
{
    printf '{\n'
    printf '  "timestamp": "%s",\n' "$timestamp"
    printf '  "host": "%s",\n' "$host"
    printf '  "git_commit": "%s",\n' "$git_commit"
    printf '  "nodes_target_per_process": %s,\n' "$NODES"
    printf '  "reps": %s,\n' "$REPS"
    printf '  "groups": %s\n' "$report_entries"
    printf '}\n'
} > "$report_file"

cat "$report_file"
if [[ -n "$OUT" ]]; then
    cp "$report_file" "$OUT"
    log "Rapport écrit dans $OUT"
fi

log ""
log "Lecture : 'gain net vs 1 seul worker' proche de N (nb de CPUs du groupe) =" \
    "quasi aucune contention, ajouter le(s) worker(s) supplémentaire(s) est presque" \
    "tout bénéfice ; proche de 1 = le(s) thread(s) matériel(s) en plus n'apportent" \
    "quasiment rien (le cœur physique était déjà saturé par le premier worker) ;" \
    "entre les deux = gain partiel, à mettre en regard du coût (isolation crash" \
    "perdue, complexité). 'efficacité vs somme des solos' mesure la même chose en" \
    "pourcentage (100 % = aucune gêne mutuelle)."

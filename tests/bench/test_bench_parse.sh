#!/usr/bin/env bash
#
# Tests unitaires des fonctions pures de tests/bench/bench_lib.sh.
#
# Pendant shell des suites greatest : aucune compilation, aucun process lancé,
# aucun fichier écrit — on n'exerce que la logique de parsing/validation.
# Lancé par `make test` (cible test-bench).
#
set -uo pipefail
export LC_ALL=C

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$SCRIPT_DIR/bench_lib.sh"

pass=0
fail=0

# expect_valid <entrée> <sortie attendue> <libellé>
expect_valid() {
    local input="$1" want="$2" label="$3" got rc
    got=$(bench_parse_elapsed "$input")
    rc=$?
    if [[ $rc -eq 0 && "$got" == "$want" ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL %s : bench_parse_elapsed "%s" -> rc=%d "%s" (attendu rc=0 "%s")\n' \
            "$label" "$input" "$rc" "$got" "$want" >&2
    fi
}

# expect_invalid <entrée> <libellé>
expect_invalid() {
    local input="$1" label="$2" got rc
    got=$(bench_parse_elapsed "$input")
    rc=$?
    if [[ $rc -ne 0 && -z "$got" ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL %s : bench_parse_elapsed "%s" -> rc=%d "%s" (attendu un rejet)\n' \
            "$label" "$input" "$rc" "$got" >&2
    fi
}

# --- Sorties normales de `time` avec TIMEFORMAT='%R' ------------------------
expect_valid '2.964'    '2.964' 'décimal simple'
expect_valid '2.964
'                       '2.964' 'saut de ligne final (sortie réelle de time)'
expect_valid '  3.030  ' '3.030' 'espaces autour'
expect_valid '12'       '12'    'entier sans fraction'
expect_valid '123.456789' '123.456789' 'fraction longue'
expect_valid '0.001'    '0.001' 'plus petite valeur représentable à %R'

# --- La régression : bogue d'arrondi de bash --------------------------------
# `timeval_to_secs` arrondit les µs en ms sans retenue vers les secondes : à
# partir de 999500 µs la fraction vaut 1000, et mkfmt imprime (1000/100)+'0' =
# ':'. Un run de 2,9997 s ressort donc en « 2.:00 », qu'awk lit comme 2.0 —
# débit surestimé de 50 %, min/max et écart-type du rapport faussés, JSON
# invalide. Avant ce garde-fou, seule une valeur VIDE était rejetée.
expect_invalid '2.:00'  'bogue bash %R : fraction arrondie à 1000 (2.9997 s)'
expect_invalid '0.:00'  'idem sous la seconde'
expect_invalid '59.:00' 'idem à deux chiffres de secondes'

# --- Autres sorties qui ne doivent jamais passer pour un temps --------------
expect_invalid ''       'fichier de temps vide'
expect_invalid '   '    'blancs uniquement'
expect_invalid '0'      'zéro : pas un temps de run mesurable'
expect_invalid '0.000'  'zéro fractionnaire (division impossible)'
expect_invalid '-1.5'   'valeur négative'
expect_invalid '2,964'  'virgule décimale (locale non forcée)'
expect_invalid '2.964s' 'unité collée'
expect_invalid '2.9.4'  'deux séparateurs'
expect_invalid '2.'     'point sans fraction'
expect_invalid '.964'   'fraction sans partie entière'
expect_invalid '1e3'    'notation exponentielle'
expect_invalid 'real0m2.964suser0m2.900s' 'TIMEFORMAT par défaut (non appliqué)'
expect_invalid '0:02.96elapsed' 'sortie de /usr/bin/time GNU'
expect_invalid '2.964 3.030' 'deux valeurs concaténées'

# --- Rejeu du run sur temps illisible (bench_retry_valid_time) --------------
#
# Doublures : `fake_log` collecte les messages, `fake_run` débite les valeurs de
# FAKE_TIMES l'une après l'autre (une par « run »), ce qui permet d'exercer la
# boucle sans compiler ni lancer le solveur.
# bench_retry_valid_time appelle fn_run dans un sous-shell (substitution de
# commande) : les compteurs passent par des fichiers, sinon les incréments sont
# perdus au retour.
TRACE_DIR=$(mktemp -d "${TMPDIR:-/tmp}/bench_parse.XXXXXX")
trap 'rm -rf "$TRACE_DIR"' EXIT
FAKE_TIMES=()

fake_run() {
    local n
    n=$(wc -l < "$TRACE_DIR/runs")
    printf '%s' "${FAKE_TIMES[$n]}"
    echo x >> "$TRACE_DIR/runs"
}
fake_log() { echo x >> "$TRACE_DIR/logs"; }
count() { wc -l < "$TRACE_DIR/$1" | tr -d ' '; }
reset_trace() { : > "$TRACE_DIR/runs"; : > "$TRACE_DIR/logs"; }

# retry_case <libellé> <rc attendu> <sortie attendue> <runs attendus> <temps...>
retry_case() {
    local label="$1" want_rc="$2" want_out="$3" want_runs="$4"
    shift 4
    FAKE_TIMES=("$@")
    reset_trace
    local got rc runs
    got=$(bench_retry_valid_time 3 fake_log "$label" fake_run)
    rc=$?
    runs=$(count runs)
    if [[ $rc -eq $want_rc && "$got" == "$want_out" && $runs -eq $want_runs ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL %s : rc=%d "%s" runs=%s (attendu rc=%d "%s" runs=%d)\n' \
            "$label" "$rc" "$got" "$runs" "$want_rc" "$want_out" "$want_runs" >&2
    fi
}

retry_case 'run valide du premier coup : aucun rejeu' \
    0 '2.964' 1 '2.964' '3.000' '3.000'
retry_case 'temps illisible puis valide : un seul rejeu, valeur du 2e run' \
    0 '3.001' 2 '2.:00' '3.001' '3.000'
retry_case 'deux temps illisibles puis valide' \
    0 '3.002' 3 '2.:00' '0.:00' '3.002'
retry_case 'trois échecs : abandon, rien sur stdout' \
    1 '' 3 '2.:00' '0.:00' '1.:00'
retry_case 'fichier de temps vide : rejoué comme le reste' \
    0 '2.5' 2 '' '2.5' '3.000'

# Un rejeu doit être signalé : sinon l'utilisateur croit à 5 runs mesurés alors
# qu'un 6e a eu lieu, et l'incident disparaît du compte rendu.
FAKE_TIMES=('2.:00' '3.001')
reset_trace
bench_retry_valid_time 3 fake_log 'run 1' fake_run > /dev/null
if [[ "$(count logs)" -eq 1 ]]; then
    pass=$((pass + 1))
else
    fail=$((fail + 1))
    printf 'FAIL rejeu journalisé : %s message(s) (attendu 1)\n' "$(count logs)" >&2
fi

# --- Bilan -------------------------------------------------------------------
printf '\n* bench_lib.sh : %d assertions passées, %d échouées\n' "$pass" "$fail"
[[ $fail -eq 0 ]]

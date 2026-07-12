#!/usr/bin/env bash
#
# Test d'intégration client/serveur sur le puzzle 16 pièces (4×4).
#
# Scénario : on lance un serveur puis un client (binaire compilé avec
# ETERN_PARTS=16). Le client résout le 4×4 en quelques instants, signale la
# solution au serveur (INST_SOLUTION) ; le serveur l'affiche, sauvegarde son
# stock et s'arrête. Le test vérifie que LES DEUX côtés ont bien le résultat.
#
# Le binaire 16 pièces et le CSV sont passés en variables d'environnement (ou
# détectés par défaut). Tout tourne dans un répertoire temporaire isolé : les
# fichiers produits (solution_*, *.back, events.log) n'atterrissent jamais dans
# le dépôt et sont supprimés à la fin.
#
# Sortie : 0 si succès, non nul (avec dump des logs) sinon. Un timeout borné
# garantit que le test ne reste jamais bloqué.
#
# Usage :
#   tests/integration/run_solution_16.sh
#   BIN=./eternityII16 DATA=data/pieces16.csv TIMEOUT=60 tests/integration/run_solution_16.sh

set -u

# --- Résolution des chemins (AVANT tout cd : on les rend absolus) -----------
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${BIN:-$REPO_ROOT/eternityII16}"
DATA="${DATA:-$REPO_ROOT/data/pieces16.csv}"
TIMEOUT="${TIMEOUT:-60}"   # secondes max avant de déclarer l'échec

case "$BIN"  in /*) ;; *) BIN="$REPO_ROOT/$BIN";; esac
case "$DATA" in /*) ;; *) DATA="$REPO_ROOT/$DATA";; esac

fail() { echo "ÉCHEC: $*" >&2; }

if [ ! -x "$BIN" ]; then
    fail "binaire 16 pièces introuvable ou non exécutable: $BIN"
    echo "  (compile-le d'abord, ex.: make CPPFLAGS=\"-DETERN_PARTS=16\" EXECUTABLE=eternityII16)" >&2
    exit 2
fi
if [ ! -f "$DATA" ]; then
    fail "fichier de pièces introuvable: $DATA"
    exit 2
fi

# --- Répertoire de travail isolé --------------------------------------------
WORK="$(mktemp -d 2>/dev/null || mktemp -d -t etii16)"
SRV_PID=""
CLI_PID=""

cleanup() {
    [ -n "$CLI_PID" ] && kill "$CLI_PID" 2>/dev/null
    [ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null
    # Filets de sécurité : enfants forkés du client encore vivants.
    pkill -P "${CLI_PID:-0}" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

cd "$WORK" || { fail "cd $WORK"; exit 2; }

echo "== Test intégration 16 pièces =="
echo "  binaire : $BIN"
echo "  données : $DATA"
echo "  travail : $WORK  (timeout ${TIMEOUT}s)"

# --- Lancement serveur puis client ------------------------------------------
# --stop-on-solution : le serveur s'arrête à la 1re solution (et sauvegarde son
# stock), ce qui donne au test un point de terminaison déterministe. Sans ce
# drapeau, le défaut est de continuer à tourner — voir AGENTS.md.
#
# 2 threads (et non 1) : le processus PARENT du client ouvre, EN PLUS de la
# connexion de travail de son unique fork de recherche, sa propre connexion de
# canal de contrôle (INST_CONTROL_HELLO, v9) — cf. AGENTS.md, section canal de
# contrôle. Cette session de contrôle occupe elle aussi un slot du pool
# NB_THREADS du serveur (même pool que les connexions de travail, pas de pool
# dédié) et le garde tant que le client tourne. Avec 1 seul thread serveur, la
# session de contrôle gagnait systématiquement la course de connexion et
# affamait pour toujours le fork de recherche ("all threads busy" en boucle) —
# le serveur ne recevait jamais la solution et le test expirait au timeout.
"$BIN" tcpserver 2 "$DATA" --stop-on-solution </dev/null >server.log 2>&1 &
SRV_PID=$!
sleep 1   # laisse le serveur écouter (le client a de toute façon un back-off)

"$BIN" tcpclient 127.0.0.1 1 1000 "$DATA" --stop-on-solution </dev/null >client.log 2>&1 &
CLI_PID=$!

# --- Attente bornée de l'arrêt du serveur (il s'arrête sur solution) --------
server_exited=0
waited=0
# On scrute 5 fois par seconde → TIMEOUT*5 itérations.
ticks=$(( TIMEOUT * 5 ))
i=0
while [ "$i" -lt "$ticks" ]; do
    if ! kill -0 "$SRV_PID" 2>/dev/null; then
        server_exited=1
        break
    fi
    sleep 0.2
    i=$(( i + 1 ))
done

if [ "$server_exited" -ne 1 ]; then
    fail "le serveur ne s'est pas arrêté dans le délai imparti (${TIMEOUT}s)"
    echo "----- server.log -----"; cat server.log 2>/dev/null
    echo "----- client.log -----"; cat client.log 2>/dev/null
    exit 1
fi

wait "$SRV_PID" 2>/dev/null
srv_rc=$?

# --- Vérifications ----------------------------------------------------------
rc=0
check() { # libellé ; condition déjà évaluée via "$?"
    if [ "$1" -eq 0 ]; then echo "  OK   $2"; else echo "  FAIL $2"; rc=1; fi
}

[ "$srv_rc" -eq 0 ]; check $? "le serveur s'est arrêté proprement (code $srv_rc)"

grep -qi "SOLUTION reçue" server.log; check $? "le serveur a reçu la solution (server.log)"
grep -qi "SOLUTION FOUND" client.log; check $? "le client a trouvé la solution (client.log)"

ls solution_server_* >/dev/null 2>&1; check $? "fichier solution côté serveur présent (solution_server_*)"
ls solution_[0-9]*   >/dev/null 2>&1; check $? "fichier solution côté client présent (solution_*)"

[ -f eternityII.back ]; check $? "stock serveur sauvegardé (eternityII.back)"
[ -f eternityII-in_analyse.back ]; check $? "files 'en analyse' sauvegardées (eternityII-in_analyse.back)"

if [ "$rc" -ne 0 ]; then
    echo "----- server.log -----"; cat server.log 2>/dev/null
    echo "----- client.log -----"; cat client.log 2>/dev/null
    fail "une ou plusieurs vérifications ont échoué"
    exit 1
fi

echo "== SUCCÈS : client et serveur ont tous deux le résultat, serveur arrêté avec backup =="
exit 0

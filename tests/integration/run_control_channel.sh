#!/usr/bin/env bash
#
# Test d'intégration du canal de contrôle (v9, INST_CONTROL_HELLO) sur le
# puzzle 16 pièces (4×4).
#
# Scénario : on lance un serveur puis un client SANS --stop-on-solution (les
# deux processus restent vivants indéfiniment, qu'une solution soit trouvée ou
# non — cf. docs/utilisation.md, option --stop-on-solution) pour avoir tout le
# temps voulu de piloter le canal de contrôle. Le processus PARENT du client
# ouvre, en plus de la connexion de travail de son fork de recherche, sa propre
# connexion de canal de contrôle ; le serveur pilote ce canal via sa commande
# console `clientsStats` et via `pause`/`resume` (qui diffusent désormais
# systématiquement `CTRL_COMMAND` aux sessions actives, en plus de leur effet
# local — l'ancien `clientsPause`/`clientsResume` a été fusionné dedans, cf.
# docs/echanges_client_serveur.md, section canal de contrôle), et on
# vérifie le round-trip complet à travers les logs des DEUX côtés :
#
#   console serveur -> CTRL_COMMAND/CTRL_GET_STATS -> canal de contrôle client
#   -> do_command_line (côté client) -> CTRL_RESULT/CTRL_STATS -> console serveur
#
# Le serveur est démarré avec 2 threads (et non 1) : le fork de recherche ET
# la session de contrôle du parent client occupent chacun un slot du même pool
# NB_THREADS (pas de pool dédié, cf. control_registry.h) — voir la note dans
# run_solution_16.sh pour l'origine de cette exigence.
#
# Les deux processus sont pilotés depuis leur console (stdin) via des named
# pipes (FIFO), sur le modèle classique "writer FD ouvert après le lancement du
# reader en arrière-plan" pour éviter le rendez-vous bloquant à l'ouverture.
# Fin déterministe : la commande console `exit` (et non un `kill`, gardé en
# filet de sécurité par le trap) sur chacun des deux process.
#
# Sortie : 0 si succès, non nul (avec dump des logs) sinon. Timeout borné.
#
# Usage :
#   tests/integration/run_control_channel.sh
#   BIN=./eternityII16 DATA=data/pieces16.csv TIMEOUT=60 tests/integration/run_control_channel.sh

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
WORK="$(mktemp -d 2>/dev/null || mktemp -d -t etii16ctrl)"
SRV_PID=""
CLI_PID=""

cleanup() {
    # Filet de sécurité : si les commandes "exit" console n'ont pas suffi
    # (échec avant d'y arriver), on tue tout de force.
    exec 3>&- 2>/dev/null
    exec 4>&- 2>/dev/null
    [ -n "$CLI_PID" ] && kill "$CLI_PID" 2>/dev/null
    [ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null
    pkill -P "${CLI_PID:-0}" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

cd "$WORK" || { fail "cd $WORK"; exit 2; }

echo "== Test intégration canal de contrôle =="
echo "  binaire : $BIN"
echo "  données : $DATA"
echo "  travail : $WORK  (timeout ${TIMEOUT}s)"

# --- Attente bornée d'un motif dans un fichier journal ----------------------
# wait_for_log <fichier> <motif grep -E> <timeout_secondes>
# Renvoie 0 dès que le motif apparaît, 1 si le délai est dépassé.
wait_for_log() {
    local file="$1" pattern="$2" tmax="$3"
    local ticks=$(( tmax * 5 )) i=0
    while [ "$i" -lt "$ticks" ]; do
        if [ -f "$file" ] && grep -Eq "$pattern" "$file" 2>/dev/null; then
            return 0
        fi
        sleep 0.2
        i=$(( i + 1 ))
    done
    return 1
}

# --- Idem, en RÉÉMETTANT la commande console tant qu'on attend --------------
# wait_for_log_repeating <fichier> <motif> <timeout_s> <commande console>
#
# Les commandes de la console SERVEUR qui pilotent un client (`clientsStats`,
# `pause`, `resume`) sont ASYNCHRONES : elles déposent une demande dans la file
# de la session de contrôle (`control_registry_post_command`) et rendent la main
# aussitôt. Le thread de session ne la relève qu'à son prochain passage par
# `control_registry_wait_command` ; s'il est à cet instant au milieu d'un
# aller-retour CTRL_PING, dont la réception est bornée par le `SO_RCVTIMEO` du
# socket (`tcp_timeout`, 10 s par défaut), le relevé attend d'autant.
#
# En local ce délai est indiscernable (mesuré : < 1 s sur 12 exécutions), mais
# sur un runner GitHub à 2 vCPU il a été observé à 9 s dans un cas et au-delà
# des 60 s du timeout dans un autre — sur le MÊME commit, l'exécution
# déclenchée par `pull_request` passant et celle déclenchée par `push`
# échouant. Attendre un unique relevé rend donc ce test tributaire de l'instant
# où la commande tombe dans le cycle de la session.
#
# Réémettre la commande toutes les 2 s supprime cette dépendance — c'est ce que
# ferait un opérateur devant une console qui ne répond pas — sans rien changer
# au comportement mesuré : les trois commandes concernées sont idempotentes
# (`pause` sur un client déjà en pause, `resume` sur un client déjà actif et
# `clientsStats` sont sans effet supplémentaire), et le motif attendu reste
# rigoureusement le même.
wait_for_log_repeating() {
    local file="$1" pattern="$2" tmax="$3" cmd="$4"
    local ticks=$(( tmax * 5 )) i=0
    while [ "$i" -lt "$ticks" ]; do
        if [ -f "$file" ] && grep -Eq "$pattern" "$file" 2>/dev/null; then
            return 0
        fi
        # Réémission toutes les 10 ticks (2 s), jamais au premier tour : le
        # cas normal est que la commande déjà envoyée suffise.
        if [ "$i" -gt 0 ] && [ $(( i % 10 )) -eq 0 ]; then
            echo "$cmd" >&3
        fi
        sleep 0.2
        i=$(( i + 1 ))
    done
    return 1
}

# --- Lancement serveur puis client, pilotés par FIFO ------------------------
# 2 threads serveur : 1 pour le fork de recherche du client, 1 pour la session
# de contrôle de son parent (même pool NB_THREADS, cf. control_registry.h).
mkfifo srv_in cli_in

"$BIN" server 2 "$DATA" <srv_in >server.log 2>&1 &
SRV_PID=$!
# Ouvre le bout écriture APRÈS avoir lancé le lecteur en tâche de fond : sinon
# l'ouverture bloquante d'une FIFO en lecture (côté serveur) et en écriture
# (ici) attendraient chacune l'autre avant que le serveur soit même démarré.
exec 3>srv_in
sleep 1   # laisse le serveur écouter (le client a de toute façon un back-off)

# PAS de --stop-on-solution : les deux processus restent vivants tout le temps
# nécessaire au pilotage du canal de contrôle, indépendamment de la vitesse de
# résolution du 4×4 (quasi instantanée).
"$BIN" client 127.0.0.1 1 1000 "$DATA" <cli_in >client.log 2>&1 &
CLI_PID=$!
exec 4>cli_in

rc=0
check() { # code de sortie déjà évalué via "$?" ; $1 = code, $2 = libellé
    if [ "$1" -eq 0 ]; then echo "  OK   $2"; else echo "  FAIL $2"; rc=1; fi
}

# --- 1. La session de contrôle du parent client doit s'enregistrer ----------
wait_for_log server.log "session de contrôle enregistrée" "$TIMEOUT"
check $? "session de contrôle enregistrée côté serveur"

# --- 2. clientsStats : round-trip CTRL_GET_STATS ----------------------------
echo "clientsStats" >&3
wait_for_log_repeating server.log "stats client :" "$TIMEOUT" "clientsStats"
check $? "clientsStats : statistiques agrégées reçues (server.log)"

# --- 3. pause (console serveur) : round-trip CTRL_COMMAND "pause" -----------
echo "pause" >&3
wait_for_log_repeating server.log 'commande distante "pause" exécutée \(code retour 0\)' "$TIMEOUT" "pause"
check $? "pause : commande diffusée et acquittée avec succès (server.log)"
wait_for_log client.log "pause administrative demandée" "$TIMEOUT"
check $? "pause : pause administrative appliquée côté client (client.log)"

# --- 4. resume (console serveur) : round-trip CTRL_COMMAND "resume" ---------
echo "resume" >&3
wait_for_log_repeating server.log 'commande distante "resume" exécutée \(code retour 0\)' "$TIMEOUT" "resume"
check $? "resume : commande diffusée et acquittée avec succès (server.log)"
wait_for_log client.log "pause administrative levée" "$TIMEOUT"
check $? "resume : pause administrative levée côté client (client.log)"

# --- 5. Arrêt déterministe des deux processus via leur propre console -------
echo "exit" >&4
echo "exit" >&3
exec 3>&-
exec 4>&-

wait_srv=0
ticks=$(( TIMEOUT * 5 )); i=0
while [ "$i" -lt "$ticks" ]; do
    kill -0 "$SRV_PID" 2>/dev/null || { wait_srv=1; break; }
    sleep 0.2
    i=$(( i + 1 ))
done
check $(( 1 - wait_srv )) "le serveur s'est arrêté après la commande console \"exit\""

wait_cli=0
ticks=$(( TIMEOUT * 5 )); i=0
while [ "$i" -lt "$ticks" ]; do
    kill -0 "$CLI_PID" 2>/dev/null || { wait_cli=1; break; }
    sleep 0.2
    i=$(( i + 1 ))
done
check $(( 1 - wait_cli )) "le client s'est arrêté après la commande console \"exit\""

if [ "$rc" -ne 0 ]; then
    echo "----- server.log -----"; cat server.log 2>/dev/null
    echo "----- client.log -----"; cat client.log 2>/dev/null
    fail "une ou plusieurs vérifications ont échoué"
    exit 1
fi

echo "== SUCCÈS : canal de contrôle vérifié bout-en-bout (stats/pause/resume, arrêt propre) =="
exit 0

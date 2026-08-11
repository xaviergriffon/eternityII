#!/usr/bin/env bash
#
# Test d'intégration du cycle de vie dynamique des fils de recherche d'un
# client sur le puzzle 16 pièces (4×4).
#
# Scénario : le client démarre SANS fils (--config-file), lit un fichier de
# configuration présent au boot -> auto-démarrage ~5 s plus tard (ORCH_COUNTDOWN,
# cf. AGENTS.md/fork_orchestrator.h). Le serveur pilote ensuite ce client
# EXCLUSIVEMENT à distance, via sa propre console et `clientsCommand` (canal de
# contrôle v9/CTRL_COMMAND) -- jamais en tapant directement dans la console du
# client, pour prouver que "start"/"stopForks"/"configApply"/"config" sont bien
# devenues des commandes pilotables à distance (control_command_allowed) :
#
#   1. auto-démarrage : le serveur voit une session de contrôle enregistrée
#      avec forks=1 sans qu'aucune commande n'ait été envoyée.
#   2. `clientsCommand stopForks` : les fils s'arrêtent, le parent client reste
#      vivant (canal de contrôle, console toujours actifs).
#   3. `clientsCommand start` : re-fork à distance, nouvelle session forks=1.
#   4. `clientsCommand config nb_forks 2` puis `clientsCommand configApply` :
#      redémarrage à chaud (NEEDS_RESTART), le serveur voit une nouvelle
#      session de contrôle enregistrée avec forks=2.
#
# Le serveur est démarré avec 2 threads : le fork de recherche ET la session de
# contrôle du parent client occupent chacun un slot du même pool NB_THREADS
# (cf. la note dans run_solution_16.sh).
#
# Fin déterministe : commande console `exit` sur les deux process (filet de
# sécurité `kill` conservé par le trap).
#
# Sortie : 0 si succès, non nul (avec dump des logs) sinon. Timeout borné.
#
# Usage :
#   tests/integration/run_client_lifecycle.sh
#   BIN=./eternityII16 DATA=data/pieces16.csv TIMEOUT=60 tests/integration/run_client_lifecycle.sh

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
WORK="$(mktemp -d 2>/dev/null || mktemp -d -t etii16lifecycle)"
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

echo "== Test intégration cycle de vie dynamique des fils =="
echo "  binaire : $BIN"
echo "  données : $DATA"
echo "  travail : $WORK  (timeout ${TIMEOUT}s)"

# --- Attente bornée d'un motif dans un fichier journal ----------------------
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

# --- Attente qu'EXACTEMENT une session de contrôle soit active côté serveur -
# Après une (re)connexion (ex. suite à "start"), l'ancienne session (fermée
# côté client) reste brièvement enregistrée côté serveur jusqu'à ce que sa
# lecture socket détecte la déconnexion -- un `clientsCommand` envoyé pendant
# cette fenêtre de recouvrement est diffusé aux DEUX sessions, dont l'ancienne
# (morte) échoue son round-trip CTRL_RESULT et ne journalise donc jamais
# l'acquittement attendu (race trouvée en testant ce script manuellement).
# On force le serveur à publier un instantané frais (`clients`) et on attend
# qu'il ne rapporte plus qu'UNE session avant d'envoyer la commande suivante.
wait_for_single_control_session() {
    local tmax="$1"
    local ticks=$(( tmax * 5 )) i=0
    while [ "$i" -lt "$ticks" ]; do
        echo "clients" >&3
        sleep 0.2
        if grep -q "clients : 1 session(s) de contrôle active(s)" server.log 2>/dev/null; then
            return 0
        fi
        i=$(( i + 1 ))
    done
    return 1
}

# --- Fichier de configuration client : présent AU BOOT pour déclencher le
#     décompte d'auto-démarrage (ORCH_COUNTDOWN) sans aucune commande console.
#     nb_forks=1 : un seul fork, suffisant pour occuper le 2e thread du
#     serveur (le 1er est pris par la session de contrôle du parent).
cat > eternityii-client.conf <<EOF
nb_forks    = 1
server_host = 127.0.0.1
parts_file  = $DATA
EOF

# --- Lancement serveur puis client, pilotés par FIFO ------------------------
# 4 threads serveur (et non 2, cf. run_control_channel.sh) : ce scénario
# enchaîne PLUSIEURS cycles stopForks/start/configApply, chacun fermant puis
# rouvrant à la fois la connexion de travail du fork ET la session de
# contrôle du parent -- avec seulement 2 threads, une ancienne session pas
# encore détectée déconnectée par le serveur (fenêtre asynchrone, cf.
# wait_for_single_control_session ci-dessus) peut se retrouver à retenir un
# thread pile au moment où la NOUVELLE connexion de travail ou de contrôle
# tente de se connecter -- "request unfulfilled: all threads busy" côté
# serveur, qui fait échouer le round-trip CTRL_COMMAND et déclenche une
# reconnexion en cascade côté client (race trouvée en testant ce script
# manuellement). 4 threads laissent la marge nécessaire à ces recouvrements
# transitoires (même pool NB_THREADS que les connexions de travail, cf.
# control_registry.h).
mkfifo srv_in cli_in

"$BIN" server 4 "$DATA" <srv_in >server.log 2>&1 &
SRV_PID=$!
exec 3>srv_in
sleep 1   # laisse le serveur écouter (le client a de toute façon un back-off)

# Sans argument positionnel : server_host/nb_forks/parts_file viennent
# ENTIÈREMENT du fichier de configuration ci-dessus (cf. client_config.c,
# priorité CLI > fichier > défauts -- argc trop court ici pour que la CLI
# fournisse quoi que ce soit). PAS de --stop-on-solution : le parent doit
# rester vivant indéfiniment pour être piloté à distance.
"$BIN" client <cli_in >client.log 2>&1 &
CLI_PID=$!
exec 4>cli_in

rc=0
check() {
    if [ "$1" -eq 0 ]; then echo "  OK   $2"; else echo "  FAIL $2"; rc=1; fi
}

# --- 1. Auto-démarrage : décompte de 5s, aucune commande envoyée -----------
wait_for_log client.log "auto-démarrage dans" "$TIMEOUT"
check $? "décompte d'auto-démarrage visible dans le journal client (client.log)"

wait_for_log server.log "session de contrôle enregistrée.*forks=1" "$TIMEOUT"
check $? "auto-démarrage : session de contrôle enregistrée côté serveur avec forks=1 (server.log)"

# --- 2. clientsCommand stopForks : arrêt à distance, parent client vivant --
echo "clientsCommand stopForks" >&3
wait_for_log server.log 'commande distante "stopForks" exécutée \(code retour 0\)' "$TIMEOUT"
check $? "clientsCommand stopForks : acquittée côté serveur (server.log)"
wait_for_log client.log "orchestrateur : fils arrêtés" "$TIMEOUT"
check $? "stopForks : séquence d'arrêt terminée côté client, parent vivant (client.log)"

# Le parent doit être TOUJOURS vivant (c'est tout l'objet de la fonctionnalité).
kill -0 "$CLI_PID" 2>/dev/null
check $? "le process parent client est resté vivant après stopForks"

# --- 3. clientsCommand start : re-fork à distance ---------------------------
echo "clientsCommand start" >&3
wait_for_log server.log 'commande distante "start" exécutée \(code retour 0\)' "$TIMEOUT"
check $? "clientsCommand start : acquittée côté serveur (server.log)"
# Preuve fiable du re-fork : la reconnexion du canal de contrôle avec forks=1
# (control_channel_request_reconnect, appelée par orchestrator_spawn_forks sur
# succès) déclenche une NOUVELLE ligne "session de contrôle enregistrée" côté
# serveur -- déjà vue une fois à l'étape 1, donc on exige au moins 2 occurrences.
ticks=$(( TIMEOUT * 5 )); i=0; seen2=1
while [ "$i" -lt "$ticks" ]; do
    n=$(grep -Ec "session de contrôle enregistrée.*forks=1" server.log 2>/dev/null || echo 0)
    if [ "${n:-0}" -ge 2 ]; then seen2=0; break; fi
    sleep 0.2
    i=$(( i + 1 ))
done
check $seen2 "clientsCommand start : re-fork effectué, nouvelle session forks=1 (server.log)"

# Laisse le serveur détecter la déconnexion de l'ANCIENNE session (fermée
# côté client au moment de la reconnexion) avant d'envoyer la commande
# suivante : sans cette attente, un `clientsCommand` diffusé pendant la courte
# fenêtre où les DEUX sessions sont encore enregistrées échoue son
# round-trip CTRL_RESULT contre la session morte (cf. la doc de
# wait_for_single_control_session ci-dessus).
wait_for_single_control_session "$TIMEOUT"
check $? "une seule session de contrôle active après le re-fork (server.log)"

# --- 4. clientsCommand config nb_forks 2 + configApply : redémarrage à chaud
echo "clientsCommand config nb_forks 2" >&3
wait_for_log server.log 'commande distante "config nb_forks 2" exécutée \(code retour 0\)' "$TIMEOUT"
check $? "clientsCommand config nb_forks 2 : acquittée côté serveur (server.log)"

echo "clientsCommand configApply" >&3
wait_for_log server.log 'commande distante "configApply" exécutée \(code retour 0\)' "$TIMEOUT"
check $? "clientsCommand configApply : acquittée côté serveur (server.log)"

wait_for_log client.log "orchestrateur : nb_forks 1 -> 2 — reconstruction des tableaux de fils" "$TIMEOUT"
check $? "configApply : redémarrage à chaud effectué côté client, nb_forks 1 -> 2 (client.log)"

wait_for_log server.log "session de contrôle enregistrée.*forks=2" "$TIMEOUT"
check $? "configApply : nouvelle session de contrôle enregistrée avec forks=2 (server.log)"

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

echo "== SUCCÈS : cycle de vie dynamique des fils vérifié bout-en-bout (auto-démarrage, stopForks, start, configApply à distance) =="
exit 0

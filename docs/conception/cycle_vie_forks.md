# Cycle de vie dynamique des processus fils (client)

**Statut : proposition.** Ce document décrit une **cible**, pas le comportement actuel du code.

## Objectif

Aujourd'hui, le client (`handle_client`, `src/app/main.c:163-335`) forke ses `NB_THREADS` fils de
recherche **immédiatement** au démarrage, puis le thread principal se bloque dans `wait_child()`
(`src/app/app_runtime.c:532`), une boucle `wait()` sans condition de relance. Aucun paramètre
structurel — nombre de fils, serveur cible, fichier de pièces — n'est modifiable sans tuer le
process principal et le relancer.

On veut :

1. Lancer l'application **sans** démarrer les fils, pour maîtriser leur vie.
2. Une configuration saisie **en console** et **enregistrée dans un fichier**.
3. Si ce fichier existe (ou si un fichier est précisé au démarrage) : **auto-démarrage 5 secondes
   après le boot**, annulé dès qu'une nouvelle configuration est commencée.
4. Une reconfiguration **à chaud** : arrêter les fils, appliquer la nouvelle configuration,
   redémarrer les fils — sans jamais arrêter le process principal.
5. Un pilotage possible **à distance**, via le canal de contrôle et l'API HTTP admin, en plus de la
   console locale.

**Verdict de faisabilité : faisable.** Le point dur unique est de forker alors que les threads du
parent (console, checker, `server_tcp` IPC, canal de contrôle) tournent déjà, ce qui viole la règle
énoncée en commentaire à `src/app/main.c:209-214` (« aucun thread du parent ne doit tourner pendant
la boucle de `fork()` »). La section *Faisabilité et risques* détaille la mitigation retenue et ce
qu'il reste comme risque résiduel.

## Arbitrages tranchés

### D1 — L'orchestrateur vit dans le thread principal du parent

`handle_client` n'appelle plus `wait_child()` : après avoir initialisé l'état et démarré les threads
du parent, le thread principal entre dans une **boucle d'orchestration** portée par un nouveau module
`src/app/fork_orchestrator.{h,c}`. La console, le canal de contrôle et l'API HTTP **postent des
événements** dans une boîte aux lettres (mutex + condvar + file bornée) ; **seul le thread principal
exécute `fork()` et `waitpid()`**.

*Raison* : c'est la seule topologie où le fork a un exécutant unique et déterministe, et où la
récolte des fils par slot est possible sans course. Les alternatives — forker depuis le thread
console, ou déléguer à un process « spawner » forké avant tout thread — ont été écartées : la
première multiplie les forkeurs concurrents, la seconde transforme les fils en petits-fils (elle
casse `waitpid`, la remontée SIGCHLD et le routage IPC `etii_main`/`etii_fork`) pour un bénéfice
purement théorique.

### D2 — Quiescence coopérative avant chaque fork

Trois options ont été étudiées pour respecter l'invariant réel derrière la règle « mono-thread
pendant le fork » (*aucun verrou ne doit être détenu, au moment du fork, par un thread absent du
fils* — sinon le fils bloque au premier `printf`/`malloc`) :

- **(a) Process « spawner » dédié**, forké avant tout thread. Seul schéma qui respecte la règle à la
  lettre, mais il déplace SIGCHLD, `waitpid` et la remontée de statistiques dans un process
  intermédiaire et impose un protocole de pipe pour chaque changement de configuration. **Écarté**,
  trop invasif.
- **(b) `pthread_atfork` sur les verrous stdio/logger.** Les verrous internes d'un `FILE` ne sont pas
  ré-initialisables de façon portable. **Écarté**, fragile.
- **(c) Quiescence coopérative. Retenue.**

Fonctionnement de (c). Avant chaque fork, l'orchestrateur lève un drapeau `fork_quiesce` ; chaque
thread du parent possède un **checkpoint** en tête de boucle où il se gare sur une condvar. Leurs
boucles ont déjà des tours courts : `check_client_threads` sait sous-découper son `sleep(10)`
(précédent du banc de mesure, `src/app/etii_client.c:804-812`), `server_tcp` et le canal de contrôle
lisent avec timeout. Un thread garé sur `pthread_cond_wait` ne détient **aucun** verrou — ni stdio,
ni logger, ni malloc.

La console est le cas particulier : bloquée dans `read()`, elle ne détient rien non plus. On
l'instrumente avec un état atomique posé autour de son read bloquant, et la règle : au retour du
read, si `fork_quiesce` est levé, elle se gare **avant** tout traitement. L'orchestrateur considère
« garé » ou « bloqué en read » comme quiescent.

Le thread principal prend ensuite lui-même le verrou du logger, puis `flockfile(stdout)` /
`flockfile(stderr)` et `fflush(NULL)`, forke, et relâche **dans le parent et dans le fils** — les
verrous `flockfile` sont détenus par le thread forkeur, qui est le seul thread du fils, donc
`funlockfile` y est valide.

Si la quiescence n'est pas atteinte sous ~2 s (bug d'un thread qui ne rejoint pas son checkpoint),
l'orchestrateur **refuse de forker** et journalise une erreur. On ne forke jamais dans le doute.

### D3 — Pas de re-spawn automatique d'un fils mort en v1

L'orchestrateur constate les morts inattendues à son tick, les journalise et nettoie le slot. Le
re-spawn automatique interagit mal avec `--stop-on-solution` (un fils qui sort après une solution
n'est pas une panne) et mériterait sa propre politique de back-off.

*Bénéfice collatéral* : ce nettoyage corrige un trou existant. `sigchld_handler`
(`src/app/app_runtime.c:485`) moissonne les zombies mais ne remet à jour ni `childrens_pid[]` ni
`forkId[]` ; aujourd'hui `send_command_to_childs` (`src/net/local_socket.c:131`) émet donc vers des
sockets Unix disparues.

### D4 — Le canal de contrôle démarre dès le boot, avec `nb_forks = 0`

`hello.nb_forks` est aujourd'hui **figé** à la valeur passée à la création du thread
(`start_control_channel(serverIp, created)`, `src/app/etii_control.c:266-299`). Il devient la lecture
d'un global rafraîchi, relue à chaque itération de la boucle de reconnexion. Après chaque
(re)démarrage des fils, l'orchestrateur appelle `control_channel_request_reconnect()` : fermeture du
socket, la boucle de reconnexion existante fait le reste et ré-émet un hello à jour.

*Raison* : le serveur doit voir le nombre réel de fils. Côté serveur c'est une
déconnexion/reconnexion ordinaire — nouveau `session_no`, cumuls `known_clients` préservés puisqu'ils
sont indexés par `machine_uid`.

### D5 — Fichier de configuration texte clé=valeur

Format une clé par ligne, chemin par défaut `./eternityii-client.conf` (même convention que les
`.back` et `eternityii-machine_uid`), surchargé par une nouvelle option CLI valuée
`--config-file <path>` — donc une entrée dans `cli_topics[]` (`src/app/app_runtime.c`), règle du
projet pour toute nouvelle option.

Écriture atomique `.tmp` + `rename()` (patron de `src/core/datamanager.c`). Lecture **tolérante** :
fichier absent ≠ erreur, ligne vide ou `#` ignorée, clé inconnue ou valeur invalide → avertissement
et clé ignorée. Le process ne refuse jamais de démarrer à cause de ce fichier.

Toutes les chaînes lues sont **`strdup`ées**. Les globales `serverIp` / `parts_files` pointent
aujourd'hui directement dans `argv` (`parse_cli_options`, `src/app/static_variables.c:193`, stocke un
pointeur jamais copié) : on ne libère donc jamais une ancienne valeur susceptible de venir d'`argv`.

### D6 — Périmètre complet du fichier de configuration

Le fichier couvre aussi les clés qui exigent un redémarrage des fils, pas seulement celles déjà
modifiables à chaud :

```
nb_forks            = 4                 # redémarrage requis
server_host         = 192.168.1.10      # redémarrage requis
parts_file          = data/pieces.csv   # redémarrage requis (reconstruction de la map COW)
max_stock_by_thread = 200               # à chaud (IPC maxStockByThread)
limit               = 0                 # à chaud (IPC limit)
pruner_batch        = 500               # à chaud (IPC prunerBatch)
```

*Raison* : sans `server_host` ni `parts_file`, la feature ne permettrait pas de rebasculer une
machine vers un autre serveur ni de changer de puzzle sans relancer le process — soit exactement
l'usage visé. Le **mode** (client / pruner / `--gpu`) reste hors fichier : il détermine le rôle du
process, il n'est pas reconfigurable.

Priorité au démarrage : **CLI > fichier > défauts**.

### D7 — Pilotage à distance autorisé

`start`, `stopForks`, `configApply`, `config <clé> <valeur>` et `configSave` rejoignent
`control_command_allowed` (`src/net/control_protocol.c:183`), avec la double vérification habituelle :
côté serveur avant diffusion, et côté client en défense en profondeur dans
`control_channel_handle_frame`. Sur l'API HTTP ce sont toutes des commandes **modifiantes** : jeton
Bearer obligatoire, aucune n'est éligible à `control_command_read_only`. Leur exécution passe par les
branches réentrantes d'`admin_apply_remote_command` (`strtok_r`), jamais par les interpréteurs
console qui utilisent le curseur `strtok` global.

Sur un serveur, `control_registry` est vide côté client, donc pousser ces commandes vers un client
depuis un client est un no-op silencieux — même raisonnement que `pause`/`resume`.

`exit` n'entre pas dans cette liste et n'y entrera pas.

## Machine à états (`fork_orchestrator`)

États : `WAITING_CONFIG` (pas de fichier → attente manuelle, jamais de décompte), `COUNTDOWN`
(configuration chargée, auto-démarrage à T+5 s), `CONFIGURING` (une saisie a commencé → décompte
annulé définitivement), `RUNNING`, `STOPPING`, `APPLYING`, `EXITING`.

Événements : `EV_CONFIG_BEGUN`, `EV_START`, `EV_STOP_FORKS`, `EV_RESTART`, `EV_EXIT`,
`EV_CHILD_DIED`.

Cœur **pur et testable**, patron déjà employé par le dépôt (`check_client_threads_step`,
`pruner_batch_clamp`) :

```c
orch_state_t orchestrator_step(orch_state_t s, orch_event_t ev, long now_ms, orch_actions_t *out);
```

Aucune I/O, aucun fork : la boucle exécute les actions décrites dans `out`. La boucle attend en
`pthread_cond_timedwait` avec un tick de 100 ms — le décompte de 5 s vaut 50 ticks, et un message
« auto-démarrage dans N s (toute commande de configuration l'annule) » est journalisé une fois par
seconde.

En `RUNNING`, chaque tick balaie les slots avec `kill(pid, 0)` (technique déjà utilisée par la
commande `exit`, `src/ui/command_lines.c:470`) ; un slot mort déclenche `EV_CHILD_DIED` et son
nettoyage. Avec `--stop-on-solution`, la mort de tous les fils après une solution reste une sortie
propre du parent, comme aujourd'hui, et non une panne.

## Module de configuration (`src/app/client_config.{h,c}`)

API pure : `client_config_parse_line`, `client_config_load`, `client_config_format`,
`client_config_save` (`.tmp` + `rename`), et `client_config_diff(courante, staged)` renvoyant
`HOT_ONLY` ou `NEEDS_RESTART` — c'est cette fonction qui permet à `configApply` de choisir entre
simple diffusion IPC et redémarrage complet.

## Commandes console

`src/ui/command_lines.c`, `NB_COMMANDS` passe de 47 à 52.

| Commande | Sémantique |
|---|---|
| `config` | Affiche l'état de l'orchestrateur, la configuration courante et la configuration en préparation. **N'annule pas** le décompte. |
| `config <clé> <valeur>` | Écrit dans la configuration en préparation et poste `EV_CONFIG_BEGUN` → **annule le décompte**. |
| `configSave` | Écrit la configuration fusionnée dans le fichier (`.tmp` + `rename`). |
| `start` | Fork immédiat avec la configuration effective ; erreur si déjà en `RUNNING`. |
| `stopForks` | Arrête les fils sans quitter le process parent. |
| `configApply` | Diff `HOT_ONLY` → diffusion IPC seule ; `NEEDS_RESTART` → arrêt, application, re-fork. |

Chacune suit le patron du dépôt : logique pure exportée et testable, interpréteur mince, métadonnées
d'aide dans la table `commands[]`, mise à jour de `docs/console.md`. La commande `exit` est réécrite
pour poster `EV_EXIT` — comportement observable inchangé.

## Séquence de redémarrage à chaud

1. **STOPPING.** Masquer SIGCHLD (`sigprocmask`) pour neutraliser la course avec `sigchld_handler`,
   qui moissonne en `WNOHANG` et rendrait le `waitpid` par pid non déterministe. Puis, pour chaque
   slot vivant : `kill(pid, SIGINT)` (le fils passe par `signal_end_handler`, `request = REQUEST_STOP`
   local, sortie propre de `run_client`, suppression de sa socket `etii_fork.<pid>`), puis
   `waitpid(pid, …)` borné, avec escalade SIGTERM à +5 s et SIGKILL à +10 s via une fonction pure
   `stop_escalation_next(elapsed_ms)`. Nettoyage : `childrens_pid[c] = -1`, `forkId[c][0] = '\0'`,
   `fork_statistics[c]` remis à zéro. Démasquer SIGCHLD.
2. **APPLYING**, sous quiescence — les lecteurs des tableaux (`server_tcp`/`find_fork_index`, le
   checker, `control_channel_build_stats`) sont garés, donc aucun mutex dédié n'est nécessaire :
   - `nb_forks` changé → `free_childs()` (nouvelle fonction symétrique d'`init_childs`,
     `src/app/app_runtime.c:571`), `NB_THREADS = staged`, puis `init_childs()` et `init_counters()` ;
   - `parts_file` changé, tous les fils morts → `set_inherited_search_parts(NULL)`,
     `free_search_parts`, reconstruction et republication. **La propriété de la map migre** de la pile
     de `handle_client` vers l'orchestrateur. Si `parts_file` est inchangé, la map COW reste valide
     telle quelle ;
   - `server_host` et les valeurs à chaud → copie dans les globales, héritées au prochain fork.
3. **Re-fork.** La boucle actuelle (`src/app/main.c:218-284`) est extraite vers
   `orchestrator_spawn_forks()`, tolérance aux échecs partiels et bilan `count_created_forks`
   conservés, sous le protocole de quiescence D2. La socket `etii_main.<pid>` du parent est
   **réutilisée telle quelle** (le pid du parent ne change pas) ; chaque fils recrée sa
   `etii_fork.<pid>`.
4. `g_active_forks = created`, puis `control_channel_request_reconnect()` (D4).

`stop_on_solution`, `machine_uid`, `client_uid` et `label` sont des globales du parent : elles sont
ré-héritées au re-fork, rien à refaire. `client_uid` reste celui du process parent, ce qui est
cohérent — c'est la même exécution.

**Côté serveur, rien à modifier.** L'arrêt d'un fils ferme sa connexion de travail, et le mécanisme
existant de bail + preuve de vie (PR7 de la série identification_clients) re-rend distribuable le
travail prêté.

## Faisabilité et risques

| # | Risque | Mitigation | Résiduel |
|---|---|---|---|
| 1 | **Fork multi-thread** : verrou stdio/logger/malloc détenu par un thread absent du fils → interblocage au premier `printf`/`malloc` du fils (c'est l'avertissement de `main.c:209-214`). | Quiescence coopérative (D2) : threads garés sur condvar, verrous stdio pris par le forkeur lui-même. | Un thread qui ne rejoint pas son checkpoint (bug) → attente bornée à ~2 s puis **refus de forker** avec erreur journalisée. Jamais de fork dangereux. |
| 2 | **Course SIGCHLD** pendant l'arrêt : moisson `WNOHANG` concurrente du `waitpid` par slot. | Masquage SIGCHLD pendant STOPPING. | Nul : la réutilisation de pid est éliminée par le `waitpid` déterministe. |
| 3 | **Map COW** : double libération ou usage après libération si on reconstruit avec des fils vivants. | Reconstruction seulement quand zéro fils vivant ; propriété centralisée dans l'orchestrateur (migration obligatoire depuis la pile de `handle_client`). | À couvrir par ASan sous `make test-docker`. |
| 4 | **Réallocation des tableaux** `childrens_pid`/`forkId`/`fork_statistics` pendant qu'un autre thread les lit → corruption dans `find_fork_index` ou les stats. | Fenêtre de quiescence (les lecteurs sont garés). | — |
| 5 | **`hello.nb_forks` figé** : le serveur garde une vision fausse après un redémarrage. | D4 : lecture dynamique + reconnexion volontaire. | Un `session_no` supplémentaire par redémarrage côté serveur (bruit dans `clients`, sans conséquence). |
| 6 | **Client sans fils connecté au canal de contrôle** : visible dans `clients` et `GET /api/v1/clients` sans rien produire. | Acceptable — l'état de l'orchestrateur est justement l'information utile. | À documenter dans `docs/echanges_client_serveur.md`. |
| 7 | **Durée de vie des chaînes** : globales pointant dans `argv` vs copies heap issues du fichier. | D5 : tout `strdup`é, jamais de libération d'une valeur potentiellement issue d'`argv`. | Micro-fuite bornée de l'ancienne copie heap à chaque reconfiguration, assumée et documentée. |
| 8 | **`DEBUG_IN_MONO_PROCESS`** (`main.c:220-224`) : le « fork » est une exécution inline dans le parent. | Incompatible avec le mode dynamique : sous ce define, `start` exécute inline (bloquant), `stopForks`/`configApply` répondent « non supporté en mono-process ». | Documenté ; la CI compile ce mode, elle ne l'exécute pas. |
| 9 | **Pilotage à distance** (D7) : un `stopForks` distant peut arrêter la production d'un client. | Double liste blanche, jeton Bearer sur l'API HTTP, journalisation de chaque commande authentifiée. `exit` jamais dans la liste. | Assumé — c'est la fonctionnalité demandée. |
| 10 | **ncurses** : `wait()` gère aujourd'hui EINTR/SIGWINCH. | La boucle en `cond_timedwait` y est insensible ; le décompte s'affiche via le logger, donc compatible `logger_ncurses`. | — |
| 11 | **Pruner et pruner GPU** : même chemin `handle_client`, donc concernés. | À confirmer en PR C/D : aucun contexte CUDA ne doit être créé avant le fork (il l'est aujourd'hui dans le fils). | Vérification explicite inscrite au périmètre de la PR. |
| 12 | **Datagramme IPC de 100 octets** (`fork_udp`, `src/app/app_runtime.c:1014`). | Les diffusions à chaud (`limit n`, …) tiennent largement ; aucune nouvelle commande n'est envoyée aux fils. | — |

## Points laissés ouverts

- **Re-spawn automatique d'un fils mort** (politique de back-off, interaction avec
  `--stop-on-solution`) : hors périmètre, D3.
- **Exposition de l'état de l'orchestrateur et du décompte** dans `GET /api/v1/status` côté client :
  le client n'expose pas d'API HTTP aujourd'hui ; l'information passerait plutôt par `control_stats_t`
  ou un nouveau `CTRL_*`. À trancher si le besoin de supervision se confirme.
- **Nom définitif du fichier de configuration** (`eternityii-client.conf` proposé) et question de
  savoir s'il doit être **par machine** ou **par process** quand plusieurs clients tournent sur le
  même hôte depuis le même répertoire — le conflit d'écriture existe aussi pour les `.back`, mais ici
  deux clients ne veulent pas forcément la même valeur de `nb_forks`.
- **Sort du stock local d'un fils à l'arrêt** : perdu puis re-servi par le serveur via le
  requeue/bail. Correct, mais à mesurer — si la perte s'avère coûteuse sur un redémarrage fréquent,
  un renvoi explicite du stock avant sortie serait à concevoir.
- **`configApply` partiel** : le comportement en cas d'échec du re-fork (par exemple `nb_forks` trop
  grand) — rester à zéro fils, ou revenir à la configuration précédente ?

## Découpage en PR

Branche dédiée par PR, jamais sur `master`, messages de commit brefs et sans signature. Chaque PR met
à jour `README.md`, `AGENTS.md` et les documents de `docs/` concernés.

- **PR A — `client_config`.** Module de parsing/écriture clé=valeur, option `--config-file` avec son
  entrée `cli_topics[]`, lecture au démarrage appliquée aux globales (CLI > fichier), commandes
  `config` (affichage seul) et `configSave`. Tests purs dans `tests/app/test_client_config.c`. Aucun
  changement du cycle de vie.
- **PR B — Infrastructure de quiescence.** Checkpoints dans le checker, `server_tcp`, le canal de
  contrôle et la console ; primitives `fork_gate_*` (verrou logger + `flockfile`) ; nettoyage des
  slots morts au tick. Comportement externe inchangé — le fork reste avant le démarrage des threads.
  Tests des primitives, `fork_assert.h` au besoin.
- **PR C — Orchestrateur, démarrage différé et décompte de 5 s.** Module `fork_orchestrator`,
  `handle_client` restructuré (threads d'abord, boucle d'orchestration à la place de `wait_child`),
  commandes `start` et `config <clé> <valeur>` avec annulation du décompte, `nb_forks` dynamique côté
  canal de contrôle. Tests exhaustifs de `orchestrator_step` (transitions, décompte, annulation,
  fichier absent).
- **PR D — Arrêt et redémarrage à chaud.** `stopForks`, `configApply`, séquence
  arrêt/escalade/récolte sous SIGCHLD masqué, `free_childs` et réallocation sur changement de
  `nb_forks`, reconstruction de la map sur changement de `parts_file`,
  `control_channel_request_reconnect`. Tests de `stop_escalation_next`, `client_config_diff` et des
  transitions STOPPING/APPLYING.
- **PR E — Pilotage à distance et intégration.** Listes blanches de `control_protocol.c` avec la
  défense en profondeur côté client, branches réentrantes dans `admin_apply_remote_command` (jeton
  Bearer sur l'API HTTP), script `tests/integration/run_client_lifecycle.sh` sur le patron de
  `run_control_channel.sh` : serveur 16 pièces, auto-démarrage à ~5 s, `stopForks`, `configApply` avec
  changement de `nb_forks`, vérification côté serveur. Passage `make test-docker` (WERROR + ASan) et
  `make test-docker-arm`.

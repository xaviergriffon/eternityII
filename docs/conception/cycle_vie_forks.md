# Cycle de vie dynamique des processus fils (client)

**Statut : en cours d'implémentation.** PR A ([#183](https://github.com/xaviergriffon/eternityII/pull/183),
module `client_config`), PR B (infrastructure de quiescence, `src/app/fork_gate.{h,c}`), PR C
(orchestrateur de démarrage différé, `src/app/fork_orchestrator.{h,c}`) et PR D (arrêt/redémarrage à
chaud, commandes `stopForks`/`configApply`) livrées — voir le découpage en PR ci-dessous pour le détail
et le suivi. PR E reste une **proposition** : ce document continue de décrire une **cible**, pas le
comportement actuel du code, sauf pour les parties couvertes par PR A (chargement de `--config-file` au
démarrage et commandes `config`/`configSave`, documentées dans `AGENTS.md`, `README.md` et
`docs/console.md`), PR B (primitives `fork_gate_*`, checkpoints câblés dans les quatre threads candidats,
nettoyage des slots morts), PR C (démarrage réellement différé : le fork n'a plus lieu au boot mais sur
décompte de 5 s ou commande `start`/`config <clé> <valeur>`) et PR D (arrêt à chaud `stopForks`,
redémarrage à chaud `configApply` — voir `AGENTS.md`, `docs/console.md`, `docs/utilisation.md` pour le
comportement actuel). Le pilotage à **distance** de `stopForks`/`configApply` (canal de contrôle, API
HTTP admin) reste hors périmètre jusqu'à PR E.

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

**Suivi : 4/5 livrées (PR A, PR B, PR C, PR D).** Branche dédiée par PR, jamais sur `master`, messages de commit brefs et
sans signature. Chaque PR met à jour `README.md`, `AGENTS.md` et les documents de `docs/` concernés.

- **PR A — `client_config`. Livrée** ([#183](https://github.com/xaviergriffon/eternityII/pull/183)).
  Module de parsing/écriture clé=valeur (`src/app/client_config.{h,c}`), option `--config-file` avec son
  entrée `cli_topics[]`, lecture au démarrage appliquée aux globales (CLI > fichier > défauts, priorité
  décidée en réutilisant les seuils `argc` de `parse_client_args`), commandes `config` (affichage de la
  configuration EFFECTIVE, pas d'un instantané de démarrage) et `configSave` (écriture atomique
  `.tmp`+`rename`). Tests dans `tests/app/test_client_config.c` et `tests/ui/test_command_lines.c`.
  Aucun changement du cycle de vie, conformément au périmètre prévu. Un ajustement décidé pendant la
  revue, **hors du périmètre initialement décrit ci-dessus** : `config`/`configSave` sont masquées
  côté SERVEUR (ni listées dans `help`, ni exécutables, ni suggérées en cas de faute de frappe,
  `command_is_client_only` dans `src/ui/command_lines.c`) — sans ce garde-fou, exécutées sur un
  serveur, elles agiraient sur SES globales à lui (`NB_THREADS` y désigne le pool de connexions, pas un
  nombre de forks), produisant un fichier trompeur plutôt que le no-op inoffensif des autres commandes
  `[serveur]` de la table sur un client. `--config-file` reste lui aussi sans effet sur `server`
  (jamais lu par `handle_server`) : ce fichier ne concerne que `client`/`pruner`, comme prévu par
  l'arbitrage D6.
- **PR B — Infrastructure de quiescence. Livrée.** Module `src/app/fork_gate.{h,c}` : table bornée
  (`FORK_GATE_MAX_PARTICIPANTS` = 8) de participants enregistrés (`fork_gate_register`/
  `fork_gate_unregister`), point de contrôle à chemin rapide (`fork_gate_checkpoint` — une lecture
  atomique quand aucune quiescence n'est demandée, sinon le thread se gare sur une condvar),
  cas particulier des threads bloqués sans verrou (`fork_gate_mark_blocked`, pour la console),
  et `fork_gate_request_quiesce`/`fork_gate_release_quiesce` (budget par défaut 2 s — passé, la
  demande est ANNULÉE et tout participant déjà garé est relâché : jamais de fork dans le doute, cf.
  D2). Primitives d'E/S dédiées (`fork_gate_acquire_io_locks`/`_release_io_locks`) : verrou de sortie
  du logger (nouveau `logger_lock_output`/`logger_unlock_output`, ajouté à `src/ui/logger.h` et
  implémenté dans les deux variantes `logger.c`/`logger_ncurses.c`) puis `flockfile(stdout)`/
  `flockfile(stderr)` puis `fflush(NULL)` — exposées et testées de façon autonome, aucun site de PR B
  ne les appelle encore autour d'un vrai `fork()` (PR C/D).

  Checkpoints câblés dans les quatre threads candidats à la quiescence côté client PARENT : le checker
  (`check_client_threads`, `src/app/etii_client.c`), `server_tcp` (`src/app/app_runtime.c` — désormais
  doté d'un `SO_RCVTIMEO` de 1 s sur son `recvfrom`, condition nécessaire pour qu'un checkpoint soit
  jamais réévalué quand aucun fork n'envoie de statistiques), le canal de contrôle
  (`run_control_channel`, `src/app/etii_control.c`) et la console — les deux variantes, ANSI
  (`console()`, `src/ui/console.c`, via `fork_gate_mark_blocked` autour du `read()` bloquant de
  `getcmdline`) et ncurses (`nc_console_loop`, `src/ui/logger_ncurses.c`, déjà non bloquante via
  `wgetch`/`nodelay`, donc un simple checkpoint en tête de boucle suffit). Comportement externe
  **inchangé** : rien n'appelle `fork_gate_request_quiesce` en production dans cette PR, le fork reste
  avant le démarrage des threads (`main.c`).

  Nettoyage des slots morts au tick (D3, bénéfice collatéral) : `reap_dead_child_slots`/`pid_is_alive`
  (`src/app/app_runtime.{h,c}`) corrigent le trou où `sigchld_handler` moissonnait les zombies sans
  jamais mettre à jour `childrens_pid[]`/`forkId[]` — un fils mort de façon inattendue (hors sortie
  normale via `wait_child`) laissait un slot fantôme, ciblé indéfiniment par `send_command_to_childs`.
  Prédicat de vivacité injecté (`kill(pid, 0)` en production) pour rester testable sans process réels,
  même patron que `analysed_owner_alive_fn` (PR7 de `identification_clients.md`). Appelé à chaque tour
  de `check_client_threads`, sur le process PARENT uniquement.

  Tests : `tests/app/test_fork_gate.c` (enregistrement/table pleine, chemin rapide, quiescence
  multi-thread avec un vrai `pthread_create` worker, timeout avec annulation, `mark_blocked`,
  primitives d'E/S) et de nouveaux tests dans `tests/app/test_app_runtime.c`
  (`pid_is_alive_*`/`reap_dead_child_slots_*`, technique `fork()`+`waitpid()` déjà utilisée par
  `tests/ui/test_command_lines.c` pour obtenir un pid mort déterministe).
- **PR C — Orchestrateur, démarrage différé et décompte de 5 s. Livrée.** Module
  `src/app/fork_orchestrator.{h,c}` : cœur pur et total `orchestrator_step` (7 états × 6 événements,
  chaque cas explicite — pas de `default:`, pour que `-Wswitch` signale un événement oublié si l'enum
  grandit), prédicat pur `orchestrator_countdown_elapsed`, et un driver impur (`fork_orchestrator_run`,
  tick de 100 ms) qui remplace `wait_child()` dans `handle_client` — restructuré pour démarrer les
  threads du parent (checker, console, canal de contrôle, `server_tcp`) **avant** tout fork, exactement
  le scénario que PR B préparait. `handle_client` calcule l'état initial (`COUNTDOWN` si
  `client_config_load` a chargé un fichier, `WAITING_CONFIG` sinon) et ne fait plus rien d'autre côté
  fork — tout est piloté par l'orchestrateur.

  **Écarts délibérés par rapport à ce paragraphe**, tranchés pendant l'implémentation (aucun n'est une
  régression de portée, juste des détails que ce document laissait sous-spécifiés) :
  - Le décompte de 5 s n'est PAS stocké dans l'état pur : sa deadline est une variable locale du
    driver. À échéance, le driver appelle `orchestrator_step(COUNTDOWN, EV_START, now_ms, &out)` —
    exactement le même appel qu'un `start` manuel : un seul chemin de code, testé une fois.
  - La "boîte aux lettres" de D1 (mutex+condvar+file bornée) est réalisée plus simplement : un point
    d'entrée thread-safe unique, `fork_orchestrator_post_event`, applique la transition
    SYNCHRONEMENT sous verrou et rend la main avec le résultat exact (retour immédiat de `start` en
    cas d'erreur "déjà en RUNNING", sans latence de sondage) ; le thread orchestrateur dédié, seul à
    jamais appeler `fork()` (D1 respecté), se réveille sur cette même condvar. Les événements de PR C
    étant des changements d'état idempotents (pas du travail à empiler), une file bornée littérale
    n'apportait rien.
  - `EV_STOP_FORKS`/`EV_RESTART` sont déclarés et traités par `orchestrator_step` (retour à l'état
    inchangé + `ORCH_ERR_UNSUPPORTED`, dans tous les états) mais leur vraie sémantique — où atterrit
    `STOPPING` selon qu'il s'agit d'un `stopForks` ou d'un `configApply` — n'est PAS devinée : ça
    dépend de détails que seule l'implémentation de PR D peut trancher. `EV_EXIT` est géré correctement
    par le cœur pur (tout état → `EXITING`, jamais une erreur) mais aucun driver ne le poste encore ;
    `exit_interpreter` garde son comportement actuel (kill direct + `exit()`), inchangé.
  - `config <clé> <valeur>` écrit dans une configuration "en préparation" tenue par
    `fork_orchestrator.c` (réutilise `client_config_parse_line`, PR A, zéro logique dupliquée) et
    n'annule le décompte QUE si la ligne est acceptée (`CLIENT_CONFIG_LINE_SET`) — une faute de frappe
    ne doit pas coûter l'auto-démarrage. `start` (manuel ou déclenché par le décompte) consomme
    toujours la configuration EFFECTIVE, jamais celle en préparation — fidèle au libellé exact du doc
    ("Fork immédiat avec la configuration effective"). La configuration en préparation reste inerte
    tant que `configApply` (PR D) n'existe pas pour la consommer ; `configSave` (PR A) n'est pas
    modifiée, elle continue de capturer/écrire l'effective.
  - `nb_forks` dynamique (D4) : nouveau global `g_active_forks`, relu par `run_control_channel` à
    chaque reconnexion (plus figé au démarrage du thread), et `control_channel_request_reconnect()`
    (drapeau atomique one-shot, consulté dans la boucle de service interne du canal) force la
    reconnexion d'une session déjà établie après chaque (re)fork — sans lui le serveur ne voit jamais
    le vrai nombre de fils tant que la session initiale reste en vie.
  - **Deux interblocages spécifiques à macOS découverts en testant manuellement un vrai client** (pas
    par les tests unitaires — `fork_gate_suite`/`fork_orchestrator_suite` ne forkent jamais réellement
    et n'ont pas de console bloquée dans un vrai `read()`) : (1) `fflush(NULL)` dans
    `fork_gate_acquire_io_locks` bloque contre le thread console, qui détient le verrou stdio de
    `stdin` pour toute la durée de son `fgetc()` bloquant — un opérateur simplement assis au prompt
    suffisait à reproduire l'interblocage à chaque tentative de fork. (2) même après ce correctif,
    `flockfile(stdout)`/`flockfile(stderr)` ne survivent PAS de façon fiable à `fork()` sous macOS
    dans un process multi-thread — le fils hérite un verrou marqué détenu par une identité de thread
    qui n'existe plus en tant que telle, et son premier `flockfile()` (le tout premier `log_info`
    après le fork) bloque indéfiniment. Les deux corrigés en abandonnant `flockfile`/`fflush(NULL)`
    au profit de `fflush(stdout)`/`fflush(stderr)` explicites — la quiescence coopérative de PR B
    protège déjà entièrement contre le risque que `flockfile` était censé couvrir (un AUTRE thread
    mi-écriture au moment du fork), donc verrouiller le flux depuis le thread forkeur lui-même
    n'apportait aucune protection supplémentaire. Voir `AGENTS.md` (*Deferred-start orchestrator*)
    pour le diagnostic complet (`sample(1)`) ; les deux sont désormais exercés par
    `run_solution_16.sh` (client piloté par une FIFO laissée ouverte, donc réellement bloqué dans
    `fgetc()` comme un opérateur inactif, avant de forker pour de vrai).
  - La boucle de fork historique de `main.c` est déplacée dans `orchestrator_spawn_forks`, avec la
    quiescence/les verrous d'E/S pris et relâchés AUTOUR DE CHAQUE `fork()` individuel — pas une seule
    fois pour tout le lot de `NB_THREADS` : une section critique élargie à la boucle entière
    s'auto-interbloquait dès qu'un message de bilan (échec de fork, trace `DEBUG_THREAD`) reprenait le
    verrou du logger déjà détenu. `run_client` (anciennement privé à `main.c`) est devenu une fonction
    statique de `fork_orchestrator.c`, qui doit rester linkable dans le binaire de test (jamais lié à
    `main.c`).

  **Trois ajustements faits après des tests manuels réels** (le point dur du guide de tests :
  `fork_orchestrator_suite` ne détecte rien de ceci, car aucun de ces trois éléments n'est exercé sans
  un vrai opérateur au clavier) :
  1. `configSave` ne capturait que l'EFFECTIVE, jamais la configuration en préparation — un
     `config <clé> <valeur>` suivi de `configSave` puis d'un redémarrage du process ne changeait donc
     jamais rien, alors que c'est exactement le geste qu'un opérateur ferait pour rendre une
     modification permanente. Corrigé par `fork_orchestrator_merge_staged_config`, appelée par
     `configSave` juste avant l'écriture : superpose chaque clé stagée sur l'effective déjà capturée.
  2. Le décompte de 5 s n'est en pratique pas assez long pour taper une commande complète — attendre
     qu'une ligne `config <clé> <valeur>` soit intégralement saisie ET acceptée avant d'annuler le
     laissait de facto ininterruptible. Corrigé en postant `EV_CONFIG_BEGUN` dès la PREMIÈRE touche lue
     par la console (`console.c`/`logger_ncurses.c`), pas seulement sur une ligne complète et valide —
     conforme au sens littéral de `CONFIGURING` ("une saisie a COMMENCÉ"). Sans effet hors `COUNTDOWN`
     (self-loop). La console cuite (non-TTY, cas des tests pilotés par FIFO) n'est pas concernée : elle
     ne voit jamais qu'une ligne entière à la fois.
  3. Rien n'affichait le CONTENU de la configuration chargée avant le décompte — seul le fait qu'un
     fichier avait été trouvé était loggé. Un opérateur ne pouvait donc pas juger, sans rien taper,
     s'il fallait l'interrompre. `fork_orchestrator_run` logge désormais la configuration effective
     complète juste avant d'entrer en `COUNTDOWN`.

  **Deux régressions supplémentaires trouvées en re-testant manuellement le point 3 ci-dessus et la
  consommation de la configuration en préparation, toutes deux invisibles à `fork_orchestrator_suite`
  pour la même raison — aucun vrai console/fork n'y est exercé :**
  1. **Le point 3 ne s'affichait en réalité JAMAIS**, malgré le correctif : `log_info` (`src/ui/logger.c`)
     ne flushe `stdout` que si une lecture console est déjà bloquante (`input_active`, cf.
     `write_stream_locked`) — et à l'endroit où `fork_orchestrator_run` journalise, le thread console
     vient tout juste d'être lancé (asynchrone) par `handle_client` et n'a très probablement pas encore
     atteint sa première lecture bloquante. Le bloc restait donc dans le tampon de la libc, invisible
     jusqu'à ce qu'un autre événement finisse par flusher `stdout` (ou jamais, en `--headless`, où aucun
     thread console n'existe pour faire passer `input_active` à 1). Corrigé en remplaçant `log_info` par
     `log_console` (flush inconditionnel, `write_output(stdout, buf, 1)`) pour ce message ET pour le
     tick de décompte une fois par seconde — exactement la fonction que le projet réserve déjà à
     l'affichage interactif destiné à l'opérateur.
  2. **`start` ne consommait QUE l'effective, jamais la configuration en préparation** — un
     `config nb_forks 8` suivi de `start` forkait toujours avec l'ANCIENNE valeur ; il fallait
     `configSave` PUIS redémarrer le process pour que la nouvelle valeur prenne effet (correctif 1
     ci-dessus). C'est un geste bien plus naturel pour un opérateur (préparer, puis démarrer tout de
     suite) que celui couvert par correctif 1 (préparer, sauvegarder, redémarrer) — l'un ne remplace pas
     l'autre, les deux coexistent désormais. Corrigé par `fork_orchestrator_apply_staged_config`,
     appelée par le driver juste avant CHAQUE fork effectif (`start` manuel ou décompte qui va à son
     terme, même point de code) : superpose la configuration en préparation directement sur les globales
     en vigueur via `client_config_apply_direct` (`src/app/client_config.{h,c}`, jumeau inconditionnel de
     `client_config_apply_to_globals` — sans seuil `argc`, puisqu'un ordre explicite donné en cours de
     session est par construction plus récent que tout argument de lancement).
  3. **`config nb_forks <n>` au-delà du nombre initial faisait segfaulter le PARENT** — bug rapporté
     par un opérateur via un vrai crash reproduit avec exactement cette séquence : démarrage,
     Entrée, `config`, `config nb_forks 6` (au-delà du nombre initial), `configSave`, `start` — avec
     les fils déjà forkés restant vivants après coup. Cause : `init_childs()` dimensionne
     `childrens_pid`/`forkId`/`fork_statistics` sur `NB_THREADS` AU MOMENT de son appel, avant tout
     fork ; une fois que `fork_orchestrator_apply_staged_config` (correctif 2 ci-dessus) peut relever
     `NB_THREADS` après coup, la boucle de `orchestrator_spawn_forks` écrivait hors bornes dès que le
     nouveau `nb_forks` dépassait l'allocation d'origine — débordement de tas classique, cohérent
     avec le symptôme observé (le parent plante dans sa propre boucle de fork, les fils déjà créés
     avant le débordement restent vivants). Corrigé par `ensure_childs_capacity`
     (`src/app/app_runtime.{h,c}`) : agrandit (jamais ne rétrécit) les trois tableaux via `realloc`,
     préserve les slots existants, initialise les nouveaux comme `init_childs` — appelée juste après
     `fork_orchestrator_apply_staged_config` et avant `orchestrator_spawn_forks`, à chaque tentative
     de fork effective.

  Tests : `tests/app/test_fork_orchestrator.c` — matrice exhaustive de `orchestrator_step` (chaque état
  × chaque événement), `orchestrator_countdown_elapsed`, et tests d'intégration légers du driver
  thread-safe (`post_event`/`snapshot`/`stage_config_line`/`merge_staged_config`/`apply_staged_config`) ;
  `tests/app/test_app_runtime.c` pour `ensure_childs_capacity` (agrandissement en préservant les slots
  existants, no-op quand la capacité est déjà suffisante). Pas de test unitaire du fork réel
  (`orchestrator_spawn_forks`/`fork_orchestrator_run`) ni du câblage clavier — couverts par
  `make test-integration`, dont `run_solution_16.sh` pilote désormais le client par FIFO (au lieu de
  `</dev/null`) pour pouvoir lui envoyer `start` ; `run_control_channel.sh` reste inchangé, ses
  vérifications ne dépendant d'aucun fork réel.
- **PR D — Arrêt et redémarrage à chaud. Livrée.** `stopForks`, `configApply`, séquence
  arrêt/escalade/récolte sous SIGCHLD masqué, `free_childs` et réallocation sur changement de
  `nb_forks`, reconstruction de la map sur changement de `parts_file`, réutilisation de
  `control_channel_request_reconnect` (livrée en PR C) après le re-fork. `orchestrator_step` donne enfin
  sa vraie sémantique à `EV_STOP_FORKS`/`EV_RESTART` (table déclarée depuis PR C, `ORCH_ERR_UNSUPPORTED`
  jusqu'ici) : `RUNNING -> STOPPING` (`stop_forks=1`) pour les deux événements — la distinction "faut-il
  redémarrer après" n'est PAS portée par l'état pur (même convention que la deadline du décompte, cf. PR
  C) mais mémorisée par le driver (`g_restart_after_stop`, sous le même mutex) — et tout autre état ->
  inchangé + un nouveau code `ORCH_ERR_NOT_RUNNING`. `EV_START` accepte désormais aussi `ORCH_APPLYING`
  comme source de spawn : c'est le MÊME chemin qui re-forke à la fin d'un `configApply` NEEDS_RESTART,
  un seul code testé une fois.

  **Écarts délibérés par rapport à ce paragraphe**, tranchés pendant l'implémentation :
  - Masquage des signaux via `pthread_sigmask`, pas `sigprocmask` (le texte original de D2/la séquence
    d'arrêt employait ce dernier) : dans un process multi-thread, `sigprocmask` n'a pas un comportement
    portable garanti par POSIX (même s'il alias souvent `pthread_sigmask` sous Linux) — `pthread_sigmask`
    est l'appel correct pour masquer SIGCHLD sur LE thread orchestrateur spécifiquement, sans toucher aux
    autres threads du parent.
  - La séquence d'arrêt (`orchestrator_do_stop_forks`, `src/app/fork_orchestrator.c`) traite tous les
    slots vivants EN LOT (un seul chronomètre, un seul niveau d'escalade partagé) plutôt qu'un
    chronomètre par slot — plus simple, et cohérent avec le fait qu'un `kill(pid, SIGINT)` est envoyé à
    tous les slots au même instant.
  - **Bogue trouvé en testant manuellement (invisible à `fork_orchestrator_suite`, qui ne lance jamais de
    vrai fork ni de vraie boucle) : la condition de sortie de `fork_orchestrator_run` confondait « plus
    aucun fork » avec « le process doit quitter ».** Avant PR D, un fork ne pouvait tomber à zéro que par
    mort naturelle (solution + `--stop-on-solution`, crash) ou `Ctrl-C` — la condition historique
    (`remaining_forks == 0 && (ever_running || request == REQUEST_STOP)`) était donc correcte. `stopForks`
    introduit une TROISIÈME raison, délibérée, de tomber à zéro fork — qui ne doit JAMAIS terminer le
    process parent (console/canal de contrôle/API HTTP doivent rester actifs, c'est tout l'objet de la
    fonctionnalité). Corrigé par un drapeau local `forks_parked` (distinct d'`ever_running`), posé après
    un `stopForks` réussi ou un échec de re-fork en fin de `configApply`, levé dès qu'un (re)fork réussit ;
    condition de sortie devenue `remaining_forks == 0 && (request == REQUEST_STOP || (ever_running &&
    !forks_parked))` — `Ctrl-C` continue de l'emporter dans tous les cas.
  - **Second bogue, même cause (test manuel, pas de test unitaire) : `configApply` en branche HOT_ONLY ne
    vérifiait pas qu'un fork existait.** `client_config_diff` ne regarde QUE si `nb_forks`/`server_host`/
    `parts_file` a changé de VALEUR, sans aucune notion de vivacité des fils. Séquence reproduite :
    `stopForks` (zéro fork), puis `config nb_forks <même valeur qu'avant>` + `configApply` — le diff
    répond correctement `HOT_ONLY` (rien de restart-worthy n'a changé), donc l'ancienne implémentation
    appliquait la configuration aux globales du parent et répondait "configuration à chaud appliquée,
    aucun redémarrage nécessaire" — en ayant réellement ZÉRO fork vivant, un no-op trompeur. Corrigé en
    vérifiant `fork_orchestrator_snapshot() == ORCH_RUNNING` EN PREMIER, avant même de calculer le diff :
    `configApply` (comme `stopForks`) n'a de sens que contre des fils réellement en cours d'exécution.

  Tests : `tests/app/test_fork_orchestrator.c` (matrice `EV_STOP_FORKS`/`EV_RESTART`/`EV_START`-depuis-
  `APPLYING` mise à jour, seuils de `stop_escalation_next`, garde `ORCH_ERR_NOT_RUNNING` au niveau
  `post_event`), `tests/app/test_client_config.c` (`client_config_diff` : rien de stagé, clés à chaud
  seules, clé de redémarrage changée/inchangée, clé stagée sans valeur courante), `tests/app/test_app_runtime.c`
  (`free_childs` rétrécit/regrandit proprement, idempotence sur état déjà libéré). Pas de test unitaire
  du redémarrage réel (fork/signaux réels) ni du correctif de sortie de boucle — vérifiés manuellement en
  pilotant un vrai client par FIFO (même technique que `run_solution_16.sh`) : `start` -> `stopForks` ->
  le parent reste vivant et réactif -> `config nb_forks <n>` + `configApply` -> le re-fork a lieu et la
  session de canal de contrôle se reconnecte avec le nouveau nombre de fils.

  **Deux bogues supplémentaires trouvés après coup, rapportés par un opérateur ayant réellement exercé
  `configApply`** (crash sous `NCURSES=1`, configuration paraissant non prise en compte en ANSI) — ni
  l'un ni l'autre couvert par `fork_orchestrator_suite`, qui ne fork jamais réellement et n'envoie aucun
  vrai signal :
  1. `orchestrator_apply_restart_config` ne demandait AUCUNE quiescence avant de libérer/réallouer
     `childrens_pid`/`forkId`/`fork_statistics` et la map de recherche partagée — contrairement à
     l'exigence explicite de D2 pour APPLYING. Le checker, `server_tcp`, le canal de contrôle et la
     console pouvaient donc déréférencer un pointeur en cours de libération : crash quasi systématique
     sous `NCURSES=1` (rafraîchissement très fréquent de la bannière de stats), plus rare mais tout aussi
     réel en ANSI. Corrigé par `fork_gate_request_quiesce`/`_release_quiesce` autour de toute la fonction,
     qui renvoie désormais 1 (reconstruction faite) ou 0 (quiescence en échec — RIEN n'est modifié, jamais
     de reconstruction dans le doute) ; l'appelant retombe en `WAITING_CONFIG` sur 0. Verrouillé par
     `apply_restart_config_quiesces_concurrent_array_readers` : un thread compagnon en boucle serrée sur
     `fork_gate_checkpoint` lit `childrens_pid[0]` à chaque tour non garé — test de CONTRAT déterministe
     (le thread ne peut par construction pas s'exécuter pendant la quiescence), pas une mesure de timing.
  2. `orchestrator_do_stop_forks` pouvait rester bloqué indéfiniment en `STOPPING`, même après l'escalade
     SIGKILL. Le masquage de SIGCHLD (`pthread_sigmask`) ne porte que sur le thread orchestrateur ; les
     autres threads du parent ne le bloquent pas, donc `sigchld_handler` peut moissonner un enfant mort
     sur N'IMPORTE LEQUEL d'entre eux avant le `waitpid(pid, …)` ciblé de la séquence d'arrêt — qui reçoit
     alors `-1`/`ECHILD` ("plus mon enfant"), que l'ancien code confondait avec "encore vivant" au lieu de
     "déjà mort ailleurs". Corrigé par un nouveau prédicat pur `waitpid_target_is_reaped(résultat, pid,
     errno)` (errno capturé par l'appelant juste après `waitpid`, jamais lu directement par le prédicat,
     pour rester testable avec des paires synthétiques) — `ECHILD` compte désormais comme une mort.
     Reproduit et vérifié corrigé avec la séquence exacte rapportée par l'opérateur, en ANSI (console
     pilotée par FIFO) et sous `NCURSES=1` (`script -q`/`TERM=xterm`) : plus de crash, `config` rapporte
     `état=RUNNING` (plus jamais bloqué en `STOPPING`) avec le nouveau `nb_forks` effectif, et le serveur
     voit la session de canal de contrôle se reconnecter avec le compte de fils à jour.
- **PR E — Pilotage à distance et intégration.** Listes blanches de `control_protocol.c` avec la
  défense en profondeur côté client, branches réentrantes dans `admin_apply_remote_command` (jeton
  Bearer sur l'API HTTP), script `tests/integration/run_client_lifecycle.sh` sur le patron de
  `run_control_channel.sh` : serveur 16 pièces, auto-démarrage à ~5 s, `stopForks`, `configApply` avec
  changement de `nb_forks`, vérification côté serveur. Passage `make test-docker` (WERROR + ASan) et
  `make test-docker-arm`.

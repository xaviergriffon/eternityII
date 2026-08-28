# API HTTP REST admin

Ce document décrit l'API HTTP REST exposée par le serveur (`server`) quand il est
lancé avec l'option `--http-port <n>` : une interface texte (JSON sur HTTP/1.1),
pensée pour qu'une application tierce — dans **n'importe quel langage** — puisse lire
la télémétrie et piloter quelques commandes admin **sans parler le protocole binaire**
(`packet`/`possibility_packet`, [échanges client/serveur](echanges_client_serveur.md))
ni le [canal de contrôle](echanges_client_serveur.md#canal-de-contrôle-v9) (v9,
réservé aux processus client eternityII).

Le code correspondant vit dans :

- [src/net/http_codec.h](../src/net/http_codec.h) / [http_codec.c](../src/net/http_codec.c) — parsing HTTP/1.1 (dont l'en-tête `Authorization`), routage, formatage JSON, extraction/vérification du jeton Bearer (`http_extract_bearer_token`, `http_token_equals_constant_time`, `http_command_authorize`) : fonctions pures, sans socket ;
- [src/net/http_server.h](../src/net/http_server.h) / [http_server.c](../src/net/http_server.c) — écouteur réseau (thread détaché, boucle accept), les fonctions `http_*_collect` qui alimentent les vues JSON à partir de l'état serveur/registre vivant, et `http_token_load` (chargement/validation du fichier jeton au démarrage) ;
- [src/ui/command_lines.c](../src/ui/command_lines.c) (`admin_apply_remote_command`, `admin_apply_privileged_command`) — exécution des commandes admin, réentrante ;
- [src/net/control_protocol.c](../src/net/control_protocol.c) (`control_command_classify`, source unique de vérité ; `control_command_allowed`/`control_command_privileged`/`control_command_read_only` n'en sont que des projections, voir encadré ci-dessous) — `control_command_allowed` (lecture + écriture relayable) est **partagée** avec le canal de contrôle binaire, `control_command_privileged` (écriture serveur-seulement : restore/backup/sortAsc/sortDesc/sortDescMulti/split/regroup/rebalance/stockMaxRam/spill) ne l'est **pas** (accessible uniquement via cette API, jamais via le canal de contrôle) ;
- [src/app/control_registry.h](../src/app/control_registry.h) / [control_registry.c](../src/app/control_registry.c) (`control_registry_snapshot`, `control_registry_record_stats`, `control_registry_broadcast_get_stats`) — registre des sessions de [canal de contrôle](echanges_client_serveur.md#canal-de-contrôle-v9), source de `GET /api/v1/clients` et `POST /api/v1/clients/stats` ;
- [src/app/known_clients_registry.h](../src/app/known_clients_registry.h) / [known_clients_registry.c](../src/app/known_clients_registry.c) (`known_clients_registry_snapshot`) — [registre de clients connus](echanges_client_serveur.md#registre-de-clients-connus) (cumul par `machine_uid`, survit à la déconnexion), source de `GET /api/v1/known-clients` ;
- [src/core/best_board.h](../src/core/best_board.h) / [best_board.c](../src/core/best_board.c) (`g_server_best_board`) — représentation du meilleur plateau connu, source de `GET /api/v1/best-board` ;
- [src/core/datamanager.c](../src/core/datamanager.c) (`datamanager_stock_distribution`) — répartition du stock par `alloc`, source **partagée** de `GET /api/v1/stock-distribution` et de la commande console `statistic`.

## Activation

Désactivée par défaut. À activer explicitement au démarrage du serveur :

```sh
./eternityII server 4 --http-port 8080 data/pieces.csv
```

`--http-port` est une option position-indépendante (comme `--expand-level`), retirée
d'`argv` avant l'analyse positionnelle des arguments du mode. Une valeur absente ou
hors de l'intervalle `[1, 65535]` est **ignorée silencieusement** (l'API reste
désactivée) plutôt que d'ouvrir un port au hasard.

Optionnellement, `--http-token-file <chemin>` active l'authentification de toutes
les commandes de MODIFICATION (`pause`, `resume`, `limit`, `maxStockByThread`,
`prunerBatch`, `prunerDfsBudget`, `clientsCommand`/`clientsCmd`, `restore`, `backup` — voir
[Authentification](#authentification) ci-dessous) :

```sh
chmod 600 /etc/eternityii/http-token
./eternityII server 4 --http-port 8080 --http-token-file /etc/eternityii/http-token data/pieces.csv
```

## Posture de sécurité

- **Écoute en boucle locale uniquement** (`127.0.0.1`, jamais `0.0.0.0`/`INADDR_ANY`) :
  l'API n'est **jamais** exposée hors de la machine par défaut. Un accès distant passe
  par un tunnel SSH ou un reverse-proxy explicite, à la charge de l'opérateur.
- **Lecture libre, écriture authentifiée.** Sans `--http-token-file`, l'API se
  comporte comme une API en lecture seule : toutes les routes `GET` et `clientsWork`
  (`POST /api/v1/command`) fonctionnent sans authentification (réseau de confiance
  supposé — la machine elle-même, ou un tunnel authentifié en amont — ne jamais
  exposer directement sur un réseau non maîtrisé), mais **aucune commande de
  modification** (`pause`, `resume`, `limit`, `maxStockByThread`, `prunerBatch`,
  `prunerDfsBudget`, `clientsCommand`/`clientsCmd`, `restore`, `backup`) n'est exécutable. Avec
  `--http-token-file`, un jeton Bearer devient nécessaire pour toutes ces commandes de
  modification — plus seulement `restore`/`backup` (voir
  [Authentification](#authentification)).
- **Liste blanche stricte des commandes** (voir [POST /api/v1/command](#post-apiv1command))
  : `exit`, `import` et toute autre commande destructrice sont **rejetées avant même
  d'être interprétées**, jamais atteignables depuis cette API, jeton ou non.
- **Aucun fichier n'est servi** : pas de chemin de requête interprété comme un chemin
  disque, donc pas de risque de traversée de répertoire.

## Modèle réseau

Volontairement minimal — c'est une API d'administration occasionnelle, pas un serveur
web de production :

- **Une requête par connexion.** Chaque réponse porte `Connection: close` : le client
  doit ouvrir une nouvelle connexion TCP pour chaque appel (pas de keep-alive HTTP/1.1,
  pas de pipelining). La plupart des bibliothèques HTTP client gèrent ça de manière
  transparente (elles rouvrent une connexion si le serveur ferme).
- **Un seul thread accepteur**, requêtes traitées **séquentiellement**. Ne pas
  paralléliser les appels côté client : ils seront simplement mis en attente par le
  système d'exploitation (backlog TCP de 4), pas d'erreur, mais pas de gain non plus.
- **Requête plafonnée à 8 Ko** (en-têtes + corps compris) : au-delà, `413 Payload Too
  Large` sans tentative de lecture supplémentaire. Largement suffisant pour tous les
  usages de cette API (les corps de requête réels font quelques dizaines d'octets).
- **Timeout d'E/S de 5 s** par connexion : un client qui ouvre la connexion sans
  envoyer de requête complète, ou qui ne lit pas la réponse, voit sa connexion
  abandonnée côté serveur après ce délai.
- **Sous-ensemble HTTP/1.1** : ligne de requête + en-têtes + corps optionnel borné par
  `Content-Length`. Pas de `Transfer-Encoding: chunked`, pas de query string dans
  l'URL, pas de fragments. `Content-Length` est le seul en-ête interprété (recherche
  insensible à la casse).

## Format des réponses

Toute réponse a la forme :

```
HTTP/1.1 <code> <libellé>
Content-Type: application/json
Content-Length: <n>
Connection: close

<corps JSON>
```

| Code | Libellé | Sens |
|---|---|---|
| `200` | OK | Requête traitée avec succès |
| `400` | Bad Request | Requête HTTP malformée, ou `POST /api/v1/command` avec un champ `command` absent/invalide/aux arguments manquants |
| `401` | Unauthorized | `POST /api/v1/command` avec une commande de **modification** (toutes sauf `clientsWork`, voir [Authentification](#authentification)) sans jeton Bearer valide (absent, invalide, ou aucun jeton configuré côté serveur) — porte l'en-tête `WWW-Authenticate: Bearer` |
| `403` | Forbidden | Commande reconnue mais hors des deux listes blanches (ex. `exit`) |
| `404` | Not Found | Chemin inconnu |
| `405` | Method Not Allowed | Chemin connu, mauvaise méthode HTTP (ex. `POST /api/v1/stats`) |
| `413` | Payload Too Large | Requête (en-têtes + corps) dépassant 8 Ko |

Les corps d'erreur ont la forme `{"error":"<message>"}` ; un succès de commande
renvoie `{"result":"ok"}`. Les messages d'erreur sont informatifs mais **non
contractuels** (ne pas faire de correspondance exacte de chaîne côté client — se fier
au code de statut HTTP).

<a id="le-champ-alloc"></a>
### Le champ `alloc` (et `max_result`)

Plusieurs routes exposent `alloc` — ou sa variante « meilleur atteint »,
`max_result`/`best_max_result`. Depuis la bascule MRV (voir
[docs/conception/mrv_moteur_unique.md](conception/mrv_moteur_unique.md)),
`alloc` est le **nombre de pièces posées sur le plateau** — le compte exact des
cases non vides de `grid` —, de `0` (plateau vide) à `ETERN_PARTS` (256, ou 16 en
build `ETERN_PARTS=16`). Ce n'est plus une position dans un ordre de parcours :
le moteur de recherche (MRV) choisit la case la plus contrainte à chaque étape,
sans ordre fixe, donc deux plateaux au même `alloc` peuvent avoir des cases
différentes remplies. `directions[]`/`dirx[]`/`diry[]` ne survivent que comme
un ordre d'énumération déterministe (départage d'ex æquo), plus comme un
référentiel d'état.

Un consommateur qui veut le nombre de pièces d'un plateau peut lire `alloc`
directement, ou le retrouver indépendamment en comptant les cases de `grid` qui
ne valent pas `null` (voir [`GET /api/v1/best-board`](#get-apiv1best-board)) —
les deux comptes coïncident toujours.

<a id="le-champ-min_candidats"></a>
### Le champ `min_candidats` (seconde coordonnée de `alloc`)

Deux plateaux au même `alloc` (même nombre de pièces posées) n'ont pas
forcément la même difficulté : l'un peut avoir été atteint par une suite de
choix presque forcés, l'autre par une suite de choix larges. `min_candidats`
donne le nombre de candidats de la case qui a reçu la **dernière** pièce posée
sur ce plateau, tel que calculé par le moteur MRV au moment du choix (score
`mrv_choose_cell`, cf. [docs/autosearch_step.md](autosearch_step.md)) — un
score bas signale un plateau atteint par un chemin contraint.

`-1` signifie « non mesuré » : plateau non issu du moteur de recherche MRV
(genèse, expansion en ordre fixe), ou stock restauré depuis un fichier écrit
avant l'introduction de ce champ. Ne jamais confondre avec `0` : un
sous-arbre à 0 candidat est mort et n'est jamais matérialisé, `0` n'apparaît
donc jamais comme un score réel — `GET /api/v1/stock-distribution` s'en sert
pour représenter « aucune mesure disponible pour ce niveau » sans ambiguïté
(`avg_min_candidats: 0.0`).

## Endpoints

### GET /api/v1/stats

Instantané de la télémétrie serveur courante.

```json
{
  "shots_per_second": 12345,
  "possibility_stock": 100,
  "checked_stock": 20,
  "analysed_stock": 5,
  "max_result": 42,
  "active_threads": 3,
  "pruner_checked": 0,
  "pruner_removed": 0,
  "stock_spilled_packets": 0,
  "stock_spill_segments": 0,
  "queues": [
    { "file": 0, "unchecked": 10, "checked": 2, "analysed": 1 },
    { "file": 1, "unchecked": 8,  "checked": 0, "analysed": 0 }
  ]
}
```

| Champ | Type | Sens |
|---|---|---|
| `shots_per_second` | entier ≥ 0 | Débit de recherche courant (essais/seconde), même valeur que le bandeau `coups/s` de la console — publié toutes les 10 s, donc granularité de mise à jour de cet ordre |
| `possibility_stock` | entier ≥ 0 | Total des possibilités **non vérifiées** en stock (somme de toutes les files) — RÉSIDENT uniquement, hors débordement sur disque (voir `stock_spilled_packets` ci-dessous) |
| `checked_stock` | entier ≥ 0 | Total des possibilités **vérifiées** en attente de service — résident uniquement, même remarque |
| `analysed_stock` | entier ≥ 0 | Total des possibilités dans le pool **en cours d'analyse** (distribuées aux pruners, pas encore acquittées) |
| `max_result` | entier ≥ 0 | Meilleur nombre de pièces posées atteint (voir [le champ `alloc`](#le-champ-alloc)), 0 à 256 (ou 0 à 16 en build `ETERN_PARTS=16`) |
| `active_threads` | entier ≥ 0 | Nombre de connexions clients actuellement servies (canal de travail **et** de contrôle confondus, cf. [dimensionnement](echanges_client_serveur.md#impact-sur-le-dimensionnement-du-serveur)) |
| `pruner_checked` / `pruner_removed` | entier ≥ 0 | Toujours `0` côté serveur (ces compteurs n'existent que côté processus pruner ; conservés dans le schéma pour rester alignable avec `control_stats_t` du canal de contrôle) |
| `stock_spilled_packets` | entier ≥ 0 | Possibilités actuellement déportées sur disque ([`--stock-spill-dir`](utilisation.md#débordement-sur-disque-du-stock---stock-spill-dir)), tous pools et toutes files confondus. `0` si le débordement est désactivé, illimité (`--stock-max-ram` absent), ou simplement inactif à cet instant |
| `stock_spill_segments` | entier ≥ 0 | Nombre de fichiers de segment de débordement actuellement sur disque, tous pools et toutes files confondus |
| `queues` | tableau | Une entrée par file de stock **active** (`nb_file_possibility`, configurable au démarrage via `--stock-files`, 10 par défaut, jusqu'à 128 — voir [Utilisation](utilisation.md#maîtrise-de-la-charge-serveur---stock-files---rebalance-budget---tcp-timeout)), avec ses trois compteurs par pool — RÉSIDENT uniquement, comme `possibility_stock`/`checked_stock`. L'ordre des entrées suit l'index de file (0 à `nb_file_possibility - 1`), pas garanti trié par une autre clé |

### GET /api/v1/status

Instantané de l'état et de la configuration courante.

```json
{
  "state": "admin_pause",
  "uptime_seconds": 3600,
  "version": 9,
  "limit": 1000,
  "max_stock_by_thread": 500,
  "pruner_batch": 64,
  "pruner_dfs_budget": 10000,
  "last_backup_duration_ms": 42,
  "stock_ram_limit_mb": 2048,
  "stock_ram_used_mb": 512
}
```

| Champ | Type | Sens |
|---|---|---|
| `state` | chaîne | Voir tableau ci-dessous |
| `uptime_seconds` | entier ≥ 0 | Secondes écoulées depuis le démarrage de l'API HTTP (pas depuis le démarrage du serveur, qui la précède de quelques instants) |
| `version` | entier | Numéro de version du protocole **binaire** (`VERSION`, actuellement 9) — informatif uniquement : l'API HTTP n'est pas versionnée avec lui (voir [Compatibilité](#compatibilité)) |
| `limit` | entier ≥ 0 | Débit de recherche maximum configuré (essais/seconde) ; `0` = illimité |
| `max_stock_by_thread` | entier | Seuil de stock local par thread avant délégation au serveur |
| `pruner_batch` | entier | Taille de lot d'échange du pruner courante |
| `pruner_dfs_budget` | entier | Budget de nœuds courant de la preuve de fermeture bornée du pruner (§4.6b) ; `0` = désactivée |
| `last_backup_duration_ms` | entier ≥ 0 | Durée (millisecondes) de la DERNIÈRE sauvegarde automatique effectivement exécutée — englobe tout ce que ce tour a réellement déclenché (stock/analysé, meilleur plateau, clients connus — chacun sauté indépendamment si son propre artefact n'a pas changé). `0` tant qu'aucune sauvegarde n'a encore eu lieu |
| `stock_ram_limit_mb` | entier ≥ 0 | Plafond RAM configuré des deux pools de stock (non vérifié + vérifié), en Mo — [`--stock-max-ram`](utilisation.md#plafond-ram-du-stock---stock-max-ram)/commande `stockMaxRam`. `0` = illimité |
| `stock_ram_used_mb` | entier ≥ 0 | Occupation ESTIMÉE actuelle des deux pools de stock, en Mo (jamais un relevé RSS réel du process — dérivée du nombre de possibilités résidentes) |

Valeurs possibles de `state` :

| Valeur | Sens |
|---|---|
| `running` | Recherche active (`REQUEST_CONTINUE`) |
| `admin_pause` | Pause administrative posée par la commande `pause` (ou `POST /api/v1/command {"command":"pause"}`) — ne se lève que par `resume` |
| `regulation_pause` | Pause de régulation de débit interne (`limit`), levée automatiquement par le régulateur — pas une action externe |
| `stopping` | Arrêt du serveur en cours (`exit` a été demandé par un autre canal — jamais atteignable via cette API, voir liste blanche) |
| `unknown` | Valeur de repli, ne devrait jamais apparaître en usage normal |

### POST /api/v1/command

Exécute une commande admin whitelistée.

**Requête :**

```json
{ "command": "limit 1000" }
```

`command` est une chaîne unique contenant le nom de la commande et, le cas échéant,
son argument, séparés par un espace — exactement comme sur la console interactive du
serveur. **Limitation volontaire de l'extracteur JSON** : les échappements
(`\"`, `\\`, `\uXXXX`, …) ne sont **pas** supportés dans la valeur de `command` et
provoquent un `400` plutôt qu'une interprétation incorrecte ; ce n'est pas gênant en
pratique car toutes les commandes whitelistées n'utilisent que `[A-Za-z0-9 ]`.

**Commandes acceptées** (`control_command_allowed`, identique à la liste blanche du
[canal de contrôle](echanges_client_serveur.md#double-vérification-de-la-liste-blanche)) :

| Commande | Effet | Authentification |
|---|---|---|
| `pause` | Pose une pause administrative (`state` devient `admin_pause`) | **requise** |
| `resume` | Lève une pause administrative posée par `pause` | **requise** |
| `limit <n>` | Fixe le débit maximum de recherche à `n` essais/seconde (`0` = illimité) | **requise** |
| `maxStockByThread <n>` | Fixe le seuil de stock local par thread | **requise** |
| `prunerBatch <n>` | Fixe la taille de lot du pruner, bornée à `[1, PRUNER_BATCH_MAX]` (65536) — une valeur hors borne est silencieusement ramenée à la borne la plus proche, pas un `400` | **requise** |
| `prunerDfsBudget <n>` | Fixe le budget de nœuds de la preuve de fermeture bornée du pruner (§4.6b), bornée à `[0, PRUNER_DFS_BUDGET_MAX]` (10000000) — `n <= 0` la désactive ; valeur hors borne ramenée silencieusement, pas un `400` | **requise** |
| `clientsCommand [--to <session_no\|client_uid\|label>] <ligne...>` (alias `clientsCmd`) | Équivalent HTTP de la commande console du même nom : sans `--to`, diffuse `<ligne...>` à toutes les sessions de [canal de contrôle](echanges_client_serveur.md#canal-de-contrôle-v9) actives ; avec `--to <cible>`, l'envoie à UNE SEULE session (résolue par `session_no` décimal, `client_uid` hexadécimal, ou `label` déclaré — cf. `control_registry_send_command_to`). `<ligne...>` elle-même est revérifiée par `control_command_allowed` avant tout envoi : cibler une session n'élargit jamais le jeu de commandes exécutables sur un client (`exit`, `restore`, … restent hors de portée même via `clientsCommand --to`) | **requise** |
| `clientsWork <session_no\|client_uid\|label>` | Consultation en lecture seule : journalise (`log_info`, aucune donnée dans le corps de la réponse — voir note ci-dessous) ce que le serveur a lui-même attribué à la session ciblée (même résolution de cible que `clientsCommand --to`) | **aucune** |

**Cinq commandes de `control_command_allowed` listées ci-dessus dans le code mais
JAMAIS directement exécutables via cette route** : `start`, `stopForks`,
`configApply`, `config [<clé> <valeur>]`, `configSave` (cycle de vie des fils,
voir [Pilotage à distance du cycle de vie des fils](echanges_client_serveur.md#pilotage-à-distance-du-cycle-de-vie-des-fils))
sont whitelistées pour être poussées par le SERVEUR vers un CLIENT sur le [canal de
contrôle](echanges_client_serveur.md#canal-de-contrôle-v9) (`CTRL_COMMAND`), mais
répondent toujours `403` en `command` direct de `POST /api/v1/command` — cette route
n'est atteignable que depuis `runserver` (`--http-port` est une option serveur
uniquement), et ces cinq commandes agissent sur `fork_orchestrator`/`client_config`,
qui ne veulent rien dire côté serveur (`NB_THREADS` y désigne le pool de connexions,
pas un nombre de forks ; `fork_orchestrator_run` n'y tourne jamais) — même garde-fou
que `command_is_client_only` pour la console (`admin_remote_command_is_client_only`,
`src/ui/command_lines.c`). Pour les déclencher à distance sur un client précis,
passer par `clientsCommand --to <cible> <commande>` (ligne du tableau ci-dessus), qui
les relaie telles quelles sur le canal de contrôle de ce client — ex.
`{"command":"clientsCommand --to jetson-1 stopForks"}` puis
`{"command":"clientsCommand --to jetson-1 configApply"}`.

**Toute commande de modification exige un jeton Bearer valide** (voir
[Authentification](#authentification) ci-dessous) — `clientsWork` est la SEULE
exception : une consultation pure (`control_command_read_only`,
`src/net/control_protocol.c`) qui ne change aucun état, ni local ni distant, et reste
donc accessible sans authentification, au même titre que les routes `GET`. Une cible
`--to`/`clientsWork` inconnue, déconnectée, ou un `label` ambigu (partagé par
plusieurs sessions) répond `400`, pas `403`/`401` — la commande elle-même reste
whitelistée (et, pour `clientsCommand`, authentifiée si un jeton valide a été fourni),
seul son argument est invalide. **`clientsWork` ne renvoie aucune donnée dans le corps
JSON** : `POST /api/v1/command` ne répond jamais que `{"result":"ok"}` en cas de
succès (même contrat que pour `clientsStats`, cf.
[`POST /api/v1/clients/stats`](#post-apiv1clientsstats) plus haut) ; le résultat de la
consultation (nombre de possibilités attribuées, `alloc` max) est à lire dans les
journaux du serveur.

**Commandes privilégiées** (`control_command_privileged`), **UNIQUEMENT** accessibles
via cette route, et seulement avec un jeton Bearer valide (voir
[Authentification](#authentification)) :

| Commande | Effet |
|---|---|
| `restore [fichier [fichier_analyse]]` | Remplace le stock courant par le contenu des fichiers `.back` indiqués (défaut : `./eternityII.back` / `./eternityII-in_analyse.back`) — remet aussi en place, avant l'import, le cliché de débordement disque le plus récent (`<--stock-spill-dir>/snapshot/`, absence tolérée) |
| `backup` | Sauvegarde les files courantes dans les fichiers `.back`, ainsi qu'un cliché incrémental du débordement disque si `--stock-spill-dir` est actif |
| `sortAsc` | Trie tout le stock par ordre croissant (moins avancées d'abord) — équivalent HTTP de la commande console `sortAsc` |
| `sortDesc [n]` | Trie par ordre décroissant, toutes les files ou la file `<n>` si précisé — équivalent HTTP de `sortDesc` |
| `sortDescMulti` | Trie toutes les files en parallèle (multi-thread) — équivalent HTTP de `sortDescMulti`, peut prendre du temps sur un gros stock |
| `split` | Répartit les possibilités entre les différentes files — équivalent HTTP de `split` |
| `regroup` | Regroupe toutes les possibilités dans une seule file — équivalent HTTP de `regroup` |
| `rebalance [n]` | Rééquilibre le stock d'un seul pas incrémental (file la plus pleine → la plus vide, `n` possibilités par pool, défaut `rebalance_budget`) — équivalent HTTP de `rebalance` |
| `stockMaxRam <mo>` | Fixe à chaud le [plafond RAM du stock](utilisation.md#plafond-ram-du-stock---stock-max-ram) — équivalent HTTP de `stockMaxRam` ; `<mo> <= 0` désactive le plafond (illimité) |
| `spill [n]` | Déclenche immédiatement un pas de [débordement/rechargement sur disque](utilisation.md#débordement-sur-disque-du-stock---stock-spill-dir) — équivalent HTTP de `spill` ; `n` optionnel : budget de possibilités pour ce pas (défaut 4096), `<n> <= 0` fourni explicitement est un usage invalide |

Ces six dernières commandes (`sortAsc`/`sortDesc`/`sortDescMulti`/`split`/`regroup`/`rebalance`)
ne remplacent aucun fichier, mais réorganisent en bloc, sous verrou, l'ensemble du
stock de possibilités du serveur — un effet de bord suffisamment large (et
potentiellement coûteux) pour n'avoir de sens que côté serveur. `stockMaxRam`/`spill` rejoignent
cette liste pour la même raison (modifient un réglage/état strictement serveur), sans en
partager le coût pour `stockMaxRam` (une simple affectation, jamais un réarrangement du stock) ;
`spill` réarrange bien le stock (RAM ↔ disque), mais par petits pas incrémentaux comme
`rebalance`, jamais en bloc.

> **Deux listes, un seul niveau d'authentification.** `control_command_allowed`
> ("standard", partagée avec le canal de contrôle) et `control_command_privileged`
> ("privilégiée", HTTP-only) restent deux listes disjointes, mais elles n'encodent
> **plus** deux niveaux d'authentification différents sur cette route : `pause`,
> `limit`, `clientsCommand`… (standard, modifiantes) exigent exactement le même
> jeton Bearer que `restore`/`backup`/`sortDesc`… (privilégiées). Le seul axe qui
> détermine encore l'authentification est lecture-vs-écriture (`clientsWork` est la
> seule commande dispensée de jeton) ; le clivage standard/privilégié encode un axe
> **différent et orthogonal** : relayable à un client via `CTRL_COMMAND` (standard)
> ou strictement serveur/HTTP (privilégiée). Voir `control_command_class_t`
> (`src/net/control_protocol.h`), qui rend ces deux axes explicites plutôt que de
> les laisser se déduire du nom des deux listes.

Toute autre commande — en particulier `exit`, `import` — est **rejetée avant même
d'être tokenisée**, avec `403`, jeton ou non.

**Sans `--http-token-file` configuré, cette API se comporte comme une API en lecture
seule** : toutes les routes `GET`, plus `clientsWork` via `POST /api/v1/command`,
restent pleinement fonctionnelles ; toute autre commande (`pause`, `limit`,
`clientsCommand`, `restore`, `backup`, …) répond `401`, sans exception — même
comportement que `restore`/`backup` avant cette page (voir
[Authentification](#authentification) ci-dessous).

**Réponses :**

| Cas | Code | Corps |
|---|---|---|
| Commande sans authentification requise (`clientsWork`), ou authentifiée avec succès, appliquée | `200` | `{"result":"ok"}` |
| Commande de modification sans jeton valide (absent, invalide, ou aucun jeton configuré) | `401` | `{"error":"unauthorized"}` (en-tête `WWW-Authenticate: Bearer`) |
| Commande hors des deux listes blanches | `403` | `{"error":"command not allowed"}` |
| Champ `command` absent, vide, mal formé (JSON invalide, échappement) | `400` | `{"error":"missing or malformed \"command\" field"}` |
| Commande reconnue mais argument manquant (ex. `"limit"` sans nombre) | `400` | `{"error":"missing or invalid argument"}` |

**Exécution non bloquante pour les autres canaux.** La commande est appliquée par
`admin_apply_remote_command` (commandes standard) ou `admin_apply_privileged_command`
(commandes privilégiées, après authentification) — et non par la fonction console
`do_command_line`, qui utilise un curseur de tokenisation global non réentrant : un
appel HTTP concurrent à une saisie sur la console interactive, ou à une commande
poussée via le [canal de contrôle](echanges_client_serveur.md#canal-de-contrôle-v9),
ne corrompt jamais le découpage de l'autre.

### Authentification

**Toute commande de modification** — `pause`, `resume`, `limit`, `maxStockByThread`,
`prunerBatch`, `prunerDfsBudget`, `clientsCommand`/`clientsCmd`, `restore`, `backup` — exige un jeton
Bearer valide. Seule `clientsWork` (pure lecture, `control_command_read_only`) en est
dispensée, au même titre que les routes `GET`. Par défaut (sans `--http-token-file`),
**aucune** commande de modification n'est exécutable via cette API : sans jeton
configuré côté serveur (`HTTP_ADMIN_TOKEN` vide), `http_command_authorize` répond
toujours `401` — jamais d'exécution sans preuve d'identité, même en l'absence de
configuration. Ce n'était, avant cette page, vrai que pour `restore`/`backup` ; c'est
désormais la règle pour toute commande qui change un état, local ou distant.

**Activation :**

```sh
echo -n "un-secret-suffisamment-long" > /etc/eternityii/http-token
chmod 600 /etc/eternityii/http-token
./eternityII server 4 --http-port 8080 --http-token-file /etc/eternityii/http-token data/pieces.csv
```

- Le fichier doit être lisible du **seul propriétaire** (`mode & 0077 == 0`, même
  exigence qu'une clé privée SSH) : un mode plus permissif fait échouer le démarrage
  du serveur avec un message explicite, **avant** l'ouverture du port TCP.
- Une seule ligne = le jeton ; les espaces/retours à la ligne de fin sont retirés.
- `--http-token-file` sans `--http-port` est accepté (avertissement au démarrage,
  jeton inutilisé) — pratique pour préparer une configuration à l'avance.
- `--http-port` **sans** `--http-token-file` démarre normalement (avertissement au
  démarrage, pas un échec) : l'API reste utilisable en lecture (`GET`, `clientsWork`),
  mais aucune commande de modification n'y sera jamais acceptée tant qu'un jeton n'est
  pas configuré.

**Utilisation :** ajouter l'en-tête `Authorization: Bearer <jeton>` à la requête
`POST /api/v1/command` :

```sh
curl -X POST \
     -H "Authorization: Bearer un-secret-suffisamment-long" \
     -d '{"command":"pause"}' \
     http://127.0.0.1:8080/api/v1/command
```

**Ce que l'authentification ne change PAS :**

- `clientsWork` reste accessible **sans** authentification, jeton configuré ou non —
  c'est une consultation pure, qui ne modifie aucun état.
- Les routes `GET` (`stats`, `status`, `clients`, `best-board`, `known-clients`) ne
  demandent jamais de jeton.
- Le [canal de contrôle binaire](echanges_client_serveur.md#canal-de-contrôle-v9)
  (`CTRL_COMMAND`) reste borné à `control_command_allowed` des deux côtés, sans aucune
  notion de jeton : `control_command_read_only` n'existe que pour cette API HTTP et
  n'est jamais consulté par le canal de contrôle ni par la console — un opérateur avec
  accès console (déjà de confiance) et le serveur poussant `pause`/`resume` à un
  client déjà connecté ne sont pas concernés par cette exigence.

**Comparaison en temps constant.** Le jeton fourni est comparé au jeton configuré via
`http_token_equals_constant_time` (XOR cumulé sur toute la longueur comparée, sans
retour anticipé sur un mismatch de longueur), pour ne pas laisser le temps de réponse
fuiter combien de caractères initiaux d'un jeton deviné sont corrects.

**Anti-bruteforce minimal.** Un jeton **présent mais invalide** ajoute un délai
d'environ 200 ms avant la réponse `401` (aucun délai si le jeton est simplement
absent). Le serveur HTTP admin traite une connexion à la fois (voir
[Modèle réseau](#modèle-réseau)) : ralentir cette unique voie séquentielle suffit à
rendre un essai exhaustif de jetons peu pratique, sans le moindre état à maintenir
côté serveur (pas de compteur, pas de fenêtre glissante, pas de blocage d'IP).

**Journalisation.** Toute tentative sur une commande privilégiée est journalisée
(succès via `log_info`, échec — jeton absent ou invalide — via `log_error`) ; le
contenu du jeton lui-même n'apparaît **jamais** dans les journaux.

### GET /api/v1/clients

Instantané des sessions de [canal de contrôle](echanges_client_serveur.md#canal-de-contrôle-v9)
actives — l'équivalent HTTP de la commande console `clients`, statistiques par client
comprises (équivalent de `clientsStats`, voir plus bas).

```json
{
  "clients": [
    {
      "session_no": 1,
      "pid": 4242,
      "forks": 4,
      "mode": "search",
      "label": "jetson-1",
      "machine_uid": "0102030405060708090a0b0c0d0e0f10",
      "client_uid": "101112131415161718191a1b1c1d1e1f",
      "ip": "192.168.1.42",
      "last_activity": 1730000000,
      "stats": null
    },
    {
      "session_no": 2,
      "pid": 5555,
      "forks": 1,
      "mode": "pruner",
      "label": "workstation-3",
      "machine_uid": "aabbccddeeff00112233445566778899",
      "client_uid": "998877665544332211ffeeddccbbaa00",
      "ip": "192.168.1.55",
      "last_activity": 1730000042,
      "stats": {
        "shots_per_second": 12345,
        "possibility_stock": 300,
        "analysed_stock": 12,
        "max_result": 180,
        "pruner_checked": 900,
        "pruner_removed": 40,
        "pruner_cells_per_second": 320,
        "stats_time": 1730000040
      }
    }
  ]
}
```

| Champ | Type | Sens |
|---|---|---|
| `clients` | tableau | Une entrée par session de canal de contrôle active (`control_registry_snapshot`) — **tableau vide** si aucun client n'est connecté, jamais une erreur |
| `session_no` | entier | *(v12)* Identifiant de session monotone attribué par le serveur à l'enregistrement (`control_registry_register`), **jamais réutilisé** même si le slot de registre sous-jacent l'est après une déconnexion |
| `pid` | entier | PID du processus **parent** du client (jamais un fork de recherche, cf. canal de contrôle) |
| `forks` | entier ≥ 0 | Nombre de processus de recherche forkés par ce client |
| `mode` | chaîne | `search` (client de recherche), `pruner` (élagage CPU), `gpu_pruner` (élagage GPU), ou `unknown` (valeur de repli, ne devrait pas apparaître en usage normal) |
| `label` | chaîne | *(v12)* Libellé déclaré du client (option CLI `--name`, défaut le nom d'hôte) — affichage seul, jamais une clé. **Déclaratif et non vérifié** (à la différence de `ip`) : échappé côté serveur avant sérialisation JSON, puisqu'un client peut y placer un contenu arbitraire |
| `machine_uid` / `client_uid` | chaîne hexadécimale | *(v12)* Nonces 128 bits encodés en hexadécimal (`net/client_identity.h`) : `machine_uid` identifie la MACHINE et survit à un redémarrage du client (persisté dans `--machine-uid-file`) ; `client_uid` identifie CETTE EXÉCUTION du process parent (tiré à chaque démarrage, jamais persisté) |
| `ip` | chaîne | Adresse IP du pair de la connexion TCP (`accept()`, capturée par `inet_ntop` côté serveur) — contrairement aux autres champs, **non falsifiable** : le client ne la déclare pas, elle vient de la connexion réseau elle-même. Chaîne vide si, en théorie, jamais affectée (ne devrait pas arriver pour une session enregistrée) |
| `last_activity` | entier | Horodatage Unix (secondes) du dernier échange observé sur cette session (hello, commande acquittée, ping/ack, ou stats reçues) |
| `stats` | objet ou `null` | `null` tant qu'aucun `CTRL_GET_STATS` n'a encore abouti pour cette session ; sinon un instantané **mis en cache** (voir ci-dessous) |
| `stats.shots_per_second` / `.possibility_stock` / `.analysed_stock` / `.max_result` / `.pruner_checked` / `.pruner_removed` / `.pruner_cells_per_second` | entier ≥ 0 | Mêmes champs que `control_stats_t` du canal de contrôle (voir [échanges client/serveur](echanges_client_serveur.md#canal-de-contrôle-v9)) — agrégés côté client sur tous ses forks. `pruner_cells_per_second` est le pendant « coups/s » du pruner (débit de cases étudiées/seconde), 0 hors mode pruner |
| `stats.stats_time` | entier | Horodatage Unix (secondes) auquel **cette réponse précise** a été reçue — peut être ancien si le client n'a pas été re-sondé depuis (voir `POST /api/v1/clients/stats` ci-dessous) |

**`stats` est un instantané en cache, pas une lecture en direct.** Une session de
canal de contrôle échange avec le serveur sur son propre thread dédié, indépendant du
thread unique de l'API HTTP (voir [Modèle réseau](#modèle-réseau)) : ce GET ne
déclenche **aucun** aller-retour réseau vers les clients, il relit simplement la
dernière réponse `CTRL_STATS` mise en cache par `control_registry_record_stats`
(`src/app/control_registry.c`). Pour rafraîchir ce cache avant de lire, voir
`POST /api/v1/clients/stats`.

### POST /api/v1/clients/stats

Déclenche une collecte de statistiques auprès de tous les clients connectés —
l'équivalent HTTP de la commande console `clientsStats`.

**Requête :** aucun corps requis.

**Réponse :**

```json
{ "result": "ok", "requested": 3 }
```

| Champ | Type | Sens |
|---|---|---|
| `result` | chaîne | Toujours `"ok"` (code `200`) |
| `requested` | entier ≥ 0 | Nombre de sessions de canal de contrôle actives auxquelles `CTRL_GET_STATS` vient d'être diffusé (`control_registry_broadcast_get_stats`) |

**« Fire-and-forget », par nécessité.** Cette route répond **dès que la demande est
posée** dans la file de chaque session, **sans attendre** les réponses — exactement
comme la commande console `clientsStats`. Ce n'est pas une simplification arbitraire :
le thread HTTP est **unique** et sert les requêtes **séquentiellement** (voir
[Modèle réseau](#modèle-réseau)), alors que chaque réponse `CTRL_STATS` arrive de
façon asynchrone sur le thread de sa propre session ; le faire attendre bloquerait
l'API entière derrière le client le plus lent (voire un client mort qui ne répondra
jamais). En pratique, chaque session se réveille **immédiatement** sur la commande
postée (signal de `pthread_cond_t`, pas d'attente du prochain ping), donc
l'aller-retour est typiquement de l'ordre de la milliseconde à la seconde — mais rien
ne le garantit formellement pour un client lent ou en cours de déconnexion. Séquence
d'usage typique : `POST /api/v1/clients/stats` puis, un court instant après,
`GET /api/v1/clients` pour lire les `stats` rafraîchies.

### GET /api/v1/best-board

Représentation complète (pas seulement le compte) du meilleur plateau connu du
serveur — agrégat `g_server_best_board` ([src/core/best_board.h](../src/core/best_board.h)),
alimenté par sa propre genèse et par les clients connectés (via
`CTRL_GET_BEST_BOARD`/`CTRL_BEST_BOARD` sur le [canal de contrôle](echanges_client_serveur.md#meilleur-plateau-connu-ctrl_get_best_boardctrl_best_board-v10)).
Requête **dédiée**, volontairement absente de `GET /api/v1/stats` : la grille (256
cases) est un ordre de grandeur plus grosse qu'un compteur, un consommateur qui ne
s'intéresse qu'au débit ne doit pas la payer à chaque poll.

**Requête :** aucun corps.

**Réponse (un plateau déjà enregistré) :**

```json
{
  "has_board": true,
  "alloc": 187,
  "min_candidats": 2,
  "grid": [
    [ {"id": 4, "rotation": 1, "top": 0, "right": 19, "bottom": 21, "left": 0}, null, ... ],
    [ null, null, ... ],
    ...
  ]
}
```

**Réponse (aucun record depuis le démarrage/restore) :**

```json
{ "has_board": false }
```

| Champ | Type | Sens |
|---|---|---|
| `has_board` | booléen | `false` si le serveur n'a encore aucun plateau enregistré (juste après démarrage, sans `restore`) — `alloc`/`grid` absents dans ce cas |
| `alloc` | entier | Nombre de pièces posées sur ce plateau (voir [le champ `alloc`](#le-champ-alloc)) — coïncide avec le compte des cases de `grid` qui ne valent pas `null` |
| `min_candidats` | entier | Score MRV de la dernière pièce posée (voir [le champ `min_candidats`](#le-champ-min_candidats)) — `-1` si non mesuré |
| `grid` | tableau 2D | `grid[x][y]` : `null` si la case est vide, sinon la description de la pièce réellement posée — **jamais** l'indice brut interne (`id + ETERN_PARTS*rotation`, cf. `id_for_rotated_part`) |
| `grid[x][y].id` | entier | Identifiant réel de la pièce (celui du fichier `pieces.csv`) |
| `grid[x][y].rotation` | entier (0-3) | Rotation appliquée à la pièce dans cette orientation |
| `grid[x][y].top`/`.right`/`.bottom`/`.left` | entier | Couleurs des 4 bords de la pièce **dans son orientation posée** (motifs à faire correspondre avec les cases voisines) |

La décodage passe par `g_server_rotate_parts` (`src/app/etii_server.c`), la
même table de rotations construite par `runserver` et déjà partagée avec
chaque `client_t.rotate_parts` pour sérialiser les solutions en CSV — pas de
lecture supplémentaire du fichier de pièces. Si cette table n'est pas encore
disponible (fenêtre très étroite entre le tout début de `runserver` et la fin
de la construction), la réponse retombe sur `id`/`rotation` seuls (décodés de
l'indice brut) avec les 4 couleurs à `-1`, plutôt que de planter ou de bloquer.

Cette lecture est **synchrone et locale** : comme `GET /api/v1/stats`, elle ne
déclenche aucun aller-retour réseau vers les clients, elle relit l'agrégat déjà
maintenu par le serveur.

### GET /api/v1/known-clients

*(v12, PR4)* Instantané des machines **connues** — l'équivalent HTTP de la commande console
`knownClients` (voir [Registre de clients connus](echanges_client_serveur.md#registre-de-clients-connus)).
Distinct de `GET /api/v1/clients` : cette liste inclut aussi les machines
déconnectées depuis le démarrage du serveur (jusqu'à éviction par la borne du
registre) et cumule, par `machine_uid`, plusieurs exécutions successives d'un
même client.

```json
{
  "known_clients": [
    {
      "machine_uid": "0102030405060708090a0b0c0d0e0f10",
      "label": "jetson-1",
      "ip": "192.168.1.42",
      "mode": "search",
      "connected": true,
      "active_sessions": 1,
      "connections_total": 3,
      "first_seen": 1729990000,
      "last_seen": 1730000000,
      "total_pruner_checked": 0,
      "total_pruner_removed": 0,
      "best_max_result": 210,
      "cumulative_uptime_seconds": 9000
    }
  ]
}
```

| Champ | Type | Sens |
|---|---|---|
| `known_clients` | tableau | Une entrée par machine connue (`known_clients_registry_snapshot`) — **tableau vide** si aucune machine n'a encore été vue, jamais une erreur |
| `machine_uid` | chaîne hexadécimale | Nonce machine persistant (`net/client_identity.h`) — clé de cumul de cette entrée, la SEULE identité qui survit à un redémarrage de processus client |
| `label` / `ip` / `mode` | — | Dernières valeurs déclarées/observées pour cette machine (mêmes conventions que `GET /api/v1/clients` : `label` échappé côté serveur, `ip` non falsifiable) |
| `connected` | booléen | `true` si au moins une session de cette machine est actuellement active (`active_sessions > 0`) |
| `active_sessions` | entier ≥ 0 | Nombre de sessions **actuellement** actives pour cette machine — peut dépasser 1 (ex. un client de recherche et un pruner lancés en parallèle sur le même hôte) |
| `connections_total` | entier | Nombre total de connexions (hellos de contrôle) observées pour cette machine depuis le démarrage du serveur |
| `first_seen` / `last_seen` | entier | Horodatages Unix (secondes) de la première connexion et de la dernière activité observées |
| `total_pruner_checked` / `total_pruner_removed` | entier ≥ 0 | Cumul, sur toutes les sessions passées **et** en cours de cette machine, des possibilités vérifiées/éliminées par le pruner. Calculé par **accroissement** observé à chaque `CTRL_STATS` (jamais par simple somme des valeurs instantanées) : un client qui redémarre voit son compteur par-processus repartir de 0, mais ce total continue de croître dessus au lieu d'être écrasé |
| `best_max_result` | entier | Meilleur nombre de pièces posées (voir [le champ `alloc`](#le-champ-alloc)) jamais rapporté par cette machine, toutes sessions confondues — un **pic**, jamais remplacé par une valeur plus basse |
| `cumulative_uptime_seconds` | entier ≥ 0 | Somme des durées de connexion des sessions déjà **terminées** de cette machine — n'inclut pas la durée de la session en cours tant qu'elle n'est pas close |

**Cumul persisté** depuis PR5
(`./eternityII-known_clients.back`, voir
[Registre de clients connus](echanges_client_serveur.md#registre-de-clients-connus))
: un redémarrage du serveur ne remet plus ce registre à zéro, sous réserve
d'avoir exécuté la commande console `restore` (aucun chargement automatique
au démarrage). Comme `GET /api/v1/clients`, cette lecture est **synchrone et
locale**, sans aucun aller-retour réseau vers les clients.

### GET /api/v1/stock-distribution

Répartition du stock par nombre de pièces posées (`alloc`, voir [le champ `alloc`](#le-champ-alloc)) — l'équivalent
HTTP de la commande console `statistic`, qui elle ne fait qu'imprimer cet
histogramme dans les journaux du serveur (voir [console](console.md)). Répond à
la question « à quelle profondeur en est l'exploration ? », là où
`GET /api/v1/stats` ne donne que des totaux et `GET /api/v1/best-board` que le
meilleur plateau atteint.

```json
{
  "total_unchecked": 5312,
  "total_checked": 40,
  "total_analysed": 12,
  "levels": [
    { "alloc": 3, "unchecked": 12,   "checked": 0,  "analysed": 0,  "avg_min_candidats": 2.50 },
    { "alloc": 4, "unchecked": 5300, "checked": 40, "analysed": 12, "avg_min_candidats": 0.0  }
  ]
}
```

| Champ | Type | Sens |
|---|---|---|
| `total_unchecked` | entier ≥ 0 | Total du pool **non vérifié** — même valeur que `possibility_stock` de `GET /api/v1/stats` |
| `total_checked` | entier ≥ 0 | Total du pool **vérifié** (`checked_stock` de `/stats`) |
| `total_analysed` | entier ≥ 0 | Total du pool **en cours d'analyse** (`analysed_stock` de `/stats`) |
| `levels` | tableau | Un objet par niveau `alloc` **non vide**, trié par `alloc` croissant |
| `levels[].alloc` | entier | Nombre de pièces posées de ces possibilités (voir [le champ `alloc`](#le-champ-alloc)), 0 à 256 (ou 0 à 16 en build `ETERN_PARTS=16`) |
| `levels[].unchecked` / `checked` / `analysed` | entier ≥ 0 | Nombre de possibilités de ce niveau dans chacun des trois pools |
| `levels[].avg_min_candidats` | flottant | Seconde coordonnée (voir [le champ `min_candidats`](#le-champ-min_candidats)) : moyenne des scores MRV connus à ce niveau, trois pools combinés. `0.0` si aucune mesure disponible pour ce niveau (niveau 4 de l'exemple ci-dessus) — jamais une vraie moyenne, `0` candidat n'étant jamais stocké (sous-arbre mort) |

**Les niveaux entièrement vides sont omis.** Sur les 257 niveaux possibles, un
serveur réel n'en occupe qu'une poignée : les émettre tous n'apporterait que des
zéros et ferait frôler la borne de réponse de 32 Ko. Un stock entièrement vide
donne donc `"levels": []`, jamais une erreur — les trois totaux, eux, sont
toujours présents. Un client ne doit donc **pas** supposer que l'index d'une
entrée dans `levels` vaut son `alloc` : lire le champ `alloc`.

Contrairement à `GET /api/v1/stats` (simple lecture de compteurs déjà tenus à
jour), cette route **parcourt toutes les files sous verrou** — c'est précisément
pourquoi elle est une requête dédiée et non des champs supplémentaires de
`/stats` : un consommateur qui ne poll que le débit ne doit pas payer ce
parcours. À réserver à un usage occasionnel, pas à une boucle de polling serrée.

L'instantané n'est **pas atomique entre les pools** : les deux pools de stock
sont lus sous une famille de verrous, le pool analysé sous une autre, jamais les
deux en même temps (discipline de verrouillage de `datamanager.c`). Une
possibilité servie à un client pile entre les deux passes peut donc être comptée
deux fois ou pas du tout. C'est une donnée d'observation, pas une comptabilité.

## Séquences typiques

### Polling de télémétrie

```mermaid
sequenceDiagram
    participant App as Application tierce
    participant S as Serveur (API HTTP)
    loop toutes les N secondes
        App->>S: GET /api/v1/stats
        S-->>App: 200 + JSON stats
        Note over App: nouvelle connexion à chaque appel<br/>(Connection: close)
    end
```

### Pause / reprise administrative

`pause`/`resume` sont des commandes de MODIFICATION : elles exigent le jeton Bearer
(serveur lancé avec `--http-token-file`), exactement comme `restore`/`backup`.

```mermaid
sequenceDiagram
    participant App as Application tierce
    participant S as Serveur (--http-token-file)
    App->>S: POST /api/v1/command {"command":"pause"}<br/>Authorization: Bearer <jeton>
    S-->>App: 200 {"result":"ok"}
    App->>S: GET /api/v1/status
    S-->>App: 200 {"state":"admin_pause", ...}
    Note over App: … plus tard …
    App->>S: POST /api/v1/command {"command":"resume"}<br/>Authorization: Bearer <jeton>
    S-->>App: 200 {"result":"ok"}
```

### Commande refusée

```mermaid
sequenceDiagram
    participant App as Application tierce
    participant S as Serveur
    App->>S: POST /api/v1/command {"command":"exit"}
    S-->>App: 403 {"error":"command not allowed"}
    Note over S: le serveur continue de tourner —<br/>la liste blanche est vérifiée AVANT toute exécution
```

### Commande de modification (restore, ou pause/limit/…) avec/sans jeton

```mermaid
sequenceDiagram
    participant App as Application tierce
    participant S as Serveur (--http-token-file)
    App->>S: POST /api/v1/command {"command":"restore"}
    Note over S: pas d'en-tête Authorization
    S-->>App: 401 {"error":"unauthorized"}<br/>WWW-Authenticate: Bearer
    App->>S: POST /api/v1/command {"command":"restore"}<br/>Authorization: Bearer <jeton>
    Note over S: jeton comparé en temps constant<br/>au jeton chargé au démarrage
    S-->>App: 200 {"result":"ok"}
```

Le même échange s'applique à `pause`, `resume`, `limit`, `maxStockByThread`,
`prunerBatch`, `prunerDfsBudget`, `clientsCommand`/`clientsCmd` — pas seulement `restore`/`backup`.
`clientsWork` est la seule commande de `POST /api/v1/command` qui ne suit jamais ce
schéma : elle répond directement `200`, sans en-tête `Authorization`.

### Détail par client (`clients` / `clientsStats`)

```mermaid
sequenceDiagram
    participant App as Application tierce
    participant S as Serveur (thread HTTP)
    participant R as control_registry (cache)
    participant C as Client (session de contrôle)

    App->>S: GET /api/v1/clients
    S->>R: control_registry_snapshot()
    R-->>S: pid/forks/mode/ip/last_activity + stats en cache (peut être null)
    S-->>App: 200 {"clients":[...]}

    App->>S: POST /api/v1/clients/stats
    S->>R: control_registry_broadcast_get_stats()
    R-->>S: N sessions sollicitées
    S-->>App: 200 {"result":"ok","requested":N}
    Note over R,C: en parallèle, sur le thread de CHAQUE session :<br/>CTRL_GET_STATS -> CTRL_STATS -> control_registry_record_stats()

    Note over App: … court instant plus tard …
    App->>S: GET /api/v1/clients
    S->>R: control_registry_snapshot()
    R-->>S: stats désormais rafraîchies
    S-->>App: 200 {"clients":[{ "stats": {...} }, ...]}
```

## Exemples d'implémentation client

### curl

```sh
curl http://127.0.0.1:8080/api/v1/stats
curl http://127.0.0.1:8080/api/v1/status
curl http://127.0.0.1:8080/api/v1/clients
curl http://127.0.0.1:8080/api/v1/stock-distribution      # répartition du stock par alloc (usage occasionnel)
curl -X POST http://127.0.0.1:8080/api/v1/clients/stats   # puis relire /clients pour les stats rafraîchies
curl -X POST -d '{"command":"clientsWork beta"}' http://127.0.0.1:8080/api/v1/command   # lecture seule -> {"result":"ok"}, résultat dans les journaux serveur

# Commandes de modification (serveur lancé avec --http-token-file) : toutes exigent le jeton.
curl -X POST -d '{"command":"limit 1000"}' http://127.0.0.1:8080/api/v1/command       # -> 401, pas de jeton
curl -X POST -H "Authorization: Bearer un-secret-suffisamment-long" \
     -d '{"command":"limit 1000"}' http://127.0.0.1:8080/api/v1/command               # -> 200
curl -X POST -H "Authorization: Bearer un-secret-suffisamment-long" \
     -d '{"command":"clientsCommand --to beta limit 500"}' http://127.0.0.1:8080/api/v1/command   # une seule session ciblée
curl -X POST -H "Authorization: Bearer un-secret-suffisamment-long" \
     -d '{"command":"backup"}' http://127.0.0.1:8080/api/v1/command                   # -> 200
```

### Python (bibliothèque standard, sans dépendance)

```python
import json
import time
import urllib.request

BASE = "http://127.0.0.1:8080/api/v1"

def get_stats():
    with urllib.request.urlopen(f"{BASE}/stats", timeout=5) as r:
        return json.load(r)

def send_command(command, token=None):
    body = json.dumps({"command": command}).encode()
    headers = {"Authorization": f"Bearer {token}"} if token else {}
    req = urllib.request.Request(f"{BASE}/command", data=body, method="POST", headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status, json.load(r)
    except urllib.error.HTTPError as e:
        return e.code, json.load(e)

def get_clients():
    with urllib.request.urlopen(f"{BASE}/clients", timeout=5) as r:
        return json.load(r)["clients"]

def refresh_clients_stats():
    req = urllib.request.Request(f"{BASE}/clients/stats", data=b"", method="POST")
    with urllib.request.urlopen(req, timeout=5) as r:
        return json.load(r)

print(get_stats())
# "pause" est une commande de MODIFICATION : jeton requis (serveur lancé avec
# --http-token-file), sinon 401.
print(send_command("pause", token="un-secret-suffisamment-long"))

refresh_clients_stats()
time.sleep(0.5)  # laisse le temps aux sessions de répondre (typiquement sub-seconde)
print(get_clients())
```

### Notes générales pour tout langage

- Utiliser une bibliothèque HTTP/1.1 standard : rien de spécifique au protocole n'est
  requis côté client, c'est du JSON sur HTTP classique.
- Ne pas réutiliser une connexion entre deux appels (`Connection: close` — la plupart
  des clients HTTP le gèrent automatiquement et rouvrent une connexion).
- Traiter tout code hors `200` comme un échec métier (403/404/405) ou de forme
  (400/413), et lire le corps JSON `{"error": "..."}` pour le diagnostic — sans faire
  de correspondance exacte de message (non contractuel, voir plus haut).
- Poser un timeout côté client (le serveur en pose un de 5 s de son côté, mais un
  client sans timeout propre resterait bloqué en cas de coupure réseau asymétrique).

## Compatibilité

Cette API est un **port TCP indépendant** de celui du protocole binaire de travail
(`SERVER_PORT`, 2020 par défaut) et du canal de contrôle (multiplexé sur ce même
port). Elle n'a **aucun impact** sur le handshake de version `VERSION` du protocole
binaire ([échanges client/serveur](echanges_client_serveur.md#handshake-de-version)) :
activer `--http-port` ne change rien pour les clients/pruners eternityII existants, et
inversement, aucun client/pruner n'a besoin d'être mis à jour pour qu'un serveur
commence à exposer cette API.

Le champ `version` de `GET /api/v1/status` est purement informatif — cette API HTTP
elle-même n'est pas encore versionnée (`/api/v1/...` réserve la place pour une
évolution future si le besoin apparaît, mais aucun `/api/v2/` n'existe à ce jour).

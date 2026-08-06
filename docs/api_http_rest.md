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
- [src/net/control_protocol.c](../src/net/control_protocol.c) (`control_command_allowed`, `control_command_privileged`) — listes blanches des commandes ; `control_command_allowed` est **partagée** avec le canal de contrôle binaire, `control_command_privileged` (restore/backup) ne l'est **pas** (accessible uniquement via cette API, jamais via le canal de contrôle) ;
- [src/app/control_registry.h](../src/app/control_registry.h) / [control_registry.c](../src/app/control_registry.c) (`control_registry_snapshot`, `control_registry_record_stats`, `control_registry_broadcast_get_stats`) — registre des sessions de [canal de contrôle](echanges_client_serveur.md#canal-de-contrôle-v9), source de `GET /api/v1/clients` et `POST /api/v1/clients/stats` ;
- [src/core/best_board.h](../src/core/best_board.h) / [best_board.c](../src/core/best_board.c) (`g_server_best_board`) — représentation du meilleur plateau connu, source de `GET /api/v1/best-board`.

## Activation

Désactivée par défaut. À activer explicitement au démarrage du serveur :

```sh
./eternityII server 4 --http-port 8080 data/pieces.csv
```

`--http-port` est une option position-indépendante (comme `--expand-level`), retirée
d'`argv` avant l'analyse positionnelle des arguments du mode. Une valeur absente ou
hors de l'intervalle `[1, 65535]` est **ignorée silencieusement** (l'API reste
désactivée) plutôt que d'ouvrir un port au hasard.

Optionnellement, `--http-token-file <chemin>` active l'authentification des deux
commandes privilégiées `restore`/`backup` (voir [Authentification](#authentification-restorebackup)
ci-dessous) :

```sh
chmod 600 /etc/eternityii/http-token
./eternityII server 4 --http-port 8080 --http-token-file /etc/eternityii/http-token data/pieces.csv
```

## Posture de sécurité

- **Écoute en boucle locale uniquement** (`127.0.0.1`, jamais `0.0.0.0`/`INADDR_ANY`) :
  l'API n'est **jamais** exposée hors de la machine par défaut. Un accès distant passe
  par un tunnel SSH ou un reverse-proxy explicite, à la charge de l'opérateur.
- **Authentification partielle, par défaut absente.** Sans `--http-token-file`, l'API
  se comporte exactement comme avant cette fonctionnalité : aucune authentification,
  pensée pour un réseau de confiance (la machine elle-même, ou un tunnel authentifié
  en amont) — ne jamais l'exposer directement sur un réseau non maîtrisé. Avec
  `--http-token-file`, un jeton Bearer devient nécessaire pour les deux seules
  commandes capables de remplacer l'état du serveur (`restore`, `backup`) ; toutes les
  autres routes et commandes restent, elles, sans authentification (voir
  [Authentification](#authentification-restorebackup)).
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
| `401` | Unauthorized | `POST /api/v1/command` avec une commande **privilégiée** (`restore`, `backup`) sans jeton Bearer valide (absent, invalide, ou aucun jeton configuré côté serveur) — porte l'en-tête `WWW-Authenticate: Bearer` |
| `403` | Forbidden | Commande reconnue mais hors des deux listes blanches (ex. `exit`) |
| `404` | Not Found | Chemin inconnu |
| `405` | Method Not Allowed | Chemin connu, mauvaise méthode HTTP (ex. `POST /api/v1/stats`) |
| `413` | Payload Too Large | Requête (en-têtes + corps) dépassant 8 Ko |

Les corps d'erreur ont la forme `{"error":"<message>"}` ; un succès de commande
renvoie `{"result":"ok"}`. Les messages d'erreur sont informatifs mais **non
contractuels** (ne pas faire de correspondance exacte de chaîne côté client — se fier
au code de statut HTTP).

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
  "queues": [
    { "file": 0, "unchecked": 10, "checked": 2, "analysed": 1 },
    { "file": 1, "unchecked": 8,  "checked": 0, "analysed": 0 }
  ]
}
```

| Champ | Type | Sens |
|---|---|---|
| `shots_per_second` | entier ≥ 0 | Débit de recherche courant (essais/seconde), même valeur que le bandeau `coups/s` de la console — publié toutes les 10 s, donc granularité de mise à jour de cet ordre |
| `possibility_stock` | entier ≥ 0 | Total des possibilités **non vérifiées** en stock (somme des 10 files) |
| `checked_stock` | entier ≥ 0 | Total des possibilités **vérifiées** en attente de service |
| `analysed_stock` | entier ≥ 0 | Total des possibilités dans le pool **en cours d'analyse** (distribuées aux pruners, pas encore acquittées) |
| `max_result` | entier ≥ 0 | Meilleur résultat atteint (nombre de cases placées), 0 à 256 (ou 0 à 16 en build `ETERN_PARTS=16`) |
| `active_threads` | entier ≥ 0 | Nombre de connexions clients actuellement servies (canal de travail **et** de contrôle confondus, cf. [dimensionnement](echanges_client_serveur.md#impact-sur-le-dimensionnement-du-serveur)) |
| `pruner_checked` / `pruner_removed` | entier ≥ 0 | Toujours `0` côté serveur (ces compteurs n'existent que côté processus pruner ; conservés dans le schéma pour rester alignable avec `control_stats_t` du canal de contrôle) |
| `queues` | tableau de 10 objets | Une entrée par file interne (`NB_FILE_POSSIBILITY`), avec ses trois compteurs par pool. L'ordre des entrées suit l'index de file (0 à 9), pas garanti trié par une autre clé |

### GET /api/v1/status

Instantané de l'état et de la configuration courante.

```json
{
  "state": "admin_pause",
  "uptime_seconds": 3600,
  "version": 9,
  "limit": 1000,
  "max_stock_by_thread": 500,
  "pruner_batch": 64
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

| Commande | Effet |
|---|---|
| `pause` | Pose une pause administrative (`state` devient `admin_pause`) |
| `resume` | Lève une pause administrative posée par `pause` |
| `limit <n>` | Fixe le débit maximum de recherche à `n` essais/seconde (`0` = illimité) |
| `maxStockByThread <n>` | Fixe le seuil de stock local par thread |
| `prunerBatch <n>` | Fixe la taille de lot du pruner, bornée à `[1, PRUNER_BATCH_MAX]` (65536) — une valeur hors borne est silencieusement ramenée à la borne la plus proche, pas un `400` |

**Commandes privilégiées** (`control_command_privileged`), **UNIQUEMENT** accessibles
via cette route, et seulement avec un jeton Bearer valide (voir
[Authentification](#authentification-restorebackup)) :

| Commande | Effet |
|---|---|
| `restore [fichier [fichier_analyse]]` | Remplace le stock courant par le contenu des fichiers `.back` indiqués (défaut : `./eternityII.back` / `./eternityII-in_analyse.back`) |
| `backup` | Sauvegarde les files courantes dans les fichiers `.back` |

Toute autre commande — en particulier `exit`, `import` — est **rejetée avant même
d'être tokenisée**, avec `403`, jeton ou non.

**Réponses :**

| Cas | Code | Corps |
|---|---|---|
| Commande whitelistée (standard ou privilégiée authentifiée), appliquée | `200` | `{"result":"ok"}` |
| Commande privilégiée sans jeton valide (absent, invalide, ou aucun jeton configuré) | `401` | `{"error":"unauthorized"}` (en-tête `WWW-Authenticate: Bearer`) |
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

### Authentification (restore/backup)

Par défaut (sans `--http-token-file`), `restore` et `backup` sont **inaccessibles**
via cette API : `control_command_privileged` les identifie comme privilégiées, et sans
jeton configuré côté serveur (`HTTP_ADMIN_TOKEN` vide), `http_command_authorize`
répond toujours `401` — jamais d'exécution sans preuve d'identité, même en l'absence
de configuration.

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

**Utilisation :** ajouter l'en-tête `Authorization: Bearer <jeton>` à la requête
`POST /api/v1/command` :

```sh
curl -X POST \
     -H "Authorization: Bearer un-secret-suffisamment-long" \
     -d '{"command":"restore"}' \
     http://127.0.0.1:8080/api/v1/command
```

**Ce que l'authentification ne change PAS :**

- Les commandes standard (`pause`, `resume`, `limit`, `maxStockByThread`,
  `prunerBatch`) restent accessibles **sans** authentification, jeton configuré ou
  non — comportement strictement inchangé.
- Les routes `GET` (`stats`, `status`, `clients`, `best-board`) ne demandent jamais de
  jeton.
- Le [canal de contrôle binaire](echanges_client_serveur.md#canal-de-contrôle-v9)
  (`CTRL_COMMAND`) reste borné à `control_command_allowed` des deux côtés : ajouter
  `restore`/`backup` à cette API ne les rend **pas** déclenchables à distance sur un
  client via le canal de contrôle.

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
      "pid": 4242,
      "forks": 4,
      "mode": "search",
      "ip": "192.168.1.42",
      "last_activity": 1730000000,
      "stats": null
    },
    {
      "pid": 5555,
      "forks": 1,
      "mode": "pruner",
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
| `pid` | entier | PID du processus **parent** du client (jamais un fork de recherche, cf. canal de contrôle) |
| `forks` | entier ≥ 0 | Nombre de processus de recherche forkés par ce client |
| `mode` | chaîne | `search` (client de recherche), `pruner` (élagage CPU), `gpu_pruner` (élagage GPU), ou `unknown` (valeur de repli, ne devrait pas apparaître en usage normal) |
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
| `alloc` | entier | Nombre de pièces placées de ce plateau |
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

```mermaid
sequenceDiagram
    participant App as Application tierce
    participant S as Serveur
    App->>S: POST /api/v1/command {"command":"pause"}
    S-->>App: 200 {"result":"ok"}
    App->>S: GET /api/v1/status
    S-->>App: 200 {"state":"admin_pause", ...}
    Note over App: … plus tard …
    App->>S: POST /api/v1/command {"command":"resume"}
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

### Commande privilégiée (restore) avec/sans jeton

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
curl -X POST -d '{"command":"limit 1000"}' http://127.0.0.1:8080/api/v1/command
curl http://127.0.0.1:8080/api/v1/clients
curl -X POST http://127.0.0.1:8080/api/v1/clients/stats   # puis relire /clients pour les stats rafraîchies

# Commandes privilégiées (serveur lancé avec --http-token-file) :
curl -X POST -d '{"command":"restore"}' http://127.0.0.1:8080/api/v1/command        # -> 401, pas de jeton
curl -X POST -H "Authorization: Bearer un-secret-suffisamment-long" \
     -d '{"command":"backup"}' http://127.0.0.1:8080/api/v1/command                 # -> 200
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
print(send_command("pause"))

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

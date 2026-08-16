# Utilisation

Ce document détaille les modes d'exécution du binaire `eternityII`, leurs paramètres,
les fichiers manipulés (pièces, sauvegardes, solutions) et les limitations connues.
Les commandes interactives disponibles une fois le programme lancé sont décrites dans
[Console interactive](console.md).

## Aide en ligne de commande

Le binaire embarque sa propre aide, sans avoir à consulter cette documentation :

```sh
./eternityII --help            # aide générale : usage, modes, options (alias : -h)
./eternityII help              # équivalent de --help
./eternityII help server    # détail d'un mode
./eternityII help http-port    # détail d'une option (tirets de tête facultatifs)
```

`--help`/`-h` sont acceptées à n'importe quelle position, par tous les modes :
l'aide est affichée puis le programme sort en succès, avant toute initialisation.
Les noms de sujets sont insensibles à la casse ; un sujet inconnu affiche une
erreur puis l'aide générale (sortie en échec). Des arguments invalides au
lancement affichent la même aide générale sur la sortie d'erreur.

## Mode serveur

Lance le serveur qui distribue les possibilités aux clients.

```sh
./eternityII server [nb_threads] [--expand-level N] [--expand-max-stock N] [--expand-max-levels N] [--stock-files N] [--rebalance-budget N] [--http-port N] [--http-token-file CHEMIN] [fichier_pieces.csv]
```

| Paramètre | Défaut | Description |
|---|---|---|
| `nb_threads` | 80 | Nombre de connexions clients simultanées |
| `--expand-level N` | *(absent)* | Développe le stock au démarrage jusqu'au niveau de curseur `N` (anti-famine, voir ci-dessous) |
| `--expand-max-stock N` | `EXPAND_MAX_STOCK` (100000) | Plafonne en NOMBRE de possibilités la pré-expansion `--expand-level` (voir ci-dessous) ; sans effet si `--expand-level` est absent |
| `--expand-max-levels N` | `EXPAND_MAX_LEVELS` (4) | Plafonne en NOMBRE DE PASSES la pré-expansion `--expand-level` (voir ci-dessous) ; sans effet si `--expand-level` est absent |
| `--stock-files N` | `NB_FILE_POSSIBILITY_DEFAULT` (10) | Nombre de files de stock, fixé une seule fois au démarrage (jamais à chaud), plafonné à `NB_FILE_POSSIBILITY_MAX` (128) — voir ci-dessous |
| `--rebalance-budget N` | `REBALANCE_BUDGET_DEFAULT` (1000) | Nombre de possibilités rééquilibrées entre files à chaque tour serveur (10 s) — voir ci-dessous |
| `--http-port N` | *(absent)* | Active l'[API HTTP REST admin](api_http_rest.md) sur `127.0.0.1:N` (désactivée par défaut) |
| `--http-token-file CHEMIN` | *(absent)* | Jeton Bearer requis pour toute commande de MODIFICATION de l'[API HTTP](api_http_rest.md#authentification) (`pause`, `resume`, `limit`, `maxStockByThread`, `prunerBatch`, `clientsCommand`/`clientsCmd`, `restore`, `backup`) — sans cette option, ces commandes restent inaccessibles via l'API (seule `clientsWork`, en lecture seule, reste utilisable) |
| `fichier_pieces.csv` | `data/pieces.csv` | Fichier de définition des pièces |

Exemples :
```sh
./eternityII server 80
./eternityII server 80 data/pieces.csv
./eternityII server 80 --expand-level 4 data/pieces.csv
./eternityII server 80 --expand-level 4 --expand-max-stock 1000000 data/pieces.csv
./eternityII server 80 --expand-level 8 --expand-max-stock 1000000 --expand-max-levels 8 data/pieces.csv
./eternityII server 80 --stock-files 32 --rebalance-budget 5000 data/pieces.csv
./eternityII server 80 --http-port 8080 data/pieces.csv
./eternityII server 80 --http-port 8080 --http-token-file /etc/eternityii/http-token data/pieces.csv
```

### Maîtrise de la charge serveur (`--stock-files`, `--rebalance-budget`, `--tcp-timeout`)

Sous forte charge (gros stock, sauvegarde volumineuse), une sauvegarde qui immobilise
longtemps les verrous du stock peut affamer les clients connectés jusqu'à leur timeout TCP.
La sauvegarde automatique gèle toutes les files des deux pools (stock et analysé) à un
instant T unique, puis les écrit et les libère progressivement, une file à la fois — la
fenêtre de blocage total vaut donc le temps d'écriture d'**une seule** file, pas de la
sauvegarde entière. `--stock-files` augmente le nombre de files pour réduire ce temps
d'écriture par file ; `--rebalance-budget` règle la vitesse à laquelle le stock est
rééquilibré entre ces files (file la plus pleine → la plus vide, borné en temps, disponible
aussi via la commande console `rebalance [n]` — voir [Console interactive](console.md)), ce
qui maintient des files de taille comparable et rend le gain de `--stock-files` effectif ;
`--tcp-timeout` (ci-dessous) élargit en plus la marge côté réseau. Chaque artefact sauvegardé
(stock, pool analysé, meilleur plateau connu, registre des clients connus) n'est réécrit que
si son propre contenu a changé depuis sa dernière écriture — la durée de la dernière
sauvegarde effectivement exécutée est exposée par `GET /api/v1/status`
(`last_backup_duration_ms`, voir [API HTTP REST admin](api_http_rest.md)).

> ⚠️ **Dimensionnement de `nb_threads`** : chaque processus client connecté ouvre,
> en plus des connexions de travail de ses forks, une connexion de
> [canal de contrôle](echanges_client_serveur.md#canal-de-contrôle-v9) qui occupe un
> slot du **même** pool. Un serveur dimensionné au plus juste doit compter
> (connexions de travail simultanées) **+** (processus clients connectés), pas
> seulement le premier terme. Le défaut (80) laisse une large marge.

### Expansion du stock au démarrage (`--expand-level`, anti-famine)

Au démarrage, le serveur ne détient que le paquet *genèse* et ses tout premiers
enfants. Le premier client qui se connecte récupère cet unique arbre et le garde en
local ; le serveur se retrouve sans rien à distribuer aux autres clients, qui tournent
à vide — c'est la **famine du démarrage**.

L'option `--expand-level N` (position-indépendante, retirée d'argv avant l'analyse
positionnelle) demande au serveur de **développer lui-même son stock** avant toute
connexion : il place une pièce candidate sur la case suivante de chaque possibilité
jusqu'à ce que leur curseur `alloc` atteigne le niveau `N`. Le paquet genèse devient
ainsi des milliers de possibilités distribuables. C'est un calcul **purement serveur,
sans aucun impact client**.

L'expansion est bornée sur deux axes, tous deux configurables au lancement (dans
[src/app/static_variables.h](../src/app/static_variables.h)) : `--expand-max-levels N`
(défaut `EXPAND_MAX_LEVELS`, 4) plafonne le nombre de passes quelle que soit la
consigne — garde-fou en *profondeur* — et `--expand-max-stock N` (défaut
`EXPAND_MAX_STOCK`, 100000) plafonne le *nombre* de possibilités entre passes ; comme le
facteur de branchement est inconnu et qu'une seule passe peut exploser, ce plafond en
nombre est le vrai garde-fou de temps et de mémoire. Sur le puzzle 256 le branchement
mesuré est ≈11×/niveau (niveau 3 → ~500 possibilités, niveau 4 → ~5300, niveau 5 →
~56000) : **le niveau 3–4 est le point idéal** — de quoi remplir le stock local de tous
les clients avec réserve, en bien moins d'une seconde. Les deux options sont
volontairement configurables — un serveur disposant de plus de capacité (RAM, CPU) peut
relever `--expand-max-stock` (~54 Mo à 100000, la mémoire consacrée à la réserve) et/ou
`--expand-max-levels` (le temps qu'il s'autorise à passer sur cette pré-expansion) pour
atteindre un `--expand-level` élevé sans être arrêté prématurément (`N ≤ 0` est ignoré
pour les deux, la valeur par défaut ou déjà fixée est conservée). La même opération
d'expansion est disponible à chaud via la commande interactive `expand N` (utile si le
stock distribuable se raréfie en cours de recherche) ; elle respecte elle aussi les deux
plafonds en vigueur.

> Cette expansion est le pendant *serveur* de la délégation anticipée côté *client*
> (sonde de faim `INST_NEED_WORK`, VERSION 8) décrite dans
> [Échanges client / serveur](echanges_client_serveur.md).

## Mode client

Se connecte à un serveur pour y lancer `N` processus de recherche en
parallèle — **le fork de ces process est différé**, piloté par un
orchestrateur d'état (`WAITING_CONFIG`/`COUNTDOWN`/`CONFIGURING`/`RUNNING`) :
si un fichier de configuration existe (défaut `./eternityii-client.conf`,
option `--config-file`), un décompte de 5 s démarre automatiquement les fils
de recherche ; sinon le process reste en attente d'une commande console
`start` (fork immédiat) ou `config <clé> <valeur>` (prépare une configuration
et annule le décompte). Voir la commande console
[`config`/`start`](console.md#général) pour le détail de l'orchestrateur. Les
paramètres positionnels ci-dessous restent ceux consultés au moment du fork
effectif (qu'il soit automatique ou déclenché par `start`).

```sh
./eternityII client [--name LABEL] [--machine-uid-file CHEMIN] [--config-file CHEMIN] [serveur] [nb_threads] [max_stock_par_thread] [fichier_pieces.csv]
```

| Paramètre | Défaut | Description |
|---|---|---|
| `serveur` | `localhost` | Adresse IP ou nom d'hôte du serveur |
| `nb_threads` | 1 | Nombre de processus de recherche à forker |
| `max_stock_par_thread` | 300 | Nombre max de possibilités stockées par thread avant d'en renvoyer au serveur |
| `fichier_pieces.csv` | `data/pieces.csv` | Fichier de définition des pièces |
| `--name LABEL` | nom d'hôte | Libellé déclaré, affiché côté serveur (commande console `clients`, `GET /api/v1/clients`) — purement déclaratif, jamais vérifié |
| `--machine-uid-file CHEMIN` | `./eternityii-machine_uid` | Fichier d'identité machine persistante (nonce hexadécimal, tiré et écrit au premier lancement) — absent/illisible : régénéré silencieusement ; répertoire non inscriptible : identité volatile pour cette exécution (la recherche continue) |
| `--config-file CHEMIN` | `./eternityii-client.conf` | Fichier de configuration `clé = valeur` : présent au démarrage → décompte d'auto-démarrage de 5 s (`COUNTDOWN`) ; absent → attente d'un `start`/`config` en console (`WAITING_CONFIG`). Priorité CLI > fichier > défauts. Voir la commande console `config`/`configSave` |

> En conteneur, monter ce fichier en volume (ou pointer `--machine-uid-file` dessus) :
> sans ça, chaque redémarrage de conteneur régénère un `machine_uid` et fragmente le
> cumul de statistiques par machine (`knownClients`) sur autant d'entrées « nouvelles ».

Exemples :
```sh
./eternityII client localhost
./eternityII client 192.168.1.10 8
./eternityII client localhost 4 300 data/pieces.csv
./eternityII client --name jetson-1 localhost 8
```

> `--name`/`--machine-uid-file`/`--config-file` s'appliquent aussi au mode `pruner`
> ci-dessous (même plomberie d'identité et de configuration — `pruner` partage
> `handle_client` avec `client`, donc le même orchestrateur de démarrage différé).
> Trois notions distinctes, à ne pas confondre : `machine_uid`
> (persistant, survit aux redémarrages — clé de cumul des statistiques),
> `client_uid` (nonce tiré à chaque démarrage du processus parent, jamais persisté —
> identité de LA SESSION en cours), et `fork_seq` (rang du fork dans son parent,
> `0..N-1` — rattache une connexion de travail à son processus parent). Le `label`
> (`--name`) n'est qu'un affichage, jamais une clé : deux clients peuvent
> légitimement partager le même.

## Mode pruner (élagage)

Un **pruner** réutilise la même plomberie réseau qu'un client, mais au lieu d'explorer
il demande au serveur des possibilités *à vérifier* et élague celles qui n'ont aucune
continuation possible. Deux variantes :

**Sans aucun pruner en service, un serveur accumule du travail déjà mort.** Mesuré sur
un stock réel (`tests/bench/bench_refutation.c --pruner-profile`, voir
[docs/tests_et_ci.md](tests_et_ci.md#mode---pruner-profile--rejoue-le-vrai-pipeline-du-pruner)) :
le seul contrôle superficiel qu'un pruner exécute (`possibility_all_has_a_next_counted`,
gratuit — un pruner ne fait rien de plus cher tant que `prunerDfsBudget` n'est pas réglé)
rejette déjà **50,2 %** d'un stock produit par un client à ordre fixe (16,3 % sur un stock
produit par un client MRV, dont les possibilités sont en moyenne plus avancées avant
délégation). Faire tourner au moins un pruner, même en CPU et avec un seul thread, réduit
donc le stock distribué de façon substantielle et gratuite — indépendamment de tout autre
réglage.

```sh
./eternityII pruner [serveur] [nb_threads] [fichier_pieces.csv] [taille_lot]   # élagage CPU
./eternityII pruner --gpu [serveur] [nb_threads] [fichier_pieces.csv] [taille_lot]   # élagage GPU (build CUDA=1)
```

| Paramètre | Défaut | Description |
|---|---|---|
| `serveur` | `localhost` | Adresse IP ou nom d'hôte du serveur |
| `nb_threads` | 1 | Nombre de processus de vérification à forker |
| `fichier_pieces.csv` | `data/pieces.csv` | Fichier de définition des pièces |
| `taille_lot` | 100 | Nombre de possibilités échangées par aller-retour TCP (borné à 65536) |

> L'option `--gpu` exige un binaire compilé avec `make CUDA=1` (voir
> [Pruner GPU (CUDA)](pruner_gpu_cuda.md)) — sinon le lancement échoue avec une
> erreur explicite (pas de repli CPU silencieux). Sur Jetson, penser à
> `LD_LIBRARY_PATH=/usr/local/cuda/lib64`.

### Échange par lots

Le contrôle d'une possibilité est très rapide : sans lot, l'aller-retour TCP (une
requête `GET` puis un acquittement par possibilité) plafonne le débit réseau et affame
le GPU. Le pruner échange donc avec le serveur **par lots** : il demande jusqu'à
`taille_lot` possibilités en un seul aller-retour et acquitte de même le lot analysé.

La taille de lot **borne la mémoire** détenue par le pruner (il ne reçoit/acquitte
jamais plus que ce lot) et dimensionne les tampons GPU (un lot = un lancement de
kernel sur tous les SM). Elle se règle :

- **au démarrage** : 4ᵉ argument de `pruner` / `pruner --gpu` (`taille_lot`) ;
- **à l'exécution** : commande interactive `prunerBatch <n>` (propagée aux process enfants).

Exemples :
```sh
./eternityII pruner localhost 4 data/pieces.csv 500     # lots de 500 (CPU)
./eternityII pruner --gpu localhost 1 data/pieces.csv 4096    # lots de 4096 (GPU)
```

> ⚠️ **Compatibilité protocole** : le handshake exige une égalité stricte des
> versions — **tous les nœuds (serveur, clients, pruners) doivent être recompilés
> ensemble** ; deux binaires de `VERSION` différente ne dialoguent pas. Voir
> [Échanges client / serveur](echanges_client_serveur.md).

## Mode test (autonome)

Exécute la recherche localement sans serveur. Utile pour valider la configuration ou
déboguer.

```sh
./eternityII test [fichier_pieces.csv]
```

## Option `--stop-on-solution`

Acceptée par tous les modes, à n'importe quelle position (retirée d'argv avant
l'analyse positionnelle) : s'arrêter à la **première** solution. Un processus de
recherche qui en trouve une se termine ; un serveur qui en reçoit une sauvegarde ses
files et s'arrête. **Par défaut (option absente), la recherche continue** : le
processus revient en arrière pour chercher d'autres solutions et le serveur reste en
service. Chaque solution est enregistrée dans un fichier **unique**
(`./solution_<pid>_<seq>` côté client, `./solution_server_<pid>_<seq>` côté serveur) —
plusieurs solutions ne s'écrasent jamais.

## Option `--headless` (exécution en service)

Acceptée par tous les modes, à n'importe quelle position, comme
`--stop-on-solution` : empêche le démarrage de la console interactive
(lecture de l'entrée standard). Pensée pour une exécution en service
(systemd `StandardInput=null`, conteneur sans TTY, …).

Sans ce flag, la console se termine déjà proprement dès qu'elle rencontre une
fin de fichier immédiate sur stdin (cas `/dev/null`) — pas de blocage ni de
plantage — mais un thread démarre puis meurt inutilement à chaque lancement.
`--headless` évite ce détour. Les logs ne changent pas dans les deux cas :
`logger.c` détecte que la sortie standard n'est pas un terminal
(`isatty(STDOUT_FILENO)`) et n'émet alors jamais de codes ANSI (bannière de
stats, zone Events, ligne d'édition) — la sortie est déjà du texte simple
adapté à `journald` ou à un fichier de log.

Exemple d'unité systemd minimale (serveur) :

```ini
[Service]
ExecStart=/opt/eternityII/eternityII server 80 --headless /opt/eternityII/data/pieces.csv
StandardInput=null
StandardOutput=journal
StandardError=journal
Restart=on-failure
```

## Option `--tcp-timeout` (serveur et client/pruner)

Acceptée par tous les modes réseau, à n'importe quelle position : règle le timeout
d'inactivité (secondes) des sockets TCP de travail (`SO_RCVTIMEO`/`SO_SNDTIMEO`), des deux
côtés de la connexion. Défaut `DEFAULT_TCP_TIMEOUT` (10 s). Une maintenance serveur longue
(sauvegarde, restore, tri) reste largement sous ce budget par construction (les boucles
d'attente de `datamanager.c` abandonnent après un délai borné plutôt que de tourner
indéfiniment) ; cette option reste une soupape pour un réseau plus lent ou un stock plus
volumineux. Valeur absente ou `<= 0` : ignorée (garde le défaut).

```sh
./eternityII server 80 --tcp-timeout 30 data/pieces.csv
./eternityII client --tcp-timeout 30 localhost 4
```

## Canal de contrôle et pilotage à distance

Chaque processus client ouvre automatiquement une seconde connexion TCP vers le
serveur, sur laquelle le **serveur** devient l'initiateur : demande de statistiques,
poussée de commandes (`pause`, `resume`, `limit`, …), récupération du meilleur plateau
connu. Ça se pilote depuis la console du serveur (`clients`, `clientsStats`,
`clientsCommand`, `pause`/`resume` — voir [Console interactive](console.md)) ou via
l'[API HTTP REST admin](api_http_rest.md) (`--http-port`). Détails du protocole :
[Canal de contrôle](echanges_client_serveur.md#canal-de-contrôle-v9).

## Format du fichier de pièces

```
ntiles: 256
<id> <top> <right> <bottom> <left>
...
```

- Chaque pièce est définie par son identifiant et les 4 couleurs de ses bords (entiers).
- La valeur `0` représente la bordure grise (bord du puzzle).
- Le fichier `data/pieces.csv` contient les 256 pièces officielles du puzzle 16×16.
- Le fichier `data/pieces16.csv` contient 16 pièces pour un puzzle 4×4 (tests rapides,
  nécessite un build `ETERN_PARTS=16` — voir [Compilation](compilation.md#configuration-du-puzzle)).

## Fichiers générés

### Sauvegardes (`.back`)

Le programme sérialise ses files de possibilités dans des fichiers binaires `.back` :

| Fichier | Contenu |
|---|---|
| `eternityII.back` | Files de possibilités en attente d'exploration (serveur) |
| `eternityII-in_analyse.back` | Possibilités actuellement distribuées aux clients |
| `eternityII.back_<pid>` | Sauvegarde propre à un processus client |
| `failed_exit_eternityII_<pid>.back` | Possibilités non vidées à l'arrêt anormal d'un client |
| `eternityII-best_board.back` / `temp-best_board.back` | Représentation complète du meilleur plateau connu du serveur (`g_server_best_board`, [src/core/best_board.h](../src/core/best_board.h)) — sauvegardé aux mêmes instants que les fichiers ci-dessus (autobackup, arrêt sur solution) |

Ces fichiers permettent de reprendre une recherche interrompue avec la commande
`restore` (voir [Console interactive](console.md)).

### Journal et solutions

| Fichier | Contenu |
|---|---|
| `events.log` | Journal des évènements horodatés (nouveaux records, solutions, etc.) **et des erreurs** (`log_error`/`log_errno`, ex. écriture de fichier échouée). Append-only ; voir [Console interactive](console.md#zone-events-en-bas-de-lécran). |
| `solution_<pid>_<seq>` | Plateau sérialisé quand une solution complète est trouvée (déclenche aussi un évènement). |

## Limitations connues

- **Cadence d'attente figée quand le serveur n'a rien à fournir.** Un thread de
  recherche ou de pruner sans travail assigné (`works == 0`) attend en boucle avec une
  cadence fixe de 100 µs (`MICRO_SLEEP`, `autosearch_step`/`autoprune_step`/
  `autoprune_gpu` dans [src/core/etii_search.c](../src/core/etii_search.c)), qu'il
  s'agisse d'une pénurie momentanée ou d'un épuisement durable du stock serveur. C'est
  typiquement le cas d'un `pruner` une fois que **toutes** les possibilités ont été
  vérifiées : le serveur n'a plus rien à distribuer, mais chaque thread continue de
  sonder à cadence rapide indéfiniment, consommant du CPU pour rien. Une piste serait
  d'appliquer à cette boucle un back-off progressif similaire à celui déjà en place
  côté thread d'alimentation (`feed_thread_aposs`, `NO_WORK_SLEEP_START`/
  `NO_WORK_SLEEP_MAX` dans [src/app/static_variables.h](../src/app/static_variables.h)),
  afin de distinguer une pénurie ponctuelle d'un épuisement long/définitif — par
  opposition aux pauses (régulation `REQUEST_PAUSE` / admin `REQUEST_ADMIN_PAUSE`),
  qui bénéficient déjà chacune d'une cadence dédiée (`PAUSE_POLL_SLEEP_US` /
  `ADMIN_PAUSE_POLL_SLEEP_US`).

## Voir aussi

- [Console interactive](console.md) — commandes interactives et interface.
- [Échanges client / serveur](echanges_client_serveur.md) — protocole TCP et canal de contrôle.
- [API HTTP REST admin](api_http_rest.md) — télémétrie et pilotage HTTP du serveur.
- [Pruner GPU (CUDA)](pruner_gpu_cuda.md) — mode `pruner --gpu` en détail.

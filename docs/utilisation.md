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
./eternityII server [nb_threads] [--expand-level N] [--http-port N] [--http-token-file CHEMIN] [fichier_pieces.csv]
```

| Paramètre | Défaut | Description |
|---|---|---|
| `nb_threads` | 80 | Nombre de connexions clients simultanées |
| `--expand-level N` | *(absent)* | Développe le stock au démarrage jusqu'au niveau de curseur `N` (anti-famine, voir ci-dessous) |
| `--http-port N` | *(absent)* | Active l'[API HTTP REST admin](api_http_rest.md) sur `127.0.0.1:N` (désactivée par défaut) |
| `--http-token-file CHEMIN` | *(absent)* | Jeton Bearer requis pour les commandes admin privilégiées `restore`/`backup` de l'[API HTTP](api_http_rest.md#authentification-restorebackup) — sans cette option, ces deux commandes restent inaccessibles via l'API |
| `fichier_pieces.csv` | `data/pieces.csv` | Fichier de définition des pièces |

Exemples :
```sh
./eternityII server 80
./eternityII server 80 data/pieces.csv
./eternityII server 80 --expand-level 4 data/pieces.csv
./eternityII server 80 --http-port 8080 data/pieces.csv
./eternityII server 80 --http-port 8080 --http-token-file /etc/eternityii/http-token data/pieces.csv
```

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

L'expansion est bornée sur deux axes (dans
[src/app/static_variables.h](../src/app/static_variables.h)) : `EXPAND_MAX_LEVELS` (4)
plafonne le nombre de passes quelle que soit la consigne — garde-fou en *profondeur* —
et `EXPAND_MAX_STOCK` (100000) plafonne le *nombre* de possibilités entre passes ;
comme le facteur de branchement est inconnu et qu'une seule passe peut exploser, ce
plafond en nombre est le vrai garde-fou de temps et de mémoire. Sur le puzzle 256 le
branchement mesuré est ≈11×/niveau (niveau 3 → ~500 possibilités, niveau 4 → ~5300,
niveau 5 → ~56000) : **le niveau 3–4 est le point idéal** — de quoi remplir le stock
local de tous les clients avec réserve, en bien moins d'une seconde. La même opération
est disponible à chaud via la commande interactive `expand N` (utile si le stock
distribuable se raréfie en cours de recherche).

> Cette expansion est le pendant *serveur* de la délégation anticipée côté *client*
> (sonde de faim `INST_NEED_WORK`, VERSION 8) décrite dans
> [Échanges client / serveur](echanges_client_serveur.md).

## Mode client

Se connecte à un serveur et lance `N` processus de recherche en parallèle.

```sh
./eternityII client [serveur] [nb_threads] [max_stock_par_thread] [fichier_pieces.csv]
```

| Paramètre | Défaut | Description |
|---|---|---|
| `serveur` | `localhost` | Adresse IP ou nom d'hôte du serveur |
| `nb_threads` | 1 | Nombre de processus de recherche à forker |
| `max_stock_par_thread` | 300 | Nombre max de possibilités stockées par thread avant d'en renvoyer au serveur |
| `fichier_pieces.csv` | `data/pieces.csv` | Fichier de définition des pièces |

Exemples :
```sh
./eternityII client localhost
./eternityII client 192.168.1.10 8
./eternityII client localhost 4 300 data/pieces.csv
```

## Mode pruner (élagage)

Un **pruner** réutilise la même plomberie réseau qu'un client, mais au lieu d'explorer
il demande au serveur des possibilités *à vérifier* et élague celles qui n'ont aucune
continuation possible. Deux variantes :

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
| `events.log` | Journal des évènements horodatés (nouveaux records, solutions, etc.). Append-only. |
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

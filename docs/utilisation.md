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
./eternityII server [nb_threads] [--expand-level N] [--expand-max-stock N] [--expand-max-levels N] [--stock-files N] [--rebalance-budget N] [--no-rebalance] [--no-autobackup] [--stock-max-ram N] [--stock-spill-dir CHEMIN] [--no-rmnonext] [--rmnonext-interval N] [--sort-enabled] [--sort-interval N] [--sort-direction asc|desc] [--sort-lock-attempts N] [--auto-roles] [--http-port N] [--http-token-file CHEMIN] [--config-file CHEMIN] [fichier_pieces.csv]
```

| Paramètre | Défaut | Description |
|---|---|---|
| `nb_threads` | 80 | Nombre de connexions clients simultanées |
| `--expand-level N` | *(absent)* | Développe le stock au démarrage jusqu'à `N` pièces posées (anti-famine, voir ci-dessous) |
| `--expand-max-stock N` | `EXPAND_MAX_STOCK` (100000) | Plafonne en NOMBRE de possibilités la pré-expansion `--expand-level` (voir ci-dessous) ; sans effet si `--expand-level` est absent |
| `--expand-max-levels N` | `EXPAND_MAX_LEVELS` (4) | Plafonne en NOMBRE DE PASSES la pré-expansion `--expand-level` (voir ci-dessous) ; sans effet si `--expand-level` est absent |
| `--stock-files N` | `NB_FILE_POSSIBILITY_DEFAULT` (10) | Nombre de files de stock, fixé une seule fois au démarrage (jamais à chaud), plafonné à `NB_FILE_POSSIBILITY_MAX` (128) — voir ci-dessous |
| `--rebalance-budget N` | `REBALANCE_BUDGET_DEFAULT` (1000) | Nombre de possibilités rééquilibrées entre files à chaque tour serveur (10 s) — voir ci-dessous |
| `--no-rebalance` | *(absente, rééquilibrage ACTIF)* | Ne rééquilibre plus automatiquement le stock entre files à chaque tour — voir ci-dessous |
| `--no-autobackup` | *(absente, sauvegarde automatique ACTIVE)* | Ne sauvegarde plus automatiquement le stock toutes les ~60 s — voir ci-dessous |
| `--stock-max-ram N` | *(absent, illimité)* | Plafond en Mo des DEUX pools de stock (non vérifié + vérifié) — voir ci-dessous |
| `--stock-spill-dir CHEMIN` | `./eternityii-spill` | Répertoire de débordement sur disque une fois `--stock-max-ram` approché — voir ci-dessous |
| `--no-rmnonext` | *(absente, élagage ACTIF)* | Ne démarre pas l'élagage automatique des possibilités sans suite — voir ci-dessous |
| `--rmnonext-interval N` | `RMNONEXT_INTERVAL_DEFAULT` (30) | Intervalle en secondes entre deux passes d'élagage automatique ; sans effet si `--no-rmnonext` est présent |
| `--sort-enabled` | *(absente, désactivée)* | Active le tri périodique du stock par file — voir ci-dessous |
| `--sort-interval N` | `SORT_PERIODIC_INTERVAL_DEFAULT` (60) | Intervalle en secondes entre deux passes de tri périodique ; sans effet si `--sort-enabled` est absent |
| `--sort-direction asc\|desc` | `asc` | Sens du tri périodique |
| `--sort-lock-attempts N` | `SORT_LOCK_ATTEMPTS_DEFAULT` (50) | Tentatives de trylock par segment (une file d'un pool) avant abandon pour cette passe — voir ci-dessous |
| `--auto-roles` | *(absente, désactivée)* | Active la politique automatique de dosage recherche/contrôle du parc — voir ci-dessous |
| `--http-port N` | *(absent)* | Active l'[API HTTP REST admin](api_http_rest.md) sur `127.0.0.1:N` (désactivée par défaut) |
| `--http-token-file CHEMIN` | *(absent)* | Jeton Bearer requis pour toute commande de MODIFICATION de l'[API HTTP](api_http_rest.md#authentification) (`pause`, `resume`, `limit`, `maxStockByThread`, `shallowRootAbandonDepth`, `prunerBatch`, `clientsCommand`/`clientsCmd`, `restore`, `backup`) — sans cette option, ces commandes restent inaccessibles via l'API (seule `clientsWork`, en lecture seule, reste utilisable) |
| `--config-file CHEMIN` | `./eternityii-server.conf` | Fichier de configuration `clé = valeur`, lu une seule fois au démarrage — voir ci-dessous |
| `fichier_pieces.csv` | `data/pieces.csv` | Fichier de définition des pièces |

Exemples :
```sh
./eternityII server 80
./eternityII server 80 data/pieces.csv
./eternityII server 80 --expand-level 4 data/pieces.csv
./eternityII server 80 --expand-level 4 --expand-max-stock 1000000 data/pieces.csv
./eternityII server 80 --expand-level 8 --expand-max-stock 1000000 --expand-max-levels 8 data/pieces.csv
./eternityII server 80 --stock-files 32 --rebalance-budget 5000 data/pieces.csv
./eternityII server 80 --no-rebalance data/pieces.csv
./eternityII server 80 --no-autobackup data/pieces.csv
./eternityII server 80 --stock-max-ram 4096 data/pieces.csv
./eternityII server 80 --stock-max-ram 4096 --stock-spill-dir /var/lib/eternityii/spill data/pieces.csv
./eternityII server 80 --sort-enabled --sort-interval 120 --sort-direction desc data/pieces.csv
./eternityII server 80 --no-rmnonext data/pieces.csv
./eternityII server 80 --rmnonext-interval 3600 data/pieces.csv
./eternityII server 80 --http-port 8080 data/pieces.csv
./eternityII server 80 --http-port 8080 --http-token-file /etc/eternityii/http-token data/pieces.csv
./eternityII server --config-file /etc/eternityii/server.conf
```

### Fichier de configuration serveur (`--config-file`)

Comme le client (voir [Mode client](#mode-client) ci-dessous), le serveur peut lire
au démarrage un fichier `clé = valeur` qui pré-remplit les options non fournies en
ligne de commande — priorité **CLI > fichier > défauts**. Contrairement au client, le
serveur n'a pas d'orchestrateur de démarrage différé : le fichier est lu une seule
fois, de façon synchrone, avant que le serveur ne démarre réellement (pas de
décompte, pas de `configApply` — le serveur n'a pas de configuration "en
préparation" à appliquer à chaud, celle-ci reste propre au mode client/pruner).
Les commandes console `config` (affichage) et `configSave` (persistance), elles,
fonctionnent aussi côté serveur — voir [Console interactive](console.md#général).

```sh
./eternityII server --config-file /etc/eternityii/server.conf
```

Chemin par défaut `./eternityii-server.conf` (même convention que
`./eternityii-client.conf`, `--config-file` est la même option pour les deux modes —
un seul mode s'exécute par process). Fichier absent ou illisible : jamais une erreur
de démarrage, les valeurs par défaut/CLI s'appliquent normalement.

Toutes les options de démarrage du serveur sont couvertes, avec les mêmes clés que
les options CLI correspondantes (sans les tirets) :

```
nb_threads         = 40
parts_file          = data/pieces.csv
expand_level       = 3
expand_max_stock   = 500000
expand_max_levels  = 6
http_port          = 8080
http_token_file    = /etc/eternityii/http-token
stock_files        = 32
stock_max_ram      = 4096
stock_spill_dir    = /var/lib/eternityii/spill
rebalance_budget   = 5000
rebalance_enabled  = 0
autobackup_enabled = 0
tcp_timeout        = 20
sort_enabled       = 1
sort_interval      = 120
sort_direction     = desc
rmnonext_enabled   = 0
rmnonext_interval  = 3600
auto_roles         = 1
stop_on_solution   = 0
headless           = 1
```

`nb_threads` et `parts_file` correspondent aux paramètres positionnels
(`server [nb_threads] [pieces.csv]`) ; toutes les autres clés correspondent à
l'option CLI de même nom (`stop_on_solution`/`headless`/`auto_roles`/`sort_enabled`
valent `0` ou `1` ; `sort_direction` vaut `asc` ou `desc`). `rmnonext_enabled`,
`rebalance_enabled` et `autobackup_enabled` sont les trois seules clés booléennes
dont le **défaut est `1`** : c'est `rmnonext_enabled = 0` (équivalent de
`--no-rmnonext`), `rebalance_enabled = 0` (équivalent de `--no-rebalance`) et
`autobackup_enabled = 0` (équivalent de `--no-autobackup`) qui sont la demande
explicite. Une ligne à clé inconnue ou à valeur invalide est journalisée
(avertissement) puis ignorée, le chargement continue avec les lignes suivantes.

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
qui maintient des files de taille comparable et rend le gain de `--stock-files` effectif.

Sans autre précaution, un appel ADD ou GET non contesté (le cas nominal à faible concurrence)
verrouille toujours la première file libre en partant de la file 0 — tout le trafic s'y
concentrerait donc systématiquement, à l'exact opposé de l'objectif de `--stock-files`, et au
prix d'un travail constant pour `--rebalance-budget` qui doit sans cesse compenser ce biais.
Chaque appel démarre en réalité son balayage sur une file différente (rotation round-robin,
indépendante entre ADD et GET, et entre le pool non vérifié et le pool vérifié) plutôt que
toujours sur la file 0 : la charge se répartit d'elle-même sur les `--stock-files` files, le
rééquilibrage n'ayant plus qu'à corriger de véritables déséquilibres de contenu, pas un biais
structurel de trafic.

Le pool des possibilités **en cours d'analyse** (« analysé ») a un point sensible différent : un
pruner qui acquitte un lot (`prunerBatch`, voir ci-dessous) verrouille et déverrouille cette file
en boucle serrée pour chacune des possibilités du lot, un temps de blocage sensiblement plus long
que pour un ADD/GET isolé — visible côté opérateur comme un autre pruner « bloqué » sur la même
file le temps du lot. Chaque connexion serveur (recherche ou pruner) se voit assigner une file
dédiée pour toute la durée de sa connexion (dérivée de son propre slot dans le pool de threads),
plutôt qu'une rotation par possibilité ou par lot : toutes les possibilités qu'elle reçoit ET
acquitte tombent ainsi sur la même file, connue des deux côtés — le retrait devient direct
(une seule file verrouillée) au lieu de devoir chercher parmi toutes. Deux connexions actives
occupent toujours des slots différents, donc se retrouvent en général sur des files différentes ;
sous forte concurrence, augmenter `--stock-files` réduit la probabilité que deux connexions
partagent la même file.

`--tcp-timeout` (ci-dessous) élargit en plus la marge côté réseau. Chaque artefact sauvegardé
(stock, pool analysé, meilleur plateau connu, registre des clients connus) n'est réécrit que
si son propre contenu a changé depuis sa dernière écriture — la durée de la dernière
sauvegarde effectivement exécutée est exposée par `GET /api/v1/status`
(`last_backup_duration_ms`, voir [API HTTP REST admin](api_http_rest.md)).

> ⚠️ **Dimensionnement de `nb_threads`** : chaque processus client connecté ouvre,
> en plus des connexions de travail de ses forks, une connexion de
> [canal de contrôle](echanges_client_serveur.md#canal-de-contrôle-v9-étendu-en-v10-et-v12) qui occupe un
> slot du **même** pool. Un serveur dimensionné au plus juste doit compter
> (connexions de travail simultanées) **+** (processus clients connectés), pas
> seulement le premier terme. Le défaut (80) laisse une large marge.

### Rééquilibrage automatique du stock (`--no-rebalance`)

À chaque tour serveur (10 s), `check_server_step` déplace jusqu'à
`--rebalance-budget` possibilités de la file la plus pleine vers la plus vide (voir
ci-dessus). C'est ce qui garde les files de taille comparable, et donc court le temps
de blocage **par fichier** d'une sauvegarde cohérente — le bénéfice de `--stock-files`
en dépend directement.

`--no-rebalance` (ou `rebalance_enabled = 0`) **supprime cet appel de tour**. C'est la
deuxième des trois options-drapeaux *négatives* du programme, avec `--no-rmnonext` et
[`--no-autobackup`](#sauvegarde-automatique-périodique---no-autobackup) :
toutes les autres sont des opt-in, le rééquilibrage, lui, est actif depuis toujours et
le reste par défaut.

> ⚠️ `--rebalance-budget 0` **n'est pas** un moyen de couper le rééquilibrage : une
> valeur `<= 0` est ignorée à l'analyse des options et le budget garde son défaut
> (1000). Le budget dose l'appel, il ne le supprime pas — d'où un drapeau séparé.

À réserver à un serveur dont on veut qu'**aucune possibilité ne change de file sans
ordre explicite** : un rééquilibrage défait l'ordre qu'un `sortAscFiles`/`sortDescFiles`
(ou le tri périodique `--sort-enabled`) vient d'établir dans les files qu'il touche. En
contrepartie, les files dérivent en taille au fil du trafic, et la sauvegarde cohérente
bloque d'autant plus longtemps par fichier.

Le rééquilibrage reste possible **à la demande** : la commande console `rebalance [n]`
(ou `POST /api/v1/command`, voir [API HTTP REST](api_http_rest.md)) déclenche un pas au
moment choisi par l'opérateur. Même partage des rôles qu'entre `--no-rmnonext` et
`removeNoNext` : l'option gouverne l'appel automatique, jamais la commande manuelle.

Une ligne est journalisée dans `events.log` au démarrage **uniquement en cas de
désactivation** (`rééquilibrage automatique du stock désactivé (--no-rebalance)`) — le
cas actif est déjà tracé par le budget dans le diagnostic de démarrage
(`rebalance_enabled=oui/non`). Des files qui dérivent en taille sont alors un
comportement voulu, traçable, et non une régression à chercher.

```sh
./eternityII server 80 --no-rebalance data/pieces.csv
```

### Sauvegarde automatique périodique (`--no-autobackup`)

Toutes les ~60 s (6 tours de 10 s), `check_server_step` décide, **par artefact et
seulement si son contenu a changé**, de réécrire les quatre fichiers temporaires :
`./temp.back` et `./temp_analysed.back` (stock et pool analysé, à un instant T
unique), `./temp-best_board.back` et `./temp-known_clients.back`. C'est le pendant
automatique de la commande console `backup`, et la **seule** persistance périodique du
serveur.

**Aucune sauvegarde pendant une passe d'expansion** (`--expand-level`, commande `expand`).
Une passe vide tout le pool non vérifié dans sa file de travail avant de le reconstruire :
pendant ce temps, une partie du stock n'est ni dans les pools ni sur disque, et un cliché
l'omettrait — en écrasant la sauvegarde précédente par un stock amputé. Les portes du stock
et du pool analysé ne sont donc **pas consultées** pendant une passe : la mutation reste en
attente et part au premier tour qui suit. Entre deux passes, le stock est de nouveau entier
dans les pools, la sauvegarde y reprend normalement. Même règle pour tout autre chemin
(`consistent_backup` rend `BACKUP_SKIPPED_EXPANSION`, fichier cible intact) : la commande
`backup` échoue avec un message explicite, l'arrêt sur solution garde la sauvegarde
précédente en le journalisant, et `restore` est refusé pendant toute l'expansion.

`--no-autobackup` (ou `autobackup_enabled = 0`) **supprime cette décision** : les quatre
portes ne sont même plus consultées, plus aucune écriture périodique n'a lieu, et donc
plus aucun gel des files de stock à ce titre. C'est la troisième et dernière
option-drapeau *négative* du programme, avec
[`--no-rmnonext`](#élagage-automatique-des-possibilités-sans-suite---no-rmnonext---rmnonext-interval)
et [`--no-rebalance`](#rééquilibrage-automatique-du-stock---no-rebalance) : toutes les
autres sont des opt-in, la sauvegarde automatique, elle, est active depuis toujours et le
reste par défaut.

> ⚠️ **À la différence des deux autres drapeaux négatifs, celui-ci a un coût en cas de
> panne.** `--no-rmnonext` et `--no-rebalance` ne coupent qu'une optimisation interne ;
> `--no-autobackup` retire au serveur sa seule persistance périodique. Un arrêt brutal
> (crash, coupure, `kill -9`) perd alors **tout le travail accumulé depuis la dernière
> sauvegarde manuelle** — à n'utiliser que si l'on sauvegarde soi-même.

L'usage visé est un serveur dont on veut **maîtriser l'instant des sauvegardes** :
fenêtre de maintenance choisie, ou stock si volumineux que chaque écriture coûte assez
cher pour ne pas la laisser partir au hasard du trafic. La sauvegarde reste possible **à
la demande** : la commande console `backup` (ou `POST /api/v1/command`, voir
[API HTTP REST](api_http_rest.md)) écrit `eternityII.back` et ses compagnons au moment
choisi, et `--stop-on-solution` sauvegarde toujours avant de s'arrêter. Même partage des
rôles qu'entre `--no-rmnonext` et `removeNoNext` : l'option gouverne l'écriture
automatique, jamais la commande manuelle.

Une ligne est journalisée dans `events.log` au démarrage **uniquement en cas de
désactivation** (`sauvegarde automatique désactivée (--no-autobackup)`) — le cas actif est
déjà tracé par le diagnostic de démarrage (`autobackup_enabled=oui/non`). Un `temp.back`
qui ne vieillit plus est alors un comportement voulu, traçable, et non une régression à
chercher. `GET /api/v1/status` continue de rapporter `last_backup_duration_ms`, qui reste
simplement à sa dernière valeur connue (0 si aucune sauvegarde n'a eu lieu).

```sh
./eternityII server 80 --no-autobackup data/pieces.csv
```

### Élagage automatique des possibilités sans suite (`--no-rmnonext`, `--rmnonext-interval`)

Par défaut, le serveur démarre un thread d'élagage (`rmnonext_thread`,
`src/app/etii_server.c`) qui appelle toutes les 30 s (`--rmnonext-interval`, défaut
`RMNONEXT_INTERVAL_DEFAULT`) `remove_possibilities_with_no_next` :
un parcours de **tout** le stock qui supprime les possibilités dont aucune continuation
n'est valide. C'est le pendant automatique de la commande console `removeNoNext`.

Ce parcours est proportionnel à la taille du stock et **tient les files pendant qu'il
dure**. Sur une pile très longue (plusieurs millions de possibilités), une passe peut
donc saturer le serveur : les threads ADD/GET attendent, les clients approchent leur
timeout TCP, et une nouvelle passe repart à peine la précédente terminée.

Deux dosages du même mécanisme sont disponibles, du plus doux au plus radical :

- **`--rmnonext-interval N`** (ou `rmnonext_interval = N`) **espace** les passes. Sur un
  stock qui met plusieurs minutes à se parcourir, un intervalle de 30 s revient à
  enchaîner les passes sans répit ; `--rmnonext-interval 3600` laisse le serveur
  respirer entre deux, **sans renoncer** à l'élagage.
- **`--no-rmnonext`** (ou `rmnonext_enabled = 0`) **ne démarre jamais ce thread**. C'est
  l'une des trois seules options-drapeaux *négatives* du programme, avec
  [`--no-rebalance`](#rééquilibrage-automatique-du-stock---no-rebalance) et
  [`--no-autobackup`](#sauvegarde-automatique-périodique---no-autobackup) : toutes les
  autres sont des opt-in, l'élagage, lui, est actif depuis toujours et le reste par
  défaut. `--rmnonext-interval`
  est alors sans effet — il n'y a plus de passe à espacer.

L'élagage reste possible **à la demande** : la commande console `removeNoNext` (ou
`POST /api/v1/command`, voir [API HTTP REST](api_http_rest.md)) déclenche une passe au
moment choisi par l'opérateur — typiquement pendant une fenêtre sans client. Même
partage des rôles qu'entre `--sort-enabled` et `sortAscFiles`/`sortDescFiles` : l'option
gouverne le thread périodique, jamais la commande manuelle.

Le garde-fou historique reste en place quand l'élagage est actif : une passe automatique
est **sautée** tant qu'au moins un client est connecté (`rmnonext_pass`). Il ne suffit
pas sur un serveur de production presque toujours occupé — soit les passes ne partent
jamais, soit elles partent pendant un creux et durent trop longtemps — d'où cette option.

Une ligne est journalisée dans `events.log` au démarrage dans les deux cas —
`élagage automatique activé (intervalle Ns)` ou `élagage automatique désactivé
(--no-rmnonext)` : un stock qui ne décroît plus tout seul, ou qui décroît beaucoup plus
lentement qu'avant, est alors un comportement voulu, traçable, et non une régression à
chercher.

```sh
./eternityII server 80 --rmnonext-interval 3600 data/pieces.csv
./eternityII server 80 --no-rmnonext data/pieces.csv
```

### Tri périodique du stock (`--sort-enabled`)

`--sort-enabled` active un thread dédié qui trie **chaque file** du stock, à intervalle
régulier (`--sort-interval`, défaut 60 s), dans le sens choisi par `--sort-direction`
(`asc` par défaut, ou `desc`).

Le tri se fait **file par file, sans regroupement** (même comportement que la commande
console `sortAscFiles`/`sortDescFiles`) : chaque file reste à sa taille d'origine, la
distribution round-robin entre files (voir ci-dessus) n'est pas perturbée — contrairement
à `sortAsc`/`sortDesc`, qui fusionnent tout dans la file 0 avant de trier et sont donc
réservées à un appel console manuel.

**Tourne EN CONTINU, quel que soit le trafic.** Une première version suspendait toute
la passe dès qu'un client était connecté (même garde-fou que l'élagage automatique
`removeNoNext`), mais cela rendait le tri quasi inatteignable sur un serveur de
production, presque toujours occupé par au moins un client. Le tri verrouille
désormais **chaque file (et chaque pool, non vérifié/vérifié) individuellement**, via
un `pthread_mutex_trylock` borné par `--sort-lock-attempts` tentatives (défaut 50, soit
~5 ms de patience par segment) — même discipline que le rééquilibrage incrémental ou
l'extraction round-robin (`scroll_from_pool`/`put_to_pool`, voir
[Maîtrise de la charge serveur](#maîtrise-de-la-charge-serveur---stock-files---rebalance-budget---tcp-timeout)
ci-dessus). Un segment toujours verrouillé après ce nombre de tentatives est simplement
**sauté pour cette passe** — jamais perdu, retenté à la passe suivante — sans jamais
bloquer les threads ADD/GET, quel que soit le trafic en cours.

**Chaque segment mémorise sa dernière direction de tri** (`asc`/`desc`/inconnu) et une
passe **saute directement** (sans même tenter le `trylock`) tout segment déjà trié dans
le sens demandé et non modifié depuis — la grande majorité du temps en régime stable,
où la plupart des files n'ont reçu aucun ADD/réinjection/rééquilibrage entre deux passes.
Toute écriture dans une file (ADD client, rechargement depuis le débordement disque,
réinjection d'un bail expiré, pas de rééquilibrage, regroupement/répartition console)
oublie ce souvenir : le segment repasse "à trier" et sera retrié à la prochaine passe. Un
changement de `--sort-direction` (à chaud via `configApply`) invalide également le
souvenir de tous les segments, puisque la direction mémorisée ne correspond plus à celle
demandée.

Désactivé par défaut (opt-in, comme `--auto-roles`) : aucun thread supplémentaire n'est
démarré sans demande explicite. `--sort-interval`, `--sort-direction` et
`--sort-lock-attempts` sont sans effet tant que `--sort-enabled` est absent.

Chaque passe journalise une courte ligne `tri périodique du stock (asc|desc, N/M
segments)` dans `events.log` (M = 2 × nombre de files ; N = segments effectivement
(re)triés lors de CETTE passe, jamais ceux sautés parce que déjà à jour). `N` est donc
bas — souvent 0 — en régime stable, ce qui est le comportement ATTENDU depuis
l'optimisation ci-dessus, et ne signale un problème que si des segments restent modifiés
en continu sans jamais retomber à 0 : dans ce cas seulement, `N < M` répété peut indiquer
des segments occupés au delà de `--sort-lock-attempts` (à relever, comme avant). Voir
[Console interactive](console.md#zone-events-en-bas-de-lécran).

```sh
./eternityII server 80 --sort-enabled --sort-interval 120 --sort-direction desc --sort-lock-attempts 100 data/pieces.csv
```

### Plafond RAM du stock (`--stock-max-ram`)

Rien ne bornait auparavant la croissance du stock serveur au fil du trafic de recherche et de
délégation : un stock de plusieurs millions de possibilités peut consommer plusieurs Go de RAM
(`sizeof(struct possibility_packet)` = 576 octets sur le puzzle 256 pièces, plus le coût réel
d'allocation — `Element` de la liste chaînée + deux `malloc()` par possibilité stockée, cf.
`core/lifo.c` — porte le coût réel à environ 632 octets/possibilité).

`--stock-max-ram N` fixe un plafond en **Mo**, et c'est en **octets réellement résidents** que
chaque ajout lui est confronté (`datamanager_resident_bytes`) — jamais en nombre de
possibilités. La distinction n'était que théorique tant que le stock rangeait des
`possibility_packet` entiers, une taille par possibilité étant alors constante ; elle devient
la seule formulation juste dès que les enregistrements varient de taille. L'occupation
affichée par la console (`stockMemory`) et par `GET /api/v1/stats` est donc MESURÉE, plus
extrapolée d'un compte. Ce plafond couvre les
**deux pools de stock ensemble** (non vérifié + vérifié) — jamais le pool des possibilités en
cours d'analyse, déjà borné autrement (baux d'expiration, nombre de clients en vol, voir
[Échanges client/serveur](echanges_client_serveur.md)), ni les lots pruner en vol
(`prunerBatch`, jusqu'à ~36 Mo) ni la table de recherche partagée (~6,6 Mo) : prévoir une marge
plutôt que de régler ce plafond au plus près de la RAM physique disponible (60 à 70 % est un
point de départ raisonnable).

Un ajout qui ferait dépasser le plafond est **refusé** — le même chemin de dégradation
gracieuse qu'un refus de contention (`INST_ERROR` côté client, qui conserve la possibilité en
local et la renverra plus tard, journalisé en `log_info` — non fatal — plutôt qu'en `log_error`,
voir [AGENTS.md § Server load management](../AGENTS.md#server-load-management)). Ce
refus ne fait jamais perdre ce qui est déjà résident ; en pratique il devient rare une fois
`--stock-spill-dir` configuré (ci-dessous), qui déporte l'excédent sur disque avant que ce mur
dur ne soit atteint. Réglable à chaud via la commande console `stockMaxRam <mo>` (`<mo> <= 0`
désactive le plafond) et consultable via `stockMemory` ou `GET /api/v1/status`
(`stock_ram_limit_mb`/`stock_ram_used_mb`, voir [API HTTP REST admin](api_http_rest.md)).

Exemple :
```sh
./eternityII server 80 --stock-max-ram 2048 data/pieces.csv   # 2 Go pour les deux pools de stock
```

### Mémoire du serveur et arènes malloc

Le RSS du serveur (`top`, `ps`) et l'occupation qu'affichent `stockMemory` et
`GET /api/v1/stats` ne mesurent pas la même chose : la seconde compte les octets que le stock
occupe réellement, le premier ce que l'allocateur garde au processus. Sous glibc, l'écart
venait surtout des **arènes malloc** : chaque thread de connexion en avait une, et les
échanges avec un pruner font migrer tout le stock d'une arène à l'autre. Une possibilité
servie à un pruner est libérée du pool non vérifié (dans l'arène où elle avait été allouée,
celle du thread qui a restauré le stock par exemple), puis renaît dans le pool vérifié,
allouée par le thread de connexion du pruner qui l'a renvoyée. Chaque arène garde son haut de
tas, et glibc ne rend presque jamais au système les trous du milieu d'une arène : l'arène de
départ se vide sans que le RSS baisse, pendant que la nouvelle grandit.

Le serveur plafonne donc ses arènes à **une seule** au démarrage (`mallopt(M_ARENA_MAX, 1)`,
`server_cap_malloc_arenas`, `app/app_runtime.c`), avant de créer le moindre thread. Mesuré
sous Linux sur 1 M de possibilités passées par un pruner (lot de 100, toutes renvoyées
vivantes), stock et octets alloués identiques avant et après :

| Arènes | RSS stock chargé | RSS après le passage du pruner |
|---|---|---|
| défaut glibc (une par thread) | 129 Mo | 220 Mo |
| défaut + `malloc_trim(0)` en fin | — | 193 Mo |
| 2 | — | 181 Mo |
| **1 (serveur)** | 129 Mo | **131 Mo** |

Un seul pruner suffit à produire l'écart, et 8 pruners ne le creusent pas : c'est la
migration entre arènes qui coûte, pas le nombre de threads. Deux autres pistes ont été
mesurées sans effet : le pool des possibilités en cours d'analyse (qui garde la forme brute)
et la `File` temporaire de paquets bruts de `scroll_from_pool`.

Le prix est un verrou d'allocation partagé par tous les threads du serveur. Dans le pire cas
mesuré (8 threads qui ne font qu'allouer, sans réseau, ~500 000 possibilités/s), la passe
prend 3 s au lieu de 1,2 s ; avec un thread, aucune différence. Les threads de connexion
passent l'essentiel de leur temps bloqués sur TCP, à des débits très en dessous de ce banc.

**`MALLOC_ARENA_MAX` fourni par l'opérateur l'emporte toujours** : le serveur n'y touche plus
et le journalise. Pour revenir au comportement glibc par défaut, par exemple pour mesurer :

```sh
MALLOC_ARENA_MAX=8 ./eternityII server 80 data/pieces.csv
```

Hors glibc (macOS), rien n'est fait. L'effet existe aussi avec l'allocateur macOS, en plus
faible (+22 Mo mesurés sur le même banc avec des lots de 100), mais il n'y a pas de réglage
équivalent.

### Débordement sur disque du stock (`--stock-spill-dir`)

Le plafond RAM ci-dessus, seul, n'a aucun recours : une fois atteint, tout ADD supplémentaire
est refusé jusqu'à ce qu'un GET libère de la place. `--stock-spill-dir CHEMIN` (défaut
`./eternityii-spill`) donne un recours : un thread dédié (tick de 100 ms) écrit la possibilité
la plus **ancienne** (jamais servie, en tête de file — les possibilités récemment ajoutées,
plus susceptibles d'être demandées bientôt, restent en RAM) dans un fichier de « segment » sur
disque dès que l'occupation approche 90 % du plafond, et la recharge automatiquement si
l'occupation redescend sous 25 % et qu'un débordement existe — dans les deux cas jusqu'à
converger vers 75 % (bande morte entre 75 % et 90 % où rien ne se passe, pour éviter
d'alterner écriture/lecture à chaque tick sur une occupation qui oscille près d'un seuil). Le
plafond RAM lui-même (`--stock-max-ram`) reste le filet de sécurité si l'éviction ne suit pas
assez vite un pic d'ADD — cette option ne le remplace pas, elle le rend moins souvent atteint.

**Expansion sous plafond** (`--expand-level` au démarrage, commande console `expand`).
Chaque passe vide tout le pool non vérifié dans une file de travail avant de le reconstruire.
Cette file **compte dans l'occupation mesurée** (et donc dans `--stock-max-ram`, `stockMemory`,
`GET /api/v1/stats`) : ce sont les possibilités du stock, sorties du pool le temps de la passe.
Non comptée, elle laissait le pool se remplir d'enfants jusqu'au plafond par-dessus une file
de la taille du stock — la RAM réelle dépassait le plafond d'autant — et faisait paraître la
RAM vide au début de chaque passe : le débordement rechargeait alors ses segments, que la
passe renvoyait sur disque dès qu'elle avait rempli le pool. Sur un gros stock, ce va-et-vient
dominait la durée de l'expansion.

Deux règles en découlent :

- **pas de rechargement pendant une expansion** — ce qui remonterait n'est pas développé par
  la passe en cours et lui dispute la place ; l'éviction, elle, continue. Le rechargement
  reprend au premier tick qui suit ; `events.log` note « rechargement disque suspendu pendant
  l'expansion » s'il était en cours à son début ;
- **la file rend au stock ce qu'elle ne peut garder** : si elle tient à elle seule le plafond
  alors que les pools sont vides (le débordement, qui n'évince que depuis les pools, n'aurait
  rien à déplacer), la passe rend un bloc de sa file au pool pour qu'il soit évincé, plutôt
  que d'attendre indéfiniment. Ces possibilités seront développées à une passe suivante
  (`events.log` : « rendues au stock pour laisser le débordement évincer »).

Contrepartie assumée : sous un plafond donné, une passe a moins de place pour ses enfants,
donc écrit davantage sur disque — c'est ce que coûte un plafond qui borne vraiment la RAM.

**Une passe développe aussi le stock déporté sur disque**, pas seulement le pool résident.
Une fois sa file de travail épuisée, elle lit les segments **par le bas** de chaque pile — les
plus anciens d'abord —, un segment entier à la fois, et les développe comme le reste. Au début
de chaque passe, elle note le sommet de chaque pile (sa *frontière*). Les enfants qu'elle évince
vont au-dessus et ne sont donc jamais repris par la même passe, comme pour le pool résident. Le
segment de frontière est lu en entier : ce qu'il contenait au début de la passe est développé,
ce que la passe y a ajouté repart tel quel — au plus un segment par file et par passe relu pour
rien. Un segment n'est supprimé qu'une fois toutes ses possibilités dans la file de travail
(« peek puis commit »). Sans cette lecture, la part sur disque n'était jamais développée et
restait au fond de la pile, sous les enfants que l'expansion y empilait, servie en dernier.

Un segment n'est lu que s'il tient sous le plafond (environ 106 000 possibilités, une dizaine de
Mo en forme compacte) ; sinon la passe évince d'abord ses propres enfants pour lui faire de la
place, et à défaut le laisse à une passe suivante. Le plafond doit donc laisser la place d'un
segment au-dessus du stock résident. Une passe relit tout le disque, y compris ce qui a déjà
atteint le niveau visé et repart tel quel : une lecture et une écriture de plus par passe pour
ce stock-là, jusqu'à la passe qui ne développe plus rien.

**Suivre une expansion dans `events.log`.** Chaque passe écrit une ligne à son début, une toutes
les 5 minutes pendant, et une à sa fin :

```
expansion passe 3/10 (niveau visé 22) : 12400000 traitée(s) sur 250100000 (dont 0 lue(s) sur disque)
— 11800000 développée(s) dont 1200000 sans suite, 600000 réinjectée(s) telles quelles, 31200000 enfant(s) ;
file restante 237700000 ; résident 39100 Mo/42000 Mo ; disque 262000000 (+4100000 évincée(s),
+0 rechargée(s) depuis le point précédent) ; 2100 traitée(s)/s ; 5400 s écoulée(s)
```

(une seule ligne dans le journal). *Traitées* = développées + réinjectées ; le total grandit à
mesure que la passe lit le disque. *Sans suite* : parents sans aucun enfant, qui disparaissent.
Les compteurs du disque sont des écarts depuis la ligne précédente : une passe qui calcule sans
toucher au disque montre `+0`/`+0`, une passe freinée par le plafond montre des évictions et un
débit en baisse. Avant, une passe n'écrivait rien avant sa fin — 18 h de silence observées sur un
gros stock, sans pouvoir dire si le serveur calculait ou attendait.

**L'expansion s'arrête à la passe qui ne produit plus rien sous le niveau visé.** Avant, elle
s'arrêtait à la passe qui ne développait plus rien : il en fallait donc toujours une de plus, qui
relisait tout le stock (disque compris) et le réinjectait tel quel pour constater que le niveau
était atteint — une passe entière sur un stock de centaines de millions de possibilités. Une
passe suivante n'a lieu que si celle-ci a produit au moins une possibilité encore sous le niveau
visé, ou en a laissé de côté (plafond RAM, segment disque non lu, drainage interrompu).

**Un refus que le débordement résout sur-le-champ ne suspend plus la passe.** Seule une vraie
attente de place (débordement absent, en échec ou impuissant) suspend l'approfondissement
jusqu'à la passe suivante. Un stock sous plafond avec `--stock-spill-dir` bute sur le plafond
en permanence ; suspendre à chaque refus empêchait la passe d'aller au bout, et donc de lire le
disque. Ces refus résolus ne sont plus journalisés non plus : le journal en recevait un par
possibilité.

**Sans `--stock-max-ram` (illimité), cette option est acceptée mais reste inerte** : le
débordement n'a de sens que sous un plafond à respecter. Les segments emploient la même
forme compacte que les `.back` ([format compact](#format-compact)), mais à **pas FIXE** —
un enregistrement occupe toujours 390 octets, complété de zéros — là où un `.back` les
écrit à taille variable. Le débordement y gagne **-32 % d'espace disque** au lieu des
-88,7 % d'un `.back`, et c'est un arbitrage assumé : toute la sûreté du débordement
(« peek puis commit », troncature du segment de tête par décalage d'octets, « tout segment
sous le sommet est exactement plein ») est de l'arithmétique d'octets à pas constant, qu'un
enregistrement de taille variable remplacerait par un parcours arrière — sur le seul
mécanisme du projet dont le contrat est « aucune possibilité perdue ». Un cliché produit
avant ce format (manifeste `…-v1`) reste restaurable : ses segments sont **réencodés**
pendant la restauration, jamais liés directement. Un répertoire non inscriptible dégrade gracieusement (un
avertissement, le plafond RAM redevient un mur dur sans recours, jamais de blocage ni de
crash). Un pas immédiat est déclenchable via la commande console `spill [n]` ; l'occupation
déportée est visible via `GET /api/v1/stats` (`stock_spilled_packets`/`stock_spill_segments`,
voir [API HTTP REST admin](api_http_rest.md)).

**Le débordement survit à un `backup` suivi d'un `restore`** (console, HTTP admin,
autobackup, ou l'arrêt sur solution avec `--stop-on-solution`) : `backup` produit, en plus des
fichiers `.back` habituels, un **cliché** des segments dans `<--stock-spill-dir>/snapshot/`
(`snapshot-temp/` pour l'autobackup) — les segments **pleins** y sont dupliqués par lien
physique (`link()`, coût constant, aucune copie de données), seul le segment de **queue**
(encore mutable côté vivant) est copié. `restore` remet ces segments en place **avant**
d'importer le `.back` — un import qui déborderait ensuite (plafond plus bas, configuration
changée) **complète** ces segments au lieu de les écraser. Si `--stock-files` a changé entre
temps, chaque ancienne file `i` du cliché est reportée sur la file vivante `i %%
nb_file_possibility` ; en cas de collision (`--stock-files` réduit), les sources concernées sont
réempaquetées, jamais perdues.

**Un `import`/`restore` sous plafond RAM ne perd jamais rien.** `put_to_pool` REFUSE
(sans rien insérer) dès que le plafond est atteint ; l'import ATTEND alors qu'il y ait de
la place au lieu d'abandonner la possibilité, et **fait cette place lui-même** en pilotant
le débordement (`datamanager_set_ram_relief_hook`) plutôt qu'en subissant le tick du
thread de débordement — 4096 possibilités toutes les 100 ms, très en deçà de la cadence
d'un import en masse. L'attente n'est bornée que par l'arrêt (`REQUEST_STOP`), jamais par
un délai fixe : une configuration bloquée (plafond trop bas ET débordement indisponible)
cale VISIBLEMENT, un message toutes les 5 s, plutôt que de perdre des données en silence.
Si l'arrêt survient pendant l'attente, l'import s'interrompt en le signalant — le fichier
source est intact et rejouable, contrairement à une expansion dont les possibilités
n'existent nulle part ailleurs.

> Avant ce correctif, `import()` ignorait ce refus : restaurer 3 407 891 possibilités sous
> `--stock-max-ram 1024` en laissait 1 272 974 en RAM et 483 328 sur disque — **1 651 589
> évaporées sans le moindre message**. Restaurer sans plafond PUIS appliquer le plafond ne
> perdait rien, d'où un défaut longtemps invisible.

Que l'import pilote lui-même le débordement n'est pas qu'une question de cadence : c'est
désormais la seule issue. Un `restore` console gèle le débordement pendant TOUTE la
séquence (remise en place du cliché, puis vidage et réimport), pour qu'aucune éviction ni
rechargement concurrent ne fasse migrer une possibilité au beau milieu du remplacement.
Cette fenêtre ne tenait pas — l'état interne était un simple drapeau, que le
déverrouillage des files opéré par `restore` lui-même remettait à zéro avant l'import ; il
compte maintenant sa profondeur d'imbrication, si bien que la fenêtre va bien jusqu'au
bout. Le thread de débordement étant alors inerte, attendre son tick ne mènerait nulle
part : c'est l'import qui évince, entre deux de ses propres insertions.

**Une restauration incomplète du débordement est détectée et signalée en échec, jamais tolérée
en silence.** `backup` écrit, à côté du fichier de stock (`<fichier>.spillcount`), le nombre
exact de possibilités déportées à cet instant précis — indépendant du répertoire de débordement
lui-même. `restore` compare ce compte à ce qu'il a réellement récupéré depuis le cliché : en cas
d'écart (`--stock-spill-dir` oublié ou différent de celui utilisé à la sauvegarde, cliché
supprimé/corrompu…), la commande **échoue explicitement** (`log_error` nommant le nombre exact de
possibilités potentiellement perdues) plutôt que de rapporter un succès — le stock résident,
lui, revient tout de même (mieux vaut une restauration partielle mais honnêtement signalée que
rien du tout). Absence du fichier `.spillcount` (sauvegarde antérieure à cette vérification, ou
sans débordement actif ce jour-là) : rien à vérifier, pas une anomalie. Cette détection couvre
aussi un cliché *partiellement* endommagé — `manifest.txt` présent mais un ou plusieurs fichiers
`.dat` qu'il référence supprimés ou corrompus : ce cas précis est repéré directement, jamais
compté comme restauré.

> ⚠️ **Sans `restore` après un redémarrage, le débordement résiduel EST perdu.** Ce module
> lui-même n'a aucune conscience de la sauvegarde — au démarrage, tout segment résiduel d'un
> précédent processus est **purgé** (`log_error` explicite indiquant le nombre exact de
> possibilités supprimées), qu'un cliché existe ou non. `restore` (juste après le démarrage)
> remet ce cliché en place — voir ci-dessus. Sans `backup` préalable, il n'y a simplement rien à
> restaurer.
>
> **Limite assumée** : le cliché de débordement est **local à la machine** (chemin absolu du
> `--stock-spill-dir`) — un `.back` copié sur une autre machine ne restaure que la partie
> résidente, jamais le débordement, qui n'existe que sur le disque d'origine.

**Migration transparente d'`alloc` à la restauration (VERSION 13).** Depuis la bascule MRV
(moteur unique, [docs/conception/mrv_moteur_unique.md](conception/mrv_moteur_unique.md)),
`alloc` désigne le nombre de pièces posées, plus une position de curseur dans le parcours de
recherche. `restore`/`import` (fichiers `.back`, pool stock et pool analysé) et le rechargement
d'un segment de débordement disque recomptent systématiquement et inconditionnellement `alloc`
à la lecture — aucune détection de version de fichier, aucun marqueur de format : le recomptage
est idempotent sur un paquet déjà correct (écrit par du code v13), donc l'appliquer à tous les
paquets, anciens ou récents, donne le même résultat qu'une distinction explicite, sans logique de
version à maintenir. Aucun paquet n'est jamais rejeté, seulement réétiqueté si besoin — un `.back`
de production écrit avant cette bascule se restaure intégralement, sans perte.

Le champ `min_candidats` (score MRV, seconde coordonnée de `alloc` — voir
[docs/autosearch_step.md](autosearch_step.md)) suit une règle différente aux mêmes points de
lecture : contrairement à `alloc`, il ne se recalcule pas depuis la grille (il dépend de
l'historique de recherche), donc `restore`/`import`/le rechargement d'un segment de débordement
l'écrasent inconditionnellement par la sentinelle « inconnu » plutôt que de faire confiance à un
octet qui logeait dans le bourrage d'alignement du paquet avant l'introduction du champ. Aucune
perte de possibilité — seulement une difficulté redevenue « non mesurée » pour le stock restauré,
qui se remesure normalement au fil de son exploration.

Un pic d'expansion très rapide au démarrage (`--expand-level`) peut dépasser le plafond RAM
plus vite que le tick de 100 ms ne peut réagir. Aucune possibilité n'est perdue pour autant :
`expand_datas_to_level` **attend** que le débordement (ou un GET client) libère de la place
plutôt que d'abandonner, journalisant le refus initial puis un rappel toutes les 5 s tant que
l'attente se prolonge — un ralentissement au démarrage visible dans les logs, jamais une perte
silencieuse (voir [AGENTS.md § RAM cap & disk spillover](../AGENTS.md#ram-cap--disk-spillover)).

Exemple :
```sh
./eternityII server 80 --stock-max-ram 2048 --stock-spill-dir /var/lib/eternityii/spill data/pieces.csv
```

### Expansion du stock au démarrage (`--expand-level`, anti-famine)

Au démarrage, le serveur ne détient que le paquet *genèse* et ses tout premiers
enfants. Le premier client qui se connecte récupère cet unique arbre et le garde en
local ; le serveur se retrouve sans rien à distribuer aux autres clients, qui tournent
à vide — c'est la **famine du démarrage**.

L'option `--expand-level N` (position-indépendante, retirée d'argv avant l'analyse
positionnelle) demande au serveur de **développer lui-même son stock** avant toute
connexion : il pose, sur la case la plus contrainte choisie par MRV, une pièce
candidate de chaque possibilité jusqu'à ce que son nombre de pièces posées (`alloc`)
atteigne la cible `N`. Le paquet genèse devient ainsi des milliers de possibilités
distribuables. C'est un calcul **purement serveur, sans aucun impact client**.

L'expansion est bornée sur deux axes, tous deux configurables au lancement (dans
[src/app/app_static_variables.h](../src/app/app_static_variables.h)) : `--expand-max-levels N`
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

Si `--stock-max-ram` (ci-dessus) est également fixé et se révèle plus contraignant que
`--expand-max-stock`, l'expansion cesse d'approfondir dès que le plafond RAM est atteint — le
reste du travail en cours est réinjecté tel quel, au niveau déjà atteint, plutôt que développé
davantage. **Aucune possibilité générée n'est perdue** : un ADD qui bute sur le plafond RAM
**attend** (journalisé explicitement — refus initial puis rappel toutes les 5 s si l'attente se
prolonge, visible dans `events.log`) que `--stock-spill-dir` (ci-dessous) libère de la place,
plutôt que d'être abandonné. Une attente qui se prolonge signale un déséquilibre de
configuration (relever `--stock-max-ram`, configurer/vérifier `--stock-spill-dir`, ou réduire
`--expand-level`/`--expand-max-stock`), pas une perte de données.

**Une attente n'est pas forcément le plafond RAM, et le journal le dit.** Un ajout au stock
peut être refusé pour deux raisons sans rapport, toutes deux sûres à réessayer :

| Message | Cause | Ce qu'il faut faire |
|---|---|---|
| `plafond RAM atteint, possibilité mise en attente…` | `--stock-max-ram` serait dépassé | Relever le plafond, ou configurer/vérifier `--stock-spill-dir` |
| `stock momentanément indisponible (maintenance en cours…)` | Toutes les files du stock sont verrouillées : sauvegarde cohérente, tri, restauration, purge de descendants | **Rien** — l'attente se dénoue seule à la fin de la maintenance |

Le second cas survient **même sans aucun plafond**. Les confondre a coûté un faux diagnostic :
un `expand` sous plafond illimité journalisait « plafond RAM atteint […] relever
`--stock-max-ram` ou vérifier `--stock-spill-dir` » alors qu'aucun plafond n'était configuré et
qu'aucune possibilité ne pouvait déborder — l'attente était celle d'une sauvegarde automatique,
et elle s'est terminée d'elle-même après 28 s.

**Seul le plafond RAM suspend l'approfondissement d'une passe d'expansion.** Un verrou de
maintenance fait patienter, puis la passe reprend et va à son terme. Suspendre dans ce cas ne
protégeait de rien — le reste du travail de la passe est réinjecté par le même chemin
d'attente, donc il patiente autant — et coûtait un tour de `--expand-max-levels`, qui est un
budget de PASSES. Mesuré sur un stock de production de 3 407 891 possibilités : la même passe
traversant la même sauvegarde produisait 12 333 491 possibilités « réinjectées telles quelles »,
contre 13 291 686 menées à leur terme aujourd'hui.

**Le coût mémoire d'une passe est proportionnel au stock DÉJÀ présent, pas seulement à ce
qu'elle produit.** Chaque passe commence par drainer l'intégralité du pool non vérifié dans une
file de travail, qu'elle vide ensuite en reconstruisant le pool — c'est ce drainage qui garantit
qu'un enfant produit dans la passe n'est pas redéveloppé dans la même passe. Cette file de
travail range la **forme compacte**, comme les pools eux-mêmes ; tant qu'elle gardait des
`possibility_packet` entiers, elle matérialisait tout le stock au tarif brut le temps de la
passe.

Mesuré sur 2 000 000 de possibilités à 21 pièces posées (le profil moyen d'un stock de
production — 67 octets par enregistrement compact, 576 en paquet entier), pic de RSS relevé par
`getrusage` de part et d'autre du seul drainage :

| File de travail | Poids de la file | Pic de RSS ajouté | Rapporté à 42 496 015 possibilités |
|---|---|---|---|
| `possibility_packet` entiers | 1 152 Mo (576 o/poss.) | **+1 000 Mo** | ~21 Go |
| forme compacte | 134 Mo (67 o/poss.) | **+28 Mo** | ~0,6 Go |

Ce pic n'apparaît nulle part — ni dans les octets résidents affichés par `stock`, ni dans
`GET /api/v1/stats`, ni dans le calcul de `--stock-max-ram`, qui ne comptent que les deux pools.
Et l'allocateur ne rend pas ces octets au système une fois la passe finie : des blocs de ~600
octets repartent dans ses listes libres, pas en `munmap`, et le tas reste à son plus haut niveau
jusqu'au redémarrage du processus. Symptôme observé avant la correction : un serveur à ~30 Go de
RSS après quelques `expand` sur un stock qui, sauvegardé puis restauré dans un processus neuf,
en occupait 5,2 — d'où la tentation de conclure à une fuite alors que tout était bien libéré.
Un `expand` sur un très gros stock reste une opération qui demande de la RAM, simplement plus le
volume du stock lui-même.

> Cette expansion est le pendant *serveur* de la délégation anticipée côté *client*
> (sonde de faim `INST_NEED_WORK`, VERSION 8) décrite dans
> [Échanges client / serveur](echanges_client_serveur.md).

### Politique automatique de dosage (`--auto-roles`)

`--auto-roles` (serveur uniquement, **désactivée par défaut**) délègue au
serveur lui-même la décision que [`clientsRoles`](console.md) laisse
d'ordinaire à l'opérateur : ajuster `pruner_forks`
([`--pruner-forks`](#dosage-recherchecontrôle-par-fork---pruner-forks),
ci-dessous) diffusé à tout le parc connecté, en fonction du besoin mesuré.

```sh
./eternityII server 80 --auto-roles data/pieces.csv
```

Sans cette option, aucun comportement ne change : l'opérateur garde
entièrement la main via `clientsRoles`. Une fois activée, le serveur ajuste
lui-même le dosage à chaque tour de statistiques existant (10 s, aucune
cadence dédiée), à partir de quatre signaux déjà mesurés :

- la **famine par rôle** (`server_search_starved`/`server_prune_starved`) —
  signal le plus direct et le plus urgent ;
- la **taille des deux pools de stock** (non vérifié / vérifié) — un excès de
  non-vérifié signale trop peu de pruners (le problème d'origine : jusqu'à
  50 % de stock mort distribué sans pruner) ;
- la **pression sur `--stock-max-ram`** (ci-dessus), si configuré ;
- le **parc connecté compté par rôle**, pondéré par `nb_forks` et non par un
  simple compte de sessions (`control_registry_count_role_forks`).

Deux garde-fous fixes, non désactivables : le dosage **n'augmente jamais**
tant qu'il ne reste qu'un seul chercheur connecté (jamais 0 chercheur / jamais
100 % pruner — sans producteur, le stock ne se régénère plus), et un
**délai minimal (~2 minutes) entre deux changements effectifs** — chaque
changement coûte un redémarrage des fils (`stopForks` + re-fork) chez
chaque client visé, le même coût qu'un `clientsRoles` manuel. Chaque
ajustement se fait par pas de ±1, jamais un saut direct vers une cible
calculée.

Une décision manuelle (`clientsRoles`) reste possible en parallèle : les deux
mécanismes partagent le même dosage désiré persistant par machine, la
politique automatique pouvant le remplacer à son prochain tour si les
signaux le justifient. Détail des règles de décision et des seuils :
[Politique automatique de dosage recherche/contrôle](echanges_client_serveur.md#politique-automatique-de-dosage-recherchecontrôle).

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
| `--shallow-root-abandon-depth N` | *(absent, 0 = désactivé)* | Abandonne une racine reçue trop peu profonde une fois creusée jusqu'à `N` pièces posées — voir [ci-dessous](#option---shallow-root-abandon-depth-client-et-pruner) |

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

## Dosage recherche/contrôle par fork (`--pruner-forks`)

Un process client/pruner héberge `nb_threads` forks de travail ; par défaut, ils
partagent tous le même rôle, impliqué par le mode de lancement (`client` → tous
cherchent, `pruner` → tous contrôlent). L'option `--pruner-forks <n>` permet de
**mélanger les deux rôles au sein d'un même process** : `n` forks (parmi
`nb_threads`) sont affectés au CONTRÔLE du stock (comme un pruner), les autres
cherchent (comme un client) — le serveur, seul acteur à connaître le besoin
réel en temps réel, peut ainsi ajuster ce dosage à distance
(`clientsRoles`/`--auto-roles`, voir
[Échanges client/serveur](echanges_client_serveur.md#dosage-recherchecontrôle-par-fork-piloté-à-distance-clientsroles)),
sans redéployer le client.

```sh
./eternityII client [--pruner-forks N] [serveur] [nb_threads] [max_stock_par_thread] [fichier_pieces.csv]
```

| Valeur | Effet |
|---|---|
| *(absente)* | Comportement historique inchangé : tous les forks partagent le rôle impliqué par le mode (`client`/`pruner`) |
| `0` | Tous les forks cherchent — équivalent au mode `client` sans l'option |
| `nb_threads` | Tous les forks contrôlent — équivalent au mode `pruner` sans l'option |
| `1..nb_threads-1` | Dosage mixte : les `n` forks de plus haut rang contrôlent, les autres cherchent |

Une valeur hors `[0, nb_threads]` est clampée plutôt que rejetée. Le rôle
est fixé une fois pour toutes à la naissance de chaque fork (pas de bascule à
chaud) : changer le dosage passe par `config pruner_forks <n>` +
`configApply` (ou la clé `pruner_forks` du `--config-file`), qui redémarrent
les fils comme un changement de `nb_forks`.

> **Incompatible avec `--gpu`** dès que la valeur diffère de `nb_threads` : le
> contexte CUDA n'est initialisé qu'une seule fois par process et son
> déclenchement ne consulte pas le rôle par fork — un dosage mixte ferait donc
> tourner CHAQUE fork sur le pruner GPU, jamais sur la recherche. Le lancement
> échoue avec une erreur explicite plutôt que d'ignorer silencieusement le
> dosage demandé. Même garde-fou sur `configApply` (redémarrage à chaud) :
> un client GPU refuse (`log_error`, aucun re-fork) toute configuration en
> préparation qui rendrait `pruner_forks` différent de `nb_forks` une fois
> appliquée — y compris quand elle est poussée à distance par
> `clientsRoles`/`--auto-roles` (voir
> [Dosage recherche/contrôle par fork, piloté à distance](echanges_client_serveur.md#dosage-recherchecontrôle-par-fork-piloté-à-distance-clientsroles)) :
> un client `pruner --gpu` reste donc exclu de tout pilotage dynamique du
> dosage, quelle qu'en soit la source.

Exemples :
```sh
./eternityII client srv 8 --pruner-forks 2       # 6 forks cherchent, 2 contrôlent
./eternityII pruner srv 8 data/pieces.csv --pruner-forks 4   # 4 forks contrôlent, 4 cherchent
```

**Visibilité côté client.** La commande console `check` affiche, dans le
tableau « Thread queues », une colonne `Type` (`search`/`prune`) par fork —
le rôle EFFECTIF, calculé sans la moindre I/O (le parent connaît déjà le
dosage résolu de chaque fork). Un rôle reste valide pour toute la durée
d'affichage jusqu'au prochain `configApply` :

```
Thread queues
Fork | Type   |     In stock |     Analysed
-----+--------+--------------+-------------
   0 | search |          120 |            3
   1 | search |           98 |            2
   2 | prune  |            0 |           10
   3 | prune  |            0 |            8
-----+--------+--------------+-------------
Total|        |          218 |           23
```

**Visibilité côté serveur.** La console `clients`/`GET /api/v1/clients`
n'expose qu'un seul `mode` PAR SESSION (celui du mode de lancement
`client`/`pruner` du process, jamais le détail d'un dosage mixte). Le rôle
PAR FORK, lui, est visible via `clientsWork <cible>` (console ou `POST
/api/v1/command`) : le serveur retient déjà, par connexion de travail,
l'identité déclarée sur son `INST_CLIENT_HELLO` (`fork_seq`/`mode`) —
`clientsWork` ajoute cette liste à sa réponse habituelle (attribution du pool
analysé) :

```
clientsWork mixed-client
clientsWork (API HTTP admin) : mixed-client (client_uid=…) : 2 possibilite(s)
en cours d'analyse, alloc max=6 ; forks: 0=search 1=search 2=prune 3=prune
```

## Mode pruner (élagage)

Un **pruner** réutilise la même plomberie réseau qu'un client, mais au lieu d'explorer
il demande au serveur des possibilités *à vérifier* et élague celles qui n'ont aucune
continuation possible. Deux variantes :

**Sans aucun pruner en service, un serveur accumule du travail déjà mort.** Mesuré sur
un stock réel (`tests/bench/bench_refutation.c --pruner-profile`, voir
[docs/tests_et_ci.md](tests_et_ci.md#mode---pruner-profile--rejoue-le-vrai-pipeline-du-pruner)) :
le seul contrôle superficiel qu'un pruner exécute (`possibility_all_has_a_next_counted`,
gratuit, et seul contrôle exécuté si `prunerDfsBudget` est ramené à `0`)
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

**Ce réglage ne gouverne que l'ALLER et l'acquittement.** Le RETOUR — les
possibilités jugées vivantes, redéposées dans le stock du serveur — a sa propre
instruction de lot depuis le protocole v14 (`INST_ADD_BATCH`), avec sa propre
borne (`ADD_BATCH_MAX`, 1024 possibilités par trame) et sans réglage
utilisateur. Le rappeler a son importance : tant que ce retour se faisait
possibilité par possibilité, augmenter `prunerBatch` n'avait **aucun** effet
mesurable sur le débit d'un pruner (15 183 contre 15 254 possibilités vérifiées
par minute, de 100 à 1000). Depuis la v14, le réglage agit réellement — c'est
lui qui amortit l'attente entre deux lots, devenue le facteur limitant. Mesures :
[Dépôt par lot](echanges_client_serveur.md#dépôt-par-lot-inst_add_batch-v14).

Exemples :
```sh
./eternityII pruner localhost 4 data/pieces.csv 500     # lots de 500 (CPU)
./eternityII pruner --gpu localhost 1 data/pieces.csv 4096    # lots de 4096 (GPU)
```

### Preuve de fermeture bornée (`prunerDfsBudget`)

Au-delà du contrôle superficiel gratuit ci-dessus, un pruner peut tenter de **prouver**
qu'une possibilité est morte, en rejouant réellement son sous-arbre avec un plafond de
nœuds — `prunerDfsBudget <n>` (commande console, clé `dfs_budget` du fichier de
configuration client, pilotable à distance par `clientsCommand` et l'API HTTP).
**Activé par défaut à `10000`** depuis la mesure en conditions réelles de
[§4.6c](conception/elagage_recherche.md) : sur un stock de production de 32 480
possibilités remis au pool non vérifié, un pruner à 3 forks en élimine **22 006 (67,8 %) en
moins de 5 minutes**, palier reproduit à l'identique sur une seconde machine. Le défaut
valait `0` jusque-là, faute exactement de cette confirmation hors banc.

La valeur est un arbitrage, pas un optimum : la courbe budget/fermeture ne plafonne pas sur
ce stock (56,4 % à `1000`, 66,4 % à `10000`, 73,2 % à `100000`), contrairement à celle du
stock de §4.10 plus bas, qui plafonnait dès `1000`. Le point de fonctionnement dépend donc
du stock. `1000` reste un choix conservateur défendable, `0` désactive toujours le
mécanisme sans le moindre coût.

> **Le défaut ne traite que le FLUX.** Une possibilité déjà marquée `checked` n'est jamais
> resoumise à la preuve, quel que soit le budget. Sur un serveur dont le stock est déjà
> constitué, il faut lancer **une fois** la commande console `resetChecked` après
> déploiement pour que le passif repasse devant les pruners — sans quoi le nouveau défaut
> n'aura aucun effet visible. Voir [console.md](console.md).

Cette preuve emploie MRV, le seul moteur de backtracking depuis
[docs/conception/mrv_moteur_unique.md](conception/mrv_moteur_unique.md) (PR3) — un ancien
sélecteur opt-in (`pruner_dfs_mrv`/`ETII_PRUNER_DFS_MRV=1`) a existé le temps de mesurer ce
levier face à l'ordre fixe historique, puis a disparu avec ce dernier une fois la mesure
favorable établie : ×3 à ×4 de fermetures à budget égal (≈ 4× plus de sous-arbres fermés par
seconde de CPU sur du stock réel).

Mesuré sur un stock de production de 126 287 possibilités (échantillon de 2 000, voir
[docs/tests_et_ci.md](tests_et_ci.md#ab-historique-du-moteur-de-la-preuve-410--moteur-unique-depuis-pr3)) :
à budget 1 000, l'ancien ordre fixe fermait 8,3 % des possibilités contre **34,8 %** pour
MRV — 30 % contre **57 %** de stock éliminé au total, contrôle superficiel compris. Et
l'écart ne se rattrapait pas en payant : à budget 100 000 (×100 de CPU), l'ordre fixe
n'atteignait que 33,8 %, soit toujours moins que MRV à budget 1 000 pour 10,7× plus de
temps.

```sh
# machine dédiée à l'élagage : la preuve bornée est active d'emblée (10000)
./eternityII pruner serveur 8 data/pieces.csv 500
# pour un autre point de fonctionnement, dans sa console (ou à distance) :
prunerDfsBudget 1000
# et UNE FOIS, côté serveur, pour soumettre le passif déjà vérifié :
resetChecked
```

> À vérifier pour choisir un autre budget : le profil de profondeur du stock du serveur
> (`GET /api/v1/stock-distribution`). Et à garder en tête : élaguer profite surtout aux
> AUTRES machines (le stock est distribué à toute la flotte) — un client qui tourne
> lui-même en MRV refait déjà, à chaque nœud, l'essentiel du contrôle d'un pruner.

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

## Option `--shallow-root-abandon-depth` (client et pruner)

`max_stock_by_thread` (ci-dessus) borne le stock implicite d'un thread, mais peut
ne **jamais** se déclencher sur une racine reçue à faible profondeur : si le
branchement MRV est fin (peu de candidats par case), le stock implicite reste
petit alors que le sous-arbre total de la racine reste énorme — le thread peut y
rester des heures sans jamais rendre le moindre paquet. `--shallow-root-abandon-depth
N` (défaut `0`, désactivé) ajoute un second critère : quand la racine REÇUE (profondeur
au moment du `GET`) est sous `N` et que la profondeur COURANTE de l'étude l'atteint,
tout le travail restant est rendu au serveur (même mécanisme que l'arrêt propre) et
le thread se repositionne sur une nouvelle racine.

```sh
./eternityII client --shallow-root-abandon-depth 128 localhost 4
```

Réglable aussi à chaud (console `shallowRootAbandonDepth <n>`) ou via la clé
`shallow_root_abandon_depth` du `--config-file` (`config`/`configApply` — clé à
chaud, pas de redémarrage nécessaire). **Opt-in, à calibrer par la mesure** avant
d'envisager un défaut actif : le compteur cumulatif `shallow_root_abandoned`,
affiché par la console `statistic` (« racines abandonnées (profondeur < N) »),
sert précisément à ça. Détail du mécanisme et rationale de la valeur par défaut
(0, jamais une valeur devinée) : [autosearch_step.md §1.5bis](autosearch_step.md#15bis-abandon-dune-racine-trop-peu-profonde-shallow_root_abandon_depth).

## Canal de contrôle et pilotage à distance

Chaque processus client ouvre automatiquement une seconde connexion TCP vers le
serveur, sur laquelle le **serveur** devient l'initiateur : demande de statistiques,
poussée de commandes (`pause`, `resume`, `limit`, …), récupération du meilleur plateau
connu. Ça se pilote depuis la console du serveur (`clients`, `clientsStats`,
`clientsCommand`, `pause`/`resume` — voir [Console interactive](console.md)) ou via
l'[API HTTP REST admin](api_http_rest.md) (`--http-port`). Détails du protocole :
[Canal de contrôle](echanges_client_serveur.md#canal-de-contrôle-v9-étendu-en-v10-et-v12).

## Format du fichier de pièces

```
ntiles: 256
<id> <top> <left> <bottom> <right>
...
```

- Chaque pièce est définie par son identifiant et les 4 couleurs de ses bords (entiers).
- La valeur `0` représente la bordure grise (bord du puzzle).
- Le fichier `data/pieces.csv` contient les 256 pièces officielles du puzzle 16×16.
- Le fichier `data/pieces16.csv` contient 16 pièces pour un puzzle 4×4 (tests rapides,
  nécessite un build `ETERN_PARTS=16` — voir [Compilation](compilation.md#configuration-du-puzzle)).
- Les **clones à solution connue** (`tools/gen_clone.py`, tailles 8×8 à 14×14) suivent
  exactement ce format ; ils ne sont pas versionnés, ils se régénèrent à partir d'une
  graine. Voir [Compilation](compilation.md#tailles-supportées).

## Option `--indices-file` (indices imposés de l'instance)

```sh
./eternityII test data/clones/pieces_10_17_1.csv --indices-file data/clones/indices_10_17_1.csv
```

Acceptée par tous les modes, à n'importe quelle position, comme
`--stop-on-solution`. Désigne le fichier d'indices posés sur la **genèse**
(`first_possibility`), avant toute expansion :

```
nindices: 5
<id> <x> <y> <rotation> <mandatory>
...
```

`x` est la colonne, `y` la ligne ; `rotation` est l'indice de rotation du moteur
(`rotatePart`) ; `mandatory` distingue l'indice géométrique, toujours posé, des
indices de coin (ignorés par un build `ETERN_WITH_INDICES=0`).

| Taille | Défaut sans l'option |
|---|---|
| 16×16 | `./data/indices.csv` — les cinq indices officiels |
| toute autre | **aucun indice** : la genèse part du plateau vide |

Un serveur et ses clients doivent partir du **même** fichier d'indices, exactement
comme du même fichier de pièces : les possibilités échangées sur le fil supposent la
même genèse. Un chemin introuvable ou malformé est une **erreur fatale au démarrage** —
jamais une genèse silencieusement sans indice.

L'option existe pour les clones, qui portent chacun les leurs
(`indices_<n>_<k>_<graine>.csv`, produit par `tools/gen_clone.py --hints 5`).

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

Les variantes `temp*.back` sont celles qu'écrit la sauvegarde **automatique** du
serveur, toutes les ~60 s ; [`--no-autobackup`](#sauvegarde-automatique-périodique---no-autobackup)
la supprime, et ces fichiers cessent alors d'être mis à jour (les `eternityII*.back`,
écrits par la commande `backup` et par l'arrêt sur solution, ne sont pas concernés).

Ces fichiers permettent de reprendre une recherche interrompue avec la commande
`restore` (voir [Console interactive](console.md)).

#### Format compact

Un `.back` porte un **en-tête de 32 octets** (magie `ETIISTK`, version, géométrie
compilée `ETERN_SIZE`/`ETERN_PARTS`) suivi d'**enregistrements de taille variable**,
sérialisés champ par champ — jamais un `fwrite` de la structure, qui embarquerait son
bourrage d'alignement. Le format et ses invariants sont dans
[src/core/packet_codec.h](../src/core/packet_codec.h), les mesures et les formes
écartées dans [Forme compacte d'une possibilité](format_stock_compact.md).

Une possibilité y pèse **65 octets en moyenne au lieu de 576**, et jamais plus de 390
quel que soit le remplissage du plateau. Mesuré sur un stock de production réel
(`eternityII.back`, 3 407 891 possibilités) : **1 963 Mo → 222 Mo, soit x8,83**. Le gain
vient de ce qu'une possibilité du stock a 19,2 cases remplies sur 256 en moyenne, et que
deux de ses champs (`alloc`, `b_faceused`) sont intégralement déductibles de la grille —
vérifié sans une exception sur ces 3,4 M possibilités.

Ce n'est pas qu'un gain de disque : l'écriture d'un `.back` de stock se fait sous verrou
(file par file pour `consistent_backup`), donc neuf fois moins d'octets à écrire, c'est
neuf fois moins de temps pendant lequel un client attend — la préoccupation même de la
série « gestion de charge » ([échanges client/serveur](echanges_client_serveur.md#gestion-de-charge)).

**Les `.back` écrits avant ce format restent lisibles**, sans rien à faire : la détection
se fait sur la magie, et un fichier qui n'en porte pas est relu au pas de 576 octets
comme avant. L'inverse n'est pas vrai — un `.back` produit maintenant n'est pas relisible
par un binaire antérieur. Un fichier qui porte la magie mais une version ou une géométrie
incompatibles est **refusé bruyamment**, jamais réinterprété : c'est précisément ce qu'un
format sans en-tête ne pouvait pas faire (un `.back` de puzzle 4x4 relu par un binaire
16x16 produisait des plateaux absurdes en silence).

### Journal et solutions

| Fichier | Contenu |
|---|---|
| `events.log` | Journal des évènements horodatés (nouveaux records, solutions, etc.), **des erreurs** (`log_error`/`log_errno`, ex. écriture de fichier échouée), **de la configuration effective de démarrage** (client/pruner et serveur — jamais affichée sur la console, uniquement dans ce fichier), ainsi que **du résultat des commandes de vérification, des actions de cycle de vie (start/stopForks/configApply), du pilotage distant et des transitions de débordement disque** — voir [Console interactive](console.md#zone-events-en-bas-de-lécran) pour la liste complète. Append-only. |
| `solution_<pid>_<seq>` | Plateau sérialisé quand une solution complète est trouvée (déclenche aussi un évènement). |

### Sockets Unix de l'IPC parent↔fork

| Fichier | Contenu |
|---|---|
| `etii_main.<pid>` | Socket AF_UNIX du process parent (client/pruner) : y arrivent les statistiques, les logs et les records de ses forks |
| `etii_fork.<pid>` | Socket AF_UNIX d'un process de recherche : y arrivent les commandes console propagées par le parent |

Ce ne sont pas des fichiers ordinaires mais des **fichiers spéciaux**, créés par
`bind()` et supprimés à la terminaison du process qui les possède (`atexit`, cf.
[Architecture](architecture.md#cycle-de-vie-des-fichiers-socket)). Un `SIGKILL`
les laisse en revanche derrière lui : ils sont alors sans effet (le prochain
démarrage utilise un `<pid>` différent, et un `bind()` sur un même nom écrase
le résidu), mais `git status` ne les montre pas — git ne suit pas les fichiers
spéciaux. Ils peuvent surprendre un outil qui parcourt le répertoire de travail :
`cp -R`, par exemple, s'arrête net dessus (« Operation not supported »).

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
  `NO_WORK_SLEEP_MAX` dans [src/app/app_static_variables.h](../src/app/app_static_variables.h)),
  afin de distinguer une pénurie ponctuelle d'un épuisement long/définitif — par
  opposition aux pauses (régulation `REQUEST_PAUSE` / admin `REQUEST_ADMIN_PAUSE`),
  qui bénéficient déjà chacune d'une cadence dédiée (`PAUSE_POLL_SLEEP_US` /
  `ADMIN_PAUSE_POLL_SLEEP_US`).

## Voir aussi

- [Console interactive](console.md) — commandes interactives et interface.
- [Échanges client / serveur](echanges_client_serveur.md) — protocole TCP et canal de contrôle.
- [API HTTP REST admin](api_http_rest.md) — télémétrie et pilotage HTTP du serveur.
- [Pruner GPU (CUDA)](pruner_gpu_cuda.md) — mode `pruner --gpu` en détail.

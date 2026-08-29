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
./eternityII server [nb_threads] [--expand-level N] [--expand-max-stock N] [--expand-max-levels N] [--stock-files N] [--rebalance-budget N] [--stock-max-ram N] [--stock-spill-dir CHEMIN] [--auto-roles] [--http-port N] [--http-token-file CHEMIN] [fichier_pieces.csv]
```

| Paramètre | Défaut | Description |
|---|---|---|
| `nb_threads` | 80 | Nombre de connexions clients simultanées |
| `--expand-level N` | *(absent)* | Développe le stock au démarrage jusqu'à `N` pièces posées (anti-famine, voir ci-dessous) |
| `--expand-max-stock N` | `EXPAND_MAX_STOCK` (100000) | Plafonne en NOMBRE de possibilités la pré-expansion `--expand-level` (voir ci-dessous) ; sans effet si `--expand-level` est absent |
| `--expand-max-levels N` | `EXPAND_MAX_LEVELS` (4) | Plafonne en NOMBRE DE PASSES la pré-expansion `--expand-level` (voir ci-dessous) ; sans effet si `--expand-level` est absent |
| `--stock-files N` | `NB_FILE_POSSIBILITY_DEFAULT` (10) | Nombre de files de stock, fixé une seule fois au démarrage (jamais à chaud), plafonné à `NB_FILE_POSSIBILITY_MAX` (128) — voir ci-dessous |
| `--rebalance-budget N` | `REBALANCE_BUDGET_DEFAULT` (1000) | Nombre de possibilités rééquilibrées entre files à chaque tour serveur (10 s) — voir ci-dessous |
| `--stock-max-ram N` | *(absent, illimité)* | Plafond en Mo des DEUX pools de stock (non vérifié + vérifié) — voir ci-dessous |
| `--stock-spill-dir CHEMIN` | `./eternityii-spill` | Répertoire de débordement sur disque une fois `--stock-max-ram` approché — voir ci-dessous |
| `--auto-roles` | *(absente, désactivée)* | Active la politique automatique de dosage recherche/contrôle du parc — voir ci-dessous |
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
./eternityII server 80 --stock-max-ram 4096 data/pieces.csv
./eternityII server 80 --stock-max-ram 4096 --stock-spill-dir /var/lib/eternityii/spill data/pieces.csv
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
> [canal de contrôle](echanges_client_serveur.md#canal-de-contrôle-v9) qui occupe un
> slot du **même** pool. Un serveur dimensionné au plus juste doit compter
> (connexions de travail simultanées) **+** (processus clients connectés), pas
> seulement le premier terme. Le défaut (80) laisse une large marge.

### Plafond RAM du stock (`--stock-max-ram`)

Rien ne bornait auparavant la croissance du stock serveur au fil du trafic de recherche et de
délégation : un stock de plusieurs millions de possibilités peut consommer plusieurs Go de RAM
(`sizeof(struct possibility_packet)` = 576 octets sur le puzzle 256 pièces, plus le coût réel
d'allocation — `Element` de la liste chaînée + deux `malloc()` par possibilité stockée, cf.
`core/lifo.c` — porte le coût réel à environ 632 octets/possibilité).

`--stock-max-ram N` fixe un plafond en **Mo**, converti **une seule fois** en NOMBRE de
possibilités au démarrage (l'unité réellement comparée à chaque ajout). Ce plafond couvre les
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

**Sans `--stock-max-ram` (illimité), cette option est acceptée mais reste inerte** : le
débordement n'a de sens que sous un plafond à respecter. Le format des segments est identique
à celui des fichiers `.back` (un flux brut de possibilités, sans en-tête) — un opérateur peut
les inspecter avec les mêmes outils. Un répertoire non inscriptible dégrade gracieusement (un
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

- la **famine par rôle** (`server_search_starved`/`server_prune_starved`,
  PR2 de [docs/conception/pilotage_type_client.md](conception/pilotage_type_client.md)) —
  signal le plus direct et le plus urgent ;
- la **taille des deux pools de stock** (non vérifié / vérifié) — un excès de
  non-vérifié signale trop peu de pruners (le problème d'origine : jusqu'à
  50 % de stock mort distribué sans pruner) ;
- la **pression sur `--stock-max-ram`** (ci-dessus), si configuré ;
- le **parc connecté compté par rôle** (`control_registry_count_roles`).

Deux garde-fous fixes, non désactivables : le dosage **n'augmente jamais**
tant qu'il ne reste qu'un seul chercheur connecté (jamais 0 chercheur / jamais
100 % pruner — sans producteur, le stock ne se régénère plus), et un
**délai minimal (~2 minutes) entre deux changements effectifs** — chaque
changement coûte un redémarrage des fils (`stopForks` + re-fork) chez
chaque client visé, le même coût qu'un `clientsRoles` manuel. Chaque
ajustement se fait par pas de ±1, jamais un saut direct vers une cible
calculée.

Une décision manuelle (`clientsRoles`) reste possible en parallèle : les deux
mécanismes partagent le même dosage désiré persistant par machine (PR3), la
politique automatique pouvant le remplacer à son prochain tour si les
signaux le justifient. Détail des règles de décision et des seuils :
[Politique automatique de dosage recherche/contrôle](echanges_client_serveur.md#politique-automatique-de-dosage-recherchecontrôle-pr4).

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

## Dosage recherche/contrôle par fork (`--pruner-forks`)

Un process client/pruner héberge `nb_threads` forks de travail ; par défaut, ils
partagent tous le même rôle, impliqué par le mode de lancement (`client` → tous
cherchent, `pruner` → tous contrôlent). L'option `--pruner-forks <n>` permet de
**mélanger les deux rôles au sein d'un même process** : `n` forks (parmi
`nb_threads`) sont affectés au CONTRÔLE du stock (comme un pruner), les autres
cherchent (comme un client) — voir
[docs/conception/pilotage_type_client.md](conception/pilotage_type_client.md)
pour le contexte complet.

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
> dosage demandé.

Exemples :
```sh
./eternityII client srv 8 --pruner-forks 2       # 6 forks cherchent, 2 contrôlent
./eternityII pruner srv 8 data/pieces.csv --pruner-forks 4   # 4 forks contrôlent, 4 cherchent
```

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

### Preuve de fermeture bornée (`prunerDfsBudget`)

Au-delà du contrôle superficiel gratuit ci-dessus, un pruner peut tenter de **prouver**
qu'une possibilité est morte, en rejouant réellement son sous-arbre avec un plafond de
nœuds — `prunerDfsBudget <n>` (commande console, clé `dfs_budget` du fichier de
configuration client, pilotable à distance par `clientsCommand` et l'API HTTP).
**Désactivé par défaut (`0`)** : c'est un coût CPU que l'opérateur engage sciemment.
Valeur recommandée par la mesure : **`1000`** — le gain plafonne au-delà (voir ci-dessous),
et le `10000` de la première mesure de §4.6b ne se justifie plus sur un stock de production.

Cette preuve emploie MRV, le seul moteur de backtracking depuis
[docs/conception/mrv_moteur_unique.md](conception/mrv_moteur_unique.md) (PR3) — un ancien
sélecteur opt-in (`pruner_dfs_mrv`/`ETII_PRUNER_DFS_MRV=1`) a existé le temps de mesurer ce
levier face à l'ordre fixe historique, puis a disparu avec ce dernier une fois la mesure
favorable établie : ×3 à ×4 de fermetures à budget égal (≈ 4× plus de sous-arbres fermés par
seconde de CPU sur du stock réel).

Mesuré sur un stock de production de 126 287 possibilités (échantillon de 2 000, voir
[docs/tests_et_ci.md](tests_et_ci.md#option---pruner-dfs-mrv--lab-du-moteur-de-la-preuve-410)) :
à budget 1 000, l'ancien ordre fixe fermait 8,3 % des possibilités contre **34,8 %** pour
MRV — 30 % contre **57 %** de stock éliminé au total, contrôle superficiel compris. Et
l'écart ne se rattrapait pas en payant : à budget 100 000 (×100 de CPU), l'ordre fixe
n'atteignait que 33,8 %, soit toujours moins que MRV à budget 1 000 pour 10,7× plus de
temps.

```sh
# machine puissante dédiée à l'élagage : preuve bornée activée
./eternityII pruner serveur 8 data/pieces.csv 500
# puis, dans sa console (ou à distance) :
prunerDfsBudget 1000
```

> À vérifier avant d'activer : le profil de profondeur du stock du serveur
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
| `events.log` | Journal des évènements horodatés (nouveaux records, solutions, etc.), **des erreurs** (`log_error`/`log_errno`, ex. écriture de fichier échouée) **et de la configuration effective de démarrage** (client/pruner et serveur — jamais affichée sur la console, uniquement dans ce fichier). Append-only ; voir [Console interactive](console.md#zone-events-en-bas-de-lécran). |
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

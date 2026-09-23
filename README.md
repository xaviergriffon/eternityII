# eternityII

[![codecov](https://codecov.io/gh/xaviergriffon/eternityII/branch/master/graph/badge.svg?token=AAHN9LCWFR)](https://app.codecov.io/gh/xaviergriffon/eternityII)

Solveur distribué pour le puzzle [Eternity II](https://fr.wikipedia.org/wiki/Eternity_II).

Le puzzle consiste à placer 256 pièces carrées sur une grille 16×16 en faisant correspondre les motifs sur les bords adjacents. L'espace de recherche étant astronomique, le programme exploite une architecture client-serveur pour distribuer le travail sur plusieurs processus, voire plusieurs machines.

## Architecture

```
┌─────────────────────┐        TCP         ┌─────────────────────────────┐
│      Serveur        │◄──────────────────►│  Client (processus parent)  │
│   (distribue les    │                    │  ┌──────────┬──────────┐    │
│   possibilités)     │                    │  │ fork #1  │ fork #2  │... │
└─────────────────────┘                    │  └──────────┴──────────┘    │
                                           └─────────────────────────────┘
```

- Le **serveur** maintient le stock de positions de plateau (« possibilités ») à explorer et les distribue aux clients.
- Chaque **client** forke `N` processus de recherche ; des **pruners** (CPU ou GPU) vérifient en parallèle les possibilités et élaguent les branches mortes.
- Le serveur peut **piloter les clients à distance** (statistiques, pause/reprise) via un canal de contrôle dédié, et exposer une **API HTTP REST** pour la supervision.
- Un client peut aussi tourner en mode **autonome** (`test`), sans serveur.
- Le forward-checking de la boucle chaude inspecte les **voisines géométriques** de la pièce qu'on vient de placer (au plus 4) plutôt qu'une fenêtre de parcours : **+68,8 % de nœuds/s**, taux d'élagage quasi inchangé.
- La boucle de recherche lit sa table de candidats via un **index compact** (4 octets par compartiment au lieu de 16), qui divise par 3,8 le volume balayé par le forward-checking : **+10 % de nœuds/s** sur un worker, **+29 %** sur 16 workers concurrents.
- Cette table étant en lecture seule une fois construite, le parent la construit **avant de forker** : les processus de recherche s'en partagent **une seule copie** (copy-on-write) au lieu d'en fabriquer chacun la leur — **−90 % d'empreinte mémoire** à 16 workers (111 → 11 Mo de `Pss`), pour un débit inchangé.
- Chaque compartiment de la table de candidats est trié **à sa construction** par rareté croissante de couleur exposée (la pièce la plus rare essayée en premier) : coût nul dans la boucle chaude, **+3,2 % de nœuds/s** mesuré.
- Le balayage MRV **mémorise le pointeur de masque de chaque case** au lieu de le re-résoudre à chaque nœud (la clé d'une case ne change que si une voisine bouge) : 54,5 résolutions par nœud ramenées à ~13. Il **compare ensuite les cases sur une seule clé entière** (`count` puis côtés contraints empilés en poids fort/faible) au lieu de trois tests chaînés, ce qui supprime des sauts que le processeur ne pouvait pas prédire. Ensemble, à arbre exploré identique : **×1,35 en nœuds/s, IPC 2,56 → 3,15** (Xeon E5-2640 v4, `perf stat`, 9 répétitions alternées). Les deux tables du chemin chaud (`packed` + `bucket_id_mask`, 1 746 Kio) tiennent par ailleurs dans une seule grande page de 2 Mio.
- Le balayage MRV et le forward-check ont un **chemin rapide « masques complets »** : sur une map de production, l'index compact et le masque d'ids existent avec une largeur connue à la compilation, ce qui est décidé **une fois par recherche** au lieu d'une fois par case — plus de test NULL, plus de rechargement de la largeur, plus de boucle de mots à borne dynamique (ces trois choses faisaient ~35 des ~57 instructions par case de frontière). Départage et position sont précalculés par la frontière et empilés avec le compte dans **une seule valeur** dont le minimum est la règle MRV, et la case morte se lit après la réduction. Mesuré à arbre identique : **instructions par nœud −37 %, mauvaises prédictions −47 %, ×1,26 en nœuds/s** (Xeon E5-2630 v4, `perf stat`, rondes alternées). Voir [docs/autosearch_step.md](docs/autosearch_step.md#13-quater-mrv_choose_cell--la-case-la-plus-contrainte-pas-la-suivante-du-parcours), points 8 à 10 — et le tableau des pistes mesurées et écartées.
- Les compteurs de prunage de la boucle chaude ne sont plus incrémentés par une opération atomique verrouillée : dans un processus donné, **un seul thread les écrit** (les workers de recherche sont des *forks*, chacun avec sa copie), si bien qu'une lecture puis une écriture relâchées suffisent — même résultat, sans préfixe `lock`. **−5,0 % de cycles**, à fraîcheur des compteurs strictement inchangée.
- Le moteur de recherche est à **ordre dynamique** (MRV : la case vide la plus contrainte à chaque nœud) — c'est désormais l'unique moteur, l'ancien moteur à ordre fixe et les drapeaux `mrv_enabled`/`pruner_dfs_mrv` ont été supprimés une fois la mesure tranchée en sa faveur : à temps CPU égal, sur un vrai stock serveur, il prouve la mort de 79 possibilités sur 120 contre 20 pour l'ancien ordre fixe (mesuré par `make bench-refutation`). Conséquence directe : `alloc` (`possibility_packet`) n'est plus une position dans un ordre de parcours fixe mais le **nombre de pièces posées** sur le plateau — voir [docs/conception/mrv_moteur_unique.md](docs/conception/mrv_moteur_unique.md).
- Le choix de case du balayage MRV est pondéré par un **poids d'échec appris** : chaque refus du forward-check compte un échec sur la case morte et sur la case posée, et ce poids **divise le score MRV** — la case qui a le plus souvent tué une branche est essayée d'abord, avant même la case la mieux contrainte. C'est `dom/wdeg` ramené à un décalage (un score divisé par deux à chaque doublement du poids, plafond ×16) plutôt qu'à une division, la boucle chaude ne pouvant pas en payer une. Mesuré par `make bench-refutation` sur trois tranches de profondeur disjointes d'un stock de production, plafond 2 M de nœuds : **−96 % de nœuds** et 497 → 500 sous-arbres fermés sur les racines de ≥ 130 pièces, **124 → 287 fermetures sur 300** entre 100 et 120 pièces, réfutation **40× plus rapide bout à bout** ; côté résolution, sur 30 clones à solution connue, **médiane 504 429 → 69 826 nœuds** (apparié 29/1). Le sens du critère est mesuré, pas déduit : à l'envers il coûte +210 %, et ne compter que la case morte +46 %. Voir [docs/conception/elagage_recherche.md](docs/conception/elagage_recherche.md) §4.14 et §4.16.

> Détails (modèle de processus/threads, IPC parent↔enfants, structure des sources) : [docs/architecture.md](docs/architecture.md).

## Compilation

```sh
make                # Build de production (ANSI, sans dépendance) → ./eternityII
make NCURSES=1      # Interface ncurses (optionnelle)
make CUDA=1         # Pruner GPU CUDA (Linux/NVIDIA, option `--gpu` du mode `pruner`)
make clean          # Supprime les binaires et objets
```

Prérequis : `gcc`, `make`, pthreads (disponibles en standard sur macOS et Linux).

Le build sonde le compilateur et ajoute `-mpopcnt` s'il l'accepte : le balayage MRV fait 218 `popcount` par nœud, et sans ce drapeau gcc les compile en appels à `__popcountdi2` (libgcc) — **×2,10 en nœuds/s** avec, à arbre exploré identique. Le binaire x86 produit exige donc POPCNT (Nehalem 2008 / Barcelona 2007) ; `make POPCNT_FLAG=` revient à la ligne de base, correct mais plus lent. Voir [docs/compilation.md](docs/compilation.md#-mpopcnt--ajouté-automatiquement-et-pourquoi-cest-indispensable).

> Détails (options `DEBUG`/`WERROR`/`ASAN`, configuration du puzzle `ETERN_PARTS`/`FORWARD_CHECK_K`, drapeaux de debug) : [docs/compilation.md](docs/compilation.md) — build CUDA : [docs/pruner_gpu_cuda.md](docs/pruner_gpu_cuda.md).

## Utilisation

```sh
# Serveur (distribue les possibilités ; --expand-level évite la famine du démarrage)
./eternityII server 80 --expand-level 4 data/pieces.csv

# Client de recherche (N processus en parallèle)
./eternityII client localhost 4

# Pruner (élague les branches mortes ; --gpu pour le GPU avec un build CUDA=1)
./eternityII pruner localhost 4

# Mode autonome, sans serveur
./eternityII test
```

### Options transverses

**Communes à tous les modes**

- `--stop-on-solution` — s'arrêter à la première solution (par défaut la recherche continue).
- `--headless` — pas de console interactive (utile en service systemd).
- `--tcp-timeout <n>` — timeout d'inactivité des sockets TCP de travail (défaut 10 s).

**Côté client/pruner**

- `--name <label>` — identité affichée côté serveur (défaut le nom d'hôte).
- `--machine-uid-file <chemin>` — identité machine persistante (défaut `./eternityii-machine_uid`).
- `--pruner-forks <n>` — dosage recherche/contrôle par fork : `n` des `nb_forks` du process contrôlent le stock comme un pruner, les autres cherchent (absente : comportement historique inchangé ; incompatible avec `--gpu` si différente de `nb_forks`). Voir [Utilisation](docs/utilisation.md#dosage-recherchecontrôle-par-fork---pruner-forks).
- `--config-file <chemin>` — fichier de configuration clé=valeur (défaut `./eternityii-client.conf`), pré-remplit `nb_forks`/`pruner_forks`/`server_host`/`parts_file`/`max_stock_by_thread`/`shallow_root_abandon_depth`/`limit`/`pruner_batch` pour les positions non fournies en ligne de commande (commandes console `config`/`configSave`).
- `--shallow-root-abandon-depth <n>` — abandonne (rend au serveur) une racine reçue trop peu profonde une fois creusée jusqu'à `n` pièces posées ; opt-in, désactivé par défaut (0). Voir [Utilisation](docs/utilisation.md#option---shallow-root-abandon-depth-client-et-pruner).

**Côté serveur**

- `--stock-files <n>` / `--rebalance-budget <n>` — nombre de files de stock et vitesse de rééquilibrage. Voir [Utilisation](docs/utilisation.md#maîtrise-de-la-charge-serveur---stock-files---rebalance-budget---tcp-timeout).
- `--no-rebalance` — le rééquilibrage du stock entre files est ACTIF par défaut, à chaque tour (10 s) : il garde les files de taille comparable, donc courte la fenêtre de blocage par fichier d'une sauvegarde cohérente. `--no-rebalance` (deuxième des trois options-drapeaux négatives, avec `--no-rmnonext` et `--no-autobackup`) le supprime, pour un serveur dont on veut qu'aucune possibilité ne change de file sans ordre explicite — un rééquilibrage défait l'ordre qu'un `sortAscFiles` vient d'établir. `--rebalance-budget 0` ne joue PAS ce rôle (valeur ignorée, le défaut est conservé). La commande console `rebalance [n]` reste disponible à la demande. Voir [Utilisation](docs/utilisation.md#rééquilibrage-automatique-du-stock---no-rebalance).
- `--no-autobackup` — la sauvegarde automatique est ACTIVE par défaut : toutes les ~60 s, et seulement si l'artefact concerné a changé, le serveur réécrit `temp.back`, `temp_analysed.back`, `temp-best_board.back` et `temp-known_clients.back`. `--no-autobackup` (troisième et dernière option-drapeau négative, avec `--no-rmnonext` et `--no-rebalance`) supprime cette décision, pour un serveur dont on veut maîtriser soi-même l'instant des sauvegardes. ⚠️ C'est la seule persistance périodique : désactivée, un arrêt brutal perd tout le travail depuis la dernière sauvegarde manuelle. La commande console `backup` et `--stop-on-solution` sauvegardent toujours — sauf pendant une passe d'expansion, où une partie du stock est hors des pools : toute sauvegarde y est sautée (l'automatique repart après la passe, `backup` échoue explicitement), et `restore` est refusé. Voir [Utilisation](docs/utilisation.md#sauvegarde-automatique-périodique---no-autobackup).
- `--stock-max-ram <n>` — plafond en Mo des deux pools de stock, refuse la croissance au-delà. Voir [Utilisation](docs/utilisation.md#plafond-ram-du-stock---stock-max-ram).
- Mémoire : le serveur plafonne ses arènes malloc à une seule (glibc) avant de créer ses threads. Sans cela, les échanges avec les pruners faisaient migrer le stock d'une arène à l'autre et le RSS ne redescendait plus : **220 Mo contre 131 Mo** mesurés pour le même million de possibilités. `MALLOC_ARENA_MAX` fourni par l'opérateur l'emporte. Voir [Utilisation](docs/utilisation.md#mémoire-du-serveur-et-arènes-malloc).
- `--expand-level <n>` — pré-expansion anti-famine du stock au démarrage (voir aussi la commande console `expand`). **Une passe demande environ le volume du stock déjà présent, en plus de lui** : elle le draine entièrement dans une file de travail avant de le reconstruire. Cette file range la forme compacte : pic de RSS mesuré sur 2 M de possibilités, **+28 Mo contre +1 000 Mo** en paquets entiers. Ce pic n'entre ni dans les octets résidents affichés ni dans `--stock-max-ram`, et l'allocateur ne le rend pas au système ensuite. Voir [Utilisation](docs/utilisation.md#expansion-du-stock-au-démarrage---expand-level-anti-famine).
- `--stock-spill-dir <chemin>` — déporte sur disque plutôt que refuser une fois le plafond RAM approché (défaut `./eternityii-spill`, survit à un `backup` suivi d'un `restore` ; la file de travail d'une expansion compte dans le plafond, et rien n'est rechargé depuis le disque pendant une expansion). Voir [Utilisation](docs/utilisation.md#débordement-sur-disque-du-stock---stock-spill-dir).
- `--no-rmnonext` [`--rmnonext-interval <n>`] — l'élagage automatique des possibilités sans suite est ACTIF par défaut, toutes les 30 s. Une passe parcourt tout le stock en tenant les files : sur une pile très longue elle peut saturer le serveur. Deux dosages, du plus doux au plus radical — `--rmnonext-interval 3600` espace les passes sans renoncer à l'élagage, `--no-rmnonext` (l'une des trois seules options-drapeaux négatives, avec `--no-rebalance` et `--no-autobackup`) ne démarre jamais le thread. La commande console `removeNoNext` reste disponible pour un élagage manuel, au moment choisi. Voir [Utilisation](docs/utilisation.md#élagage-automatique-des-possibilités-sans-suite---no-rmnonext---rmnonext-interval).
- `--sort-enabled` [`--sort-interval <n>`] [`--sort-direction asc|desc`] [`--sort-lock-attempts <n>`] — tri périodique du stock par file (sans regroupement, comme `sortAscFiles`/`sortDescFiles`), désactivé par défaut ; tourne en continu quel que soit le trafic, chaque file (et chaque pool) verrouillée individuellement via un `trylock` borné (défaut 50 tentatives), un segment occupé étant simplement sauté plutôt que de suspendre toute la passe. Chaque segment mémorise sa dernière direction triée et une passe saute directement (sans trylock) tout segment déjà à jour, sans insertion depuis. Voir [Utilisation](docs/utilisation.md#tri-périodique-du-stock---sort-enabled).
- `--auto-roles` — politique automatique de dosage recherche/contrôle du parc, désactivée par défaut (l'opérateur garde la main via `clientsRoles` tant qu'elle n'est pas activée). Voir [Utilisation](docs/utilisation.md#politique-automatique-de-dosage---auto-roles).
- `--http-port N` — active l'API HTTP REST admin sur `127.0.0.1:N`, éventuellement complétée par `--http-token-file <chemin>` pour exiger un jeton Bearer sur toutes les commandes admin de modification (`pause`, `resume`, `limit`, `maxStockByThread`, `shallowRootAbandonDepth`, `prunerBatch`, `clientsCommand`, `clientsRoles`, `restore`, `backup`, `sortAsc`, `sortAscFiles`, `sortDesc`, `sortDescFiles`, `sortDescMulti`, `split`, `regroup`, `stockMaxRam` — seules les lectures restent ouvertes).
- `--config-file <chemin>` — fichier de configuration clé=valeur (défaut `./eternityii-server.conf`, même option que côté client mais son propre chemin par défaut), couvre TOUTES les options de démarrage du serveur (`nb_threads`/`parts_file` positionnels, plus `expand_level`/`expand_max_stock`/`expand_max_levels`/`http_port`/`http_token_file`/`stock_files`/`stock_max_ram`/`stock_spill_dir`/`rebalance_budget`/`tcp_timeout`/`sort_enabled`/`sort_interval`/`sort_direction`/`sort_lock_attempts`/`rmnonext_enabled`/`rmnonext_interval`/`rebalance_enabled`/`auto_roles`/`stop_on_solution`/`headless`) pour les options non fournies en ligne de commande — priorité CLI > fichier > défauts. Lu une seule fois, avant le démarrage du serveur (pas d'orchestrateur différé côté serveur, contrairement au client). Consultable/persistée via les commandes console `config`/`configSave`, qui affichent/écrivent la configuration serveur effective — `config <clé> <valeur>` (préparation d'un changement à chaud) reste, elle, refusée côté serveur, faute de configuration "en préparation" à appliquer. Voir [Utilisation](docs/utilisation.md#fichier-de-configuration-serveur---config-file).

L'aide intégrée est accessible via `./eternityII --help` (aide générale) et `./eternityII help <sujet>` (détail d'un mode ou d'une option).

Un client/pruner ne fork plus ses process de recherche immédiatement au démarrage : si un fichier de configuration est trouvé (`--config-file`), un décompte de 5 s lance l'auto-démarrage (annulé dès la première touche pressée) ; sinon il attend une commande `start`/`config <clé> <valeur>`. Ce cycle de vie (`start`, `stopForks`, `configApply`, `config`, `configSave`) est pilotable en **console locale** ou **à distance depuis le serveur** via `clientsCommand [--to <cible>]` (voir ci-dessous) — ex. `clientsCommand --to jetson-1 stopForks` puis `clientsCommand --to jetson-1 configApply` après avoir préparé `clientsCommand --to jetson-1 config nb_forks 8`. Détails : [docs/console.md](docs/console.md) et [docs/echanges_client_serveur.md](docs/echanges_client_serveur.md#pilotage-à-distance-du-cycle-de-vie-des-fils).

Les possibilités sont déposées au serveur **par lots** (`INST_ADD_BATCH`, protocole v14) : un `int32` K, K paquets, un seul acquittement. Avant, chaque possibilité coûtait un aller-retour TCP complet — un fork pruner passait **70 % de son temps bloqué dans un `recv` d'un octet**, et augmenter `prunerBatch` n'y changeait rien (ce réglage ne gouverne que l'aller). À latence égale (12 ms de RTT), le dépôt par lot vaut **×17 sur le débit d'un pruner** (3 205 → 54 469 possibilités vérifiées par minute à réglage égal, CPU par fork 12,8 % → 83,6 %). Voir [Échanges client/serveur](docs/echanges_client_serveur.md#dépôt-par-lot-inst_add_batch-v14).

Les sauvegardes `.back` et les segments de débordement emploient une **forme compacte** (en-tête magie/version/géométrie, puis des enregistrements sérialisés champ par champ) : une possibilité y pèse 65 octets en moyenne au lieu de 576. Mesuré sur un stock de production réel, **1 963 Mo → 222 Mo (x8,83)**, et autant de temps de sauvegarde sous verrou en moins. Les `.back` écrits dans l'ancien format restent lus sans rien à faire. Voir [Utilisation](docs/utilisation.md#format-compact).

> Détails (paramètres et défauts de chaque mode, expansion anti-famine, échange par lots des pruners, format du fichier de pièces, fichiers générés `.back`/`solution_*`/`events.log`/sockets Unix `etii_main.<pid>`, limitations connues) : [docs/utilisation.md](docs/utilisation.md).

## Console interactive

Une fois lancé, le programme écoute des commandes sur l'entrée standard.

- **Commandes principales** :
  - Sauvegarde/restauration du stock : `backup`, `restore`.
  - Tri et élagage des files : `sortDesc`, `sortAscFiles`, `sortDescFiles`, `removeNoNext`, `expand`, `resetChecked` (rebascule tout le pool vérifié vers le pool non vérifié, pour le resoumettre aux pruners).
  - Régulation : `limit`, `pause`/`resume`.
  - Activité des fils : `check` (rapport périodique par fork) et `statistic` — sur un client ou un pruner, les deux affichent la ligne `pruner : <mortes> mortes / <contrôlées> vérifiées (<taux>), <n> cases étudiées`, agrégée depuis les compteurs remontés par les forks. Les files locales d'un client sont toujours vides (le travail a lieu dans les forks) : c'est cette ligne, et non elles, qui dit si un pruner travaille. Voir [docs/console.md](docs/console.md).
  - Diagnostic du stock : `checkDatas` (intégrité des plateaux), `checkDuplicate` (doublons et relations ancêtre/descendant, dans les deux pools), `checkOrigin [purge]` — vérifie qu'aucune possibilité en stock n'est la **racine** d'une autre (plateau entièrement contenu dans un plateau plus profond, dont le sous-arbre est donc exploré deux fois) et détecte aussi les **doublons exacts** à `alloc` égal, dans les deux pools ; avec `purge`, supprime les descendants/doublons redondants et garde les racines/un seul survivant par groupe de doublons, sans perdre de branche de recherche.
  - Cycle de vie des fils côté client/pruner : `start`, `stopForks`, `configApply`, `config [<clé> <valeur>]`, `configSave`.
  - Pilotage des clients connectés depuis le serveur :
    - `clients`, `clientsStats` — état des clients connectés.
    - `clientsCommand [--to <session_no|client_uid|label>]` — cible un seul client au lieu de diffuser à tous, y compris les cinq commandes de cycle de vie ci-dessus poussées à distance.
    - `clientsRoles [--to <cible>] <nb_pruner>` — compose `config pruner_forks`/`configApply` en une commande et mémorise le dosage désiré par machine (survit à une reconnexion/un redémarrage du client).
    - `knownClients` — liste cumulative des machines connues, y compris déconnectées.
    - `clientsWork <cible>` — consulte ce qu'un client précis détient actuellement en cours d'analyse.
    - `leaseDuration <n>` — règle le bail à expiration qui rend automatiquement au stock la part d'un client disparu.
- **Aide intégrée** : `help` liste les commandes par catégorie avec leur syntaxe, `help <commande>` détaille une commande, `help <catégorie>` filtre une section ; les noms canoniques sont en camelCase complet et insensibles à la casse, les noms historiques abrégés restent des alias (`sortd`, `rmnonext`, `clientsCmd`, `quit`, …) et un argument manquant affiche automatiquement le rappel d'usage.
- **Évènements & historique** : les évènements notables (records, solutions, connexions) sont affichés dans une zone dédiée et journalisés dans `events.log` ; l'historique des commandes (↑/↓) est persisté entre sessions.
- **Affichage** : aucune commande n'efface l'écran implicitement — seule la commande `clear` (alias `cls`, raccourci Ctrl-L) le fait, sans perdre le contenu (scrollback du terminal en ANSI, PgUp en ncurses) — et la ligne de saisie en cours n'est plus corrompue par les logs asynchrones (elle est redessinée sous chaque log). Les sorties longues (`help`, `statistic`, `print`, …) sont paginées en mode interactif (`--Suite--`, comme `more` ; en ncurses le pad scrollable — PgUp/PgDn et molette — couvre le besoin).
- **Export** : `print`, `printFile <n>` et `printAnalysed` acceptent en plus un fichier de destination optionnel (`print ./dump.json`) pour exporter un gros stock plutôt que de le pousser à l'écran.
- **Édition de ligne** : curseur ←/→, Home/End, Ctrl-A/E/U/W, rappel d'historique — module commun aux deux builds, testable sans terminal.
- **Bandeau de stats live** (vidéo inverse, `coups/s`/`stock`/`record`/…) affiché en continu au-dessus de la zone Events dans les deux builds — auparavant réservé à ncurses.

> Détails (table complète des commandes, zone Events, historique, interface ncurses) : [docs/console.md](docs/console.md).

## Tests et intégration continue

```sh
make test             # tests unitaires (framework greatest, vendoré) — le runner tourne
                      # dans un répertoire temporaire : aucun fichier n'est écrit dans le dépôt,
                      # et CMakeLists.txt est vérifié en phase avec le makefile
make test-integration # scénarios bout-en-bout client/serveur (16 pièces)
make test-docker      # rejoue les jobs de test CI dans 3 conteneurs Linux en parallèle
make test-docker-arm  # vérifie la compilation croisée ARM 64-bit (Raspberry Pi) dans le même conteneur
make coverage-report  # rapports de couverture gcovr (XML + HTML + Markdown)

# Banc de RÉFUTATION : coût de la preuve qu'une possibilité est morte (la mesure qui
# correspond à l'objectif du solveur — cf. docs/tests_et_ci.md)
make bench-refutation BENCH_REFUT_ARGS="--from-back temp.back --min-pieces 90 --budget 5000000"

# Banc de mesure du débit de la boucle de recherche (préalable à toute optimisation)
tests/bench/bench_search.sh --nodes 5000000 --reps 5

# Banc « CÔTÉ TROUVER » : coût de l'ATTEINTE d'une solution, sur des CLONES à solution
# connue — la seule grandeur que les deux bancs ci-dessus ne peuvent PAS voir, puisqu'un
# sous-arbre mort est exploré en entier quel que soit l'ordre des valeurs. Première
# campagne (2 640 exécutions) : aucun ordre des valeurs ne bat celui de production.
python3 tools/gen_clone.py --size 10 --inner-colours 17 --seed 1 --hints 5 --out-dir data/clones
make bench-solve CPPFLAGS=-DETERN_PARTS=100 BENCH_SOLVE_ARGS="--instance-dir data/clones"

# Garde-fou des renvois de documentation : un lien fichier.md#ancre dont le titre visé a
# été étendu depuis n'affiche AUCUNE erreur sur GitHub (il ouvre le haut du fichier), la
# dérive est donc invisible à la relecture. Hors de `make test` : c'est un outil.
make check-doc-links
```

La CI GitHub Actions compile **toutes les combinaisons du code** avec `WERROR=1`, lance les tests unitaires et d'intégration, et publie la couverture sur Codecov.

> Détails (scripts d'intégration, Docker, couverture, matrice CI, banc de mesure `ETII_BENCH_NODES`, outil `make gen-root` qui convertit un plateau externe en racine de stock, générateur de clones `tools/gen_clone.py`, banc `make bench-solve` et garde-fou des renvois `make check-doc-links`) : [docs/tests_et_ci.md](docs/tests_et_ci.md) — conventions d'écriture des tests : [tests/README.md](tests/README.md).

## Documentation

Le répertoire [`docs/`](docs/) rassemble la documentation détaillée :

### État du code

Ces documents décrivent le comportement **implémenté** :

| Document | Contenu |
|---|---|
| [docs/architecture.md](docs/architecture.md) | Architecture d'ensemble : processus/threads, IPC parent↔enfants, structure des sources. |
| [docs/compilation.md](docs/compilation.md) | Options de build, prérequis, configuration du puzzle, drapeaux de debug. |
| [docs/utilisation.md](docs/utilisation.md) | Modes d'exécution et leurs paramètres, fichiers manipulés, limitations connues. |
| [docs/console.md](docs/console.md) | Commandes interactives, zone Events, historique, interface ncurses. |
| [docs/echanges_client_serveur.md](docs/echanges_client_serveur.md) | Protocole TCP client/serveur : instructions, gestion de charge, séquences, pannes, et le [canal de contrôle](docs/echanges_client_serveur.md#canal-de-contrôle-v9-étendu-en-v10-et-v12) (v9). |
| [docs/api_http_rest.md](docs/api_http_rest.md) | API HTTP REST admin (`--http-port`) : schémas JSON complets (télémétrie, clients, meilleur plateau, répartition du stock), codes d'erreur, authentification par jeton Bearer des commandes de modification (`--http-token-file`), exemples client (curl, Python). |
| [docs/autosearch_step.md](docs/autosearch_step.md) | Flux de recherche (`autosearch_step`) et gestion mémoire d'un thread de recherche. |
| [docs/format_stock_compact.md](docs/format_stock_compact.md) | Forme compacte d'une possibilité : mesures, formes écartées, pistes à ne pas rejouer. |
| [docs/pruner_gpu_cuda.md](docs/pruner_gpu_cuda.md) | Pruner GPU (`pruner --gpu`) : prérequis de compilation et d'exécution, flux CUDA, avantages. |
| [docs/tests_et_ci.md](docs/tests_et_ci.md) | Cibles de test, intégration bout-en-bout, Docker, couverture, CI, banc de mesure du débit de recherche. |
| [tests/README.md](tests/README.md) | Organisation des suites unitaires, conventions, ajout d'un test. |

### Conception et réflexions en cours

[`docs/conception/`](docs/conception/) rassemble les documents de conception : propositions,
arbitrages et découpages en PR **pas encore entièrement implémentés**. Ils décrivent une cible, pas
le comportement du code — voir [docs/conception/README.md](docs/conception/README.md) pour la
convention (statut, cycle de vie), la table des documents actifs et l'historique des documents
entièrement implémentés puis supprimés (leur contenu ayant rejoint la documentation de référence
ci-dessus).

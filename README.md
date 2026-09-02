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
- Le moteur de recherche est à **ordre dynamique** (MRV : la case vide la plus contrainte à chaque nœud) — c'est désormais l'unique moteur, l'ancien moteur à ordre fixe et les drapeaux `mrv_enabled`/`pruner_dfs_mrv` ont été supprimés une fois la mesure tranchée en sa faveur : à temps CPU égal, sur un vrai stock serveur, il prouve la mort de 79 possibilités sur 120 contre 20 pour l'ancien ordre fixe (mesuré par `make bench-refutation`). Conséquence directe : `alloc` (`possibility_packet`) n'est plus une position dans un ordre de parcours fixe mais le **nombre de pièces posées** sur le plateau — voir [docs/conception/mrv_moteur_unique.md](docs/conception/mrv_moteur_unique.md).

> Détails (modèle de processus/threads, IPC parent↔enfants, structure des sources) : [docs/architecture.md](docs/architecture.md).

## Compilation

```sh
make                # Build de production (ANSI, sans dépendance) → ./eternityII
make NCURSES=1      # Interface ncurses (optionnelle)
make CUDA=1         # Pruner GPU CUDA (Linux/NVIDIA, option `--gpu` du mode `pruner`)
make clean          # Supprime les binaires et objets
```

Prérequis : `gcc`, `make`, pthreads (disponibles en standard sur macOS et Linux).

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
- `--config-file <chemin>` — fichier de configuration clé=valeur (défaut `./eternityii-client.conf`), pré-remplit `nb_forks`/`pruner_forks`/`server_host`/`parts_file`/`max_stock_by_thread`/`limit`/`pruner_batch` pour les positions non fournies en ligne de commande (commandes console `config`/`configSave`).

**Côté serveur**

- `--stock-files <n>` / `--rebalance-budget <n>` — nombre de files de stock et vitesse de rééquilibrage. Voir [Utilisation](docs/utilisation.md#maîtrise-de-la-charge-serveur---stock-files---rebalance-budget---tcp-timeout).
- `--stock-max-ram <n>` — plafond en Mo des deux pools de stock, refuse la croissance au-delà. Voir [Utilisation](docs/utilisation.md#plafond-ram-du-stock---stock-max-ram).
- `--stock-spill-dir <chemin>` — déporte sur disque plutôt que refuser une fois le plafond RAM approché (défaut `./eternityii-spill`, survit à un `backup` suivi d'un `restore`). Voir [Utilisation](docs/utilisation.md#débordement-sur-disque-du-stock---stock-spill-dir).
- `--auto-roles` — politique automatique de dosage recherche/contrôle du parc, désactivée par défaut (l'opérateur garde la main via `clientsRoles` tant qu'elle n'est pas activée). Voir [Utilisation](docs/utilisation.md#politique-automatique-de-dosage---auto-roles).
- `--http-port N` — active l'API HTTP REST admin sur `127.0.0.1:N`, éventuellement complétée par `--http-token-file <chemin>` pour exiger un jeton Bearer sur toutes les commandes admin de modification (`pause`, `resume`, `limit`, `maxStockByThread`, `prunerBatch`, `clientsCommand`, `clientsRoles`, `restore`, `backup`, `sortAsc`, `sortAscFiles`, `sortDesc`, `sortDescFiles`, `sortDescMulti`, `split`, `regroup`, `stockMaxRam` — seules les lectures restent ouvertes).
- `--config-file <chemin>` — fichier de configuration clé=valeur (défaut `./eternityii-server.conf`, même option que côté client mais son propre chemin par défaut), couvre TOUTES les options de démarrage du serveur (`nb_threads`/`parts_file` positionnels, plus `expand_level`/`expand_max_stock`/`expand_max_levels`/`http_port`/`http_token_file`/`stock_files`/`stock_max_ram`/`stock_spill_dir`/`rebalance_budget`/`tcp_timeout`/`auto_roles`/`stop_on_solution`/`headless`) pour les options non fournies en ligne de commande — priorité CLI > fichier > défauts. Lu une seule fois, avant le démarrage du serveur (pas d'orchestrateur différé côté serveur, contrairement au client). Consultable/persistée via les commandes console `config`/`configSave`, qui affichent/écrivent la configuration serveur effective — `config <clé> <valeur>` (préparation d'un changement à chaud) reste, elle, refusée côté serveur, faute de configuration "en préparation" à appliquer. Voir [Utilisation](docs/utilisation.md#fichier-de-configuration-serveur---config-file).

L'aide intégrée est accessible via `./eternityII --help` (aide générale) et `./eternityII help <sujet>` (détail d'un mode ou d'une option).

Un client/pruner ne fork plus ses process de recherche immédiatement au démarrage : si un fichier de configuration est trouvé (`--config-file`), un décompte de 5 s lance l'auto-démarrage (annulé dès la première touche pressée) ; sinon il attend une commande `start`/`config <clé> <valeur>`. Ce cycle de vie (`start`, `stopForks`, `configApply`, `config`, `configSave`) est pilotable en **console locale** ou **à distance depuis le serveur** via `clientsCommand [--to <cible>]` (voir ci-dessous) — ex. `clientsCommand --to jetson-1 stopForks` puis `clientsCommand --to jetson-1 configApply` après avoir préparé `clientsCommand --to jetson-1 config nb_forks 8`. Détails : [docs/console.md](docs/console.md) et [docs/echanges_client_serveur.md](docs/echanges_client_serveur.md#pilotage-à-distance-du-cycle-de-vie-des-fils).

> Détails (paramètres et défauts de chaque mode, expansion anti-famine, échange par lots des pruners, format du fichier de pièces, fichiers générés `.back`/`solution_*`/`events.log`/sockets Unix `etii_main.<pid>`, limitations connues) : [docs/utilisation.md](docs/utilisation.md).

## Console interactive

Une fois lancé, le programme écoute des commandes sur l'entrée standard.

- **Commandes principales** :
  - Sauvegarde/restauration du stock : `backup`, `restore`.
  - Tri et élagage des files : `sortDesc`, `sortAscFiles`, `sortDescFiles`, `removeNoNext`, `expand`, `resetChecked` (rebascule tout le pool vérifié vers le pool non vérifié, pour le resoumettre aux pruners).
  - Régulation : `limit`, `pause`/`resume`.
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
make test             # tests unitaires (framework greatest, vendoré)
make test-integration # scénarios bout-en-bout client/serveur (16 pièces)
make test-docker      # rejoue les jobs de test CI dans 3 conteneurs Linux en parallèle
make test-docker-arm  # vérifie la compilation croisée ARM 64-bit (Raspberry Pi) dans le même conteneur
make coverage-report  # rapports de couverture gcovr (XML + HTML + Markdown)

# Banc de RÉFUTATION : coût de la preuve qu'une possibilité est morte (la mesure qui
# correspond à l'objectif du solveur — cf. docs/tests_et_ci.md)
make bench-refutation BENCH_REFUT_ARGS="--from-back temp.back --min-pieces 90 --budget 5000000"

# Banc de mesure du débit de la boucle de recherche (préalable à toute optimisation)
tests/bench/bench_search.sh --nodes 5000000 --reps 5
```

La CI GitHub Actions compile **toutes les combinaisons du code** avec `WERROR=1`, lance les tests unitaires et d'intégration, et publie la couverture sur Codecov.

> Détails (scripts d'intégration, Docker, couverture, matrice CI, banc de mesure `ETII_BENCH_NODES`) : [docs/tests_et_ci.md](docs/tests_et_ci.md) — conventions d'écriture des tests : [tests/README.md](tests/README.md).

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
| [docs/echanges_client_serveur.md](docs/echanges_client_serveur.md) | Protocole TCP client/serveur : instructions, gestion de charge, séquences, pannes, et le [canal de contrôle](docs/echanges_client_serveur.md#canal-de-contrôle-v9) (v9). |
| [docs/api_http_rest.md](docs/api_http_rest.md) | API HTTP REST admin (`--http-port`) : schémas JSON complets (télémétrie, clients, meilleur plateau, répartition du stock), codes d'erreur, authentification par jeton Bearer des commandes de modification (`--http-token-file`), exemples client (curl, Python). |
| [docs/autosearch_step.md](docs/autosearch_step.md) | Flux de recherche (`autosearch_step`) et gestion mémoire d'un thread de recherche. |
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

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
- La boucle de recherche lit sa table de candidats via un **index compact** (4 octets par compartiment au lieu de 16), qui divise par 3,8 le volume balayé par le forward-checking : **+10 % de nœuds/s** sur un worker, **+29 %** sur 16 workers concurrents.
- Cette table étant en lecture seule une fois construite, le parent la construit **avant de forker** : les processus de recherche s'en partagent **une seule copie** (copy-on-write) au lieu d'en fabriquer chacun la leur — **−90 % d'empreinte mémoire** à 16 workers (111 → 11 Mo de `Pss`), pour un débit inchangé.

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

Options transverses : `--stop-on-solution` (s'arrêter à la première solution ; par défaut la recherche continue), `--headless` (pas de console interactive — utile en service systemd), côté client/pruner `--name <label>` (identité affichée côté serveur, défaut le nom d'hôte) et `--machine-uid-file <chemin>` (identité machine persistante, défaut `./eternityii-machine_uid`), et côté serveur `--http-port N` pour activer l'API HTTP REST admin sur `127.0.0.1:N`, éventuellement complétée par `--http-token-file <chemin>` pour exiger un jeton Bearer sur les deux commandes admin privilégiées (`restore`, `backup`). L'aide intégrée est accessible via `./eternityII --help` (aide générale) et `./eternityII help <sujet>` (détail d'un mode ou d'une option).

> Détails (paramètres et défauts de chaque mode, expansion anti-famine, échange par lots des pruners, format du fichier de pièces, fichiers générés `.back`/`solution_*`/`events.log`, limitations connues) : [docs/utilisation.md](docs/utilisation.md).

## Console interactive

Une fois lancé, le programme écoute des commandes sur l'entrée standard.

- **Commandes principales** : sauvegarde/restauration du stock (`backup`, `restore`), tri et élagage des files (`sortDesc`, `removeNoNext`, `expand`), régulation (`limit`, `pause`/`resume`), et pilotage des clients connectés depuis le serveur (`clients`, `clientsStats`, `clientsCommand [--to <session_no|client_uid|label>]` pour cibler un seul client au lieu de diffuser à tous, `knownClients` pour la liste cumulative des machines connues — y compris déconnectées, `clientsWork <cible>` pour consulter ce qu'un client précis détient actuellement en cours d'analyse).
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
make test-docker      # rejoue les jobs de test CI dans un conteneur Linux
make coverage-report  # rapports de couverture gcovr (XML + HTML + Markdown)

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
| [docs/api_http_rest.md](docs/api_http_rest.md) | API HTTP REST admin (`--http-port`) : schémas JSON complets, codes d'erreur, authentification par jeton Bearer des commandes privilégiées (`--http-token-file`), exemples client (curl, Python). |
| [docs/autosearch_step.md](docs/autosearch_step.md) | Flux de recherche (`autosearch_step`) et gestion mémoire d'un thread de recherche. |
| [docs/pruner_gpu_cuda.md](docs/pruner_gpu_cuda.md) | Pruner GPU (`pruner --gpu`) : prérequis de compilation et d'exécution, flux CUDA, avantages. |
| [docs/tests_et_ci.md](docs/tests_et_ci.md) | Cibles de test, intégration bout-en-bout, Docker, couverture, CI, banc de mesure du débit de recherche. |
| [tests/README.md](tests/README.md) | Organisation des suites unitaires, conventions, ajout d'un test. |

### Conception et réflexions en cours

[`docs/conception/`](docs/conception/) rassemble les documents de conception : propositions,
arbitrages et découpages en PR **non encore implémentés**. Ils décrivent une cible, pas le
comportement du code — voir [docs/conception/README.md](docs/conception/README.md) pour la
convention (statut, cycle de vie).

| Document | Contenu |
|---|---|
| [docs/conception/identification_clients.md](docs/conception/identification_clients.md) | Identification unique des clients au-delà du PID : modèle d'identité, statistiques cumulées persistantes, bail à expiration sur les analyses en cours, découpage en 7 PR. |

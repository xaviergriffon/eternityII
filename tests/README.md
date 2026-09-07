# Tests unitaires

Tests unitaires basés sur [greatest](https://github.com/silentbicycle/greatest),
un framework C *single-header* (licence ISC) vendoré dans `tests/greatest.h` —
aucune dépendance externe à installer.

## Lancer les tests

```sh
make test
```

Compile un binaire isolé (`tests/run_tests`) qui ne lie **que** les modules
sous test et leurs dépendances de link — `main.c` n'est jamais inclus (il a son
propre `main()`). Le runner sort avec un code non nul si un test échoue, ce qui
le rend utilisable tel quel en CI.

Options utiles du runner (passées via greatest) :

```sh
./tests/run_tests -v          # verbeux (affiche chaque test)
./tests/run_tests -t rotate   # ne lance que les tests dont le nom contient "rotate"
./tests/run_tests -l          # liste les tests sans les exécuter
```

## Organisation

Les suites sont rangées par domaine, **en miroir de `src/`** ; le harnais
partagé reste à la racine de `tests/`.

| Emplacement | Contenu |
|---|---|
| `tests/test_main.c` | Point d'entrée unique du runner (enregistre toutes les suites). |
| `tests/greatest.h`, `tests/fork_assert.h` | Framework greatest + helper d'assertions par `fork()`. |
| `tests/core/` | Suites des modules `src/core/` (`test_lifo`, `test_part`, `test_readdata`, `test_possibility`, `test_etii_search`, `test_datamanager`, `test_solution16`). |
| `tests/net/` | Suites des modules `src/net/` (`test_etii_protocol`, `test_control_protocol` — codec du [canal de contrôle](../docs/echanges_client_serveur.md#canal-de-contrôle-v9), `test_local_socket`, `test_tcp`). |
| `tests/ui/` | Suites des modules `src/ui/` (`test_command_history`, `test_command_match`, `test_command_lines`, `test_console`, `test_logger`). |
| `tests/app/` | Suites des modules `src/app/` (`test_static_variables`, `test_app_runtime`, `test_etii_client`, `test_etii_server`, `test_control_registry` — registre serveur du canal de contrôle, `test_etii_control` — thread client du canal de contrôle). |
| `tests/tools/` | Outils autonomes + leur cœur testé (`gen_root` / `root_from_board` — conversion d'un plateau externe en racine de stock ; `border_mass` / `border_walk` — masse totale des anneaux de bordure). |

Chaque `test_<module>.c` inclut ses en-têtes de production en forme qualifiée
(`#include "core/part.h"`, résolu via `-Isrc`) et le harnais en forme courte
(`#include "greatest.h"`, résolu via `-Itests`).

## Outils (`tests/tools/`)

`gen_root` convertit un plateau externe (plateau publié, sauvegarde d'un autre
solveur, réparation locale d'un plateau connu dont on retire quelques cases)
en **racine de stock** au format `.back`, chargeable par la console `restore`
ou `import`.

```sh
make gen-root                                   # ou : make gen-root CPPFLAGS="-DETERN_PARTS=16"
tests/tools/gen_root data/pieces.csv plateau.txt racine.back
```

`plateau.txt` compte `ETERN_SIZE²` lignes « id top right bottom left » en ordre
**ligne-majeur**, `0 0 0 0 0` pour une case vide.

L'outil lui-même n'est qu'une enveloppe d'entrées/sorties : toute la conversion
vit dans `root_from_board.c`, compilé avec les autres modules et couvert par
`test_root_from_board.c`. Deux conventions y sont verrouillées, chacune avec sa
contre-épreuve, parce qu'un outil externe les inverse sans que rien ne
proteste : l'orientation `grid[colonne][ligne]`, et l'indice de rotation — qui
n'est jamais calculé mais **retrouvé** dans la table de `rotate_all_parts` par
correspondance des 4 faces.

Deux pièges à l'usage, tous deux rencontrés :

- **Le serveur sème toujours ses paquets genèse** (`first_possibility`, appelé
  par `runserver`). Pour ne chercher QUE la racine importée, il faut `restore`
  (qui remplace le stock), pas `import` (qui ajoute) — sinon les clients
  travaillent sur la genèse et l'on croit à tort que la racine est explorée.
- **Le plateau doit respecter les indices officiels.** `ETERN_WITH_INDICES`
  vaut 1 par défaut, donc `first_possibility` pose les 5 indices de
  `data/indices.csv` : un plateau qui en contredit un est hors de l'espace de
  recherche, et sa racine ne mènera jamais à rien.

`border_mass` mesure la masse totale des anneaux de bordure valides (voir
[docs/tests_et_ci.md](../docs/tests_et_ci.md#outil-border_mass-make-border-mass)
et [docs/superpowers/specs/2026-09-06-masse-bordure-design.md](../docs/superpowers/specs/2026-09-06-masse-bordure-design.md)).
Son cœur pur (`border_walk.c`) est, comme celui de `gen_root`, compilé avec
les autres modules et couvert par `test_border_walk.c` ; le second algorithme
(`--dp`, `border_ring_dp.c`) l'est de même, couvert par
`test_border_ring_dp.c`.

```sh
make border-mass
tests/tools/border_mass data/pieces.csv data/indices.csv
```

Ne termine pas en plusieurs heures sur les données réelles (256 pièces) — voir
docs/tests_et_ci.md pour le détail.
`--forks N` parallélise par forks (coins d'abord, expansion en largeur,
distribution round-robin) — voir la spec liée depuis `docs/tests_et_ci.md`.
`--dp` bascule sur un algorithme exact par programmation dynamique sur des
classes de pièces interchangeables, niveau par niveau — combinable avec
`--forks N`. Un niveau assez gros mais qui tient encore en RAM voit sa
transition répartie sur N process forkés ; au-delà d'un seuil (2 Go par
défaut), le niveau lui-même bascule en fragments sur disque (partitionnement
externe par hachage, comme un GROUP BY qui ne tient pas en RAM), pour ne
jamais avoir à matérialiser un niveau entier en mémoire quelle que soit sa
taille. `--spill-dir DIR` redirige les fragments et fichiers temporaires vers
un disque plus grand que `/tmp` (souvent une petite partition ou un tmpfs
plafonné indépendamment de la RAM de la machine — a saturé en pratique sur la
machine 2×10 cœurs/48 Go visée avant correction). Bien plus rapide et sobre
en mémoire que le DFS brut ; voir `docs/tests_et_ci.md` pour les mesures et
le détail du mode disque.

## Tests d'intégration bout-en-bout

`tests/integration/` contient des scripts shell séparés de la suite `greatest`
ci-dessus (pas de lien statique : ils lancent de vrais processus `server`/
`client` sur `localhost`). Lancés ensemble via `make test-integration` (voir le
[Tests et CI](../docs/tests_et_ci.md)) :

- `run_solution_16.sh` — round-trip complet de la solution (16 pièces, `--stop-on-solution`).
- `run_control_channel.sh` — round-trip du [canal de contrôle](../docs/echanges_client_serveur.md#canal-de-contrôle-v9) (`clientsStats`/`pause`/`resume`, piloté via une FIFO sur la console serveur).

Chacun tourne dans un répertoire `mktemp -d` isolé avec un timeout borné.

## Tests du banc de mesure

`tests/bench/bench_lib.sh` isole les fonctions **pures** du banc de mesure
(`bench_search.sh`, voir [Tests et CI](../docs/tests_et_ci.md#banc-de-mesure-du-débit-de-recherche-testsbenchbench_searchsh))
: validation du temps écoulé rendu par `time`, et boucle de rejeu quand il est
illisible. `tests/bench/test_bench_parse.sh` les couvre sans rien compiler ni
lancer, et tourne dans `make test` (cible `test-bench`) — c'est la version shell
de la même règle que côté C : la logique à verrouiller est extraite dans une
fonction sans effet de bord plutôt que noyée dans un script qui compile et
mesure.

Motivation : bash peut imprimer un temps **malformé**. `timeval_to_secs`
(`execute_cmd.c`) arrondit les µs en ms sans propager la retenue vers les
secondes ; dès 999500 µs la fraction vaut 1000, et `mkfmt` imprime ses chiffres
par `(fraction / 100) + '0'`, soit 10 + '0' = `:`. Un run de 2,9997 s ressort
donc en `2.:00` au lieu de `3.000`, qu'`awk` lit comme 2.0 — débit surestimé de
50 %, min/max et écart-type du rapport faussés, JSON invalide. Le banc ne
testait auparavant que la non-vacuité de la valeur.

## Conventions et limites

- **Fixtures construites à la main** plutôt que via `rotate_all_parts` /
  `pieces.csv` : les tests restent indépendants de `ETERN_PARTS` (256/16) et de
  la présence d'un fichier de pièces dans le répertoire courant.
- **Les identifiants de pièce d'une fixture doivent rester ≤ `ETERN_PARTS`.**
  Indépendance ne veut pas dire immunité : dès qu'une fixture est *jouée* par le
  moteur (et pas seulement lue), ses ids indexent `idParts[ETERN_PARTS + 1][…]`
  et le masque `b_faceused` (`ETERN_PARTS` bits). Un jeu de 31 pièces passe donc
  en 16×16 mais **déborde la pile en 4×4** — sans aucun symptôme sous
  macOS/clang, et diagnostiqué seulement par ASan sous Linux
  (`make test-docker`). Si la fixture doit être plus riche, faire dépendre sa
  taille de `ETERN_PARTS`, ou garder le test derrière un `#if ETERN_PARTS == 256`
  (ex. `check_possibility_missing_genesis_anchor_is_minus_six`,
  `tests/core/test_possibility.c`) ; pour une fixture pensée d'emblée pour les
  deux tailles de plateau, voir
  `bt_forward_check_same_verdict_with_and_without_packed_index`
  (`tests/core/test_etii_search.c`). Même famille de piège que le `.size` d'un
  `array_part`, qui doit compter **exactement** les entrées renseignées.
- **Une fixture jouée par le moteur ne doit pas pouvoir compléter le plateau**,
  sauf si c'est le sujet du test : `record_solution` écrirait un fichier
  `solution_*` dans le répertoire courant au milieu de la suite. Le plus simple
  est de fournir moins de pièces d'un type que le plateau n'a de cases
  correspondantes.
- **Chemins d'erreur non testés là où le code appelle `exit()`** (ex. fichier
  CSV absent dans `read_parts`) : greatest tournant dans un seul processus, un
  `exit()` tuerait tout le runner. Tester ces cas demanderait d'isoler chaque
  test dans un `fork()`.

## Couverture de code

```sh
make coverage
```

Recompile la suite instrumentée (`--coverage`, en `-O0` pour un mapping ligne
fidèle), la lance, puis affiche le pourcentage de lignes couvertes par module
testé via **gcov** (intégré à gcc/clang — rien à installer) :

```
===== Couverture de code (tout le code de production) =====
  src/core/lifo.c    Lines executed:99.15% of 234
  src/core/part.c    Lines executed:91.49% of 388
  src/core/readdata.c Lines executed:85.42% of 192
```

Tous les artefacts (`.o`, `.gcno`, `.gcda`, `.gcov`) restent dans
`tests/coverage/` (ignoré par git, nettoyé par `make clean`).

**Drill-down ligne par ligne** : ouvrir `tests/coverage/<module>.c.gcov`. La
première colonne indique le nombre d'exécutions ; `#####` marque une ligne
jamais exécutée. Exemple :

```sh
grep -n '#####' tests/coverage/readdata.c.gcov   # lignes non couvertes
```

Les chemins d'erreur en `exit()` et les fonctions non encore testées (ex.
`read_from_json` / `compute_grid`) ressortent ainsi comme non couverts.

### Rapports gcovr (`make coverage-report`)

```sh
pip install gcovr          # ou pipx install gcovr / brew install gcovr
make coverage-report       # exécute `coverage` puis génère XML + HTML + Markdown
open tests/coverage/html/index.html
```

La cible produit en un passage, depuis les `.gcda` de `coverage` :

- `tests/coverage/coverage.xml` — Cobertura, consommé par Codecov en CI ;
- `tests/coverage/html/index.html` — rapport HTML navigable ;
- `tests/coverage/coverage.md` — résumé Markdown (lignes/fonctions/branches).

Le `coverage.md` est ensuite enrichi d'une section **« Couverture par domaine »**
(sous-totaux `src/core/`, `src/net/`, `src/ui/`, `src/app/`) insérée entre
l'*overall* et le détail par fichier. Elle est calculée par
[`tests/coverage_by_domain.py`](coverage_by_domain.py) à partir du
`--json-summary` de gcovr (les sous-totaux se recoupent avec l'*overall*).

Sur macOS, gcov natif étant `llvm-cov`, la cible passe automatiquement
`--gcov-executable "llvm-cov gcov"` à gcovr ; sur Linux/Jetson le gcov natif est
utilisé directement.

En CI (`.github/workflows/ci.yml`), ces sorties alimentent trois visualisations :

- **Codecov** (badge, annotations) depuis `coverage.xml` — dépôt privé, donc le
  secret `CODECOV_TOKEN` est **obligatoire** (pas d'upload « tokenless »).
- **Commentaire de PR** + **Job Summary** : `coverage.md` publié via
  `actions/github-script` (Node 24, maintenu). 100 % GitHub, aucun service tiers,
  le code privé ne sort pas. Aperçu local : `cat tests/coverage/coverage.md`.
- **Artefact** `coverage-html` téléchargeable depuis la page du run.

## Ajouter un test

1. Écrire un `TEST mon_test(void) { ...; PASS(); }` dans le `test_<module>.c`
   adéquat, puis l'ajouter au `SUITE(<module>_suite)` du même fichier.
2. Pour un nouveau module : créer `tests/<domaine>/test_<module>.c` (même domaine
   que le module sous `src/`) avec sa `SUITE`, l'ajouter à `SUITE_EXTERN`/`RUN_SUITE`
   dans `test_main.c`, et compléter `TEST_SRCS` / `TEST_MODULES` dans le `Makefile`
   (en ajoutant les dépendances de link transitives du module).
3. **Si le module ajouté à `TEST_MODULES` ne vit pas sous `src/`** (le cœur pur
   d'un outil de `tests/tools/`, par exemple), l'ajouter aussi à
   `COV_TESTTREE_MODULES` : les boucles d'instrumentation de `coverage-256` /
   `coverage-16` ne balaient que `src/*/*.c`, alors que leur étape de link
   réclame `TEST_MODULES` en entier. L'oubli ne se voit ni à `make test` ni à
   `make test-docker` — seulement au job de couverture de la CI, par un
   « cannot find …/<module>.o ».

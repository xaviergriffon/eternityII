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

| Fichier | Couverture |
|---|---|
| `test_main.c` | Point d'entrée unique du runner (enregistre les suites). |
| `test_lifo.c` | `lifo.c` — `File` (put/scroll LIFO, cache) et `big_table` (croissance). |
| `test_part.c` | `part.c` — `rotatePart`, `search_max_face`, `search_face`, `copy_array_part`, `id_for_rotated_part`, `buildBigArray`/`get_one_part`. |
| `test_readdata.c` | `readdata.c` — `read_parts` (parsing CSV, chemin nominal). |

## Conventions et limites

- **Fixtures construites à la main** plutôt que via `rotate_all_parts` /
  `pieces.csv` : les tests restent indépendants de `ETERN_PARTS` (256/16) et de
  la présence d'un fichier de pièces dans le répertoire courant.
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
===== Couverture de code (modules testés) =====
  lifo.c         Lines executed:48.29% of 234
  part.c         Lines executed:50.77% of 388
  readdata.c     Lines executed:18.75% of 192
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
2. Pour un nouveau module : créer `tests/test_<module>.c` avec sa `SUITE`,
   l'ajouter à `SUITE_EXTERN`/`RUN_SUITE` dans `test_main.c`, et compléter
   `TEST_SRCS` / `TEST_MODULES` dans le `Makefile` (en ajoutant les dépendances
   de link transitives du module).

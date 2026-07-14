# Tests, couverture et intégration continue

Ce document décrit les cibles de test du Makefile, les scénarios d'intégration
bout-en-bout, le rejeu de la CI en local via Docker et ce que fait la CI GitHub
Actions. Les conventions d'écriture des tests unitaires (organisation des suites,
fixtures, ajout d'un test) sont dans [tests/README.md](../tests/README.md).

## Cibles

```sh
make test             # compile tests/ + lance la suite unitaire (code de sortie non nul si échec)
make test-integration # scénarios bout-en-bout 16 pièces : solution client/serveur + canal de contrôle
make test-docker      # rejoue les jobs de test CI dans un conteneur Linux (nécessite Docker)
make coverage         # les deux passes (256 + 16) + résumé texte gcovr fusionné (nécessite gcovr)
make coverage-256     # passe 256 pièces seule ; résumé gcov par module
make coverage-report  # rapports gcovr : Cobertura XML + HTML + résumé Markdown
```

## Tests unitaires (`make test`)

Les tests unitaires vivent dans `tests/` et utilisent
[greatest](https://github.com/silentbicycle/greatest) — un framework C
*single-header* vendoré dans `tests/greatest.h`, **sans dépendance externe** à
installer. Les suites sont rangées par domaine, en miroir de `src/` (`tests/core/`,
`tests/net/`, `tests/ui/`, `tests/app/`).

`make test` produit un binaire isolé (`tests/run_tests`) qui ne lie **que** les
modules testés et leurs dépendances — `src/app/main.c` n'est jamais inclus.

Voir [tests/README.md](../tests/README.md) pour les options du runner, les
conventions (fixtures à la main, tests de chemins `exit()` via `fork_assert.h`) et la
marche à suivre pour ajouter un test.

## Tests d'intégration (`make test-integration`)

Compile un binaire dédié (`ETERN_PARTS=16`, plateau 4×4) et enchaîne deux scripts
(`tests/integration/`), qui doivent tous deux passer :

- **`run_solution_16.sh`** — exercice réel du protocole de travail : serveur + client
  lancés avec `--stop-on-solution`, vérifie que les **deux côtés** voient la solution
  (logs, fichiers `solution_*`, backups `.back`, arrêt propre du serveur).
- **`run_control_channel.sh`** — exercice du
  [canal de contrôle](echanges_client_serveur.md#canal-de-contrôle-v9) : serveur +
  client sans arrêt automatique, pilote la console du serveur via une FIFO
  (`clientsStats`, `pause`, `resume`) et vérifie le round-trip complet dans les deux
  journaux, avant un arrêt déterministe par la commande `exit`.

Chaque script tourne dans un répertoire temporaire isolé (`mktemp -d`, rien ne
retombe dans le dépôt) avec un timeout borné (`INTEGRATION_TIMEOUT`, 60 s par script
par défaut).

## Couverture de code

`make coverage` recompile en mode instrumenté, lance les deux passes (256 et
16 pièces) et affiche via [gcovr](https://gcovr.com) le pourcentage de lignes
couvertes — sur **tout le code de production** (les modules non exercés ressortent à
0 %). Le détail ligne par ligne est dans `tests/coverage/<module>.c.gcov` (lignes
jamais exécutées marquées `#####`).

`make coverage-report` produit en plus un rapport HTML navigable
(`tests/coverage/html/`), un Cobertura XML (`coverage.xml`, consommé par Codecov) et
un résumé Markdown (`coverage.md`), enrichi de sous-totaux par domaine par
[tests/coverage_by_domain.py](../tests/coverage_by_domain.py).

Voir [tests/README.md](../tests/README.md#couverture-de-code) pour le détail
(prérequis gcovr, spécificités macOS/llvm-cov, artefacts).

## Tests sous Linux via Docker (`make test-docker`)

Si [Docker](https://www.docker.com) est installé, `make test-docker` rejoue en local
les jobs de test de la CI dans un conteneur **identique au runner GitHub** (image
[tests/docker/Dockerfile](../tests/docker/Dockerfile), épinglée sur `ubuntu:24.04`
avec gcc/make/gcov et gcovr) : build `WERROR=1`, tests unitaires, passe
AddressSanitizer et tests d'intégration client/serveur.

C'est le moyen de détecter **avant de pousser** les écarts entre macOS/clang et
Linux/gcc (diagnostics `-Werror` plus stricts, over-reads vus par ASan sous Linux
seulement, glibc vs libSystem). Le dépôt est monté en lecture seule et copié dans le
conteneur : les artefacts Linux ne se mélangent jamais à ceux du poste hôte. La
séquence est surchargeable pour rejouer un seul job :

```sh
make test-docker DOCKER_TEST_CMD="make test ASAN=1"
```

## Intégration continue

À chaque push et pull request, [GitHub Actions](../.github/workflows/ci.yml) :

- compile le build de production (`make WERROR=1`), lance les tests unitaires
  (`make test`), les tests d'intégration (`make test-integration`) et les rapports de
  couverture (`make coverage-report`, via gcovr) ;
- publie la couverture : envoi à Codecov (Cobertura), commentaire de couverture sur
  la PR + récapitulatif du run (Job Summary), et rapport HTML en artefact
  téléchargeable ;
- **compile toutes les combinaisons du code**, chacune avec `WERROR=1` (tout warning
  bloque la CI), pour qu'aucun chemin compilé sous condition ne se désynchronise en
  silence : la variante ncurses (`make NCURSES=1`), la variante CUDA (`make CUDA=1`
  puis `make CUDA=1 VERIFY=1`), un build activant **tous** les flags `DEBUG_*` de
  [src/app/static_variables.h](../src/app/static_variables.h) à la fois, et les
  configurations alternatives `ETERN_PARTS=16` (plateau 4×4) et `FORWARD_CHECK_K=0`
  (forward-checking retiré). Toutes pilotées via `CPPFLAGS` (`-D…`), sans toucher la
  source — ces `#define` sont gardés par `#ifndef` pour être surchargeables (voir
  [Compilation](compilation.md#configuration-du-puzzle)).

Le toolkit CUDA est installé sur le runner (action `Jimver/cuda-toolkit`) pour la
**compilation** seule : les runners GitHub n'ayant pas de GPU NVIDIA, le binaire CUDA
n'est pas exécuté (la validation fonctionnelle se fait sur Jetson). Il en va de même
pour les autres variantes : ce sont des contrôles de compilation/édition de liens,
pas des exécutions.

## Voir aussi

- [tests/README.md](../tests/README.md) — organisation des suites, conventions, ajout d'un test.
- [Compilation](compilation.md) — options de build et drapeaux de configuration.

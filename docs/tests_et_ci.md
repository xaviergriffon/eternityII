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

## Banc de mesure du débit de recherche (`tests/bench/bench_search.sh`)

`make test`/`coverage` valident la correction ; ils ne disent rien du **débit** de
la boucle chaude (`autosearch()`, `src/core/etii_search.c`). Le banc de mesure sert
de préalable à toute optimisation mémoire ou algorithmique de cette boucle : il
donne un chiffre (nœuds/s) comparable avant/après un changement.

### Critère d'arrêt par nombre de nœuds

Mesurer un débit pendant une durée fixe est bruité (charge de la machine, jitter
de l'ordonnanceur). Le banc utilise l'inverse : mesurer le temps mural nécessaire
pour explorer un nombre de **nœuds fixe**. En mode `test`, la recherche est
déterministe (mono-processus, sans réseau), donc à N fixé le travail exploré est
le même d'un run à l'autre.

Activation par variable d'environnement `ETII_BENCH_NODES=<N>` — volontairement
**pas** une option CLI : le banc est hors du chemin de production, il n'a donc
pas besoin d'entrée dans `cli_topics[]` (`src/app/app_runtime.c`). Absente (cas par
défaut), elle ne change rien au comportement du binaire.

```sh
ETII_BENCH_NODES=5000000 ./eternityII test data/pieces.csv
# ...
# ETII_BENCH nodes_reached=5006012 target=5000000
```

Le critère d'arrêt est vérifié par le thread de statistiques
(`check_client_threads`, `src/app/etii_client.c`), qui échantillonne déjà
`counters[]` — **aucun test n'est ajouté à la boucle chaude**. Quand le banc est
actif, ce thread sonde toutes les 1 ms (au lieu des 10 s habituels) pour que le
dépassement inévitable de la cible reste une fraction négligeable de N ; le
nombre de nœuds *réellement* atteint (toujours ≥ N) est celui à utiliser pour
calculer le débit, jamais N lui-même. En mode `test`, le débit artificiel de
100000 coups/s (pensé pour un usage interactif) est aussi désactivé quand le
banc est actif, pour mesurer le débit brut de la machine.

La décision d'arrêt (`bench_should_stop`) et le parsing de la variable
d'environnement (`bench_parse_nodes_env`) sont des fonctions pures
(`src/app/static_variables.h`/`.c`), testées unitairement dans
`tests/app/test_static_variables.c`.

### Élagage forward-check inline

`autosearch()` élague aussi ses branches en ligne, sans réseau : à chaque
placement candidat, `bt_forward_check` (`src/core/etii_search.c`) teste les
`FORWARD_CHECK_K` cases suivantes (défaut 6) avant de s'engager plus loin. Ce
mécanisme est distinct du process `pruner` séparé (qui vérifie des
possibilités reçues du serveur par lots réseau) — il n'est pas couvert par ce
banc et n'a pas besoin de l'être : son coût est entièrement inclus dans le
temps mesuré, puisqu'il s'exécute dans la même boucle chaude.

Le banc journalise en plus, sur la même ligne `ETII_BENCH`, `fc_attempts` et
`fc_pruned` (lus par `bench_poll_and_maybe_stop`, `src/app/etii_client.c`, via
`__atomic_load_n` — pas de nouveau coût dans `bt_forward_check` lui-même). Le
script en tire un **taux d'élagage** (`fc_pruned / fc_attempts`), rapporté en
plus du débit dans le JSON (`fc_prune_rate_pct_median`/`_min`/`_max`) et dans
la comparaison `--baseline`. C'est un second garde-fou, complémentaire du
débit : un changement de mise en page mémoire peut accélérer la boucle sans
changer le comportement de l'élagage (taux stable), ou au contraire modifier
l'ordre d'exploration et donc le taux — un signal que le changement n'est pas
sémantiquement neutre. Absent des logs (et donc du rapport) sur un build
`FORWARD_CHECK_K=0`.

### Le script

```sh
tests/bench/bench_search.sh                                   # 5 000 000 nœuds × 5 répétitions
tests/bench/bench_search.sh --nodes 2000000 --reps 10          # cible/répétitions personnalisées
tests/bench/bench_search.sh --out rapport.json                 # sauvegarde le rapport JSON
tests/bench/bench_search.sh --baseline rapport.json            # compare au rapport précédent
```

Le script :

1. recompile en release (`make clean && make`, sans ASan ni couverture) ;
2. fait un run de chauffe non comptabilisé ;
3. enchaîne `--reps` répétitions (défaut 5) et calcule médiane/min/max/écart-type
   relatif du débit (nœuds/s) — un écart-type relatif élevé (> 5 %) est signalé
   comme une mesure trop bruitée pour conclure ;
4. épingle le process sur le cœur 0 via `taskset` si l'outil est disponible
   (Linux) ; sur macOS, qui n'a pas d'équivalent, l'affiche explicitement dans le
   rapport plutôt que d'échouer silencieusement ;
5. affiche le load average 1 min et refuse de tourner si la machine est chargée
   (au-delà d'1× le nombre de cœurs), sauf `--force` ;
6. affiche le nombre de nœuds réellement atteint (médiane/min/max) — repère de
   non-régression fonctionnelle : à cible et code identiques, il doit rester très
   proche d'un run à l'autre (l'intervalle de sondage à 1 ms borne l'écart à une
   fraction de pourcent) ; un écart brutal signale un changement de comportement
   de la recherche, pas seulement de son débit ;
7. en mode `--baseline`, relit un rapport JSON précédent et affiche le delta en
   pourcentage du débit médian.

Le rapport JSON (affiché sur stdout, et copié vers `--out` si fourni) est
volontairement à plat (pas d'objets imbriqués) pour rester parsable avec
`grep`/`sed`/`awk` seuls, sans dépendance à `jq` ou Python — dans le même esprit
que le reste du projet (voir l'API HTTP REST, dépendance-free par choix).

## Voir aussi

- [tests/README.md](../tests/README.md) — organisation des suites, conventions, ajout d'un test.
- [Compilation](compilation.md) — options de build et drapeaux de configuration.
- `tests/bench/bench_search.sh` — banc de mesure du débit de recherche (voir ci-dessus).

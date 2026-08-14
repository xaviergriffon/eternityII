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
make test-docker-arm  # vérifie la compilation croisée ARM 64-bit (Raspberry Pi) dans le même conteneur (nécessite Docker)
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

### Le conteneur tourne en root : tests sautés

Une différence subsiste volontairement avec la CI : le conteneur exécute les tests
**en root**, là où le runner GitHub tourne sous l'utilisateur non privilégié
`runner`. Or root outrepasse les bits de permission (`CAP_DAC_OVERRIDE` sous Linux) :
les tests qui posent un `chmod(dir, 0444)` pour vérifier qu'une écriture **échoue**
voient au contraire leur `fopen()` réussir, et l'assertion tombe. C'est le cas de
`do_command_line_print_fails_on_unwritable_dir` et de
`do_command_line_backup_fails_on_unwritable_dir`
([tests/ui/test_command_lines.c](../tests/ui/test_command_lines.c)) — le premier
faisait échouer `make test-docker` en permanence, masquant les vrais échecs que
cette cible sert justement à révéler.

Ces deux tests commencent donc par `SKIP_IF_ROOT()`, qui les saute via `SKIPm` quand
`geteuid() == 0` — même logique que le `SKIPm("chmod non supporté sur cet
environnement")` déjà présent en cas d'échec du `chmod` lui-même : quand le postulat
de permission ne tient pas, le test n'a rien à prouver. Sous `make test-docker` ils
apparaissent donc en `skipped` (et non en `failed`) ; sur le poste hôte comme en CI,
tous deux s'exécutent normalement.

L'alternative — faire tourner le conteneur sous un utilisateur non-root (`USER` dans
le Dockerfile, ou `--user` dans la cible `test-docker`) — rapprocherait le conteneur
de la CI et **exécuterait** réellement ces tests, mais au prix d'une divergence de
propriétaire entre la copie `/work` et l'utilisateur du build. Le garde-fou dans le
test reste par ailleurs utile en soi : il protège tout environnement root, conteneur
maison compris.

## Compilation croisée ARM (`make test-docker-arm`)

Les diagnostics gcc ne sont pas portables d'une architecture à l'autre : un build
propre sous macOS/clang et sous la CI x86_64/gcc peut quand même avertir sur
ARM/gcc. `__builtin_object_size` (qui alimente `-Wstringop-truncation` et
`-Wformat-truncation`) calcule ses bornes différemment selon l'architecture sous
`-Ofast`, et peut retomber sur une borne pessimiste (le tableau englobant plutôt
que le sous-objet indexé) sur une cible et pas une autre — c'est exactement ce qui
s'est produit sur `http_known_clients_collect`/`http_clients_collect`
([src/net/http_server.c](../src/net/http_server.c)) : `snprintf` déclenchait
`-Wformat-truncation` sur un Raspberry Pi (aarch64) en restant muet partout
ailleurs, corrigé en passant à `memcpy` (qui ne fait aucun raisonnement de
longueur de chaîne) plus des `_Static_assert` verrouillant l'hypothèse de tailles
identiques source/destination dont ce remplacement dépend.

Détecter cette classe de bug ne demande ni matériel ARM ni émulation QEMU — seul
le *compilateur* doit connaître le jeu d'instructions cible. `tests/docker/Dockerfile`
embarque donc en plus `crossbuild-essential-arm64` (`gcc-aarch64-linux-gnu` +
`libc6-dev-arm64-cross`), et le Makefile expose une variable `CC` (`?= gcc`,
surchargeable) branchée uniquement sur les deux règles qui produisent l'exécutable
de production (règle motif + édition de liens finale) — les binaires de
test/couverture restent liés avec le gcc de l'hôte, puisqu'ils doivent encore
*s'exécuter* localement :

```sh
make test-docker-arm                              # utilise aarch64-linux-gnu-gcc
make test-docker-arm DOCKER_ARM_CC=<autre-gcc>     # toolchain croisé alternatif
make test-docker-arm DOCKER_ARM_TEST_CMD="…"       # commande de remplacement
```

C'est une vérification de **compilation + édition de liens uniquement** : l'ELF
aarch64 produit n'est pas exécutable sur le conteneur x86_64, donc ceci révèle des
diagnostics du compilateur, pas un comportement à l'exécution — un vrai Raspberry
Pi reste la référence pour ça.

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
  [src/app/static_variables.h](../src/app/static_variables.h) à la fois, la
  compilation croisée ARM 64-bit (`make CC=aarch64-linux-gnu-gcc WERROR=1`, job
  `arm64-build` — même principe que `make test-docker-arm` mais toolchain installée
  directement sur le runner, sans Docker), et les configurations alternatives
  `ETERN_PARTS=16` (plateau 4×4) et `FORWARD_CHECK_K=0` (forward-checking retiré).
  Toutes pilotées via `CPPFLAGS` (`-D…`), sans toucher la source — ces `#define` sont
  gardés par `#ifndef` pour être surchargeables (voir
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
voisines géométriques encore vides de la case posée (au plus 4) avant de
s'engager plus loin — voir
[autosearch_step.md §1.3 ter](autosearch_step.md#13-ter-bt_forward_check--les-voisines-de-la-pièce-posée-pas-une-fenêtre-de-parcours).
Ce mécanisme est distinct du process `pruner` séparé (qui vérifie des
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

### `max_result` : le débit seul ne prouve pas un vrai gain

**Piège concret, rencontré en pratique.** Un changement qui réduit la portée du
forward-check (ex. inspecter moins de cases par placement) peut faire
progresser `nodes/s` ET, simultanément, faire élaguer une part légèrement
différente des branches — observable comme « plus de coups, moins
d'éliminations » à l'exécution. Le débit seul ne dit pas si c'est un vrai gain
(le même travail utile, fait plus vite) ou un gain en trompe-l'œil (un arbre
plus large exploré plus vite, donc *pas moins* de temps réel jusqu'à un
résultat donné). Le **taux d'élagage** (`fc_pruned / fc_attempts`) ne tranche
pas non plus : une variation de quelques dixièmes de point ne dit rien de son
effet cumulé sur la taille réelle de l'arbre exploré.

Le témoin décisif est `max_result` (profondeur maximale atteinte,
`etii_search.c`) **à cible de nœuds FIXE** : si deux versions atteignent la
même profondeur maximale pour le même nombre de nœuds explorés, alors le
travail utile accompli est identique — un débit plus élevé pour arriver au
même point est un gain réel, pas un artefact de comptage. Journalisé sur la
même ligne `ETII_BENCH` (`max_result=<n>`), agrégé par le script dans le JSON
(`max_result_median`/`_min`/`_max`) et comparé par `--baseline` **seulement
quand `nodes_target` est identique entre les deux rapports** — à cibles
différentes, plus de nœuds donnant mécaniquement plus de profondeur
indépendamment de tout changement d'élagage, la comparaison n'est
qu'indicative.

**Vérifié pour PR1** (voisines géométriques plutôt que fenêtre de parcours,
[docs/conception/elagage_recherche.md](conception/elagage_recherche.md) §4.1) :
`max_result` atteint exactement **74** avant et après, à quatre cibles de
nœuds différentes (20 M, 50 M, 200 M, 500 M) — la même profondeur réelle,
obtenue à chaque fois en 39 à 45 % de temps mural en moins. C'est cette mesure,
et non le seul débit, qui confirme que le gain de PR1 est réel.

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

### Validation du temps mesuré (bogue d'arrondi de bash)

Le temps de chaque run vient de `time` avec `TIMEFORMAT='%R'`. **Bash lui-même
peut y imprimer une valeur malformée** : `timeval_to_secs` (`execute_cmd.c`)
convertit les µs en ms avec arrondi mais **sans propager la retenue vers les
secondes**, si bien qu'à partir de 999500 µs la fraction vaut 1000 ; `mkfmt`
imprime ensuite chaque chiffre par `(fraction / 100) + '0'`, soit 10 + `'0'` =
`:`. Un run de 2,9997 s est donc rapporté `2.:00` au lieu de `3.000` (reproduit
sur bash 5.3, fenêtre de 500 µs par seconde ≈ 0,05 % des runs).

Les conséquences ne sont pas proportionnelles à la rareté du cas : `awk` lit
`2.:00` comme 2.0, le débit du run est surestimé de 50 % (10,0 M nœuds/s au lieu
de 6,7 M), cette valeur devient le `nodes_per_sec_max` du rapport et gonfle
l'écart-type relatif (17,55 % au lieu de ~1,6 %) — et le JSON produit
(`"elapsed_sec":2.:00`) n'est plus parsable. La médiane absorbe l'incident à
`--reps 5`, plus forcément à `--reps 3`.

Le banc valide donc le temps avant de l'utiliser (`bench_parse_elapsed`,
`tests/bench/bench_lib.sh`) : décimal strict et strictement positif — un simple
test de non-vacuité laissait passer `2.:00`. Quand la valeur est refusée, le run
est **rejoué** (`bench_retry_valid_time`, 3 tentatives, chaque rejeu étant
journalisé) plutôt que comptabilisé : la cause dépend de la durée mesurée à
500 µs près, donc une nouvelle exécution retombe presque certainement sur une
valeur correcte. Si les 3 tentatives échouent — cause systématique plutôt que
fortuite : message du shell atterrissant dans le fichier de temps, `TIMEFORMAT`
écrasé, `time` externe au lieu du mot-clé — le banc s'arrête avec un code
d'erreur et ne publie aucun rapport, plutôt que de rapporter un débit faux.

Ces deux fonctions sont pures et couvertes par `tests/bench/test_bench_parse.sh`
(lancé par `make test`, cible `test-bench`) — même démarche que pour
`bench_should_stop` côté C.

## Banc de réfutation (`make bench-refutation`)

`bench_search.sh` mesure un **débit** (nœuds/s) et, comme garde-fou, la
profondeur atteinte (`max_result`). Ces deux grandeurs restent des **proxys** :
le débit ne dit pas si les nœuds explorés servent à quelque chose, et
`max_result` récompense le fait de descendre loin dans une branche — or
descendre loin dans une branche qui ne mène nulle part n'est pas l'objectif du
solveur. Le vrai travail du moteur est l'inverse : établir **le plus tôt
possible** qu'une possibilité est morte, pour ne jamais développer son
sous-arbre.

`tests/bench/bench_refutation.c` mesure exactement cela : le coût (nœuds et
temps) jusqu'à `BT_CORE_EXHAUSTED` — le sous-arbre entièrement exploré, donc
**mort prouvé** — à racine IDENTIQUE entre les deux ordres de parcours. C'est une
comparaison appariée : les deux moteurs traitent le même sous-arbre, seul l'ordre
change. La primitive est celle de la preuve bornée du pruner (§4.6b) : un plafond
de nœuds, et trois issues possibles (`FERMÉ`, `budget` = indéterminé, `arrêt`).

```sh
# Racines d'un VRAI stock serveur (fichier .back), filtrées par profondeur
make bench-refutation BENCH_REFUT_ARGS="--from-back /chemin/temp.back --min-pieces 90 --max-roots 25 --budget 5000000"
# Racines fabriquées : préfixes d'une descente MRV profonde
make bench-refutation BENCH_REFUT_ARGS="--depths 100,110,120 --budget 40000000"
```

### D'où viennent les racines — et pourquoi un backup serveur est la bonne source

Une racine utile doit avoir **beaucoup de suites mais aucune solution**, et être
assez petite pour être fermée dans un budget raisonnable. Un **backup d'un
serveur en cours** (`temp.back`, `eternityII.back`) est la source la plus
représentative qui soit : c'est littéralement le travail que le serveur
distribue. `--from-back` les lit directement (`fread` de `possibility_packet`,
comme `import()`), affiche le profil de profondeur du stock, et `--min-pieces`/
`--max-pieces` permettent de sélectionner une bande.

Sur un stock réel produit par un serveur (`--expand-level 3`) alimenté 60 s par un
client à ordre fixe : **17 815 possibilités, de 8 à 153 pièces posées, moyenne
34,5**. Le stock contient donc bien des racines profondes exploitables —
contrairement à ce que la mesure de §4.6b laissait croire (elle bornait à
`alloc` ≈ 72, c'est-à-dire le CURSEUR de parcours et non le nombre de pièces).

À défaut de backup, le banc fabrique des racines : une descente MRV produit un
plateau profond, dont on extrait des préfixes (`--depths`) — utile pour
construire des cas **durs** (sous-arbre réellement vivant), que le stock réel
fournit rarement.

### Résultats (moteur à ordre fixe vs MRV, plafond 5 M nœuds)

Sur le stock réel ci-dessus :

| Bande (pièces posées) | Fermées, ordre fixe | Fermées, MRV | Nœuds sur les racines fermées par les DEUX |
|---|---|---|---|
| 20–45 | 5/10 | **10/10** | 70 vs 22 |
| 55–89 | 16/16 | 16/16 | 40 804 vs **32** (×1 275) |
| ≥ 90 | 17/25 | **25/25** | 565 677 vs **25** (×22 627) |

Sur les racines **fabriquées** (préfixes, plafond 40 M nœuds), la conclusion
s'inverse :

| Profondeur | Ordre fixe | MRV |
|---|---|---|
| 100 pièces | FERMÉ, 73 482 nœuds, 0,004 s | FERMÉ, **4 443 906** nœuds, 5,17 s |
| 110 pièces | FERMÉ, 63 029 nœuds | FERMÉ, 78 547 nœuds |
| 120 pièces | FERMÉ, 19 391 nœuds | FERMÉ, **1 403** nœuds |

**Lecture.** Sur ce que le serveur distribue réellement, MRV réfute massivement
plus vite, et surtout il ferme des racines que l'ordre fixe ne ferme pas du tout
dans le plafond. Mais son ordre n'est pas uniformément meilleur : sur un
sous-arbre réellement vivant (préfixe à 100 pièces), il explore 60× plus de
nœuds que l'ordre fixe pour le même résultat. Les deux faits sont vrais et
doivent être cités ensemble.

**Nuance importante, à ne pas passer sous silence** : une grande partie des
réfutations MRV coûtent **1 nœud**. La possibilité était déjà morte au moment où
le serveur l'a créée, et le balayage de plateau de `mrv_choose_cell` le voit
immédiatement — c'est le même test que `possibility_all_has_a_next_counted`
(le pruner), mais appliqué à CHAQUE nœud au lieu d'une seule fois. Cela dit
aussi quelque chose du stock lui-même : sans pruner en service, un serveur
accumule du travail déjà mort. Comparer les deux moteurs sur ces racines-là
mesure surtout la présence de ce test global, pas la qualité de l'ordre.

## Voir aussi

- [tests/README.md](../tests/README.md) — organisation des suites, conventions, ajout d'un test.
- [Compilation](compilation.md) — options de build et drapeaux de configuration.
- `tests/bench/bench_search.sh` — banc de mesure du débit de recherche (voir ci-dessus).

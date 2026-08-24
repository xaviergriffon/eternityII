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

### Le « mur à `max_result` ≈ 74 » est un artefact DE CE BANC, pas des moteurs

Conséquence directe de ce que ce banc a permis de mesurer, et qui corrige une
affirmation propagée par plusieurs sections de
[conception/elagage_recherche.md](conception/elagage_recherche.md) : le plafond à
74 pièces posées est une propriété du **protocole de mesure** de
`bench_search.sh` — mono-processus, depuis la genèse, sans stock ni délégation,
donc une seule descente en profondeur piégée dans le sous-arbre le plus à
gauche. Contre un vrai serveur (expansion + délégation répartissant le travail),
un client à **ordre fixe** atteint `max_result` = 186 et délègue des paquets à
153 pièces posées ; un client MRV atteint 209 et délègue jusqu'à 186.

Ce qui reste vrai : à protocole IDENTIQUE (le banc), MRV atteint 186 là où
l'ordre fixe plafonne à 74. Ce qui est faux : en déduire une propriété des
moteurs hors du banc — et, surtout, écarter un mécanisme d'élagage au motif que
« la profondeur atteinte reste trop faible pour qu'il joue » (le raisonnement de
§4.4 et §4.6b), alors que la profondeur en question était celle du banc.

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
34,5** ; par un client MRV pendant 90 s : **29 481 possibilités, de 8 à 186
pièces posées, moyenne 72,3**. Le banc contrôle au passage l'intégrité du stock
(`check_possibility` + `normalize_possibility_packet` sur chaque paquet) : **0
incohérent, 0 non canonique** dans les deux cas — c'est la vérification, sur
données réelles et à l'échelle (29 481 paquets), de la re-canonisation des
paquets délégués en ordre dynamique (§4.7). Le stock contient donc bien des racines profondes exploitables —
contrairement à ce que la mesure de §4.6b laissait croire (elle bornait à
`alloc` ≈ 72, c'est-à-dire le CURSEUR de parcours et non le nombre de pièces).

À défaut de backup, le banc fabrique des racines : une descente MRV produit un
plateau profond, dont on extrait des préfixes (`--depths`) — utile pour
construire des cas **durs** (sous-arbre réellement vivant), que le stock réel
fournit rarement.

### Trois moteurs, pas deux : l'ablation qui sépare les deux axes

Les deux moteurs de production **confondent deux axes indépendants** :

| | détection de case morte **locale** (4 voisines) | détection **globale** (tout le plateau) |
|---|---|---|
| **ordre fixe** | moteur historique (`fixe`) | `fixe+global` — l'ablation |
| **ordre dynamique** | (dégénéré : le balayage la donne gratuitement) | moteur MRV (`MRV`) |

Ne comparer que les deux coins opposés ne dit pas lequel des deux axes produit
l'effet mesuré. D'où le drapeau `global_dead_check` (`src/app/static_variables.h`,
défaut 0, coût nul quand il vaut 0) : il fait appeler à l'ordre FIXE exactement
le même balayage que MRV (`mrv_choose_cell`) en **jetant le choix de case**, ne
gardant que le test de mort. `--engines fixe,fixe+global,mrv` sélectionne les
variantes comparées. Verrou de correction : le balayage global est une condition
nécessaire, il ne doit coûter aucune solution —
`search_backtracking_global_dead_check_preserves_solution_count`
(`tests/core/test_etii_search.c`), exploration exhaustive du vrai 4×4 avec et
sans, même nombre de solutions.

### Résultat 1 — KPI de production, à TEMPS CPU ÉGAL

120 racines échantillonnées régulièrement dans un stock serveur réel (1 sur 148,
**aucun filtre de profondeur** : c'est ce que le serveur sert vraiment). Le
plafond par racine est calibré **par moteur** pour que chacun dépense le même
temps total (~22 s) — sans quoi le moteur qui renonce le plus vite paraît le plus
« efficace » simplement parce qu'il abandonne moins cher :

| moteur | plafond/racine | fermées | temps total | fermetures/s |
|---|---|---|---|---|
| `fixe` | 2 500 000 nœuds | 20/120 | 21,97 s | 0,91 |
| `fixe+global` | 275 000 nœuds | 52/120 | 23,02 s | 2,26 |
| `MRV` | 500 000 nœuds | **79/120** | 22,67 s | **3,48** |

À temps CPU égal, MRV résout **4× plus** du stock que l'ordre fixe. Et l'ablation
répartit le mérite : la détection globale seule fait passer 20 → 52 (×2,6), l'ordre
dynamique ajoute 52 → 79 (×1,5). **Aucun des deux axes n'est redondant** — l'effet
de l'ordre ne s'explique pas par le seul test global, contrairement à l'hypothèse
que les réfutations à 1 nœud suggéraient.

Comparaison appariée (mêmes racines, plafond commun de 500 k nœuds, les 19 que les
trois moteurs ferment) : `fixe` 295 339 nœuds / 0,026 s, `fixe+global` 124 030
nœuds / 0,159 s, `MRV` **40 nœuds** / ~0 s. Noter que `fixe+global` explore moins
de nœuds que `fixe` mais met plus de TEMPS : le balayage coûte environ 10× le prix
d'un nœud ordinaire. C'est le coût que MRV paie aussi — et qu'il rentabilise.

### Résultat 2 — le contre-exemple : racines réellement vivantes

Sur des racines **fabriquées** (préfixes d'une descente MRV, `--seed-nodes 200000
--depths 100,110,120`), c'est-à-dire des sous-arbres que rien ne tue d'emblée :

| moteur | nœuds (3 racines) | temps |
|---|---|---|
| `fixe` | 155 902 | 0,012 s |
| `fixe+global` | 134 218 | 0,112 s |
| `MRV` | **4 523 856** | **5,094 s** |

Ici MRV est 29× pire en nœuds et 400× pire en temps — et l'ablation montre que
c'est bien l'**ordre** qui est en cause, pas le balayage : `fixe+global` n'élague
presque rien de plus que `fixe` (134 k contre 156 k nœuds) sur ces racines. Le
gain de MRV n'est donc pas universel : il tient à la structure du stock réel
(beaucoup de possibilités déjà mortes ou presque), pas à une supériorité
intrinsèque de l'ordre en toutes circonstances.

### Nuance à ne pas passer sous silence

Une grande partie des réfutations coûtent **1 nœud** : la possibilité était déjà
morte quand le serveur l'a créée, et le balayage de plateau le voit
immédiatement — c'est le même test que `possibility_all_has_a_next_counted`
(le pruner), mais joué à chaque nœud au lieu d'une fois par possibilité. Cela dit
aussi quelque chose du stock : **sans pruner en service, un serveur accumule du
travail déjà mort** en quantité (ici 52/120 racines tuées par le seul test global).
Sur ces racines-là, la comparaison mesure la présence de ce test, pas la qualité
de l'ordre — d'où l'importance de la comparaison appariée ci-dessus, où MRV garde
un avantage de 3 ordres de grandeur.

### Quatrième moteur : `fixe+singleton`, la remesure de §4.4

Le conflit de singletons (§4.4 de
[elagage_recherche.md](conception/elagage_recherche.md), théorème de Hall
`|S| = 2`) avait été implémenté, mesuré à **0 déclenchement** sur 500 M nœuds
via `bench_search.sh` (protocole mono-processus depuis la genèse), puis
reverté. Même erreur de méthode que le mur à 74 et que la première mesure de
§4.6b : ce protocole ne construit jamais de sous-arbre RÉEL, profond, dérivé
d'une vraie délégation — il ne pouvait pas voir le mécanisme se déclencher
même s'il se déclenche réellement ailleurs.

Réimplémenté derrière `singleton_conflict_check` (`src/app/static_variables.h`,
défaut 0, vit dans `bt_forward_check`, donc actif pour les DEUX moteurs de
recherche) et ajouté au banc comme quatrième variante (`--engines
fixe,fixe+singleton`). Deux mesures distinctes, parce que ce sont deux
questions différentes :

**1. Fermeture bornée (même protocole que §4.6b) : aucun effet.** Sur 120
racines réelles, budget 5 000 000 nœuds : `fixe` et `fixe+singleton` ferment
exactement le **même nombre** de racines (20/120), avec des totaux de nœuds
**identiques au nœud près** sur le sous-ensemble commun (1 326 277 pour les
deux). Pourtant `fc_singleton_conflict` (compteur direct, pas une inférence)
rapporte **35 056 déclenchements** sur cette même exécution — le mécanisme
tire bien, mais exclusivement dans des sous-arbres qui dépassent le budget de
toute façon : jamais dans les 20 qui ferment. Reproduit sur un second stock
(client MRV) à 134 565 déclenchements, avec cette fois un effet marginal sur
le sous-ensemble commun (6 330 646 contre 6 334 296 nœuds, −0,06 %) — dans le
bruit, sans conséquence sur aucune décision de fermeture.

**2. Débit agrégé sur l'échantillon entier (même instrument que la mesure
originale, sur stock réel plutôt que synthétique) : coût confirmé.**

| Stock | `fixe` (nœuds/s) | `fixe+singleton` (nœuds/s) | Delta |
|---|---|---|---|
| Client à ordre fixe, 120 racines, budget 5 M | 11 775 963 | 10 500 749 | **−10,8 %** |
| Même stock, 60 racines, budget 10 M | 10 898 683 | 9 861 136 | **−9,5 %** |
| Client MRV, 120 racines, budget 5 M | 15 746 462 | 13 949 110 | **−11,4 %** |

Trois mesures indépendantes, deux stocks, un coût de **−9,5 à −11,4 %** —
cohérent avec le −9 % mesuré à l'origine sur le protocole synthétique. La
COMBINAISON des deux mesures explique ce que l'original ne pouvait pas voir :
le mécanisme se déclenche réellement (des dizaines de milliers de fois par
échantillon) mais uniquement au fond de sous-arbres trop grands pour être
fermés dans les budgets testés — c'est-à-dire jamais là où une preuve de
fermeture aurait pu en profiter — tout en payant son coût de balayage (un
deuxième candidat cherché au lieu de s'arrêter au premier) sur CHAQUE appel,
y compris les 98 % qui ne mènent à rien.

**Verdict : la décision de ne pas fusionner ce mécanisme est CONFIRMÉE, mais
la raison invoquée à l'origine (« ne se déclenche jamais ») est CORRIGÉE.** Il
se déclenche, sur du stock réel, à un rythme mesurable — simplement jamais là
où ça compterait pour une preuve de fermeture, et son coût par nœud reste
supérieur à tout bénéfice observé sur trois mesures indépendantes. Voir §4.4
du document de conception pour la trace complète.

### Mode `--pruner-profile` : rejoue le VRAI pipeline du pruner

Les modes précédents mesurent des moteurs de RECHERCHE. `--pruner-profile <n>`
mesure le PRUNER lui-même : il rejoue, possibilité par possibilité, exactement
la logique d'`autoprune_step` (`src/core/etii_search.c`) — contrôle superficiel
(`possibility_all_has_a_next_counted`), puis, seulement si vivant et pas encore
`checked`, la preuve de fermeture bornée (`search_packet_backtracking_budgeted`,
§4.6b) — sur `n` possibilités échantillonnées régulièrement dans un `.back`, et
rapporte la répartition : mortes au contrôle superficiel (gratuit), fermées par
la preuve DFS (coûteux, borné par `--budget`), solutions rencontrées, survivent
intactes.

```sh
make bench-refutation BENCH_REFUT_ARGS="--from-back temp.back --pruner-profile 500 --budget 10000"
```

Sur le stock réel de 17 815 possibilités (§ ci-dessus, produit par un client à
ordre fixe), échantillon de 500, plafond DFS variable :

| Budget DFS | Mortes au contrôle superficiel | + Fermées par DFS | Survivent | Coût DFS cumulé |
|---|---|---|---|---|
| 0 (désactivé) | 50,2 % | — | 49,8 % | — |
| 10 000 | 50,2 % | +4,6 pt (54,8 % cumulé) | 45,2 % | 2 277 016 nœuds, 0,195 s |
| 100 000 | 50,2 % | +5,2 pt (55,4 %) | 44,6 % | 22 457 285 nœuds, 1,887 s |
| 1 000 000 | 50,2 % | +5,6 pt (55,8 %) | 44,2 % | 221 842 743 nœuds, 18,590 s |

Sur un second stock, produit par un client MRV (possibilités en moyenne plus
profondes, donc déjà davantage travaillées avant délégation) : 16,3 % mortes au
contrôle superficiel, +2,3 pt à budget 10 000, +5,7 pt à budget 1 000 000.

**Ceci corrige directement la mesure originale de §4.6b** (« 0 % de fermeture à
n'importe quel budget testé, jusqu'à 1 000 000 de nœuds ») : sur du VRAI stock
serveur, la preuve DFS ferme bien des possibilités, de façon reproductible sur
deux générations de stock différentes. La cause de la mesure originale n'était
pas un défaut du mécanisme mais la profondeur du stock synthétique utilisé —
voir la correction en §4.6b du document de conception.

**Rendements décroissants nets** : passer de 10 000 à 1 000 000 nœuds (×100)
n'apporte qu'un point de pourcentage de fermetures en plus pour ~100× le coût
CPU. 10 000 nœuds capture 82 % du gain total mesuré à 1 M pour 1 % du coût — un
bon point de départ si ce mécanisme est réactivé.

**Ce que ce mode donne aussi, gratuitement : la quantification de « faites
tourner un pruner ».** Le taux « mortes au contrôle superficiel » (50,2 % sur le
premier stock) mesure exactement ce qu'un pruner en service éliminerait sans
même la preuve DFS — puisque c'est le même appel, sur le même stock, que celui
qu'`autoprune_step` fait réellement en premier. Sans aucun pruner actif, un
serveur conserve donc une moitié de stock déjà morte, occupant de la mémoire et
de la bande passante de distribution pour rien.

#### Option `--pruner-dfs-mrv` : l'A/B du moteur de la preuve (§4.10)

La preuve DFS bornée peut employer le moteur à ordre DYNAMIQUE au lieu de l'ordre fixe
(`pruner_dfs_mrv`, `ETII_PRUNER_DFS_MRV=1` en production). `--pruner-dfs-mrv` bascule ce
même drapeau dans le banc : même stock, même échantillon, même budget, seul le moteur de la
preuve change — la comparaison appariée exigée par le protocole §7 du document de
conception.

```sh
make bench-refutation BENCH_REFUT_ARGS="--from-back temp.back --pruner-profile 500 --budget 10000"
make bench-refutation BENCH_REFUT_ARGS="--from-back temp.back --pruner-profile 500 --budget 10000 --pruner-dfs-mrv"
```

Mesuré sur un stock de PRODUCTION de 126 287 possibilités (produites par de vrais clients,
pas par `expand_datas_to_level`), échantillon de 2 000 pris 1 sur 63 — contrôle superficiel
identique aux six lignes : 22,1 % de mortes.

| Budget DFS | Moteur | Fermées par la preuve | Total éliminé | Nœuds | Temps |
|---|---|---|---|---|---|
| 1 000 | ordre fixe | 8,3 % | 30,4 % | 1 404 859 | 0,129 s |
| 1 000 | MRV | 34,8 % | 56,8 % | 888 745 | 1,081 s |
| 10 000 | ordre fixe | 10,0 % | 32,0 % | 13 746 435 | 1,177 s |
| 10 000 | MRV | 35,6 % | 57,7 % | 8 540 962 | 8,998 s |
| 100 000 | ordre fixe | 11,7 % | 33,8 % | 133 804 294 | 11,521 s |
| 100 000 | MRV | 36,0 % | 58,1 % | 84 108 383 | 85,575 s |

Trois enseignements pour l'exploitation : ×3 à ×4 de fermetures à budget égal ; **les deux**
moteurs plafonnent au-delà de 1 000 nœuds (l'ordre fixe ne gagne que 3,4 points en
multipliant le budget par 100), donc `prunerDfsBudget 1000` est le bon point de
fonctionnement ; et `MRV@1000` domine strictement `fixe@100000` (56,8 % contre 33,8 %
éliminés, pour 10,7× moins de CPU). Attention en revanche à ne pas lire le débit de
fermetures isolément : à budget égal MRV coûte ~2× plus par fermeture sur ce stock, parce
qu'il ferme aussi les sous-arbres que l'ordre fixe ne ferme jamais. Voir §4.10 du document
de conception pour l'analyse complète et une mesure secondaire sur un stock d'expansion.

L'en-tête du rapport rappelle le moteur employé, pour qu'une sortie collée hors contexte
reste interprétable.
#### Option `--w2x2` : compte les fenêtres 2×2 vides sans remplissage possible

**Comptage seul.** Ce mode n'élague rien, ne modifie aucun autre résultat du
banc et n'ajoute rien au chemin de production. Il répond à une seule question,
posée avant d'écrire le moindre mécanisme : *un test JOINT sur les 4 cases d'une
fenêtre 2×2 vide fermerait-il des possibilités que le pipeline actuel laisse
passer ?*

Le motif est un angle mort de **forme** du contrôle superficiel : dans une
fenêtre 2×2 entièrement vide, aucune case n'a jamais plus de 2 faces connues —
les 2 autres regardent les cases vides de la fenêtre. `possibility_all_has_a_next_counted`
juge chaque case isolément et ne peut donc voir que 2 contraintes ; un test joint
en voit jusqu'à 8. Le point fixe de §4.6a n'y change rien tant qu'aucune case de
la fenêtre n'est forcée.

```sh
make bench-refutation BENCH_REFUT_ARGS="--from-back eternityII.back --pruner-profile 20000 --w2x2 --budget 0"
```

Le mode balaye les 169 fenêtres 2×2 **intérieures** (`x`, `y` dans
`1..ETERN_SIZE-3`, pour que les 8 voisines existent), et pour chaque fenêtre
entièrement vide tente un remplissage exhaustif des 4 cases avec les primitives
du moteur (`what_search_in_grid_to_key` / `get_parts_bigarray_with_key`) — pas
une table de blocs précalculée : la mesure doit porter sur le POUVOIR de
réfutation, pas sur une implémentation, et aucune divergence de convention de
faces n'est alors possible.

**Trois garde-fous intégrés**, parce qu'un test d'élimination bogué produit
exactement le symptôme qu'on espère de lui :

1. **Auto-test de plomberie** au démarrage : sur un plateau vide, les 169
   fenêtres doivent toutes être remplissables. Échec ⇒ sortie en erreur.
2. **Oracle indépendant** : chaque réfutation est repassée par
   `w2_fillable_bruteforce`, qui balaye toutes les rotations de toutes les pièces
   en comparant les faces à la main, **sans la map ni `what_search_in_grid_to_key`**.
   Deux chemins de code sans primitive commune doivent toujours concorder.
3. **Cohérence des paquets** : une réfutation par disponibilité n'a de sens que
   si `b_faceused` est cohérent avec la grille. Les paquets recalés par
   `check_possibility` sont écartés du comptage et signalés.

#### Mesure sur stock réel : le pouvoir de réfutation croît avec la profondeur

Deux stocks réels, de profondeurs très différentes, mesurés avec le même mode.

**Stock peu profond** (2 416 950 possibilités, 10 à 31 pièces posées, échantillon
de 20 000) : le pipeline actuel n'élimine **rien** (0 % au contrôle superficiel comme
à la preuve DFS), le balayage 2×2 ferme **155 possibilités (0,78 %)**, pour
14,8 µs/possibilité contre 1,9 µs pour le contrôle superficiel. Origine des
réfutations : 107 par les couleurs, 48 par la disponibilité.

**Stock profond** (2 511 possibilités, 9 à 112 pièces posées, moyenne 23,
échantillon complet) — c'est la mesure qui décide :

| Budget DFS | Fermé par le pipeline | Coût DFS | Marginal 2×2 | Coût 2×2 |
|---|---|---|---|---|
| 0 | 0 (0 %) | — | **37 (1,47 %)** | 0,125 s |
| 10 000 | 114 (4,5 %) | 24,0 M nœuds, 2,281 s | **24 (0,96 %)** | 0,144 s |
| 100 000 | 121 (4,8 %) | 239,4 M nœuds, 21,848 s | **23 (0,92 %)** | 0,153 s |
| 1 000 000 | 137 (5,5 %) | 2 382,8 M nœuds, 201,295 s | **17 (0,68 %)** | 0,136 s |

**Le test 2×2 reste complémentaire de la preuve DFS à tous les budgets.** Même à
1 000 000 de nœuds et 201 s de DFS, 17 des 37 réfutations restent hors de portée du
DFS ; le recouvrement plafonne (13, puis 14, puis 20 sur 37). Les deux mécanismes ne
trouvent pas les mêmes impasses. Rapport de coût : passer le DFS de 10 k à 1 M coûte
**+199 s pour +23 fermetures** ; le balayage 2×2 en apporte **24 de plus pour 0,14 s**.

Ventilation par profondeur (budget indifférent, le balayage ne dépend pas du DFS) :

| Pièces posées | Possibilités | Réfutées | Taux |
|---|---|---|---|
| 0-15 | 1358 | 1 | 0,07 % |
| 16-31 | 531 | 1 | 0,19 % |
| 32-47 | 322 | 1 | 0,31 % |
| 48-63 | 150 | 1 | 0,67 % |
| 64-79 | 63 | 5 | **7,94 %** |
| 80-95 | 60 | 13 | **21,67 %** |
| 96-111 | 26 | 15 | **57,69 %** |

Croissance monotone jusqu'à 57,7 %. Le modèle de branchement (§4.9 du document de
conception) situe à ~186 pièces posées le point où le nombre de candidats attendu par
case croise 1 : ce tableau n'a donc pas atteint son plateau.

**Trois enseignements pour qui voudrait implémenter le mécanisme :**

- **Il n'y a pas de table de blocs 2×2 à construire.** Sur les deux stocks, **aucune**
  fenêtre vide n'a 3 ou 4 côtés connus (0 sur 372 520 dans le stock profond, 0 sur
  2 975 025 dans le peu profond). Une table indexée par 3 ou 4 côtés ne se
  déclencherait jamais. Ce qui tire, c'est le test joint à 1 ou 2 côtés — la
  contrainte vient de l'interaction des 4 cases entre elles et de la disponibilité
  des pièces, pas d'une bordure dense.
- **La source des réfutations s'inverse avec la profondeur** : 107 couleurs / 48
  disponibilité sur le stock peu profond, **3 couleurs / 35 disponibilité** sur le
  stock profond. En profondeur, c'est l'épuisement des pièces qui tue — exactement ce
  que le contrôle superficiel ne peut pas voir CONJOINTEMENT sur les 4 cases.
- **Mesurer sur stock profond, toujours.** Le même mode donne 0,78 % de fermetures
  peu discriminantes sur stock peu profond et une courbe montant à 57,7 % sur stock
  profond. C'est l'erreur de méthode qui a faussé §4.4 et §4.6b, reproduite ici à
  l'identique — et corrigée seulement parce qu'un stock plus profond a été produit
  exprès.

Garde-fous à chaque exécution ci-dessus : 0 faux positif sur solution, 0 désaccord de
l'oracle indépendant, 0 paquet écarté pour incohérence.

### Mode `--pruner-profile --gpu` : rejoue le VRAI pipeline GPU

Variante de `--pruner-profile` (ci-dessus) qui rejoue, au lieu du pipeline CPU
superficiel+DFS, le pipeline **GPU** réel : `gpu_pruner_check_batch`
(`src/app/gpu_pruner.cu`), soumis PAR LOTS (`--gpu-batch`, défaut 100 — proche
de `PRUNER_BATCH_SIZE`, comme `autoprune_gpu` en production), jamais
possibilité par possibilité. N'a pas d'équivalent DFS : le GPU ne fait, en
production, que le contrôle superficiel (§4.6b explique pourquoi un DFS
divergent par thread convient mal au modèle SIMT). Build dédié, indépendant du
flag `CUDA=1` du binaire de production :

```sh
make bench-refutation-gpu BENCH_REFUT_ARGS="--from-back temp.back --pruner-profile 8438 --gpu --gpu-batch 100"
```

`checked` est forcé à 0 sur l'état de départ soumis aux deux côtés (GPU et
contrôle CPU de comparaison) : le contrôle CPU
(`possibility_all_has_a_next_counted`) recalcule toujours indépendamment de ce
champ, alors que le kernel GPU court-circuite dessus en production
(`p->checked == 1` → vivant sans recalcul) — sans ce forçage la mesure
confondrait la divergence « une passe vs point fixe » (celle qu'on veut
chiffrer) avec celle, hors sujet ici, du court-circuit `checked`.

Sur un stock réel de 8438 possibilités (8 à 73 pièces posées, généré comme au
§ ci-dessus — client à ordre fixe contre un serveur `--expand-level 3`,
mesuré sur Jetson Orin Nano) :

| | Mortes au contrôle superficiel | Cases examinées/possibilité | Débit (possibilités/s) |
|---|---|---|---|
| CPU (point fixe, `--pruner-profile --budget 0`) | 32,2 % | 289,29 | 216 669 |
| GPU (une passe, `--pruner-profile --gpu`) | 21,4 % | 194,33 | ~69 000–74 000 (invariant, lot 100 à 8438) |

Divergence vs le CPU point fixe, sur le MÊME état de départ (8438
possibilités) : **910 (10,8 %)** que le GPU juge vivantes et le CPU mortes (la
cascade en fin de balayage que le point fixe rattrape et l'unique passe GPU
non — §4.6a) ; **0** que le GPU juge mortes et le CPU vivantes (aucun faux
mort — le contrôle GPU reste une condition nécessaire, jamais une
heuristique, cf. §5 de [elagage_recherche.md](conception/elagage_recherche.md)).
Le mode échoue explicitement (`exit(EXIT_FAILURE)`) si un seul faux mort est
observé, plutôt que de se contenter de le documenter.

**Le CPU séquentiel va plus vite que le GPU par lots, à cette échelle.** Le
débit GPU ne bouge pas avec la taille de lot (100, 500, 1000, 4219, 8438
testés : 68 000–74 000 possibilités/s dans tous les cas) — le goulot n'est
donc pas le nombre de lancements kernel, mais le lancement + la synchronisation
(`cudaDeviceSynchronize`) eux-mêmes, mal amortis par un travail par
possibilité aussi bon marché (194 cases examinées en moyenne). Même
enseignement que l'étude GPU sur le lookahead de recherche (rejetée, cf.
mémoire de session) : l'avantage GPU suppose un travail par élément assez
coûteux pour dominer le coût fixe du lancement, ce qui n'est pas le cas de ce
contrôle à cette échelle de lot. Voir [pruner_gpu_cuda.md](pruner_gpu_cuda.md)
pour la discussion complète (à ne pas vendre sur le débit brut).

## Voir aussi

- [tests/README.md](../tests/README.md) — organisation des suites, conventions, ajout d'un test.
- [Compilation](compilation.md) — options de build et drapeaux de configuration.
- `tests/bench/bench_search.sh` — banc de mesure du débit de recherche (voir ci-dessus).

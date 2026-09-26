# Tests, couverture et intégration continue

Ce document décrit les cibles de test du Makefile, les scénarios d'intégration
bout-en-bout, le rejeu de la CI en local via Docker et ce que fait la CI GitHub
Actions. Les conventions d'écriture des tests unitaires (organisation des suites,
fixtures, ajout d'un test) sont dans [tests/README.md](../tests/README.md).

## Cibles

```sh
make test             # compile tests/ + lance la suite unitaire (code de sortie non nul si échec)
make check-build-lists # vérifie que CMakeLists.txt décrit les mêmes sources que le makefile
make test-integration # scénarios bout-en-bout 16 pièces : solution client/serveur + canal de contrôle
make test-docker      # rejoue les jobs de test CI dans 3 conteneurs Linux en parallèle (nécessite Docker)
make test-docker-arm  # vérifie la compilation croisée ARM 64-bit (Raspberry Pi) dans le même conteneur (nécessite Docker)
make coverage         # les deux passes (256 + 16) + résumé texte gcovr fusionné (nécessite gcovr)
make coverage-256     # passe 256 pièces seule ; résumé gcov par module
make coverage-report  # rapports gcovr : Cobertura XML + HTML + résumé Markdown
make gen-root         # outil : convertit un plateau externe en racine de stock .back
make check-doc-links  # outil : vérifie les renvois Markdown internes (fichier + ancre)
make bench-solve      # banc « côté trouver » : coût de l'ATTEINTE d'une solution (clones)
make bench-ram-tier   # banc de l'étage RAM compressé : octets résidents par possibilité (Linux)
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

### Le runner travaille dans un bac à sable, jamais dans le dépôt

`tests/test_main.c` appelle `test_sandbox_enter()` (`tests/sandbox.{h,c}`) **avant
toute suite** : le runner crée un répertoire temporaire (`mkdtemp`) et s'y place.
Tous les artefacts écrits sous chemin relatif par le code de production —
`./eternityII.back`, `./eternityII-in_analyse.back`, `./eternityII-best_board.back`,
`events.log`, `solution_server_<pid>_<seq>`, `*.spillcount`, les sockets Unix
`etii_main.<pid>` / `etii_fork.<pid>` — y atterrissent, et le répertoire est effacé à
la sortie du process.

**Pourquoi** : ces chemins relatifs sont *corrects* en production (un serveur
sauvegarde dans son propre répertoire de travail) mais destructeurs sous `make test`,
dont le répertoire courant est la racine du dépôt. Un stock de production de 1,96 Go
posé à la racine y a été **écrasé par un `make test`** et réduit à 76 octets (en-tête
`ETERN_SIZE=4` : le fichier venait du build 4×4 de la suite). Le déclencheur était un
test exerçant la branche « solution trouvée » de `remove_possibilities_with_no_next`,
qui appelle `consistent_backup("./eternityII.back", …)`. Rien ne le signalait : les
motifs `*.back`, `*.spillcount`, `solution_*` et `etii_*` sont ignorés par le
versionnage, donc l'état du dépôt restait « propre » — 76 fichiers `.spillcount`
s'étaient accumulés à la racine sans que personne ne le voie.

**Correction centrale, pas par site** : une seule protection couvre les tests
existants *et à venir*. La variante « chaque test qui écrit se protège lui-même » a
été écartée — c'est justement par un test nouvellement ajouté que l'accident est
arrivé. Les scripts d'intégration appliquaient déjà ce schéma (`mktemp -d` + `cd`,
chemins résolus en absolu avant), le runner unitaire ne fait que le rejoindre.

**Contrepartie** : un test qui *lit* une donnée du dépôt par chemin relatif casserait
après le `chdir`. Inventaire fait : les seuls chemins concernés sont les globales de
production `parts_files` et `indices_file` (`core/core_static_variables.c`), rendues
**absolues avant** le `chdir`. Les chemins d'*écriture* restent relatifs à dessein.
Aucune modification de `src/` : le comportement de production est inchangé.

**Garde-fou (la partie qui mord)** : `sandbox_suite` (`tests/test_sandbox.c`), jouée
**en dernier**, compare le répertoire de lancement à la photographie prise au
démarrage et échoue en nommant l'entrée apparue. Elle attrape les deux fuites que le
bac à sable ne peut pas couvrir : une écriture par chemin **absolu**, et un `chdir`
vers le dépôt sans retour (second test, `runner_stays_in_its_temporary_sandbox`).
Vérifié par sabotage dans les deux sens : un test écrivant `<racine>/eternityII.back`
fait tomber le garde-fou (`+ eternityII.back` nommé dans le message d'échec) ; le
même test écrivant `./eternityII.back` **ne pollue plus rien** — le fichier atterrit
dans le bac à sable. Sur échec, le bac à sable est conservé (son chemin est affiché)
pour l'analyse post-mortem.

La comparaison porte sur les entrées **directes** du répertoire de lancement, pas sur
une descente récursive : la pollution y est toujours (les défauts de production sont
des `"./…"`), et une récursion signalerait à tort les `.gcda` que `make coverage`
sème dans ses sous-dossiers.

## Second système de build : CMake, et son garde-fou (`make check-build-lists`)

Le dépôt décrit ses binaires **deux fois** : dans le `makefile` — la référence,
seule jouée par la CI — et dans `CMakeLists.txt`, pour le confort des IDE
(`cmake -S . -B build && cmake --build build --target run_tests_16`, puis
`ctest`). CMake reproduit les mêmes listes de sources : `PROD_SRCS` (pendant
d'`OBJS`), `TEST_RUNNER`, `TEST_SUITES_COMMON`, `TEST_SOLUTION16`,
`TEST_MODULES`.

Deux listes recopiées dérivent. C'est arrivé : plusieurs suites et modules
ajoutés au makefile (`test_best_board`, `test_stock_spill`, `test_packet_codec`,
`test_root_from_board`, `test_bench_solve_stats`, `test_cross_mask`, et les
modules correspondants) n'avaient jamais été reportés dans CMake, et
`run_tests_16` ne se liait plus — une dizaine de symboles manquants
(`best_board_suite`, `cross_mask_suite`, `build_search_parts`…). Rien ne pouvait
le signaler : `make test` passait, et la CI n'appelle jamais CMake.

`tests/tools/check_build_lists.py` compare désormais les listes homonymes des
deux fichiers, **ensemble par ensemble** (l'ordre des sources n'a aucune
incidence sur le link) et échoue en nommant chaque écart, dans les deux sens. Il
est branché aux deux endroits :

- cible `make check-build-lists`, **dépendance de `make test`** — donc jouée par
  la CI à chaque exécution ;
- test CTest `build-lists-sync`, joué par un simple `ctest`.

Deux écarts sont voulus et inscrits dans le script : `src/app/main.c` (que CMake
passe directement à `add_executable`, `PROD_SRCS` existant justement pour être
réutilisé sans lui) et le module GPU conditionnel. Les variables dépendant d'une
option (`$(LOGGER_OBJ)` / `${LOGGER_SRC}`, `$(CUDA_OBJ)`) sont substituées sur
leur valeur par défaut.

Le binaire d'intégration `eternityII16` **dérive** de `PROD_SRCS` (logger ANSI
substitué) au lieu d'en recopier la liste : il lui manquait `packet_codec.c` pour
la même raison.

Une source ajoutée au makefile se reporte donc dans `CMakeLists.txt` dans la
même passe — le garde-fou le rappellera sinon.

## Tests d'intégration (`make test-integration`)

Compile un binaire dédié (`ETERN_PARTS=16`, plateau 4×4) et enchaîne deux scripts
(`tests/integration/`), qui doivent tous deux passer :

- **`run_solution_16.sh`** — exercice réel du protocole de travail : serveur + client
  lancés avec `--stop-on-solution`, vérifie que les **deux côtés** voient la solution
  (logs, fichiers `solution_*`, backups `.back`, arrêt propre du serveur).
- **`run_control_channel.sh`** — exercice du
  [canal de contrôle](echanges_client_serveur.md#canal-de-contrôle-v9-étendu-en-v10-et-v12) : serveur +
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
conteneur : les artefacts Linux ne se mélangent jamais à ceux du poste hôte.

Par défaut, les 3 jobs (build+unitaire, ASan, intégration) tournent dans **3
conteneurs indépendants en parallèle**, comme les 3 runners GitHub séparés de la
CI (`test`, `test-asan`, `integration-test`), plutôt qu'un seul conteneur
séquentiel qui n'utilise jamais plus d'un cœur. Chaque conteneur reçoit sa
propre copie `/work` (montage `/src` en lecture seule commun, `make clean`
individuel) : aucun conflit entre jobs. Les logs de chaque job sont capturés
puis affichés une fois le job terminé (pas d'entrelacement), et la cible échoue
si **au moins un** des 3 jobs échoue.

Fournir `DOCKER_TEST_CMD` explicitement bascule sur l'ancien mode — un seul
conteneur qui rejoue exactement cette commande — pratique pour cibler un job
précis sans attendre les deux autres :

```sh
make test-docker DOCKER_TEST_CMD="make test ASAN=1"
```

### La copie `/src` → `/work` ignore les fichiers spéciaux

Le repo hôte est monté en lecture seule sur `/src` puis recopié dans `/work`.
Cette copie n'est **pas** un `cp -R` : `cp` ne sait pas copier un fichier
spécial et s'arrête net dessus —

```
cp: cannot stat '/src/./etii_main.76645': Operation not supported
```

— une seule socket résiduelle suffisait donc à bloquer *tout* le rejeu de la CI.
Or le répertoire de travail en contient légitimement : les sockets Unix
`etii_main.<pid>` / `etii_fork.<pid>` de l'IPC parent↔fork y sont créées par
`bind()` (cf. [Architecture](architecture.md#cycle-de-vie-des-fichiers-socket)).
Elles sont désormais supprimées à la terminaison du process qui les a créées,
mais un process **encore vivant** — ou tué par `SIGKILL` — en laisse forcément
une derrière lui, et elles sont invisibles de `git status` (git ne suit pas les
fichiers spéciaux) : le diagnostic est donc particulièrement déroutant.

La copie passe par `find ! -type s | tar`, qui les ignore explicitement plutôt
que de dépendre d'un répertoire propre — une socket n'a de toute façon aucun sens
à être recopiée dans le conteneur. Même mécanisme pour `make test-docker-arm`
(variable `DOCKER_COPY_SRC` du Makefile, partagée par les deux cibles).

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
  puis `make CUDA=1 VERIFY=1`), la variante zstd (`make ZSTD=1`, suivie de
  `make test ZSTD=1` : l'étage RAM compressé et ses tests propres), un build activant **tous** les flags `DEBUG_*` de
  [src/core/core_static_variables.h](../src/core/core_static_variables.h) à la fois, la
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

## Outil `gen_root` (`make gen-root`)

Convertit un plateau externe — plateau publié, sauvegarde d'un autre solveur,
réparation locale d'un plateau connu dont on retire quelques cases pour laisser
le moteur rejouer la région — en **racine de stock** au format `.back`. Le
fichier produit est un dump brut de `struct possibility_packet` (576 octets),
chargeable par la console `restore` ou `import`.

```sh
make gen-root
tests/tools/gen_root data/pieces.csv plateau.txt racine.back
```

Entrée : `ETERN_SIZE²` lignes « id top right bottom left » en ordre
ligne-majeur, `0 0 0 0 0` pour une case vide. `CPPFLAGS="-DETERN_PARTS=16"` est
propagé par la cible pour produire une racine du puzzle 16.

Ce n'est pas une suite de tests : la cible n'est pas rattachée à `make test`,
au même titre que `make bench-refutation`. Son **cœur pur**
(`tests/tools/root_from_board.c`), lui, est compilé avec les autres modules et
couvert par `tests/tools/test_root_from_board.c` — c'est là que vivent les deux
conventions qu'un outil externe inverse sans que rien ne proteste
(`grid[colonne][ligne]`, et l'indice de rotation retrouvé par correspondance
des faces plutôt que calculé). Détail d'usage et les deux pièges rencontrés
(`restore` et non `import` ; indices officiels obligatoires) :
[tests/README.md](../tests/README.md#outils-teststools).

## Garde-fou des renvois de documentation (`make check-doc-links`)

```sh
make check-doc-links              # tout le dépôt
python3 tools/check_doc_links.py docs/console.md   # un fichier en particulier
```

Vérifie **tous les liens Markdown internes** du dépôt : la cible existe-t-elle,
et si le lien porte une ancre (`fichier.md#ancre`), cette ancre correspond-elle
encore à un titre du fichier visé ?

Le problème que ça résout est silencieux par construction. Un titre est étendu
au fil du temps — « `## Canal de contrôle (v9)` » devient « `## Canal de
contrôle (v9, étendu en v10 et v12)` » — et son ancre change avec lui, mais les
renvois, eux, ne bougent pas. GitHub **n'affiche aucune erreur** sur une ancre
inconnue : il ouvre simplement le haut du fichier. Le lien reste cliquable, il
atterrit juste à côté, et rien dans une relecture ne le distingue d'un lien
juste. La dérive s'était accumulée à **34 renvois morts** avant que ce script
n'existe, dont 23 sur le seul titre du canal de contrôle.

Le script rejoue l'**algorithme d'ancre de GitHub** (github-slugger) : passage
en minuscules, suppression de la ponctuation *sauf* le tiret et le souligné,
espaces convertis en tirets, suffixe `-1`/`-2`… pour les titres homonymes d'un
même fichier. Les caractères accentués sont conservés (`canal-de-contrôle-…`,
pas `canal-de-controle-…`) — c'est bien ce que fait GitHub, et s'en écarter
produirait de faux positifs sur la moitié de la documentation.

Il sépare **deux catégories**, parce qu'elles ne se réparent pas de la même
façon :

| Catégorie | Cause | Réparation |
|---|---|---|
| **ancre absente** | le fichier est là, le titre a bougé | repointer sur l'ancre réelle — **jamais** renommer le titre, il est cité ailleurs |
| **fichier absent** | la cible a été supprimée ou renommée | `git log --diff-filter=D -- <chemin>` pour retrouver le remplaçant, ou retirer le lien en gardant le texte si la cible a été absorbée ailleurs |

Pour chaque ancre absente, les trois ancres existantes les plus proches sont
proposées (`difflib`), ce qui suffit presque toujours à trancher.

Deux pièges d'analyse que le script prend en charge, et qu'un `grep` ne prend
pas :

- **les blocs de code et le code en ligne sont neutralisés** avant l'extraction
  — un `tab[i](j)` dans un exemple C n'est pas un lien, et un `## titre` dans un
  bloc `sh` n'est pas un titre ;
- **un libellé de lien peut enjamber une fin de ligne.** Le rehabillage des
  paragraphes coupe plusieurs renvois du dépôt en deux, et une première version
  ligne à ligne en ratait deux en silence. Le compte des cibles extraites a été
  croisé avec un `grep` brut jusqu'à concordance exacte (197) avant de faire
  confiance au parseur — un vérificateur qui rate des liens est pire qu'absent,
  il rassure à tort.

**Volontairement hors de `make test`**, même convention que `make gen-root` :
c'est un outil de documentation, pas une suite, et un renvoi caduc ne casse
aucun binaire. Le code de sortie est non nul en cas de lien cassé, donc la cible
reste utilisable telle quelle depuis un *hook* ou une CI documentaire.

**Vérifié par sabotage**, comme le reste du dépôt : casser une ancre dans un
lien le fait sortir en 1 en nommant le fichier et la ligne ; renommer le titre
`## Canal de contrôle (v9, étendu en v10 et v12)` fait remonter d'un coup ses
**23** référents ; remplacer une cible par un fichier inexistant le classe en
« fichier absent » et non en « ancre absente ».

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

**`ETII_BENCH_NODES` ne vaut que pour le mode `test`, et deux pièges vont avec.**
Le premier est un simple fait d'architecture : le thread de statistiques sonde
`counters[]` du **processus courant**, or en mode `client` la recherche tourne
dans des **forks**, dont les compteurs ne remontent au parent que par IPC
(`fork_statistics`). Le seuil n'est donc jamais atteint côté parent et la
recherche ne s'arrête pas d'elle-même : pour instrumenter une mesure sur stock
réel en mode `client`, on laisse tourner un temps choisi puis on arrête par
`SIGINT`, et chaque fork émet son rapport en sortant. Le second était un bug, corrigé : la boucle de sondage à 1 ms
n'appelait pas `fork_gate_checkpoint`, ce qui rendait le thread injoignable
pendant sa tranche de 10 s, très au-delà du budget de 2 s de
`fork_gate_request_quiesce` — **tout fork décidé par l'orchestrateur était
refusé** (« quiescence non atteinte — fork refusé ») et un client lancé avec
`ETII_BENCH_NODES` ne démarrait jamais ses process de recherche. Le découpage en
tranches avait été appliqué à la branche sans banc uniquement. Verrouillé par
`checker_thread_parks_at_the_fork_gate_in_bench_mode`
(`tests/app/test_etii_client.c`), qui échoue si le point de rendez-vous
disparaît.

La décision d'arrêt (`bench_should_stop`) et le parsing de la variable
d'environnement (`bench_parse_nodes_env`) sont des fonctions pures
(`src/app/app_static_variables.h`/`.c`), testées unitairement dans
`tests/app/test_app_static_variables.c`.

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

> Mesure antérieure à la bascule MRV
> ([docs/conception/mrv_moteur_unique.md](conception/mrv_moteur_unique.md)) : le
> « client à ordre fixe » et `normalize_possibility_packet` ont depuis disparu
> (PR1/PR3, `alloc` est désormais directement le nombre de pièces posées, plus
> un curseur à re-canoniser). Chiffres conservés tels quels comme repère
> historique de volumétrie ; ne pas s'attendre à retrouver ces deux éléments
> dans le code actuel.

À défaut de backup, le banc fabrique des racines : une descente MRV produit un
plateau profond, dont on extrait des préfixes (`--depths`) — utile pour
construire des cas **durs** (sous-arbre réellement vivant), que le stock réel
fournit rarement.

### Trois moteurs, pas deux : l'ablation qui sépare les deux axes

> **Note (PR3, [docs/conception/mrv_moteur_unique.md](conception/mrv_moteur_unique.md)) :**
> cette section décrit la mesure qui a motivé la bascule vers MRV comme moteur
> UNIQUE. Les moteurs `fixe`, `fixe+global` et `fixe+singleton` cités ci-dessous
> (et le drapeau `global_dead_check` qui les distinguait) ont depuis été
> **supprimés** avec l'ancien moteur à ordre fixe — le banc n'accepte plus que
> `mrv`/`mrv+singleton` en argument de `--engines`. Le récit de la mesure reste
> ci-dessous tel quel : c'est un post-mortem de décision valide, pas l'état
> actuel du code.

Les deux moteurs de production **confondaient deux axes indépendants** :

| | détection de case morte **locale** (4 voisines) | détection **globale** (tout le plateau) |
|---|---|---|
| **ordre fixe** | moteur historique (`fixe`) | `fixe+global` — l'ablation |
| **ordre dynamique** | (dégénéré : le balayage la donne gratuitement) | moteur MRV (`MRV`) |

Ne comparer que les deux coins opposés ne dit pas lequel des deux axes produit
l'effet mesuré. D'où le drapeau `global_dead_check` (`src/core/core_static_variables.h`,
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

Réimplémenté derrière `singleton_conflict_check` (`src/core/core_static_variables.h`,
défaut 0, vit dans `bt_forward_check` — actif pour le seul moteur MRV depuis
PR3, autrefois partagé avec l'ordre fixe) et ajouté au banc comme quatrième
variante (`--engines fixe,fixe+singleton` à l'époque de cette mesure ;
`--engines mrv,mrv+singleton` aujourd'hui). Deux mesures distinctes, parce que
ce sont deux questions différentes :

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

### Bras d'ordre des CASES : la croix séparatrice

Les moteurs ci-dessus ne varient que le contenu du forward-check. `--engines`
déclare en plus **six bras d'ordre des variables** — quelle case le balayage MRV
ouvre en premier — l'axe que le §7.7 de
[banc_resolution_clones.md](conception/banc_resolution_clones.md) avait laissé
ouvert faute d'un second point d'entrée dans le moteur. Conception complète :
[croix_separatrice_ordre_variables.md](conception/croix_separatrice_ordre_variables.md).

La croix est définie en compréhension (`tests/bench/cross_mask.c`,
`abs(x−y) ≤ 1 ou abs(x+y−(n−1)) ≤ 1`) : 88 cases au 16×16, découpant le reste du
plateau en quatre régions de 42 sans aucun passage orthogonal, et portant les
cinq indices officiels. Son cœur est pur et rattaché à `make test`
(`tests/bench/test_cross_mask.c`), comme `bench_solve_stats.c`.

| Bras | Hauteur du bit dans la clé MRV | Ce qu'il fait |
|---|---|---|
| `cross-key` | sous `count` | à score MRV ÉGAL, une case de croix d'abord |
| `cross-mrv` | au-dessus de `count` | une case de croix avant TOUTES les autres |
| `rand-key` / `rand-mrv` | idem | **contrôle** : un tirage à la densité de la CROIX (88/256) |
| `anti-key` / `anti-mrv` | idem | **contrôle** : le complément de la croix (168/256) |
| `randc-key` / `randc-mrv` | idem | **contrôle** : un tirage à la densité du COMPLÉMENT |

**Les deux contrôles ne sont pas optionnels.** Le §4.14 de
[elagage_recherche.md](conception/elagage_recherche.md) a mesuré qu'un départage
**aléatoire** bat déjà l'ordre positionnel de 37 à 56 %. Un bit inséré au-dessus
du champ de position perturbe cet ordre par construction : sans `rand-*` à même
densité, un gain ne distingue pas « la croix est un bon a priori » de
« n'importe quoi vaut mieux que l'ordre des bits ». `--cross-seed <n>` ensemence
le tirage (défaut 1).

**Et un contrôle ne vaut qu'à la densité du bras qu'il contrôle** — c'est la
raison d'être de `randc-*`, ajouté en cours de campagne. La croix marque 88
cases sur 256, son complément 168 : les opposer au même tirage confond « la
géométrie compte » avec « la densité compte », et c'est exactement ce sur quoi
la campagne a buté (§7.4 du document de conception). Les deux tirages partent de
graines décalées, faute de quoi l'un serait un préfixe de l'autre — deux
contrôles corrélés ne font qu'un seul contrôle.

**Résultat de la campagne (2026-09-17) : négatif, aucun bras adopté.**
`cross-key` change réellement l'arbre sur 232 racines de production sur 499 et
n'y gagne rien (113/119, p = 0,74) ; `cross-mrv` ferme 94 racines sur 200 là où
la production en ferme 199. Les bras restent dans le banc — ils ne coûtent rien
tant qu'on ne les demande pas, et c'est ce qui permet de refaire la mesure
plutôt que de la croire sur parole.

```sh
make bench-refutation BENCH_REFUT_ARGS="--from-back temp.back --min-pieces 130 --max-roots 50 --engines mrv,cross-key,rand-key,anti-key --budget 5000000"
```

**Ces six bras ne sont jamais joués par défaut** (`--engines` vaut
`mrv,mrv+singleton` sans argument) : une invocation existante mesure exactement
ce qu'elle mesurait, et comparer huit moteurs sur les mêmes racines coûte quatre
fois plus cher.

**Auto-test, joué dès qu'un bras d'ordre des cases est demandé.** Le moteur a
deux balayages choisis une fois par recherche (`bt_masks_complete`) :
`mrv_choose_cell_fast` sur une map de production, `mrv_choose_cell` en repli.
Leur équivalence est verrouillée en production par
`mrv_choose_cell_fast_matches_generic_on_real_map` — mais ce test-là compile
**sans le hook** et ne dit donc rien des bras, alors que les deux chemins n'ont
pas la même forme (le générique sort dès `count == 0`, le rapide va au bout de
son balayage). L'auto-test compare verdict, case et score sur des plateaux de
profondeurs croissantes, pour chaque bras demandé, et **refuse de mesurer** en
cas de désaccord. Il ne contrôle PAS que les bras coûtent le même nombre de
nœuds — ce serait faux, et c'est tout l'objet de la mesure ; contrairement au
banc « côté trouver », dont la prémisse (un sous-arbre mort coûte pareil quel
que soit l'ordre des VALEURS) se retourne, elle, en test.

**Le banc refuse aussi de tourner en build 16** : en 4×4 la croix couvre le
plateau entier, tous les bras y sont des no-op, et une mesure y serait une
mesure de rien.

**Rien de tout cela n'entre en production.** Le hook vit sous
`ETII_BENCH_CELL_HOOKS`, défini par ce seul banc ; `build/core/etii_search.o`
est **octet pour octet identique** avec et sans lui, et c'est le contrôle à
refaire avant d'y toucher. Ce banc ne définit volontairement pas
`ETII_BENCH_HOOKS` (qui active en plus l'ordre des valeurs) : l'indirection de
l'ordre des valeurs coûterait un test de pointeur par candidat dans sa boucle
chaude et rendrait ses temps incomparables à ses campagnes précédentes — et
l'ordre des valeurs n'a de toute façon aucun effet sur un sous-arbre mort.

### Point de chute : la profondeur maximale atteinte

Chaque ligne par racine porte une quatrième colonne, et le bilan deux agrégats :
la **profondeur maximale atteinte** pendant la fermeture (`max_result`). C'est
la grandeur qu'un parc de clients observe (« le max revient vers 206 ») et dont
il n'a pas la moyenne.

**Lecture : à budget de nœuds égal, PLUS BAS est MEILLEUR** — la branche morte a
été abandonnée plus tôt, donc la contradiction a été vue plus tôt. C'est
l'inverse de la lecture de `bench_search.sh`, où `max_result` est un garde-fou
contre un moteur qui gagnerait en débit en cessant d'avancer.

Deux précautions, toutes deux payées par une mesure fausse avant d'être écrites
ici :

- **`max_result` reste à 0 si aucun placement n'aboutit.** Le moteur ne l'écrit
  qu'après un placement réussi, donc une racine réfutée d'emblée laisse le
  compteur à zéro — pris tel quel, cela donne une « chute moyenne » inférieure à
  la profondeur des racines, ce qui est impossible. Le banc rapporte donc
  `max(max_result, profondeur de la racine)`, et publie à part le nombre de
  racines **réfutées sans placer** : sur un stock de production, MRV en réfute
  9 % à ce régime.
- **Le maximum ne se compare pas entre deux échantillons de tailles
  différentes.** Un maximum croît avec le nombre d'essais : les 206-216 d'un
  parc de clients ne se comparent pas aux 200 d'un banc de 200 racines. Seul
  l'écart apparié entre moteurs, à budget égal, se lit.

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

#### A/B historique du moteur de la preuve (§4.10) — moteur unique depuis PR3

**Statut : mesure historique, mécanisme figé.** La preuve DFS bornée pouvait autrefois
employer le moteur à ordre DYNAMIQUE au lieu de l'ordre fixe (`pruner_dfs_mrv`,
`ETII_PRUNER_DFS_MRV=1` en production, `--pruner-dfs-mrv` dans ce banc) — le tableau
ci-dessous est le résultat de cette comparaison appariée, exigée par le protocole §7 du
document de conception. Depuis [docs/conception/mrv_moteur_unique.md](conception/mrv_moteur_unique.md)
(PR3), MRV est le SEUL moteur (recherche réelle et preuve bornée du pruner) : le drapeau
`pruner_dfs_mrv` et l'option `--pruner-dfs-mrv` de ce banc ont été supprimés, il n'y a plus
d'A/B à rejouer.

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

**Statut : mécanisme ÉVALUÉ et ÉCARTÉ** (mesure ci-dessous) — le mode de comptage
reste disponible pour le remesurer si la référence bouge.

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

L'A/B historique qui a tranché s'est fait **contre la preuve MRV** (§4.10), pas contre
l'ordre fixe — MRV est désormais le seul moteur (PR3), donc la seule commande utile
aujourd'hui est :

```sh
make bench-refutation BENCH_REFUT_ARGS="--from-back eternityII.back --pruner-profile 2000 --w2x2 --budget 1000"
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

#### Mesure : le test 2×2 est absorbé par la preuve MRV — ÉCARTÉ

**Verdict : ne pas implémenter le mécanisme.** Le test réfute réellement, et
beaucoup — mais presque exclusivement des possibilités que le pipeline ferme déjà,
dès lors que la preuve bornée emploie le moteur MRV (§4.10).

A/B apparié sur deux stocks de PRODUCTION (126 287 et 141 734 possibilités, 9 à 167
pièces posées, moyenne 46), échantillon de 2 000, budget DFS 1 000 — le point de
fonctionnement retenu par §4.10 :

| Stock | Moteur de la preuve | Éliminé par le pipeline | **Marginal 2×2** |
|---|---|---|---|
| 126 287 poss. | ordre fixe | 608 (30,4 %) | **71 (3,55 %)** |
| 126 287 poss. | **MRV** | 1 136 (56,8 %) | **3 (0,15 %)** |
| 141 734 poss. | ordre fixe | 583 (29,1 %) | **67 (3,35 %)** |
| 141 734 poss. | **MRV** | 1 100 (55,0 %) | **2 (0,10 %)** |

Reproduit à budget 10 000 sur le premier stock : 3,10 % marginal en ordre fixe,
**0,10 %** en MRV. Le moteur de la preuve, pas le budget, est ce qui décide.

**Le mécanisme se déclenche énormément** — 433 possibilités sur 2 000 (21,6 %) ont au
moins une fenêtre 2×2 sans remplissage possible. Ce n'est pas une absence de
déclenchement comme §4.2 ou §4.4. C'est un **recoupement structurel**, comme §4.3 et
§4.5 : sur ces 433, **339 sont déjà mortes au contrôle superficiel** (gratuit) et
**91 de plus sont fermées par la preuve MRV**. Il en reste 3.

**Ce qui rendait la piste crédible, et pourquoi ça ne suffit pas.** Sur un stock plus
ancien et moins profond (2 511 possibilités, ≤ 112 pièces), le taux de réfutation
croissait de 0,07 % (0-15 pièces posées) à **57,69 %** (96-111), et le test restait
complémentaire de la preuve en **ordre fixe** à tous les budgets — y compris 1 000 000
de nœuds et 201 s de DFS, où 17 réfutations sur 37 échappaient encore au DFS. Cette
conclusion était juste, et elle n'a pas survécu au changement de référence : §4.10 a
remplacé le moteur de la preuve, et le nouveau absorbe ce que l'ancien laissait passer.
C'est exactement la règle du protocole §7 du document de conception — *mesurer
par-dessus la PR précédente, jamais contre `master`* — vérifiée dans le sens
désagréable.

**Sur le coût, une précaution.** Le balayage coûte 12 à 19 ms par possibilité dans
cette instrumentation, contre 0,55 ms pour la preuve MRV entière à budget 1 000. Ce
chiffre ne doit PAS être lu comme le coût d'une implémentation : le mode réutilise
délibérément les primitives du moteur plutôt qu'une table de blocs 2×2 précalculée, et
une vraie implémentation serait de plusieurs ordres de grandeur plus rapide. **Le
verdict ne repose pas sur le coût mais sur le pouvoir marginal** (0,10 à 0,15 %), qui
est mesuré, lui, indépendamment de toute implémentation.

**Deux enseignements qui survivent au verdict :**

- **Il n'y aurait de toute façon aucune table de blocs 2×2 à construire.** Sur tous
  les stocks mesurés, **aucune** fenêtre vide n'a jamais 3 ou 4 côtés connus (0 sur
  372 520, 0 sur 2 975 025). Une table indexée par 3 ou 4 côtés — dont le pouvoir de
  rejet théorique est pourtant de 88,5 % et 99,94 % — ne se déclencherait jamais.
- **La source des réfutations s'inverse avec la profondeur** : 107 couleurs / 48
  disponibilité sur stock peu profond, 3 / 35 sur stock profond. En profondeur c'est
  l'épuisement des pièces qui tue, jamais les couleurs.

**Pourquoi le mode reste dans le banc** malgré le verdict : même raison que
`--engines mrv+singleton`, qui sert encore à remesurer §4.4 quand la référence bouge.
Le mode ne coûte rien tant que `--w2x2` n'est pas passé. (MRV étant désormais le seul
moteur — PR3 — l'hypothèse « si `pruner_dfs_mrv` s'avérait indéployable » ne se pose
plus : il n'y a plus d'ordre fixe vers lequel replier.)

Garde-fous à chaque exécution ci-dessus : 0 faux positif sur solution, 0 désaccord de
l'oracle indépendant (476 à 558 réfutations repassées par exécution), 0 paquet écarté
pour incohérence.


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

## Banc de résolution : clones à solution connue (`make bench-solve`)

`bench_refutation` mesure le coût de la **preuve qu'un sous-arbre est mort** ;
`bench_search.sh` mesure un **débit**. Ni l'un ni l'autre ne peut voir **l'ordre
des valeurs** — quelle pièce essayer d'abord sur une case — pour une raison de
fond : dans un sous-arbre mort, *tous* les candidats sont essayés quel que soit
leur ordre, donc le compte de nœuds d'une réfutation en est rigoureusement
indépendant. Or c'est l'ordre des valeurs, et le point de départ, qui décident
**à quel moment** la branche portant la solution est atteinte.

Le puzzle réel n'ayant jamais été résolu, ce côté-là n'est mesurable que sur des
instances **construites autour d'une solution plantée** : des *clones*.
Conception complète : [conception/banc_resolution_clones.md](conception/banc_resolution_clones.md).

### 1. Générer des clones — `tools/gen_clone.py`

```sh
python3 tools/gen_clone.py --size 10 --inner-colours 17 --frame-colours 5 \
        --seed 1 --hints 5 --out-dir data/clones
```

Produit `pieces_10_17_1.csv` (format de `data/pieces.csv`),
`indices_10_17_1.csv` (format de `data/indices.csv`) et `solution_10_17_1.txt`
(la grille plantée, pour contrôle). Le générateur :

- tire les couleurs des arêtes selon l'histogramme de `data/pieces.csv` mis à
  l'échelle, **pas un uniforme naïf** — sur le 16×16 la règle redonne exactement
  les comptes observés (12 couleurs à 25 arêtes, 5 à 24 ; cadre 12 chacune) ;
- **rejette et retire** toute instance à pièce dupliquée ou à symétrie de
  rotation — les deux propriétés ont été vérifiées sur `data/pieces.csv`
  (0 doublon, 0 pièce symétrique) avant d'être imposées ;
- ré-assemble la solution **depuis les fichiers produits** et vérifie chaque
  arête, puis passe le clone par `tools/validate_pieces.py`.

Deux familles à mesurer, jamais une seule (§3.2 du document de conception) :
`--inner-colours 17` garde les statistiques de compartiments du puzzle réel,
une valeur calibrée par taille garde sa **dureté** relative. Un classement de
politiques qui change entre les deux familles n'est pas un résultat.

### 2. Mesurer — `make bench-solve`

La taille du plateau est celle du binaire, donc `CPPFLAGS` :

```sh
make bench-solve CPPFLAGS=-DETERN_PARTS=100 \
     BENCH_SOLVE_ARGS="--instance-dir data/clones --budget 50000000 --seeds 3"
```

Une exécution = **un processus fils** : il pose la genèse, met
`stop_on_solution` à 1 et cherche ; une solution fait sortir `record_solution`
par `exit()`, et un `atexit` rapporte nœuds et temps sur un tube. Rien n'est
ajouté au moteur pour cela — le chemin `stop_on_solution` réel est exercé tel
quel, et la queue lourde de la distribution ne peut pas polluer les exécutions
suivantes.

Le banc imprime une ligne par exécution, puis, par politique : médiane et
**moyenne géométrique** des nœuds (la distribution est à queue lourde, la
moyenne arithmétique ne classe rien), la **courbe de survie** (part résolue sous
10⁴, 10⁵, … nœuds), la **comparaison appariée** contre la politique de référence,
et le coût attendu d'une stratégie de **redémarrage** à seuil — une lecture de la
distribution, aucun mécanisme n'est implémenté.

| Option | Rôle |
|---|---|
| `--pieces <f>` / `--indices <f>` | une instance et ses indices |
| `--instance-dir <d>` | toutes les `pieces_*.csv` d'un répertoire, appariées aux `indices_*.csv` |
| `--policies <liste>` | `natural` (production), `reverse`, `random`, `lcv`, `mcv` — la **première** sert de référence appariée |
| `--root <nom>` | `genesis` (production), `center`, `border` : sur quelle case la genèse est développée |
| `--seeds <n>` | graines des politiques aléatoires (les déterministes n'en consomment qu'une) |
| `--budget <n>` | plafond de nœuds par exécution |
| `--selftest-budget <n>` | plafond de l'auto-test de l'instrument (0 = désactivé) |

### 2 bis. Axe « ordre des CASES » (variables)

`--policies` ne sélectionne pas que des ordres de valeurs : huit bras d'ordre
des **cases** y sont déclarés, tous à l'ordre de valeurs de production pour ne
faire varier qu'un axe à la fois. Ils passent par le même hook
`ETII_BENCH_CELL_HOOKS` que `bench_refutation`, et répondent à une question que
le banc de réfutation ne peut pas poser : la recherche met plus de 100 pièces à
**rencontrer** les indices, bien qu'ils soient posés dès la genèse — les
rencontrer plus tôt fait-il TROUVER plus vite ?

| Bras | Masque | Hauteur |
|---|---|---|
| `cross-key` / `cross-mrv` | la croix séparatrice (88 cases en 16×16) | sous / au-dessus de `count` |
| `halo-key` / `halo-mrv` | le **halo des indices de l'instance** (20 cases) | idem |
| `randc-*` / `randh-*` | contrôles aléatoires aux densités de la croix / du halo | idem |

Le **halo** est dérivé du plateau de genèse — les cases vides voisines d'une
case posée, c'est-à-dire exactement celles sur lesquelles une contrainte
d'indice porte. Il est donc une propriété de l'INSTANCE, pas de la géométrie du
plateau : contrairement à la croix, il se transpose d'une taille de clone à
l'autre sans distorsion de densité.

```sh
make bench-solve CPPFLAGS=-DETERN_PARTS=100 \
     BENCH_SOLVE_ARGS="--instance-dir data/clones --policies natural,halo-key,halo-mrv,randh-key,randh-mrv --budget 20000000"
```

**Ces huit bras ne sont pas joués par défaut** (`--policies` vaut les cinq
ordres de valeurs), pour la même raison que dans `bench_refutation` : une
invocation existante doit continuer de mesurer ce qu'elle mesurait.
`--cross-seed <n>` ensemence les contrôles aléatoires.

**Résultat de la campagne (2026-09-18) : négatif, aucun bras adopté.** Sur deux
cellules × 30 instances, tous les bras perdent la comparaison appariée, et
`halo-mrv` — le bras le plus économique en apparence — ne résout **aucune** des
60 instances quand la production les résout toutes. Détail et mécanisme :
§7 bis de [croix_separatrice_ordre_variables.md](conception/croix_separatrice_ordre_variables.md).

### 3. L'auto-test de l'instrument, en deux contrôles

**(a) Les permutations en sont-elles ?** Pour chaque case vide de la racine, le
banc appelle le point d'entrée d'ordre des valeurs sur le vrai compartiment et
vérifie que les indices rendus forment une **bijection** de `[0, taille[` :
aucun candidat perdu, dupliqué, ni hors compartiment. Ce contrôle-ci ne suppose
rien du moteur — il ne regarde que le hook — et il est **fatal**. Il porte son
contre-contrôle : une permutation délibérément fausse doit le faire échouer,
sans quoi il ne prouverait rien.

**(b) L'ordre des valeurs influe-t-il sur un sous-arbre MORT ?** Le banc cherche
ensuite une racine que la politique de référence **ferme** dans
`--selftest-budget` nœuds, puis la rejoue sous toutes les politiques demandées.
Dans un sous-arbre sans solution, tous les candidats de chaque case sont
essayés : les comptes de nœuds devraient donc coïncider.

**Mais cette dernière lecture repose sur une hypothèse qu'il faut dire** : que
l'ordre des **variables** ne dépend pas de l'ordre des **valeurs**. Elle est
vraie du moteur actuel ; elle cesse de l'être d'un moteur dont le départage de
cases **apprend** de la recherche (§4.14 de
[conception/elagage_recherche.md](conception/elagage_recherche.md) : un poids
d'échec par case, accumulé dans l'ordre où les échecs surviennent — donc dans
un ordre que la politique de valeurs détermine). Mesuré sur un tel moteur : la
même racine morte ferme en 168 152 à 225 683 nœuds selon la politique, sans
qu'aucune permutation soit fausse.

C'est pourquoi (b) n'est **pas** fatal. Les permutations ayant déjà été validées
par (a), un désaccord ne peut plus signifier qu'une chose : le moteur lie les
deux ordres. Le banc l'affiche comme un **diagnostic**, avec les comptes par
politique et les deux conséquences à connaître — « l'ordre des valeurs est
neutre pour la réfutation » cesse d'être vrai, et une comparaison de politiques
mesure alors deux effets à la fois.

L'auto-test tourne dans le **processus courant** (pas de fork),
mais dans une parenthèse isolée : `chdir` vers le répertoire de travail et
sortie standard détournée. Ce n'est pas une précaution de style — fermer une
racine, c'est l'explorer entièrement, donc `stop_on_solution` y vaut 0 par
nécessité, et toute solution rencontrée en chemin passe par `log_solution`
(un fichier `solution_<pid>_<seq>` dans le répertoire courant, la grille
complète sur stdout). Sans cet isolement, une campagne de 60 instances déverse
des milliers de fichiers et de plateaux dans le répertoire d'où le banc a été
lancé. Le budget par défaut (200 000) est délibérément bas pour ne rien coûter ;
sur des instances où aucune racine ne ferme si vite, l'auto-test rend « non
concluant » — le relever (`--selftest-budget 2000000`) rend alors le contrôle
effectif, à quelques dixièmes de seconde par instance. Même intention que
l'auto-test `--w2x2` et l'oracle indépendant de `bench_refutation` : le symptôme
qu'on espère d'une politique gagnante est exactement ce qu'un hook bogué
produit, donc l'instrument se valide avant de mesurer.

### 4. Ce que le banc n'ajoute PAS à la production

L'ordre des valeurs entre dans le moteur par un **`#ifdef ETII_BENCH_HOOKS`**
(`src/core/etii_search.c`), défini par la seule unité de compilation du banc —
jamais un drapeau runtime, conformément à la règle §6 de
[conception/mrv_moteur_unique.md](conception/mrv_moteur_unique.md) et à la même
discipline que `ETII_ARENA_ORDER`. Hors banc, la macro `ETII_BENCH_ORDER_INDEX`
se réduit à l'identité. **Vérifié objectivement** : `build/core/etii_search.o`
compilé avant et après l'ajout du hook est **octet pour octet identique**
(`cc -O3 -ffast-math -mpopcnt -Isrc -Werror -c`). Refaire ce contrôle avant de
toucher au hook.

Les fonctions d'agrégation (médiane, moyenne géométrique, survie, appariement,
redémarrage) sont pures et vivent dans `tests/bench/bench_solve_stats.{h,c}`,
compilées dans le binaire de test et couvertes par
`tests/bench/test_bench_solve_stats.c` — rattachées à `make test`, comme le
cœur pur de `gen_root`. Le banc lui-même ne l'est pas.

### 5. Ce que la première campagne a donné

Résultat d'ensemble, sur 2 640 exécutions et quatre régimes d'instances :
**aucun ordre des valeurs ne bat celui de production**, et le seul point de
départ réellement distinct du choix de MRV est nettement plus mauvais. Détail,
chiffres et décisions :
[conception/banc_resolution_clones.md §6 et §7](conception/banc_resolution_clones.md).

Trois enseignements de méthode, utiles avant toute campagne future :

- **Mesurer le temps autant que les nœuds.** `mcv` gagne en nœuds sur une
  cellule (p = 0,025) et perd en temps sur les quatre (1,27× à 1,40× par nœud).
  En ne lisant que les nœuds, on l'adopterait.
- **Compter les paires indécises comme telles.** Sur les cellules dures, c'est
  la *part résolue au plafond* (14/60 contre 54/60) et non l'appariement qui
  départage `center`, l'appariement restant indécis faute de paires complètes.
- **Une référence censurée ne conclut pas.** Le coût « sans redémarrage » n'est
  qu'une borne inférieure dès qu'une exécution bute sur le plafond, et la
  censure joue dans le sens qui favoriserait le redémarrage : on ne peut donc
  rien conclure contre lui sur ces cellules-là.

## Banc « oracle CDCL » (`make bench-cdcl`)

`bench_refutation` mesure ce que le moteur du dépôt dépense pour prouver qu'un
sous-arbre est mort. Il ne peut pas dire si ce coût est **intrinsèque à
l'instance** ou **propre à sa famille d'inférence** : pour cela il faut donner
les mêmes racines à une famille d'inférence différente. Ce banc le fait avec un
solveur SAT à **apprentissage de clauses** (CDCL), la seule famille qui, elle,
a une mémoire des conflits — le résidu que
[conception/elagage_recherche.md](conception/elagage_recherche.md) §4.11 laisse
explicitement non mesuré (« la propagation des ensembles de conflit »).

C'est un **oracle**, pas un moteur : rien n'entre dans `src/`, et la campagne
n'a débouché sur aucun changement de production. Méthode, encodage, tables et
verdict : [conception/oracle_cdcl.md](conception/oracle_cdcl.md).

```sh
# le solveur n'est PAS fourni par le dépôt
git clone https://github.com/arminbiere/kissat && (cd kissat && ./configure && make)
KISSAT=$PWD/kissat/build/kissat make bench-cdcl

# le régime qui compte (racines peu profondes, les plus chères pour le DFS)
KISSAT=... make bench-cdcl BENCH_CDCL_ARGS="--min-pieces 100 --max-pieces 120 --roots 30"
```

Comme `bench-refutation` et `bench-solve`, la cible est `.PHONY` et **n'entre
pas dans `make test`** — c'est un banc, et il exige un binaire externe que le
dépôt ne fournit ni ne télécharge (`$KISSAT`, sinon le `PATH`). Sans solveur,
la cible sort proprement en disant où en trouver un.

### Ce que le banc joue, et dans quel ordre

1. **Contrôle positif, obligatoire.** `tools/root_to_cnf.py --self-test` encode
   trois instances à **solution connue** — le puzzle 16 pièces
   (`data/pieces16.csv`, plateau vide) et deux racines d'un clone 16×16 tiré par
   `tools/gen_clone.py`, à 139 et 120 pièces posées, soit exactement les deux
   profondeurs mesurées ensuite. Le solveur doit rendre SAT, **et le modèle est
   re-vérifié pièce par pièce** par le script : une et une seule pièce par case,
   une et une seule case par pièce, les quatre faces de chaque case contrôlées
   contre leurs voisines et contre le cadre. Le banc **refuse de mesurer** si ce
   contrôle échoue. C'est la garde la plus importante de l'instrument : un
   encodage sur-contraint rend des UNSAT gratuits, et toute la campagne
   mesurerait alors un bug plutôt qu'une instance.
2. **Bras DFS.** `bench_refutation` sur le filtre demandé.
3. **Bras CDCL.** La même racine, encodée puis soumise au solveur.
4. **Bilan apparié** : ratios nœuds/conflits et temps/temps, médiane et bornes,
   nombre de racines où le CDCL gagne d'un facteur 10, et — le second volet du
   critère — nombre de racines **laissées ouvertes par le DFS** que le solveur
   tranche.

### L'appariement, et pourquoi il est exact

Une racine est désignée par **(filtre de taille, rang)**. C'est la règle de
sélection de `bench_refutation` lui-même : les paquets du `.back` sont lus dans
l'**ordre du fichier**, ceux qui passent `--min-pieces`/`--max-pieces` sont
retenus, et les `--max-roots` premiers joués sous les étiquettes `back#0`,
`back#1`… `tools/root_to_cnf.py --min-placed/--max-placed/--rank` applique la
même règle, donc le rang `k` du bras CDCL **est** la ligne `back#k` du bras DFS.
Aucune agrégation sur deux populations différentes — le piège relevé au §4.13 de
`elagage_recherche.md`.

### `tools/root_to_cnf.py` seul

L'encodeur s'emploie aussi hors du banc :

```sh
# contrôle positif
python3 tools/root_to_cnf.py --self-test --solver /chemin/kissat

# contrôle du décodage du stock : 32 480 paquets, adjacences, cadre, doublons
python3 tools/root_to_cnf.py --check-back eternityII.back

# une racine -> CNF, puis re-vérification d'un éventuel modèle
python3 tools/root_to_cnf.py --back eternityII.back --placed 139 --rank 0 \
        --out /tmp/r.cnf --map /tmp/r.map.json
kissat /tmp/r.cnf > /tmp/r.log
python3 tools/root_to_cnf.py --verify --map /tmp/r.map.json --model /tmp/r.log
```

Un **SAT sur une racine réelle** serait soit une **solution du puzzle**, soit un
bug d'encodage : les deux instruments s'arrêtent, conservent le modèle et le
passent au vérificateur plutôt que de poursuivre la mesure.

L'encodage est celui de Heule (SAT 2008), compact : les pièces voisines ne sont
jamais comparées deux à deux, l'appariement transite par une variable de couleur
portée par l'arête. Une racine de 139 pièces donne ≈ 5,6·10⁴ variables et
≈ 2,0·10⁵ clauses (3,0 Mo de DIMACS), produites en ≈ 0,65 s de Python pur — à
comparer aux ≈ 0,7 ms que le DFS met à fermer la même racine. **Ce coût
d'encodage compte dans la lecture** : il est à lui seul de trois ordres de
grandeur au-dessus du bras qu'il sert à mesurer.

## Compaction du stock, disque et mémoire (`core/packet_codec.c`)

La suite `packet_codec_suite` (`tests/core/test_packet_codec.c`) verrouille le
format compact des `.back` et des segments de débordement. Ce qu'elle vérifie,
dans l'ordre d'importance :

- **L'aller-retour est exact à TOUTES les profondeurs de plateau**, de 0 à
  `ETERN_PARTS`, pas sur un échantillon : le plateau vide et le plateau plein
  sont les deux bords où un calcul de taille faux passerait inaperçu sur un
  tirage aléatoire. Le générateur est déterministe (xorshift, graine fixe) pour
  qu'un échec soit rejouable.
- **`alloc` et `b_faceused` sont RECONSTRUITS, pas stockés** : un paquet dont
  ces deux champs mentent doit ressortir avec les valeurs que la grille impose.
  Le test tomberait si quelqu'un les ajoutait au format.
- **Un enregistrement n'est jamais plus gros que la forme brute.** C'est la
  propriété qui interdit toute régression de taille quel que soit le profil de
  profondeur du stock — deux formes plus compactes EN MOYENNE ont été écartées
  pour avoir échoué exactement ici (tableau dans
  [Forme compacte d'une possibilité](format_stock_compact.md#formes-concurrentes--deux-écartées)). Elle est aussi
  vérifiée à la compilation.
- **Le bourrage d'un paquet décodé est déterministe** : le pool analysé hache
  le paquet octet par octet (`hash_possibility_key`), donc deux décodages du
  même enregistrement doivent donner deux images mémoire identiques, sinon un
  paquet restauré ne se dédupliquerait jamais contre son jumeau produit en
  direct.
- **Ce qui est refusé, et ce qui ne l'est pas.** Une valeur de case
  irreprésentable (négative autre que `-2`, au-delà de la dernière rotation)
  est refusée ; `0` ne l'est PAS, bien qu'il ne soit l'identifiant d'aucune
  pièce. Une première version le refusait, au nom de l'intégrité — et échouait
  sur une demi-douzaine de fixtures qui construisent un paquet par `memset(0)`,
  idiome répandu dans cette base de test. La leçon, payée en allers-retours :
  **un sérialiseur n'a pas à juger de la légalité de ce qu'on lui confie**, il
  doit le rendre tel quel ; la validation d'un plateau est une autre affaire,
  et un autre endroit.
- **Un en-tête de fichier d'une autre version ou d'une autre géométrie est
  refusé**, jamais réinterprété.

Côté `datamanager` et `stock_spill`, trois tests supplémentaires couvrent le
volet fichier : la sauvegarde porte bien la magie et pèse une fraction de la
forme brute, l'aller-retour préserve le CONTENU du plateau (pas seulement le
nombre de possibilités), et un `.back` compacté d'une autre géométrie est
refusé sans toucher au stock courant. Le format HÉRITÉ reste couvert par les
tests pré-existants, qui écrivent des `.back` bruts à la main
(`write_synthetic_back`).

**La conversion d'un cliché de débordement hérité** a deux tests :
`restore_snapshot_converts_a_legacy_format_snapshot` (`manifest.txt` en v1,
segments en `possibility_packet` bruts) et
`restore_snapshot_converts_a_v2_stride_snapshot` (v2, forme compacte à pas
fixe — ce qu'un serveur mis à jour trouve à côté de son dernier `.back`) : ce
sont les seuls chemins où des données réelles traversent la frontière entre
formats, donc les seuls qui puissent attraper une confusion de pas. Vérifié par
sabotage — forcer la lecture au pas compact d'un cliché v1 le fait tomber.

**Les segments en trames** (manifeste v3, un bloc de l'étage par trame) ont
trois tests propres : `tier_blocks_reach_the_disk_in_their_stored_form` (un
bloc part sur disque dans sa forme stockée : zstd sous `ZSTD=1`, moins que la
forme compacte, bien moins que l'ancien pas fixe),
`tier_rollover_after_a_partial_reload_trims_the_left_segment` (une trame
insécable fait rouler la pile après un rechargement partiel : sans le recalage
du segment quitté, deux possibilités déjà servies reviennent — vérifié par
sabotage) et `restore_snapshot_refuses_a_damaged_frame` (un pied de trame qui
ne correspond plus à son en-tête : le groupe n'est pas restauré).

### Aucune perte sous plafond RAM : trois tests, trois sabotages

`restore_under_a_ram_cap_loses_nothing` rejoue à petite échelle le cas réel
(200 possibilités, plafond de 50) et vérifie que `résident + déporté` vaut
toujours 200. Sabotage : faire ignorer à `import` la valeur de retour d'
`add_possibility` — l'ancien comportement — rend 50 sur 200.

`import_makes_room_itself_when_it_holds_the_maintenance_window` couvre
l'appelant qui DÉTIENT la fenêtre de maintenance et importe dedans. Il tourne
dans un FILS avec `alarm()`, parce que sans le correctif il ne rend jamais la
main : un test qui pend bloque la CI au lieu d'échouer. Sabotage : refermer la
porte de `stock_spill_relieve` (le faire abandonner sous maintenance comme
`stock_spill_step`) fait tuer le fils par l'alarme — `run_in_fork` rend -1.

`restore_keeps_the_maintenance_window_open_through_the_import` verrouille la
cohérence de la fenêtre elle-même. La fenêtre que `restore_apply`
(`ui/command_lines.c`) pose pour « TOUTE la séquence » ne tenait pas : `maintenance`
était un DRAPEAU, et le `unlock_all_file()` interne à `restore()` le remettait à 0
**avant l'import**. Le débordement redevenait donc actif en plein remplacement du
stock, exactement ce que la fenêtre existait pour interdire — un défaut préexistant,
découvert en corrigeant la perte de possibilités ci-dessus. `maintenance` compte
désormais sa **profondeur d'imbrication** (`maintenance_enter`/`maintenance_leave`,
`core/datamanager.c`) : un `unlock_*` imbriqué ramène la profondeur de 2 à 1, et
seul le `datamanager_end_maintenance()` correspondant referme la fenêtre.

Le test pose la fenêtre, lance `restore()` sur un `.back` et observe à deux
instants : le crochet de dégagement RAM sert de **sonde** au cœur de l'import
(le plafond garantit qu'il est atteint — le test échoue aussi s'il ne l'est
jamais, sans quoi il passerait à vide), puis la fenêtre doit encore être ouverte
au retour de `restore()`. Il tourne lui aussi dans un FILS avec `alarm()` : la
fenêtre tenue rend `stock_spill_step` inerte, donc une régression du crochet de
dégagement se manifesterait par un blocage. Sabotage : rendre le décrément
inconditionnel (le drapeau d'origine) le fait tomber aux deux observations —
code 4 sur la sonde, code 3 au retour.

C'est ce correctif qui rend les deux tests solidaires : la fenêtre tient
vraiment, donc `import` DOIT faire la place lui-même
(`datamanager_set_ram_relief_hook` → `stock_spill_relieve`), sans quoi l'attente
de place ne serait jamais servie. Refermer la porte de `stock_spill_relieve`
n'est plus une régression théorique mais un blocage sur le chemin réel du
`restore` console.

### Les lecteurs de `.back` hors du programme

`bench_refutation --from-back` lit des stocks de PRODUCTION, donc des fichiers
des deux âges. Il passe par un `back_reader_t` qui détecte le format sur la
magie, comme `import` — sans quoi un `fread` au pas de 576 octets sur un
fichier compact ne se plaindrait de rien : il fabriquerait des plateaux
absurdes et le banc mesurerait du bruit. Vérifié sur le même stock converti
dans les deux formats : profil identique (3 407 891 possibilités, pièces
posées min/moy/max 19 / 19,2 / 152).

`tests/tools/gen_root`, lui, **écrit** une racine au format brut : c'est
toujours lisible (détection sur la magie), et laisser cet outil en l'état évite
de lui faire dépendre du codec pour un fichier d'une seule possibilité.

### Vérification sur données réelles

Le codec a été passé sur un stock de production réel (`eternityII.back`,
3 407 891 possibilités, 1 963 Mo) avant d'être branché : **aller-retour exact
sur les 3 407 891 paquets, zéro divergence**, 65,2 octets par possibilité en
moyenne (x8,83). Puis bout en bout, par `import` → `backup` → `restore` :
**1 962 945 216 → 222 314 458 octets**, 3 407 891 possibilités relues. Un
second cycle donne un fichier qui diffère octet pour octet du premier — mais
les deux portent le même MULTI-ENSEMBLE de possibilités : c'est la répartition
round-robin entre files qui change l'ordre, pas le contenu. Ne pas conclure
d'un `cmp` qui échoue que l'aller-retour perd quelque chose.

### Étage mémoire : le stock résident (`init_file_variable`, `core/datamanager.c`)

Les deux pools de stock rangent la même forme compacte que le disque. Mesuré sur
le même stock de production, `restore` puis lecture de l'occupation réelle
(`datamanager_resident_bytes`) : **632 → 121,2 octets par possibilité, soit
2154 Mo → 413 Mo (x5,21)** pour 3 407 891 possibilités. Le pool ANALYSÉ reste en
paquets bruts, délibérément (cf. `AGENTS.md`).

### Campagne de non-régression client et pruner

La compaction échange de la mémoire contre du calcul ; la question posée n'était
pas un gain mais l'absence de dégradation. Protocole : binaires construits une
fois, **ordre ALTERNÉ entre variantes** (la machine dérive thermiquement de
plusieurs pour cent à l'heure — « toutes les répétitions de A puis toutes celles
de B » compare deux températures autant que deux codes), médianes.

Coût absolu du codec (200 000 paquets, 3 répétitions, aller-retour
encodage + décodage contre le `memcpy` du struct que le stock payait avant) :

| pièces posées | octets/enr. | encode+decode | `memcpy` aller-retour |
|---|---|---|---|
| 8 | 49 | 0,81 µs | 0,40 µs |
| **19** (moyenne du stock réel) | 65 | **0,91 µs** | 0,39 µs |
| 130 | 217 | 1,53 µs | 0,38 µs |
| 255 | 389 | 2,28 µs | 0,39 µs |

Soit **+0,52 µs par possibilité traversant le stock** à la profondeur réelle.
Bout en bout :

| régime | instrument | master | compact | écart |
|---|---|---|---|---|
| trafic de stock PUR — expansion niveau 14 (142 415 possibilités), 11 rép. | temps de l'expansion | 1,350 s | 1,306 s | **−3,3 %** |
| idem, niveau 18 (1 075 265 possibilités), 7 rép. | temps de l'expansion | 15,987 s | 15,928 s | −0,4 % |
| pruner — serveur + pruner 4 forks, fenêtre 65 s, 3 rép. | possibilités servies (décodage) | 73 400 | 74 600 | +1,6 % |
| idem | ADD encaissés (encodage) | 53 053 | 53 985 | +1,8 % |
| client — serveur + client 4 forks, fenêtre 65 s, 8 rép. | ADD encaissés | 31 850 | 31 500 | −1,1 % |

Aucune dégradation, et un léger gain là où le codec pèse le plus : les 0,5 µs
d'encodage sont remboursés par les 65 octets écrits au lieu de 576, une
allocation de moins par possibilité et un cache bien mieux utilisé.

**Trois règles de méthode que cette campagne a payées**, à relire avant de
refaire une mesure de ce genre :

1. **Les écarts positifs ci-dessus ne sont pas des gains.** Ils sont dans le
   bruit ; seul leur signe compte, et il exclut la dégradation.
2. **Le banc CLIENT ne résout rien en dessous de ±10 %** — étendue relative de
   14 % (master) et 34 % (compact) sur 8 répétitions, parce que son débit est
   gouverné par la recherche et non par le stock : 490 ADD/s, contre ~2 000
   opérations de codec par seconde en régime pruner. Son −1,1 % médian ne peut
   donc pas être imputé au codec — le même codec, sollicité quatre fois plus
   fort, ne coûte rien. C'est l'expansion, purement stock et bien plus précise,
   qui répond pour lui.
3. **Diagnostiquer un run aberrant plutôt que le moyenner.** Un run a produit
   133 969 possibilités contre 142–147 k ailleurs : collision sur le port 2020
   (non paramétrable en CLI) avec un autre `eternityII` de la machine. Un banc
   client/serveur doit vérifier que ce port est libre avant de mesurer.

Une sonde trop grossière a aussi failli publier un faux résultat : un premier
chronomètre à 100 ms démarré au lancement du processus (construction de la map
et lecture du CSV comprises) donnait l'expansion **+3,6 % plus lente** ; la même
mesure à 10 ms entre les deux lignes de journal donne −3,3 %. Sur 1,7 s, une
sonde à 100 ms vaut ±6 %.

## Banc de l'étage RAM compressé (`make bench-ram-tier`)

`tests/bench/bench_ram_tier.c` répond à une seule question, celle de
[docs/conception/etage_ram_compresse.md](conception/etage_ram_compresse.md) : combien
d'octets **réellement tenus par l'allocateur** coûte une possibilité du stock, selon qu'elle
est rangée en maillon de liste chaînée (le pool actuel) ou en blocs contigus, éventuellement
compressés ? Et à quel débit passe-t-on de l'une à l'autre ?

```sh
make bench-ram-tier BENCH_RAM_TIER_ARGS="--count 2000000 eternityII.back"
make bench-ram-tier BENCH_RAM_TIER_ARGS="--count 2000000 --skip 130000000 --shuffle --blocks 64 --codecs none,zstd-1 eternityII.back"
```

- La source est un `.back` **compact** (magie `ETIISTK`) de la géométrie compilée. Les
  enregistrements sont lus et déplacés sans être décodés, sous la forme même des pools.
  `--skip N` saute N possibilités (lues, donc au prix de l'E/S), et `--shuffle` mélange
  l'échantillon avec une graine fixe pour casser toute localité entre voisins.
- Chaque configuration (`--codecs` × `--blocks`, en Kio) tourne dans un **fils**. Le banc
  remplit une `File` comme le pool (`init_file_variable` + `put_sized`), l'évince par la tête
  (`scroll_fifo_sized`) vers des blocs, puis la recharge bloc le plus récent d'abord.
- **Octets par possibilité** : pour la liste, le delta de `mallinfo2` (chunks en usage +
  mmap) ; pour les blocs, `malloc_usable_size` + l'en-tête de chunk, plus le tableau qui les
  indexe. Le RSS est affiché pour la liste à titre de contrôle. Il n'est pas utilisé pour
  les blocs, parce que le fils hérite des pages de l'échantillon et que l'allocateur garde
  les chunks libérés par l'éviction.
- **Vérification** : après rechargement, même compte et même multi-ensemble
  d'enregistrements (somme de hachages FNV-1a, indépendante de l'ordre). Sinon, la
  configuration est déclarée `FAUX` et le banc sort avec le code 3. Deux sabotages ont été
  vérifiés : un bit du bitmap retourné dans un bloc déclenche « bloc illisible », un bit
  d'une valeur déclenche « hachage DIFFÉRENT ».
- **Codecs** : `none` est toujours disponible. `lz4` et `zstd-N` ne sont compilés que si
  leurs en-têtes sont visibles (`__has_include`), et la cible ajoute `-lzstd`/`-llz4`
  d'après les mêmes en-têtes. Sans paquet `-dev` installé, il suffit des en-têtes extraits
  du paquet et des bibliothèques d'exécution :
  `BENCH_RAM_TIER_CFLAGS="-isystem <dir>/usr/include" BENCH_RAM_TIER_LIBS="-l:libzstd.so.1 -l:liblz4.so.1"`.
  Ce banc est le seul code du dépôt à dépendre de ces bibliothèques, et rien de la
  production ne le lie.
- **Linux/glibc uniquement** (`mallinfo2`, `malloc_usable_size`), avec une `#error` explicite
  ailleurs. Comme les autres bancs, il n'est pas rattaché à `make test`.

Les résultats (liste 112,0 octets/possibilité, blocs bruts 70,0 soit ×1,60, zstd -1 en blocs de
64 Kio 31,4 soit ×3,57) et leur lecture sont dans le document de conception.

## Garde-fou de durée (`TEST_TIMEOUT`)

Chaque binaire de test est lancé sous une alarme, 600 s par défaut :

```sh
make test                    # garde-fou à 600 s par binaire
make test TEST_TIMEOUT=1200  # suite lente (ASan, machine chargée)
make test TEST_TIMEOUT=0     # désactivé
```

**Pourquoi** : un test qui PEND ne donne rien à personne. En local on attend sans
savoir quoi ; en CI le job se fait tuer par le plafond de la forge après des
dizaines de minutes, sans jamais nommer le test fautif. Ce n'est pas théorique —
trois blocages distincts sont survenus en une seule session de travail sur le
stock, chacun pour une cause différente : une boucle `while (size > 0)
scroll(...)` sur une file qui refusait de se vider, une attente de place sous un
plafond RAM que rien ne pouvait plus libérer, et un compteur d'octets saboté
pour un test qui rendait ce même plafond indépassable.

Au dépassement, le binaire meurt sur `SIGALRM` et la cible échoue nettement
(`make: *** [test-16] Alarm clock: 14`). Le dernier test AFFICHÉ avant l'arrêt
désigne le coupable : relancer avec `-v` (`./tests/run_tests_16 -v`) fait
afficher chaque test au fil de l'eau, ce qui le nomme exactement.

L'implémentation passe par `perl -e 'alarm shift; exec @ARGV'` plutôt que par
`timeout` : ce dernier n'existe pas sur macOS sans installer les coreutils GNU,
alors que perl est présent partout où ce projet se compile.

## Voir aussi

- [tests/README.md](../tests/README.md) — organisation des suites, conventions, ajout d'un test.
- [Compilation](compilation.md) — options de build et drapeaux de configuration.
- `tests/bench/bench_search.sh` — banc de mesure du débit de recherche (voir ci-dessus).
- `tools/gen_clone.py` — générateur de clones à solution connue (voir ci-dessus).
- `tools/root_to_cnf.py` — encodeur DIMACS d'une racine, et son contrôle positif (voir ci-dessus).

# Tests, couverture et intégration continue

Ce document décrit les cibles de test du Makefile, les scénarios d'intégration
bout-en-bout, le rejeu de la CI en local via Docker et ce que fait la CI GitHub
Actions. Les conventions d'écriture des tests unitaires (organisation des suites,
fixtures, ajout d'un test) sont dans [tests/README.md](../tests/README.md).

## Cibles

```sh
make test             # compile tests/ + lance la suite unitaire (code de sortie non nul si échec)
make test-integration # scénarios bout-en-bout 16 pièces : solution client/serveur + canal de contrôle
make test-docker      # rejoue les jobs de test CI dans 3 conteneurs Linux en parallèle (nécessite Docker)
make test-docker-arm  # vérifie la compilation croisée ARM 64-bit (Raspberry Pi) dans le même conteneur (nécessite Docker)
make coverage         # les deux passes (256 + 16) + résumé texte gcovr fusionné (nécessite gcovr)
make coverage-256     # passe 256 pièces seule ; résumé gcov par module
make coverage-report  # rapports gcovr : Cobertura XML + HTML + résumé Markdown
make gen-root         # outil : convertit un plateau externe en racine de stock .back
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
  puis `make CUDA=1 VERIFY=1`), un build activant **tous** les flags `DEBUG_*` de
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

## Outil `border_mass` (`make border-mass`)

Mesure la **masse totale** des anneaux de bordure valides (les
`BORDER_RING_LEN` = `4×(ETERN_SIZE-1)` cases du pourtour du plateau — 60 sur
le puzzle 256 pièces) : une recherche exhaustive ancrée au coin `(0,0)`
trouve toute la population d'anneaux, et le nombre brut trouvé **est** déjà
la masse totale — aucun voisin n'est encore posé à la toute première case,
donc le DFS explore de lui-même les 4 coins possibles comme point
d'ouverture, retrouvant chaque anneau abstrait une fois par coin (constaté
empiriquement pendant l'implémentation). Cela suppose qu'aucun indice
officiel ne touche une case de bord (vérifié au démarrage par l'outil
lui-même) — sinon un seul coin serait valide comme ouverture, pas 4. Voir
[docs/conception/border_mass.md](conception/border_mass.md)
pour le raisonnement complet.

```sh
make border-mass
tests/tools/border_mass data/pieces.csv data/indices.csv
```

Depuis l'ajout du parallélisme, la recherche parallèle « coins d'abord » est
le comportement PAR DÉFAUT (pas une option opt-in) — `--forks 1` est la
façon de demander un seul worker, pas de revenir au DFS séquentiel
`border_ring_order` historique (qui reste accessible en code, via
`border_walk_count`, mais plus utilisé par cet outil).

`--forks N` (défaut : nombre de cœurs détecté) parallélise la recherche :
le plateau est posé dans l'ordre « coins d'abord » plutôt que l'ordre
séquentiel (très peu de pièces ont 2 faces nulles adjacentes — 4 sur le
jeu 256 pièces réel — donc le facteur de branchement des 4 premières étapes
est minuscule), la recherche est étendue en largeur jusqu'à `N*8` états
partiels puis distribuée à `N` process forkés. Chaque worker journalise sa
progression sur `stderr` à deux granularités : une ligne par partition
terminée (« partition X/Y terminee, sous-total N »), et — comme une seule
partition peut à elle seule tourner bien plus de 15 minutes sans jamais se
terminer — une ligne tous les `BM_PROGRESS_INTERVAL_NODES` nœuds DFS visités
(défaut 10⁸, réglé empiriquement sur `data/pieces.csv` pour ~1 ligne toutes
les 4-5 secondes) donnant le nombre de nœuds explorés, d'anneaux trouvés
dans la partition en cours, et un débit nœuds/s instantané — un signal de
vie et de vitesse indépendant du bouclage d'une partition entière. Le
callback de progression (`struct border_progress_opts`,
`tests/tools/border_walk.h`) est un point d'extension générique du cœur
d'énumération ; `border_walk.c` ne lit aucune horloge (reste un cœur pur
sans I/O), tout le calcul de vitesse et le formatage du message vivent dans
`border_mass.c`. Voir
[docs/conception/border_mass.md](conception/border_mass.md)
pour le raisonnement complet (notamment pourquoi ceci ne réutilise pas
`fork_gate.c`).

Mesuré empiriquement sur `data/pieces.csv` (256 pièces) : ne termine pas en
plusieurs heures (une seule partition sur 270 avait déjà dépassé 33 milliards
d'anneaux trouvés sans se terminer, `--forks 4`, run interrompu) — le DFS
séquentiel n'a aucune heuristique d'élagage (MRV, forward-check), contrairement
au moteur de recherche principal, par choix de conception explicite (voir la
spec). Ordre de grandeur explicable : les faces "anneau" du vrai puzzle
n'utilisent que 5 couleurs, chacune partagée par 24 des 120 faces de bord —
une bordure volontairement peu contrainte par conception du puzzle réel, qui
donne un facteur de branchement élevé au DFS séquentiel.

### `--dp` : comptage exact par programmation dynamique sur classes de pièces

`--dp` bascule sur `border_ring_count_dp` (`tests/tools/border_ring_dp.c`),
un algorithme radicalement différent et EXACT (même définition, même
résultat que `border_walk_count`) : la face intérieure d'une pièce de bord
n'étant jamais vérifiée par ce comptage, deux pièces de bord partageant les
deux mêmes couleurs "anneau" **dans le même ordre** (laquelle est attendue en
entrée, laquelle est produite en sortie — une propriété FIXE de chaque pièce
réelle, pas un choix libre, cf. le commentaire de `bd_build_classes`) sont
strictement interchangeables. Les regrouper en classes et calculer, NIVEAU
PAR NIVEAU (une position de l'anneau à la fois — voir plus bas pourquoi),
le nombre de façons d'atteindre chaque état (couleur requise, compteurs
restants PAR CLASSE — `nb_classes≈28` sur le vrai jeu 256 pièces, au lieu
d'un masque de bits par pièce parmi 60) transforme l'énumération
exponentielle en programmation dynamique.

**Une seule pièce-coin d'ouverture est calculée en entier** (`bd_count_openings`) : par
symétrie de rotation à 90° du plateau, toute bordure valide utilise
nécessairement l'ensemble des pièces-coin candidates à `(0,0)`, une fois
chacune — leurs totaux individuels sont donc rigoureusement égaux (vérifié
empiriquement avant ce changement : les 4 candidats de `data/pieces16.csv`
donnent chacun `N=1`). Un seul est calculé, puis multiplié par le nombre de
candidats — au lieu de rejouer le DP complet une fois par candidat (÷4 sur le
temps total pour le jeu 256 pièces, qui en a exactement 4).

**Piège corrigé pendant l'implémentation** : grouper par paire de couleurs
NON ORDONNÉE (au lieu de la paire ORDONNÉE entrée/sortie) donnait 128 au lieu
de 4 sur `data/pieces16.csv` (32× trop) — deux pièces réelles peuvent
partager les deux mêmes couleurs "anneau" avec une orientation opposée l'une
de l'autre, et ne sont alors PAS interchangeables. `tests/tools/test_border_ring_dp.c`
verrouille ce cas précis avec les vraies données `data/pieces16.csv`
(gated `#if ETERN_PARTS == 16`, même convention que `test_solution16.c`).

**Architecture « niveau par niveau », pas une table globale** : une première
version mémoïsait les 59 positions de l'anneau dans UNE SEULE table (position
incluse dans la clé). Or un état à la position P ne dépend jamais que du
niveau P+1 déjà calculé, jamais des autres positions — cette table globale
gardait donc en mémoire des positions déjà consommées, inutilement.
`border_ring_count_dp` ne garde plus que le niveau courant et le niveau en
construction (`struct bd_level`, tableaux parallèles `keys`/`values`/bitmap
`occupied` — un `struct { clé; valeur; occupé; }` gâchait ~40 % de
remplissage d'alignement à cause du `long long` voisin, et dimensionnait
chaque clé sur une borne `BD_MAX_CLASSES=64` plutôt que sur `nb_classes` réel).
Le vrai levier mémoire est la structure par niveau elle-même (borne le pic à
la position la PLUS chargée, pas la somme des 59), le gain d'octets par
emplacement n'étant qu'un facteur secondaire (~×2,3 à lui seul, mesuré avant
ce changement de structure).

**Parallélisation par forks (`--forks N` combiné à `--dp`)** : la transition
d'un niveau vers le suivant ne dépend que du niveau courant, déjà calculé et
immuable — ses états sont donc indépendants les uns des autres. Dès qu'un
niveau dépasse 50 000 états, sa transition est répartie sur `N` process
forkés (chacun une plage d'indices disjointe), chaque worker écrivant sa part
du niveau suivant dans un fichier temporaire (jamais un pipe : la sortie
sérialisée peut atteindre des dizaines de Mo, bien au-delà du tampon noyau
d'un pipe, et le parent ne lit qu'un worker à la fois — un pipe bloquerait un
worker en écriture pendant que le parent lit un autre worker, interblocage
classique). Le parent fusionne les fichiers par ACCUMULATION
(`bd_level_add`), pas une simple concaténation : deux workers différents
peuvent légitimement produire le même état suivant depuis des états de
départ différents. Correctness verrouillée par
`border_ring_count_dp_matches_brute_force_when_forked`
(`tests/tools/test_border_ring_dp.c`), qui abaisse le seuil de déclenchement
à 1 état via le hook test-only `border_ring_dp_set_fork_min_states_for_tests`
(même schéma que `stock_spill_set_segment_bytes_for_tests`) pour exercer
réellement fork+fichier+fusion sur un fixture minuscule.

**Limite mesurée (niveau en mémoire, avant le mode disque décrit plus bas)** :
sur `data/pieces.csv`, la taille des niveaux croît vite et ne semble pas près
de plafonner — 21 001 574 états (2,32 Go) déjà atteints à la position 17/59
sur une petite machine (échec propre sous un plafond `ulimit -v` de 5 Go en
tentant la position 18), et **9,28 Go déjà atteints à la position 19/59 sur
la machine cible (2×10 cœurs, 48 Go de RAM)**, qui a fini par échouer à la
position 20 — la parallélisation par forks borne le CALCUL d'une transition
par plages d'indices, mais la table de sortie fusionnée par le parent reste
un niveau ENTIER en mémoire : plus de cœurs accélère le calcul, plus de RAM
recule l'échec, mais aucun des deux n'empêche un niveau de continuer à
grossir sans borne.

**Scission par pile LIFO, au-delà d'un budget RAM obligatoire
(`--dp-max-ram-mo MO`, aucune valeur par défaut)** — remplace un mécanisme
antérieur de « mode disque » permanent (deux vagues de forks à CHAQUE
position tant qu'un niveau restait trop gros), qui réécrivait/relisait
l'intégralité d'un niveau à chaque position tant qu'il dépassait le seuil :
sur une plage de plusieurs dizaines de positions consécutives toutes trop
grosses, ça multipliait le volume d'E/S par le nombre de positions
concernées (mesuré : ~777 Go d'E/S cumulées pour une masse de pointe de
111 Go étalée sur ~7 positions, sur la machine cible). `--dp-max-ram-mo`
pilote plusieurs seuils dérivés (`border_ring_dp_set_max_ram_mo`), tous du
même budget brut mais PAS de la même valeur en octets : `bd_pool_budget_bytes`
(plafond d'admission du mode POOL, somme visée sur tous les slots actifs)
prend directement la valeur donnée ; les scissions elles-mêmes sont
déclenchées PAR JOB, jamais par ce plafond d'admission, par deux seuils
distincts selon le mode — `bd_solo_budget_bytes` (mode SOLO, = budget / 4) et
`bd_pool_job_budget_bytes` (mode POOL, = budget / (nb_workers × 2)) ;
`bd_shard_target_bytes` (taille cible d'un fragment) est dérivée du budget
brut — `budget / (nb_workers × 3)` — pour que `nb_workers` fragments
simultanés pendant une compaction restent, ensemble, sous ce même budget.
Les deux seuils de scission par job sont détaillés ci-dessous. **Note pour
qui règle `--dp-max-ram-mo` en production** : à budget brut égal, le seuil de
scission effectif par job est maintenant plus bas qu'avant ce mécanisme
(`/4` en SOLO, `/(nb_workers × 2)` en POOL, au lieu d'une seule valeur brute)
— une conséquence voulue des marges de co-résidence ci-dessous, pas une
régression, mais qui vaut la peine d'être su avant d'augmenter la valeur du
flag pour compenser.

Quand un niveau dépasse le seuil, il est scindé en K fragments sur disque
(`bd_level_to_shards`, partitionnement externe par hachage FNV-1a de la
clé — la même technique qu'un GROUP BY externe qui ne tient pas en RAM) :
les K fragments sont TOUS empilés (**pile LIFO**, `struct bd_pending_stack`)
sur une pile partagée — un job qui vient de scinder ne garde jamais l'un des
fragments produits pour lui-même, il repousse sa production entière et
sort. L'ordre LIFO (le dernier fragment créé est repris en premier, pas le
plus ancien) borne la profondeur de la pile par le nombre de positions de
l'anneau, pas par la largeur de l'espace d'états — même raisonnement qu'une
pile explicite remplaçant une récursion en profondeur d'abord. C'est un
coordinateur central (`bd_run_opening`) qui décide, à chaque tour de boucle,
lequel des fragments en attente reprendre et comment.

**Signal de progression à la fermeture d'un fragment** : chaque ligne
`position %d/%d, %zu etats` n'affiche que la taille du niveau COURANT. Or le
nombre d'états atteignables à la DERNIÈRE position de l'anneau est borné par
le nombre de classes (chaque transition consomme exactement une pièce d'une
classe, donc à la dernière position il ne reste jamais qu'UNE seule pièce à
placer, toutes classes confondues) — indépendant de la taille du fragment qui
y arrive. Sur un run avec beaucoup de fragments, cette dernière position
revient donc répéter la même poignée d'états à chaque fragment traité, sans
rien montrer qui avance visiblement. `bd_apply_job_result` journalise donc,
dès qu'un fragment FERME l'anneau (`r->closed`), sa contribution et le total
cumulé :
`fragment ferme, +N anneaux (total cumule T, K fragment(s) en attente)` — un
signe de vie indépendant du numéro de position, utile pour distinguer un run
qui avance encore d'un run bloqué. Ce total cumulé est celui de l'UNIQUE
ouverture traitée par `bd_run_opening` (`bd_count_openings` le multiplie par
`nb_candidates` seulement après, donc ce nombre n'est pas encore le total
final affiché par `border_mass`).

**Pool de workers à deux modes (SOLO / POOL)** — `bd_should_run_solo(active,
stack_count, nb_workers)` tranche entre les deux, jamais simultanément : un
pool déjà lancé va jusqu'au bout de ses jobs actifs avant que le mode soit
réévalué.

- **Mode SOLO** (`active == 0 && stack_count < nb_workers`, c'est-à-dire tant
  que la pile n'a pas de quoi remplir tous les workers) : un seul job
  tourne, dans le process du coordinateur, et reste autorisé à se
  paralléliser en interne via `bd_transition_parallel` pour ses propres
  transitions de niveau — le comportement historique (un seul fragment
  actif à la fois). Son budget est `bd_effective_solo_budget_bytes()`, qui
  vaut `bd_solo_budget_bytes` (= budget donné / 4) — une marge
  **HEURISTIQUE, PAS un calcul exact** : `/2` pour la co-résidence de
  l'ancien et du nouveau niveau pendant toute la durée d'une transition,
  `/2` supplémentaire pour la non-déduplication entre les `nb_workers`
  tables locales de `bd_transition_parallel` (chaque worker produit sa
  propre table de niveau-suivant avant fusion par le parent, donc une même
  clé peut exister en double dans plusieurs tables simultanément) — cf. le
  commentaire à la déclaration de `bd_solo_budget_bytes` dans
  `tests/tools/border_ring_dp.c`.
- **Mode POOL** (dès que la pile contient au moins `nb_workers` fragments en
  attente) : le coordinateur forke jusqu'à `nb_workers` jobs concurrents, un
  par fragment (`bd_fork_pool_job`), chacun STRICTEMENT monoprocessus
  (`allow_parallel=0` — jamais `bd_transition_parallel`, jamais de
  compaction forkée dans une scission ultérieure). Un job ne reste jamais
  résident : il communique son résultat au coordinateur via un petit fichier
  (`struct bd_job_result`, écrit par `bd_job_result_write_or_die`, lu par
  `bd_job_result_read`) puis sort — il ne garde JAMAIS un fragment pour
  lui-même après une nouvelle scission, tous les fragments qu'il produit
  sont repoussés sur la pile partagée pour que le coordinateur les
  redistribue (`bd_apply_job_result`). Son budget de scission (passé à
  `bd_run_fragment_job` comme `effective_budget_bytes`) est
  `bd_effective_pool_job_budget_bytes()`, c'est-à-dire `bd_pool_job_budget_bytes`
  — marge heuristique `/2` (co-résidence ancien+nouveau niveau ; pas de
  seconde `/2` comme en SOLO puisqu'un job POOL n'appelle jamais
  `bd_transition_parallel`) — une variable **distincte** de l'admission dans
  un slot du pool, gardée par une estimation RAM **EXACTE** :
  `bd_estimate_reload_bytes` relit uniquement l'en-tête sérialisé (12 octets)
  d'un fragment — sans le charger — pour calculer précisément ce que
  `bd_level_load_file` allouerait à sa reprise, comparé au plafond
  `bd_pool_budget_bytes` (une somme visée sur tous les slots actifs). Si le
  coordinateur ne peut admettre ne serait-ce qu'un seul fragment en attente,
  il échoue bruyamment (`exit(1)`) plutôt que de rester bloqué.

**`bd_pool_budget_bytes` (admission POOL) et les seuils de scission par job
(`bd_solo_budget_bytes`, `bd_pool_job_budget_bytes`) sont des variables
distinctes**, bien que toutes dérivées du même `--dp-max-ram-mo` en
production. Il fallait les découpler : le hook test-only
`border_ring_dp_set_disk_mode_min_bytes_for_tests` pousse délibérément
`bd_solo_budget_bytes` ET `bd_pool_job_budget_bytes` à une valeur quasi
nulle pour forcer des scissions sur des fixtures minuscules — réutiliser
`bd_pool_budget_bytes` (l'admission) à cette fin aurait rendu même un
fragment de ~200 octets inadmissible en test, empêchant tout job POOL de
démarrer. En production, `bd_pool_budget_bytes` porte la valeur brute
donnée par `--dp-max-ram-mo`, alors que les deux seuils de scission par job
en portent chacun une fraction (`/4` et `/(nb_workers × 2)` respectivement)
— un détail d'implémentation qui compte pour quiconque relit le code, ou
règle le flag en production (cf. la note plus haut).

En cas d'échec d'un job du pool (code de sortie non nul, fichier résultat
illisible, ou `fork()` lui-même en échec en cours d'admission d'un lot), le
coordinateur tue et récupère (`waitpid`) tous les AUTRES jobs du pool
actuellement en cours (`bd_abort_active_jobs`) avant de sortir en échec —
jamais d'enfant orphelin laissé derrière, jamais un total partiel rapporté
comme définitif.

Chaque fragment n'est donc écrit qu'UNE fois (à sa création) et relu qu'UNE
fois (à sa reprise, par le mode qui le dépile), contre une
réécriture/relecture de la totalité à CHAQUE position sous l'ancien
mécanisme. Voir
[docs/conception/border_mass.md](conception/border_mass.md)
pour le raisonnement complet (comptabilité RAM détaillée, ordonnancement,
gestion des échecs, alternatives écartées).

**Garde contre une scission dégénérée** : un niveau à 0 ou 1 entrée réelle
n'est jamais scindé (`cur.used > 1`), même si sa taille nominale dépasse le
seuil — la capacité plancher d'une table de hachage (~200 octets, `struct
bd_level`) dépasse n'importe quel seuil assez bas indépendamment du contenu
réel ; sans cette garde, un fragment vide rechargé se re-scinderait
indéfiniment (observé pendant le développement : boucle sans fin sur un
seuil de test à 1 octet — pas qu'un artefact de test, rien n'empêcherait la
même situation en production avec un budget mal choisi).

Verrouillé par des tests dédiés dans `test_border_ring_dp.c`
(`border_ring_count_dp_matches_brute_force_when_sharded_to_disk` et son
pendant `..._on_real_pieces16_sharded_to_disk`, gated `#if ETERN_PARTS == 16`)
qui abaissent `bd_solo_budget_bytes`/`bd_pool_job_budget_bytes`/
`bd_shard_target_bytes` (hooks test-only
`border_ring_dp_set_disk_mode_min_bytes_for_tests` — qui pousse les DEUX
seuils de scission par job à la fois, cf. plus haut —
`_set_shard_target_bytes_for_tests`, même schéma que
`border_ring_dp_set_fork_min_states_for_tests`) au point où CHAQUE position
déclenche une nouvelle scission — y compris pour les tranches reprises
depuis la pile — exerçant plusieurs niveaux d'empilement en cascade, jamais
atteints par les autres tests (aucun de leurs fixtures n'approche le budget
par défaut).

**Pause mi-transition, au-delà de la scission de fin de position ci-dessus**
— incident de production du 2026-09-10 : `--dp-max-ram-mo 30000 --forks 10`
a tué un job du pool par OOM (`oom-kill:constraint=CONSTRAINT_NONE`, un vrai
manque de RAM globale, pas une limite de cgroup) alors que son budget nominal
par job (`bd_pool_job_budget_bytes = budget / (nb_workers × 2)`, ~1,5 Gio)
aurait dû l'empêcher — RSS réel observé 3 à 8,5 Gio, x2 à x6 le budget. Cause
racine : le contrôle de taille (`bd_run_fragment_job`, ci-dessus) ne
s'exécutait QU'UNE FOIS PAR POSITION, après que le niveau suivant `next`
ait été bâti EN ENTIER (`bd_transition_range` sur `0..cur.capacity` en un
seul appel) — un niveau au facteur de branchement élevé pouvait donc
dépasser le budget de plusieurs fois avant que le moindre contrôle n'ait
lieu, le pic mémoire réel ayant déjà eu lieu au moment où le code
« découvrait » qu'il fallait scinder.

Le chemin séquentiel (celui utilisé par TOUT job POOL, `allow_parallel=0`,
et par un job SOLO tant que `cur.used < bd_fork_min_states`) traite
désormais `cur` par tronçons de `bd_transition_chunk_slots` emplacements
(65536 par défaut, ajustable pour les tests via
`border_ring_dp_set_transition_chunk_slots_for_tests`), avec un contrôle de
`bd_level_bytes(&next)` après chaque tronçon **ayant réellement traité au
moins une entrée occupée** (jamais après un tronçon entièrement vide — cf.
le piège de livelock ci-dessous). Si `next` dépasse le budget avant que tout
`cur` n'ait été consommé, le job s'arrête : le reliquat de `cur` (les
entrées pas encore traitées, sérialisées par `bd_level_write_slice_file` —
une variante de `bd_level_write_file` bornée à une plage d'emplacements) et
`next` tel qu'accumulé jusque-là (`bd_level_write_file`, format inchangé)
sont écrits séparément et renvoyés comme un successeur **unique** (jamais de
fan-out en K fragments comme la scission de fin de position — `struct
bd_job_result`/`bd_pending_slice` portent pour cela deux nouveaux champs,
`leftover_path`/`next_partial_path`, vides sauf dans ce cas précis) à
reprendre plus tard, exactement à cette position — `bd_run_fragment_job`
recharge alors `next` (au lieu de le rebâtir de zéro) et continue de le
nourrir depuis là où il s'était arrêté.

**Piège corrigé avant que ce mécanisme ne parte en production** (repéré sous
test, `bd_transition_chunk_slots=1` + budget quasi nul, avant tout run réel)
: contrôler la taille après CHAQUE tronçon, y compris un tronçon vide,
bouclait indéfiniment. Un reliquat rechargé a une capacité RECALCULÉE à
partir de son SEUL compte d'entrées (`bd_level_load_file` — jamais de
l'étendue qu'il représentait avant sa pause), donc la ou les entrées qu'il
contient peuvent retomber À LA MÊME position de hachage qu'avant (même clé,
même capacité, fonction de hachage déterministe) sans jamais tomber dans le
tout premier tronçon parcouru — recontrôler après un tronçon vide (rien de
nouveau dans `next`, `chunk_start` reparti de 0 à chaque reprise) répétait
alors indéfiniment le même diagnostic « il reste du travail, le budget est
dépassé » sans jamais progresser jusqu'à l'entrée réelle : un vrai livelock,
pas qu'une inefficacité. Corrigé en ne (re)contrôlant qu'après un tronçon
ayant fait progresser `occupied_seen` (le compte d'entrées occupées de `cur`
réellement traitées) — ce qui garantit que `cur.used` du reliquat persisté
décroît STRICTEMENT à chaque pause, donc une terminaison bornée par le
nombre fini d'entrées d'origine.

Verrouillé par `border_ring_count_dp_matches_brute_force_when_mid_transition_pauses`
(même fixture à fourche que les tests de scission ci-dessus,
`bd_transition_chunk_slots` abaissé à 1) et par
`border_ring_dp_get_mid_transition_pauses_for_tests()` (même schéma que
`_get_pool_jobs_forked_for_tests` : incrémenté par `bd_run_fragment_job`
lui-même, invisible d'un process parent si la pause survient dans un job
POOL forké — le test reste donc en mode SOLO, `nb_workers` assez grand pour
qu'une pause, qui ne produit jamais qu'UN SEUL successeur, ne fasse jamais
basculer le coordinateur en mode POOL).

`border_ring_count_dp_matches_brute_force_when_pool_mode_engages` va plus
loin : un total final identique ne suffit pas à distinguer « le mode POOL a
réellement forké des jobs concurrents » de « le mode SOLO a tout traité
séquentiellement et produit, par coïncidence, le même total » — c'est
précisément ce qui s'est produit une fois en pratique (une variable de seuil
renommée pendant un refactor avait rendu le hook `_set_disk_mode_min_bytes_for_tests`
sans effet, si bien que ce test passait via SOLO sans jamais engager POOL).
Le test compte donc, en plus du total, un compteur test-only
(`border_ring_dp_reset_pool_jobs_forked_for_tests`/
`_get_pool_jobs_forked_for_tests`) incrémenté par le PARENT juste après
chaque `fork()` réussi dans `bd_fork_pool_job`, et vérifie qu'il est bien
`> 0` après l'appel — la seule façon de faire échouer ce test si POOL
dégradait de nouveau silencieusement en SOLO.

**Échec réel observé sur la machine visée, corrigé** : le mode disque a
d'abord échoué à la position 21/59 (17 fragments à 13,92 Go à la position 20)
avec des messages « fragment brut tronqué » côté compactage, alors qu'AUCUN
worker d'éclatement n'avait signalé d'échec. Cause racine : les `fwrite()`
des fragments bruts (et le `fclose()` qui les clôt) n'étaient jamais
vérifiés — un `ENOSPC` (disque plein) y échoue silencieusement, laissant un
fichier tronqué que seul le compactage suivant découvre, bien après le
worker fautif qui, lui, se termine avec un code de succès. `/tmp` peut
saturer même sur une machine par ailleurs bien dotée (48 Go de RAM) : sa
taille est indépendante de la RAM (petite partition ou tmpfs plafonné), et un
niveau en mode disque y dépose ses fragments BRUTS (non dédupliqués, donc
plus gros que leur forme finale compactée) — ici, l'ancien niveau (13,92 Go)
restait sur disque tout le temps de l'éclatement du suivant, faute d'être
supprimé avant la fin de toute la transition. Trois corrections :

- Toute écriture de fragment passe désormais par `bd_write_or_die`/
  `bd_close_or_die` (`border_ring_dp.c`) — échoue bruyamment (avec un indice
  « disque plein ? ») au lieu de laisser un fichier tronqué se propager en
  silence jusqu'au compactage suivant.
- Un fragment est supprimé dès qu'il est chargé en mémoire, pas seulement une
  fois son traitement fini — principe conservé par la pile LIFO qui a
  remplacé ce mécanisme depuis (`bd_run_opening` : `unlink()` juste après
  `bd_level_load_file`, aussi bien à la création d'une tranche qu'à sa
  reprise) : un fragment lu une fois n'est plus jamais utile après coup, donc
  plus la peine de le garder sur disque plus longtemps que nécessaire.
- `border_ring_dp_set_spill_dir` (`border_mass --spill-dir DIR` combiné à
  `--dp`) permet de rediriger fragments et fichiers temporaires vers un
  disque plus grand que `/tmp` — même logique que `--stock-spill-dir` pour le
  stock principal (`core/stock_spill.c`).

**`--save-rings FILE --max-rings N` (uniquement avec `--dp`) reconstruit les
anneaux réels** et les écrit dans `FILE` au format `.back` — le sous-projet 2
mentionné plus haut, désormais implémenté. Sur le jeu réel (256 pièces),
Xavier a mesuré (comptage `--dp` avec un seul coin fixé) `n_single_opening = 8`,
soit un total réel de `8 × 4 = 32` anneaux — minuscule, malgré le DFS brut qui
ne termine pas (voir plus haut). `border_ring_count_dp` ne conserve pourtant
rien d'exploitable une fois le total calculé (chaque niveau est jeté dès le
suivant construit) — reconstruire exige donc un vrai mécanisme dédié
(`border_ring_reconstruct_dp`, `tests/tools/border_ring_dp.c`) :

1. **Passe avant persistée** : la même DP que `border_ring_count_dp`, mais
   chaque niveau produit est en plus écrit sur disque
   (`bd_run_opening(..., persist_dir)`) au lieu d'être jeté — un fichier par
   job/fragment contributeur à chaque position (fusionnés à la LECTURE, pas
   à l'écriture, cf. `bd_persist_load_merged`).
2. **Tables de complétion** (`bd_build_and_persist_completions`) : un balayage
   BOTTOM-UP des niveaux persistés, de la dernière position vers la première —
   pour CHAQUE état réellement atteint par la passe avant à une position,
   calcule le nombre de façons de compléter l'anneau jusqu'à la fermeture à
   partir de cet état (`bd_completion_step`). ATTENTION : une tentative
   initiale utilisant une DP « miroir » indépendante (classes couleur
   d'entrée/sortie échangées, repartant d'un état générique) s'est révélée
   incorrecte — la complétion d'un état DOIT être dérivée des états
   RÉELLEMENT atteints par la passe avant à cette position, jamais d'un
   espace d'états recalculé séparément.
3. **DFS guidé sur les CLASSES** (`bd_reconstruct_class_dfs`) : à chaque
   position, une classe candidate n'est essayée que si son état résultant a
   une complétion non nulle dans la table de la position suivante — chaque
   branche explorée mène donc forcément à une fermeture valide. Bien moins
   coûteux qu'un DFS sur les pièces réelles : l'alphabet de branchement est
   ~15-18 classes, pas des dizaines de candidats par case.
4. **Expansion en pièces réelles** (`bd_expand_class_sequence`/`bd_expand_dfs`) :
   chaque suite de classes complète trouvée est développée en toutes ses
   assignations de pièces réelles possibles (une par combinaison de pièces
   disponibles quand une classe utilisée a plusieurs pièces encore libres) —
   réutilise directement la mécanique de lookup de `bw_dfs`
   (`tests/tools/border_walk.c`), aucune logique de rotation dupliquée.

Le total délivré DOIT correspondre exactement à `border_ring_count_dp` — échec
bruyant (`exit(1)`) sinon, sauf si `--max-rings` a délibérément coupé la
délivrance avant (dans ce cas un total inférieur est attendu, pas une erreur).
`--max-rings` est un plafond de SÉCURITÉ obligatoire (aucune valeur par défaut
choisie à la place de l'utilisateur, même principe que `--dp-max-ram-mo`), pas
une estimation de la masse réelle. Chaque anneau reconstruit est un
`possibility_packet` complet (bordure remplie, intérieur à `-2`) écrit tel
quel (`checked` forcé à 0) — même format headerless que `gen_root.c:76-86` et
`docs/utilisation.md` (§ format `.back`).

**Coût réel important** : la passe avant persistée écrit TOUS les niveaux
intermédiaires sur disque (jusqu'à `BORDER_RING_LEN - 1` d'entre eux), chacun
pouvant peser autant que son équivalent en mode `--dp` normal (dizaines de Go
observés) — `--spill-dir` doit pointer vers un disque avec assez d'espace
libre. Coût temps : une reconstruction complète PAR pièce-coin d'ouverture
réelle (typiquement ×4 sur le jeu réel — pas de raccourci par rotation ici,
contrairement à `bd_count_openings` : reconstruire une rotation géométrique du
paquet serait un risque de bug pour un gain minime).

Le cœur pur (`border_walk.c`) est compilé avec les autres modules et couvert
par `test_border_walk.c`, comme `root_from_board.c` pour `gen_root`. Il
réutilise sans modification `prepare_map_part`/`map_bucket_packed`/
`what_search_in_grid_to_key` : le côté intérieur d'une pièce de bord n'est
jamais posé donc toujours traité comme joker par ces fonctions existantes,
exactement le comportement voulu.

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
document de conception. Depuis [docs/conception/mrv_moteur_unique.md](../conception/mrv_moteur_unique.md)
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

## Voir aussi

- [tests/README.md](../tests/README.md) — organisation des suites, conventions, ajout d'un test.
- [Compilation](compilation.md) — options de build et drapeaux de configuration.
- `tests/bench/bench_search.sh` — banc de mesure du débit de recherche (voir ci-dessus).

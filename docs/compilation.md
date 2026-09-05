# Compilation

Ce document détaille les options de build du Makefile, les prérequis et les
configurations alternatives du puzzle.

## Prérequis

`gcc` (ou clang), `make` et pthreads — disponibles en standard sur macOS et Linux.
Le build par défaut ne dépend d'**aucune bibliothèque externe**.

## Cibles et options principales

```sh
make                          # Build de production (ANSI, sans dépendance) → ./eternityII
make DEBUG=1                  # Build debug (symboles -g, conservation des .o)
make NCURSES=1                # Build avec interface ncurses (liée à -lncurses)
make CUDA=1                   # Build avec pruner GPU CUDA (option `--gpu` du mode `pruner`)
make WERROR=1                 # Tout warning devient une erreur (mode CI)
make ASAN=1                   # Build instrumenté AddressSanitizer
make EXECUTABLE=monBinaire    # Nom de sortie personnalisé
make clean                    # Supprime les binaires et objets
```

- **`NCURSES=1`** est facultatif et nécessite ncurses (présent par défaut sur macOS
  et la plupart des distributions Linux) ; il remplace `src/ui/logger.c` par
  `src/ui/logger_ncurses.c` (voir [Console interactive](console.md#interface-ncurses-optionnelle-make-ncurses1)).
  Sans lui, le programme compile et tourne avec une interface texte ANSI pure.
- **`CUDA=1`** active l'option `--gpu` du mode `pruner` (Linux/NVIDIA uniquement) ; prérequis,
  variables (`NVCC`, `CUDA_PATH`, `NVCC_ARCH`, `NVCCFLAGS`), mode de vérification
  croisée `VERIFY=1` et notes Jetson sont détaillés dans
  [Pruner GPU (CUDA)](pruner_gpu_cuda.md#pré-requis). Sans `CUDA=1`, le binaire est
  strictement identique au build classique (aucun `.cu` compilé, runtime CUDA non lié).
- Sur macOS (Darwin), le Makefile détecte la plateforme et lierait OpenCL via
  `-framework OpenCL` au lieu de `-lOpenCL` (le support OpenCL est actuellement
  commenté dans l'édition de liens).

### `-mpopcnt` : ajouté automatiquement, et pourquoi c'est indispensable

Le build de production est `-Ofast` **sans `-march`**, donc sur la ligne de base
`x86-64` — antérieure à SSE4.2. Sans rien de plus, `__builtin_popcountll` ne
peut pas devenir une instruction : **gcc le compile en `call __popcountdi2`**,
le helper logiciel de libgcc (21 instructions, plus `call`/`ret` et
l'indirection PLT). Le balayage MRV en fait **218 par nœud**, soit ~54 % de ses
instructions. Mesuré : **×2,10** en nœuds/s avec le drapeau, à arbre exploré
identique (cf. [autosearch_step.md §1.3 quater](autosearch_step.md#13-quater-mrv_choose_cell--la-case-la-plus-contrainte-pas-la-suivante-du-parcours)).

Le makefile et `CMakeLists.txt` **sondent** donc le compilateur
(`POPCNT_FLAG` / `check_c_compiler_flag`) et ajoutent `-mpopcnt` s'il
l'accepte. C'est une sonde et non un test de plate-forme, parce que le drapeau
n'existe que sur x86 : la compilation croisée ARM (`make test-docker-arm`, avec
`CC=aarch64-linux-gnu-gcc`) le refuserait, et aarch64 n'en a pas besoin — le
builtin y est déjà rendu en ligne (`cnt`/`addv`).

```sh
make                          # -mpopcnt ajouté si le compilateur l'accepte
make CC=aarch64-linux-gnu-gcc # sonde négative : drapeau omis, build ARM intact
```

Deux conséquences à connaître :

- **Le binaire produit exige POPCNT**, c'est-à-dire un x86 postérieur à Nehalem
  (2008) ou Barcelona (2007). Pour viser plus ancien, forcer `POPCNT_FLAG=` :
  le programme reste correct, simplement plus lent.
- **Sans le drapeau, il n'y a jamais d'appel de bibliothèque pour autant.**
  `etii_popcount64` ([src/core/part.h](../src/core/part.h)) ne prend
  `__builtin_popcountll` que sous `__POPCNT__` ou `__aarch64__`, et retombe
  sinon sur un SWAR **en ligne** — ce que clang fait spontanément, et qui vaut
  déjà 21 % de temps en moins face à l'appel libgcc. C'est ce qui explique
  qu'un même code source ait longtemps tourné plus vite sur un Mac (clang) que
  sur un Linux (gcc) à fréquence de cœur comparable. Le repli est compilé sur
  toutes les cibles et testé sur toutes les cibles
  (`popcount64_swar_matches_naive_oracle`, `tests/core/test_part.c`), pour ne
  jamais devenir un chemin de code non exécuté.

## Configuration du puzzle

[src/core/core_static_variables.h](../src/core/core_static_variables.h) contrôle la taille du
puzzle et l'algorithme :

```c
#define ETERN_PARTS 256   // 256 pièces → plateau 16×16 (data/pieces.csv)
// ou
#define ETERN_PARTS 16    // 16 pièces  → plateau 4×4   (data/pieces16.csv)
```

Ces `#define` sont gardés par `#ifndef`, donc surchargeables sans toucher la source :

```sh
make CPPFLAGS="-DETERN_PARTS=16"        # plateau 4×4 (c'est ainsi que la CI le compile)
make CPPFLAGS="-DFORWARD_CHECK_K=0"     # forward-checking retiré
make NCURSES=1 CPPFLAGS="-DOUTPUT_PAD_LINES=10000"  # profondeur d'historique du pad ncurses (défaut 3000)
```

Changer `ETERN_PARTS` nécessite un rebuild complet (`make clean` d'abord).

## Drapeaux de debug

Des traces conditionnelles sont définies (et commentées) dans
[src/core/core_static_variables.h](../src/core/core_static_variables.h) — à décommenter avant
compilation, ou à passer via `CPPFLAGS="-D…"` :

```c
#define DEBUG_IN_MONO_PROCESS   // force le mono-processus (pas de fork) — indispensable au débogueur
#define DEBUG_SOCKET            // traces connexion/déconnexion TCP
#define DEBUG_SIGNAL            // traces des handlers de signaux
#define DEBUG_LOCAL_SOCKET      // traces des sockets Unix locaux
#define DEBUG_THREAD            // traces de création de threads
#define DEBUG_COMMANDS          // traces du parsing de commandes
#define DEBUG_CHECK_POSSIBILITY // valide les paquets de possibilités
#define DEBUG_RM_NO_NEXT        // traces de l'élagage rmnonext
```

La CI compile un build avec **tous** ces drapeaux à la fois (et `WERROR=1`), pour que
ce code de trace normalement mort ne se dégrade pas en silence — voir
[Tests et CI](tests_et_ci.md#intégration-continue).

## Drapeaux de mesure

Distincts des drapeaux de debug ci-dessus : ils n'ajoutent pas de traces mais des
**compteurs**, et ne sont jamais compilés en production parce que leur coût
fausserait la mesure qu'ils servent à faire. Ils se passent uniquement via
`CPPFLAGS`, jamais en décommentant quoi que ce soit.

| Drapeau | Ce qu'il mesure | Coût mesuré |
|---|---|---|
| `ETII_STAT_CORNER_ZONES` | Nœuds du moteur MRV où une zone d'angle 3×3 est entièrement entourée alors qu'elle est encore incomplète, avec la répartition des trous, la profondeur, et le nombre de nœuds explorés sous ces nœuds. Répond à la seule condition de réouverture laissée par [§4.9 de l'étude d'élagage](conception/elagage_recherche.md) — voir son verdict. | **+2,1 % de temps CPU** (banc 20 M nœuds, A/B alterné) |

```sh
make WERROR=1 CPPFLAGS="-DETII_STAT_CORNER_ZONES"
env ETII_BENCH_NODES=200000000 ./eternityII test data/pieces.csv   # rapport en fin de run
```

Le rapport est émis par `corner_zone_report()` en fin de `run_mono_client`, donc
aussi bien en mode `test` qu'en mode `client` (côté fork de recherche). Hors
build instrumenté, cette fonction a un corps vide : l'appelant n'a aucun
`#ifdef`. Les masques de zone et le prédicat, eux, sont **toujours** compilés —
donc toujours couverts par les tests unitaires — seul l'appel par nœud est
conditionnel. `ETII_STAT_CORNER_ZONES` exige `ETERN_SIZE >= 6` (en deçà les
quatre zones se recouvrent) et le refuse à la compilation.

## Ajouter un fichier source

Un nouveau `.c` se range sous le bon `src/<domaine>/` (voir la
[structure des sources](architecture.md#structure-des-sources)), son objet
`build/<domaine>/<nom>.o` s'ajoute à la liste `OBJS` du Makefile (et à
`add_executable` dans `CMakeLists.txt`).

## Voir aussi

- [Pruner GPU (CUDA)](pruner_gpu_cuda.md) — build et exécution du pruner GPU (`pruner --gpu`).
- [Tests et CI](tests_et_ci.md) — cibles de test, couverture, rejeu de la CI en local.
- [Utilisation](utilisation.md) — lancer le binaire produit.

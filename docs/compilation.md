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
make CUDA=1                   # Build avec pruner GPU CUDA (mode `gpupruner`)
make WERROR=1                 # Tout warning devient une erreur (mode CI)
make ASAN=1                   # Build instrumenté AddressSanitizer
make EXECUTABLE=monBinaire    # Nom de sortie personnalisé
make clean                    # Supprime les binaires et objets
```

- **`NCURSES=1`** est facultatif et nécessite ncurses (présent par défaut sur macOS
  et la plupart des distributions Linux) ; il remplace `src/ui/logger.c` par
  `src/ui/logger_ncurses.c` (voir [Console interactive](console.md#interface-ncurses-optionnelle-make-ncurses1)).
  Sans lui, le programme compile et tourne avec une interface texte ANSI pure.
- **`CUDA=1`** active le mode `gpupruner` (Linux/NVIDIA uniquement) ; prérequis,
  variables (`NVCC`, `CUDA_PATH`, `NVCC_ARCH`, `NVCCFLAGS`), mode de vérification
  croisée `VERIFY=1` et notes Jetson sont détaillés dans
  [Pruner GPU (CUDA)](pruner_gpu_cuda.md#pré-requis). Sans `CUDA=1`, le binaire est
  strictement identique au build classique (aucun `.cu` compilé, runtime CUDA non lié).
- Sur macOS (Darwin), le Makefile détecte la plateforme et lierait OpenCL via
  `-framework OpenCL` au lieu de `-lOpenCL` (le support OpenCL est actuellement
  commenté dans l'édition de liens).

## Configuration du puzzle

[src/app/static_variables.h](../src/app/static_variables.h) contrôle la taille du
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
```

Changer `ETERN_PARTS` nécessite un rebuild complet (`make clean` d'abord).

## Drapeaux de debug

Des traces conditionnelles sont définies (et commentées) dans
[src/app/static_variables.h](../src/app/static_variables.h) — à décommenter avant
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

## Ajouter un fichier source

Un nouveau `.c` se range sous le bon `src/<domaine>/` (voir la
[structure des sources](architecture.md#structure-des-sources)), son objet
`build/<domaine>/<nom>.o` s'ajoute à la liste `OBJS` du Makefile (et à
`add_executable` dans `CMakeLists.txt`).

## Voir aussi

- [Pruner GPU (CUDA)](pruner_gpu_cuda.md) — build et exécution du mode `gpupruner`.
- [Tests et CI](tests_et_ci.md) — cibles de test, couverture, rejeu de la CI en local.
- [Utilisation](utilisation.md) — lancer le binaire produit.

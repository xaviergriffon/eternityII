# eternityII

Solveur distribué pour le puzzle [Eternity II](https://fr.wikipedia.org/wiki/Eternity_II).

Le puzzle consiste à placer 256 pièces carrées sur une grille 16×16 en faisant correspondre les motifs sur les bords adjacents. L'espace de recherche étant astronomique, le programme exploite une architecture client-serveur pour distribuer le travail sur plusieurs processus, voire plusieurs machines.

## Architecture

```
┌─────────────────────┐        TCP        ┌─────────────────────────────┐
│      Serveur        │◄──────────────────►│  Client (processus parent)  │
│   (distribue les    │                    │  ┌──────────┬──────────┐    │
│   possibilités)     │                    │  │ fork #1  │ fork #2  │... │
└─────────────────────┘                    │  └──────────┴──────────┘    │
                                           └─────────────────────────────┘
```

- Le **serveur** maintient une liste de positions de plateau (« possibilités ») à explorer et les distribue aux clients.
- Chaque **client** fork `N` processus enfants. Chaque enfant se connecte au serveur, récupère des possibilités, les explore, puis renvoie les nouvelles positions découvertes.
- Les processus enfants communiquent avec leur parent via des **sockets Unix UDP locaux** (`etii_fork.<pid>`) pour :
  - remonter les statistiques en temps réel (`shots/sec`, possibilités en stock, `max_result`) ;
  - **router leurs logs** (`log_info`, `log_error`, `log_event`, …) au parent — qui possède la seule console, ce qui évite tout entrelacement dans le terminal et permet le bon fonctionnement de l'interface ncurses (voir [src/net/ipc_protocol.h](src/net/ipc_protocol.h)).
- Un seul client peut aussi fonctionner en mode **autonome** (`test`) sans serveur, utile pour des tests rapides.

### Structure des sources

Le code est rangé sous `src/`, réparti en quatre domaines ; les `#include` sont explicites et qualifiés par domaine (`#include "core/part.h"`), résolus via `-Isrc`.

| Répertoire | Domaine |
|---|---|
| `src/core/` | Logique du puzzle, structures de données et moteur de recherche (`part`, `readdata`, `possibility`, `lifo`, `etii_search`, `datamanager`, …) |
| `src/net/`  | Protocole TCP, sockets et IPC parent↔enfant (`etii_protocol`, `tcpclient`, `tcpserver`, `local_socket`, `ipc_protocol`) |
| `src/ui/`   | Journalisation, console et commandes (`logger`, `logger_ncurses`, `console`, `command_lines`, `command_match`, `command_history`) |
| `src/app/`  | Point d'entrée, rôles client/serveur, état global et GPU (`main`, `etii_client`, `etii_server`, `static_variables`, `gpu_pruner`) |

Les données du puzzle sont dans `data/` (`pieces.csv`, `pieces16.csv`) et les objets de compilation dans `build/` (miroir de `src/`, ignoré par git).

## Compilation

```sh
make                          # Build de production (ANSI, sans dépendance) → ./eternityII
make DEBUG=1                  # Build debug (symboles -g, conservation des .o)
make NCURSES=1                # Build avec interface ncurses (liée à -lncurses)
make CUDA=1                   # Build avec pruner GPU CUDA (mode `gpupruner`)
make EXECUTABLE=monBinaire    # Nom de sortie personnalisé
make clean                    # Supprime les binaires et objets
```

Prérequis : `gcc`, `make`, pthreads (disponibles en standard sur macOS et Linux). L'option `NCURSES=1` est facultative et nécessite ncurses (présent par défaut sur macOS et la plupart des distributions Linux) ; sans elle, le programme compile et tourne avec une interface texte ANSI pure, sans dépendance externe.

### Compilation CUDA (pruner GPU, optionnel)

Le build `CUDA=1` active un **pruner GPU** (mode d'exécution `gpupruner`) : même plomberie réseau que `tcppruner`, mais le contrôle de chaque lot de possibilités est exécuté sur le GPU (`src/app/gpu_pruner.cu` / `src/app/gpu_pruner.h`, dont le kernel calque la fonction CPU `possibility_all_has_a_next`). La cible matérielle est une carte **NVIDIA** ; le code a été validé sur **Jetson Orin Nano**, dont la mémoire unifiée permet l'usage de `cudaMallocManaged` en zero-copy.

> **Plateforme** : Linux/NVIDIA uniquement. macOS (Darwin) n'est **pas** supporté pour ce mode (pas de `nvcc`, pas de runtime CUDA). Le build par défaut (`make`, sans `CUDA=1`) reste strictement identique au binaire classique : tout le code GPU est encadré par `#ifdef WITH_CUDA`, aucun `.cu` n'est compilé et le runtime CUDA n'est pas lié.

**Prérequis :**

- Un **GPU NVIDIA** compatible CUDA et ses pilotes.
- Le **toolkit CUDA** (`nvcc` + runtime `libcudart`). Installé par défaut sous `/usr/local/cuda` (surchargeable via `CUDA_PATH`).
- Une **architecture GPU** (`NVCC_ARCH`) correspondant à la carte (Jetson Orin Nano = `sm_87`, défaut).

**Commandes :**

```sh
make CUDA=1                              # Active gpupruner (ajoute -DWITH_CUDA,
                                         #   compile gpu_pruner.cu avec nvcc,
                                         #   lie -lcudart -lstdc++)
make CUDA=1 VERIFY=1                     # Build de vérification croisée : rejoue le
                                         #   contrôle CPU pour chaque lot et logue
                                         #   toute divergence GPU/CPU (parité stricte)
```

**Variables surchargeables :**

| Variable | Défaut | Description |
|---|---|---|
| `NVCC` | `nvcc` | Chemin du compilateur CUDA (doit être dans le `PATH`, sinon échec explicite) |
| `CUDA_PATH` | `/usr/local/cuda` | Racine du toolkit CUDA (pour `-L$(CUDA_PATH)/lib64`) |
| `NVCC_ARCH` | `sm_87` | Architecture GPU cible (Orin Nano = `sm_87`) |
| `NVCCFLAGS` | `-O3 -arch=$(NVCC_ARCH)` | Drapeaux passés à `nvcc` |

**Note Jetson** : sur Jetson Orin Nano, `nvcc` n'est pas dans le `PATH` par défaut. Indiquer son chemin explicitement, et exporter le runtime CUDA à l'exécution :

```sh
make CUDA=1 NVCC=/usr/local/cuda/bin/nvcc
# puis à l'exécution :
LD_LIBRARY_PATH=/usr/local/cuda/lib64 ./eternityII gpupruner localhost 4
```

Le mode `VERIFY=1` n'est utile que pour valider la parité GPU/CPU ; en production, laisser `VERIFY=0` (le code de vérification est alors entièrement exclu du binaire).

## Tests et couverture

Des tests unitaires couvrent les modules à logique pure (`src/core/lifo.c`, `src/core/part.c`, `src/core/readdata.c`), basés sur [greatest](https://github.com/silentbicycle/greatest) — un framework C *single-header* vendoré dans `tests/`, **sans dépendance externe** à installer.

```sh
make test            # compile et lance la suite (code de sortie non nul si échec)
make coverage        # idem + rapport de couverture par module (gcov)
make coverage-report # rapports gcovr : Cobertura XML + HTML + résumé Markdown
```

`make test` produit un binaire isolé (`tests/run_tests`) qui ne lie **que** les modules testés et leurs dépendances — `src/app/main.c` n'est jamais inclus. `make coverage` recompile en mode instrumenté et affiche le pourcentage de lignes couvertes :

```
===== Couverture de code (tout le code de production) =====
  src/core/lifo.c        Lines executed:48.29% of 234
  src/core/part.c        Lines executed:50.77% of 388
  src/core/readdata.c    Lines executed:18.75% of 192
```

Le détail ligne par ligne est dans `tests/coverage/<module>.c.gcov` (lignes jamais exécutées marquées `#####`). `make coverage-report` produit en plus, via [gcovr](https://gcovr.com), un rapport HTML navigable, un Cobertura XML (consommé par Codecov) et un résumé Markdown. Voir [tests/README.md](tests/README.md) pour ajouter un test ou générer ces rapports.

## Intégration continue

À chaque push et pull request, [GitHub Actions](.github/workflows/ci.yml) :

- compile le build de production (`make`), lance les tests unitaires (`make test`) et les rapports de couverture (`make coverage-report`, via gcovr) ;
- publie la couverture : envoi à Codecov (Cobertura), commentaire de couverture sur la PR + récapitulatif du run (Job Summary), et rapport HTML en artefact téléchargeable ;
- vérifie que la variante ncurses (`make NCURSES=1`) compile toujours.

Le mode CUDA n'est pas testé en CI (pas de `nvcc` sur les runners ; la validation se fait sur Jetson).

## Utilisation

### Mode serveur

Lance le serveur qui distribue les possibilités aux clients.

```sh
./eternityII tcpserver [nb_threads] [fichier_pieces.csv]
```

| Paramètre | Défaut | Description |
|---|---|---|
| `nb_threads` | 80 | Nombre de connexions clients simultanées |
| `fichier_pieces.csv` | `data/pieces.csv` | Fichier de définition des pièces |

Exemple :
```sh
./eternityII tcpserver 80
./eternityII tcpserver 80 data/pieces.csv
```

### Mode client

Se connecte à un serveur et lance `N` processus de recherche en parallèle.

```sh
./eternityII tcpclient [serveur] [nb_threads] [max_stock_par_thread] [fichier_pieces.csv]
```

| Paramètre | Défaut | Description |
|---|---|---|
| `serveur` | `localhost` | Adresse IP ou nom d'hôte du serveur |
| `nb_threads` | 1 | Nombre de processus de recherche à forker |
| `max_stock_par_thread` | 300 | Nombre max de possibilités stockées par thread avant d'en renvoyer au serveur |
| `fichier_pieces.csv` | `data/pieces.csv` | Fichier de définition des pièces |

Exemples :
```sh
./eternityII tcpclient localhost
./eternityII tcpclient 192.168.1.10 8
./eternityII tcpclient localhost 4 300 data/pieces.csv
```

### Mode pruner (élagage)

Un **pruner** réutilise la même plomberie réseau qu'un client, mais au lieu d'explorer il demande au serveur des possibilités *à vérifier* et élague celles qui n'ont aucune continuation possible. Deux variantes :

```sh
./eternityII tcppruner [serveur] [nb_threads] [fichier_pieces.csv] [taille_lot]   # élagage CPU
./eternityII gpupruner [serveur] [nb_threads] [fichier_pieces.csv] [taille_lot]   # élagage GPU (build CUDA=1)
```

| Paramètre | Défaut | Description |
|---|---|---|
| `serveur` | `localhost` | Adresse IP ou nom d'hôte du serveur |
| `nb_threads` | 1 | Nombre de processus de vérification à forker |
| `fichier_pieces.csv` | `data/pieces.csv` | Fichier de définition des pièces |
| `taille_lot` | 100 | Nombre de possibilités échangées par aller-retour TCP (borné à 65536) |

> Le mode `gpupruner` n'est disponible que si le binaire a été compilé avec `make CUDA=1` (voir [Compilation CUDA](#compilation-cuda-pruner-gpu-optionnel)). Sur Jetson, penser à `LD_LIBRARY_PATH=/usr/local/cuda/lib64`.

#### Échange par lots

Le contrôle d'une possibilité est très rapide : sans lot, l'aller-retour TCP (une requête `GET` puis un acquittement par possibilité) plafonne le débit réseau et affame le GPU. Le pruner échange donc avec le serveur **par lots** : il demande jusqu'à `taille_lot` possibilités en un seul aller-retour et acquitte de même le lot analysé.

La taille de lot **borne la mémoire** détenue par le pruner (il ne reçoit/acquitte jamais plus que ce lot) et dimensionne les tampons GPU (un lot = un lancement de kernel sur tous les SM). Elle se règle :

- **au démarrage** : 4ᵉ argument de `tcppruner` / `gpupruner` (`taille_lot`) ;
- **à l'exécution** : commande interactive `prunerBatch <n>` (propagée aux process enfants).

Exemples :
```sh
./eternityII tcppruner localhost 4 data/pieces.csv 500     # lots de 500 (CPU)
./eternityII gpupruner localhost 1 data/pieces.csv 4096    # lots de 4096 (GPU)
```

> ⚠️ **Compatibilité protocole** : cet échange par lots fait passer la `VERSION` du protocole de 5 à 6. Le handshake exige une égalité stricte des versions — **tous les nœuds (serveur, clients, pruners) doivent être recompilés ensemble** ; un binaire VERSION 6 ne dialogue pas avec un VERSION 5.

### Mode test (autonome)

Exécute la recherche localement sans serveur. Utile pour valider la configuration ou déboguer.

```sh
./eternityII test [fichier_pieces.csv]
```

## Commandes interactives

Une fois lancé, le programme écoute des commandes sur l'entrée standard. Taper `help` pour la liste complète. En cas de faute de frappe, le programme propose automatiquement la commande la plus proche (`vouliez-vous dire "sortd" ?`).

| Commande | Description |
|---|---|
| `help` | Affiche la liste des commandes |
| `backup` | Sauvegarde les files de possibilités dans `eternityII.back` et `eternityII-in_analyse.back` |
| `restore` | Restaure les files depuis les fichiers `.back` |
| `restoreOld` | Restaure depuis un fichier `.back` au format ancien |
| `import` | Importe des possibilités depuis les fichiers `.back` dans les files courantes |
| `exit` | Arrête proprement le programme (sauvegarde automatique) |
| `check` | Affiche le dernier état analysé |
| `sorta` | Trie les possibilités par ordre croissant (moins avancées en premier) |
| `sortd [n]` | Trie par ordre décroissant (plus avancées en premier) ; `n` pour une file spécifique |
| `sortdm` | Trie toutes les files en parallèle |
| `split` | Répartit les possibilités entre les 10 files |
| `regroup` | Regroupe toutes les files en une seule |
| `rmnonext` | Supprime les possibilités sans continuation possible (élagage) |
| `min` | Affiche le niveau minimum dans les files |
| `statistic` | Affiche des statistiques sur le contenu des files |
| `loadjson` | Importe une possibilité depuis une chaîne JSON (équivalent de `import` pour le format JSON) |
| `checkdatas` | Vérifie l'intégrité des possibilités |
| `checkduplicate` | Recherche les doublons dans les files |
| `checkfiles` | Vérifie l'intégrité de toutes les files |
| `checkfile N` | Vérifie la file numéro `N` |
| `checkdirections` | Vérifie la cohérence des directions de parcours |
| `print` | Affiche toutes les files au format JSON |
| `printfile N` | Affiche le contenu de la file numéro `N` |
| `printanalysed` | Affiche les possibilités en cours d'analyse |
| `limit N` | Limite la vitesse de recherche à `N` essais/seconde (0 = illimité) |
| `maxStockByThread N` | Ajuste le stock max par thread à la volée |
| `prunerBatch N` | Ajuste la taille de lot d'échange du pruner à la volée (borné à [1, 65536]) |

Les commandes marquées comme « propagées aux enfants » (`backup`, `restore`, `rmnonext`, `limit`, `maxStockByThread`, `prunerBatch`, `min`, `printanalysed`) sont automatiquement retransmises à tous les processus fils via socket Unix.

## Interface interactive

### Zone Events (en bas de l'écran)

Les évènements notables sont affichés dans une bande fixe en bas de la console, juste au-dessus du prompt :

```
┌──────────────────────────────────┐
│  Sortie des commandes, logs…     │
│  …                               │
│  commande : check                │
│  file:0 stock:0                  │
│  …                               │
├──────────────────────────────────┤
│  Events                          │  ← bande inversée, fixe
│  [21:29:30] new record: 65 …     │
│  [21:30:15] SOLUTION FOUND! …    │
│  …                               │
└──────────────────────────────────┘
│  commande : _                    │
```

Trois évènements sont actuellement câblés :

| Évènement | Source |
|---|---|
| `new record: N pieces placed` | Détecté à chaque tick du checker (10 s) quand `max_result` augmente |
| `request unfulfilled: all threads busy` | Côté serveur, quand un nouveau client se connecte mais aucun thread libre |
| `SOLUTION FOUND! (N pieces) - saved to ./solution_<pid>` | Émis depuis `checkIfResultFound` quand les 256 pièces sont placées |

Tout évènement est **horodaté et écrit dans `events.log`** (en plus de l'affichage dans la zone), ce qui te permet de garder une trace persistante hors session :

```sh
tail -f events.log
```

### Réaffichage en place de `check`

La commande `check` réécrit son rapport au même endroit au lieu de défiler, pour éviter l'effet « scroll continu » quand on la tape plusieurs fois. La région de défilement ANSI commence en haut de l'écran, donc les sorties longues (par ex. `statistic`, `print`) restent capturées par le **scrollback natif du terminal** (molette, Cmd+↑).

### Historique des commandes (flèches ↑ / ↓)

Les 100 dernières commandes saisies sont conservées en mémoire pour la session. Les touches ↑ et ↓ rappellent les commandes précédentes (comme dans bash) :

| Touche | Effet |
|---|---|
| ↑ | Rappelle la commande précédente (la première pression mémorise la saisie en cours pour pouvoir y revenir) |
| ↓ | Avance vers les commandes plus récentes ; revient à la saisie en cours en bas |
| Entrée | Exécute la commande et l'ajoute à l'historique (dédoublonnage si identique à la précédente) |
| Backspace | Efface le dernier caractère |

L'historique fonctionne dans les deux builds. En ANSI, le terminal est basculé en mode non-canonique (`tcsetattr`) le temps de la session pour permettre l'interception des séquences `\033[A` / `\033[B`. Le mode initial est restauré automatiquement à la sortie. Si stdin n'est pas un TTY (sortie redirigée), le programme retombe sur la lecture ligne-par-ligne classique.

### Interface ncurses (optionnelle, `make NCURSES=1`)

Le build `NCURSES=1` remplace l'affichage ANSI par une vraie interface ncurses à quatre zones :

```
┌────────────────────────────────────┐
│  output_pad (scrollable, 3000 l.)  │
│  …                                 │
├────────────────────────────────────┤
│  coups/s:… stock:… record:…        │  ← bandeau stats live (vidéo inverse)
├────────────────────────────────────┤
│  Events  [+47 sous la vue]         │
│  …                                 │
├────────────────────────────────────┤
│  commande : _                      │
└────────────────────────────────────┘
```

Le **bandeau de stats** (mis à jour en continu par le thread checker) affiche : `coups/s`, `stock`, `analyse`, `record` et `limite`. Il est rafraîchi via `log_status()` et ne perturbe pas le défilement du pad de sortie.

Touches de navigation dans l'historique :

| Touche | Effet |
|---|---|
| `PgUp` / `PgDn` | Remonte / descend d'une page |
| `Home` / `End` | Tout en haut / tout en bas |
| `Entrée` | Réactive le suivi automatique du bas |

Quand on n'est pas en bas, le titre de la zone Events affiche le nombre de lignes cachées (`[+N sous la vue — PgDn/End pour revenir]`).

Le build par défaut (sans `NCURSES=1`) reste 100 % fonctionnel et **sans aucune dépendance** sur ncurses.

## Format du fichier de pièces

```
ntiles: 256
<id> <top> <right> <bottom> <left>
...
```

- Chaque pièce est définie par son identifiant et les 4 couleurs de ses bords (entiers).
- La valeur `0` représente la bordure grise (bord du puzzle).
- Le fichier `data/pieces.csv` contient les 256 pièces officielles du puzzle 16×16.
- Le fichier `data/pieces16.csv` contient 16 pièces pour un puzzle 4×4 (tests rapides).

## Fichiers générés

### Sauvegardes (`.back`)

Le programme sérialise ses files de possibilités dans des fichiers binaires `.back` :

| Fichier | Contenu |
|---|---|
| `eternityII.back` | Files de possibilités en attente d'exploration (serveur) |
| `eternityII-in_analyse.back` | Possibilités actuellement distribuées aux clients |
| `eternityII.back_<pid>` | Sauvegarde propre à un processus client |
| `failed_exit_eternityII_<pid>.back` | Possibilités non vidées à l'arrêt anormal d'un client |

Ces fichiers permettent de reprendre une recherche interrompue avec la commande `restore`.

### Journal et solutions

| Fichier | Contenu |
|---|---|
| `events.log` | Journal des évènements horodatés (nouveaux records, solutions, etc.). Append-only. |
| `solution_<pid>` | Plateau sérialisé quand une solution complète est trouvée (déclenche aussi un évènement). |

## TODO

- Persister l'historique des commandes entre sessions (fichier `.eternityII_history`)
- Élargir la liste des évènements câblés (déconnexions client, erreurs de protocole, …)

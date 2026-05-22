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
- Les processus enfants communiquent avec leur parent via des **sockets Unix UDP locaux** (`etii_fork.<pid>`) pour remonter les statistiques en temps réel.
- Un seul client peut aussi fonctionner en mode **autonome** (`test`) sans serveur, utile pour des tests rapides.

## Compilation

```sh
make                          # Build de production → ./eternityII
make DEBUG=1                  # Build debug (symboles -g, conservation des .o)
make EXECUTABLE=monBinaire    # Nom de sortie personnalisé
make clean                    # Supprime les binaires et objets
```

Prérequis : `gcc`, `make`, pthreads (disponibles en standard sur macOS et Linux).

## Utilisation

### Mode serveur

Lance le serveur qui distribue les possibilités aux clients.

```sh
./eternityII tcpserver [nb_threads] [fichier_pieces.csv]
```

| Paramètre | Défaut | Description |
|---|---|---|
| `nb_threads` | 80 | Nombre de connexions clients simultanées |
| `fichier_pieces.csv` | `pieces.csv` | Fichier de définition des pièces |

Exemple :
```sh
./eternityII tcpserver 80
./eternityII tcpserver 80 pieces.csv
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
| `fichier_pieces.csv` | `pieces.csv` | Fichier de définition des pièces |

Exemples :
```sh
./eternityII tcpclient localhost
./eternityII tcpclient 192.168.1.10 8
./eternityII tcpclient localhost 4 300 pieces.csv
```

### Mode test (autonome)

Exécute la recherche localement sans serveur. Utile pour valider la configuration ou déboguer.

```sh
./eternityII test [fichier_pieces.csv]
```

## Commandes interactives

Une fois lancé, le programme écoute des commandes sur l'entrée standard. Taper `help` pour la liste complète.

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

Les commandes marquées comme « propagées aux enfants » (`backup`, `restore`, `rmnonext`, `limit`, `maxStockByThread`, `min`, `printanalysed`) sont automatiquement retransmises à tous les processus fils via socket Unix.

## Format du fichier de pièces

```
ntiles: 256
<id> <top> <right> <bottom> <left>
...
```

- Chaque pièce est définie par son identifiant et les 4 couleurs de ses bords (entiers).
- La valeur `0` représente la bordure grise (bord du puzzle).
- Le fichier `pieces.csv` contient les 256 pièces officielles du puzzle 16×16.
- Le fichier `pieces16.csv` contient 16 pièces pour un puzzle 4×4 (tests rapides).

## Fichiers de sauvegarde (`.back`)

Le programme sérialise ses files de possibilités dans des fichiers binaires `.back` :

| Fichier | Contenu |
|---|---|
| `eternityII.back` | Files de possibilités en attente d'exploration (serveur) |
| `eternityII-in_analyse.back` | Possibilités actuellement distribuées aux clients |
| `eternityII.back_<pid>` | Sauvegarde propre à un processus client |
| `failed_exit_eternityII_<pid>.back` | Possibilités non vidées à l'arrêt anormal d'un client |

Ces fichiers permettent de reprendre une recherche interrompue avec la commande `restore`.

## TODO

- Affichage des statistiques en temps réel sans faire défiler la console
- Historique des commandes avec les flèches (↑/↓)

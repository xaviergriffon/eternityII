# Architecture

Ce document décrit l'architecture d'ensemble d'eternityII : le modèle de
processus/threads, la communication interne parent↔enfants et l'organisation des
sources. Le protocole TCP entre serveur et clients est détaillé dans
[Échanges client / serveur](echanges_client_serveur.md).

## Vue d'ensemble

```
┌─────────────────────┐        TCP         ┌─────────────────────────────┐
│      Serveur        │◄──────────────────►│  Client (processus parent)  │
│   (distribue les    │                    │  ┌──────────┬──────────┐    │
│   possibilités)     │                    │  │ fork #1  │ fork #2  │... │
└─────────────────────┘                    │  └──────────┴──────────┘    │
                                           └─────────────────────────────┘
```

- Le **serveur** (`server`) maintient le stock global de positions de plateau
  (« possibilités », `struct possibility_packet`) à explorer, réparties en files
  protégées par mutex ([src/core/datamanager.c](../src/core/datamanager.c)), et les
  distribue aux clients. Il peut sauvegarder/restaurer ce stock dans des fichiers
  `.back` (voir [Utilisation — fichiers générés](utilisation.md#fichiers-générés)).
- Chaque **client** (`client`, `pruner`, `pruner --gpu`) forke `N` processus
  enfants. Chaque enfant se connecte au serveur, récupère des possibilités, les
  explore (ou les vérifie, en mode pruner), puis renvoie les nouvelles positions
  découvertes.
- Un client peut aussi fonctionner en mode **autonome** (`test`) sans serveur, utile
  pour des tests rapides.

## Modèle de processus et threads

- **Mode client** : le processus parent forke `NB_THREADS` processus enfants ;
  chacun exécute `run_mono_client()` indépendamment et ouvre sa propre connexion TCP
  vers le serveur.
- **Thread de statistiques** (`run_checker`) : thread détaché dans chaque processus,
  qui mesure le débit de recherche (coups/seconde) et remonte les compteurs au parent
  chaque seconde.
- **Thread console** (`run_console`) : thread détaché qui lit les commandes sur
  l'entrée standard et les dispatche à `do_command_line()` (voir
  [Console interactive](console.md)).
- **Thread d'alimentation** (`feed_thread_aposs`) : côté client, remplit les files
  locales des threads de recherche depuis le serveur (voir
  [autosearch_step](autosearch_step.md)).
- **Canal de contrôle** (`run_control_channel`) : thread détaché du **parent** client
  uniquement, qui ouvre une seconde connexion TCP permettant au serveur de piloter le
  client à distance (voir
  [Canal de contrôle](echanges_client_serveur.md#canal-de-contrôle-v9)).

## Communication parent ↔ enfants (IPC)

Les processus enfants communiquent avec leur parent via des **sockets Unix UDP
locaux** (`etii_main.<pid>` et `etii_fork.<pid>`,
[src/net/local_socket.c](../src/net/local_socket.c), messages définis dans
[src/net/ipc_protocol.h](../src/net/ipc_protocol.h)) pour :

- **remonter les statistiques** en temps réel (`shots/sec`, possibilités en stock,
  `max_result`) — et, uniquement quand un fork bat son propre record, la
  **représentation complète** du plateau à ce moment (pas seulement le compte,
  cf. [Canal de contrôle](echanges_client_serveur.md#canal-de-contrôle-v9) et
  [src/core/best_board.h](../src/core/best_board.h)) ;
- **router leurs logs** (`log_info`, `log_error`, `log_event`, …) au parent — qui
  possède la seule console, ce qui évite tout entrelacement dans le terminal et
  permet le bon fonctionnement de l'interface ncurses ;
- **propager les commandes console** du parent vers les enfants (commandes marquées
  « propagées aux enfants », voir [Console interactive](console.md)).

## Structure des sources

Le code est rangé sous `src/`, réparti en quatre domaines ; les `#include` sont
explicites et qualifiés par domaine (`#include "core/part.h"`), résolus via `-Isrc`.

| Répertoire | Domaine |
|---|---|
| `src/core/` | Logique du puzzle, structures de données et moteur de recherche (`part`, `readdata`, `possibility`, `best_board`, `lifo`, `etii_search`, `datamanager`, …) |
| `src/net/`  | Protocole TCP, sockets et IPC parent↔enfant (`etii_protocol`, `control_protocol`, `tcpclient`, `tcpserver`, `local_socket`, `ipc_protocol`, `http_codec`, `http_server`) |
| `src/ui/`   | Journalisation, console et commandes (`logger`, `logger_ncurses`, `console`, `command_lines`, `command_match`, `command_history`) |
| `src/app/`  | Point d'entrée, rôles client/serveur, signaux, état global et GPU (`main`, `etii_client`, `etii_server`, `etii_control`, `control_registry`, `app_runtime`, `static_variables`, `gpu_pruner`) |

Les données du puzzle sont dans `data/` (`pieces.csv`, `pieces16.csv` — voir
[le format du fichier de pièces](utilisation.md#format-du-fichier-de-pièces)), les
objets de compilation dans `build/` (miroir de `src/`, ignoré par git) et les tests
dans `tests/` (miroir de `src/` lui aussi, voir [Tests et CI](tests_et_ci.md)).

## Voir aussi

- [Échanges client / serveur](echanges_client_serveur.md) — protocole TCP de travail et canal de contrôle.
- [autosearch_step](autosearch_step.md) — flux de recherche et gestion mémoire d'un thread.
- [API HTTP REST admin](api_http_rest.md) — télémétrie et pilotage HTTP du serveur.
- [Pruner GPU (CUDA)](pruner_gpu_cuda.md) — vérification des possibilités sur GPU.

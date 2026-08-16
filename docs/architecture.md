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
  `.back` (voir [Utilisation — fichiers générés](utilisation.md#fichiers-générés)) et,
  optionnellement, plafonner sa croissance en RAM (`--stock-max-ram`, voir
  [Utilisation — plafond RAM du stock](utilisation.md#plafond-ram-du-stock---stock-max-ram)).
- Chaque **client** (`client`, `pruner`, `pruner --gpu`) forke `N` processus
  enfants. Chaque enfant se connecte au serveur, récupère des possibilités, les
  explore (ou les vérifie, en mode pruner), puis renvoie les nouvelles positions
  découvertes.
- Un client peut aussi fonctionner en mode **autonome** (`test`) sans serveur, utile
  pour des tests rapides.

## Modèle de processus et threads

- **Mode client** : le processus parent construit la **map de lookup** (une seule
  fois, voir la section suivante) puis forke `NB_THREADS` processus enfants ;
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

## Map de lookup partagée entre les processus de recherche

La table de candidats (`map_big_array` : `flat` 5,06 Mo + index compact `packed`
1,27 Mo + arène 0,11 Mo sur le puzzle 256, voir
[autosearch_step §1.3 bis](autosearch_step.md#13-bis-map_bucket_packed--lindex-compact-du-forward-checking))
est **construite une seule fois par le processus parent**, avant sa boucle de
`fork()` ([src/app/main.c](../src/app/main.c), `handle_client`), puis publiée via
`set_inherited_search_parts` ([src/app/etii_client.c](../src/app/etii_client.c)).

Elle n'est **jamais réécrite** après sa construction : les processus enfants en
héritent donc par **copy-on-write** et se partagent physiquement **une seule
copie**, sans `mmap` partagé explicite ni changement de format. Auparavant chaque
enfant appelait `prepare_map_part()` *après* le fork et en fabriquait une copie
privée.

**Propriété — la seule règle à respecter** (`search_parts_t`, `acquire_search_parts`) :

| Contexte | Origine de la map | Qui libère |
|---|---|---|
| Fork de recherche (`client`, `pruner`) | héritée du parent | **personne côté enfant** — la map survit au fork et est partagée par ses frères |
| Processus parent client | construite par lui | lui, après `wait_child()` |
| Mode `test` (`run_auto`, aucun fork) | construite par `run_mono_client` | `run_mono_client`, en fin de recherche |

`acquire_search_parts()` renvoie `0` (héritée) ou `1` (construite ici) : cet
unique booléen porte toute la décision de libération, il n'y a pas deux chemins
de code à garder synchronisés. `run_mono_client()` reste donc correct qu'il soit
appelé depuis un fork ou depuis le mode `test`.

**Gain mesuré — Linux** (conteneur `tests/docker`, puzzle 256, `Pss` de
`/proc/<pid>/smaps_rollup`, la seule métrique qui distingue une page partagée
d'une page privée) :

| Workers | Mesure | Avant | Après | Delta |
|---|---|---|---|---|
| 4  | `Pss` cumulé des forks | 28,3 Mo | 6,7 Mo | **−76 %** |
| 4  | `Private_Dirty` par fork | 7004 Ko | **176 Ko** | **−97 %** |
| 16 | `Pss` cumulé (forks + parent) | 111,2 Mo | 11,1 Mo | **−90 %** (≈ −100 Mo) |

Le `Rss` (140 Mo à 16 workers), lui, ne bouge pas d'un pouce : il compte les
pages partagées dans **chaque** process. Seuls `Pss` et `Private_Dirty` montrent
le partage — après le changement, chaque fork n'a plus que 176 Ko de pages
sales à lui, et le `Shared_Dirty` de 6864 Ko qui apparaît est exactement la map
(6600 Ko) devenue commune.

**Gain mesuré — macOS** (`footprint`/`vmmap`, empreinte physique par process) :

| Workers | Empreinte cumulée des forks — avant | après | Delta |
|---|---|---|---|
| 1  | 7,3 Mo | 1,6 Mo | **−78 %** |
| 4  | 29,6 Mo | 6,7 Mo | **−77 %** |
| 16 | 118,7 Mo | 25,6 Mo | **−78 %** (≈ −93 Mo) |

`vmmap` sur un fork montre directement le basculement : les trois régions
`MALLOC_LARGE` de la map (5184 Ko + 1296 Ko + 120 Ko) passent de `SM=PRV` avec
6600 Ko *dirty* à `SM=COW` avec ~900 Ko résidents partagés. Ces 900 Ko sont les
pages que ce fork a effectivement fait entrer dans sa table de pages, c'est-à-dire
son **jeu de travail** dans cette map ; avant, les 6600 Ko étaient *tous* sales
simplement parce que le fork venait de les écrire en construisant sa copie.

**Débit : aucun gain — plutôt −1 %.** Contrairement à l'intuition « moins de
pression cache, donc plus vite », le débit cumulé (coups/s, client réel avec
serveur, i9-9880H : 8 cœurs physiques, L3 de 16 Mo) ne s'améliore pas. Campagne
de 4 paires à 16 workers en **alternant l'ordre** des deux binaires (la machine
dérive de −14,6 % sur la durée de la campagne — throttling thermique — ce qui
pénaliserait systématiquement celui qui passe en second) :

| Ordre de la paire | Delta partagé / master |
|---|---|
| master puis partagé (2 paires) | −2,90 % |
| partagé puis master (2 paires) | +0,79 % |
| **moyenne géométrique (dérive annulée)** | **−1,07 %** |

En **mono-process** (mode `test`, `ETII_BENCH_NODES=20000000`, 4 paires alternées)
l'écart est de **−0,16 %**, c'est-à-dire rien : le changement y est de toute façon
neutre (une seule map, construite puis libérée comme avant). Les nœuds atteints
et le taux d'élagage forward-check (45,7098 – 45,7105 % dans les deux groupes)
sont identiques : **l'exploration n'a pas bougé d'un nœud**, comme attendu d'un
changement qui ne touche que la propriété de la mémoire.

Le −1,07 % à 16 workers est au niveau du bruit de ce banc, mais le signe est
stable. L'explication la
plus probable : à 16 workers sur 8 cœurs physiques, le facteur limitant est la
contention de cœurs, et le jeu de travail réel dans la map (~900 Ko par fork,
cf. `vmmap` ci-dessus) tenait déjà en cache avant le partage — il n'y avait donc
pas de gain de latence à récupérer. **Le bénéfice de ce changement est
l'empreinte mémoire** (combien de workers tiennent sur une machine à mémoire
contrainte), pas la vitesse. Sur une machine où le L3 est réellement le facteur
limitant, la mesure serait à refaire avant de conclure.

Contraintes à ne pas casser en touchant cette zone :

- **Aucun thread du parent ne doit tourner pendant la boucle de `fork()`** (un
  enfant hériterait d'un verrou stdio détenu par un thread inexistant chez lui).
  La construction de la map se fait avant, alors que le parent est encore
  mono-thread ; elle ne déplace aucune création de thread. Un `fflush(NULL)` est
  fait juste avant la boucle pour que les enfants n'héritent pas d'un tampon
  stdio non vidé (qu'ils ré-émettraient chacun à leur sortie).
- Le **pruner GPU** initialise CUDA *après* le fork (les contextes CUDA ne
  s'héritent pas) ; il lit simplement la map héritée.
- Le **serveur**, le mode `test` et les commandes console `removeNoNext` /
  `expand` construisent chacun leur propre map : rien n'y change.

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

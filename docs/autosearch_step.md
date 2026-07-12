# autosearch\_step — Flux de recherche et gestion mémoire

## Vue d'ensemble

`autosearch` ([src/core/etii_search.c:889](../src/core/etii_search.c)) est un pthread lancé par `runThreadClient` ou `run_mono_client`. Il boucle indéfiniment en appelant `autosearch_step`, qui constitue **un cycle complet de travail** : attente d'un lot de possibilités → exploration par backtracking → délégation / renvoi → acquittement.

```
autosearch()
  └─ while (autosearch_step(client, idParts))
             │
             ├── attente du lot (works==0)
             ├── search_packet_backtracking() × N paquets
             ├── [REQUEST_STOP] requeue_unprocessed_packets()
             ├── [REQUEST_STOP] send_possibility_analysed()
             └── free_array_possibility_packet(client->aposs)
```

Le thread d'**alimentation** (`feed_thread_aposs` / `feed_one_thread`) tourne en parallèle et remplit `client->aposs` depuis le serveur. Les deux threads se synchronisent via `client->works_mutex`.

---

## 1. Flux des possibilités

### 1.1 Alimentation (thread d'alimentation → thread de recherche)

```
Serveur TCP
   │ INST_GET
   ▼
get_possibility()          (etii_protocol.c)
   │  reçoit N paquets possibility_packet
   ▼
feed_one_thread()          (etii_client.c)
   │  malloc(array_possibility_packet) + malloc(possibility_packet[N])
   │  memcpy des paquets reçus
   ▼
client->aposs = aposs      (sous works_mutex)
client->works = 1
```

`autosearch_step` **attend** que `client->works == 1` en boucle `usleep(MICRO_SLEEP = 100 µs)`.

### 1.2 Consommation (entrée dans `autosearch_step`)

Pour chaque paquet racine `client->aposs->possibilities[a]`, le thread appelle :

```c
search_packet_backtracking(client, &client->aposs->possibilities[a], idParts)
```

Le paquet est **copié sur la pile** (`memcpy(&board, root, …)`) ; l'original dans `aposs` n'est jamais modifié (nécessaire pour l'acquittement et le renvoi éventuel).

### 1.3 `get_parts_bigarray_with_key` : un pointeur, pas un calcul

```c
stack[top].search = get_parts_bigarray_with_key(map_part, &constraints[x][y]);
```

Cette fonction ne calcule rien et n'alloue rien. Elle retourne un **pointeur direct** dans la map 4D pré-construite au démarrage (`map_big_array`), indexée par la clé `(top, right, bottom, left)` de la case courante. Le tableau `search->parts[]` pointé est en lecture seule et partagé entre tous les niveaux de la pile.

`stack[top].search` est ainsi le **curseur vers la liste de candidats** de ce niveau — il ne change jamais tant qu'on est à ce niveau.

### 1.4 Exploration d'un niveau : un candidat à la fois

Une idée clé : **on ne génère pas tous les successeurs de la case courante**. On prend le premier candidat valide, on descend d'un niveau, et on reviendra aux candidats suivants seulement si le sous-arbre en dessous est épuisé.

```
À la case (x, y), search pointe vers [A, B, C, D] :

  1re visite  → essaie A, A est valide → place A, next_s = 1, descend
                  └─ explore tout le sous-arbre sous A ...
  retour ici  → annule A, reprend à next_s = 1
                → essaie B, valide → place B, next_s = 2, descend
                  └─ explore tout le sous-arbre sous B ...
  retour ici  → annule B, reprend à next_s = 2
                → essaie C, rejeté par forward-check → continue
                → essaie D, valide → place D, next_s = 4, descend
                  └─ explore tout le sous-arbre sous D ...
  retour ici  → annule D, next_s = 4 = search->size → niveau épuisé
                → top-- (remonte au niveau N-1)
```

`next_s` est le seul état persistant du niveau : il mémorise la progression dans `search->parts[]` entre chaque retour. Tous les candidats finissent par être essayés, mais en profondeur d'abord (*depth-first*), pas simultanément.

Les candidats rejetés par forward-check (`bt_forward_check` retourne 0) ne sont **pas comptés** dans `counters` — le placement et le retrait ont eu lieu mais aucun niveau supplémentaire n'a été ouvert. Le compteur `fc_pruned` les enregistre séparément.

### 1.5 Placement effectif et descente

Quand un candidat valide est trouvé dans la boucle interne :

```
board.grid[cx][cy] = idParts[id][rotation]   ← écriture in-place (une seule case)
BOARD_SET_FACE(&board, id-1, 1)               ← bit faceused à 1
bt_propagate_place(constraints, cx, cy, part) ← mise à jour des 4 voisins dans le cache
stack[top].next_s   = s + 1                  ← mémorise la reprise pour le backtrack
stack[top].placed_pos = id - 1               ← mémorise quoi effacer au retour
placed = 1  →  break du while  →  counters++ →  retour au for(;;)
```

La prochaine itération du `for(;;)` incrémente `depth` d'un cran et appelle de nouveau `get_parts_bigarray_with_key` **pour la case suivante** (`dirx[depth+1]`, `diry[depth+1]`), avec des contraintes potentiellement différentes grâce à `bt_propagate_place`.

Aucune allocation heap : le plateau est unique et modifié en place.

### 1.6 Annulation / backtrack

Quand tous les candidats d'un niveau sont épuisés (ou tous rejetés par le forward-check) :

```
board.grid[cx][cy] = -2                              ← case libérée
BOARD_SET_FACE(&board, placed_pos, 0)                ← bit faceused à 0
bt_propagate_undo(constraints, cx, cy, all_face)     ← voisins remis à "any"
top--                                                ← remontée d'un niveau
```

Le `while (top >= 0)` continue alors sur le niveau précédent, en reprenant à `stack[top].next_s` (le candidat suivant de ce niveau). `search` n'est pas recalculé : c'est toujours le même pointeur dans la map, initialisé lors de la première visite du niveau.

Si `top < 0`, le sous-arbre du paquet racine est **entièrement exploré** : `search_packet_backtracking` retourne `0`.

### 1.5 Délégation du surplus au serveur

Toutes les **1 000 000 itérations**, si au moins 500 ms se sont écoulées depuis la dernière délégation :

```
bt_count_pending(board, stack, top)
   │  compte les frères non encore explorés dans la pile
   ▼
quota = bt_delegation_quota(pending, max_stock_by_thread, server_hunger)
   │  pending > max_stock_by_thread → max_stock_by_thread   (règle historique)
   │  sinon, si le serveur a faim   → min(faim, pending/2)  (délégation anticipée, v8)
   │  sinon                          → 0 (aucun envoi)
   ▼
si quota > 0 :
   bt_materialize_pending()     ← reconstruit quota paquets
   add_possibility(client, aposs)   TCP INST_ADD → serveur
   stack[i].next_s = new_next_s[i] ← marque les niveaux délégués
   server_hunger -= envoyés         (si délégation anticipée)
```

Les niveaux sont matérialisés **du plus profond vers la racine** (petites unités de travail en priorité, cohérent avec l'ordre LIFO historique). La mise à jour de `next_s` n'a lieu qu'après un envoi **réussi** : en cas d'échec, le travail reste local.

`server_hunger` est la **faim du serveur** publiée par le thread d'alimentation
(sonde `INST_NEED_WORK` toutes les ~2 s, protocole v8 — voir
[echanges_client_serveur.md](echanges_client_serveur.md)) : elle autorise une
délégation *sous* le seuil quand le stock serveur ne suffit plus à nourrir les autres
clients (famine du démarrage). Le thread ne cède jamais le chemin courant ni son
dernier frère (`pending < 2` → quota 0), et au plus la moitié de son stock implicite.

### 1.6 Solution trouvée

Quand `depth >= ETERN_PARTS` (toutes les pièces placées) :

```
record_solution(client, &board)
   ├── log_solution()          → ANSI console + fichier solution_<pid>_<seq>.csv
   └── send_solution(client)   → TCP INST_SOLUTION → serveur
       [--stop-on-solution] exit(EXIT_SUCCESS)
       [défaut]              goto backtrack  (continue pour d'autres solutions)
```

Des solutions peuvent aussi être détectées pendant la **matérialisation** des frères (`bt_materialize_pending` vérifie `pkt->alloc >= ETERN_PARTS`).

### 1.7 Renvoi du travail restant (arrêt)

Sur `REQUEST_STOP`, avant de quitter le cycle :

```
bt_flush_pending(client, &board, stack, top, …)
   ├── bt_materialize_pending() de TOUS les frères restants
   ├── + paquet représentant le chemin courant (prochaine case à explorer)
   └── add_possibility(client, aposs)    TCP INST_ADD → serveur

requeue_unprocessed_packets(client, a)
   └── paquets racines non encore traités [a .. aposs->size)
       add_possibility(client, aposs)    TCP INST_ADD → serveur

send_possibility_analysed(client)         TCP INST_POSSIBILITY_ANALYSED
   └── signale au serveur que le lot est terminé (retire du suivi "en analyse")
```

L'acquittement est **inconditionnel** : le thread d'alimentation est stoppé et n'acquittera plus.

---

## 2. Communication avec les files et le serveur

### 2.1 Canaux

| Direction | Instruction TCP | Fonction appelée | Moment |
|---|---|---|---|
| Serveur → client | `INST_GET` | `get_possibility()` | Thread d'alimentation, chaque cycle |
| Client → serveur | `INST_ADD` | `add_possibility()` | Délégation surplus + renvoi sur arrêt |
| Client → serveur | `INST_SOLUTION` | `send_solution()` | Solution complète trouvée |
| Client → serveur | `INST_POSSIBILITY_ANALYSED` | `send_possibility_analysed()` | Fin de chaque `autosearch_step` |

Le mutex `client->socket_mutex` sérialise tous les échanges réseau entre le thread d'alimentation et le thread de recherche (ils partagent le même `socket_id`).

### 2.2 Synchronisation inter-threads (alimentation ↔ recherche)

```
Thread alimentation                    Thread recherche
────────────────────                   ────────────────
get_possibility() → aposs              while works==0 → usleep(100µs)
pthread_mutex_lock(works_mutex)        ...
client->aposs = aposs                  ...
client->works = 1                      pthread_mutex_lock(works_mutex)
pthread_mutex_unlock(works_mutex)      reads client->aposs
                                       pthread_mutex_unlock(works_mutex)
                                       search_packet_backtracking()
                                       ...
                                       pthread_mutex_lock(works_mutex)
                                       client->works = 0
                                       pthread_mutex_unlock(works_mutex)
```

### 2.3 Statistiques (file locale implicite)

`bt_count_pending()` calcule le stock en attente dans la pile (frères non explorés). Ce nombre est publié dans `lastfilesize[client->compteur]` pour que le thread de contrôle puisse le reporter. Le vrai stock local **n'est pas une `File` heap** : il est entièrement implicite dans `stack[]`.

---

## 3. Allocations, libérations et écritures mémoire

### 3.1 Structures sur la pile (aucune allocation heap)

| Variable | Taille approximative | Durée de vie |
|---|---|---|
| `board` (`possibility_packet`) | ~540 o | durée de `search_packet_backtracking` |
| `constraints[ETERN_SIZE][ETERN_SIZE]` (`key_part`) | 16×16×4 = 1 024 o | durée de `search_packet_backtracking` |
| `stack[ETERN_PARTS]` (`bt_level`) | 256×(ptr+int+int16) ≈ 3 Ko | durée de `search_packet_backtracking` |
| `scratch` (`possibility_packet`) | ~540 o | durée de `bt_count_pending` / `bt_materialize_pending` |
| `idParts[ETERN_PARTS+1][PART_SIZES]` (`int16_t`) | 257×4×2 ≈ 2 Ko | durée d'`autosearch` (tout le thread) |

### 3.2 Allocations heap par cycle d'`autosearch_step`

#### `client->aposs` (créé par le thread d'alimentation)

```
malloc(sizeof(array_possibility_packet))          // ~16 o
malloc(sizeof(possibility_packet) * N)            // N × ~540 o
```

**Libération** à la fin de `autosearch_step` (toujours, arrêt ou non) :

```c
free_array_possibility_packet(client->aposs);
client->aposs = NULL;
```

#### Délégation : `bt_delegate_if_needed`

Pas de malloc/free par appel : le conteneur `array_possibility_packet` (8 octets)
vit sur la pile, et les paquets matérialisés sont écrits dans le **buffer
pré-alloué du thread** (`client->delegate_buf`), dimensionné au quota demandé et
réutilisé d'une délégation à l'autre (`bt_ensure_delegate_buf` ne `realloc` que si
la capacité doit grandir). Libéré une seule fois, à la sortie d'`autosearch`.

```
bt_ensure_delegate_buf(client, quota)   // realloc uniquement si capacité < quota
array_possibility_packet aposs = { .possibilities = client->delegate_buf }  // pile
// add_possibility() copie les paquets : le buffer reste réutilisable
```

#### Renvoi sur arrêt : `bt_flush_pending`

```
malloc(sizeof(array_possibility_packet))
malloc(sizeof(possibility_packet) * (pending + 1))
// ↓ après add_possibility()
free_array_possibility_packet(aposs)              // libération immédiate
```

#### Renvoi des paquets non traités : `requeue_unprocessed_packets`

```
malloc(sizeof(array_possibility_packet))
malloc(sizeof(possibility_packet) * (aposs->size - a))
// ↓ après add_possibility()
free_array_possibility_packet(aposs)              // libération immédiate
```

### 3.3 Écritures in-place sur le plateau (boucle chaude, zéro malloc)

| Opération | Écriture mémoire |
|---|---|
| Placer une pièce | `board.grid[cx][cy] = idParts[id][rotation]` |
| Marquer pièce utilisée | `set_face_used(board.b_faceused, id-1, 1)` → bit à 1 dans `uint16_t[]` |
| Propager les couleurs | `constraints[voisin].k? = p->couleur` (4 voisins max) |
| Retirer une pièce | `board.grid[cx][cy] = -2` |
| Libérer la pièce | `set_face_used(board.b_faceused, id-1, 0)` → bit à 0 |
| Annuler la propagation | `constraints[voisin].k? = all_face` |
| Mise à jour curseur | `board.alloc = d+1`, `board.x = dirx[d+1]`, `board.y = diry[d+1]` |

### 3.4 Bitmask `b_faceused`

Chaque pièce occupe **1 bit** dans un tableau de `FACES_USED_SIZE` `uint16_t` (16 pièces par mot) :

```c
// Marquer la pièce `pos` comme utilisée :
set_face_used(faceused, pos, 1)
  groupe = pos >> 4          // indice du mot uint16_t
  bit    = pos & 0xF         // position dans le mot
  faceused[groupe] |= (1 << bit)

// Vérifier :
is_face_used(faceused, pos)  → 0 ou 1  (inline, boucle chaude)
```

### 3.5 Cycle de vie complet d'un `possibility_packet`

```
Serveur
  │ TCP INST_GET
  ▼
array_possibility_packet *aposs              ← malloc (thread alimentation)
  │  aposs->possibilities[i] = paquet reçu  ← memcpy
  ▼
client->aposs = aposs                        ← transfert de propriété
  │
  ▼ (thread recherche)
board = copy of aposs->possibilities[a]      ← memcpy sur pile (stack)
  │  boucle de backtracking modifie board
  │  délégation → memcpy vers client->delegate_buf[]  ← buffer pré-alloué réutilisé
  │                → add_possibility() (copie ; le buffer reste au thread)
  ▼
free_array_possibility_packet(client->aposs) ← free (fin autosearch_step)
client->aposs = NULL
```

---

## 4. Résumé des étapes clés

```
autosearch_step()
│
├─ [ATTENTE] usleep(100µs) tant que works==0 ou aposs==NULL
│
├─ Pour chaque paquet racine aposs->possibilities[a] :
│   │
│   └─ search_packet_backtracking()
│       ├─ bt_init_constraints()       init cache couleurs (pile)
│       └─ BOUCLE PRINCIPALE :
│           ├─ [depth == ETERN_PARTS]  → record_solution() → TCP INST_SOLUTION
│           ├─ [REQUEST_PAUSE ou REQUEST_ADMIN_PAUSE] → usleep(10µs) continue
│           │     (pause de régulation de débit OU pause administrative — `pause`/
│           │      `resume`, locale ou via le canal de contrôle v9, cf.
│           │      docs/echanges_client_serveur.md)
│           ├─ [REQUEST_STOP]          → bt_flush_pending() + return 1
│           ├─ [toutes les 1M iter]    → bt_delegate_if_needed() → TCP INST_ADD
│           │     (seuil dépassé, OU faim serveur publiée par la sonde INST_NEED_WORK — v8)
│           │
│           ├─ AVANCER (haut du for) :
│           │   ├─ depth = start_depth + top + 1
│           │   ├─ top++
│           │   ├─ si grille[x][y] != -2 → case pré-remplie, counters++, continue
│           │   └─ sinon → stack[top].search = get_parts_bigarray_with_key(...)
│           │              (pointeur dans la map 4D — zéro calcul, zéro copie)
│           │
│           └─ PLACER / RECULER (while top >= 0) :
│               ├─ si stack[top].placed_pos >= 0 → annuler le placement courant
│               │     grid=-2, faceused=0, undo contraintes, placed_pos=-1
│               ├─ for s = next_s .. search->size :   ← reprend là où on était
│               │   ├─ id==0 ou pièce déjà utilisée → continue (pas compté)
│               │   ├─ placer la pièce (grid, faceused, propagate)
│               │   ├─ [FORWARD_CHECK_K > 0] bt_forward_check()
│               │   │   └─ branche morte → undo, fc_pruned++, continue (pas compté)
│               │   └─ next_s = s+1, placed_pos = id-1, placed=1 → break
│               ├─ placed ? → break du while → counters++ → retour haut du for
│               └─ niveau épuisé → top-- → continuer while au niveau supérieur
│
├─ [REQUEST_STOP]
│   ├─ requeue_unprocessed_packets()   TCP INST_ADD (paquets racines non traités)
│   └─ send_possibility_analysed()     TCP INST_POSSIBILITY_ANALYSED
│
└─ free_array_possibility_packet(client->aposs)
   client->works = 0
   lastfilesize[compteur] = 0
   [REQUEST_STOP] → return 0 (arrêt du thread)
   [sinon]        → return 1 (prochain cycle)
```

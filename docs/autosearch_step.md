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

### 1.3 bis `map_bucket_packed` : l'index compact du forward-checking

La map expose **deux représentations des mêmes données**, et le chemin chaud ne lit pas la même que les autres :

| Représentation | Contenu | Taille (puzzle 256) | Lue par |
|---|---|---|---|
| `flat` | `sizearray^4` × `struct array_part` (16 o : une taille + un pointeur) | **5,06 Mo** | `get_parts_bigarray*` — lookup de placement (§1.3) et tous les autres appelants |
| `packed` | `sizearray^4` × `uint32_t` : `{offset:16 \| size:16}`, offset relatif à `arena` | **1,27 Mo** | `map_bucket_packed` — uniquement `bt_forward_check` |

**Pourquoi.** Sur le puzzle 256, la map compte 331 776 compartiments dont **6 254 non vides (1,9 %)** ; les données utiles (`arena`, 14 401 pièces = 0,11 Mo) tiennent confortablement en L2. Autrement dit, **98 % des 5 Mo balayés par la boucle chaude sont du vide**. Or `bt_forward_check` est appelé une fois par candidat posé et fait jusqu'à 4 itérations (les voisines géométriques de la case posée, cf. §1.3 ter — 6 au moment de cette mesure, quand la fenêtre suivait encore le parcours), chacune un accès aléatoire dans ces 5 Mo — le plus souvent pour ne lire qu'un compteur de 4 octets. En ramenant le compartiment à un `uint32_t`, une ligne de cache rapporte **16 compartiments au lieu de 4**.

**Gain mesuré** (`tests/bench/bench_search.sh`, puzzle 256, i9-9880H, médianes) :

| Configuration | Avant | Après | Delta |
|---|---|---|---|
| 1 worker (20 M nœuds × 7) | 6 112 645 nœuds/s | 6 747 789 nœuds/s | **+10,4 %** |
| 8 workers concurrents | 39,7 M nœuds/s cumulés | 43,7 M nœuds/s | **+10,1 %** |
| 16 workers concurrents | 41,9 M nœuds/s cumulés | 53,8 M nœuds/s | **+28,6 %** |

L'écart se creuse avec le nombre de workers : dans cette mesure, chaque worker est un **processus indépendant** (`bench_search.sh` lance N fois le mode `test`) et a donc **sa propre copie** de la map — 16 × 1,27 Mo tient dans les 16 Mo de L3 de la machine là où 16 × 5,06 Mo ne tient pas.

> En **mode client**, ce n'est plus le cas : les processus de recherche sont des `fork()` d'un même parent, qui construit la map avant de forker ; ils s'en partagent physiquement **une seule copie** en copy-on-write (voir [Architecture — map de lookup partagée](architecture.md#map-de-lookup-partagée-entre-les-processus-de-recherche)). L'index compact reste utile pour autant : il réduit la taille du jeu de travail lu par le forward-check, partagé ou non.

**Invariant.** `packed` est **purement redondant** : `map_bucket_packed` renvoie exactement la même taille et la même liste de pièces que `get_parts_bigarray_with_key`, pour **toute** clé. C'est un changement de représentation, jamais de sémantique — les nœuds explorés et leur ordre sont inchangés (le taux d'élagage rapporté par le banc reste identique à la 4ᵉ décimale : 45,7099 %). Le test `packed_index_matches_flat_for_every_key` (`tests/core/test_part.c`) balaie **toutes** les clés d'une map réelle pour verrouiller cette équivalence, et `bt_forward_check_same_verdict_with_and_without_packed_index` (`tests/core/test_etii_search.c`) la vérifie à travers la fonction chaude elle-même, en neutralisant l'index pour comparer les deux verdicts.

**Absence d'index (`packed == NULL`) : un état normal, pas une erreur.** `map_bucket_packed` retombe alors sur `flat` et renvoie la même chose. Ce repli couvre deux cas :

- une `map_big_array` **bâtie à la main** (fixtures de tests), qui n'a ni arène ni index ;
- un **dépassement de capacité** : les offsets et les tailles tiennent sur 16 bits (puzzle 256 : arène de 14 401 pièces, plus gros compartiment de 784 — très loin des 65 535). Si un autre jeu de pièces dépassait cette borne, `build_packed_index` (`src/core/part.c`) **renonce à construire l'index** au lieu de tronquer silencieusement, journalise la raison, et la recherche continue via `flat`. La décision est isolée dans `map_packed_fits`, testée aux bornes exactes 65535/65536.

**Variante évaluée et écartée.** Un **bitmap d'occupation** (41 Ko, 1 bit par compartiment, entièrement résident en L1/L2) a été implémenté et mesuré par-dessus l'index compact, pour ne payer qu'une lecture minuscule sur le test « compartiment vide ». Résultat : **−4 %**, donc abandonné. Le forward-check réussit ~54 % du temps, si bien que la majorité des cases inspectées tombent sur un compartiment **non vide** — et paient alors *deux* accès aléatoires (bitmap puis `packed`) au lieu d'un seul.

### 1.3 ter `bt_forward_check` : les voisines de la pièce posée, pas une fenêtre de parcours

Après avoir placé une pièce en `(cx, cy)`, `bt_forward_check` inspecte ses **voisines géométriques encore vides** — au plus 4 (haut/droite/bas/gauche, cf. `bt_propagate_place`) — et non plus les `FORWARD_CHECK_K` prochaines cases du parcours `directions[]` (comportement antérieur, toujours en vigueur sur le chemin froid `forward_check_next_k`, cf. §1.3 bis pour son usage exclusif via `packed`).

**Pourquoi.** Seul un placement modifie la clé de ses voisines directes ; une case du parcours peut se trouver à des dizaines de cases de distance sans jamais être une voisine. Mesuré sur le puzzle 256 (analyse statique de `dirx[]`/`diry[]`, script reproductible dans `docs/conception/elagage_recherche.md`) : l'ancienne fenêtre `K=6` ne couvrait que 61 % des relations « case → voisine encore vide » du parcours, avec un retard de détection médian de 8 niveaux (max 152).

**Gain mesuré** (`tests/bench/bench_search.sh`, puzzle 256, A/B à ordre alterné pour neutraliser la dérive thermique de la machine, 20 M nœuds × 5 :

| Configuration | Nœuds/s | Taux d'élagage |
|---|---|---|
| Avant (fenêtre `K=6`) | 5 867 621 | 45,7099 % |
| Après (voisines) | 9 906 019 | 45,6471 % |
| Delta | **+68,8 %** | −0,06 point |

Le débit progresse fortement (moins de lookups par placement : au plus 4 voisines contre jusqu'à 6 cases de fenêtre, souvent moins avec l'arrêt anticipé sur la première case morte) alors que le taux d'élagage — la part des tentatives de placement rejetées — reste quasiment inchangé : la condition nécessaire reste tout aussi efficace en pratique, seule son assiette de calcul est devenue moins chère.

**Le test « reste-t-il un candidat ? » passe par le masque d'ids.** Pour chaque voisine vide inspectée, la question posée n'est pas *combien* de pièces restent candidates mais seulement *s'il en reste une*. Le parcours des entrées du compartiment y répondait en s'arrêtant au premier candidat libre — bon marché quand la réponse est « oui », mais **proportionnel à la taille du compartiment quand elle est « non »** : plusieurs centaines d'entrées, toutes visitées, et c'est précisément le cas qui compte puisque c'est celui qui élague. `bucket_id_mask` (le même index que celui bâti pour le comptage MRV, §1.3 quater) répond en quelques `AND` sur `map->id_mask_words` mots, avec sortie au premier mot non nul (`map_mask_any_free`, `src/core/part.h`) : **coût borné, indépendant de la taille du compartiment**.

L'équivalence est exacte et non approchée : `build_bucket_id_mask` n'inscrit que les ids `> 0` réellement présents, exactement le filtre `id != 0` du parcours. La distinction « identifiants distincts vs entrées » qui sépare les deux comptages dans `map_mask_free_count` est ici sans objet, les deux s'annulant au test `== 0`.

Le parcours reste en repli (map bâtie à la main, index absent, compartiment vide, masque plus large que le miroir) et reste le **seul** chemin quand `singleton_conflict_check` est levé : cette variante-là compte des ENTRÉES — deux rotations d'une même pièce libre valent 2, donc « pas un singleton » — ce qu'un masque d'identifiants ne sait pas reproduire. La convertir changerait son verdict, pas seulement son coût.

**Gain mesuré** (`tests/bench/bench_search.sh`, puzzle 256, 5 M nœuds, A/B **apparié à ordre alterné** sur 6 rondes × 7 répétitions par configuration — machine chargée ce jour-là, une seule ronde n'aurait pas tranché) :

| | Nœuds/s (médiane) | Taux d'élagage | `max_result` |
|---|---|---|---|
| Parcours des entrées | 992 756 | 38,7344 % | 190 |
| Masque d'ids | 1 008 393 | 38,7345 % | 190 |
| Delta apparié médian | **+1,66 %** | — | — |

Les 6 rondes sont positives (+1,29 % à +2,50 %, test des signes unilatéral p = 0,0156). Le verdict du forward-check étant inchangé, taux d'élagage et `max_result` sont des contrôles de non-régression stricts, et non de simples garde-fous. Le gain moyen est modeste ; ce que la conversion apporte en plus ne se lit pas dans la médiane : elle **borne le pire cas**, jusque-là proportionnel à la taille du plus gros compartiment.

**Invariant : nécessaire, pas complet.** Comme toute condition de forward-checking, `bt_forward_check` ne peut jamais produire de faux positif (une branche qu'il laisse passer peut être morte pour d'autres raisons, une branche qu'il élague l'est réellement), mais il n'a **aucune obligation d'exhaustivité** sur l'ensemble des cases qu'il pourrait inspecter — restreindre son périmètre aux voisines directes est donc un choix de performance, jamais un risque de correction. Verrouillé par `bt_forward_check_inspects_at_most_geometric_neighbors` (`tests/core/test_etii_search.c`), qui compte les cases réellement inspectées (`fc_cells_studied`) sur un coin (2 voisines) et une case intérieure (4).

**Statistique par position (`fc_pruned_at[]`).** Depuis ce changement, l'indice n'est plus une distance de parcours mais une **position dans la fenêtre inspectée** : 1..4 pour la boucle chaude (rang de la voisine dans l'énumération haut/droite/bas/gauche), 1..`FORWARD_CHECK_K` pour le chemin froid `forward_check_next_k`. Le tableau est dimensionné sur `FC_STAT_MAX_K` (8, indépendant de `FORWARD_CHECK_K`) pour rester sûr quel que soit le plus petit des deux domaines — voir le commentaire de `fc_pruned_at` dans `core_static_variables.h`.

**Seconde variante évaluée et écartée : le lookup de placement via `packed`.** Le septième et dernier accès à la map par nœud — celui du placement (`stack[top].search`) — est resté sur `flat`. Le convertir lui aussi à `map_bucket_packed` a été implémenté, mesuré, puis **reverté** (PR #161, annulée par `revert/placement-lookup-packed`). L'idée était bonne sur le papier : `flat` quittait entièrement le chemin chaud et le jeu de travail partagé de la boucle tombait de 6,44 Mo (`packed` + arène + `flat`) à 1,38 Mo, soit **−79 %**. Le gain mesuré est **nul** : sur i9-9880H en A/B apparié à ordre alterné, **1 worker −0,9 %** (IC 95 % [−3,8 %, +2,0 %]) et **16 forks −0,7 %** (IC [−3,5 %, +2,2 %]).

`perf stat` sur un Pentium G2020 (Ivy Bridge, 2 cœurs, 256 Ko de L2 par cœur, 3 Mo de L3) explique pourquoi, et **le chiffre décisif est absolu, pas relatif**. Sur un run de 4,64 s à 2,9 GHz, soit ≈ 13,4 milliards de cycles :

| Compteur | Valeur | Coût estimé | Part du run |
|---|---|---|---|
| `L1-dcache-load-misses` | 2,92 M | ≈ 35 M cycles (à ~12 cy) | 0,26 % |
| `cache-misses` (→ DRAM) | 265 k | ≈ 53 M cycles (à ~200 cy) | 0,40 % |

**Toute la hiérarchie mémoire pèse moins de 0,7 % du temps d'exécution.** Soit ≈ **0,2 défaut L1 par nœud, pour 7 lookups par nœud** : le sous-ensemble réellement chaud de la map est assez petit pour tenir en L1 (32 Ko). Réduire la *table* de 5,06 à 1,27 Mo ne peut donc rien rapporter, sur aucune hiérarchie de cache — gros L2 ou petit, beaucoup de cœurs ou peu. La version convertie mesure même légèrement moins bien (défauts LLC +49 %, défauts L1 +18 %, temps +3,3 % — runs uniques, non répétés) ; comme l'écart de cache ne représente que ~0,24 % du temps, cette régression n'est pas d'origine mémoire : le suspect est le `map_bucket` de 16 octets copié **par valeur** dans `stack[top]` à chaque nœud, là où l'ancien code écrivait un pointeur de 8 octets, plus 2 Ko de pile C automatique.

> **À lire comme une seule histoire avant de rouvrir ce chantier.** Le +28,6 % de l'index compact (tableau ci-dessus) a été mesuré sur 16 **processus indépendants**, chacun avec sa copie privée : la pression venait de la **duplication** (16 × 5,06 Mo contre 16 × 1,27 Mo), pas de la taille de la table. Le partage en copy-on-write l'a supprimée à la racine — une seule copie de 6,44 Mo pour toute la machine, déjà résidente en cache. La conversion du lookup de placement optimisait donc un jeu de travail qui n'était plus sous pression, et l'a mesuré. **La boucle de recherche est bornée par le calcul et les branchements, pas par la mémoire** : toute future optimisation de jeu de travail sur cette boucle doit être précédée d'un `perf stat` démontrant que la hiérarchie mémoire pèse une fraction non négligeable du temps. Ce n'est pas le cas aujourd'hui. Corollaire pour le dimensionnement matériel : **la taille du cache n'est pas un critère de choix pour ce programme** — maximiser le nombre de cœurs entiers, l'IPC par cœur et la qualité de la prédiction de branchement.

**Re-vérifié après le passage au MRV, et la conclusion tient.** Le doute était légitime : le balayage de frontière a multiplié par ~15 le nombre de lookups par nœud (7 → 109), et par 448 les défauts de dTLB L1 (0,16 → 71,4 par nœud — le balayage touche ~59 pages de 4 Kio distinctes par nœud, contre un dTLB L1 de 64 entrées). `perf stat` sur E5-2640 v4, 5 M nœuds, tranche pourtant sans ambiguïté :

| Poste | Cycles | Part du run |
|---|---|---|
| `cycle_activity.stalls_l1d_pending` | 61,1 M | **0,29 %** |
| `dtlb_load_misses.walk_duration` | 1,4 M | **0,007 %** |
| `l2_rqsts.all_demand_miss` × ~40 cy | 97,8 M | **0,46 %** (majorant) |

Les défauts de dTLB sont absorbés par le STLB (les page walks restent négligeables) et masqués par l'exécution dans le désordre ; le jeu de travail chaud lui-même plafonne à **57 Kio sur une fenêtre de 32 k nœuds**, donc il tient dans n'importe quel L2 et le L3 ne sert à rien. Surtout, **l'IPC MONTE** (2,46 → 2,59) : une boucle bornée par la mémoire s'effondrerait. Le ×12,9 sur les cycles par nœud est un ×13,6 sur les **instructions** par nœud, dont ~54 % partaient dans le helper `__popcountdi2` (cf. §1.3 quater, point 3). La règle reste donc entière : **avant toute optimisation de jeu de travail sur cette boucle, exiger un `perf stat` qui montre que la mémoire pèse une fraction non négligeable du temps.** Ce n'est toujours pas le cas — c'est le *nombre d'instructions* qu'il faut regarder.

### 1.3 quater `mrv_choose_cell` : la case la plus contrainte, pas la suivante du parcours

Depuis §4.7 de [docs/conception/elagage_recherche.md](conception/elagage_recherche.md), la recherche remplit les cases dans l'ordre **dynamique** : au lieu de suivre l'ordre figé `dirx[]/diry[]`, elle choisit à chaque nœud la **case vide la plus contrainte** (MRV, *minimum remaining values*) — celle qui offre le moins de pièces encore libres. C'est `search_packet_backtracking_mrv` (`src/core/etii_search.c`), **le SEUL moteur de backtracking depuis [docs/conception/mrv_moteur_unique.md](conception/mrv_moteur_unique.md) (PR3)**, pour la recherche réelle comme pour la preuve bornée du pruner (§4.6b). L'ancien moteur à ordre FIXE (`search_packet_backtracking_core`) et les deux drapeaux qui pouvaient le sélectionner (`mrv_enabled`/`ETII_MRV`, `pruner_dfs_mrv`/`ETII_PRUNER_DFS_MRV`) ont été supprimés une fois la mesure favorable établie dans les deux usages — un interrupteur laissé en place aurait été un chemin de code non testé (cf. §6 de mrv_moteur_unique.md).

**Ce que le passage à l'ordre dynamique a changé, mesuré** (`tests/bench/bench_search.sh`, puzzle 256, 2 M nœuds × 3 répétitions, avant la bascule) :

| Configuration | Nœuds/s (médiane) | `max_result` à budget de nœuds identique |
|---|---|---|
| Ordre fixe (ancien défaut, supprimé) | 6 102 866 | 74 |
| Ordre dynamique (MRV, seul moteur désormais) | 811 617 | **186** |
| Delta | **−86,7 %** | **+151 %** |

Le débit n'est pas la mesure de ce changement (cf. [tests_et_ci.md](tests_et_ci.md#max_result--le-débit-seul-ne-prouve-pas-un-vrai-gain)) : à temps mural égal l'ordre fixe explore ~10× plus de nœuds et **reste à 74** ; à 50 M nœuds, MRV atteint 188. **Attention à la portée de ce chiffre** : les 74 sont un plafond DU BANC (mono-processus, depuis la genèse, sans stock ni délégation), pas une propriété de l'ordre fixe — contre un vrai serveur, un client à ordre fixe atteint 186 (cf. §4.7 de [elagage_recherche.md](conception/elagage_recherche.md), correction). La comparaison reste valide à protocole identique ; la mesure qui tranche vraiment est le coût de RÉFUTATION ([banc dédié](tests_et_ci.md#banc-de-réfutation-make-bench-refutation)).

**Cinq choses rendent le choix abordable** (les quatre premières le rendent bon marché, la cinquième le rend meilleur), là où le prototype de mesure de la PR 9 tombait à 23 k nœuds/s (−99,7 %) :

1. **Balayage restreint à la frontière.** Une case dont les 4 côtés valent `all_face` n'est contrainte par rien (ni bord de plateau, ni voisine posée) : elle accepte toutes les pièces libres et ne peut donc jamais être le minimum tant qu'une case contrainte existe. La frontière compte **52,2 cases en moyenne (max 79)** contre 256 cases balayées par le prototype — mesuré sous MRV, départ genèse, 1,5 M nœuds. **Ce n'est pas le « 29 en moyenne (max 52) » de [§3.2](conception/elagage_recherche.md)**, et l'écart a deux causes distinctes, toutes deux mesurées : §3.2 compte les cases vides adjacentes à une case *posée*, quand le balayage retient aussi toute case de bord vide (`what_search_in_grid_to_key` y pose la clé 0, une vraie couleur, donc une contrainte permanente) ; et surtout §3.2 vient d'une analyse statique de l'ordre **fixe** `dirx[]`/`diry[]`, qui remplit de proche en proche, là où MRV saute d'un bout à l'autre et laisse une frontière bien plus déchiquetée. À définition de §3.2 mais sous MRV, la même grandeur vaut 45,2 en moyenne (max 54) : l'ordre de remplissage pèse plus lourd que la définition. La restriction reste largement payante — 52 cases sur 256 — mais le chiffre de §3.2 la surestimait d'environ 1,8×.
2. **Énumération de la frontière par masque de bits** (`bt_frontier`, `src/core/etii_search.c`). Le point 1 n'était d'abord qu'un TEST : il évitait le *lookup*, pas la *visite* — la boucle lisait quand même les 256 cases de la grille puis ~182 clés du cache pour n'en retenir ~29, si bien que **repérer la frontière coûtait plus cher que la compter**. Deux masques (`empty`, `constrained`) maintenus incrémentalement par `bt_frontier_place`/`_undo` — exactement comme le cache de contraintes et le miroir des pièces utilisées — donnent directement la liste : `empty & constrained` par mot de 64 bits, parcourue au `__builtin_ctzll`. La position d'une case est `x * ETERN_SIZE + y`, donc l'ordre croissant des bits **reproduit exactement** l'ordre `for x { for y }` de l'ancien balayage : même départage d'égalité, donc même arbre exploré. `empty & ~constrained` fournit au passage le repli `fallback`. Un troisième champ, `nconstr`, compte les côtés contraints de chaque case (bords de grille, constants, + voisines posées) : c'est un compteur et non un booléen, deux voisines pouvant contraindre la même case. Mesuré `tests/bench/bench_search.sh` (5 M nœuds × 5, puzzle 256) : **917 554 → 1 000 844 nœuds/s, +9,08 %**, à taux d'élagage (38,7344 %) et `max_result` (190) inchangés — le gain est bien du coût retiré, pas un arbre différent.
3. **Comptage par `popcount`.** `bucket_id_mask` (construit une fois avec la map, à côté de `packed` — `build_bucket_id_mask`, `src/core/part.c`) donne le masque des ids d'un compartiment ; le nombre de pièces encore libres est `popcount(masque & ~utilisées)`, **indépendant de la taille du compartiment**, là où le prototype parcourait toutes ses entrées (jusqu'à plusieurs centaines). Le masque des pièces utilisées du plateau est miroité en mots de 64 bits (`mrv_used_init`/`mrv_used_set`/`mrv_used_clear`), maintenu incrémentalement comme le cache de contraintes.

   **Le `popcount` doit être une instruction, jamais un appel — et ça ne va pas de soi.** Le balayage en fait **218 par nœud** (52,2 cases de frontière × 4 mots de masque). Or `__builtin_popcountll` n'est pas gratuit par décret : sur x86 sans `-mpopcnt`, **gcc le compile en `call __popcountdi2`** — le helper logiciel de libgcc, 21 instructions plus `call`/`ret` et l'indirection PLT — là où clang déplie la même émulation *en ligne*. Le projet compilant en `-Ofast` sans `-march`, la ligne de base x86-64 est antérieure à SSE4.2 et le drapeau manquait : le moteur MRV passait **~54 % de ses instructions** dans ce helper. Deux corrections, toutes deux nécessaires :
   - le makefile et `CMakeLists.txt` **sondent** le compilateur et ajoutent `-mpopcnt` s'il l'accepte (sonde plutôt que test d'OS : le drapeau n'existe que sur x86 et la compilation croisée ARM le refuserait) ;
   - `etii_popcount64` (`src/core/part.h`) ne prend `__builtin_popcountll` que là où il est rendu en ligne de façon certaine (`__POPCNT__`, ou `__aarch64__` où c'est `cnt`/`addv`) et retombe sinon sur un SWAR **en ligne** — de sorte qu'aucun build, quelle que soit la chaîne, ne repasse par un appel de bibliothèque. Le repli est compilé partout et testé partout (`popcount64_swar_matches_naive_oracle`, `tests/core/test_part.c`), précisément pour ne pas être un chemin de code jamais exécuté.

   Mesuré (`ETII_BENCH_NODES`, 2 M nœuds, 6 répétitions alternées, `max_result` et taux d'élagage identiques — c'est du coût retiré, pas un arbre différent) :

   | Génération du `popcount` | Médiane | Nœuds/s | vs. actuel |
   |---|---|---|---|
   | `call __popcountdi2` (gcc sans drapeau) | 2,121 s | 0,943 M | — |
   | SWAR en ligne (ce que produit clang) | 1,756 s | 1,139 M | ×1,21 |
   | `popcntq` (`-mpopcnt`) | **1,011 s** | **1,978 M** | **×2,10** |

   **Ce tableau est aussi le diagnostic d'un écart entre machines.** À source, plateau et arbre exploré identiques, un binaire gcc sans drapeau est 21 % plus lent qu'un binaire clang — un écart de *génération de code*, pas de processeur, invisible avant le MRV puisque la boucle chaude ne contenait alors aucun `popcount`. Constaté en production entre un i9-9880H/macOS/clang et un Xeon E5-2640 v4/Linux/gcc de fréquence de base voisine : `perf stat` sur ce dernier donne **10 896 instructions par nœud contre 804 avant le MRV (×13,6), à IPC en hausse (2,46 → 2,59)**. La hiérarchie mémoire, elle, est hors de cause — voir l'encadré de [§1.3 ter](#13-ter-bt_forward_check--les-voisines-de-la-pièce-posée-pas-une-fenêtre-de-parcours) et sa vérification post-MRV ci-dessous.
4. Le balayage reste **complet** (pas d'arrêt anticipé sur une case à un seul candidat) : toute case de frontière sans aucun candidat est détectée au passage, un test de mort plus large que le forward-check local — gratuit puisque le balayage a lieu de toute façon.
5. **Départage des égalités par les côtés contraints.** À score MRV égal — cas fréquent, le minimum valant 1 dans plus de la moitié des appels — la case dont le PLUS de côtés sont déjà contraints (`bt_frontier.nconstr` : bords de grille + voisines posées) l'emporte, au lieu de la première rencontrée. Le critère est gratuit à lire, `nconstr` étant maintenu de toute façon par la frontière. **Son sens a été mesuré, pas déduit** : l'heuristique de degré classique de la littérature CSP recommande l'inverse (`nconstr` minimal, soit le plus de voisines encore vides) et se révèle nettement perdante ici. Sur la population entière des racines fermables d'un stock de production (433 racines, 383 fermées par les deux) : **−6,29 % de nœuds, −5,01 % de temps, et 4 sous-arbres fermés de plus** à budget égal ; la variante classique coûte +4,4 %. Quand `nconstr` est lui aussi à égalité, l'ordre d'énumération tranche comme avant. Protocole et pièges : [elagage_recherche.md §4.12](conception/elagage_recherche.md).

6. **Cache du pointeur de masque par case** (`cell_mask`, `bt_mask_init`/`bt_mask_refresh`). Les points 1 à 3 avaient rendu chaque case de frontière bon marché, mais il en restait 54,5 par nœud et chacune payait encore un `map_bucket_id_mask` complet : recalcul de l'index 4D (3 `imul` + 3 `add`), lecture de `packed[idx]` — un accès **dispersé dans 1,3 Mio** — puis dérivation de l'adresse du masque. Or la clé d'une case ne change QUE lorsqu'une voisine est posée ou retirée : `bt_propagate_place`/`_undo` n'écrivent que les ≤4 voisines. Le pointeur est donc mémorisé par case et rafraîchi aux mêmes endroits, ce qui ramène le balayage de 54,5 résolutions par nœud à **~13**. Mesuré E5-2640 v4, 3 M nœuds × 7 répétitions alternées : **1,195 → 1,470 M nœuds/s, ×1,22**, `max_result` (189) et taux d'élagage inchangés. C'est le **quatrième** cache en lockstep avec le plateau, et `bt_mask_refresh` s'appelle À CÔTÉ de `bt_propagate_place`/`_undo` comme `bt_frontier_place`/`_undo` — un site de placement peut donc l'oublier. Deux garde-fous : la re-dérivation des builds `DEBUG_CHECK_POSSIBILITY`, et `bt_mask_cache_stays_in_lockstep_with_constraints` (`tests/core/test_etii_search.c`), qui porte un **contre-contrôle** — une pose sans rafraîchissement doit faire diverger le cache, sans quoi le test ne prouverait plus rien si `bt_mask_refresh` devenait un no-op.

**Ce qui a été mesuré et ÉCARTÉ sur cette boucle** (E5-2640 v4, même protocole, à arbre identique) — à ne pas refaire sans raison nouvelle :

| Piste | Résultat |
|---|---|
| `-march=x86-64-v3` (BMI1/BMI2/AVX2 : `andn`, `blsr`, `tzcnt`) | **×0,99** — nul, et restreindrait le parc à Haswell+ |
| `popcount` déroulé en dur pour `words == 4` | **×1,00** — gcc gère déjà la boucle à 4 tours |
| Épinglage NUMA (`numactl`, machine bi-socket) | **×1,00** — un cœur du node 1 lisant la mémoire du node 0 coûte 0,3 % |
| Grandes pages 2 Mio (`alloc_hugepage_zeroed`, `src/core/part.c`) | ×1,045 **seul**, mais **+1,5 % seulement** une fois le point 6 en place — retenu quand même, sans risque de correction |

L'échec de NUMA et des grandes pages a la même cause, déjà établie par l'encadré de §1.3 ter : le jeu de travail chaud ne quitte jamais L1/L2, la DRAM n'est jamais sur le chemin critique, et les 82,6 défauts de dTLB par nœud sont absorbés par le STLB puis recouverts par l'exécution dans le désordre. **Leur majorant théorique (×7 cycles = 29 % du run) surestime le vrai coût d'un facteur 7** : mesuré, c'est 4 %.

**Délégation : `alloc` est recompté, pas re-canonisé.** Depuis VERSION 13 ([docs/conception/mrv_moteur_unique.md](conception/mrv_moteur_unique.md), PR1), `alloc` n'est plus un curseur de position dans `directions[]` mais le **nombre de cases non vides de la grille** (`possibility_placed_count`) — `directions[]`/`dirx[]`/`diry[]` ne sont plus qu'un simple ordre d'énumération. La profondeur de pile n'ayant de toute façon aucun rapport avec ce compte en ordre dynamique, `bt_materialize_pending`/`bt_flush_pending` fixent `alloc` de chaque paquet matérialisé par un recomptage direct — le comptage est déjà exact par construction puisqu'une pièce vient d'être posée. Les anciennes fonctions `bt_canonicalize_packet`/`normalize_possibility_packet` (qui recalaient `alloc` sur la première case vide du parcours) ont disparu avec cette bascule : un paquet troué (cases posées dans le désordre par rapport à `directions[]`) est un état NORMAL du format depuis VERSION 13, plus une anomalie à corriger. Verrouillé par `search_backtracking_mrv_delegation_preserves_solution_count` (`tests/core/test_etii_search.c`) : exploration MRV du vrai puzzle 4×4 interrompue en cours de route, travail restant repris jusqu'à épuisement, nombre total de solutions rigoureusement égal à celui d'une exploration exhaustive ininterrompue.

**Ce que MRV n'apporte pas.** Le taux d'élagage du forward-check bouge à peine (44,2 % → 43,2 %) : la valeur de MRV n'est pas dans l'élagage, elle est dans la **forme** de l'arbre exploré.

**Seconde coordonnée : `min_candidats`.** Deux plateaux à autant de pièces posées (`alloc` identique) n'ont pas forcément la même difficulté : l'un peut avoir été atteint par une suite de choix presque forcés (peu de candidats à chaque case), l'autre par une suite de choix larges. `mrv_choose_cell` calcule déjà, gratuitement, le nombre de candidats de la case qu'il choisit à chaque nœud (`best_count`) — ce score est reporté tel quel dans `possibility_packet.min_candidats` : celui de la case qui a reçu la **dernière** pièce posée sur ce plateau. Aucun recalcul dédié : le score est stocké dans `bt_level.mrv_score` au moment où `mrv_choose_cell` l'a produit, et recopié dans `min_candidats` partout où `alloc` est fixé par observation (boucle principale, `bt_materialize_pending`, `bt_flush_pending`, `record_solution`). `POSSIBILITY_MIN_CANDIDATS_UNKNOWN` (-1, `core/possibility.h`) marque un score non mesuré : case non contrainte (repli `fallback` de `mrv_choose_cell`, jamais scoré), paquet genèse, expansion en ordre fixe (`search_possiblity_light`), placements forcés (`possibility_all_has_a_next_counted`), ou stock restauré depuis un fichier antérieur à l'introduction du champ (recomptage impossible : contrairement à `alloc`, `min_candidats` dépend de l'historique de recherche, pas seulement de la grille — `import`/`import_analysed`/`stock_spill_reload` l'écrasent donc inconditionnellement par la sentinelle plutôt que de faire confiance à un octet qui était du bourrage d'alignement avant son introduction). Exposé par `GET /api/v1/stock-distribution` (`avg_min_candidats` par niveau, moyenne des trois pools) et `GET /api/v1/best-board` (`min_candidats` du meilleur plateau), et par la commande console `statistic`. N'occupe aucun octet supplémentaire sur le fil : loge dans le bourrage d'alignement de `b_faceused` (12 des 25 octets de bourrage de `possibility_packet`), `sizeof` reste 576 octets, aucun bump de `VERSION`.

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

### 1.5bis Abandon d'une racine trop peu profonde (`shallow_root_abandon_depth`)

`max_stock_by_thread` seul peut ne **jamais** se déclencher sur une racine reçue
à faible profondeur : si le branchement MRV est fin (peu de candidats par case),
`pending` (le stock implicite compté par `bt_count_pending`) reste petit alors que
le sous-arbre total de la racine reste énorme — le thread peut y rester des heures
sans jamais délivrer `max_stock_by_thread` frères. `shallow_root_abandon_depth`
(défaut `0`, désactivé — CLI `--shallow-root-abandon-depth <n>`, console
`shallowRootAbandonDepth <n>`, clé `config`/`--config-file`
`shallow_root_abandon_depth`) ajoute un second critère, évalué **au même point de
contrôle périodique** que la délégation (donc à coût nul sur la boucle chaude) :

```
root_depth   = possibility_placed_count(root)   ← figé une fois, avant la boucle
placed_count = possibility_placed_count(board)  ← courant, réévalué à chaque contrôle

bt_should_abandon_shallow_root(root_depth, placed_count, shallow_root_abandon_depth)
   │  abandon_depth > 0 ET root_depth < abandon_depth ET placed_count >= abandon_depth
   ▼
si vrai :
   bt_abandon_shallow_root()
      ├── bt_flush_pending()              ← même mécanisme que l'arrêt propre (§1.7) :
      │                                      tous les frères de la pile + le chemin courant
      └── shallow_root_abandoned++        ← compteur, remonté par `statistic`
   return BT_CORE_EXHAUSTED               ← le fil se repositionne sur une nouvelle
                                             racine au prochain GET (batch de 1 en recherche)
```

Un seul déclenchement possible par racine : la fonction retourne aussitôt après
avoir tout rendu, donc rien ne peut re-déclencher pendant l'étude de la MÊME
racine. Opt-in, désactivé par défaut : à calibrer par le protocole de mesure en
paires alternées déjà employé dans ce dépôt (moyenne géométrique, métrique
cumulée sur fenêtre fixe — cf. [conception/elagage_recherche.md](conception/elagage_recherche.md)
§6) avant d'envisager un défaut actif. Deux heuristiques de profondeur/ordre très
proches ont déjà perdu à la mesure ici (départage MRV par nombre de côtés
contraints, §4.12 du même document ; sens de cession « moins profond d'abord »
de la branche `feat/dispatch-local-possibilites-forks`, non retenue) — la valeur
128 (moitié du puzzle) n'est donc qu'un point de départ documenté, pas un défaut
recommandé.

### 1.5ter Observabilité : profondeur minimale en attente (commande `min`)

`placed_count` (profondeur du chemin COURANT) ne fait que croître le long
d'une seule branche : un fil peut afficher `placed_count=200` tout en
détenant encore, quelque part dans sa pile de décisions, un frère non exploré
au niveau 1 — exactement le stock implicite que `shallow_root_abandon_depth`
cible. `placed_count` seul serait donc un majorant trompeur si on cherchait
« la profondeur la plus superficielle que ce fil détient encore ».
`bt_min_pending_depth(board, stack, top)` répond correctement à cette
question : même parcours racine → pile que `bt_count_pending`, mais en
maintenant aussi `scratch.grid[][]` (pas seulement les faces) pour pouvoir
recompter par `possibility_placed_count` dès le premier niveau PENDING
trouvé, en partant du moins profond — jamais dérivé de l'indice de niveau
(la pile n'a pas de rapport fiable avec `alloc`, cf. §1.5). Coût borné par
`top` (≤ `ETERN_PARTS`), évalué au même point de contrôle périodique que
`shallow_root_abandon_depth` — toujours à coût nul sur la boucle chaude.

```
lastroot[compteur]  = root_depth               ← figé une fois, à la réception de la racine
lastdepth[compteur] = bt_min_pending_depth(…)   ← réévalué à chaque point de contrôle périodique
```

Remontés au parent par IPC (`client_statistics.root_depth`/`min_pending_depth`,
comme `lastfilesize`/`possibilities_in_stock`), agrégés par fork (minimum,
sentinelle `-1` = idle ou rôle pruner ignorée) et affichés par la commande
console `min` côté client/pruner — tableau « Search depth », une ligne par
fork, colonnes **Racine** (profondeur de la possibilité reçue du serveur) et
**Min** (profondeur minimale encore en attente). Voir
[Utilisation](utilisation.md#option---shallow-root-abandon-depth-client-et-pruner)
et [Console interactive](console.md) pour l'usage. Côté SERVEUR, `min` garde
son sens historique (`search_min_datas`, minimum dans les files du
datamanager) : sans objet côté client, dont les files restent vides après
fork (chaque fork explore en interne, cf. AGENTS.md).

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
| Mise à jour du compteur | `board.alloc = (uint16_t)possibility_placed_count(&board)` — nombre de cases posées, recompté après chaque pose (`x`/`y` ne sont plus tenus à jour, voir §3.2/PR1 de [docs/conception/mrv_moteur_unique.md](conception/mrv_moteur_unique.md)) |

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
│               │   │   ├─ lookups via map_bucket_packed (index compact, §1.3 bis)
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

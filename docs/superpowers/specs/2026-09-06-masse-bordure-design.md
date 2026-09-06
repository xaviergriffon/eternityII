# Masse totale des bordures — walker dédié (phase 1 : comptage)

**Date** : 2026-09-06
**Statut** : approuvé pour implémentation (phase 1 seulement)
**Auteur** : Xavier Griffon, avec Claude Code

## Contexte

Idée de départ : calculer toutes les bordures valides (les 60 cases du pourtour
du plateau 16×16 — 4 coins + 56 pièces de bord) en ancrant la recherche sur un
seul coin, puis utiliser la symétrie de rotation du plateau pour en déduire les
3 autres placements de coin, plutôt que de relancer 4 recherches indépendantes.
L'objectif immédiat n'est pas de produire des racines de stock, mais d'obtenir
un chiffre : la masse totale d'arrangements de bordure valides, pour juger si
cette voie est exploitable avant d'aller plus loin.

Ce document couvre uniquement cette **phase 1 (comptage)**. L'intégration au
système normal (génération de vraies racines `.back` à partir des anneaux
trouvés, injection dans le stock) est un **sous-projet 2, hors scope**, à
spécifier séparément une fois le chiffre de phase 1 obtenu et jugé exploitable.

## Pourquoi la symétrie de rotation tient

`data/indices.csv` contient les 5 indices officiels du puzzle 256 pièces :

```
139 7 8 2 1
208 2 2 3 0
255 13 2 3 0
181 2 13 3 0
249 13 13 0 0
```

Format `id x y rotation mandatory` (`src/core/readdata.h`). Leurs coordonnées
(`x,y ∈ {2,7,8,13}`) ne touchent jamais une case de bord (`x ou y ∈
{0, ETERN_SIZE-1}`). L'anneau des 60 cases de bord n'est donc ancré par aucune
contrainte figée : le sous-problème de correspondance de couleurs sur l'anneau
est intégralement symétrique sous rotation à 90°/180°/270° (les 4 côtés du
plateau font chacun 14 pièces de bord + 1 coin, donc de longueur égale).

Conséquence : une recherche exhaustive ancrée en un seul coin trouve déjà
**toute** la population d'anneaux valides — un anneau, en tant qu'objet
cyclique, n'a pas de coin de départ privilégié ; l'ancrer ailleurs retrouverait
les mêmes anneaux, pas d'autres. Ce que la rotation apporte, c'est 3
**placements** supplémentaires sur la grille absolue par anneau trouvé (lequel
de ses 4 coins tombe en `(0,0)` physique vs `(15,0)` vs `(15,15)` vs `(0,15)`)
— donc potentiellement 4 racines candidates par anneau, mais cela ne concerne
que la phase 2. Seule la réflexion (miroir) ne s'applique pas : les pièces ne
se retournent pas, seulement 0/90/180/270°.

## Objectif (phase 1)

Un outil qui :
1. Énumère par recherche exhaustive tous les anneaux de bordure valides,
   ancrés en `(0,0)`.
2. Rapporte `N` (nombre d'anneaux trouvés) et la masse totale `4×N`.
3. Vérifie explicitement, avant de lancer la recherche, qu'aucun indice
   officiel ne tombe sur une case de bord — précondition dont dépend le
   raisonnement `×4`.

Rien d'autre : pas de fichier `.back` produit, pas de stockage des anneaux
trouvés (au-delà d'un compteur). C'est un outil **permanent** (pas un
instrument jetable) car il sert de socle direct au sous-projet 2 — mais son
seul livrable observable en phase 1 est un chiffre affiché sur la sortie
standard.

## Non-objectifs (explicitement hors scope de cette spec)

- Génération de racines `.back` (sous-projet 2).
- Persistance des anneaux trouvés sur disque.
- Détection/gestion d'un indice qui tomberait un jour sur le bord (l'outil
  échoue explicitement dans ce cas plutôt que de le gérer).
- Optimisation meet-in-the-middle (approche C) — à envisager seulement si la
  mesure sur le jeu 256 pièces montre que l'approche A ne termine pas en
  temps raisonnable.
- Toute modification du moteur de recherche principal (`etii_search.c`,
  `bt_frontier`, MRV). Le walker est un DFS séquentiel indépendant, jamais
  branché sur ces caches.

## Architecture

Même schéma que l'outil `gen_root` existant (`tests/tools/gen_root.c` +
`tests/tools/root_from_board.c`, voir `docs/tests_et_ci.md`) : un cœur pur
testé, une enveloppe CLI fine.

| Fichier | Rôle |
|---|---|
| `tests/tools/border_walk.h` / `.c` | Cœur pur : ordre de parcours de l'anneau, DFS d'énumération, comptage. Aucune I/O. |
| `tests/tools/border_mass.c` | Enveloppe CLI : charge pièces + indices, construit la map, appelle le cœur pur, affiche le résultat. |
| `tests/tools/test_border_walk.c` | Tests unitaires du cœur pur (greatest, comme le reste de `tests/`). |

Le cœur pur (`border_walk.c`) est compilé avec les autres modules dans
`TEST_MODULES` (makefile) et couvert par `make test`, exactement comme
`root_from_board.c`. L'enveloppe (`border_mass.c`) ne l'est pas — nouvelle
cible `.PHONY: border-mass`, calquée sur `gen-root`, compilée à la demande,
`CPPFLAGS` propagé pour permettre `-DETERN_PARTS=16` sur le puzzle de test.

## Pourquoi aucune modification à `part.c` / `readdata.c`

`what_search_in_grid_to_key(all_rotate_parts, possibility, x, y, &key,
all_face)` (`src/core/possibility.c:139`) calcule déjà, pour une case
quelconque `(x,y)`, une clé à 4 faces où chaque composante vaut :
- `0` si c'est un bord de la grille (`x-1<0`, `x+1>=ETERN_SIZE`, etc.) ;
- la vraie couleur du voisin si celui-ci est posé (`grid[..] >= 0`) ;
- le sentinel `all_face` (= `map->sizearrayM`, "toute couleur") si le voisin
  n'est pas posé (`grid[..] == -2`).

Le walker ne pose **jamais** de case intérieure — elle reste à `-2` en
permanence. Le côté intérieur d'une pièce de bord reste donc automatiquement
wildcard, exactement le comportement voulu : le lookup ne contraint que ce
qui doit l'être (les côtés tournés vers l'extérieur du plateau et vers les
cases de bord déjà posées). La pièce 0 (sentinel bordure interne à
`read_parts`, faces toutes à 0) est déjà exclue des candidats par la
convention 1-based existante (`rotate_all_parts`, `src/core/part.c:91`).

Conclusion : le cœur pur réutilise tel quel `prepare_map_part`,
`map_bucket_packed`, `what_search_in_grid_to_key`, `set_face_used`/
`is_face_used`, et la structure `struct possibility_packet` comme état de
travail. Aucune nouvelle fonction, aucun nouveau champ dans `core/`.

## Algorithme

### Ordre de l'anneau

`border_ring_order()` génère les `4*(ETERN_SIZE-1)` couples `(x,y)` en
parcourant le pourtour dans le sens horaire depuis `(0,0)` :
- ligne du haut : `(0,0) → (ETERN_SIZE-1, 0)`
- colonne droite : `(ETERN_SIZE-1, 1) → (ETERN_SIZE-1, ETERN_SIZE-1)`
- ligne du bas : `(ETERN_SIZE-2, ETERN_SIZE-1) → (0, ETERN_SIZE-1)`
- colonne gauche : `(0, ETERN_SIZE-2) → (0, 1)`

`(0,1)` est donc la **dernière** case de l'anneau, juste avant de reboucler
sur `(0,0)`.

### État de travail

Un `struct possibility_packet` ordinaire : grille entièrement initialisée à
`-2`, `b_faceused` vidé. Pas de structure parallèle — c'est exactement l'état
qu'utilise déjà `what_search_in_grid_to_key`/`set_face_used`.

### DFS séquentiel

Ordre fixe de l'anneau (pas MRV, pas `bt_frontier`, pas `directions[]` — un
tableau de position dédié, propre au walker). À chaque étape `i` (case
`(x,y) = ring[i]`) :
1. `what_search_in_grid_to_key(all_rotate_parts, &state, x, y, &key,
   map->sizearrayM)`.
2. `map_bucket_packed(map, &key)` → liste de candidats.
3. Pour chaque candidat non déjà utilisé (`is_face_used`) : le poser
   (`grid[x][y] = id_for_rotated_part(...)`, `set_face_used(...)`), récurser
   sur `i+1`, puis dépose (backtrack).
4. À `i == BORDER_RING_LEN` (tous les 60 posés) : un anneau complet est
   trouvé, `N++`.

### Fermeture de l'anneau

La dernière case posée, `(0,1)`, a pour voisin `(0,0)` (déjà posé depuis la
toute première étape). Sa clé exige donc la vraie couleur du côté `bottom`
de la pièce en `(0,0)`, pas un wildcard. La cohérence de fermeture du cycle
est donc vérifiée **gratuitement** par le mécanisme générique de
`what_search_in_grid_to_key` — aucun code de validation dédié à la fermeture
n'est nécessaire.

### Comptage et masse

`N` = nombre d'anneaux fermés trouvés par le DFS ancré en `(0,0)`.
Masse totale rapportée = `4 × N` (voir « Pourquoi la symétrie tient »). Cette
multiplication reste purement arithmétique en phase 1 : aucune rotation n'est
matérialisée, aucun anneau n'est stocké.

Signature du cœur pur, pensée pour rester stable en phase 2 (callback
optionnel non utilisé pour l'instant) :

```c
typedef void (*border_ring_found_cb)(const struct possibility_packet *ring, void *ctx);

/**
 * Énumère tous les anneaux de bordure valides ancrés en (0,0).
 * on_found (peut être NULL) est appelé pour chaque anneau complet trouvé ;
 * inutilisé en phase 1, réservé à une extension (génération de racines).
 * Retourne N (nombre d'anneaux trouvés ancrés en un coin), jamais la masse
 * ×4 — cette multiplication reste à la charge de l'appelant.
 */
long long border_walk_count(map_big_array *map,
                             struct array_part *all_rotate_parts,
                             border_ring_found_cb on_found, void *ctx);
```

## Précondition vérifiée au démarrage

L'enveloppe `border_mass.c` charge `data/indices.csv` (`read_indices`) et
**échoue explicitement** (message d'erreur clair, code de sortie non nul) si
un indice a `x ∈ {0, ETERN_SIZE-1}` ou `y ∈ {0, ETERN_SIZE-1}` — précondition
dont dépend tout le raisonnement `×4`. Avec les 5 indices actuels, la
vérification passe trivialement ; elle reste un garde-fou explicite plutôt
qu'une hypothèse implicite non vérifiée, conformément à la convention du
projet (« aucune perte de possibilité silencieuse, tout refus a un plan de
secours explicite » — appliqué ici à un refus de calcul plutôt qu'à une
possibilité).

## Tests

`tests/tools/test_border_walk.c`, fixture réduite via `CPPFLAGS`
(`-DETERN_PARTS=<n> -DETERN_SIZE=<n>`, comme les tests existants qui
paramètrent la taille du puzzle) :

- Un petit jeu de pièces de bord construit à la main (par ex. un carré 4×4,
  12 cases de bord) où le nombre d'anneaux valides est calculable/vérifiable
  à la main.
- `border_walk_count` retourne exactement ce nombre.
- Aucun anneau trouvé ne réutilise une pièce deux fois (assertion sur
  `b_faceused` à la fermeture de chaque anneau trouvé, via le callback).
- Cas dégénéré : jeu de pièces sans solution de bordure valide →
  `border_walk_count` retourne 0 sans planter.

## Build

Nouvelle cible dans `makefile`, calquée sur `gen-root` :

```make
BORDER_MASS_BIN := tests/tools/border_mass

.PHONY: border-mass
border-mass:
	gcc -Wall -Wextra -std=gnu99 -O2 -Isrc -Itests $(CPPFLAGS) -Werror -o $(BORDER_MASS_BIN) \
	    tests/tools/border_mass.c tests/tools/border_walk.c \
	    src/core/readdata.c src/core/part.c src/ui/logger.c \
	    src/core/core_static_variables.c src/app/app_static_variables.c -lm -pthread
```

`tests/tools/border_walk.c` ajouté à `TEST_MODULES` pour être couvert par
`make test`/`make coverage`, comme `root_from_board.c`.

## Documentation à mettre à jour

- `docs/tests_et_ci.md` : nouvelle section « Outil `border_mass` », sur le
  modèle de la section `gen_root` existante.
- `tests/README.md` : entrée dans le tableau des outils `tests/tools/`.
- `AGENTS.md` : pas de changement structurel (pas de nouveau domaine source,
  pas de nouvelle variable globale) — seule la doc ci-dessus est requise.

## Critères de succès de la phase 1

- Compile et tourne sans erreur sur le puzzle de test (16 pièces).
- Tourne sur le jeu réel (256 pièces, `data/pieces.csv`) et termine en temps
  mesurable (quelques minutes à quelques heures jugées acceptables ; au-delà,
  la phase 1 rapporte l'échec de tractabilité comme un résultat en soi et
  déclenche la discussion sur l'approche C).
- `make test` (avec `border_walk.c` inclus) reste vert, `WERROR=1` compris.

## Risque connu, assumé

Rien ne garantit que le DFS séquentiel termine en temps raisonnable sur le
vrai jeu 256 pièces — la mesure sur ce jeu fait partie du livrable de phase 1,
pas une simple vérification a posteriori. Bascule vers l'approche C
(meet-in-the-middle, deux demi-anneaux calculés séparément puis joints)
seulement si la mesure le justifie.

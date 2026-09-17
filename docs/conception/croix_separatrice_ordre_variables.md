# Croix séparatrice : un a priori structurel dans l'ordre des variables

**Statut : en cours d'implémentation — PR1 livrée, la campagne (PR2) reste à exécuter.** Aucune ligne de moteur en production. Le document tranche deux
choses avant tout code — la **géométrie** de la croix (normalisée, et démontrée
séparatrice) et le **rejet de la variante à ordre imposé**, écartée sur quatre
pré-mesures statiques reproductibles en moins d'une seconde, dans la méthode du
§4.9 de [elagage_recherche.md](elagage_recherche.md). Restent deux bras à mesurer,
`cross-key` et `cross-mrv`, qui ne sont que **le même bit placé à deux hauteurs
différentes** dans la clé composite de `mrv_choose_cell`. Les mêmes pré-mesures
**prédisent que `cross-mrv` perdra** (§4.3) : il est conservé parce qu'il a été
demandé et qu'il ne coûte presque rien de plus que `cross-key`, pas parce qu'on
l'attend gagnant.

Ce document ouvre l'axe que le §7.7 de
[banc_resolution_clones.md](banc_resolution_clones.md) a explicitement laissé
ouvert : le **départage des cases**, « troisième axe du §3.4 : il n'a pas été
mesuré […] le mesurer demanderait un second point d'entrée sous
`ETII_BENCH_HOOKS`, que PR3 n'a pas livré ». Le résultat négatif de la campagne
« côté trouver » porte sur l'ordre des **valeurs** et sur le point de **départ** :
il ne ferme rien ici.

## 0. État de l'implémentation

| PR | État | Ce qui a été livré |
|---|---|---|
| 1 | **livrée** | `tests/bench/cross_mask.{h,c}` (croix en compréhension, complément, tirage de contrôle à densité imposée) + `tests/bench/test_cross_mask.c` rattaché à `make test` ; second point d'entrée `ETII_BENCH_CELL_HOOKS` dans `mrv_choose_cell`/`_fast` ; les six bras dans `bench_refutation` (`cross-key`/`rand-key`/`anti-key`, `cross-mrv`/`rand-mrv`/`anti-mrv`), `--cross-seed` ; auto-test des deux balayages par bras |
| 2 | à exécuter | Campagne `bench_refutation` sur stock de production (§6) |
| 3 | conditionnelle | `bench_solve` sur clones, si un bras passe le §6.5 (1-3) |
| 4 | conditionnelle | Adoption inconditionnelle du bras retenu |

**Verrou de PR1 tenu** : `build/core/etii_search.o` compilé avant et après
l'ajout du hook est **octet pour octet identique** — la production ne voit
strictement rien de ce mécanisme. Vérifié par recompilation des deux versions
avec les drapeaux du makefile.

**Écarts assumés par rapport au plan.**

- **Deux macros, pas une.** Le §5.4 parlait d'« un second point d'entrée sous
  `ETII_BENCH_HOOKS` ». L'implémentation en fait une macro distincte,
  `ETII_BENCH_CELL_HOOKS`, qu'`ETII_BENCH_HOOKS` implique. Raison : si
  `bench_refutation` définissait `ETII_BENCH_HOOKS`, il hériterait de
  l'indirection d'ordre des VALEURS (un test de pointeur par candidat dans sa
  boucle chaude) et ses temps cesseraient d'être comparables à ses campagnes
  précédentes — pour un mécanisme qui, par construction, n'a aucun effet sur un
  sous-arbre mort.
- **`cross-mrv` n'est pas un bit en poids fort, c'est une seconde passe.** Le
  §5.3 identifiait le piège (un bit au-dessus de `count` casse la détection de
  case morte du chemin rapide) et proposait un second minimum sur `count` seul.
  La seconde passe restreinte, exécutée **après** le verdict de mort, obtient le
  même résultat et ne coûte rien aux autres bras (un test de pointeur par nœud
  au lieu d'un registre et d'un `cmov` par case de frontière). Le verdict de
  mort reste celui du balayage complet, ce qui était l'exigence.
- **Les six bras ne sont pas joués par défaut.** `--engines` vaut toujours
  `mrv,mrv+singleton` sans argument : une invocation existante du banc mesure
  exactement ce qu'elle mesurait.
- **Le banc refuse de tourner en build 16.** Conséquence du cas dégénéré du §3
  (en 4×4 la croix est le plateau entier) : plutôt que de produire des chiffres
  qui ne veulent rien dire, il sort en erreur.

**Première observation, à l'exécution et non encore une mesure** : sur des
racines fabriquées que `mrv` ferme en 4 nœuds, `cross-mrv` épuise un plafond de
2 000 000. C'est la direction annoncée par le §4.3, sur trop peu de racines pour
compter comme un résultat — la campagne reste à faire.

## 1. Question posée

> Fermer d'abord un **séparateur** du plateau — la croix formée par les deux
> diagonales — plutôt que de laisser MRV remplir de proche en proche, est-il un
> meilleur ordre de variables ?

L'intuition derrière la proposition est structurelle et non heuristique : les
deux diagonales coupent le plateau en quatre régions qui ne se touchent plus, et
elles passent exactement par les cinq indices officiels. MRV, lui, n'a aucune
notion de structure globale : il choisit localement la case la moins pourvue en
candidats, et le §3.2 de [elagage_recherche.md](elagage_recherche.md) mesure
qu'il balaie une frontière de 52,2 cases en moyenne — il ne remplit pas de
proche en proche, mais il ne vise rien non plus.

La question n'est donc pas « MRV est-il bon ? » (il l'est : §4.7) mais
« un a priori structurel vaut-il d'être **ajouté** à son critère, et à quelle
hauteur de la clé ? ».

## 2. Ce que le code fait aujourd'hui

| Point | État | Conséquence |
|---|---|---|
| Choix de la case, recherche | `mrv_choose_cell` / `_fast`, un **seul** site d'appel (`etii_search.c:1632`) | un bras s'insère en un point unique |
| Choix de la case, expansion serveur | `light_choose_cell` (`possibility.c`), MRV autonome hors boucle chaude | un bras y serait *aussi* applicable — hors périmètre de ce document |
| `directions[]`/`dirx[]`/`diry[]` | simple énumération depuis VERSION 13 | **ne peut pas porter la croix** : `directions[]` doit rester une permutation des `ETERN_PARTS` indices, verrouillé par `test_directions` |
| Clé de choix | composite, calculée à la compilation (`count`, `nconstr`, `weight`, `pos`) | l'ajout d'un critère est un **champ de plus**, pas une branche de plus |
| Départage appris | `bt_frontier.weight`, §4.14, −79,6 % de nœuds de réfutation | un a priori statique ne le remplace pas : il se **compose** avec lui, et sa hauteur dans la clé dit lequel domine |
| Interrupteurs de mesure | règle du dépôt (§6 de [mrv_moteur_unique.md](mrv_moteur_unique.md)) : pas de drapeau runtime inutilisé en production | les bras vivent sous `#ifdef ETII_BENCH_HOOKS` |

## 3. La croix : vérification et normalisation

La proposition d'origine donnait une liste de 89 cases (trois tables
redondantes, `directions[i] == diry[i]*16 + dirx[i]`, 89 cases distinctes, les
cinq indices en tête). Elle est correcte, et **elle sépare bien** : les 167
cases restantes forment exactement quatre composantes connexes, de 38, 42, 42 et
45 cases. Deux corrections l'amènent à une forme normalisée.

**Forme retenue — définition en compréhension, pas en extension :**

```
croix(n) = { (x,y) : |x − y| ≤ 1  ou  |x + y − (n−1)| ≤ 1 }
```

Pour `n = 16` : **88 cases**, et quatre régions de **42 cases exactement**.

```
   0123456789012345      # = croix (88)   O = indice officiel
 0 ##............##       Régions restantes : 42 / 42 / 42 / 42
 1 ###..........###       Aucun passage orthogonal entre régions.
 2 .#O#........#O#.
 3 ..###......###..       Les CINQ indices officiels sont sur la croix :
 4 ...###....###...         (2,2) (13,13) sur y = x
 5 ....###..###....         (13,2) (2,13) (7,8) sur y = n−1−x
 6 .....######.....         — ce n'est pas une coïncidence, les indices
 7 ......####......           officiels sont placés symétriquement.
 8 ......#O##......
 9 .....######.....
10 ....###..###....
11 ...###....###...
12 ..###......###..
13 .#O#........#O#.
14 ###..........###
15 ##............##
```

Ce qui change par rapport à la liste de 89 : trois cases de la bande basse
gauche (`(4,12) (5,11) (6,10)` entrent, `(2,11) (3,10) (4,9) (5,8)` sortent).
L'asymétrie de la liste d'origine déséquilibrait les régions (38/45 contre
42/42) sans raison identifiable.

**Pourquoi ces deux corrections :**

- **En compréhension** : la croix devient définie pour **toute** taille compilée,
  donc mesurable sur les clones de `bench_solve` (64/100/144/196) sans table
  supplémentaire, et un test la vérifie au lieu de la recopier.
- **Symétrique** : quatre régions égales, ce que la liste d'origine n'assurait
  pas.

**L'épaisseur 2 est nécessaire, et c'est mesuré.** Le X *fin* (32 cases, les deux
diagonales simples) sépare aussi le plateau — en quatre régions de 56 — et
contient lui aussi les cinq indices, puisque `7+8 = 15`. Il est pourtant
inutilisable : les cases `(i,i)` ne sont pas voisines orthogonales entre elles,
donc **26 de ses 32 cases s'ouvriraient avec zéro côté contraint**. L'épaisseur 2
est le minimum pour que la bande se contraigne elle-même en avançant.

## 4. Pré-mesures statiques : ce que coûte la croix à l'entrée

Quatre grandeurs, aucune exécution du solveur, script en annexe.

### 4.1 Côtés contraints à l'ouverture

Nombre de côtés contraints (bords de grille + voisines déjà posées) au moment où
chaque case s'ouvre, sous un choix glouton « le plus contraint d'abord » — proxy
statique du critère MRV. Les cinq indices sont exclus : ils sont posés par
`first_possibility`, ils ne sont pas des points de branchement.

| Ordre | 1 côté | 2 côtés | ≥ 3 côtés |
|---|---|---|---|
| Croix 89, ordre **imposé** | **37** | 42 | 5 (dont 2 à 4 côtés) |
| Croix 88, **MRV restreint à la croix** | **30** | 53 | 0 |
| MRV **libre**, même budget de coups | **0** | 74 | 9 |

MRV libre n'ouvre **jamais** une case mono-contrainte. La croix y est forcée par
sa géométrie : une bande d'épaisseur 2 qui avance expose nécessairement un front
de cases à un seul voisin posé. Ce n'est pas un défaut de l'ordre proposé, c'est
le prix du séparateur.

### 4.2 Ce que vaut un côté contraint de moins, sur le jeu de pièces réel

Candidats moyens (pondérés par probabilité de rencontre) sur `data/pieces.csv`,
toutes rotations confondues :

| Faces imposées | Candidats moyens |
|---|---|
| 1 | **47,4** |
| 2 | **4,33** |
| 3 | 1,24 |

Un facteur **11** entre une case mono-contrainte et une case bi-contrainte.

### 4.3 Le produit, à budget de coups égal

En composant §4.1 et §4.2 — produit brut des facteurs de branchement sur les
mêmes 83 coups (84 pour la liste de 89), **avant tout élagage** :

| Ordre | log₁₀ du produit | Écart à MRV libre |
|---|---|---|
| Croix 89, ordre **imposé** | **89,2** | 10⁴¹ |
| Croix 88, **MRV restreint à la croix** (`cross-mrv`) | **84,0** | 10³⁶ |
| MRV **libre** | **47,9** | — |

Le majorant est grossier (il ignore le forward-check, qui tue une grande partie
de ces 47 candidats avant qu'un niveau s'ouvre, et l'épuisement des pièces).
Aucun raffinement ne rattrape trente-six ordres de grandeur.

**Deux conséquences, à énoncer avant la campagne et non après.**

**Arbitrage : la variante à ordre imposé (`cross-hard`) est écartée sans
implémentation.** C'est la troisième piste du dépôt tranchée avant écriture de
code, après le CBJ (§4.11) et la table de région (§4.9).

**Prédiction sur `cross-mrv` : il devrait perdre, et largement.** Restreindre MRV
à la croix ne supprime pas le problème, il le réduit d'un facteur 10⁵ sur un
handicap de 10⁴¹. Le bras est conservé dans la campagne — il a été demandé, il
partage tout son outillage avec l'autre, et une prédiction statique n'est pas une
mesure — mais s'il gagne contre cette prédiction, c'est le **proxy** du §4.1
qu'il faudra corriger d'abord, pas la conclusion qu'il faudra célébrer.

**`cross-key` échappe entièrement à ce calcul**, et c'est ce qui en fait le seul
bras réellement candidat : le bit y est *sous* `count`, donc il ne départage que
des cases de **score MRV égal**. Il n'ouvre jamais une case plus branchante
qu'une autre, et le tableau ci-dessus ne s'y applique pas.

### 4.4 La transposition aux clones n'est pas à fraction constante

| Taille | Croix | Part du plateau | Régions |
|---|---|---|---|
| 8×8 | 40 | 62 % | 6 × 4 |
| 10×10 | 52 | 52 % | 12 × 4 |
| 12×12 | 64 | 44 % | 20 × 4 |
| 16×16 | 88 | **34 %** | 42 × 4 |

La croix est en `O(4n)` et les régions en `O(n²/4)` : plus l'instance est petite,
plus la croix mange le plateau. Un résultat sur clones mesurera donc le
**mécanisme**, jamais la magnitude attendue au 16×16 — c'est exactement la
réserve du §6.4 de [banc_resolution_clones.md](banc_resolution_clones.md), à
consigner avant la campagne et non après.

## 5. Conception : un bit, deux hauteurs

### 5.1 Le constat qui simplifie tout

La clé de `mrv_choose_cell` est déjà composite, et son ordre lexicographique
**est** la règle de choix :

```
key = (count << (NC + W)) | ((NC_MAX − nconstr) << W) | (W_MAX − weight)
```

Les deux bras demandés ne sont pas deux mécanismes : ce sont **le même champ
d'un bit** (« cette case est-elle sur la croix ? », 0 = oui pour que le plus
petit gagne), inséré à deux hauteurs différentes.

| Bras | Hauteur du bit | Sémantique |
|---|---|---|
| `cross-mrv` | **au-dessus de `count`** | toute case de croix passe avant toute autre ; MRV arbitre *à l'intérieur* de la croix, puis reprend la main partout une fois la croix pleine |
| `cross-key` | **entre `count` et `nconstr`** | à score MRV égal, une case de croix d'abord ; MRV n'ouvre jamais une case plus branchante qu'une autre |

C'est la lecture qui rend les deux bras comparables : ils partagent la table, le
hook, les contrôles et les tests ; seule une constante de décalage change.

### 5.2 Budget de bits : il y en a, et c'est vérifié à la compilation

Le chemin rapide empile tout dans un `uint32_t` :
`cand = (count << MRV_KEY_LOW_BITS) | nc_key`, avec
`MRV_KEY_LOW_BITS = NC(3) + W(8) + POS(8) = 19` et `count ≤ ETERN_PARTS` (9 bits),
soit 28 bits. Le contrôle `mrv_pos_fits_check` exige
`(ETERN_PARTS + 1) << MRV_KEY_LOW_BITS < 0x7fffffff` :

| `MRV_KEY_LOW_BITS` | `257 << LOW` | Tient ? |
|---|---|---|
| 19 (aujourd'hui) | 1,35·10⁸ | oui |
| 22 | 1,08·10⁹ | oui |
| 23 | 2,16·10⁹ | **non** |

**Trois bits de marge, un seul requis.** Le contrôle à la compilation existant
suffit à interdire la faute ; il n'y a rien à ajouter de ce côté.

### 5.3 Coût dans la boucle chaude : `cross-key` est gratuit, `cross-mrv` ne l'est pas

L'appartenance à la croix est **statique par case** : elle peut être cuite dans
`bt_frontier.nc_key` à l'initialisation et n'être jamais recalculée. Pour
`cross-key`, le balayage garde donc exactement une charge de 32 bits et un `or`
par case de frontière — **coût nul par nœud**, ce qui compte dans une boucle
mesurée *issue-bound* (IPC ≈ 3,15, ~0,25 cycle par µop × ~54 cases par nœud,
§1.3 quater de [autosearch_step.md](../autosearch_step.md)).

`cross-mrv` a un piège, et c'est le point technique central de ce document.

> Le chemin rapide détecte la case morte par `(best >> MRV_KEY_LOW_BITS) == 0`,
> c'est-à-dire « le minimum de la clé a un `count` nul » — ce qui n'est vrai que
> parce que `count` est le champ de **poids fort**. Un bit de croix placé
> au-dessus de lui casse cette lecture : une case morte **hors croix** cesserait
> d'être vue au moment du choix.

La branche mourrait quand même, un niveau plus bas — mais on aurait alors
comparé un **élagage affaibli**, pas un ordre différent. La mesure ne dirait
rien. Correction obligatoire : maintenir un second minimum, sur `count` seul,
pour le verdict de mort — un registre et un `cmov` de plus par case de
frontière. Donc :

- **`cross-key` : aucun surcoût par nœud** ;
- **`cross-mrv` : surcoût réel**, à mesurer en temps autant qu'en nœuds.

C'est précisément la leçon de méthode du §7.6 de
[banc_resolution_clones.md](banc_resolution_clones.md) — « mesurer le temps, pas
seulement les nœuds » — qui y avait rejeté `mcv`, gagnant en nœuds sur une
cellule.

Conséquence annexe : `mrv_choose_cell` (générique) sort *immédiatement* sur
`count == 0`, alors que `mrv_choose_cell_fast` va au bout du balayage. Sous
`cross-mrv`, les deux doivent continuer de rendre **le même verdict et la même
case** — l'équivalence est déjà verrouillée par
`mrv_choose_cell_fast_matches_generic_on_real_map`, ce test doit rester vert
pour chaque bras.

### 5.4 L'instrument : le second point d'entrée que PR3 n'avait pas livré

`bench_solve` déclare aujourd'hui un seul hook (`etii_bench_value_order`, ordre
des **valeurs**). Il en faut un second, pour l'ordre des **cases** : une fonction
rendant, pour une case, le bit à injecter — et **NULL par défaut**, comme
l'autre, pour que la politique de production ne subisse aucune indirection.

Le verrou de PR3 est repris tel quel et n'est pas négociable :
`build/core/etii_search.o` compilé avec et sans le hook doit être **octet pour
octet identique**.

## 6. Protocole de mesure et règle de décision

### 6.1 Instrument primaire

`bench_refutation`, `--from-back` sur un **stock de production réel**, comparaison
**appariée** (mêmes racines, même plafond de nœuds) : c'est l'instrument qui a
fait adopter §4.12 et §4.14, et la seule métrique dont le projet ait démontré
qu'elle transporte.

Métriques rapportées ensemble, jamais séparément : **nœuds de réfutation**,
**nombre de fermetures**, **temps**, **temps par nœud**.

### 6.2 Deux contrôles obligatoires

**Contrôle `random`.** Le §4.14 a mesuré qu'un départage **aléatoire** bat déjà
l'ordre positionnel de 37 à 56 %. Un bit de croix inséré au-dessus du champ
`pos` perturbe l'ordre positionnel *par construction* : sans contrôle, un gain
mesuré ne distingue pas « la croix est un bon a priori » de « n'importe quoi vaut
mieux que l'ordre des bits ». Le contrôle est un bit **tiré au sort par case**, à
la **même densité** que la croix (34 % au 16×16) et à la **même hauteur** de clé.

**Contrôle `anti-croix`.** Le même bit, inversé : les cases *hors* croix
d'abord. Si l'anti-croix gagne aussi, ce qui est mesuré n'est pas le séparateur.

Un bras n'est retenu que s'il bat **les deux** contrôles.

### 6.3 Biais de source des racines, à énoncer et non à corriger en douce

Les racines d'un stock de production ont été fabriquées par l'expansion MRV
(`search_possiblity_light`) : ce sont des plateaux de forme MRV, ce qui avantage
structurellement le bras qui leur ressemble. Le mode à racines fabriquées
(`--depths`) ne corrige pas le biais, il le déplace — la descente y est menée par
un moteur, donc les racines deviennent propres à chaque bras et **l'appariement
est perdu**. Les deux sont rapportés ; le mode `--from-back` reste primaire
puisque c'est le régime de production ; le biais est écrit dans le résultat.

### 6.4 Censure

Une exécution qui atteint le plafond de nœuds donne un **minorant**, jamais une
valeur. On compte des **fermetures** et on compare des médianes sur paires
complètes — on ne moyenne pas des exécutions tronquées. Règle héritée du §7.5
de [banc_resolution_clones.md](banc_resolution_clones.md), où la censure jouait
dans le sens qui favorisait la conclusion tentante.

### 6.5 Règle de décision

Un bras est adopté si, et seulement si :

1. **gain apparié** en nœuds de réfutation sur le stock de production, avec au
   moins autant de fermetures ;
2. **pas de régression en temps par nœud** (critère (c) du §3.5 de
   `banc_resolution_clones.md`) ;
3. le gain **survit aux deux contrôles** du §6.2 ;
4. *alors seulement*, confirmation « côté trouver » sur clones via `bench_solve`
   — avec la réserve du §4.4 sur la fraction de plateau.

Un bras adopté devient **inconditionnel** : pas de drapeau runtime laissé en
place (règle §6 de [mrv_moteur_unique.md](mrv_moteur_unique.md), un interrupteur
survivant est un chemin de code non testé).

## 7. Arbitrages tranchés

| Sujet | Décision | Raison |
|---|---|---|
| Ordre **imposé** sur la croix (`cross-hard`) | **écarté sans implémentation** | §4.3 : 37 niveaux mono-contraints, 10⁴¹ sur le produit des facteurs de branchement |
| X **fin** (32 cases) | écarté | 26 de ses 32 cases s'ouvrent à zéro côté contraint (§3) |
| Géométrie | croix **en compréhension**, `abs(x−y) ≤ 1 ou abs(x+y−(n−1)) ≤ 1` | définie pour toute taille, symétrique (4 × 42), contient les 5 indices, testable au lieu d'être recopiée |
| Support de la croix | table **propre**, jamais `directions[]` | `directions[]` doit rester une permutation des `ETERN_PARTS` indices (`test_directions`) |
| Rapport au poids appris (§4.14) | **composition**, pas remplacement | le bit est statique, le poids est appris ; la hauteur dans la clé dit lequel domine, et c'est ce que la campagne mesure |
| Portée | `mrv_choose_cell` seulement | `light_choose_cell` (expansion serveur) est hors périmètre : hors boucle chaude, et la question y est celle de la *forme du stock*, pas de l'ordre de recherche |
| Interrupteur | `#ifdef ETII_BENCH_HOOKS` | règle du dépôt ; adoption ⇒ inconditionnel |

## 8. Points laissés ouverts

- **Le séparateur ne se monnaye pas tout seul — et ce document ne prétend pas le
  monnayer.** Les quatre régions ne sont indépendantes que pour les contraintes
  de **couleur** ; elles tirent dans le **même sac de pièces**, et c'est ce
  couplage qui fait la difficulté d'Eternity II. Un DFS avec forward-check ne
  capitalise aucune décomposition : il ré-explorera la région 1 à chaque échec de
  la région 4. Encaisser réellement la coupure demanderait une **table de région
  par composante** — famille écartée au §4.9 de
  [elagage_recherche.md](elagage_recherche.md), où la croissance mesurée (×2,6
  par case intérieure) met une région de 42 cases très au-delà du stockable. Ce
  document mesure donc un **biais d'ordre**, pas une décomposition. Si les deux
  bras perdent, c'est cette explication-là qu'il faudra retenir en premier.
- **L'épaisseur 3** n'est pas mesurée (l'épaisseur 1 est écartée, la 2 est
  mesurée).
- **Le hasard comme politique**, et non comme contrôle. §4.14 a montré qu'un
  départage aléatoire bat l'ordre positionnel ; si `random` bat aussi les deux
  bras de croix, la question « faut-il randomiser le dernier champ de la clé en
  production ? » devient ouverte — elle n'est pas dans ce document.
- **L'expansion serveur** (`light_choose_cell`) : une croix fermée *avant*
  distribution changerait la forme du stock, pas l'ordre de recherche. Question
  distincte, non instruite ici.
- **La transposition au 16×16** depuis les clones, à fraction de croix non
  constante (§4.4).

## 9. Découpage en PR

| PR | Contenu | Risque | Verrou |
|---|---|---|---|
| 1 | **livrée** (cf. §0). Table de croix en compréhension + tests ; second point d'entrée (`ETII_BENCH_CELL_HOOKS`) ; les six bras dans `bench_refutation` ; seconde passe restreinte plutôt que second minimum sur `count` (§0) | moyen — le piège de la détection de case morte est silencieux | `etii_search.o` **octet pour octet identique** avec et sans le hook ✓ ; équivalence des deux balayages contrôlée **pour chaque bras** par l'auto-test du banc ✓ ; `make test` vert ✓ |
| 2 | Campagne `bench_refutation` sur stock de production, consignée ici | mesure | les quatre bras sur les mêmes racines, nœuds **et** temps, fermetures comptées, censure déclarée |
| 3 | *Conditionnelle* — `bench_solve` sur clones si un bras passe le §6.5 (1-3) | mesure | réserve du §4.4 écrite avec le résultat |
| 4 | *Conditionnelle* — adoption inconditionnelle, suppression du hook du bras retenu | faible | `bench_refutation` non régressé, doc `docs/` et `AGENTS.md` mises à jour |

PR1 ne touche aucun chemin de production. PR2 peut conclure négativement, et
c'est une issue prévue : le §8 dit déjà quelle explication retenir.

## Annexe — script des pré-mesures du §4

Reproduit les quatre tableaux du §4 sans exécuter le solveur (`python3 croix.py
data/pieces.csv`). Sortie attendue au 16×16 : croix de 88 cases, régions
`[42, 42, 42, 42]`, profil `{1: 30, 2: 53}` contre `{2: 74, 3: 9}`, candidats
moyens `47,4 / 4,33 / 1,24`.

```python
#!/usr/bin/env python3
"""Pré-mesures statiques de la croix séparatrice (§4)."""
import sys
from collections import Counter

N = 16
HINTS = {(7, 8), (2, 2), (13, 2), (2, 13), (13, 13)}


def cross(n=N):
    """Croix normalisée : les deux diagonales épaissies à 2 cases."""
    return {(x, y) for x in range(n) for y in range(n)
            if abs(x - y) <= 1 or abs(x + y - (n - 1)) <= 1}


def components(sep, n=N):
    """Tailles des composantes connexes (4-voisinage) du complément de `sep`."""
    seen, out = set(), []
    for c in [(x, y) for x in range(n) for y in range(n) if (x, y) not in sep]:
        if c in seen:
            continue
        stack, k, seen = [c], 0, seen | {c}
        while stack:
            cx, cy = stack.pop()
            k += 1
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                nb = (cx + dx, cy + dy)
                if nb in seen or nb in sep:
                    continue
                if 0 <= nb[0] < n and 0 <= nb[1] < n:
                    seen.add(nb)
                    stack.append(nb)
        out.append(k)
    return sorted(out)


def nsides(x, y, placed, n=N):
    """Côtés contraints : bords de grille + voisines déjà posées."""
    k = 0
    for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        nx, ny = x + dx, y + dy
        if not (0 <= nx < n and 0 <= ny < n):
            k += 1
        elif (nx, ny) in placed:
            k += 1
    return k


def opening_profile(pool, budget=None, n=N):
    """Distribution des côtés contraints à l'ouverture, sous un choix glouton
    « le plus de côtés contraints d'abord » — proxy statique de MRV."""
    placed, todo, c = set(HINTS), set(pool) - HINTS, Counter()
    budget = len(todo) if budget is None else budget
    for _ in range(min(budget, len(todo))):
        best = max(todo, key=lambda t: (nsides(*t, placed, n), -(t[0] * n + t[1])))
        c[nsides(*best, placed, n)] += 1
        placed.add(best)
        todo.discard(best)
    return dict(sorted(c.items()))


def branching(path):
    """Candidats moyens (pondérés par probabilité de rencontre) pour 1, 2 et 3
    faces imposées, sur le jeu de pièces réel."""
    rows = [tuple(int(v) for v in ln.split())
            for ln in open(path).read().splitlines()[1:] if len(ln.split()) == 5]
    f = [Counter(), Counter(), Counter()]
    for _id, t, l, b, r in rows:
        faces = [t, l, b, r]
        for k in range(4):
            rot = [faces[(k + i) % 4] for i in range(4)]
            for j in range(3):
                f[j][tuple(rot[:j + 1])] += 1
    return [sum(v * v for v in c.values()) / sum(c.values()) for c in f]


def main():
    pieces = sys.argv[1] if len(sys.argv) > 1 else "data/pieces.csv"
    X = cross()
    print("croix normalisée : %d cases, régions %s" % (len(X), components(X)))
    print("indices officiels tous sur la croix :", HINTS <= X)
    print("croix, MRV restreint   :", opening_profile(X))
    print("MRV libre (même budget):", opening_profile(
        {(x, y) for x in range(N) for y in range(N)}, budget=len(X) - len(HINTS)))
    print("candidats moyens : 1 face %.1f | 2 faces %.2f | 3 faces %.2f" % tuple(branching(pieces)))
    for n in (8, 10, 12, 14):
        G = cross(n)
        print("clone %dx%d : croix %d/%d cases (%.0f %%), régions %s"
              % (n, n, len(G), n * n, 100.0 * len(G) / (n * n), components(G, n)))


if __name__ == "__main__":
    main()
```

Le tableau du §4.3 se déduit des deux précédents :
`37·log₁₀(47,4) + 42·log₁₀(4,33) + 5·log₁₀(1,24) = 89,2` (croix 89 imposée),
`30·log₁₀(47,4) + 53·log₁₀(4,33) = 84,0` (croix 88, MRV restreint) et
`74·log₁₀(4,33) + 9·log₁₀(1,24) = 47,9` (MRV libre).

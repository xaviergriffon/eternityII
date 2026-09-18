# Croix séparatrice : un a priori structurel dans l'ordre des variables

**Statut : deux campagnes exécutées, résultat NÉGATIF dans les deux régimes — aucun changement de moteur adopté.** PR1 (outillage) livrée, PR2 (réfutation) au §7, PR3 (« côté trouver ») au §7 bis, PR4 sans objet. Aucune ligne de moteur en production, et il n'y en aura pas : la croix séparatrice ne vaut rien comme a priori d'ordre des variables. L'outillage, lui, reste — le second point d'entrée rend l'ordre des VARIABLES mesurable sur `bench_refutation`, ce qu'aucun instrument du dépôt ne savait faire. Le document tranche deux
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
| 2 | **exécutée** (§7) | 2 300 fermetures de sous-arbres sur stock de production. `cross-key` strictement neutre (113/119, p = 0,74 sur 232 racines réellement modifiées) ; `cross-mrv` ferme 94 racines sur 200 contre 199 ; le seul signal (`anti-mrv`) est dissous par son contrôle de densité. Un bras de contrôle ajouté en cours de route (`randc-*`, cf. §7.4) |
| 3 | **exécutée** (§7 bis) | Campagne « côté trouver » sur clones — le régime que le §7 ne pouvait pas voir, et celui où la proposition se jouait vraiment. 540 exécutions, 2 cellules, 9 bras dont 4 contrôles. Bras `halo-*` ajouté (le halo des indices : 20 cases au lieu de 88 pour le même effet). Aucun bras ne gagne ; `halo-mrv` ne résout **aucune** des 60 instances |
| 4 | **sans objet** | Rien à adopter |

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

**Écart de PR2 par rapport au plan** : le §6.2 exigeait un contrôle aléatoire
« à même densité », sans voir que deux bras de densités différentes en exigent
**deux**. Le contrôle `randc-*` (densité du complément) a donc été ajouté en
cours de campagne — et c'est lui qui a dissous le seul signal qu'elle avait
produit (§7.4).

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

## 7. Mesures — PR2, campagne (2026-09-17)

### 7.1 Protocole

Stock de **production** (`eternityII.back`) : 32 480 possibilités, pièces posées
min/moy/max = 8 / 102,5 / 184, intégrité vérifiée par le banc (0 paquet
incohérent, 0 `alloc` différent du recomptage). 11 224 racines à ≥ 130 pièces —
la bande du §4.14, qui n'en avait que 418 sur son stock.

Quatre séries, toutes en comparaison **appariée** (mêmes racines, même ordre,
même plafond de 5 000 000 de nœuds), les racines étant les N premières de la
bande dans l'ordre du fichier — donc identiques pour tous les bras d'une série.

| Série | Bras | Racines |
|---|---|---|
| A | `mrv` `cross-key` `rand-key` `anti-key` | 500 |
| B | `mrv` `cross-mrv` `rand-mrv` `anti-mrv` | 200 |
| C | A moins `anti-key`, aux graines 2, 3, 4 | 500 × 3 |
| D | `mrv` `anti-mrv` `randc-mrv` `anti-key` `randc-key` | 200 |

La série D a été ajoutée **en cours de campagne** : voir §7.4.

### 7.2 Hauteur « -key » : `cross-key` est strictement neutre

| Bras | Fermées | Racines changées | Apparié V/D | Moy. géo | p |
|---|---|---|---|---|---|
| `mrv` | 499/500 | — | — | — | — |
| `cross-key` | 499/500 | 232 | 113/119 | 0,999 | **0,74** |
| `rand-key` | 494/500 | 319 | 170/149 | 1,041 | 0,26 |
| `anti-key` | 499/500 | 269 | 110/159 | 0,710 | 0,003 |

`cross-key` **change réellement l'arbre** — 232 racines sur 499, soit 46 % — et
n'y gagne rien : 113 victoires contre 119 défaites, moyenne géométrique 0,999.
Aucune bande de profondeur ne le sauve (130-139 : 60/70, p = 0,43 ; 140-149 :
41/35, p = 0,57 ; 150-159 : 9/10 ; 160+ : 3/4).

**Le critère (c) passe, et c'est la seule bonne nouvelle du tableau** : sur les
494 racines fermées par tous, le coût par nœud est de **0,734 µs pour
`cross-key` contre 0,738 µs pour la production**. Le « coût par nœud nul »
annoncé au §5.3 — bit statique cuit dans `nc_key`, balayage inchangé — est donc
vérifié à la mesure. Mais (c) ne sert qu'à départager un gain, et il n'y en a
pas.

**La moyenne géométrique de 0,710 d'`anti-key` est un mirage, et c'est la
première leçon d'instrument de cette campagne.** Sa médiane vaut 1,019, sa
moyenne géométrique élaguée à 10 % vaut 0,913, et il **perd** le test des
signes (110/159, p = 0,003). Les 0,710 viennent de trois racines chanceuses
(1 477 421 → 21 nœuds, 3 785 855 → 1 026, 552 283 → 269) qui portent à elles
seules 15 % de la somme des |log| des ratios. Sur cette distribution, une
moyenne — géométrique comprise — ne classe rien.

### 7.3 Hauteur « -mrv » : `cross-mrv` s'effondre, et ce n'est pas la croix

| Bras | Fermées | Nœuds totaux | Temps | µs/nœud |
|---|---|---|---|---|
| `mrv` | **199/200** | 20 051 814 | 15,0 s | 0,748 |
| `cross-mrv` | **94/200** | 551 475 608 | 579,6 s | 1,051 |
| `rand-mrv` | **86/200** | 574 318 659 | 1 027,7 s | 1,789 |
| `anti-mrv` | 198/200 | 15 550 846 | 16,9 s | 1,085 |

**`cross-mrv` ferme 94 racines sur 200 là où la production en ferme 199.** La
direction annoncée par le §4.3 est confirmée. Sa magnitude ne l'est pas, et il
faut le dire : le §4.3 annonçait 10³⁶ sur un produit de facteurs de branchement
**sans aucun élagage**, la mesure donne un facteur 27 en nœuds et surtout une
censure massive. Le majorant était bon pour trancher une direction, pas pour
prédire un nombre.

**Et ce n'est pas la géométrie de la croix qui est en cause, c'est la hauteur.**
`rand-mrv` — un tirage aléatoire de même densité, à la même hauteur — ferme 86
racines sur 200, c'est-à-dire **encore moins**. Contraindre fortement l'ordre
des cases est mauvais en soi ; le faire selon la croix n'est ni meilleur ni pire
que de le faire au hasard. C'est exactement ce que le contrôle du §6.2 devait
établir, et il l'établit.

### 7.4 Le seul signal de la campagne — et son contrôle de densité le dissout

`anti-mrv` (les cases HORS croix d'abord) est le seul bras à réduire les nœuds
de façon systématique : médiane 0,555, 106 victoires contre 50 défaites,
p < 0,001. Assez pour arrêter la campagne et regarder de plus près.

**La campagne, telle que le §6.2 la spécifiait, ne pouvait pas l'interpréter** :
son contrôle aléatoire était à la densité de la CROIX (88 cases sur 256), alors
qu'`anti-*` en marque 168. Opposer les deux confond « la géométrie compte » avec
« la densité compte ». Le §6.2 exigeait un contrôle « à même densité » — il
n'avait pas vu que deux bras de densités différentes exigent **deux** contrôles.
D'où la série D, et un bras de plus dans le banc (`randc-*`, tirage à la densité
du complément, graine décalée pour que les deux tirages ne soient pas l'un
inclus dans l'autre).

| Bras (série D, 200 racines) | Fermées | Médiane du ratio | Apparié V/D | p | µs/nœud |
|---|---|---|---|---|---|
| `mrv` | 199 | — | — | — | 0,803 |
| `anti-mrv` | 198 | 0,555 | 106/50 | 0,000 | 1,103 |
| `randc-mrv` (contrôle) | 183 | 0,590 | 96/66 | 0,022 | 1,245 |
| `anti-key` | 199 | 1,020 | 45/68 | 0,038 | 0,751 |
| `randc-key` (contrôle) | 199 | 0,998 | 64/61 | 0,86 | 0,753 |

**Le contrôle fait presque aussi bien que le bras.** Un masque ALÉATOIRE de 168
cases à la hauteur forte réduit les nœuds dans les mêmes proportions
(médiane 0,590 contre 0,555). Ce que mesure `anti-mrv` n'est donc pas la
croix : c'est **la densité et la hauteur**. Il reste un écart en sa faveur
(106/50 contre 96/66, et surtout 198 fermetures contre 183), mais pas de quoi
attribuer quoi que ce soit à la géométrie du séparateur.

Deux réserves qui achèvent de vider ce signal :

- **Il ne paie pas en temps.** `anti-mrv` coûte 1,103 µs/nœud contre 0,803 :
  22 % de nœuds en moins, 7 % de temps en **plus** (17,2 s contre 16,1 s). Une
  partie de ce surcoût est l'implémentation du banc (la seconde passe du §0),
  mais le §5.3 avait déjà établi qu'aucune forme « au-dessus de `count` » n'est
  gratuite. Le critère (c) échoue.
- **La population est censurée en sa faveur.** Les statistiques appariées ne
  portent que sur les racines fermées des DEUX côtés : les racines où le bras
  bute sur le plafond — c'est-à-dire celles où il est le plus mauvais — en sont
  exclues. Léger pour `anti-mrv` (2 non fermées contre 1), massif pour
  `randc-mrv` (17). C'est la leçon du §7.5 de
  [banc_resolution_clones.md](banc_resolution_clones.md), qui se repose ici à
  l'identique.

### 7.5 Trois leçons d'instrument, payées par cette campagne

- **Compter les racines où le bras change QUELQUE CHOSE.** La médiane du ratio
  vaut 1,000 dès que la majorité des racines sont des égalités exactes, et ne
  dit alors plus rien. `cross-key` ne touche que 232 racines sur 499 : c'est sur
  celles-là que tout se joue, et c'est ce dénominateur-là qu'il faut publier.
- **Seul le test des signes sur les paires fermées des deux côtés est robuste.**
  Sommes de nœuds et moyennes géométriques sont dominées par trois racines sur
  269 (§7.2). Sur une distribution à queue aussi lourde, elles pointent
  régulièrement dans la direction opposée au test apparié — et c'est ce dernier
  qui a raison.
- **Une seule graine de contrôle aléatoire ne conclut rien.** Sur les quatre
  graines de la série C, `rand-key` donne 170/149, 150/167, 142/150 et 157/157,
  moyennes géométriques de 0,839 à 1,154 : du bruit, dans les deux sens. Un
  contrôle jugé sur une graine aurait pu « prouver » à peu près n'importe quoi.
  Contrôle de cohérence de l'instrument au passage : `cross-key` rend exactement
  le même résultat aux quatre graines (232 racines changées, 113/119, 0,999) —
  il ne dépend pas du tirage, et l'instrument le confirme.

### 7.6 Décisions

| Bras | Décision | Critère §6.5 |
|---|---|---|
| `cross-key` | **Aucun changement.** | (1) échoue : 113/119, p = 0,74, sur 232 racines réellement modifiées, aucune bande de profondeur favorable. (2) passe (0,734 contre 0,738 µs/nœud) mais ne départage rien |
| `cross-mrv` | **Rejeté.** | (1) échoue catégoriquement : 94 fermetures sur 200 contre 199 |
| `anti-mrv` | **Non adopté**, consigné comme piste ouverte | (3) échoue : son contrôle de densité fait presque aussi bien. (2) échoue : +7 % de temps total |

**Le résultat est négatif pour la proposition, et il est net.** La croix
séparatrice ne vaut rien comme a priori d'ordre des variables : à la hauteur où
elle est gratuite, elle ne change rien (p = 0,74 sur 232 racines modifiées) ; à
la hauteur où elle contraint, elle détruit la capacité de fermeture — et pas
plus qu'un masque aléatoire de même densité, ce qui montre que la faute est à la
contrainte d'ordre, pas à la géométrie.

**Ce que la campagne ne dit PAS**, et qu'il serait tentant de lui faire dire :
elle ne montre pas que la décomposition en quatre régions est sans valeur. Elle
montre qu'**un biais d'ordre ne suffit pas à l'encaisser** — ce que le §8
annonçait déjà, et qui reste l'explication à retenir en premier.

**L'outillage, lui, reste.** Le second point d'entrée `ETII_BENCH_CELL_HOOKS`
rend désormais mesurable n'importe quel ordre de variables sur ce banc : c'est
l'axe que le §7.7 de [banc_resolution_clones.md](banc_resolution_clones.md)
listait comme non mesurable, et il ne l'est plus.

## 7 bis. Mesures — campagne « côté trouver » (2026-09-18)

### 7bis.1 Pourquoi une seconde campagne : le §7 mesurait le mauvais régime

L'objectif d'origine de la proposition n'était pas la réfutation. Il était
**d'atteindre les contraintes d'indice tôt** : la croix passe par les cinq
indices, donc une fois faite, les pièces restantes n'ont plus de contrainte
fixe à satisfaire. Le §7 ne pouvait pas en juger — il mesurait le coût de
fermeture de sous-arbres MORTS, sur des racines de ≥ 130 pièces où les indices
sont déjà rencontrés depuis longtemps.

**La prémisse, elle, est vraie, et mesurée sur le stock de production**
(32 480 possibilités ; les cinq indices sont posés dès la genèse par
`first_possibility` — 0 absent — mais leurs contraintes ne portent que sur les
20 cases voisines) :

| Pièces posées | Possibilités | Indices rencontrés | 5/5 rencontrés | Halo rempli |
|---|---|---|---|---|
| 0-20 | 4 376 | 0,91 / 5 | **0 %** | 1,0 / 20 |
| 41-60 | 1 335 | 2,75 / 5 | **0 %** | 7,0 / 20 |
| 81-100 | 893 | 3,76 / 5 | **0 %** | 12,1 / 20 |
| 121-140 | 9 082 | 4,15 / 5 | 14,7 % | 15,1 / 20 |
| 161-180 | 988 | 4,57 / 5 | 57,1 % | 16,9 / 20 |

**Aucune possibilité en dessous de 100 pièces posées n'a ses cinq indices
rencontrés.** La production les rencontre donc bien très tard, exactement comme
la proposition le soutenait.

**Et le bras minimal de cette idée n'est pas la croix.** Les 20 cases du halo
des indices sont toutes sur la croix (vérifié par test) : compléter la croix
consomme toutes les contraintes d'indice — mais en 88 cases là où 20 suffisent.
Le halo a en outre l'avantage d'être défini par l'INSTANCE et non par la
géométrie, donc il se transpose sans la distorsion de densité du §4.4.

### 7bis.2 Protocole

`bench_solve` sur clones à solution connue, deux cellules « accessibles » du
§6.4 de [banc_resolution_clones.md](banc_resolution_clones.md), 30 instances
chacune, plafond 20 000 000 de nœuds (≈ 40× la médiane de production), ordre
des valeurs tenu à `natural` pour ne faire varier qu'un axe. Neuf bras :
production, croix et halo aux deux hauteurs, et **un contrôle aléatoire par
densité et par hauteur** — la leçon du §7.4, appliquée cette fois d'avance.

### 7bis.3 Hauteur « -key » : rien ne bat la production, sur les deux cellules

| Bras | n10k14 résolus / moy. géo | apparié V/D | n12k22 résolus / moy. géo | apparié V/D |
|---|---|---|---|---|
| `natural` | 30/30 — 402 166 | — | 30/30 — 77 028 | — |
| `cross-key` | 30/30 — 458 351 | 9/21 | 30/30 — 114 033 | 3/27 |
| `halo-key` | 30/30 — 420 833 | 8/22 | 30/30 — 78 056 | 10/20 |
| `randc-key` (contrôle) | 30/30 — 536 935 | 2/28 | 30/30 — 100 339 | 4/26 |
| `randh-key` (contrôle) | 30/30 — 425 730 | 10/20 | 30/30 — 115 047 | 0/30 |

Les quatre bras **perdent la comparaison appariée sur les deux cellules**, et
les contrôles aléatoires sont dans la même fourchette que les bras géométriques.
`halo-key` est le moins mauvais (+5 % et +1,3 % de moyenne géométrique) sans
jamais gagner. Même verdict qu'au §7.2, dans un régime entièrement différent.

### 7bis.4 Hauteur « -mrv » : le bras MINIMAL est le PIRE

| Bras | n10k14 | n12k22 |
|---|---|---|
| `natural` | **30/30** | **30/30** |
| `cross-mrv` | 15/30 | 2/30 |
| `halo-mrv` | **0/30** | **0/30** |
| `randc-mrv` (contrôle) | 0/30 | 0/30 |
| `randh-mrv` (contrôle) | 0/30 | 0/30 |

Le résultat qui tranche la question, et qui n'était pas prévu : **forcer les 20
cases du halo est PIRE que forcer les 88 de la croix** — 0 instance résolue
contre 15. Le bras le plus économique en apparence est le plus cher en pratique,
et le mécanisme est exactement celui du §4.2 :

> Un indice est une pièce **isolée**. Tant que la région n'est pas arrivée, ses
> quatre voisines n'ont qu'**un seul côté contraint**, soit 47,4 candidats
> contre 4,33 pour deux côtés. `halo-mrv` n'a QUE ces 20 cases dans son
> ensemble de choix : il est obligé d'ouvrir une case mono-contrainte vingt fois
> de suite. `cross-mrv` a 88 cases, dont les coins et les bandes qui se
> contraignent mutuellement en avançant — il commence par les cases à deux côtés
> contraints et ne touche au halo que lorsqu'il est devenu bon marché.

Autrement dit, **le retard de la production à rencontrer les indices n'est pas
un oubli : c'est le prix qu'elle refuse de payer.** MRV diffère le halo jusqu'à
ce que la région arrivée le rende bi-contraint — et le tableau du §7bis.1 montre
précisément ce report en action, le halo se remplissant de 1,0 à 16,9 cases à
mesure que la région avance.

### 7bis.5 Décision

| Bras | Décision | Critère |
|---|---|---|
| `halo-key` | **Aucun changement.** | Perd l'apparié sur les deux cellules (8/22 et 10/20) sans jamais gagner |
| `halo-mrv` | **Rejeté.** | 0 instance résolue sur 60, contre 60/60 pour la production |
| `cross-key` / `cross-mrv` | Rejet du §7.6 **confirmé dans un second régime** | 9/21 et 3/27 ; 15/30 puis 2/30 |

**La question du §7bis.1 est donc tranchée, et négativement : confronter les
indices tôt ne fait pas trouver plus vite.** La prémisse était juste — la
production les rencontre très tard — mais la conclusion qu'on en tirait ne
l'est pas : les rencontrer tôt coûte plus cher que ce que leur élagage rapporte.
Ce n'est plus une conjecture, c'est mesuré dans les deux régimes que le dépôt
sait mesurer, avec des contrôles aléatoires appariés en densité dans les deux.

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
  constante (§4.4) — sans objet désormais, aucune campagne sur clones n'ayant
  eu lieu.
- **CLOS par le §7 bis** : « et si l'objectif était de rencontrer les indices
  tôt ? ». La prémisse est vraie et mesurée (aucune possibilité de production
  sous 100 pièces n'a ses cinq indices rencontrés), la conclusion est fausse
  (aucun bras ne gagne, et le bras minimal ne résout rien). Le mécanisme est
  acquis : une case voisine d'un indice ISOLÉ est mono-contrainte, donc à 47,4
  candidats — la production ne « rate » pas les indices, elle refuse de les
  payer au prix fort et attend que la région les rende bon marché.
- **Ouvert PAR la mesure : une restriction grossière de l'ensemble de choix de
  MRV réduit les nœuds de réfutation.** À la hauteur forte, un masque de 168
  cases sur 256 — la croix inversée comme un tirage aléatoire — divise la
  médiane du ratio par presque deux (0,555 et 0,590, §7.4). Ce n'est pas la
  géométrie, puisque le hasard y suffit ; ce n'est pas exploitable en l'état,
  puisque le coût par nœud mange le gain. Mais l'effet est net, il n'était pas
  prévu, et personne ne sait d'où il vient. La question — pourquoi
  *n'importe quelle* restriction à deux tiers du plateau aide-t-elle ? — n'est
  pas celle de ce document.
- **Le seuil de densité.** 88 cases : effondrement. 168 : gain en nœuds. Entre
  les deux, rien n'est mesuré.

## 9. Découpage en PR

| PR | Contenu | Risque | Verrou |
|---|---|---|---|
| 1 | **livrée** (cf. §0). Table de croix en compréhension + tests ; second point d'entrée (`ETII_BENCH_CELL_HOOKS`) ; les six bras dans `bench_refutation` ; seconde passe restreinte plutôt que second minimum sur `count` (§0) | moyen — le piège de la détection de case morte est silencieux | `etii_search.o` **octet pour octet identique** avec et sans le hook ✓ ; équivalence des deux balayages contrôlée **pour chaque bras** par l'auto-test du banc ✓ ; `make test` vert ✓ |
| 2 | **exécutée, §7.** Campagne sur stock de production ; contrôle de densité ajouté en cours de route | mesure | quatre séries appariées, nœuds **et** temps, fermetures comptées, censure déclarée |
| 3 | **exécutée, §7 bis.** Campagne « côté trouver » sur clones ; bras `halo-*` et axe d'ordre des cases ajoutés à `bench_solve` | mesure | 9 bras sur 2 cellules, contrôles aléatoires appariés en densité ET en hauteur, auto-test de l'instrument adapté (un bras d'ordre des cases n'est pas tenu au même nombre de nœuds sur une racine morte — seule sa FERMETURE l'est) |
| 4 | **sans objet** — rien à adopter | — | — |

PR1 ne touche aucun chemin de production. PR2 **a** conclu négativement — c'était
l'issue prévue, et le §8 disait déjà quelle explication retenir.

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

# Oracle CDCL : l'apprentissage de clauses a-t-il un plafond qui mérite un moteur ?

**Statut : mesuré, campagne close — résultat MIXTE, et la famille n'est PAS
close.** Aucune ligne de moteur de production n'a été écrite, et aucune n'est
proposée : ce document est une **mesure de plafond**, pas une proposition
d'implémentation.

Le critère de décision, annoncé avant toute lecture des chiffres (§1), était une
disjonction. **Son premier terme est vérifié** — les conflits CDCL sont une à
trois décades sous les nœuds DFS sur 25 des 28 racines dures tranchées ; **son
second ne l'est pas du tout** — le CDCL ne ferme **aucune** racine que le DFS
laisse ouverte à 4·10⁸ nœuds. En TEMPS, le DFS gagne la médiane dans les trois
régimes et 70 racines sur 79, et **aucune racine, nulle part, ne voit le CDCL
gagner un ordre de grandeur** (accélération maximale : 6,2×).

**Décision : ne pas implémenter de moteur à apprentissage, sans fermer la
famille** (§8). Au passage, la conclusion structurelle de §4.15 de
[elagage_recherche.md](elagage_recherche.md) — *« une branche meurt au placement
suivant, jamais à distance »* — est **réfutée dans le régime 100-120 pièces**,
où une preuve 100× plus courte existe.

Instrument livré : `tools/root_to_cnf.py`, `tests/bench/bench_cdcl.py`,
cible `make bench-cdcl` (hors `make test`).
Sorties brutes : `/home/xavier/etii_mesures_2026-09-18b/`.

---

## 1. La question, et pourquoi elle se pose maintenant

Le dépôt sait mesurer le **coût de réfutation** d'une racine : combien de nœuds
et de secondes son moteur DFS dépense à prouver qu'un sous-arbre ne contient
aucune solution (`make bench-refutation`, [tests_et_ci.md](../tests_et_ci.md)).
Il ne sait pas dire si ce coût est **intrinsèque à l'instance** ou **propre à sa
famille d'inférence**.

Deux résidus le laissaient ouvert :

- [elagage_recherche.md](elagage_recherche.md) **§4.11** a écarté le
  backjumping dirigé par conflit sur une mesure sans appel — sous MRV, le saut
  optimal remonte au niveau immédiatement précédent dans **100,00 %** des
  152 128 détections observées : « une case ne meurt qu'au dernier placement ».
  Mais le même paragraphe laisse explicitement non mesurée **« la propagation
  des ensembles de conflit »** : le saut immédiat est nul, l'**apprentissage**
  de ce que le conflit enseigne aux autres branches ne l'est pas forcément.
- **§4.15** (Hall complet + comptage couleur à chaque nœud) a plafonné le gain
  d'une inférence locale plus riche à **0,35 %** des nœuds sur les racines
  ≥ 130 pièces, avec la même explication structurelle : *« une branche meurt au
  placement suivant, jamais à distance »*.

Ces deux verdicts portent sur des mécanismes **sans mémoire**. Le CDCL est la
famille qui en a une : à chaque conflit il dérive un **nogood** et le garde,
donc il peut, en principe, tuer *à distance* une branche dont la contradiction a
déjà été payée ailleurs. Et le dépôt observe un phénomène qui ressemble
exactement à ce que l'apprentissage saurait exploiter — le **« point de chute »**
(`tests_et_ci.md`) : avant de réfuter une racine de 139 pièces, le moteur
descend jusqu'à ~155-160 pièces posées, soit une excursion d'une vingtaine de
pièces, répétée branche après branche.

**Si la chute est intrinsèque**, le DFS ne peut rien y faire et la famille se
ferme. **Si elle est un artefact de l'absence de mémoire**, un moteur à nogoods
la raccourcirait, et la famille mérite un projet.

La façon honnête de trancher sans écrire de moteur est d'aller chercher le
meilleur moteur à apprentissage existant et de lui donner **les mêmes racines** :
c'est un **oracle**, au sens « plafond de ce qu'une autre famille obtiendrait ».

### Critère de décision, annoncé AVANT de lire les chiffres

> La famille « apprentissage » mérite un moteur si **les conflits CDCL sont
> inférieurs d'au moins un ordre de grandeur aux nœuds DFS sur la majorité des
> racines dures**, OU si **le CDCL ferme des racines que le DFS laisse ouvertes
> à 4·10⁸ nœuds**. Sinon, famille close.

---

## 2. Montage

| Bras | Binaire | Rôle |
|---|---|---|
| DFS | `tests/bench/bench_refutation`, `master` du jour (`b600033`, §4.16 — dom/wdeg complet) | référence |
| CDCL principal | **kissat 4.0.4**, cloné de github.com/arminbiere/kissat, `./configure && make` | l'oracle |
| CDCL contrôle | **CaDiCaL 3.0.1**, même origine, même procédure | recoupement du verdict |
| CDCL contrôle 2 | kissat `--preprocess=false` | sépare **apprentissage** et **simplification** |

Machine : Linux/gcc, 4 cœurs. Aucun autre binaire téléchargé, aucune
installation système. **Le point de comparaison est le moteur COURANT** : §4.16
a été mergée le jour même et divise les nœuds de réfutation par 20 à 50 ; le
comparer aux chiffres d'avant aurait donné un avantage gratuit au CDCL.

### Appariement

Une racine est désignée par **(filtre de taille, rang)**, ce qui reproduit à
l'identique la sélection de `bench_refutation` : les paquets du `.back` sont lus
**dans l'ordre du fichier**, ceux qui passent `--min-pieces`/`--max-pieces` sont
retenus, et les `--max-roots` premiers joués, étiquetés `back#0`, `back#1`…
`tools/root_to_cnf.py --min-placed/--max-placed/--rank` applique la même règle :
le rang `k` d'une ligne de résultat **est** la ligne `back#k` du banc. La
comparaison est donc appariée racine par racine, jamais agrégée sur deux
populations différentes — le piège relevé au §4.13 de `elagage_recherche.md`.

---

## 3. L'encodage

`tools/root_to_cnf.py` produit le DIMACS CNF de « ce plateau partiel admet-il
une complétion ? ». L'encodage est le **compact de Heule** (M. Heule, *Solving
Edge-Matching Problems with Satisfiability Solvers*, SAT 2008) : ne jamais
comparer deux pièces voisines deux à deux, mais faire transiter l'appariement
par une **variable de couleur portée par l'arête**.

Pour une racine à `A` pièces posées sur le 16×16 : `E = 256 − A` cases vides et
`F = 256 − A` pièces libres (effectifs égaux par construction).

**Variables.** `x[case][pièce][rotation]` pour les triplets localement
compatibles, rotations symétriques dédoublonnées ; `e[arête][couleur]` pour les
seules arêtes **libres** (leurs deux cases vides) ; plus les auxiliaires de
l'encodage « au plus un » séquentiel de Sinz.

**Clauses.** Exactement une pièce par case vide ; exactement une case par pièce
libre ; exactement une couleur par arête libre ; et l'implication
`x[c][p][r] → e[arête][couleur présentée]` sur chacun des quatre côtés libres.
« Au plus un » est encodé par paires jusqu'à 20 littéraux et par Sinz au-delà —
les groupes « une pièce par case » comptent plusieurs centaines de littéraux, où
l'encodage par paires produirait des dizaines de millions de clauses.

**Absorbé par le filtre, donc sans variable ni clause :** une arête vers
l'extérieur du plateau est fixée à la couleur 0 (gris), une arête vers une case
déjà posée est fixée à la couleur de cette pièce. Le filtre est une **passe
unique**, délibérément : l'inférence est le travail du solveur, pas celui de
l'encodeur.

### Les conventions sont vérifiées, pas devinées

Trois d'entre elles se seraient trompées silencieusement :

- `data/pieces.csv` est `id top **left** bottom **right**` — c'est l'ordre que
  lit `read_parts` (`src/core/readdata.c`), et non le `id top right bottom left`
  que l'énoncé de la mission et `tools/validate_pieces.py` mentionnent. L'écart
  n'est qu'un miroir du plateau, mais il change chaque couleur d'arête.
- La rotation de `rotatePart` fait `top ← left` par quart de tour, donc
  `face[i] = orig[(i − r) % 4]`.
- `grid[x][y]` a `x` en **colonne**, `y` en **ligne** ; la case vide vaut `−2`,
  sinon elle contient `id + 256 × rotation` (`id_for_rotated_part`).

**Contrôle du décodage sur la totalité du stock** (`--check-back`, 32 480
paquets de `eternityII.back`) : **0 adjacence fausse, 0 incohérence de cadre,
0 doublon**, `b_faceused` en accord avec la grille partout, et les cinq indices
de `data/indices.csv` présents et corrects dans les 32 480 paquets. Une seule
convention fausse dans la chaîne aurait produit des milliers de violations.

### « Pas de gris sur une arête intérieure » est démontré, pas supposé

Le filtre interdit la couleur 0 sur une arête intérieure, ce qui restreint
l'espace **par rapport à ce que le moteur C accepterait sur un plateau partiel**.
C'est licite parce que la question porte sur les **complétions** : le jeu compte
4 pièces à deux faces grises et 4×(16−2) = 56 à une seule, soit 4×2 + 56 = 64
faces grises pour exactement 64 côtés tournés vers l'extérieur du plateau. Dans
toute grille complète chaque face grise est donc consommée par le cadre, et une
adjacence gris/gris intérieure est impossible. `structural_grey_check` vérifie
ce comptage sur le jeu de pièces effectivement chargé et **désactive le filtre**
s'il ne tient pas (`--allow-inner-grey` le force). Sur `data/pieces.csv` il
tient.

### Taille produite

| Racine | Variables | Clauses | Fichier |
|---|---|---|---|
| 139 pièces (`back#0` du filtre `=139`, index 630) | 55 824 | 196 982 | 3,0 Mo |
| régime A (≥ 130), min / méd / max | 21 940 / 55 590 / 80 970 | 78 880 / 197 000 / 286 000 | |

L'encodage lui-même coûte **0,34 à 0,85 s** par racine en Python pur (médiane
0,65 s) — à comparer aux **0,0007 s** que le DFS met en médiane à fermer ces
mêmes racines. Ce coût est réel et compte dans la lecture finale.

---

## 4. Le contrôle positif, avant toute mesure

Un UNSAT ne veut **rien** dire tant que l'encodage n'a pas trouvé un SAT là où
une solution existe : un encodage sur-contraint produit des UNSAT gratuits, et
toute la campagne aurait alors mesuré un bug. `--self-test` enchaîne donc trois
instances à solution connue, et **re-vérifie le modèle rendu pièce par pièce**
(une et une seule pièce par case, une et une seule case par pièce, les quatre
faces de chaque case contrôlées contre leurs voisines et contre le cadre) :

| Instance | Variables | Clauses | Statut | Modèle re-vérifié |
|---|---|---|---|---|
| `data/pieces16.csv`, plateau vide (4×4) | 180 | 988 | SAT | oui |
| clone 16×16 `tools/gen_clone.py`, racine de 139 pièces | 71 313 | 250 302 | SAT | oui |
| clone 16×16, racine de 120 pièces | 106 691 | 370 367 | SAT | oui |

Les deux clones sont le point important : **même chemin de code qu'une racine
réelle, à pleine taille, et aux deux profondeurs exactes des deux régimes de la
campagne**. `bench_cdcl.py` refuse de mesurer si ce contrôle échoue.

**Corollaire opérationnel** : sur une racine réelle, un SAT serait soit une
**solution du puzzle**, soit un bug d'encodage. Les deux instruments s'arrêtent,
sauvent le modèle et le passent au vérificateur. *Aucun SAT n'a été rendu.*

---

## 5. Échantillon

| Régime | Effectif | Construction |
|---|---|---|
| **A** | 50 racines ≥ 130 pièces (130 à 166) | les 500 premières racines ≥ 130 du stock, stratifiées en **10 strates de profondeur** d'effectif égal, 5 par strate à rangs régulièrement espacés — **aucun biais de coût** dans le choix |
| **B** | 30 racines de 100 à 120 pièces | les **13** que `master` laisse ouvertes à 2·10⁶ nœuds, plus les **17 fermées les plus coûteuses** |

Références DFS des deux populations complètes, sur `master` du jour :

| Population | Fermées | Nœuds (total) | Temps (total) | Chute moy. |
|---|---|---|---|---|
| 500 racines ≥ 130, plafond 2·10⁶ | **500 / 500** | 484 811 | **0,390 s** | 156,7 |
| 300 racines 100-120, plafond 2·10⁶ | **287 / 300** | 22 324 849 (appariées) | 37,11 s | 146,2 |
| 300 racines 100-120, plafond 4·10⁸ | **299 / 300** | 269 889 707 (appariées) | 529,06 s | 146,5 |

Les deux premières lignes reproduisent §4.16 à l'identique. La troisième est
neuve et compte pour le second volet du critère : **à 4·10⁸ nœuds, le DFS ne
laisse plus qu'UNE racine ouverte sur 300.**

---

## 6. Régime A — racines ≥ 130 pièces : le CDCL n'y apprend rien

50 racines, profondeurs 130 à 166. **kissat UNSAT 50/50, CaDiCaL d'accord 50/50,
aucun SAT, aucun désaccord.**

| | DFS (`master`) | kissat 4.0.4 |
|---|---|---|
| tranchées | 50 / 50 | 50 / 50 |
| coût, min / méd / max | 1 / **68** / 10 399 nœuds | 0 / **0** / 2 249 conflits |
| temps, méd | **0,0007 s** | 0,030 s |
| temps total | **0,034 s** | 3,24 s (+ 31,4 s d'encodage) |

- nœuds DFS / conflits CDCL : **méd 46,5** (q1 5,0 ; q3 137) — conflits ≤ nœuds/10
  sur **33/50**.
- temps DFS / temps CDCL : **méd 0,025** (q1 0,015 ; q3 0,050).
  **Le DFS est plus rapide sur 50 racines sur 50**, ~40× en médiane.
  CDCL ≥ 10× plus rapide : **0/50**.

**Le chiffre qui explique le régime : 30 racines sur 50 sont réfutées à ZÉRO
conflit** — et les mêmes 30 à zéro conflit avec `--preprocess=false`. Elles ne
sont donc closes ni par l'apprentissage ni par la simplification, mais par la
**propagation unitaire seule**. 12 racines sur 50 sont même insatisfiables *dès
l'encodage* (une case sans candidat) : c'est très exactement la condition du
forward-check, que le DFS voit en 1 ou 2 nœuds. **À cette profondeur, la famille
« apprentissage » ne se déclenche pas** — le ratio favorable en « nœuds contre
conflits » ne mesure pas un meilleur raisonnement, il mesure que le CDCL n'a
besoin d'aucun raisonnement.

## 7. Régime B — racines 100-120 pièces : là, l'apprentissage travaille

30 racines (13 ouvertes à 2·10⁶ + 17 fermées les plus coûteuses), profondeurs
101 à 117. CNF de 1,0·10⁵ à 1,7·10⁵ variables, 3,6·10⁵ à 5,7·10⁵ clauses.
**kissat : 28 UNSAT, 2 TIMEOUT à 600 s. CaDiCaL : 23 UNSAT, 7 TIMEOUT (plafond
de contrôle 60 s), zéro désaccord. Aucun SAT.**

### Table 1 — les 13 racines que le DFS laisse OUVERTES à 2·10⁶ nœuds

Le DFS est ici mesuré à **4·10⁸ nœuds** : comparer au temps censuré de 2·10⁶
serait comparer le CDCL à une mesure qui n'a pas fini.

| racine | pièces | DFS 4·10⁸ nœuds | DFS 4·10⁸ | conflits kissat | kissat | nœuds/confl. | gagnant |
|---|---|---|---|---|---|---|---|
| back#2 | 101 | 16 103 476 | 12,22 s | 500 289 | 77,67 s | 32 | DFS |
| back#6 | 104 | 103 384 009 | 84,42 s | 2 912 111 | 449,22 s | 36 | DFS |
| back#37 | 109 | 2 056 572 | 1,68 s | **726** | **0,27 s** | **2 833** | **CDCL** |
| back#59 | 110 | 37 426 993 | 28,54 s | *timeout* | > 600 s | — | DFS |
| back#80 | 113 | 5 971 020 | 4,54 s | 279 879 | 44,19 s | 21 | DFS |
| back#134 | 117 | 6 930 199 | 5,74 s | 181 927 | 28,87 s | 38 | DFS |
| back#139 | 117 | 2 437 929 | 1,93 s | 2 887 | 0,69 s | 844 | **CDCL** |
| back#181 | 101 | *budget 4·10⁸* | 314,79 s | *timeout* | > 600 s | — | *aucun* |
| back#182 | 101 | 52 223 870 | 40,88 s | 597 464 | 92,81 s | 87 | DFS |
| back#187 | 104 | 3 972 229 | 3,17 s | 31 861 | 6,53 s | 125 | DFS |
| back#200 | 107 | 2 346 783 | 1,92 s | 2 268 | 0,61 s | 1 035 | **CDCL** |
| back#211 | 108 | 3 180 725 | 2,59 s | 14 470 | 2,18 s | 220 | **CDCL** |
| back#257 | 112 | 11 531 053 | 8,75 s | 1 019 800 | 158,60 s | 11 | DFS |

- tranchées par kissat à 600 s : **11 / 13** — tranchées par le DFS à 4·10⁸ :
  **12 / 13**.
- **Racines fermées par le CDCL que le DFS ne ferme PAS à 4·10⁸ : 0.**
  La seule racine qui résiste au DFS (`back#181`) résiste aussi à kissat : elle
  est ouverte **des deux côtés**.
- nœuds DFS / conflits CDCL sur les 11 fermées des deux côtés :
  **méd 87** (min 11, max 2 833) — ≥ 10× sur **11/11**.
- temps : **DFS 8, CDCL 4** (une indécise).

### Table 2 — les 17 racines les plus coûteuses FERMÉES par le DFS à 2·10⁶

| racine | pièces | nœuds DFS | DFS | conflits kissat | kissat | nœuds/confl. | gagnant |
|---|---|---|---|---|---|---|---|
| back#137 | 117 | 1 869 015 | 1,440 s | 84 572 | 15,20 s | 22 | DFS |
| back#61 | 110 | 1 237 297 | 0,993 s | **609** | **0,27 s** | **2 032** | **CDCL** |
| back#201 | 107 | 1 124 348 | 0,720 s | 34 156 | 7,77 s | 33 | DFS |
| back#254 | 112 | 1 069 579 | 0,853 s | 228 272 | 34,18 s | 5 | DFS |
| back#189 | 105 | 1 000 117 | 0,774 s | 1 451 | 0,30 s | 689 | **CDCL** |
| back#42 | 109 | 977 297 | 0,780 s | 613 | 0,32 s | 1 594 | **CDCL** |
| back#60 | 110 | 936 082 | 0,742 s | 8 580 | 1,48 s | 109 | DFS |
| back#66 | 112 | 819 695 | 0,656 s | 189 300 | 26,57 s | 4 | DFS |
| back#186 | 104 | 808 370 | 0,619 s | 3 382 | 0,85 s | 239 | DFS |
| back#242 | 111 | 658 425 | 0,523 s | 8 183 | 1,34 s | 80 | DFS |
| back#277 | 114 | 641 753 | 0,471 s | 48 131 | 7,61 s | 13 | DFS |
| back#58 | 110 | 627 140 | 0,502 s | 5 887 | 0,97 s | 107 | DFS |
| back#243 | 111 | 554 972 | 0,439 s | 555 | 0,27 s | 1 000 | **CDCL** |
| back#291 | 115 | 551 961 | 0,459 s | 135 704 | 19,35 s | 4 | DFS |
| back#34 | 109 | 530 721 | 0,421 s | 758 | 0,34 s | 700 | **CDCL** |
| back#209 | 108 | 528 172 | 0,388 s | 47 583 | 8,54 s | 11 | DFS |
| back#114 | 115 | 512 430 | 0,409 s | 3 259 | 0,74 s | 157 | DFS |

- nœuds DFS / conflits CDCL : q1 **13** | **méd 107** | q3 **689** —
  conflits ≤ nœuds/10 sur **14 / 17**.
- temps DFS / temps CDCL : q1 0,062 | **méd 0,501** | q3 1,238 —
  **DFS 12, CDCL 5** ; CDCL ≥ 10× plus rapide : **0 / 17**.

### Table 3 — le rapport de TEMPS par dureté de racine

| Population | n | temps DFS / temps CDCL, méd | min | max | CDCL gagne |
|---|---|---|---|---|---|
| A : ≥ 130 pièces | 50 | **0,025** | 0,002 | 0,167 | 0 |
| B : 100-120, fermées à 2·10⁶ | 17 | **0,501** | 0,024 | 3,678 | 5 |
| B : 100-120, ouvertes à 2·10⁶ (DFS à 4·10⁸) | 11 | **0,441** | 0,055 | 6,211 | 4 |

L'avantage du DFS s'effondre d'un facteur 20 entre A et B — puis **il cesse de
se réduire** : le rapport plafonne autour de 0,44-0,50, c'est-à-dire un DFS
encore ~2× plus rapide en médiane sur les racines les plus dures du stock.
**Aucun croisement n'est observé**, et **l'accélération CDCL maximale sur les
80 racines est de 6,2×** : aucune racine, dans aucun régime, ne voit le CDCL
gagner un ordre de grandeur en temps.

### Apprentissage ou simplification ? La variante `--preprocess=false`

- **Régime A** : 30 racines à zéro conflit avec préprocessing, **les mêmes 30
  sans**. Le préprocessing n'y est pour rien : c'est la propagation unitaire.
- **Régime B** : sur les 24 racines tranchées par les deux variantes, le rapport
  « conflits sans préprocessing / conflits avec » a une **médiane de 1,12**.
  Le préprocessing ne fait gagner qu'environ 12 % des conflits : **en régime B,
  le travail EST l'apprentissage**, et c'est bien la famille visée qu'on mesure.

---

## 8. Lecture, contre le critère annoncé au §1

Le critère était une disjonction. Il faut le lire terme à terme, et le résultat
n'est **pas** celui que les verdicts §4.11 et §4.15 laissaient attendre.

> **Terme 1 — « les conflits CDCL sont inférieurs d'au moins un ordre de grandeur
> aux nœuds DFS sur la majorité des racines dures » : VÉRIFIÉ.**
> Sur le régime B, 25 racines sur les 28 tranchées (14/17 fermées, 11/11
> ouvertes) ; médiane 107 sur les fermées, 87 sur les ouvertes, jusqu'à 2 833.

> **Terme 2 — « le CDCL ferme des racines que le DFS laisse ouvertes à 4·10⁸
> nœuds » : NON VÉRIFIÉ, et sans ambiguïté — 0 racine.**
> Le DFS ferme 299/300 racines du régime 100-120 à 4·10⁸ ; la seule qui résiste
> résiste aussi à kissat.

**Le critère est donc satisfait par son premier terme, et la famille n'est pas
close.** Il faut l'écrire tel quel : le critère avait été annoncé avant la
mesure précisément pour ne pas être réinterprété après.

### Ce que cela établit — et c'est un résultat neuf

**La « chute » n'est pas intrinsèque à l'instance.** Les racines de production
admettent des preuves de réfutation une à trois décades plus courtes que ce que
le DFS produit, et ces preuves sont réellement construites par une machinerie de
nogoods. §4.11 avait mesuré que le saut *immédiat* est nul à 100 % ; le résidu
qu'il laissait ouvert — la propagation des ensembles de conflit — **n'est pas
nul**, il vaut un facteur 100 en médiane sur le nombre d'inférences.

### Ce que cela n'établit pas

Un nombre d'inférences n'est pas un temps. Le DFS du dépôt fait **1,25·10⁶
nœuds/s** ; kissat fait ici de l'ordre de **6·10³ conflits/s** sur ces instances
— **un conflit coûte ~200 fois un nœud**. Le facteur 100 gagné en inférences est
donc consommé, et au-delà, par le coût de l'inférence. D'où les trois constats
qui, eux, sont sans appel :

1. **Le DFS gagne le temps en médiane dans tous les régimes** (0,025 ; 0,501 ;
   0,441), et gagne 70 racines sur 79 tranchées.
2. **Aucune racine, nulle part, ne voit le CDCL gagner 10× en temps** (max 6,2×).
3. **Le CDCL ne ferme rien que le DFS ne ferme.**

Et le coût de l'encodage, 1,26 s par racine en régime B, est à lui seul du même
ordre que la fermeture DFS complète (0,39 à 1,60 s). Bout à bout sur le régime B :
**522 s de DFS contre 2 188 s de solveur**, hors encodage.

### Décision

**Ne pas implémenter de moteur à apprentissage de clauses — mais ne pas fermer
la famille.** Les deux moitiés comptent :

- *Ne pas implémenter*, parce qu'un moteur maison devrait battre kissat 4.0.4
  d'un facteur ~200 sur le coût par conflit pour convertir l'avantage mesuré en
  avantage réel. Kissat est l'état de l'art d'un domaine entier ; ce n'est pas
  une cible pour ce dépôt, et ce serait parier sur le contraire exact de ce que
  la campagne mesure.
- *Ne pas fermer*, parce que le terme 1 du critère est vérifié et que la
  conclusion structurelle de §4.15 — *« une branche meurt au placement suivant,
  jamais à distance »* — vient d'être **réfutée dans le régime peu profond**.
  Elle reste vraie là où elle a été mesurée (racines ≥ 130 pièces, où 30/50
  racines tombent sur la seule propagation), et **fausse en 100-120 pièces**,
  où une preuve 100× plus courte existe et où l'apprentissage la trouve.

### Ce qui rouvrirait la piste, et ce qu'il faudrait mesurer d'abord

Par ordre de coût croissant, et **aucun de ces points n'est mesuré ici** :

1. **Un nogood n'a pas à coûter un conflit CDCL complet.** Le facteur 200 est
   celui de kissat, qui maintient des clauses apprises générales, des watched
   literals et une base de clauses à réduire. Une forme *restreinte* — mémoriser
   les seuls sous-ensembles de cases mortes, sans clause générale ni watcher —
   serait beaucoup moins chère, et beaucoup moins puissante. Le rapport des deux
   n'est pas connu.
2. **Le régime qui compte n'est pas forcément celui-ci.** L'avantage CDCL croît
   quand la racine est peu profonde. Le stock de production contient 5 % de
   possibilités sous 100 pièces, non mesurées ici.
3. **L'encodage est un paramètre, pas une donnée.** Tout le verdict en temps
   dépend du CNF produit. Un encodage différent (contraintes de cardinalité
   natives, `x[case][pièce]` séparé de la rotation) changerait les temps sans
   changer la question.

Une piste ne se rouvre qu'avec une mesure sur le point 1 : **combien coûte le
nogood le moins cher qui capture ce que kissat capture ?**

---

## 9. Ce qui n'a pas abouti, et les écarts de protocole

Rapporté intégralement, y compris ce qui affaiblit la campagne :

- **2 timeouts kissat à 600 s** en régime B : `back#59` (que le DFS ferme en
  28,5 s) et `back#181` (que le DFS ne ferme pas non plus à 4·10⁸). Aucun en
  régime A.
- **Les deux bras de contrôle du régime B tournent avec un plafond de 60 s, pas
  600 s.** Le bras principal saturait ses 600 s sur les racines les plus dures ;
  garder 600 s sur les trois bras coûtait 30 min par racine sans rien apprendre
  de plus. Conséquence assumée : **6 timeouts sur 30 pour `--preprocess=false`
  et 7 sur 30 pour CaDiCaL sont des timeouts À 60 s**, donc ces deux bras ne
  disent rien des racines correspondantes — en particulier le rapport
  « conflits sans préprocessing / avec » est calculé sur les **24** racines
  tranchées par les deux variantes, pas sur 30. Le bras principal, lui, a bien
  eu ses 600 s sur les 80 racines.
- **Aucune racine exclue** : 80 racines choisies, 80 mesurées.
- **Aucun désaccord kissat / CaDiCaL** sur les 73 racines tranchées par les deux.
- **Aucun SAT** : aucune solution, et aucun signe de bug d'encodage par cette
  voie.
- **Un défaut d'instrument, corrigé après coup.** Le pilote lisait le temps de
  kissat avec `^c process-time:\s+([0-9.]+) seconds`, qui rate la forme
  `c process-time:   1m 18s   77.67 seconds` — celle que kissat émet **au-delà
  de la minute**, c'est-à-dire exactement sur les racines qui comptent. Les logs
  bruts étant complets, la table finale est **reconstruite depuis les logs**
  (`scripts/reparse.py`) et non depuis la lecture en vol ; la même passe relue
  sur le régime A ne corrige **aucun** champ, ce qui vaut contrôle.
- **Le régime A a été joué deux fois.** La première exécution (trois bras à
  600 s) a été interrompue au début du régime B, quand le coût par racine s'est
  révélé rédhibitoire ; la seconde, complète, est celle qui est rapportée. Les
  50 lignes du régime A sont identiques entre les deux.
- **Contention CPU.** Le régime A a été mesuré pendant que le DFS à 4·10⁸
  occupait un cœur (4 cœurs, 2 processus). Sans effet sur un verdict à 40× ;
  le **régime B, lui, a été joué seul et strictement séquentiellement**.
- **Le régime B a fini en ~3 h, non en ~6 h** comme le laissait craindre la
  première racine. Les 13 racines *ouvertes* ont été jouées en tête
  d'échantillon et coûtent des minutes chacune ; les 17 racines *fermées* qui
  suivent coûtent 0,27 à 34 s de kissat. Le coût par racine s'effondre donc en
  cours de campagne — ce n'est pas une accélération de la machine, c'est l'ordre
  de l'échantillon.

---

## 10. Reproduction

```sh
# contrôle positif, puis contrôle du décodage du stock
python3 tools/root_to_cnf.py --self-test --solver <kissat>
python3 tools/root_to_cnf.py --check-back eternityII.back

# bras DFS (les trois populations de référence)
make bench-refutation BENCH_REFUT_ARGS="--from-back eternityII.back \
    --min-pieces 130 --max-roots 500 --budget 2000000 --engines mrv"
make bench-refutation BENCH_REFUT_ARGS="--from-back eternityII.back \
    --min-pieces 100 --max-pieces 120 --max-roots 300 --budget 2000000 --engines mrv"
make bench-refutation BENCH_REFUT_ARGS="--from-back eternityII.back \
    --min-pieces 100 --max-pieces 120 --max-roots 300 --budget 400000000 --engines mrv"

# campagne (scripts sous ~/etii_mesures_2026-09-18b/, cf. son README)
python3 scripts/campaign.py sample
python3 scripts/campaign.py run only=A out=results_A.csv
python3 scripts/campaign.py run only=B out=results_B.csv t_ctrl=60
python3 scripts/reparse.py results/results_B.csv
python3 scripts/report.py results/results_B.csv "régime B"

# le même instrument, en petit et versionné dans le dépôt
KISSAT=<kissat> make bench-cdcl BENCH_CDCL_ARGS="--min-pieces 100 --max-pieces 120 --roots 10"
```

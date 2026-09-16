# Banc « côté trouver » : clones d'Eternity II à solution connue

**Statut : en cours — PR1 à PR3 livrées, PR4 (calibration) mesurée et consignée
au §6, PR5 (campagne de politiques) au §7.** Suite 3 de la campagne du 2026-09-16 (§4.14/§4.15 de
[elagage_recherche.md](elagage_recherche.md)) : après le départage appris, la seule
grandeur que le projet ne sait pas encore mesurer est le temps pour **trouver** la
solution, par opposition au coût pour **réfuter** un sous-arbre.

L'outillage existe désormais et est documenté dans `docs/` — ce document ne garde que
le raisonnement, les arbitrages et les écarts constatés à l'implémentation. Référence
d'usage : [tests_et_ci.md](../tests_et_ci.md#banc-de-résolution--clones-à-solution-connue-make-bench-solve)
et [compilation.md](../compilation.md#tailles-supportées).

## 0. État de l'implémentation

| PR | État | Ce qui a été livré |
|---|---|---|
| 1 | **livrée** | `ETERN_SIZE` dérivé d'`ETERN_PARTS` pour 16/64/100/144/196/256 (toute autre valeur : `#error`), `FACES_USED_SIZE` en expression unique, parcours `directions[]`/`dirx[]`/`diry[]` construit à l'exécution pour les tailles de clone, genèse branchée sur `indices_file != NULL` (runtime, plus `#if ETERN_PARTS == 256`), option `--indices-file`, bornes en dur levées, taille 100 ajoutée à la matrice `WERROR=1` de la CI |
| 2 | **livrée** | `tools/gen_clone.py` : tirage équilibré, rejet des doublons et des pièces symétriques, indices aux positions relatives officielles, auto-contrôle par ré-assemblage **depuis les fichiers produits**, passage par `validate_pieces.py` |
| 3 | **livrée** | `tests/bench/bench_solve.c`, cible `make bench-solve`, hook `ETII_BENCH_HOOKS` d'ordre des valeurs, cœur pur `bench_solve_stats.{h,c}` testé dans `make test`, auto-test de l'instrument |
| 4 | **mesurée** (§6) | Calibration : **aucune des deux familles du §3.2 ne donne deux tailles exploitables**, et le comptage de l'annexe n'est pas une échelle de dureté transposable d'une taille à l'autre. Quatre cellules retenues à la place, calibrées par le nombre de couleurs |
| 5 | **mesurée** (§7) | Campagne de politiques sur les quatre cellules (60 instances chacune) |

**Verrou de PR1 tenu** : un clone 10×10 à 17 couleurs et 5 indices est résolu de bout en
bout par `./eternityII test … --indices-file … --stop-on-solution`, et le plateau trouvé
est **exactement** la solution plantée (100 cases sur 100, pièce et rotation).

**Verrou de PR3 tenu, et plus fort que prévu** : le document annonçait « même nombre de
nœuds à politique *production* qu'un `./eternityII test` sur la même instance ». Cette
égalité-là n'est **pas** vérifiable telle quelle — `./eternityII test` consomme ses
racines depuis les files du datamanager (round-robin entre fichiers), le banc les
consomme dans l'ordre de la genèse : le total dépend de *quelles* racines ont été
explorées avant celle qui porte la solution, pas seulement de l'arbre par racine. Deux
contrôles la remplacent, tous deux plus directs :

1. `build/core/etii_search.o` compilé avant et après l'ajout du hook est **octet pour
   octet identique** — la production ne voit strictement rien du banc.
2. L'**auto-test de l'instrument** rejoue une racine MORTE sous toutes les politiques et
   exige des comptes de nœuds identiques au nœud près. C'est la prémisse même du banc
   (§1) retournée en test : un désaccord signalerait une permutation fausse, pas une
   politique gagnante.

## 0 bis. Écarts assumés par rapport au plan

- **Un seul point d'entrée dans le moteur, pas deux.** §3.3 en prévoyait un à
  l'ouverture d'un niveau (ordre des valeurs) *et* un à la genèse (racine). Seul le
  premier est dans `etii_search.c` : la racine est construite **par le banc**, qui est
  mono-racine par construction (§4) et n'a donc pas besoin que le moteur l'aide. Moins
  de code sous `#ifdef` dans la boucle chaude, à fonctionnalité identique.
- **Les politiques de racine sont des choix de CASE à développer**, pas des plateaux
  pré-remplis. « Centre d'abord » développe la case vide la plus proche du centre,
  « bordure d'abord » le premier coin vide : tous les candidats de cette case deviennent
  des racines, donc l'espace exploré reste **le même**, seul l'ordre de visite change.
  Un plateau pré-rempli d'un choix particulier, lui, amputerait l'espace — et le seul
  moyen de choisir « le bon » préfixe serait de lire la solution plantée, c'est-à-dire
  de tricher.
- **La formule de redémarrage de §3.4 est corrigée.** Le document écrivait
  `E[coût] = (c + E[N | N < c]) / P(N < c)`. Cette forme charge le seuil `c` sur la
  tentative *gagnante* — qui s'arrête pourtant avant de l'atteindre, par définition — et
  divise en plus son coût propre par `p`. Les deux erreurs vont dans le même sens : elle
  **surestime** le coût du redémarrage, donc en sous-estime l'intérêt. Implémentée :
  `E[coût] = (1 − p)/p × c + E[N | N < c]`, la forme de la littérature (nombre de
  tentatives géométrique, les infructueuses seules coûtant `c`).
- **Les deux points laissés ouverts de §5 sur les pièces sont tranchés** :
  `data/pieces.csv` n'a **aucun** doublon (à rotation près) ni **aucune** pièce à
  symétrie de rotation — vérifié par comptage. Le générateur impose donc les deux.
- **Le seuil d'un redémarrage est strict** (`N < c`, pas `N ≤ c`) : une exécution qui
  coûte exactement `c` aurait été coupée juste avant d'aboutir.

## 1. Question posée

> Quel ordre des valeurs (quelle pièce essayer d'abord sur une case), quel point de
> départ et quelle politique de diversification atteignent la solution le plus tôt ?

Tous les instruments du dépôt mesurent la réfutation (`bench_refutation`, coût de
fermeture d'un sous-arbre mort) ou le débit (`bench_search.sh`, nœuds/s depuis la
genèse, avec `max_result` en garde-fou). Aucun ne peut voir l'ordre des valeurs : dans
un sous-arbre mort, **tous** les fils sont explorés quelle que soit leur ordre, donc le
compte de nœuds d'une réfutation n'en dépend pas. Or c'est précisément l'ordre des
valeurs, et le choix de la racine, qui décident *à quel moment* la branche portant la
solution est atteinte. Le puzzle réel n'ayant jamais été résolu, rien ne permet de
mesurer ce côté-là sur `data/pieces.csv`.

La solution d'Eternity II est de plus quasi unique. Estimation par comptage (voir
annexe) du nombre attendu de solutions **accidentelles** d'une instance aléatoire aux
statistiques de `pieces.csv` (16×16, 17 couleurs intérieures, 5 de cadre) :
≈ 10¹ sans les indices, ≈ 10⁻¹³ avec les 5 indices officiels. C'est cohérent avec ce
que le concepteur annonçait (les indices ont été ajoutés pour rendre la solution
unique). Conséquence : la recherche complète ne peut pas s'appuyer sur la multiplicité
des solutions ; elle est une course vers une branche unique, et cette course n'est
mesurable que sur des instances **construites** autour d'une solution plantée.

## 2. Contraintes du code existant

| Point | État | Conséquence pour ce plan |
|---|---|---|
| Taille du plateau | `ETERN_PARTS` compilé, deux valeurs seulement (256 → 16×16, sinon 4×4), `core/core_static_variables.h` | il faut dériver `ETERN_SIZE`/`FACES_USED_SIZE` pour 64, 100, 144, 196 |
| `directions[]`/`dirx[]`/`diry[]` | tableaux en dur pour 256 et 16 | simple énumération depuis VERSION 13 : un ordre ligne par ligne suffit pour les nouvelles tailles |
| Genèse (`first_possibility`) | `#if ETERN_PARTS == 256` lit `indices_file`, sinon pas d'indices | brancher sur `ETERN_WITH_INDICES` et un fichier d'indices par instance |
| Bornes en dur | `datamanager.c:632` (`x > 16`), `part.c:413` (`p.id > 256`) | à exprimer en `ETERN_SIZE`/`ETERN_PARTS` |
| Arrêt sur solution | `record_solution` fait `exit()` sous `stop_on_solution` ; `counters[]` porte le compte de nœuds | un banc **en processus fils** récupère les nœuds par un `atexit` sans toucher au moteur |
| Fichiers `solution_<pid>_<seq>` | écrits par `log_solution` dans le répertoire courant | le fils tourne dans un répertoire temporaire |
| Interrupteurs de mesure | règle du dépôt : pas de drapeau runtime inutilisé en production (§6 de `mrv_moteur_unique.md`) | les variantes vivent sous `#ifdef ETII_BENCH_HOOKS`, définie par le seul banc qui inclut `etii_search.c` — coût nul en production, même discipline que `ETII_ARENA_ORDER` (§4.8) |

## 3. Conception

### 3.1 Générateur de clones — `tools/gen_clone.py`

Entrées : `--size n`, `--inner-colours k`, `--frame-colours m`, `--seed`, `--hints`
(0 ou 5). Sortie : `pieces_<n>_<k>_<seed>.csv` (format de `data/pieces.csv`),
`indices_<…>.csv` (format de `data/indices.csv`) et `solution_<…>.txt` (la grille
plantée, pour contrôle).

1. Tirer une couleur sur chaque arête intérieure de la grille n×n : couleurs de cadre
   sur les arêtes entre deux cases de bordure, couleurs intérieures sur les autres, gris
   (0) sur le pourtour. **Numérotation retenue à l'implémentation** : intérieures
   `1..k`, cadre `k+1..k+m` — celle de `pieces.csv` (intérieures 1–17, cadre 18–22), et
   non le `1..m` réservé au cadre qu'écrivait ce plan : un clone doit être
   statistiquement indiscernable de l'original, y compris sur l'indexation. Le tirage
   suit l'histogramme de `pieces.csv` mis à l'échelle (cadre uniforme, intérieur
   quasi uniforme : 48 à 50 demi-arêtes par couleur sur 256 pièces) — pas un uniforme
   naïf.
2. Découper en pièces, appliquer une rotation aléatoire et une permutation aléatoire
   des identifiants. La solution existe par construction.
3. Rejeter et retirer les instances à pièces dupliquées (`pieces.csv` n'en a aucune, à
   vérifier une fois par `tools/validate_pieces.py`, qui sert aussi de contrôle de
   format sur chaque clone produit).
4. Indices : les 5 mêmes positions relatives que les officiels — `(2,2)`, `(n−3,2)`,
   `(2,n−3)`, `(n−3,n−3)` et le centre — avec la pièce et la rotation lues dans la
   solution plantée.
5. Auto-contrôle : ré-assembler la solution depuis le CSV et vérifier chaque arête.

Python plutôt que C : l'outil n'est jamais sur le chemin de production, et
`validate_pieces.py` établit déjà ce précédent.

### 3.2 Calibrer la taille : deux familles d'instances, pas une

Une instance 16×16 clone est aussi dure que le puzzle réel — inutilisable comme banc.
Il faut des tailles où la solution est atteinte en secondes ou minutes, tout en gardant
un régime de recherche comparable. Deux façons de réduire `n`, à mesurer toutes deux :

| Famille | Choix de `k` | Ce qu'elle conserve | Ce qu'elle perd |
|---|---|---|---|
| **A — mêmes couleurs** | `k = 17`, `m = 5` | les statistiques de compartiments (candidats par clé) | la dureté relative : l'instance est bien plus surcontrainte (log₁₀ E ≈ −38 à n = 10 contre +1,2 à n = 16), l'arbre meurt plus tôt |
| **B — même dureté** | `k*(n)` tel que log₁₀ E ≈ +1,2 comme le 16×16 | la position du « seuil » (branchement qui croise 1 à la même fraction de la profondeur) | les tailles de compartiments |

Valeurs de `k*` par le comptage de l'annexe (m = 5) : n = 8 → 6, n = 10 → 9, n = 12 →
12, n = 14 → 14, n = 16 → 17. Le comptage est un modèle (couleurs uniformes,
indépendance des arêtes) : il fixe un ordre de grandeur, la calibration réelle est
faite au banc (PR4) — on retient la taille où la médiane des nœuds jusqu'à la solution
tombe entre 10⁵ et 10⁷ avec le moteur actuel (secondes à minutes par instance), et on
ne fait confiance qu'aux classements de politiques **identiques dans les deux
familles et sur deux tailles**.

### 3.3 Le banc — `tests/bench/bench_solve.c`

Même squelette que `bench_refutation.c` (inclusion de `core/etii_search.c`, un
`client_possibility_t` local, `make bench-solve`), et un contrat aussi strict : ne rien
ajouter au chemin de production.

- **Une exécution = un processus fils** (`fork`) : le fils charge l'instance, pose la
  genèse (`first_possibility`, indices compris), positionne `stop_on_solution = 1`,
  enregistre un `atexit` qui écrit `counters[0]` (nœuds) et le temps sur un tube, puis
  appelle `search_packet_backtracking_mrv` avec un plafond de nœuds et sans
  délégation. Solution ⇒ `record_solution` sort par `exit()` et l'`atexit` rapporte ;
  plafond ⇒ le fils rapporte lui-même et sort. Le fils tourne dans un répertoire
  temporaire (fichiers `solution_*`). Aucune modification du moteur pour cela.
- **Grille de mesure** : instances × politiques × graines (pour les politiques
  aléatoires). Une ligne par exécution (`instance politique graine nœuds temps
  statut`), puis par politique : médiane et moyenne géométrique des nœuds (la
  distribution est à queue lourde, la moyenne arithmétique ne veut rien dire), part
  d'instances résolues à chaque plafond (courbe de survie à 10⁴, 10⁵, …, plafond), et
  comparaison **appariée** contre la politique de production (gagne / perd / égal par
  instance), exactement comme `compare.py` de la campagne §4.14.
- **Politiques** sélectionnées par option, implémentées dans le moteur sous
  `#ifdef ETII_BENCH_HOOKS` : un point d'entrée à l'ouverture d'un niveau (ordre des
  candidats d'un compartiment) et un à la genèse (racine). Rien de tout cela n'est
  compilé dans `eternityII`.
- Les parties pures (courbe de survie, moyenne géométrique, appariement, analyse de
  redémarrage ci-dessous) sont des fonctions sans E/S, testées dans `tests/bench/`
  comme `bench_lib.sh` l'est déjà.

### 3.4 Première campagne : ce qui est comparé

| Axe | Variantes | Pourquoi |
|---|---|---|
| Ordre des valeurs | `rare_first` (production, §4.8) ; `common_first` ; **valeur la moins contraignante** (dynamique : somme des candidats restants sur les voisines vides après pose, `popcount` sur les masques déjà en cache) ; la plus contraignante ; **aléatoire, 3 graines** | c'est l'axe aveugle de tous les bancs actuels ; le contrôle aléatoire est obligatoire — §4.14 a montré qu'un ordre « naturel » peut perdre contre le hasard |
| Départage des cases | positionnel ; appris (§4.14) ; aléatoire | connus pour la réfutation, inconnus pour la découverte |
| Point de départ | genèse actuelle (indices seuls) ; centre d'abord (racine pré-remplie autour de l'indice central) ; bordure d'abord (coins + premières pièces de bord) | réponse mesurée à la question « partir d'où ? », impossible à obtenir sur le puzzle réel |
| Redémarrages | **aucune implémentation** : la courbe de survie d'une politique aléatoire donne directement le coût attendu d'une stratégie de redémarrage à seuil `c` (`E[coût] = (1 − p)/p × c + E[nœuds \| résolu avant c]`, avec `p = P(résolu avant c)` — voir §0 bis, la forme initialement écrite ici était fausse), à comparer au coût sans redémarrage | si la distribution est à queue lourde, c'est le levier connu le plus fort de la littérature, et il correspond à ce que fait déjà `shallow_root_abandon_depth` et la répartition du stock entre clients |
| Indices | avec / sans | contrôle : sans indices la solution n'est plus unique (≈ 10 accidentelles attendues à n = 16) et les comparaisons doivent le refléter |

### 3.5 Règle de décision

Une politique n'est adoptée que si : (a) elle gagne en apparié sur ≥ 60 instances,
dans les **deux** familles d'instances et sur **deux** tailles, en médiane et en part
résolue au plafond ; (b) elle ne dégrade pas `bench_refutation` sur le stock de
production (pour l'ordre des valeurs c'est neutre par construction ; pour un départage
ou une racine, ce n'est pas garanti) ; (c) son coût par nœud est mesuré en temps
apparié alterné, comme en §4.12. Un classement qui change entre les familles A et B
n'est pas un résultat : c'est une propriété de la calibration, à consigner comme telle.

## 4. Arbitrages proposés

- **Tailles à la compilation**, un binaire par taille comme aujourd'hui (`ETERN_PARTS`
  ∈ {16, 64, 100, 144, 196, 256}), `ETERN_SIZE` et `FACES_USED_SIZE` dérivés. Pas de
  taille dynamique : `possibility_packet` et tous les caches sont dimensionnés par ces
  constantes, et rien dans ce plan n'a besoin de deux tailles dans un même processus.
- **Aucun bump de `VERSION`** : le format du paquet dépend déjà de `ETERN_PARTS`
  (c'est le cas du 4×4), un clone n'est jamais servi par un serveur de production.
- **Processus fils par exécution** plutôt qu'un retour `BT_CORE_SOLVED` ajouté au
  moteur : zéro changement dans la boucle chaude, isolation de la queue lourde, et le
  chemin `stop_on_solution` réel est exercé tel quel.
- **Variantes sous `#ifdef ETII_BENCH_HOOKS`**, jamais des globales runtime.
- **Générateur en Python**, pas en C.

## 5. Points laissés ouverts

- Les positions relatives des indices sur un petit plateau (`(2,2)` sur un 8×8 n'a pas
  le même rôle que sur un 16×16) ; à défaut, ne mesurer les indices qu'à n ≥ 10.
- ~~La règle « pas de pièce dupliquée » et l'absence de pièce à symétrie de rotation dans
  `pieces.csv` : à vérifier avant de les imposer au générateur.~~ **Tranché** : le
  comptage donne 0 doublon (à rotation près) et 0 pièce symétrique sur les 256 ; le
  générateur impose les deux et re-tire l'instance sinon.
- Le budget CPU réel : 60 instances × 6 politiques × plafond 5·10⁷ nœuds à 2 M nœuds/s
  font au plus 2,5 h par taille et par famille sur 4 cœurs ; la calibration (PR4) peut
  imposer un plafond plus bas.
- Une racine tirée d'un stock (travail délégué) plutôt que la genèse : hors de ce plan,
  le banc est mono-processus par construction.

## 6. Mesures — PR4, campagne de calibration (2026-09-16)

**Protocole.** Pour chaque cellule : 10 instances (graines 1 à 10), 5 indices,
politique de production (`natural`, racine `genesis`), plafond 5·10⁷ nœuds pour
le premier tableau et 2·10⁷ pour le balayage. Machine : i9-9880H, macOS/clang,
binaire `-O3`. Médiane calculée sur les seules exécutions résolues.

### 6.1 Les deux familles du §3.2, mesurées

| Cellule | n | k | log₁₀E (5 indices) | Résolues | Médiane nœuds |
|---|---|---|---|---|---|
| A8  | 8  | 17 | −45,3 | 10/10 | **60** |
| B8  | 8  | 6  | −7,3  | 3/10  | 4,0·10⁶ |
| A10 | 10 | 17 | −50,0 | 10/10 | 2,1·10⁴ |
| B10 | 10 | 9  | −10,2 | **0/10** | — |
| A12 | 12 | 17 | −47,0 | **0/10** | — |
| B12 | 12 | 12 | −13,7 | **0/10** | — |

**Aucune des deux familles ne donne deux tailles exploitables.** La famille A
passe de trivial (60 nœuds en 8×8 — MRV descend droit sur la solution) à hors
d'atteinte (12×12) en sautant par-dessus la fenêtre visée : la seule taille
utilisable, 10×10, y est encore sous la borne basse (2,1·10⁴ pour 10⁵ visés).
La famille B, elle, n'est jamais vraiment atteignable : marginalement en 8×8
(3 instances sur 10), plus du tout dès 10×10.

**La règle de décision du §3.5 — « gagner dans les deux familles et sur deux
tailles » — n'est donc pas satisfiable telle qu'elle est écrite.** Ce n'est pas
un défaut du banc : c'est le résultat que la calibration était chargée de
produire.

### 6.2 Le comptage de l'annexe n'est pas une échelle de difficulté

La famille B repose entièrement sur une équivalence : *même nombre attendu de
solutions accidentelles ⇒ même dureté*. La mesure la réfute directement — les
trois cellules construites pour être **également dures** ne le sont pas, et
l'écart n'est pas marginal :

| Cellule | log₁₀E (5 indices) | Résolues à 5·10⁷ |
|---|---|---|
| B8  | −7,3  | 3/10 |
| B10 | −10,2 | 0/10 |
| B12 | −13,7 | 0/10 |

Le balayage du §6.3 précise le diagnostic. **À taille fixée**, E ordonne
correctement : en 12×12, de k = 17 à k = 34, log₁₀E descend de −47 à −113 et la
médiane passe de « hors d'atteinte » à 5,3·10³ — plus l'instance est
surcontrainte, plus l'élagage mord, plus la recherche est courte. **Entre deux
tailles, l'ordre s'inverse** :

| Cellule | log₁₀E (5 indices) | Médiane nœuds |
|---|---|---|
| n10k17 | −50,0 | 2,1·10⁴ |
| n12k22 | −71,6 | 2,9·10⁵ (14× **plus** dur) |
| n10k14 | −37,8 | 7,4·10⁵ |
| n12k20 | −62,5 | 5,1·10⁶ (7× **plus** dur) |

Vingt et un ordres de grandeur d'E de moins, et une instance 14 fois plus dure.
E mesure une **densité de solutions** ; le coût de recherche, lui, dépend aussi
du nombre de pièces à poser, que E ne voit pas. Les deux grandeurs ne sont
comparables qu'à taille égale.

**Ce que l'annexe garde.** Sa conclusion du §1 — solution quasi unique,
≈ 10¹ solutions accidentelles sans les indices contre ≈ 10⁻¹³ avec — reste
valide : c'est un comptage, et c'est bien ce qu'elle sait faire. Seul son emploi
comme **échelle de dureté** (la définition de la famille B) est écarté.

### 6.3 Balayage du nombre de couleurs, à taille fixée

La taille est un levier trop grossier ; `k` en est un fin. Balayage à 10
instances, plafond 2·10⁷ :

| n | k | log₁₀E (5 indices) | Résolues | Médiane | Moy. géom. |
|---|---|---|---|---|---|
| 10 | 17 | −50,0 | 10/10 | 2,1·10⁴ | 2,0·10⁴ |
| 10 | **14** | −37,8 | **10/10** | **7,4·10⁵** | 6,7·10⁵ |
| 10 | **13** | −33,2 | 7/10 | **5,6·10⁶** | 3,6·10⁶ |
| 10 | 12 | −28,2 | 0/10 | — | — |
| 10 | 11 | −22,7 | 0/10 | — | — |
| 12 | 34 | −113,2 | 10/10 | 5,3·10³ | 9,2·10³ |
| 12 | 28 | −94,7 | 10/10 | 9,4·10³ | 1,1·10⁴ |
| 12 | 24 | −79,9 | 10/10 | 8,0·10⁴ | 5,1·10⁴ |
| 12 | **22** | −71,6 | **10/10** | **2,9·10⁵** | 2,5·10⁵ |
| 12 | **20** | −62,5 | **10/10** | **5,1·10⁶** | 4,1·10⁶ |

**La fenêtre 10⁵–10⁷ tient en un ou deux crans de couleur.** En 10×10 elle est
bornée par k = 14 et k = 13 : un cran au-dessus la médiane tombe à 2,1·10⁴, un
cran en dessous plus rien n'aboutit. Le régime change d'environ un ordre de
grandeur par couleur retirée — à comparer aux ~5 unités de log₁₀E que le même
cran déplace.

### 6.4 Cellules retenues pour la campagne de politiques

| Cellule | n | k | log₁₀E | Médiane | Rôle |
|---|---|---|---|---|---|
| `n10k14` | 10 | 14 | −37,8 | 7,4·10⁵ | 10×10, régime « accessible » |
| `n10k13` | 10 | 13 | −33,2 | 5,6·10⁶ | 10×10, régime « dur » |
| `n12k22` | 12 | 22 | −71,6 | 2,9·10⁵ | 12×12, régime « accessible » |
| `n12k20` | 12 | 20 | −62,5 | 5,1·10⁶ | 12×12, régime « dur » |

**Substitution assumée au §3.2**, et ce qu'elle coûte : les familles A (mêmes
couleurs) et B (même dureté) sont remplacées par une grille **deux tailles ×
deux régimes de dureté**, tous calibrés dans la fenêtre. L'intention du §3.2 est
préservée — un classement de politiques qui change d'une cellule à l'autre est
une propriété de la calibration, pas un résultat. Ce qui est perdu est explicite :
aucune cellule n'a les tailles de compartiments du vrai puzzle (c'était l'apport
de la famille A, qui exigeait k = 17), et aucune n'a sa dureté au sens du
comptage (c'était celui de la famille B, dont le §6.2 montre qu'il ne mesurait
pas ce qu'il prétendait). **Les conclusions de la campagne ne se transportent
donc au 16×16 qu'avec cette réserve**, et la robustesse d'un classement se juge
sur sa stabilité à travers les quatre cellules, pas sur une extrapolation.

## 7. Découpage en PR

| PR | Contenu | Risque | Livrable / verrou |
|---|---|---|---|
| 1 | **livrée.** Tailles génériques : dérivation de `ETERN_SIZE`/`FACES_USED_SIZE`, énumération ligne par ligne pour les nouvelles tailles, `first_possibility` sur la présence d'un fichier d'indices, les deux bornes en dur ; ajout d'une taille (100) à la matrice `WERROR=1` de la CI | moyen (les `16`/`256` implicites ne se voient qu'à la compilation et aux tests) | `make WERROR=1 CPPFLAGS=-DETERN_PARTS=100` vert ; suite de tests inchangée sur 16 et 256 |
| 2 | **livrée.** `tools/gen_clone.py` + auto-contrôle + passage par `validate_pieces.py` | faible | un clone 10×10 résolu de bout en bout par `./eternityII test` avec `--stop-on-solution` |
| 3 | **livrée.** `tests/bench/bench_solve.c`, cible `make bench-solve`, hook `ETII_BENCH_HOOKS` (ordre des valeurs ; la racine reste côté banc, cf. §0 bis), fonctions pures testées | moyen | `etii_search.o` octet pour octet identique avec et sans le hook ; auto-test de l'instrument sur racine morte ; `make test` inchangé (cf. §0) |
| 4 | Campagne de calibration : n ∈ {8, 10, 12}, familles A et B, moteur actuel ; choix des tailles de mesure, consigné ici | faible (mesure) | ce document passe en « en cours », section Mesures |
| 5 | Campagne de politiques (§3.4) et décisions ; toute adoption passe aussi par `bench_refutation` | mesure | §4.16 et suivants de `elagage_recherche.md` |

PR1 et PR2 sont indépendantes ; PR3 dépend des deux ; PR4 et PR5 sont des campagnes,
pas du code.

## Annexe — le comptage des solutions accidentelles

Pour une grille n×n aux couleurs tirées indépendamment (k intérieures, m de cadre),
le nombre attendu de solutions d'une instance aléatoire vaut, aux symétries près :

```
E = 4! · (4n−8)! · ((n−2)²)! · 4^((n−2)²) · m^−(4n−4) · k^−(2n(n−1) − (4n−4)) · (4(n−2)²)^−h
```

(coins, bords et intérieurs placés dans leurs zones respectives ; chaque arête
intérieure s'apparie avec probabilité 1/k, chaque arête de cadre 1/m ; `h` indices
imposent chacun une pièce et une rotation à une case). Pour `pieces.csv` :
log₁₀ E ≈ +1,2 sans indices, ≈ −13 avec les 5 indices. Le modèle ignore la
non-uniformité des couleurs et la dépendance entre arêtes : il sert à calibrer un ordre
de grandeur (§3.2), pas à prédire un nombre.

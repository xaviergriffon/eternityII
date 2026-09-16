# Banc « côté trouver » : clones d'Eternity II à solution connue

**Statut : implémenté — les 5 PR sont exécutées.** PR1-3 (outillage) livrées,
PR4 (calibration) au §6, PR5 (campagne de politiques) au §7. Le résultat
d'ensemble de PR5 est **négatif** : sur 2 640 exécutions et quatre régimes,
aucun ordre des valeurs ne bat celui de production, et le seul point de départ
réellement distinct du choix de MRV est nettement plus mauvais. Les pistes du
§3.4 sont fermées ; ce qui reste ouvert est listé au §7.7. Suite 3 de la campagne du 2026-09-16 (§4.14/§4.15 de
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
| 5 | **mesurée** (§7) | 2 640 exécutions sur les quatre cellules. Aucun changement adopté : les cinq ordres de valeurs sont à égalité en nœuds et perdent en temps (1,12× à 1,42× par nœud) ; `center` perd sur les quatre cellules ; `border` s'avère être la genèse de production elle-même ; sans indices, 0/240 instances aboutissent |

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

- ~~Les positions relatives des indices sur un petit plateau (`(2,2)` sur un 8×8 n'a pas
  le même rôle que sur un 16×16) ; à défaut, ne mesurer les indices qu'à n ≥ 10.~~
  **Sans objet en pratique** : le §6 a écarté n = 8 pour d'autres raisons (famille A
  triviale à 60 nœuds, famille B hors d'atteinte), donc toutes les cellules mesurées
  sont à n ≥ 10. Le générateur émet néanmoins l'avertissement.
- ~~La règle « pas de pièce dupliquée » et l'absence de pièce à symétrie de rotation dans
  `pieces.csv` : à vérifier avant de les imposer au générateur.~~ **Tranché** : le
  comptage donne 0 doublon (à rotation près) et 0 pièce symétrique sur les 256 ; le
  générateur impose les deux et re-tire l'instance sinon.
- ~~Le budget CPU réel : 60 instances × 6 politiques × plafond 5·10⁷ nœuds à 2 M nœuds/s
  font au plus 2,5 h par taille et par famille sur 4 cœurs ; la calibration (PR4) peut
  imposer un plafond plus bas.~~ **Mesuré** : la campagne complète (§7) a tenu en
  2 640 exécutions et environ 3 h sur un seul cœur, avec un plafond abaissé à 2·10⁷
  par la calibration. L'estimation était donc du bon ordre.
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

## 7. Mesures — PR5, campagne de politiques (2026-09-16)

**2 640 exécutions**, 4 cellules × 60 instances × 3 axes. Auto-test de
l'instrument à chaque instance : **183 concluants, 57 non concluants** (aucune
racine fermée dans le budget de 2·10⁶ nœuds), **0 échec** — les permutations
sont des permutations sur tout le matériel mesuré.

### 7.1 Protocole

60 instances par cellule (graines 1 à 60), plafond **2·10⁷ nœuds** par
exécution, cellules du §6.4. Trois axes, tous joués **sur les mêmes
instances** :

| Axe | Variantes | Référence |
|---|---|---|
| Ordre des valeurs | `natural` (production : l'ordre d'arène « rare d'abord », §4.8 de [elagage_recherche.md](elagage_recherche.md)), `reverse` (= *common_first*), `random` (3 graines), `lcv`, `mcv` | `natural` |
| Point de départ | `genesis` (production), `center`, `border` | `genesis` |
| Indices | 5 / aucun — **mêmes fichiers de pièces**, seul le fichier d'indices change (vérifié par `cmp`) | avec indices |

**Deux appariements, pas un.** Le §3.5 demande en (a) un gain apparié et en (c)
un coût par nœud en temps apparié alterné. Les deux sont rapportés côte à côte,
et c'est indispensable : sans la colonne « temps », `mcv` se lirait comme un
gain. L'alternance est structurelle — les politiques d'une même instance
s'exécutent à la suite dans le même processus, donc la dérive thermique les
touche également.

**Test des signes bilatéral** sur chaque comparaison. Sans lui, un 39/30 sur 60
instances se lirait comme un résultat alors que c'est du bruit (p = 0,33).

### 7.2 Axe 1 — ordre des valeurs : rien ne bat la production

Comparaisons appariées contre `natural` : *gagne/perd*, avec le p du test des
signes. « nœuds » est le critère (a), « temps » le critère (c).

| Cellule | Politique | Résolues | Médiane nœuds | Coût/nœud | Apparié nœuds | Apparié temps |
|---|---|---|---|---|---|---|
| `n10k14` | `natural` | 60/60 | 782 117 | 1,00× | référence | référence |
| | `reverse` | 60/60 | 833 784 | 1,04× | 96/84 (p=0,41) | 87/93 (p=0,71) |
| | `random` | 180/180 | 795 437 | 1,17× | 99/81 (p=0,21) | 45/132 (p<0,001) |
| | `lcv` | 60/60 | 945 563 | 1,39× | 84/96 (p=0,41) | 24/156 (p<0,001) |
| | `mcv` | 60/60 | 831 190 | 1,36× | 99/81 (p=0,21) | 18/162 (p<0,001) |
| `n12k22` | `natural` | 60/60 | 232 188 | 1,00× | référence | référence |
| | `reverse` | 60/60 | 252 258 | 1,05× | 87/93 (p=0,71) | 72/105 (p=0,016) |
| | `random` | 180/180 | 258 008 | 1,13× | 84/96 (p=0,41) | 54/125 (p<0,001) |
| | `lcv` | 60/60 | 246 690 | 1,31× | 84/96 (p=0,41) | 30/150 (p<0,001) |
| | `mcv` | 60/60 | 231 346 | 1,30× | 102/78 (p=0,086) | 36/144 (p<0,001) |
| `n10k13` | `natural` | 54/60 | 5 898 930 | 1,00× | référence | référence |
| | `reverse` | 53/60 | 7 787 013 | 1,05× | **57/96 (p=0,002)** | 45/108 (p<0,001) |
| | `random` | 161/180 | 6 468 091 | 1,19× | 78/80 (p=0,94) | 35/122 (p<0,001) |
| | `lcv` | 54/60 | 6 898 311 | 1,42× | **54/105 (p<0,001)** | 21/138 (p<0,001) |
| | `mcv` | 55/60 | 6 118 560 | 1,40× | 78/84 (p=0,70) | 12/150 (p<0,001) |
| `n12k20` | `natural` | 56/60 | 6 256 228 | 1,00× | référence | référence |
| | `reverse` | 56/60 | 6 312 912 | 1,03× | 84/81 (p=0,88) | 78/87 (p=0,53) |
| | `random` | 169/180 | 5 983 070 | 1,12× | 81/84 (p=0,88) | 55/110 (p<0,001) |
| | `lcv` | 54/60 | 6 140 350 | 1,29× | **57/105 (p<0,001)** | 27/135 (p<0,001) |
| | `mcv` | 58/60 | 5 889 922 | 1,27× | **99/69 (p=0,025)** | 39/129 (p<0,001) |

**Décision : aucune politique adoptée, l'ordre de production est conservé.**

- **`lcv` (valeur la moins contraignante) est rejetée franchement** : elle perd
  en nœuds sur les deux cellules dures (p < 0,001) et en temps sur les quatre.
  C'est l'échec le plus net de la campagne, et il était le moins attendu — la
  valeur la moins contraignante est l'heuristique de valeur classique de la
  littérature CSP.
- **`mcv` est le seul cas ambigu, et l'ambiguïté se tranche en temps.** Elle
  gagne en nœuds sur `n12k20` (p = 0,025) et frôle le seuil sur `n12k22`
  (p = 0,086), mais son coût par nœud est de 1,27× à 1,40× : en temps apparié
  elle perd 18/162, 36/144, 12/150, 39/129 — c'est-à-dire sur les quatre
  cellules, chaque fois à p < 0,001. Elle échoue donc au critère (c), et aussi
  à (a), qui exige le même classement sur les quatre cellules.
- **`reverse` (= *common_first*) ne gagne nulle part** et perd en nœuds sur
  `n10k13`. L'ordre d'arène « rare d'abord » (§4.8), adopté sur un critère de
  DÉBIT, n'est donc pas un mauvais choix pour la recherche non plus.
- **`random` est le contrôle du §3.4, et il fait son office** : à égalité en
  nœuds sur les quatre cellules. Le classement des politiques ordonnées n'est
  donc pas non plus un artefact — mais il n'y a rien à classer, tout est à
  égalité en nœuds.

**Ce que ça coûte de ne pas mesurer le temps.** En ne lisant que la colonne
« nœuds », `mcv` serait adoptée sur la foi de `n12k20`. Son surcoût par nœud
(1,27×) l'annule et au-delà. Le critère (c) du §3.5 n'est pas une formalité :
c'est lui qui tranche le seul cas ambigu de toute la campagne.

### 7.3 Axe 2 — point de départ : deux des trois « variantes » n'en sont qu'une

| Cellule | Variante | Résolues | Médiane nœuds | Apparié vs `genesis` |
|---|---|---|---|---|
| `n10k14` | `center` | 54/60 | 4 486 480 | 4/50 (p<0,001) |
| | `border` | 59/60 | 778 345 | 27/32 (p=0,60) |
| `n12k22` | `center` | 58/60 | 3 287 984 | 3/55 (p<0,001) |
| | `border` | 60/60 | 308 675 | 27/33 (p=0,52) |
| `n10k13` | `center` | **14/60** | 5 176 278 | 7/6 (47 indécis) |
| | `border` | 51/60 | 6 496 516 | 22/23 (p=1,00) |
| `n12k20` | `center` | **7/60** | 7 201 056 | 1/6 (53 indécis) |
| | `border` | 52/60 | 4 627 987 | 27/22 (p=0,57) |

**`center` est rejetée sur les quatre cellules**, par deux critères différents
selon le régime : en apparié sur les cellules accessibles (4/50 et 3/55, médiane
×6 à ×14), et par **effondrement de la part résolue au plafond** sur les
cellules dures (14/60 et 7/60 contre 54/60 et 56/60) — là où l'appariement reste
indécis faute de paires complètes. C'est exactement la raison pour laquelle le
§3.5 (a) demande les deux.

**`border` n'est pas une variante.** Le banc le dit lui-même : le rang de la
racine portant la solution est **complémentaire** entre `genesis` et `border` —
leur somme vaut 5 sur **208 paires résolues sur 213**. Autrement dit les deux
développent la MÊME case en QUATRE racines identiques, consommées dans l'ordre
exactement inverse. La raison est structurelle : avec les cinq indices posés, la
case la moins pourvue en candidats est un coin (quatre pièces de coin, une seule
rotation valide chacune), donc la genèse de production choisit déjà le coin que
`ROOT_BORDER` va chercher. « Bordure d'abord » mesure donc la genèse de
production avec les candidats en ordre inverse — un doublon de l'axe 1, dont le
résultat (indiscernable, p de 0,52 à 1,00) est cohérent avec celui de `reverse`.

Effet de bord utile : cette complémentarité **valide croisément la construction
de racines du banc** (`expand_at_cell`) contre celle de production
(`search_possiblity_light`) — mêmes racines, à l'ordre près.

**Ce que l'axe n'a donc pas testé**, et qu'il faudrait pour le faire : un point
de départ qui ne soit ni le choix de MRV ni le centre — une case de milieu de
bord, ou une case intérieure adjacente à un indice. En l'état, la seule chose
mesurée est que **s'écarter du choix de MRV coûte cher**, sur le seul écart
réellement testé.

### 7.4 Axe 3 — indices : sans eux, plus rien n'aboutit

| Cellule | Avec 5 indices | Sans indice |
|---|---|---|
| `n10k14` | 60/60 | **0/60** |
| `n12k22` | 60/60 | **0/60** |
| `n10k13` | 54/60 | **0/60** |
| `n12k20` | 56/60 | **0/60** |

Catégorique, sur 240 instances. Ce n'est pas un effet de densité de solutions :
sans indices, log₁₀E remonte (−37,8 → −25,8 sur `n10k14`) mais reste très
négatif, donc la solution plantée demeure essentiellement unique dans les deux
cas. Ce sont **cinq pièces de moins fixées**, donc un espace franchement plus
grand.

Conséquence pour la lecture du banc lui-même : **les clones ne sont mesurables
que parce qu'ils portent des indices**, exactement comme le puzzle réel ne l'est
qu'avec les siens. Toute campagne future « sans indices » exige un plafond d'un
autre ordre de grandeur, ou des instances recalibrées.

### 7.5 Redémarrages : tranché là où la distribution est observée, indécis ailleurs

Coût espéré d'une stratégie de redémarrage au seuil `c`, contre le coût espéré
sans redémarrage, sur la politique `random` (la seule pour laquelle relancer a
un sens — relancer une politique déterministe rejoue la même exécution) :

| Cellule | Seuil | P(succès) | E[coût] avec relance | E sans relance | Gain |
|---|---|---|---|---|---|
| `n10k14` | 10⁵ | 4 % | 2 535 615 | 1 100 425 | 0,43× |
| | 10⁶ | 59 % | 1 185 221 | 1 100 425 | 0,93× |
| | ≥ 10⁷ | 100 % | 1 100 425 | 1 100 425 | 1,00× |
| `n12k22` | 10⁵ | 15 % | 617 775 | 401 612 | 0,65× |
| | 10⁶ | 91 % | 411 307 | 401 612 | 0,98× |
| | ≥ 10⁷ | 100 % | 401 612 | 401 612 | 1,00× |
| `n10k13` | 10⁷ | 63 % | 10 391 377 | **≥** 8 667 272 | ≥ 0,83× |
| | 2·10⁷ | 89 % | 9 690 117 | **≥** 8 667 272 | ≥ 0,89× |
| `n12k20` | 10⁷ | 65 % | 9 891 366 | **≥** 8 534 221 | ≥ 0,86× |
| | 2·10⁷ | 94 % | 9 089 703 | **≥** 8 534 221 | ≥ 0,94× |

**Sur les deux cellules où TOUTE la distribution est observée** (100 % résolues
sous le plafond), la conclusion est ferme : **aucun seuil ne bat l'absence de
redémarrage.** À 10⁷ et au-delà plus aucune relance ne se déclenche, donc le
gain vaut exactement 1,00× ; en dessous, relancer ne fait qu'ajouter du coût.
Le §3.4 pariait que « si la distribution est à queue lourde, c'est le levier le
plus fort de la littérature » : sur ces instances-là elle ne l'est pas assez.

**Sur les deux cellules dures, ces données ne concluent pas**, et il faut le
dire : 6 à 11 % des exécutions butent sur le plafond, leur coût réel est inconnu
et supérieur à celui-ci, donc « E sans relance » n'est qu'une **borne
inférieure** — et le gain affiché aussi. Il suffirait que les exécutions
censurées coûtent en moyenne ~5·10⁷ nœuds pour que `n10k13` bascule en faveur du
redémarrage. **La censure joue précisément dans le sens qui favoriserait le
redémarrage** : conclure « le redémarrage ne sert jamais » à partir de ces deux
cellules serait une faute de lecture. Trancher demanderait un plafond
suffisamment haut pour que 100 % des instances aboutissent.

### 7.6 Décisions

| Axe | Décision | Critère §3.5 |
|---|---|---|
| Ordre des valeurs | **Aucun changement.** `natural` (ordre d'arène) conservé | (a) échoue pour toutes : aucune ne gagne en nœuds sur les 4 cellules. (c) échoue pour `random`/`lcv`/`mcv` : 1,12× à 1,42× par nœud |
| Point de départ | **Aucun changement.** Genèse MRV conservée | `center` perd sur les 4 cellules ; `border` est la genèse elle-même |
| Indices | **Sans objet** (propriété de l'instance, pas une politique) | — |
| Redémarrages | **Aucune implémentation**, conformément au §3.4 | Perdant là où c'est mesurable, indécis ailleurs |

Le critère (b) — « ne dégrade pas `bench_refutation` » — n'a été exercé pour
aucune de ces décisions, puisque aucune n'entraîne de changement de moteur. Il
resterait à passer avant toute adoption ultérieure. Pour l'ordre des valeurs il
est neutre **sous une condition qu'il faut énoncer** : que l'ordre des
VARIABLES ne dépende pas de l'ordre des VALEURS. C'est vrai du moteur mesuré
ici, et les 183 auto-tests concluants de cette campagne le vérifient
empiriquement — une racine MORTE y coûte le même nombre de nœuds sous les cinq
politiques. Ce n'est PAS une propriété générale : un départage de cases qui
apprend de la recherche (§4.14 de [elagage_recherche.md](elagage_recherche.md))
lie les deux ordres, et la même racine morte y ferme alors en 168 152 à 225 683
nœuds selon la politique. Sur un tel moteur, (b) redevient une vraie
obligation, et une comparaison de politiques mesure deux effets à la fois —
l'auto-test du banc le détecte et le dit désormais explicitement.

**Le résultat d'ensemble est négatif, et c'est un résultat.** Le §1 posait que
l'ordre des valeurs et le point de départ « décident à quel moment la branche
portant la solution est atteinte ». Sur 2 640 exécutions et quatre régimes, ils
ne le décident pas de façon exploitable : les cinq ordres de valeurs sont à
égalité en nœuds, et le seul point de départ réellement distinct du choix de MRV
est nettement plus mauvais. Ce que la campagne ferme, ce sont les pistes du
§3.4 ; ce qu'elle laisse ouvert est au §7.7.

### 7.7 Ce qui reste ouvert

- **Un point de départ réellement différent** (milieu de bord, case intérieure
  adjacente à un indice) — le §7.3 montre que l'axe n'a testé qu'un seul écart.
- **Les redémarrages sur distribution complète** : refaire le §7.5 avec un
  plafond où 100 % des instances aboutissent sur les cellules dures.
- **Le départage des cases** (positionnel / appris / aléatoire), troisième axe
  du §3.4 : il n'a pas été mesuré. Le départage MRV est une **clé composite
  calculée à la compilation** (`bt_nc_key`, cf. `docs/autosearch_step.md`), pas
  un point de variation ; le mesurer demanderait un second point d'entrée sous
  `ETII_BENCH_HOOKS`, que PR3 n'a pas livré et que ce document n'avait pas prévu
  à cet endroit.
- **La transposition au 16×16** : le §6.4 a consigné que les cellules retenues
  n'ont ni les tailles de compartiments ni la dureté du puzzle réel. Un
  classement stable sur quatre cellules n'est pas une preuve pour la vraie
  instance — ici la question se pose peu, le classement étant « tout à
  égalité », mais elle se reposera pour toute piste future.

## 8. Découpage en PR

| PR | Contenu | Risque | Livrable / verrou |
|---|---|---|---|
| 1 | **livrée.** Tailles génériques : dérivation de `ETERN_SIZE`/`FACES_USED_SIZE`, énumération ligne par ligne pour les nouvelles tailles, `first_possibility` sur la présence d'un fichier d'indices, les deux bornes en dur ; ajout d'une taille (100) à la matrice `WERROR=1` de la CI | moyen (les `16`/`256` implicites ne se voient qu'à la compilation et aux tests) | `make WERROR=1 CPPFLAGS=-DETERN_PARTS=100` vert ; suite de tests inchangée sur 16 et 256 |
| 2 | **livrée.** `tools/gen_clone.py` + auto-contrôle + passage par `validate_pieces.py` | faible | un clone 10×10 résolu de bout en bout par `./eternityII test` avec `--stop-on-solution` |
| 3 | **livrée.** `tests/bench/bench_solve.c`, cible `make bench-solve`, hook `ETII_BENCH_HOOKS` (ordre des valeurs ; la racine reste côté banc, cf. §0 bis), fonctions pures testées | moyen | `etii_search.o` octet pour octet identique avec et sans le hook ; auto-test de l'instrument sur racine morte ; `make test` inchangé (cf. §0) |
| 4 | **exécutée, §6.** Campagne de calibration : n ∈ {8, 10, 12}, familles A et B, moteur actuel ; choix des cellules de mesure, consigné ici | faible (mesure) | quatre cellules retenues, calibrées par le nombre de COULEURS — les familles A et B du §3.2 n'étant ni l'une ni l'autre exploitables sur deux tailles |
| 5 | **exécutée, §7.** Campagne de politiques (§3.4) et décisions ; toute adoption passe aussi par `bench_refutation` | mesure | 2 640 exécutions, aucun changement adopté ; conclusion reportée en §4.8 de `elagage_recherche.md` (l'ordre d'arène, adopté sur un critère de débit, tient aussi « côté trouver ») |

PR1 et PR2 sont indépendantes ; PR3 dépend des deux ; PR4 et PR5 sont des campagnes,
pas du code — à ceci près que PR5 a fait apparaître deux défauts de l'instrument,
corrigés en cours de route : l'auto-test écrivait ses fichiers de solution dans
le répertoire d'où le banc était lancé (il s'exécute dans le parent, qui n'avait
pas fait le `chdir` du fils), et le tableau des redémarrages comparait un coût
ESPÉRÉ à une MÉDIANE, ce qui chargeait systématiquement le redémarrage.

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

# Élagage de la recherche : pistes au-delà du forward-check

**Statut : en cours d'implémentation.** PR 1 (§4.1) livrée et mesurée — voir
[autosearch_step.md §1.3 ter](../autosearch_step.md#13-ter-bt_forward_check--les-voisines-de-la-pièce-posée-pas-une-fenêtre-de-parcours)
pour le comportement actuel. PR 2 (§4.4), PR 3 (§4.2, variante compteurs) et PR 4 (§4.3)
évaluées et **écartées** après mesure (code absent de `master`, raisonnement et chiffres
conservés dans chaque section — §4.3 pour une raison différente des deux autres : un
recoupement structurel avec le forward-check, pas une absence de déclenchement). PR 5
(§4.6a, point fixe du pruner) **livrée**. PR 6 (§4.6b, DFS à budget dans le pruner)
**implémentée et testée, mais désactivée par défaut** — mesurée sans aucun gain sur le
stock réel actuel, code conservé (opt-in), pour la même raison structurelle que §4.2/§4.4 :
voir §4.6b pour la mesure complète. PR 7 (§4.8, ordre des candidats dans l'arène)
**livrée** — comportement inconditionnel, aucun interrupteur laissé en place (comme §4.1).
Les pistes 8 et 9 (§4.5, §4.7), ainsi que la variante « partition de l'arène » de §4.2,
restent des propositions non implémentées.

## 1. Question posée

> Peut-on éliminer davantage de possibilités **avant** de développer tout leur sous-arbre ?

Réponse courte : oui. Le moteur n'exploite aujourd'hui qu'une seule famille d'élagage
(le forward-check de proximité), et il la fait porter sur les mauvaises cases. Trois
familles exactes supplémentaires sont accessibles, dont deux à coût quasi nul.

## 2. État des lieux

| Où | Quoi | Portée |
|---|---|---|
| [`bt_forward_check`](../../src/core/etii_search.c) | ≥1 candidat libre sur les `FORWARD_CHECK_K` (6) **prochaines cases du parcours `directions[]`** | boucle chaude, ~45,7 % d'élagage |
| [`forward_check_next_k`](../../src/core/possibility.c) | idem, sans le cache de contraintes | chemins froids (`bt_materialize_pending`, tests) |
| [`possibility_all_has_a_next_counted`](../../src/core/possibility.c) | balayage de **toutes** les cases restantes + placement des cases à candidat unique | pruner uniquement, **une seule passe** |
| clé 4D + wildcard ([`buildBigArray`](../../src/core/part.c)) | cohérence de couleur avec les voisins déjà posés | implicite, exacte, gratuite |

Deux angles morts structurels, tous deux mesurés en §3 : le forward-check n'inspecte pas
les cases dont la clé vient de changer, et **aucun raisonnement global** (comptage,
appariement, typage) n'existe — chaque case est jugée isolément.

## 3. Mesures de référence

Toutes obtenues par analyse statique de `dirx[]`/`diry[]` ([`static_variables.c`](../../src/app/static_variables.c))
et de [`data/pieces.csv`](../../data/pieces.csv) ; script reproductible en annexe. Aucune
n'exige de faire tourner le solveur.

### 3.1 Couverture du forward-check

| Grandeur | Valeur |
|---|---|
| Relations « case → voisine encore vide » dans le parcours | 480 |
| … couvertes par la fenêtre K=6 | **293 (61 %)** |
| Cases dont *toutes* les voisines futures sont dans la fenêtre | 108/256 (42 %) |
| Retard de détection d'une voisine hors fenêtre (médiane / max) | **8 / 152 niveaux** |
| Pire cas | index 103, case (8,1), voisine à +152 |
| Voisines futures par case (moyenne) | **1,9** (contre 6 cases inspectées) |

### 3.2 Structure du parcours

| Grandeur | Valeur |
|---|---|
| Largeur de frontière (cases vides adjacentes à une case posée), max / moyenne | 52 / 29 |
| Cases de cadre | 60, la dernière à l'index 74 |
| Cases intérieures posées **avant** la fin du cadre | 15 (index 3, 4, 16, 17, 22, 23, 39–42, 54, 55, …) |

### 3.3 Typage des pièces

| Grandeur | Valeur |
|---|---|
| Couleurs **exclusives** aux pièces de cadre | `0`, `18`, `19`, `20`, `21`, `22` |
| Pièces : coins (2 faces grises) / bords (1) / intérieures (0) | 4 / 56 / 196 |
| Part de candidats **de cadre** sur une case intérieure mono-contrainte | **2 % à 12 %** selon la couleur |

## 4. Pistes retenues

Chacune est une **condition nécessaire** : elle ne peut jamais rejeter une branche qui
mène à une solution. C'est le seul critère non négociable (§5).

### 4.1 Forward-check sur les voisines plutôt que sur la fenêtre de parcours — LIVRÉ

**Statut : implémenté.** Voir
[autosearch_step.md §1.3 ter](../autosearch_step.md#13-ter-bt_forward_check--les-voisines-de-la-pièce-posée-pas-une-fenêtre-de-parcours)
pour le comportement actuel ; ce qui suit reste la trace du raisonnement et de la mesure
qui ont tranché.

**Principe.** Poser une pièce en `c` ne modifie la clé que de ses **4 voisines**. Le FC
actuel inspecte `c+1 … c+6` dans l'ordre `directions[]`, qui n'est pas un ordre de
voisinage : d'après §3.1 il rate 39 % des cases dont la clé a effectivement changé, avec
un retard médian de 8 niveaux et jusqu'à 152.

**Coût.** *Négatif* : 1,9 case inspectée en moyenne au lieu de 6.
[`bt_propagate_place`](../../src/core/etii_search.c) énumère déjà exactement ces 4
voisines, et `constraints[x][y]` fournit leur clé sans recalcul — le lookup
`map_bucket_packed` est identique.

**Mesuré** (`tests/bench/bench_search.sh`, puzzle 256, A/B à ordre alterné pour neutraliser
la dérive thermique de la machine, 20 M nœuds × 5) :

| Configuration | Nœuds/s | Taux d'élagage |
|---|---|---|
| Avant (fenêtre `K=6`) | 5 867 621 | 45,7099 % |
| Après (voisines) | 9 906 019 | 45,6471 % |
| Delta | **+68,8 %** | −0,06 point |

**Variante prudente écartée.** L'hypothèse initiale envisageait `voisines ∪ c+1 … c+2`, au
cas où la fenêtre attraperait un second effet (une case lointaine perdant son dernier
candidat libre par consommation ailleurs). Mesuré : le taux d'élagage bouge de −0,06 point
seulement en passant aux voisines SEULES — cet effet est donc négligeable en pratique, pas
assez pour justifier le coût et la complexité de la variante mixte. **Décision : voisines
seules, pas de fenêtre résiduelle.**

**Impact statistique.** `fc_pruned_at[]` est réétiqueté « position dans la fenêtre
inspectée » (1..4 pour la boucle chaude, 1..`FORWARD_CHECK_K` pour le chemin froid
`forward_check_next_k`, resté sur l'ancienne sémantique de fenêtre) plutôt que « distance
de parcours ». Dimensionné sur `FC_STAT_MAX_K` (borne fixe, indépendante de
`FORWARD_CHECK_K`) pour rester sûr quel que soit le plus petit des deux domaines — voir le
commentaire de `fc_pruned_at` dans `static_variables.h`.

**Chemin froid non touché.** `forward_check_next_k` (`possibility.c`, matérialisation de
délégation + tests) garde sciemment l'ancienne sémantique de fenêtre : son contrat est
plus général (une fenêtre de parcours a un sens même sans pièce « juste posée », ex. à la
genèse), ses tests existants en dépendent, et il ne pèse pas sur le débit — voir la
docstring de `bt_forward_check` (`etii_search.c`) pour le partage explicite des
responsabilités entre les deux fonctions.

### 4.2 Contrainte de type coin / bord / intérieur — VARIANTE « COMPTEURS » ÉVALUÉE ET ÉCARTÉE

**Statut : la variante « compteurs » évaluée et abandonnée** (code absent de `master`) ;
la variante « partition de l'arène » reste une proposition non essayée — voir la décision
en fin de section. Même esprit que §4.4 : correct, testé, mesuré, reverté avant commit.

**Principe.** `buildBigArray` place **toutes** les pièces dans le compartiment wildcard :
rien n'interdit une pièce de cadre sur une case intérieure. Pire, le forward-check ne
peut pas rattraper l'erreur *même en inspectant la voisine* — poser une pièce de bord à
l'intérieur expose une couleur 18–22 vers une case vide, dont le compartiment n'est pas
vide (il contient les autres pièces de cadre), seulement inéligible à cette case.
L'erreur ne se révèle qu'au moment où une case de **cadre** ne trouve plus de candidat.

**Fenêtre de tir.** Le cadre est bouclé à l'index 74, donc au-delà les 60 pièces de cadre
sont toutes consommées et l'erreur est impossible. Seules les **15 cases intérieures
d'index < 74** sont concernées — mais elles sont tout en haut de l'arbre, là où un
sous-arbre condamné coûte le plus cher, et la détection n'est garantie qu'à la **dernière**
case de cadre (médiane 34 niveaux plus loin, max 71).

**Implémentation testée : compteurs, mais volontairement AFFAIBLIS.** L'idée d'origine
(« coins_libres/bords_libres » comptés en scannant `b_faceused` pour connaître le type de
chaque pièce déjà utilisée) exige d'interroger le type d'un `id` arbitraire via
`all_rotate_part` — sûr en production (toujours dimensionné à `ETERN_PARTS`), mais PAS
garanti par tous les fixtures de test existants (`make_small_parts()`, utilisé par une
dizaine de tests de `search_packet_backtracking`, n'a que 8 entrées avec un indexage qui
ne correspond pas à la convention `id`-comme-indice de `rotate_all_parts`). Plutôt que de
retoucher ce fixture partagé (risque de régression sur des tests qui n'ont rien à voir
avec cette piste), l'implémentation testée est **volontairement plus faible** : les
compteurs repartent de la capacité THÉORIQUE MAXIMALE (4 coins, `4×(ETERN_SIZE−2)` bords)
à **chaque appel** de `search_packet_backtracking`, et ne sont ajustés que par les
décisions prises PAR CET APPEL — jamais par un scan de l'état hérité d'un ancêtre ou d'un
paquet matérialisé par `bt_materialize_pending`. Un stock en excès ne peut jamais produire
de faux positif (le test ne compare qu'un déficit) : cette simplification ne peut que
**manquer** une famine déjà introduite ailleurs, jamais rejeter à tort une branche valide
— mais elle est, de ce fait, structurellement aveugle à toute famine héritée par
délégation, un chemin pourtant fréquent.

**Mesuré** (`tests/bench/bench_search.sh`, puzzle 256, A/B à ordre alterné, 50 M nœuds × 2
répétitions × 2 tours) :

| Configuration | Nœuds/s | `fc_frame_starvation` | `max_result` |
|---|---|---|---|
| Sans | ≈ 10,8 M | — | 74 |
| Avec | ≈ 9,3 M | **0** | 74 |
| Delta | **−14 %** | — | inchangé |

`fc_frame_starvation` reste à **zéro** jusqu'à 200 M nœuds explorés — jamais un seul
déclenchement, malgré l'estimation *a priori* plus favorable que §4.4 (2 à 12 % des
candidats d'une case intérieure sont des pièces de cadre, cf. §3.3 — contre un événement
composé et rare pour le conflit de singletons). Le coût (≈ 5 comparaisons entières et 2
`if` supplémentaires par placement, dans la boucle la plus chaude du programme) est,
comme pour §4.4, systématique et sans contrepartie mesurée.

**Pourquoi ça ne se déclenche pas malgré une estimation plus favorable (hypothèse, non
vérifiée en détail).** Deux explications distinctes, non exclusives : (1) le stock de
pièces de cadre (4 coins, jusqu'à 56 bords sur le puzzle 256) est large comparé au nombre
de décisions qu'un seul appel de `search_packet_backtracking` prend avant délégation ou
épuisement — une poignée de mauvais placements dans UN sous-arbre a peu de chances
d'épuiser un stock aussi grand ; (2) la variante testée, comme expliqué ci-dessus, ne voit
JAMAIS une famine introduite avant le début de cet appel (délégation, ancêtre) — exactement
le chemin par lequel une vraie famine, une fois introduite, se propagerait dans l'arbre.

**Décision : ne pas merger le code de la variante compteurs affaiblie.** La variante
« partition de l'arène » (toujours en proposition, §6) contourne le problème différemment
: elle empêche la MAUVAISE PIÈCE d'être un candidat du tout (au lieu de la rejeter après
coup), donc n'a pas besoin de connaître le type des pièces déjà utilisées — elle échappe
entièrement au risque de fixture qui a motivé l'affaiblissement des compteurs. C'est la
piste à essayer en premier si ce mécanisme est repris, plutôt qu'une version renforcée
(ancêtre-consciente) des compteurs, qui replonge dans le même risque de fixture pour un
gain non démontré.

### 4.3 Comptage global couleur : demande de frontière vs stock disponible — IMPLÉMENTÉ, MESURÉ, ÉCARTÉ

**Statut : implémenté (version complète, consciente des ancêtres), mesuré, abandonné**
(code absent de `master`). Contrairement à §4.2/§4.4 (compteurs volontairement affaiblis,
scindés à cet appel de `search_packet_backtracking` pour éviter un risque de fixture),
cette piste a été implémentée **sans concession** : demande/disponible sont initialisés
depuis l'état RÉEL du paquet racine (genèse, indices, ancêtres, délégation), pas depuis une
capacité théorique repartant de zéro. Résultat : un mécanisme qui **tire massivement**
(contrairement à §4.2/§4.4) mais qui reste un **net négatif** — pour une raison différente
et plus instructive que « ne se déclenche jamais ».

**Principe.** Le forward-check juge chaque case **isolément** : il ne verra jamais que
5 cases de frontière réclament la couleur 9 alors qu'il ne reste que 3 demi-arêtes de
couleur 9 dans le stock. Condition nécessaire :

> pour toute couleur `c` : `demande[c] ≤ disponible[c]`

où `demande[c]` = demi-arêtes de couleur `c` portées par une pièce **posée** et tournées
vers une case **vide** ; `disponible[c]` = demi-arêtes de couleur `c` sur les pièces **non
utilisées**.

**Preuve (nécessité).** Toute demi-arête de frontière sera appariée à une face de la pièce
qui remplira la case vide adjacente. Deux demi-arêtes de frontière distinctes
correspondent à deux couples (case, direction) distincts, donc à deux faces distinctes de
pièces non utilisées — même quand elles visent la même case vide (deux faces de la même
pièce). L'injection impose `demande[c] ≤ disponible[c]`. ∎

**Implémentation.** `demande[]` est dérivé du cache `constraints[][]` déjà maintenu par
4.1 (`bt_init_constraints`/`bt_propagate_place`/`bt_propagate_undo`) : pour toute case
VIDE, chaque côté non-wildcard de `constraints[case]` EST une demi-arête de demande — pas
besoin de décoder les pièces déjà posées. `disponible[]` est initialisé par un balayage de
`b_faceused` via `all_rotate_part` (id-indexé sur `ETERN_PARTS`, cf. ci-dessous), puis
maintenu en O(1) par placement/retrait (au plus 8 compteurs touchés). L'ordre LIFO du
backtracking garantit qu'à l'annulation, l'état d'emptiness des voisines est identique à
celui observé à la pose — aucun stockage supplémentaire n'est nécessaire pour cette part,
seule la couleur de la pièce posée (rotation réellement choisie, pas rotation 0) est
mémorisée par niveau pour un retrait correct à distance.

**Pré-requis découvert en cours de route : corriger `make_small_parts()`.** Ce fixture de
test partagé (`tests/core/test_etii_search.c`, utilisé par une dizaine de tests de
`search_packet_backtracking`) était indexé `parts[i]` pour l'id `i+1` — un schéma qui ne
correspond PAS à la convention `id`-comme-indice de `rotate_all_parts()` (bouchon id=0 à
l'indice 0). Ça n'avait jamais posé de problème car rien ne lisait `all_rotate_part` par id
sur un plateau vide avant cette piste. Corrigé : indexage `id`-comme-indice, ids 1..8
inchangés (valeurs historiques), ids 9..`ETERN_PARTS` complétés avec un stock généreux des
couleurs déjà en usage — un stock en excès ne peut jamais déclencher de famine (le test ne
compare qu'un déficit).

**Mesuré** (`tests/bench/bench_search.sh`, puzzle 256, A/B à ordre alterné, 50 M nœuds × 2
répétitions × 2 tours) :

| Configuration | Nœuds/s | `fc_colour_starvation` / `fc_pruned` | `max_result` |
|---|---|---|---|
| Sans | ≈ 10,8 M | — | 74 |
| Avec | ≈ 8,2 M | **≈ 47–49 %** | 74 |
| Delta | **−24 %** | — | inchangé |

Contrairement à §4.2/§4.4, `fc_colour_starvation` **tire énormément** — près de la moitié
de TOUS les élagages inline, à n'importe quelle échelle testée (20 M à 200 M nœuds). Mais
le taux d'élagage TOTAL (`fc_pruned / fc_attempts`, colonnes couleur + forward-check
confondues) reste dans la même fourchette que la référence §4.1 (~44-46 %) — c'est-à-dire
que ce mécanisme n'ajoute quasiment **aucune branche morte que le forward-check
n'aurait pas fini par trouver de toute façon**, il se contente souvent de la trouver un
peu plus tôt. Le coût de tenue à jour (8 compteurs entiers, deux directions
d'annulation, à CHAQUE placement, y compris ceux qui ne mèneront à aucune famine) dépasse
la valeur de ce raccourci.

**Pourquoi le recoupement avec le forward-check, et pas un vrai complément (hypothèse,
cohérente avec la mesure mais non prouvée formellement).** Une famine de couleur `c`
suppose que `disponible[c]` est bas — ce qui, structurellement, signifie que la plupart des
pièces de couleur `c` sont déjà posées. Sur un plateau où ça arrive, une case de bordure
proche réclamant `c` a de bonnes chances d'être elle-même la voisine directe d'une pièce
déjà posée — exactement ce que `bt_forward_check` (§4.1) inspecte déjà. Les deux
mécanismes convergent donc souvent vers la MÊME impasse locale, découverte par deux
chemins de coût différent (comparaisons d'entiers vs lookup de map) mais de portée
largement recouvrante dans la région du puzzle réellement atteinte aujourd'hui.

**Décision : ne pas merger.** Contrairement à §4.2/§4.4, ce n'est pas un problème
d'implémentation affaiblie (celle-ci était complète) — c'est un recoupement structurel
avec un mécanisme déjà en place. Reprendre uniquement avec une implémentation qui réduit
drastiquement le coût par placement (ex. ne suivre que les couleurs à faible stock, 18–22,
plutôt que les 23 uniformément — non essayé) ET seulement si une mesure ultérieure montre
que le recoupement avec le forward-check diminue (ex. après 4.7, l'ordre dynamique, qui
changerait la forme de l'arbre et donc la proximité entre demande et voisinage).

### 4.4 Conflit de singletons (mini-test de Hall) — IMPLÉMENTÉ, MESURÉ, ÉCARTÉ

**Statut : évalué et abandonné.** Implémenté dans `bt_forward_check`, mesuré, puis
**reverté** — code absent de `master`. Ce qui suit est la trace du raisonnement et de la
mesure, dans le même esprit que le lookup de placement via `packed` (PR #161,
[autosearch_step.md](../autosearch_step.md)) : une piste correcte et bien motivée peut
rester sans effet mesurable si la partie de l'arbre où elle jouerait n'est pas encore
atteinte.

**Principe.** Pendant le balayage du forward-check, retenir les cases dont le nombre de
candidats **libres** vaut exactement 1, avec l'`id` de cette pièce. Si deux cases exigent
la même pièce unique ⇒ branche morte. C'est le cas `|S| = 2` du théorème de Hall, le
premier que le forward-check ne peut structurellement pas voir — et un vrai « case par
case » ne peut jamais le détecter : chaque voisine prise isolément a bien ≥ 1 candidat.

**Implémentation.** Greffée sur la boucle de 4.1 (`bt_forward_check`, au plus 4 voisines
géométriques par appel) : au lieu de s'arrêter au premier candidat libre trouvé, compter
jusqu'à 2 (assez pour distinguer « singleton » de « pas singleton », inutile d'aller plus
loin) ; si exactement 1, comparer son `id` aux singletons déjà vus dans CE balayage (≤ 3
comparaisons, tableau de 4 entrées). Un compteur dédié, `fc_singleton_conflict`
(sous-ensemble de `fc_pruned`), isolait sa contribution — condition nécessaire pour
répondre à « rentable ou non » sans se fier au seul débit agrégé.

**Mesuré** (`tests/bench/bench_search.sh`, puzzle 256, A/B à ordre alterné, 50 M nœuds × 2
répétitions × 2 tours) :

| Configuration | Nœuds/s | `fc_singleton_conflict` | `max_result` |
|---|---|---|---|
| Sans (PR1 seul) | ≈ 10,7 M | — | 74 |
| Avec | ≈ 9,7 M | **0** | 74 |
| Delta | **−9 %** | — | inchangé |

`fc_singleton_conflict` reste à **zéro** sur 500 M nœuds explorés (373,7 M élagages) —
pas « rare », mais jamais observé dans la région du puzzle 256 réellement atteinte
aujourd'hui (le mur structurel à `max_result` = 74, cf.
[tests_et_ci.md § max_result](../tests_et_ci.md#max_result--le-débit-seul-ne-prouve-pas-un-vrai-gain)).
Le coût, lui, est systématique : chercher un deuxième candidat libre au lieu de s'arrêter
au premier coûte à CHAQUE appel, que la voisine finisse par être un singleton ou non —
d'où la perte de débit malgré un mécanisme qui ne s'est jamais déclenché.

**Pourquoi ça ne se déclenche pas (hypothèse, non vérifiée en détail).** Un conflit exige
que DEUX voisines survivent chacune jusqu'à `free_count == 1` sans que ni l'une ni l'autre
n'ait déjà déclenché l'élagage « classique » (compartiment vide ou toutes pièces
utilisées) — c'est-à-dire que la première voisine inspectée soit déjà, elle, dans cet état
rare. Dans la région du puzzle réellement explorée à ce jour (bornée par le même mur
structurel qu'ailleurs dans ce document, `max_result` ≈ 74), la plupart des élagages
observés surviennent probablement dès la première voisine inspectée (compartiment vide ou
épuisé), avant même qu'un deuxième candidat unique puisse entrer en jeu — non mesuré
précisément (`fc_pruned_at[]` n'a pas été relevé pour cette piste), mais cohérent avec un
coût constant pour un gain nul.

**Décision : ne pas merger, garder la piste consignée pour plus tard.** Reprendre
uniquement si une PR ultérieure de cette série (au premier chef 4.7, l'ordre dynamique, ou
4.5, la propagation des cases forcées) change la profondeur réellement atteinte : plus le
plateau se remplit, plus les compartiments de candidats s'amenuisent, et plus un conflit de
singletons devient a priori probable. Remesurer à ce moment-là plutôt que de raviver ce
mécanisme en l'état.

### 4.5 Propagation des cases forcées dans la boucle chaude

**Principe.** Le pruner place les pièces des cases à candidat unique
([`possibility_all_has_a_next_counted`](../../src/core/possibility.c)) ; **la recherche ne
le fait pas**. Chaque placement forcé supprime un niveau de branchement *et* resserre les
clés de ses voisines, ce qui cascade.

**Compatibilité.** Acquise : le format paquet supporte déjà les cases pré-remplies, que le
moteur traite comme un « niveau sans décision ». Le travail réel est le **défaire** —
mémoriser dans `bt_level` les positions forcées par niveau pour les libérer au backtrack.

**Attention.** `alloc` sert à la fois d'index de parcours et de compteur de pièces posées,
et les placements forcés font diverger les deux (divergence déjà présente aujourd'hui côté
pruner). Lever l'ambiguïté **avant** cette PR, pas pendant.

### 4.6 Pruner : du test superficiel à la preuve de mort bornée

C'est le processus dont le métier est précisément « éliminer sans développer », et il fait
aujourd'hui le minimum. Deux montées en puissance indépendantes :

**a) Itérer jusqu'au point fixe — LIVRÉ (PR 5/9).** `possibility_all_has_a_next_counted`
faisait **une seule passe** : une pièce forcée en fin de balayage peut rendre morte une
case déjà validée en début de passe, et personne ne revenait dessus — un manque déjà
documenté (commentaire historique au-dessus de `remove_possibilities_with_no_next`,
`src/core/datamanager.c`) mais jamais corrigé. Fixé en enveloppant le balayage existant
dans une boucle `do { … } while (…)` qui relance un passage complet tant qu'un placement a
eu lieu lors du précédent, jusqu'à point fixe (aucun placement) ou case sans issue. Coût
non nul mais borné : au pire `ETERN_PARTS` passages, chacun strictement causé par un
placement du précédent (donc un budget total de l'ordre de `O(ETERN_PARTS²)` dans le pire
cas théorique, jamais atteint en pratique — voir mesure ci-dessous).

*Test unitaire* (`all_has_a_next_fixpoint_detects_cascading_forced_dead_cell`,
`tests/core/test_possibility.c`) : montage à 3 coins mutuellement non adjacents où une
case à 2 candidats, examinée EN PREMIER dans le parcours, voit ses deux candidats
consommés par deux forçages **ultérieurs** dans le même balayage — l'ancien code renvoyait
« vivant » à tort, le point fixe détecte correctement la mort au 2e passage.

*Mesure réelle* (256 pièces, `--expand-level 4`, stock initial identique de 1193
possibilités, comparaison via git worktree contre `master`, `rmnonext` déclenché depuis la
console serveur) :

| | avant | après 1 appel `rmnonext` | stable après un 2e appel ? | temps (médiane, 5 rép.) |
|---|---|---|---|---|
| master (une passe) | 1193 | 965 (-228) | **NON** — un 2e appel retire 15 de plus (→950) | ~0,084 s |
| PR 5 (point fixe) | 1193 | 950 (-243) | oui, dès le 1er appel | ~0,142 s |

Confirmation directe et non synthétique du manque documenté : sur un stock réel, `master`
n'atteint le point fixe correct (950) qu'au bout d'un **second** appel manuel — la
possibilité de rejouer `rmnonext` plusieurs fois n'existe que pour l'opérateur console.
Côté pruner **client** (`autoprune_step`, `src/core/etii_search.c`), chaque possibilité
n'est examinée qu'**une seule fois** avant d'être renvoyée au serveur : il n'y a jamais de
« second passage » naturel, donc le manque de `master` y est un déficit **permanent et
non détecté**, pas seulement un inconvénient opérationnel. Le surcoût par appel (~1,7×)
est strictement inférieur au coût cumulé des deux appels que `master` nécessiterait pour
atteindre le même état. Conservé.

Tests : `tests/core/test_possibility.c` (`all_has_a_next_fixpoint_detects_cascading_forced_dead_cell`,
`all_has_a_next_fixpoint_isolated_force_still_returns_one` — non-régression : un forçage
isolé sans cascade continue de renvoyer 1). Suite complète (1042 tests), WERROR, tous les
`DEBUG_*`, `ETERN_PARTS=16`, ASan et `make test-integration` : verts.

**b) DFS à budget de nœuds — IMPLÉMENTÉ, TESTÉ, DÉSACTIVÉ PAR DÉFAUT (PR 6).** Rejouer
`search_packet_backtracking` avec un plafond de nœuds (ordre de grandeur envisagé
initialement : 10 000). Si le sous-arbre se **ferme** dans le budget, la possibilité est
définitivement morte : retirée du stock, jamais redistribuée. Si le budget est atteint,
elle est conservée `checked` comme aujourd'hui. Aucun faux positif possible : c'est le même
code que la recherche réelle qui tranche, pas une heuristique.

**Implémentation.** La boucle chaude de `search_packet_backtracking` (§4.1) est factorisée
en `search_packet_backtracking_core` (`src/core/etii_search.c`), paramétrée par un plafond
de nœuds (`node_budget`, `<= 0` = illimité) et un drapeau `allow_delegate` — deux fines
enveloppes la spécialisent sans dupliquer la boucle : `search_packet_backtracking` (usage
recherche réelle, inchangé : illimité, délégation autorisée) et
`search_packet_backtracking_budgeted` (nouvelle, usage pruner : plafonnée, délégation
**interdite**). La désactivation de la délégation n'est pas cosmétique : céder une partie
du sous-arbre à un tiers pendant la preuve romprait la preuve elle-même (le budget
n'aurait plus exploré tout ce qu'il prétend avoir fermé). Sur `REQUEST_STOP`, la variante
recherche réelle renvoie le travail en cours au serveur (`bt_flush_pending`) ; la variante
bornée ne renvoie RIEN — elle abandonne simplement l'exploration locale, et l'appelant
retombe sur le comportement d'avant cette PR (possibilité originale intacte, conservée
`checked`) : fragmenter une possibilité en cours de preuve n'aurait aucun sens pour un
contrôle qui n'a jamais eu vocation à déléguer. `autoprune_step` insère l'appel entre le
contrôle superficiel et la décision historique : seule une possibilité jugée vivante par
`possibility_all_has_a_next_counted` mais **pas encore** `checked` (`!work.checked &&
has_next`) déclenche la preuve bornée — une possibilité déjà `checked` ou déjà prouvée
morte par le contrôle superficiel ne paie jamais ce coût supplémentaire.

**Configuration.** `pruner_dfs_budget` (`src/app/static_variables.{h,c}`), configurable au
démarrage (fichier de configuration client, clé `dfs_budget`) et à l'exécution (commande
console `prunerDfsBudget <n>`, propagée aux process enfants, pilotable à distance via
`clientsCommand`/l'API HTTP admin comme `prunerBatch`) ; `<= 0` désactive entièrement ce
contrôle supplémentaire (même convention que `limit 0`), plafonné à
`PRUNER_DFS_BUDGET_MAX` (10 000 000) au-delà duquel un seul contrôle de possibilité
cesserait d'être une opération bon marché. `pruner_dfs_closed`/`pruner_dfs_nodes`
(compteurs locaux, non propagés au réseau — voir leur doc dans `static_variables.h` pour
la justification) isolent la contribution propre du mécanisme, dans le même esprit que
`fc_singleton_conflict` pour §4.4 : répondre à « rentable ou non » sans se fier au seul
débit agrégé.

*Tests unitaires* (`tests/core/test_etii_search.c`), à deux volets comme l'exige §5 :
`search_backtracking_budgeted_closes_when_budget_suffices` (budget large sur un arbre
minuscule → `BT_CORE_EXHAUSTED`, jamais délégué) et
`search_backtracking_budgeted_returns_budget_when_insufficient` (même arbre, budget d'UN
nœud → `BT_CORE_BUDGET`) pour le cœur ; `autoprune_step_dfs_budget_closes_possibility`,
`autoprune_step_dfs_budget_too_small_keeps_possibility` et
`autoprune_step_dfs_budget_disabled_skips_dfs` pour l'intégration dans le pruner. Un
quatrième test, `search_backtracking_budgeted_stop_returns_stopped_without_flush`,
verrouille spécifiquement la divergence volontaire avec la variante illimitée sur
`REQUEST_STOP` (aucun flush). Trois tests **préexistants** (`autoprune_step_keeps_live_packet`,
`autoprune_step_pauses_then_resumes`, `autoprune_step_add_error_reputs_locally`) réutilisaient
une fixture (`make_free_map`, 2 identifiants de pièce réels seulement) qui se trouve être,
elle aussi, un sous-arbre que la preuve bornée fermerait entièrement : désactivée
explicitement (`pruner_dfs_budget = 0`) dans ces trois tests, qui ne portent pas sur ce
mécanisme — trouvé en faisant tourner la suite complète avec le budget par défaut à 10 000
lors du développement, avant la décision de désactivation ci-dessous (la suite aurait
autrement continué de passer accidentellement avec un défaut à 0, masquant la question).

**Mesure sur stock réel.** Contrairement à §4.1/§4.2/§4.3/§4.4 (mesurés via
`tests/bench/bench_search.sh`, protocole conçu pour la boucle de recherche), §4.6b agit sur
le pruner : un harnais dédié, jetable, a rejoué le VRAI `search_packet_backtracking_budgeted`
sur du stock RÉEL (256 pièces, `data/pieces.csv`) obtenu de deux façons — (1) genèse
peu développée (`expand_datas_to_level`, comme `--expand-level` au démarrage serveur), et
(2) surtout, une recherche réelle (`search_packet_backtracking`, illimitée) lancée depuis la
genèse et interrompue après quelques secondes par `REQUEST_STOP`, dont le flush
(`bt_flush_pending`) verse dans le stock les frères non explorés — un stock représentatif
de ce que la délégation envoie réellement au pruner, pas une fixture synthétique :

| Génération du stock | Possibilités | Profondeur (`alloc`) min/moy/max | Candidats à la preuve | Fermées (n'importe quel budget testé) |
|---|---|---|---|---|
| `expand_datas_to_level(4)` | 1193 | 4 / 4 / 4 | 950 | **0** (budget 10 000) |
| Recherche réelle, 2 s (`max_result`=74) | 1689 | 1 / 33,7 / 72 | 107 | **0** (budget 10 000) |
| Recherche réelle, 8 s (`max_result`=74) | 4951 | 1 / 33,6 / 71 | 107 | **0** (budget **1 000 000**, 100× l'ordre de grandeur envisagé) |

**Verdict : 0 % de fermeture, à n'importe quelle échelle de budget testée.** La cause est
structurelle, pas un manque de budget : ce puzzle plafonne aujourd'hui à `max_result` ≈
74/256 pièces posées (le même mur structurel déjà cité en §4.4/§4.6a/§7, atteint
indépendamment sur des centaines de millions de nœuds ailleurs dans ce document) — même la
possibilité la plus profonde jamais atteinte dans ces mesures (`alloc` = 72) laisse encore
**~184 cases vides**. Le sous-arbre qui en découle est démesurément plus grand que ce
qu'aucun budget raisonnable (10 000, ni même 1 000 000 — 100× plus) ne peut épuiser : la
délégation réelle ne cède aujourd'hui **jamais** de possibilité assez profonde pour que
cette preuve ait une chance d'aboutir. Le coût, lui, est bien réel et systématique : au
budget 10 000, chaque tentative infructueuse coûte environ 1 ms (mesuré : 936 µs/tentative
sur le premier échantillon), contre ~1 µs pour le seul contrôle superficiel — un surcoût
d'environ 230× sur les possibilités qui atteignent cette branche, pour un gain mesuré nul.

**Décision : conserver le code (correct, testé, sans coût si désactivé), mais désactivé
par défaut.** Différent de §4.2/§4.3/§4.4, dont le code a été retiré de `master` : ici la
garde `pruner_dfs_budget > 0` rend le mécanisme strictement gratuit une fois désactivé
(aucun appel à `search_packet_backtracking_budgeted`), donc le garder en configuration
opt-in ne coûte rien à l'état actuel du projet et évite de perdre le travail de
factorisation (`search_packet_backtracking_core`, réutilisée telle quelle si le mécanisme
redevient pertinent) si une PR ultérieure change la donne. `PRUNER_DFS_BUDGET_DEFAULT` vaut
donc **0** (désactivé), pas l'ordre de grandeur initialement envisagé — un choix délibéré,
pas un défaut prudent en attendant un réglage plus fin : aucune valeur de budget ne changerait
la conclusion tant que la profondeur atteignable reste bornée par ce mur. **À réactiver
uniquement si 4.5 (propagation des cases forcées) ou 4.7 (ordre dynamique MRV) déplacent ce
mur significativement plus profond** (cases vides restantes de l'ordre de la dizaine, pas de
la centaine) — remesurer à ce moment-là avec le même harnais plutôt que de raviver le
mécanisme en l'état, exactement la même discipline que la décision de §4.4.

Le mode GPU ([`gpu_pruner.cu`](../../src/app/gpu_pruner.cu)) ne suit pas sur (b) — un DFS
divergent par thread convient mal au modèle SIMT. (a) lui est en revanche transposable.

### 4.7 Ordre de variable dynamique (MRV)

**Principe.** Choisir à chaque nœud la case vide **la plus contrainte** au lieu de suivre
`directions[]`. C'est le levier le plus massif connu en résolution de CSP, et le seul de
ce document qui change la forme de l'arbre plutôt que d'en couper des branches.

**Faisabilité sans casser le protocole.** Un paquet transporte la grille complète. Un
client peut explorer dans son ordre propre et ne re-canoniser qu'aux frontières
(`bt_materialize_pending`, `bt_flush_pending`) : `alloc` = première case non remplie du
parcours canonique, les cases posées « en avance » devenant des cases pré-remplies — un
état que le moteur sait déjà lire. Deux clients d'ordres internes différents restent donc
interopérables.

**Coût.** Élevé : le cache `constraints[][]` doit fournir un *choix* et plus seulement une
clé, la pile de décisions doit mémoriser la case choisie par niveau, et la
re-canonisation des paquets délégués devient non triviale. À n'engager qu'après les
mesures des PR précédentes, et à traiter comme un projet à part entière.

### 4.8 Ordre des candidats dans l'arène — ADOPTÉ (PR 7)

**Statut : implémenté, mesuré, adopté.** Comportement de production
**inconditionnel** depuis cette PR — aucun interrupteur laissé en place, même discipline
que §4.1 (« voisines seules, pas de fenêtre résiduelle »).

**Principe.** Trier chaque compartiment **à la construction** — pièce exposant les
couleurs les plus rares en premier, ou l'inverse — coûte **zéro à l'exécution** (le tri a
lieu une seule fois, à la construction de la map, avant tout fork et avant toute
recherche) et change la vitesse à laquelle les branches mortes s'épuisent. Sans effet sur
le format de paquet : les indices `next_s` sont purement locaux à un client et ne
transitent jamais — aucun bump de `VERSION`.

**Implémentation.** `compute_face_frequency` (`src/core/part.c`) compte, une seule fois par
construction de map, le nombre de demi-arêtes de chaque couleur dans `apart` (rotations
comprises — un facteur d'échelle uniforme ×4 qui ne change pas l'ordre relatif de rareté
entre couleurs). `arena_exposed_score` note une pièce candidate d'un compartiment
`(f1,f2,f3,f4)` en ne sommant la fréquence QUE sur les côtés **wildcard** (`f_i == -1`) :
les côtés contraints sont identiques pour toutes les pièces du compartiment et ne
discriminent aucun ordre. `sort_compartment_by_exposed_rarity` trie chaque compartiment
non trivial par ce score **croissant** (la pièce exposant la couleur la plus rare essayée
en premier), par insertion avec les scores précalculés une fois — les compartiments non
vides restent petits en pratique (cf. §4.2/plus gros compartiment observé, quelques
centaines de pièces). Le tri a lieu dans `buildBigArray`, sur `entry->parts` **avant** le
compactage dans `arena` : `packed` et `flat` restent construits à partir du même tableau
déjà trié, donc leur équivalence stricte (`packed_index_matches_flat_for_every_key`,
§ ci-dessous) continue de tenir sans modification.

**Garantie de correction.** Le tri ne change JAMAIS le multi-ensemble de pièces d'un
compartiment (mêmes ids, mêmes rotations, même taille) — uniquement l'ORDRE dans lequel
elles seront essayées. Verrouillé par
`arena_sort_preserves_multiset_and_orders_by_rarity` (`tests/core/test_part.c`), et par un
test d'intégration bout-en-bout,
`buildbigarray_sorts_wildcard_compartment_by_ascending_rarity`, qui construit une vraie map
via `buildBigArray` et vérifie que le plus gros compartiment obtenu est bien trié par
score croissant.

**Protocole de mesure.** Avant adoption, l'ordre était piloté par une variable
d'environnement de développement (`ETII_ARENA_ORDER=rare_first|common_first`), lue une
seule fois via `getenv()` dans `buildBigArray` — même convention que `ETII_BENCH_NODES`,
hors chemin de production. Trois configurations comparées au banc
(`tests/bench/bench_search.sh`, puzzle 256, 20 M nœuds × 5 répétitions, A/B à **ordre
alterné** pour neutraliser la dérive thermique de la machine — 8 mesures au total dans
l'ordre baseline → rare_first → baseline → common_first → rare_first → baseline →
rare_first → common_first) :

| Configuration | nœuds/s (médiane par run) | Moyenne sur N runs | Taux d'élagage forward-check | `max_result` |
|---|---|---|---|---|
| Sans tri (`master`) | 10 760 419 / 10 964 297 / 10 747 543 | **10 824 086** (3 runs) | ≈ 45,648 % | 74 |
| `rare_first` | 11 142 362 / 11 205 017 / 11 169 099 | **11 172 160** (3 runs) | ≈ 42,188 % | 74 |
| `common_first` | 10 961 956 / 11 148 979 | **11 055 468** (2 runs) | ≈ 41,904 % | 74 |

`rare_first` : **+3,22 %** de débit médian moyen par rapport à la baseline, avec un
regroupement serré entre les 3 mesures (11 142 160–11 205 017, aucun recouvrement avec la
plage des 3 baselines 10 747 543–10 964 297) — une séparation nette, contrairement à la
dérive thermique seule mesurée entre deux baselines consécutives (+1,89 % entre A1 et A2,
prise comme référence du bruit de fond de la machine). `common_first` : +2,14 % en
moyenne mais avec un recouvrement partiel avec `rare_first` (11 148 979 chevauche presque
le minimum de `rare_first`) — signal moins net, cohérent avec l'absence de justification a
priori pour cet ordre (l'intuition de §4.8 — résoudre tôt les contraintes rares pendant
qu'il reste du stock — ne motive que `rare_first`).

Fait notable : le taux d'élagage forward-check **change** dans les deux configurations
triées (45,6 % → ~42 %) sans que `max_result` bouge (toujours 74 à 20 M nœuds, dans les
huit mesures) — signe que le tri modifie réellement la **forme** de l'arbre exploré (moins
de branches nécessitent le forward-check pour être coupées, une partie de ce travail étant
maintenant fait plus tôt par le simple ordre d'essai), sans jamais explorer un arbre plus
petit pour le même travail : exactement le critère que §7 impose de vérifier avant de
crediter un gain de débit apparent (cf. la note sur `max_result` dans le protocole de
mesure du banc, [tests_et_ci.md](../tests_et_ci.md#max_result--le-débit-seul-ne-prouve-pas-un-vrai-gain)).

**Décision : adopter `rare_first`, inconditionnellement.** Le gain est modeste (+3,2 %,
loin des +68,8 % de §4.1) mais reproductible sur 3 mesures indépendantes nettement
séparées de la baseline, à coût nul (le tri n'ajoute rien à la boucle chaude — seul le
temps de construction de la map, négligeable et payé une seule fois, augmente légèrement)
et sans aucun risque de correction (invariant multi-ensemble verrouillé par test). Une
fois la décision prise, l'interrupteur de développement (`ETII_ARENA_ORDER`, l'enum de
mode, le `getenv()`) est retiré du code de production plutôt que laissé en configuration
opt-in — contrairement à §4.6b (conservé opt-in car non prouvé bénéfique sur le stock
actuel), ici le bénéfice est acquis : garder un levier inutilisé n'aurait aucune valeur et
n'est pas la convention de ce projet pour une piste adoptée (cf. §4.1). `common_first`
n'est pas retenu, faute de justification a priori et de signal aussi net.

## 5. Arbitrages tranchés

- **Une condition nécessaire, jamais une heuristique.** Un faux positif jette
  silencieusement la solution et ne se manifeste par aucun symptôme observable. Chaque
  élagage est livré avec un test unitaire à deux volets : un plateau où il **doit** tirer,
  un plateau où il **ne doit pas** tirer. Aucune mesure de débit ne remplace ce verrou.
- **Pas de bump de `VERSION`**, sauf 4.7. Un paquet reste un état de plateau ; seul le
  sous-ensemble exploré change. C'est le changement de `directions[]` qui avait forcé le
  bump v11, parce qu'il redéfinissait le sens d'`alloc`.
- **La parité de couleur est une impasse.** Les 23 couleurs de `pieces.csv` sont toutes en
  compte pair, et tout placement légal préserve cette parité : la face posée quitte le
  stock pendant que la demande correspondante apparaît (voisin vide) ou disparaît (voisin
  posé, couleurs égales par construction). C'est une **loi de conservation**, pas une
  contrainte — le test vaudrait toujours 0. Ne pas la reproposer.
- **Pas de table de transposition sur la frontière.** Largeur maximale 52 cases (§3.2), et
  l'état inclut de toute façon le masque des 256 pièces utilisées : aucune compression
  utile n'est possible.
- **Pas de bitmap d'occupation des compartiments** : déjà implémenté et mesuré à **−4 %**
  (cf. [autosearch_step.md](../autosearch_step.md)) — le forward-check réussit ~54 % du
  temps, donc la majorité des cases inspectées paieraient deux accès aléatoires au lieu
  d'un.
- **Pas de conflit de singletons dans l'état actuel de l'arbre exploré.** Implémenté,
  mesuré, reverté (§4.4) : **−9 % de débit, 0 déclenchement** sur 500 M nœuds. Correct
  mais sans effet mesurable tant que la profondeur atteinte reste bornée par le mur
  structurel à `max_result` ≈ 74 — à remesurer si une PR ultérieure (4.5 ou 4.7) déplace ce
  mur, pas à raviver en l'état.
- **Pas de comptage global couleur (implémentation uniforme sur les 23 couleurs).**
  Implémenté SANS concession (compteurs conscients des ancêtres, pas affaiblis comme
  4.2/4.4), mesuré, reverté (§4.3) : **−24 % de débit** malgré un déclenchement massif
  (≈47-49 % des élagages inline). Cas différent des deux précédents : pas une absence de
  déclenchement, un recoupement structurel avec le forward-check — les deux mécanismes
  trouvent souvent la MÊME impasse, le second à moindre coût par appel. Une variante ciblée
  (colonnes 18-22 seulement) reste envisageable mais non essayée.
- **Ne pas retoucher `directions[]` dans ce cadre.** L'ordre actuel a été choisi pour
  éliminer tôt et son changement impose un bump de protocole ; les pistes ci-dessus
  s'appliquent toutes à ordre constant.
- **4.6b (DFS à budget du pruner) : code conservé, désactivé par défaut.** Implémenté et
  testé sans concession (même code que la recherche réelle, cf. §4.6b), mesuré sur du stock
  RÉEL (delegation réelle après recherche, pas une fixture synthétique) : **0 % de
  fermeture**, même à budget 100× l'ordre de grandeur envisagé (1 000 000 contre 10 000
  nœuds). Cas différent de §4.2/§4.3/§4.4 : la garde est gratuite une fois désactivée, donc
  le code reste en configuration opt-in plutôt que d'être retiré — mais le défaut passe à 0
  (désactivé), pas à la valeur initialement envisagée. Cause identique à §4.4 (mur
  structurel `max_result` ≈ 74) : à remesurer si 4.5/4.7 le déplacent, pas à activer en
  l'état.
- **4.8 (ordre des candidats dans l'arène) : `rare_first` adopté, inconditionnel.**
  Mesuré au banc (20 M nœuds × 5 répétitions, A/B à ordre alterné, 3 baselines et 3
  mesures `rare_first` non recouvrantes) : **+3,2 % de débit médian moyen**, taux
  d'élagage forward-check modifié (45,6 % → 42,2 %, signe d'un arbre de forme différente)
  mais `max_result` inchangé (74, à N nœuds identique dans les 8 mesures) — un vrai gain
  de progrès, pas un artefact de mesure. `common_first` mesuré aussi (+2,1 % en moyenne,
  mais chevauchement partiel avec `rare_first`, signal moins net) : non retenu. Contraste
  avec §4.2/§4.3/§4.4/§4.6b : ici le mécanisme est gratuit ET bénéfique, donc adopté sans
  laisser d'interrupteur — cf. §4.8 pour le détail complet des mesures.

## 6. Points laissés ouverts

- ~~**4.2 : compteurs ou partition de l'arène ?**~~ Tranché par élimination : les compteurs
  (variante affaiblie, seule testée) sont écartés (§4.2, −14 % de débit, 0 déclenchement).
  La partition reste ouverte et devient la SEULE variante encore candidate — elle
  échappe au risque de fixture qui a motivé l'affaiblissement des compteurs, en empêchant
  la mauvaise pièce d'être candidate plutôt qu'en la rejetant après coup.
- ~~**4.1 : garder ou non la fenêtre `c+1 … c+2` ?**~~ Tranché : non — voir §4.1, mesure à
  l'appui (−0,06 point de taux d'élagage sans la fenêtre résiduelle, effet négligeable).
- ~~**4.4 : le conflit de singletons est-il rentable ?**~~ Tranché : non, dans l'état actuel
  de l'arbre exploré — voir §4.4, implémenté/mesuré/reverté (−9 % de débit, 0
  déclenchement sur 500 M nœuds).
- ~~**4.3 : le test tire-t-il jamais ?**~~ Tranché : il tire ÉNORMÉMENT (≈47-49 % de tous
  les élagages inline) — mais §4.3, non rentable quand même : −24 % de débit, car il
  recoupe structurellement ce que le forward-check trouvait déjà, pour un coût de tenue à
  jour bien plus élevé (voir §4.3, seule piste de la série où « ça tire beaucoup » et « ce
  n'est pas rentable » coexistent).
- ~~**4.6b : quel budget de nœuds ?**~~ Tranché, mais pas comme prévu : la question n'était
  pas la bonne valeur, c'était la profondeur du stock disponible — voir §4.6b, mesuré sur
  stock réel (delegation après recherche réelle) : **0 % de fermeture à n'importe quel
  budget testé** (jusqu'à 1 000 000 de nœuds), le mur structurel `max_result` ≈ 74 (§4.4)
  laissant toujours ~184 cases vides sur les possibilités les plus profondes délivrées
  aujourd'hui. Réglable en configuration (console `prunerDfsBudget <n>` + fichier de
  configuration client `dfs_budget`) comme prévu, mais désactivé par défaut (`0`) — un
  opt-in à activer manuellement si le profil de profondeur change (`GET
  /api/v1/stock-distribution` reste l'outil pour le vérifier sur un serveur réel avant
  d'activer), pas une valeur à deviner a priori.
- **Cumul des élagages.** Les gains ne s'additionnent pas : 4.1 et 4.2 attrapent en partie
  les mêmes branches. Chaque PR doit être mesurée **par-dessus** la précédente, jamais
  contre `master`.

## 7. Découpage en PR

Chaque étape est indépendante, mesurable isolément et réversible. Protocole de mesure
commun : `tests/bench/bench_search.sh --baseline <rapport précédent>`, en relevant
**débit (nœuds/s), taux d'élagage et nœuds atteints** — un taux qui bouge sans que le
débit bouge signale un changement d'arbre exploré, ce qui est précisément l'effet
recherché ici.

| PR | Contenu | Risque | Décision attendue |
|---|---|---|---|
| 1 | ~~**4.1** forward-check sur les voisines (+ réétiquetage de `fc_pruned_at[]`, + A/B)~~ **livré** | faible | pas de fenêtre résiduelle (mesuré, §4.1) |
| 2 | ~~**4.4** conflit de singletons dans le même balayage~~ **écarté** | faible | non rentable (mesuré, §4.4 : −9 %, 0 déclenchement) |
| 3 | ~~**4.2** contrainte de type coin/bord/intérieur (compteurs)~~ **écarté** | faible | non rentable (mesuré, §4.2 : −14 %, 0 déclenchement) ; partition de l'arène reste ouverte |
| 4 | ~~**4.3** comptage global couleur (implémentation complète)~~ **écarté** | faible | non rentable (mesuré, §4.3 : −24 %, recoupe le forward-check malgré ≈47-49 % de déclenchement) |
| 5 | ~~**4.6a** point fixe dans le balayage du pruner~~ **livré** | faible | rentable (mesuré, §4.6a : stock réel 1193→950 en 1 appel contre 2 pour `master`, ~1,7× de surcoût par appel largement absorbé) |
| 6 | ~~**4.6b** DFS à budget dans le pruner (+ réglage du budget)~~ **implémenté, testé, désactivé par défaut** | moyen | code conservé (opt-in), 0 % de fermeture sur stock réel à n'importe quel budget testé (mesuré, §4.6b) — mur structurel `max_result` ≈ 74, cause identique à §4.4 |
| 7 | ~~**4.8** ordre des candidats dans l'arène (expérience)~~ **adopté** | faible | `rare_first` adopté inconditionnellement (mesuré, §4.8 : +3,2 %, taux d'élagage changé mais `max_result` inchangé) |
| 8 | **4.5** propagation des forcées dans la boucle chaude | moyen | après clarification d'`alloc` |
| 9 | **4.7** ordre dynamique MRV | élevé | à rouvrir au vu des mesures 1–8 |

L'ordre 1→4 est un ordre de **rapport gain/coût décroissant présumé**, pas une dépendance :
seules 8 (qui suppose `alloc` clarifié) et 9 (à arbitrer en dernier) sont contraintes.

## Annexe — script des mesures de §3

Reproductible depuis la racine du dépôt, sans compilation ni exécution du solveur.

```python
import re, statistics
src = open('src/app/static_variables.c').read()
grab = lambda n: [int(v) for v in re.search(
    r'uint8_t ' + n + r'\[ETERN_PARTS\] = \{(.*?)\};', src, re.S
).group(1).replace('\n', '').split(',') if v.strip()]
dirx, diry = grab('dirx'), grab('diry')
N, K = 16, 6
idx = {(x, y): c for c, (x, y) in enumerate(zip(dirx, diry))}
ring = lambda x, y: x in (0, N - 1) or y in (0, N - 1)

tot = cov = 0
lags = []
for c, (x, y) in enumerate(zip(dirx, diry)):
    fut = [idx[n] - c for n in ((x+1, y), (x-1, y), (x, y+1), (x, y-1))
           if n in idx and idx[n] > c]
    tot += len(fut)
    cov += sum(1 for d in fut if d <= K)
    lags.append(max(fut) if fut else 0)
print("voisines futures couvertes par K=%d : %d/%d" % (K, cov, tot))
print("retard : mediane %d  max %d" % (statistics.median(lags), max(lags)))

placed, fronts = set(), []
for x, y in zip(dirx, diry):
    placed.add((x, y))
    fronts.append(len({n for (px, py) in placed
                       for n in ((px+1, py), (px-1, py), (px, py+1), (px, py-1))
                       if n in idx and n not in placed}))
print("frontiere : max %d  moyenne %.1f" % (max(fronts), statistics.mean(fronts)))

ring_idx = [c for c, (x, y) in enumerate(zip(dirx, diry)) if ring(x, y)]
early = [c for c, (x, y) in enumerate(zip(dirx, diry))
         if not ring(x, y) and c < max(ring_idx)]
print("cadre boucle a l'index %d ; %d cases interieures avant"
      % (max(ring_idx), len(early)))
print("detection au plus tard : mediane %d  max %d" % (
    statistics.median([max(r for r in ring_idx if r > c) - c for c in early]),
    max(max(r for r in ring_idx if r > c) - c for c in early)))

rows = [l.split() for l in open('data/pieces.csv').read().splitlines()[1:] if l.strip()]
P = [(int(r[0]), [int(v) for v in r[1:5]]) for r in rows]
frame = {pid for pid, f in P if 0 in f}
fc = {c for pid, f in P if pid in frame for c in f}
ic = {c for pid, f in P if pid not in frame for c in f}
print("couleurs exclusives au cadre :", sorted(fc - ic))
print("pieces cadre/interieures : %d/%d" % (len(frame), len(P) - len(frame)))
```

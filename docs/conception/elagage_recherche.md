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
PR 8 (§4.5, propagation des cases forcées) implémentée sans concession, testée
(intégration exhaustive sur le vrai puzzle 4×4, même ensemble de solutions activée/
désactivée), mesurée et **écartée** — code absent de `master` : −40,4 % de débit, et
`max_result` légèrement inférieur à débit égal (73 contre 74), le mécanisme recoupant le
forward-check au point de doubler son coût de lookup par placement sans que la réduction de
branchement ne compense au mur structurel actuel. PR 9 (§4.7, ordre dynamique MRV) :
**prototype scopé concluant** (délégation désactivée, non déployable en l'état) — `max_result`
74 → 180 à 5 M nœuds — puis **implémentation complète livrée et mesurée favorable** (PR 10)
(le critère décisif étant le coût de RÉFUTATION sur stock réel, pas `max_result` — voir
PR 10 pour le détail) : coût du choix de case ramené d'un balayage naïf à une frontière
comptée par `popcount` (23 k → 812 k nœuds/s), délégation rétablie par re-canonisation des
paquets émis, `max_result` **186** à 2 M nœuds — voir §4.7. Le « mur structurel » invoqué depuis §4.4
s'est révélé être un artefact du PROTOCOLE DE MESURE (mono-processus depuis la genèse), pas
une propriété de l'ordre fixe — voir la correction en §4.7 — ce qui rouvre §4.4, §4.5 et
§4.6b (tous écartés/désactivés pour cause de profondeur insuffisante) à une nouvelle mesure. La variante « partition de l'arène » de §4.2 reste une
proposition non implémentée. PR 11 (§4.10, moteur MRV pour la preuve bornée du pruner) **livrée en opt-in**
(`ETII_PRUNER_DFS_MRV=1`, défaut inchangé à l'époque) : mesurée à ×3–×4 de fermetures à budget égal sur
un stock de production de 126 287 possibilités — c'est la conséquence directe, côté pruner, du verdict de réfutation de PR 10. **MRV est depuis devenu le moteur UNIQUE, recherche et pruner, et les deux drapeaux ci-dessus ont été supprimés** — voir [docs/conception/mrv_moteur_unique.md](mrv_moteur_unique.md) (PR3).
§4.9 (table de région sur les zones d'angle,
et élimination par résolution d'un cadre complet) est **écartée sans implémentation** —
seule piste du document tranchée avant écriture de code, par quatre mesures statiques.

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

### 4.4 Conflit de singletons (mini-test de Hall) — REMESURÉ SUR STOCK RÉEL POST-PR 10, ÉCARTÉ POUR LA BONNE RAISON

**Statut : évalué, abandonné, remesuré sur stock réel — décision CONFIRMÉE, raison
CORRIGÉE.** Implémenté une première fois dans `bt_forward_check`, mesuré à 0
déclenchement sur le protocole synthétique du banc de débit, puis reverté. Ce qui suit
d'abord est la trace de ce raisonnement initial ; la correction post-PR 10 (même esprit
que celles de §4.6b et du « mur à 74 » de §4.7) suit immédiatement après, avant §4.5.

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

**Décision initiale : ne pas merger, garder la piste consignée pour plus tard.** Reprendre
uniquement une fois la profondeur réellement atteinte déplacée — remesurer si 4.7 (l'ordre
dynamique) y parvient un jour, plutôt que de raviver ce mécanisme en l'état. Voir la
correction ci-dessous : cette prémisse (« ne se déclenche jamais parce que la profondeur
atteignable reste trop faible ») était fausse, comme pour §4.6b et §4.7.

#### Correction (remesure post-PR 10) : le mécanisme se déclenche bien sur stock réel — mais jamais là où ça compte

**Même erreur de méthode que le « mur à `max_result` ≈ 74 » (§4.7) et la mesure originale de
§4.6b : le protocole `bench_search.sh` (mono-processus, depuis la genèse) ne construit jamais
de sous-arbre réellement profond dérivé d'une vraie délégation — il ne peut PAS voir un
mécanisme qui ne se déclenche que loin dans l'arbre, même si ce mécanisme se déclenche
réellement ailleurs.** Réimplémenté derrière `singleton_conflict_check`
(`src/app/static_variables.h`, défaut 0) : contrairement à §4.7/§4.6b, le mécanisme vit dans
`bt_forward_check`, PARTAGÉ par les deux moteurs de recherche — armé, il s'applique aussi bien
à l'ordre fixe qu'à MRV, sans code dupliqué. Ajouté au banc de réfutation comme quatrième
variante (`fixe+singleton`, `tests/bench/bench_refutation.c --engines`).

**Mesure 1 — fermeture bornée (protocole de §4.6b) : le mécanisme tire, sans aucun effet sur
la fermeture.** Sur 120 racines réelles, budget 5 000 000 nœuds : `fixe` et `fixe+singleton`
ferment exactement les MÊMES 20 racines, avec des totaux de nœuds identiques au nœud près
(1 326 277 pour les deux). Pourtant `fc_singleton_conflict` — un compteur direct, pas une
inférence — rapporte **35 056 déclenchements** sur cette même exécution : le mécanisme tire
bien, mais exclusivement dans les 100 racines qui dépassent le budget de toute façon, jamais
dans les 20 qui ferment. Reproduit sur un second stock (client MRV) : 134 565 déclenchements,
avec cette fois un effet marginal et dans le bruit sur le sous-ensemble commun (−0,06 % de
nœuds).

**Mesure 2 — débit agrégé sur l'échantillon entier (même instrument que la mesure originale,
sur stock réel plutôt que synthétique) : coût confirmé, cohérent avec 2024.**

| Stock | `fixe` (nœuds/s) | `fixe+singleton` (nœuds/s) | Delta |
|---|---|---|---|
| Client à ordre fixe, 120 racines, budget 5 M | 11 775 963 | 10 500 749 | **−10,8 %** |
| Même stock, 60 racines, budget 10 M | 10 898 683 | 9 861 136 | **−9,5 %** |
| Client MRV, 120 racines, budget 5 M | 15 746 462 | 13 949 110 | **−11,4 %** |

Trois mesures indépendantes, deux stocks, un coût de **−9,5 à −11,4 %** — cohérent avec le
−9 % mesuré à l'origine. La combinaison des deux mesures explique précisément ce que
l'original ne pouvait pas voir : le mécanisme se déclenche réellement (des dizaines de
milliers de fois par échantillon), mais uniquement au fond de sous-arbres trop grands pour
être fermés dans les budgets testés — jamais là où une preuve de fermeture aurait pu en
profiter — tout en payant son coût de balayage (un deuxième candidat cherché au lieu de
s'arrêter au premier) sur CHAQUE appel, y compris les ~98 % qui ne mènent à rien.

**Verrous de correction.** Trois tests directs de `bt_forward_check` (doit tirer sur deux
voisines exigeant le même id ; ne doit jamais tirer sur des ids distincts, même tous deux
singletons — pas de faux positif ; drapeau bas ⇒ comportement historique inchangé,
`tests/core/test_etii_search.c`) plus le même verrou d'intégration que §4.7/§4.6b (exhaustif
4×4, armé/désarmé, même nombre de solutions, sur les DEUX moteurs puisque le drapeau leur est
commun).

**Décision : la décision de ne pas fusionner ce mécanisme est CONFIRMÉE, mais la raison
invoquée à l'origine (« ne se déclenche jamais ») est CORRIGÉE.** Contrairement à §4.6b (où
la correction a RENVERSÉ la conclusion : le mécanisme s'est révélé bénéfique), ici la
correction affine le diagnostic sans changer le verdict — le plus proche parent de ce
résultat dans ce document est §4.3 (comptage global couleur : « tire énormément, pas
rentable »), pas §4.4 tel qu'il était compris jusqu'ici. Code non réintroduit dans le chemin
de production ; `singleton_conflict_check` reste un drapeau de mesure, comme
`global_dead_check`.

### 4.5 Propagation des cases forcées dans la boucle chaude — IMPLÉMENTÉ, MESURÉ, ÉCARTÉ

**Statut : implémenté sans concession, testé, mesuré, abandonné** (code absent de
`master`). Contrairement à §4.2/§4.4 (compteurs volontairement affaiblis) mais comme §4.3
(comptage global couleur) : ce n'est pas un problème d'implémentation partielle — le
mécanisme est correct et déclenche substantiellement — c'est un **coût par nœud qui
dépasse largement le bénéfice**, et par une marge plus large qu'aucune des pistes
précédentes de ce document.

**Principe.** Le pruner place les pièces des cases à candidat unique
([`possibility_all_has_a_next_counted`](../../src/core/possibility.c)) ; avant cette PR,
**la recherche ne le faisait pas**. Chaque placement forcé supprime un niveau de
branchement *et* resserre les clés de ses voisines, ce qui cascade.

**Levée de l'ambiguïté `alloc` (préalable, avant toute ligne de code).** Un audit complet
de tous les sites lisant/écrivant `possibility_packet.alloc` (etii_search.c, possibility.c,
datamanager.c, best_board.c, le réseau) a confirmé que la sémantique « `alloc` = curseur de
parcours, PAS un compte de pièces posées » est **déjà** celle du reste du code —
`check_possibility` exige `faceused >= alloc`, jamais `==` ; `normalize_possibility_packet`
documente explicitement que « les cases remplies au-delà de `alloc` sont conservées, les
moteurs les traitent comme des indices fixes » ; `possibility_all_has_a_next_counted`
(le pruner) pose déjà des pièces forcées hors ordre sans avancer `alloc` sauf plateau
complet. Aucun site n'utilise `alloc` pour piloter une décision d'exploration — seul
`alloc >= ETERN_PARTS` (un test de curseur pur, jamais faux tant que la boucle visite
séquentiellement chaque position) décide quoi que ce soit. La seule zone grise identifiée
était `max_result`/`best_board_try_record`, qui lisent `board.alloc` comme s'il s'agissait
du nombre réel de pièces posées — une convention déjà documentée ailleurs dans le code
(`http_codec.h`, sur le endpoint `/api/v1/best-board` : « BORNE INFÉRIEURE du nombre de
pièces réellement posées »). La « levée d'ambiguïté » demandée s'est donc résolue en
**documentation** (un commentaire canonique sur le champ `alloc` dans `possibility.h`, plus
la note symétrique au site `max_result` d'`etii_search.c`), pas en changement de code —
aucun consommateur existant n'aurait pu casser.

**Implémentation.** `bt_propagate_forced` (nouvelle fonction, `etii_search.c`) : après un
placement de décision réussi (forward-check compris), une file bornée à `ETERN_PARTS`
cases est ensemencée avec les voisines géométriques de la case posée — même périmètre que
`bt_forward_check` (§4.1), jamais un balayage du plateau entier comme le fait le pruner.
Pour chaque case défilée : si son compartiment (via `map_bucket_packed`, comme le
forward-check) n'offre plus qu'**exactement un** candidat encore libre, la pièce est posée
immédiatement, ses propres voisines sont enfilées à leur tour (cascade), et le placement
est journalisé sur une **piste partagée** `bt_forced_entry trail[ETERN_PARTS]` (position +
indice faceused). Un `bt_level.forced_start` par niveau (hauteur de la piste à l'entrée du
niveau) permet un défaire exact et symétrique du placement de décision existant : à chaque
retour en arrière sur un niveau, les cases forcées **depuis ce niveau** sont défaites en
même temps que sa propre pièce — bornant strictement la portée de l'annulation à ce que
CE niveau a produit. `bt_count_pending`/`bt_materialize_pending`/`bt_flush_pending`
(délégation/renvoi) ont dû être étendues pour dérouler/réappliquer les plages de piste par
niveau au même titre que `placed_pos`, sans quoi une pièce forcée par un niveau délégué
resterait marquée « utilisée » dans les paquets frères reconstruits. Aucun faux positif
possible : une case dont la cascade rencontre une impasse fait rejeter **tout** le
candidat de décision qui l'a déclenchée (annulation complète, y compris les cases déjà
forcées par cette même cascade) — condition nécessaire, jamais une heuristique, comme
l'exige §5.

**Garantie de correction.** Deux niveaux de verrou, comme pour les mécanismes précédents :
(a) trois tests directs de `bt_propagate_forced` (doit forcer une voisine à candidat
unique ; ne doit rien toucher à deux candidats libres ; doit défaire intégralement une
cascade qui rencontre une case morte — plateau, masque et piste restaurés à l'identique) ;
(b) un test d'intégration bout-en-bout sur le VRAI puzzle 4×4 (`pieces16.csv`) : exploration
exhaustive depuis la racine vide, deux fois — propagation activée puis désactivée — et
comparaison du **nombre de solutions enregistrées**, qui doit être rigoureusement
identique. C'est un verrou plus fort qu'un simple « ça compile et ça ne plante pas » : une
cascade qui rejetterait à tort une branche menant à une solution se serait traduite par un
comptage plus bas côté « activée ». Les deux comptages ont coïncidé.

**Mesuré** (`tests/bench/bench_search.sh`, puzzle 256, A/B à ordre alterné, 20 M nœuds × 5,
interrupteur de développement `ETII_FORCED_PROPAGATION` — même convention que
`ETII_ARENA_ORDER` pour §4.8, retiré avec le reste du code une fois la mesure faite) :

| Configuration | nœuds/s (médiane par run) | Moyenne sur 2 runs | Taux d'élagage forward-check | `max_result` (20 M nœuds) |
|---|---|---|---|---|
| Désactivée (référence post-§4.8) | 10 455 205 | **10 455 205** (1 run) | 42,19 % | 74 |
| Activée | 6 034 308 / 6 434 443 | **6 234 376** (2 runs) | ≈ 22,89 % | 73 |

**Activée : −40,4 % de débit** par rapport à désactivée (symétriquement, désactiver rapporte
**+67,7 %**) — un écart largement supérieur à tout ce que ce document a mesuré jusqu'ici, y
compris §4.3 (−24 %, la piste la plus proche par nature). Et contrairement à un mécanisme
qui ralentirait sans rien apporter, celui-ci **fait pire que rien à débit égal** :
`max_result` retombe à **73** au lieu de 74 pour le même budget de 20 M nœuds — non
seulement le mécanisme coûte cher, mais la progression réelle en pâtit aussi.

Le taux d'élagage forward-check ON (≈ 23 %) est presque exactement la **moitié** de OFF
(≈ 42 %) : la propagation forcée ne « ne se déclenche jamais » comme §4.2/§4.4 — elle
intercepte une part énorme de ce que le forward-check aurait autrement détecté lui-même
(une case forcée dont le seul candidat est en réalité un cas particulier d'une case morte
détectable plus tard). C'est le même phénomène que §4.3 (« ça tire énormément mais ce n'est
pas rentable »), en plus marqué : ici la piste **recoupe structurellement le forward-check
au point de le remplacer à moitié**, pour un coût par placement bien supérieur — chaque
case examinée par la cascade paie un `map_bucket_packed` de plus, sur un périmètre
**identique** aux voisines déjà inspectées par `bt_forward_check` juste avant dans le même
nœud (les deux fonctions ont délibérément été laissées séparées plutôt que fusionnées —
voir la piste ouverte ci-dessous), doublant de fait le nombre de lookups par placement pour
un gain de branchement qui ne compense pas au mur structurel actuel (`max_result` ≈ 74/256,
cf. §4.4/§4.6a/§4.6b) — le même plafond qui explique déjà pourquoi §4.4 et §4.6b ne
déclenchent jamais : ici le mécanisme déclenche bien, mais sur un arbre encore trop peu
profond pour que la réduction de branchement rembourse son coût de détection.

**Décision : ne pas merger.** Code entièrement retiré (comme §4.2/§4.3/§4.4), pas conservé
en configuration opt-in (contrairement à §4.6b, dont le coût est nul une fois désactivé) —
ici le coût de la vérification `alloc`/documentation était nul, mais le mécanisme
lui-même n'a aucune valeur mesurée à l'état actuel de l'arbre exploré, et le garder
« au cas où » ajouterait de la surface (nouveau champ `bt_level`, nouvelle piste partagée,
signatures étendues de 3 fonctions de délégation) sans bénéfice démontré. **Piste non
essayée qui pourrait changer la conclusion** : fusionner `bt_propagate_forced` dans
`bt_forward_check` lui-même (un seul passage sur les voisines, comptant jusqu'à 2 candidats
au lieu de s'arrêter au premier trouvé, comme le fait déjà la cascade) éliminerait
le doublon de lookups identifié comme la cause probable du surcoût — non tenté ici par
prudence sur une fonction déjà très optimisée et abondamment testée (cf. l'historique de
§4.1), et parce que rien ne garantit que la réduction de coût suffise à combler un écart de
cette ampleur. À reprendre uniquement avec cette fusion effectivement mesurée, et/ou une
fois l'implémentation complète de §4.7 (ordre dynamique MRV) livrée — son prototype déplace
déjà massivement le mur structurel (§4.7 : 74 → 180 à 5 M nœuds), ce qui changerait
significativement la profondeur atteignable et justifierait de remesurer §4.5 dans ce
nouveau régime — jamais en réactivant le mécanisme séparé en l'état.

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

**Décision initiale : conserver le code (correct, testé, sans coût si désactivé), mais
désactivé par défaut.** Différent de §4.2/§4.3/§4.4, dont le code a été retiré de `master` :
ici la garde `pruner_dfs_budget > 0` rend le mécanisme strictement gratuit une fois désactivé
(aucun appel à `search_packet_backtracking_budgeted`), donc le garder en configuration
opt-in ne coûte rien à l'état actuel du projet et évite de perdre le travail de
factorisation (`search_packet_backtracking_core`, réutilisée telle quelle si le mécanisme
redevient pertinent) si une PR ultérieure change la donne. `PRUNER_DFS_BUDGET_DEFAULT` vaut
donc **0** (désactivé). Cette décision reposait sur « aucune valeur de budget ne changerait la
conclusion tant que la profondeur atteignable reste bornée par ce mur » — voir la correction
ci-dessous : cette prémisse était fausse.

**Correction (remesure post-PR 10), même erreur que celle corrigée en §4.7 : « 0 % de
fermeture à n'importe quel budget » reposait sur un stock synthétique, pas sur du stock
réel.** La mesure originale ci-dessus générait son stock soit par `expand_datas_to_level(4)`
(profondeur `alloc` 4/4/4, un genèse à peine développée), soit par une recherche réelle
interrompue après quelques secondes (`alloc` jusqu'à 72 — c'est le même chiffre, et la même
confusion curseur/pièces posées, qui a produit le « mur à `max_result` ≈ 74 » corrigé en
§4.7). Rejoué avec `--pruner-profile` du banc de réfutation
(`tests/bench/bench_refutation.c`, voir [tests_et_ci.md](../tests_et_ci.md#mode---pruner-profile--rejoue-le-vrai-pipeline-du-pruner))
sur du VRAI stock serveur (17 815 possibilités, un client à ordre fixe délégant contre un
serveur `--expand-level 3`, exactement le pipeline réel `autoprune_step` : contrôle
superficiel puis, seulement si vivant et pas encore `checked`, la preuve DFS) :

| Budget DFS | Mortes au contrôle superficiel | + Fermées par DFS | Coût DFS cumulé (500 possibilités) |
|---|---|---|---|
| 0 | 50,2 % | — | — |
| 10 000 | 50,2 % | +4,6 pt | 2 277 016 nœuds, 0,195 s |
| 100 000 | 50,2 % | +5,2 pt | 22 457 285 nœuds, 1,887 s |
| 1 000 000 | 50,2 % | +5,6 pt | 221 842 743 nœuds, 18,590 s |

**La preuve DFS ferme bien des possibilités sur du stock réel** (+4,6 à +5,6 points de
pourcentage au-delà du contrôle superficiel gratuit, reproduit sur un second stock généré par
un client MRV : +2,3 à +5,7 points). Rendements décroissants nets : 10 000 nœuds capture 82 %
du gain mesuré à 1 000 000 pour 1 % du coût CPU — un point de départ raisonnable si le
mécanisme est réactivé.

**Décision : la correction ne change PAS `PRUNER_DFS_BUDGET_DEFAULT` (reste 0), pour la même
raison de prudence de déploiement que §4.7 (`MRV_DEFAULT_ENABLED`).** Le mécanisme est
mesurément bénéfique, mais activer un défaut consomme plus de CPU sur toute une flotte
déployée sans confirmation en conditions réelles au-delà de ce banc — décision explicitement
laissée à l'opérateur, `prunerDfsBudget <n>` restant le moyen de l'activer sans reconstruire
(valeur recommandée par la mesure ci-dessus : `10000`). Ce que la correction change, c'est le
diagnostic : le mécanisme n'a jamais été inutile, il a été désactivé sur la foi d'une mesure
dont le stock n'était pas représentatif.

Le mode GPU ([`gpu_pruner.cu`](../../src/app/gpu_pruner.cu)) ne suit pas sur (b) — un DFS
divergent par thread convient mal au modèle SIMT. (a) lui est en revanche transposable, mais
ne l'a pas encore été : `prune_kernel` reproduit toujours la version **une seule passe**
antérieure au correctif point-fixe de (a) ci-dessus — écart documenté depuis mais jamais
chiffré jusqu'à `--pruner-profile --gpu` (`tests/bench/bench_refutation.c`, voir
[tests_et_ci.md](../tests_et_ci.md#mode---pruner-profile---gpu-rejoue-le-vrai-pipeline-gpu)).
Sur un stock serveur réel (8438 possibilités, 8 à 73 pièces posées, Jetson Orin Nano) : le
contrôle GPU une passe élimine **21,4 %** de l'échantillon contre **32,2 %** pour le CPU
point fixe — **910 possibilités (10,8 %)** que le GPU garde vivantes et que le CPU point
fixe élimine (la cascade non rattrapée par une seule passe, exactement l'écart que (a)
corrige côté CPU), et **zéro** possibilité éliminée à tort par le GPU (aucun faux mort :
la condition nécessaire tient toujours, seule la complétude du contrôle diffère). Débit
mesuré sur ce même banc : le contrôle GPU par lots plafonne à ~69 000–74 000
possibilités/s (invariant à la taille de lot, 100 à 8438) contre ~217 000 possibilités/s
pour le contrôle CPU séquentiel du même banc — à cette échelle de lot, le forward-check par
possibilité (194 cases examinées en moyenne) est trop bon marché pour amortir le lancement
kernel et `cudaDeviceSynchronize`, donc le CPU séquentiel va plus vite que le GPU sur ce
contrôle précis. Porter (a) au GPU réduirait l'écart de 10,8 points sans le mécanisme DFS
(toujours écarté du GPU pour la raison SIMT ci-dessus) ; ni l'un ni l'autre n'est fait à ce
stade.

### 4.7 Ordre de variable dynamique (MRV) — implémenté et mesuré favorable (PR 10), devenu le moteur unique (PR3 de mrv_moteur_unique.md)

**Statut : implémenté, testé, mesuré favorable, puis promu moteur UNIQUE.** La bascule
décrite comme « pas encore le défaut de déploiement » ci-dessous a depuis été faite :
voir [docs/conception/mrv_moteur_unique.md](mrv_moteur_unique.md) (PR3) — `mrv_enabled`/
`ETII_MRV` et le moteur à ordre fixe qu'il sélectionnait ont été supprimés, MRV est
désormais le seul moteur de recherche. Le récit de la mesure ci-dessous est conservé tel
quel (post-mortem de décision valide), il ne décrit plus l'état actuel du code.

**Suite mesurée (postérieure à la bascule) — la restriction à la frontière n'était qu'un
test, pas une structure.** Telle qu'implémentée en PR 10, la « restriction à la frontière »
évitait le *lookup* des cases non contraintes mais pas leur *visite* : `mrv_choose_cell`
balayait toujours les 256 cases à chaque nœud pour n'en retenir ~29. Compté en accès
mémoire, repérer la frontière (256 lectures de grille + ~182 de clés) coûtait donc plus
cher que la compter (~29 × 4 `popcount`). Corrigé en énumérant la frontière depuis deux
masques de bits maintenus incrémentalement (`bt_frontier`, `src/core/etii_search.c`),
l'ordre des bits étant choisi (`pos = x * ETERN_SIZE + y`) pour reproduire à l'identique
l'ordre `for x { for y }` du balayage — donc le même départage d'égalité, donc le même
arbre. Mesuré `tests/bench/bench_search.sh` (5 M nœuds × 5 répétitions, puzzle 256,
écart-type relatif 0,3-0,7 %) : **917 554 → 1 000 844 nœuds/s, +9,08 %**, taux d'élagage
(38,7344 %) et `max_result` (190) inchangés. C'est le seul cas de la série où le débit est
une mesure honnête : la transformation ne touche pas l'arbre exploré, donc `max_result` à
budget de nœuds égal est un contrôle de non-régression strict et non un simple garde-fou.
Détail : [docs/autosearch_step.md](../autosearch_step.md) §1.3 quater.

Traité en deux temps, ce que la section garde en trace parce que le raisonnement de la
première étape est ce qui a justifié d'investir dans la seconde : un **prototype scopé**
d'abord (PR 9, ci-dessous), volontairement dégradé (balayage naïf, aucune délégation) mais
suffisant pour trancher « le levier paie-t-il ? » ; puis l'**implémentation complète**
(PR 10, § dédié en fin de section) qui lève les deux limites. `mrv_enabled` (`ETII_MRV`,
`src/app/static_variables.h`) reste à 0 par défaut (`MRV_DEFAULT_ENABLED`) : décision de
DÉPLOIEMENT, pas verdict de mesure — basculer le défaut change le moteur de recherche de
toute une flotte déployée, ce qui appelle plus de recul que ce qu'une seule PR peut
apporter. `ETII_MRV=1` l'active sans reconstruire.

**Principe.** Choisir à chaque nœud la case vide **la plus contrainte** au lieu de suivre
`directions[]`. C'est le levier le plus massif connu en résolution de CSP, et le seul de
ce document qui change la forme de l'arbre plutôt que d'en couper des branches.

**Pourquoi un prototype scopé plutôt que l'implémentation complète.** Le coût de
l'implémentation complète (cache de contraintes fournissant un *choix* et non plus
seulement une clé, re-canonisation des paquets délégués aux frontières, potentiel bump de
`VERSION`) est élevé et ne se justifie que si le levier paie RÉELLEMENT sur ce puzzle —
une question factuelle, pas une évidence a priori. Décision : mesurer le levier seul
d'abord, sur une implémentation volontairement dégradée mais correcte, avant d'investir
dans l'interopérabilité.

**Implémentation du prototype (PR 9 — remplacée depuis par `search_packet_backtracking_mrv`,
cf. PR 10 en fin de section ; ce paragraphe décrit l'état au moment de la mesure).**
`search_packet_backtracking_mrv_experiment` (alors nouvelle fonction, `etii_search.c`) : structurellement une copie de
`search_packet_backtracking_core` (même plateau unique modifié en place, même pile de
décisions, même forward-check après placement), mais où `mrv_choose_cell` remplace
`dirx[depth]/diry[depth]` — elle balaie **tout le plateau** (pas seulement les voisines
géométriques comme `bt_forward_check`) et choisit la case vide dont le compartiment offre
le MOINS de candidats encore disponibles. Sous-produit gratuit de ce balayage : toute case
sans AUCUN candidat, où qu'elle soit sur le plateau, est détectée immédiatement — un test
de mort strictement plus complet qu'un forward-check local. La pile de décisions
(`mrv_level`, isolée de `bt_level`) mémorise la case choisie par niveau (`x`, `y`), puisque
plus rien ne la déduit de la position dans `directions[]`. **Délégation
INCONDITIONNELLEMENT désactivée** dans cette branche : les paquets matérialisés
supposeraient l'ordre fixe `dirx[]/diry[]`, que cette variante ne suit plus — sur
`REQUEST_STOP`, le travail restant est simplement abandonné (comme la variante budgétée
§4.6b). Activée uniquement par la variable de développement `ETII_MRV=1` (même convention
que `ETII_ARENA_ORDER`/`ETII_FORCED_PROPAGATION`), désactivée par défaut : `mrv_enabled`
fait basculer `search_packet_backtracking` vers le prototype, sans toucher au chemin de
production par ailleurs.

**Garantie de correction.** Deux tests directs de `mrv_choose_cell` (choisit la case au
compartiment le plus restreint ; détecte une case sans issue), et surtout le même verrou
d'intégration que pour §4.5 : exploration exhaustive du VRAI puzzle 4×4 depuis la racine
vide, MRV activé puis désactivé, comparaison du nombre de solutions enregistrées —
rigoureusement identique dans les deux cas.

**Mesuré** (`tests/bench/bench_search.sh`, puzzle 256, `data/pieces.csv`) :

| Configuration | Nœuds/s (médiane) | `max_result` à 5 M nœuds | `max_result` à 500 k nœuds |
|---|---|---|---|
| Ordre fixe (référence) | 8 971 039 | **74** | — |
| MRV (balayage naïf) | 23 177 | **180** | **173** |

**Débit : −99,7 %** — attendu et sans surprise : le balayage naïf de `mrv_choose_cell`
coûte un `map_bucket_packed` par case VIDE du plateau à CHAQUE nœud (jusqu'à 256, contre
1,9 en moyenne pour `bt_forward_check`), très loin d'un cache incrémental. Ce n'est PAS la
mesure qui compte ici — c'est `max_result` : **74 → 180 à 5 M nœuds** (+143 %), et déjà
**173 à seulement 500 k nœuds** (35× moins de nœuds que le budget de référence). Le mur
structurel documenté depuis §4.4 (`max_result` ≈ 74, confirmé sur des centaines de
millions de nœuds par §4.4/§4.5/§4.6a/§4.6b) n'est **pas** une propriété du puzzle — c'est
une conséquence de l'ordre de parcours fixe. Le choisir dynamiquement le déplace
massivement, presque instantanément (quelques centaines de milliers de nœuds suffisent),
confirmant sans ambiguïté la prémisse du principe MRV pour ce puzzle précis.

Le taux d'élagage forward-check reste quasiment inchangé (43,4 % → 43,1 %/43,3 %) : la
détection de case morte du balayage MRV lui-même (sous-produit gratuit, cf.
implémentation) recoupe largement ce que le forward-check aurait trouvé de toute façon —
la vraie valeur de MRV n'est pas dans l'élagage, elle est dans la FORME de l'arbre exploré.

**Décision (PR 9) : ne PAS fusionner ce prototype (délégation désactivée = non déployable),
mais NE PAS l'écarter non plus** — il documente une direction VALIDÉE, pas un échec ;
implémentation complète recommandée comme projet séparé, sur trois axes : (1) cache de
candidats remplaçant le balayage naïf O(256), (2) re-canonisation aux frontières de
délégation, (3) arbitrage d'un bump de `VERSION`. **Ces trois axes sont traités ci-dessous
(PR 10) et le moteur est implémenté, testé, mesuré favorable.** (Note : au moment de PR3,
la bascule décrite comme « pas encore le défaut » a été faite — MRV est le moteur unique,
voir [docs/conception/mrv_moteur_unique.md](mrv_moteur_unique.md).)

#### PR 10 — implémentation complète, mesurée favorable (devenue le moteur unique en PR3)

**Statut : livrée, mesurée favorable, puis promue moteur UNIQUE (PR3).**
`search_packet_backtracking_mrv` (`src/core/etii_search.c`) remplace le prototype de mesure.
Le paragraphe suivant décrit l'état d'AVANT PR3 : `mrv_enabled` (`MRV_DEFAULT_ENABLED`,
`static_variables.h`) restait à **0** — ordre fixe par défaut, décision de DÉPLOIEMENT
distincte du verdict de mesure : basculer le défaut changeait le moteur de recherche de
toute une flotte déployée, ce qui appelait plus de recul que ce qu'une seule PR pouvait
apporter. `ETII_MRV=1` activait l'ordre dynamique sans reconstruire. Ce drapeau, et le
moteur à ordre fixe qu'il sélectionnait, ont depuis été supprimés (PR3 de
mrv_moteur_unique.md) une fois la mesure jugée favorable dans les deux usages (recherche et
pruner) — le paragraphe ci-dessous, conservé pour le contraste historique : contrairement
à §4.1/§4.8 (leviers adoptés dont l'interrupteur a été retiré une fois le défaut basculé),
l'interrupteur est ici structurel plutôt que temporaire : le protocole de mesure du banc (§7)
impose de pouvoir comparer PAR-DESSUS l'état précédent, et `search_packet_backtracking_core`
(ordre fixe) reste de toute façon vivante — c'est elle que rejoue la preuve bornée du
pruner (§4.6b).

**Axe 1 — coût du choix de case : frontière + `popcount`, pas de cache incrémental.**
Le balayage naïf du prototype coûtait, à CHAQUE nœud, un `map_bucket_packed` *et un
parcours complet du compartiment* pour chacune des ≤ 256 cases vides. Deux changements,
aucun des deux n'étant le « compte incrémental par case » envisagé en PR 9 :

- **Restriction à la frontière.** Une case dont les 4 côtés valent `all_face` n'est
  contrainte par rien (ni bord de plateau, ni voisine posée) : elle accepte *toutes* les
  pièces libres et ne peut donc jamais être le minimum tant qu'une case contrainte existe.
  Le test se lit dans le cache `constraints[][]` déjà maintenu par §4.1, sans aucun lookup.
  Frontière : 29 cases en moyenne, 52 au pire (§3.2), contre 256 balayées.
  L'existence d'au moins une case de frontière tant qu'une case vide existe se démontre
  (la première case vide en ordre lexicographique a soit un bord de plateau, soit une
  voisine de rang inférieur nécessairement remplie) — un repli couvre malgré tout le cas.
- **Comptage indépendant de la taille du compartiment.** `bucket_id_mask`
  (`build_bucket_id_mask`, `src/core/part.c`) est une TROISIÈME représentation redondante
  de `flat`, dans le même esprit que `packed` : le masque de bits des ids présents dans
  chaque compartiment, indexé par offset d'arène (0,46 Mo sur le puzzle 256, tableau creux
  — voir sa doc pour l'arbitrage contre une table indexée par clé, 10,6 Mo). Le nombre de
  pièces encore libres devient `popcount(masque & ~utilisées)`. Le masque des pièces
  utilisées du plateau est miroité en mots de 64 bits (`mrv_used_init`/`_set`/`_clear`),
  construit explicitement par décalages — jamais par réinterprétation mémoire de
  `b_faceused`, qui dépendrait de l'endianness.

**Pourquoi PAS le cache incrémental de la PR 9.** L'axe (1) tel qu'envisagé (« un compte de
candidats par case, mis à jour à chaque `bt_propagate_place`/`_undo` ») bute sur la
difficulté que la PR 9 identifiait déjà : un changement de `faceused` peut modifier le
compte de N'IMPORTE quelle case dont le compartiment contient la pièce concernée, pas
seulement des voisines. La maintenance incrémentale coûte donc, elle aussi, O(frontière)
par placement (un test d'appartenance par case suivie) — c'est-à-dire le même ordre de
grandeur que le recalcul à la demande une fois celui-ci ramené à quelques `popcount`. Le
cache incrémental n'aurait acheté qu'un facteur constant, au prix d'une symétrie
pose/retrait supplémentaire à maintenir exacte (et de la même classe de bug que §4.5 :
une annulation incomplète est invisible en test et fausse la recherche). Renoncé
délibérément ; à reconsidérer seulement si une mesure montre le choix de case redevenu
dominant.

**Axe 2 — re-canonisation aux frontières de délégation.** `bt_canonicalize_packet` rétablit,
sur chaque paquet matérialisé, `alloc` = index de la PREMIÈRE case vide dans l'ordre
`directions[]` (et recale `x`/`y`) — c'est-à-dire exactement ce que
`normalize_possibility_packet` sait déjà faire, réutilisé plutôt que réécrit. Les cases
remplies au-delà du curseur sont le cas déjà prévu et documenté du format (« indices fixes »,
traitées comme des niveaux sans décision par le moteur à ordre fixe). Conséquence :
`bt_count_pending`, `bt_materialize_pending`, `bt_delegate_if_needed` et `bt_flush_pending`
sont **partagés à l'identique** par les deux moteurs (un paramètre `dynamic_order`, et
`bt_level` porte désormais la case `(x, y)` du niveau au lieu de la déduire de
`dirx[depth]`) — une seule sémantique de délégation, testée une seule fois.

**Axe 3 — `VERSION` : pas de bump.** Découle de l'axe 2 : un paquet cédé par un client MRV
est indiscernable d'un paquet produit en ordre fixe. Une flotte mixte (clients MRV, clients
à ordre fixe, pruners, fichiers `.back` existants) partage le même serveur sans changement
de protocole — contrairement à ce qu'envisageait la version initiale de cette section.

**Garantie de correction.** Trois niveaux, dans l'esprit de §5 :
(a) `bucket_id_mask_matches_flat_for_every_key` (`tests/core/test_part.c`) : équivalence
exhaustive du comptage par `popcount` avec un comptage par parcours, sur TOUTES les clés et
trois états de plateau — une divergence changerait silencieusement la case choisie et,
pire, le test de mort ; (b) `bt_canonicalize_packet_*` et
`bt_materialize_pending_dynamic_order_emits_canonical_packets`
(`tests/core/test_etii_search.c`) : le curseur d'un paquet délégué désigne bien le premier
trou du parcours, jamais la profondeur de pile ; (c) surtout,
`search_backtracking_mrv_delegation_preserves_solution_count` : sur le VRAI puzzle 4×4,
exploration MRV depuis la racine vide **interrompue en cours de route**, travail restant
repris **à ordre FIXE** jusqu'à épuisement (en vérifiant `check_possibility` et le caractère
déjà canonique de chaque paquet reçu), nombre total de solutions rigoureusement égal à celui
d'une exploration exhaustive à ordre fixe. Ce dernier test échoue bien sur le code
sans re-canonisation (vérifié en la neutralisant), comme l'exige la règle de test du projet.
L'ancien verrou de PR 9 (`search_backtracking_mrv_preserves_solution_count`, même ensemble de
solutions quel que soit l'ordre) est conservé tel quel.

**Mesuré** (`tests/bench/bench_search.sh`, puzzle 256, 2 M nœuds × 3 répétitions, i9-9880H) :

| Configuration | Nœuds/s (médiane) | Taux d'élagage FC | `max_result` à budget de nœuds identique |
|---|---|---|---|
| Ordre fixe (`ETII_MRV=0`) | 6 102 866 | 44,15 % | 74 |
| MRV, prototype PR 9 | 23 177 | 43,3 % | 173 (à 500 k nœuds) |
| **MRV, implémentation complète** | **811 617** | 43,18 % | **186** |

**Débit : −86,7 % contre l'ordre fixe, mais ×35 contre le prototype** (23 k → 812 k nœuds/s)
— les deux axes de coût ci-dessus ont bien porté. Et, comme en PR 9, **ce n'est pas la
mesure qui compte** : à temps mural égal, l'ordre fixe explore ~10× plus de nœuds
(15 M nœuds en 1,58 s, contre 2 M en 2,46 s pour MRV) et **reste à `max_result` = 74** —
le même plafond que sur des centaines de millions de nœuds ailleurs dans ce document. MRV
atteint 186 en 2,46 s, et 188 à 50 M nœuds. Le taux d'élagage du forward-check bouge à peine
(44,15 % → 43,18 %) : la valeur de MRV n'est pas dans l'élagage, elle est dans la FORME de
l'arbre exploré — exactement ce que concluait la PR 9.

**Mesure de RÉFUTATION — la bonne question, posée après coup.** `max_result` reste un proxy
discutable : descendre loin dans une branche n'est pas l'objectif du solveur, prouver tôt
qu'une possibilité est morte l'est. La mesure correspondante est le coût de FERMETURE d'un
sous-arbre (nœuds/temps jusqu'à `BT_CORE_EXHAUSTED`) à racine IDENTIQUE entre les deux
moteurs — c'est ce que mesure `tests/bench/bench_refutation.c` (`make bench-refutation`,
cf. [tests_et_ci.md](../tests_et_ci.md#banc-de-réfutation-make-bench-refutation)). Sur un
VRAI stock serveur (17 815 possibilités, 8 à 153 pièces posées, moyenne 34,5 — produit par
un serveur `--expand-level 3` alimenté 60 s par un client à ordre fixe), plafond 5 M nœuds :

| Bande (pièces posées) | Fermées, ordre fixe | Fermées, MRV | Nœuds sur les racines fermées par les DEUX |
|---|---|---|---|
| 20–45 | 5/10 | **10/10** | 70 vs 22 |
| 55–89 | 16/16 | 16/16 | 40 804 vs **32** (×1 275) |
| ≥ 90 | 17/25 | **25/25** | 565 677 vs **25** (×22 627) |

MRV ferme donc des racines que l'ordre fixe ne ferme pas du tout dans le plafond, et coûte
des ordres de grandeur moins cher sur celles que les deux ferment. **Mais l'ordre MRV n'est
pas uniformément meilleur** : sur des racines FABRIQUÉES (préfixes d'une descente MRV
profonde, c'est-à-dire des sous-arbres réellement vivants, que le stock réel fournit
rarement), à 100 pièces posées, l'ordre fixe ferme en 73 482 nœuds là où MRV en dépense
4 443 906 — 60× plus. Les deux faits sont vrais et doivent être cités ensemble. À noter
aussi qu'une grande partie des réfutations MRV coûtent **1 nœud** : la possibilité était
déjà morte à sa création et le balayage de `mrv_choose_cell` le voit immédiatement — le même
test que `possibility_all_has_a_next_counted` (le pruner), mais à chaque nœud plutôt qu'une
fois. Sur ces racines-là, la comparaison mesure surtout la présence de ce test global, pas
la qualité de l'ordre.

**Ablation : ce qui revient à l'ORDRE et ce qui revient à la PORTÉE de la détection.** Les
deux moteurs confondaient deux axes — l'ordre fixe va toujours avec une détection de case
morte LOCALE (`bt_forward_check`, 4 voisines), l'ordre dynamique toujours avec une détection
GLOBALE (le balayage de `mrv_choose_cell` voit toute case morte du plateau). Le drapeau
`global_dead_check` (`static_variables.h`, défaut 0, coût nul désarmé) remplit la case
manquante : ordre FIXE + balayage global, en appelant exactement le même balayage que MRV et
en JETANT le choix de case. Verrouillé par
`search_backtracking_global_dead_check_preserves_solution_count` (exploration exhaustive du
4×4, même nombre de solutions armé ou non — le balayage est une condition nécessaire, il ne
doit coûter aucune solution). KPI à **temps CPU égal** (~22 s par moteur, 120 racines
échantillonnées régulièrement dans le stock réel, plafond calibré par moteur) :

| moteur | plafond/racine | fermées | temps total | fermetures/s |
|---|---|---|---|---|
| ordre fixe | 2 500 000 nœuds | 20/120 | 21,97 s | 0,91 |
| ordre fixe + balayage global | 275 000 nœuds | 52/120 | 23,02 s | 2,26 |
| MRV | 500 000 nœuds | **79/120** | 22,67 s | **3,48** |

**Les deux axes comptent, aucun n'est redondant** : le balayage global seul fait 20 → 52
(×2,6), l'ordre dynamique ajoute 52 → 79 (×1,5). L'hypothèse « tout l'effet vient du test
global, l'ordre ne sert à rien » — que les réfutations à 1 nœud rendaient plausible — est
donc réfutée. Sur les 19 racines fermées par les trois : 295 339 nœuds (fixe), 124 030
(fixe+global), **40** (MRV). À noter que fixe+global explore moins de nœuds que fixe mais met
plus de temps : le balayage coûte ~10× un nœud ordinaire — c'est le prix que MRV paie aussi,
et rentabilise.

Sur les racines FABRIQUÉES (préfixes vivants, `--seed-nodes 200000 --depths 100,110,120`), la
conclusion s'inverse et l'ablation en donne la cause : fixe 155 902 nœuds / 0,012 s,
fixe+global 134 218 / 0,112 s, MRV 4 523 856 / 5,094 s. C'est bien l'**ordre** qui coûte là
(le balayage n'élague presque rien de plus que le forward-check local sur ces racines). Le
gain de MRV tient donc à la structure du stock réel — beaucoup de possibilités déjà mortes ou
presque — pas à une supériorité de l'ordre en toutes circonstances.

**Correction importante, établie APRÈS coup sur une flotte réelle.** La lecture « le mur à
`max_result` ≈ 74 était un artefact de l'ORDRE de parcours » est **fausse**, et ce document
l'a propagée depuis §4.4. Un client à ordre FIXE lancé contre un vrai serveur (256 pièces,
`--expand-level 3`, 3 forks, 60 s) atteint `max_result` = **186**, et le stock qu'il délègue
contient des paquets à **153 pièces posées** — très au-delà de 74. Le mur est un artefact du
PROTOCOLE DE MESURE : `tests/bench/bench_search.sh` tourne en mode `test`, mono-processus,
depuis la genèse, sans stock ni délégation — une seule descente en profondeur qui reste
piégée dans le sous-arbre le plus à gauche. Dès qu'un serveur répartit le travail (expansion
+ délégation), n'importe quel moteur atteint des profondeurs bien supérieures. Ce qui reste
vrai et vérifié : à protocole de mesure IDENTIQUE (le banc), MRV atteint 186 là où l'ordre
fixe plafonne à 74. Ce qui est faux : en déduire une propriété des moteurs hors du banc.
Conséquence directe : les décisions de §4.4 et §4.6b, motivées par « la profondeur atteinte
est trop faible pour que ce mécanisme joue », reposaient sur une profondeur mesurée dans ces
conditions-là — à remesurer sur du stock réel (le banc de réfutation le fait déjà : l'ordre
fixe y ferme 20 racines sur 120, ce que §4.6b concluait impossible). **Fait depuis** : §4.4 a
été remesuré (correction dans sa propre section — le mécanisme tire réellement sur stock réel,
mais jamais dans les sous-arbres fermables, décision confirmée) et §4.6b aussi (correction
inverse : mesurément bénéfique). §4.5 (propagation des cases forcées) reste non remesuré.

**Ce que cette adoption a rouvert.** §4.4 (conflit de singletons), §4.5 (propagation des cases
forcées) et §4.6b (DFS à budget du pruner) ont tous les trois été écartés ou désactivés pour
la MÊME raison : le mur structurel à `max_result` ≈ 74 les rendait soit muets (0
déclenchement), soit non rentables. Ce mur vient de bouger d'un facteur 2,5. §4.4 et §4.6b ont
été remesurés depuis (voir leurs sections). §4.5 reste à REMESURER, avec le même protocole que
la première fois (§7 :
par-dessus l'état courant, jamais contre `master`) — §4.6b en particulier, dont le code est
resté en place derrière `pruner_dfs_budget` justement pour ce cas de figure.

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

### 4.9 Table de région sur les zones d'angle (« pattern database ») — ÉVALUÉE ET ÉCARTÉE, SANS IMPLÉMENTATION

**Statut : proposition tranchée par la mesure seule.** Aucune ligne de moteur écrite,
aucune PR. C'est la première piste de ce document écartée sans implémentation — non par
prudence, mais parce que quatre mesures statiques, toutes reproductibles en moins d'une
seconde et sans exécuter le solveur, suffisent à montrer que le mécanisme ne peut pas se
déclencher. Le raisonnement est conservé ici pour qu'il n'ait pas à être refait.

**Origine de la proposition.** Énumérer, pour chaque angle, **tous** les remplissages
valides du bloc 3×3 qui va du coin jusqu'à l'indice — les indices officiels posés par
`first_possibility` (208 en (2,2), 255 en (13,2), 181 en (2,13), 249 en (13,13)) tombent
exactement sur la case opposée au coin de chacun de ces blocs. On disposerait alors des
pièces qui « répondent » à chaque zone, et on ne poursuivrait une possibilité que si elle
correspond à l'un des cas répertoriés. Extension proposée : une fois les 4 tables
calculées, éliminer davantage en tentant de résoudre un **cadre complet**.

**Ce que c'est, dans le vocabulaire de ce document.** Une contrainte **en extension** sur
une région — une table de région, ou *pattern database*. C'est la seule famille absente du
tableau de §2, qui ne connaît que des tests par case. Elle est **exacte** au sens de §5 :
la table relâche la contrainte « pièce déjà utilisée ailleurs sur le plateau », donc elle
contient toujours au moins les remplissages réellement atteignables, et « absent de la
table » est une réfutation valide. Et elle voit, en principe, des impasses qu'aucun test
local ne peut voir : §3.1 mesure que le forward-check ignore des voisines jusqu'à 152
niveaux plus loin.

#### Mesure 1 — taille des tables : la piste est parfaitement tractable

Énumération exhaustive du bloc 3×3 d'angle (pièces distinctes, couleurs cohérentes, gris
sur le bord du plateau) :

| Zone | Remplissages valides |
|---|---|
| Angle 3×3, sans indice | **2 633 221** (0,12 s à énumérer, ~30 Mo à stocker) |
| Angle (0,0), indice 208 r3 en (2,2) | **3 215** |
| Angle (15,0), indice 255 r3 en (13,2) | **3 891** |
| Angle (0,15), indice 181 r3 en (2,13) | **3 447** |
| Angle (15,15), indice 249 r0 en (13,13) | **3 029** |

Sans indice, une seule table sert les 4 coins : aucune contrainte positionnelle ne les
distingue. Avec les indices, chaque coin a la sienne, et l'indice divise le nombre de cas
par ~780. Croissance mesurée : **×11 par case de cadre, ×2,6 par case intérieure**, d'où
3×4 ≈ 1,9·10⁸ et 4×4 ≈ 3,8·10¹⁰ — le 3×3 est la dernière taille stockable, et il l'est
confortablement. Produit des 4 tables : 1,31·10¹⁴ ; taux de disjonction des 4 zones mesuré
à 0,90 %, soit **~1,2·10¹² configurations 4-zones réellement compatibles**.

#### Mesure 2 — coût d'interrogation : rien ne bloque non plus de ce côté

Signature de frontière d'une zone 3×3 = les 6 couleurs sortantes (2 de cadre, 4
intérieures) :

| Grandeur | Valeur |
|---|---|
| Espace a priori des signatures | 2 088 025 (bitset de 261 Ko) |
| Signatures atteignables | 995 005 → **52,3 % de rejet** (prior uniforme) |
| Zones partageant une signature | moyenne 2,6, **maximum 30**, 96 % ≤ 7 |

L'absence de queue lourde est le point important : le test **exact** — signature → 2 ou 3
zones candidates → vérification du masque des pièces déjà utilisées — coûte une poignée
d'accès, pas un balayage. Techniquement, la piste est donc entièrement réalisable.

#### Mesure 3 — le verrou : la table ne peut jamais être interrogée utilement

Deux faits, indépendants de la taille de la table.

**(a) Une zone complète et valide est toujours dans la table**, par construction. Tester
l'appartenance d'une zone déjà remplie apporte exactement zéro information. La seule
requête qui informe est « existe-t-il une complétion, sachant les couleurs que le reste du
plateau réclame ? », ce qui exige que la **frontière soit connue avant que la zone soit
remplie**.

**(b) `directions[]` l'interdit.** Index de parcours mesurés (script en annexe) :

| Angle | Zone 3×3 remplie aux index | Frontière entièrement connue à l'index | Faces de frontière connues avant la fin de la zone |
|---|---|---|---|
| (0,0) | 0-4, 72-75 | 92 | 2 / 6 |
| (15,0) | 15-23 | 98 | 1 / 6 |
| (15,15) | 34-42 | 86 | 1 / 6 |
| (0,15) | 53-61 | 80 | 1 / 6 |

À chaque fois la zone est terminée **20 à 75 niveaux avant** que sa frontière existe, et la
ou les rares faces connues avant sont celles de voisines directes — donc déjà imposées par
la clé 4D. **Déclenchement attendu : 0.** C'est le mode d'échec de §4.2 et §4.4, atteint
ici avant d'avoir écrit le mécanisme.

La variante qui collerait à l'ordre actuel — précalculer « zone extensible d'un anneau » et
filtrer aux index 23/42/61/75 — tombe pour une autre raison : le nombre attendu
d'extensions d'un 3×3 vers un 4×4 est ~1,4·10⁴, donc la probabilité qu'une zone n'en
admette aucune est négligeable. Rien ne meurt à cet endroit.

#### Mesure 4 — l'élimination par résolution d'un cadre complet

En creusant la proposition, une reformulation apparaît, qui vaut d'être consignée
indépendamment du verdict :

> Chaque pièce de bordure porte une face grise ; les deux faces qui l'encadrent sont des
> couleurs de **cadre**. Une pièce de bordure est donc une **arête** d'un multigraphe à 5
> sommets, et **un cadre complet est exactement un circuit eulérien** de ce multigraphe (60
> arêtes), avec les 4 coins aux positions 0/15/30/45 du circuit.

Mesuré sur `data/pieces.csv` : 5 sommets (couleurs 18 à 22), 60 arêtes, **tous les degrés
égaux à 24**, graphe connexe. Un multigraphe 24-régulier à 5 sommets possède un nombre
astronomique de circuits eulériens — le cadre est structurellement libre, et la mesure
directe le confirme :

| | Zones sans indice | Zones avec les vrais indices |
|---|---|---|
| Quadruplets de zones disjointes testés | 300 | 300 |
| Cadre complet trouvé | 300 | 298 |
| **Cadre impossible** | **0** | **0** |
| Indécis (cap de nœuds) | 3 | 2 |
| Nœuds pour trouver un cadre | min 41 / **médiane 62** / p90 322 | — |

Médiane 62 nœuds pour placer 40 cases : quasiment aucun retour arrière. Et même à supposer
un taux d'échec non nul, éliminer une *zone* Z exigerait que **tous** les quadruplets
contenant Z échouent — hors d'atteinte à 100 % de réussite.

**Piège de mesure rencontré, à ne pas reproduire.** La première version du test enchaînait
les 4 côtés du cadre **sans backtracking entre eux** : un côté résolu gloutonnement volait
une pièce au suivant. Elle annonçait **9,7 % de quadruplets impossibles** — un artefact
intégral, tombé à 0 % dès que le backtracking a couvert les 40 cases d'un seul tenant. Un
test d'élimination mal implémenté produit exactement le symptôme qu'on espère de lui ;
c'est le pendant, côté mesure statique, de la règle de §5 sur le test unitaire à deux
volets.

#### Pourquoi cette famille ne pouvait pas payer — le modèle de branchement

Nombre attendu de candidats pour une case intérieure à 2 faces contraintes = `4R/17²`, où
`R` est le nombre de pièces intérieures encore disponibles (196 pièces, 4 rotations, 17
couleurs intérieures) :

| Profondeur (cases posées) | 60 | 100 | 140 | 180 | **186** | 200 | 220 |
|---|---|---|---|---|---|---|---|
| `R` restant | 196 | 156 | 116 | 76 | 70 | 56 | 36 |
| Candidats attendus | 2,71 | 2,16 | 1,61 | 1,05 | **0,97** | 0,78 | 0,50 |

Le branchement croise 1 vers la profondeur **~186** — la valeur de `max_result` mesurée
sous MRV en §4.7. C'est un modèle, pas une preuve, mais il donne le critère : **au-dessus
de ~180, les impasses sont exponentiellement rares**, donc aucun élagage local ne peut y
rentabiliser son coût. Or les zones d'angle et le cadre sont la région la moins profonde du
parcours (les 60 cases de cadre sont bouclées à l'index 74, §3.2). Les deux moitiés de la
proposition — table de zones et consistance du cadre — sont des raisonnements exacts et
corrects qui travaillent là où **rien ne meurt**. Ils n'ont rien à réfuter.

#### Décision : ne pas implémenter

Ni la table de zones, ni le filtre par cadre. Le verdict ne tient pas à un coût mesuré
trop élevé (contrairement à §4.3 ou §4.5) mais à une **impossibilité de déclenchement**
établie avant écriture, et il couvre par le même argument les variantes voisines (zone
4×4, zone ancrée sur l'indice, table de la bande frontière côté angles). Ne pas
reproposer sans avoir d'abord invalidé la mesure 3.

**Ce qui reste utilisable de l'analyse :**

- **La reformulation eulérienne du cadre** est un fait structurel du puzzle, indépendant de
  cette piste : le cadre ne porte aucune information discriminante, et tout mécanisme qui
  espère réfuter par le cadre est voué au même sort.
- **Le modèle de branchement** ci-dessus donne un critère d'admission bon marché pour les
  futures pistes : une piste qui ne s'applique qu'au-dessus de la profondeur ~180 n'a
  pratiquement rien à réfuter, quelle que soit sa force théorique. À rapprocher du
  diagnostic corrigé de §4.4 (« se déclenche mais jamais là où ça compte »).
- **Une seule condition rouvrirait la famille : un ordre de parcours où une zone peut être
  entourée avant d'être remplie.** C'est structurellement possible en MRV (§4.7), où
  l'ordre est dynamique. La mesure à faire, et la seule, serait d'instrumenter le moteur
  MRV pour compter les occurrences « zone d'angle entièrement entourée, encore incomplète ».
  Si ce compteur est nul ou marginal, la famille est close définitivement ; s'il ne l'est
  pas, la mesure 2 dit que le test exact serait bon marché. Ne pas retoucher `directions[]`
  pour provoquer artificiellement cette situation : bump de `VERSION` (§5).

### 4.10 Moteur de la preuve bornée du pruner : MRV plutôt qu'ordre fixe — devenu permanent (PR3 de mrv_moteur_unique.md)

**Statut : implémenté, testé, opt-in (`ETII_PRUNER_DFS_MRV=1`), puis rendu PERMANENT.**
Voir [docs/conception/mrv_moteur_unique.md](mrv_moteur_unique.md) (PR3) : `pruner_dfs_mrv`/
`ETII_PRUNER_DFS_MRV` et l'ordre fixe qu'il pouvait sélectionner ont été supprimés — la
preuve bornée du pruner emploie MRV inconditionnellement. Le récit ci-dessous (mesure
opt-in, défaut inchangé) décrit l'état d'AVANT cette bascule, conservé tel quel comme
post-mortem de décision.
Piste ouverte par une question d'exploitation : sur les machines les plus performantes,
serait-il rentable d'élaguer davantage les possibilités en cours d'étude, pour éliminer au
plus tôt ? La réponse tient en trois constats, dont le troisième est cette PR.

**Constat 1 — « régulier » n'est pas le bon axe.** Une possibilité est un état de plateau
FIGÉ : rien d'extérieur ne peut la tuer plus tard. Repasser le MÊME test dessus ne rendra
jamais rien de nouveau — le seul cas qui rendait quelque chose (la cascade de forçages non
rattrapée par une passe unique) est déjà réglé par le point fixe de §4.6a. Ce qui peut
changer d'un passage à l'autre, c'est la FORCE du test : contrôle superficiel → preuve
bornée à budget croissant. Un approfondissement itératif, pas une périodicité.

**Constat 2 — fermer un sous-arbre ne fait pas gagner de nœuds à la flotte, à moteur
égal.** Une preuve qui FERME dans un budget B démontre que le sous-arbre fait ≤ B nœuds :
c'est exactement ce que le client de recherche aurait dépensé pour le fermer lui-même. À
moteur identique, l'élagage préalable DÉPLACE le travail, il ne le supprime pas. Ses gains
propres sont ailleurs, et ils sont réels : le volume de stock (50,2 % de mortes au contrôle
gratuit, +4,6 pt au DFS 10 000 — plafond RAM, débordement disque et `consistent_backup` en
moins), et l'absence de prolifération (la preuve bornée interdit la délégation, donc un
sous-arbre condamné qu'elle absorbe ne recrache pas ses frères dans le stock, contrairement
au même sous-arbre exploré par un client de recherche).

**Constat 3 — le seul gain CPU massif est l'ASYMÉTRIE DE MOTEUR, et le code ne
l'exploitait pas.** `search_packet_backtracking_budgeted` appelait
`search_packet_backtracking_core` de façon INCONDITIONNELLE : la preuve du pruner tournait
en ordre fixe même sur un binaire lancé avec `ETII_MRV=1`. Or la mesure de réfutation de
§4.7 dit précisément l'inverse de ce que cet appel supposait — à temps CPU égal, sur du
vrai stock serveur : 3,48 fermetures/s pour MRV contre 0,91 pour l'ordre fixe, et 40 nœuds
contre 295 339 sur les racines fermées par les deux. Fermer un sous-arbre est le MÉTIER du
pruner, pas un effet de bord : c'est exactement le KPI sur lequel MRV gagne le plus.

**Ce que fait la PR.** Un seul point de bascule, `pruner_dfs_mrv`
(`src/app/static_variables.{h,c}`, défaut `PRUNER_DFS_MRV_DEFAULT` = 0), lu par
`search_packet_backtracking_budgeted` seule. Résolu une fois au démarrage depuis
`ETII_PRUNER_DFS_MRV` dans `main()`, AVANT tout `fork()` (invariant de résolution pré-fork
du projet), donc hérité à l'identique par tous les fils. Aucune commande console, aucune
entrée `cli_topics[]` : c'est un levier par MACHINE (« telle machine tourne en pruner
MRV »), pas un réglage à changer en cours de route.

**Volontairement indépendant de `mrv_enabled`.** Un process n'a qu'un rôle (recherche OU
pruner), donc un seul drapeau aurait suffi fonctionnellement — mais les deux usages n'ont
ni le même métier (réfuter vs. explorer) ni le même verdict de mesure : MRV est mesuré
favorable pour la réfutation sans l'être uniformément pour l'exploration de sous-arbres
encore VIVANTS (§4.7 : 60× plus cher que l'ordre fixe sur des racines fabriquées). Les
confondre ferait qu'un futur basculement de l'un emporterait silencieusement l'autre, et
interdirait l'A/B exigé par le protocole §7.

**Le risque connu de MRV est ici borné par construction.** Le contre-exemple de §4.7 (les
racines encore vivantes, où l'ordre dynamique s'enfonce) coûte, dans ce contexte, au plus
`pruner_dfs_budget` nœuds : la preuve échoue et l'appelant retombe sur le comportement
historique. C'est le seul endroit de ce document où le mauvais cas de MRV est plafonné.

**Aucune conséquence de protocole.** La preuve bornée ne délègue rien (`allow_delegate = 0`)
et ne modifie pas la possibilité contrôlée : seul son VERDICT sort, et il a la même
signification et la même exactitude dans les deux ordres (même sous-arbre, seul l'ordre des
décisions change). Pas de bump de `VERSION`, flotte mixte inchangée.

**Garantie de correction** (§5), trois volets :
- `search_backtracking_budgeted_mrv_closes_when_budget_suffices` /
  `_returns_budget_when_insufficient` (`tests/core/test_etii_search.c`) : un plateau où la
  fermeture DOIT être prouvée, un où elle NE DOIT PAS l'être ;
- `autoprune_step_dfs_budget_mrv_closes_possibility` : l'intégration réelle dans le pipeline
  du pruner (contrôle superficiel puis preuve), verdict et compteurs identiques à la
  variante à ordre fixe ;
- surtout `search_backtracking_budgeted_both_engines_agree_on_4x4` : sur le VRAI puzzle 4×4,
  la preuve bornée jouée depuis la racine vide rend le MÊME verdict
  (`BT_CORE_EXHAUSTED`) avec les deux moteurs, ET la solution est enregistrée dans les deux
  cas — un élagage est une condition nécessaire, il ne doit jamais coûter une solution. Le
  test vérifie aussi que le coût en nœuds DIFFÈRE entre les deux moteurs : un drapeau
  inopérant (qui routerait vers le même moteur) échoue explicitement dessus.

**Mesure.** L'instrument existe déjà : `--pruner-profile` du banc de réfutation rejoue le
pipeline réel `autoprune_step`. L'option `--pruner-dfs-mrv` lui a été ajoutée pour l'A/B —
même stock, même budget, seul le moteur de la preuve change :

```sh
make bench-refutation BENCH_REFUT_ARGS="--from-back temp.back --pruner-profile 500 --budget 10000"
make bench-refutation BENCH_REFUT_ARGS="--from-back temp.back --pruner-profile 500 --budget 10000 --pruner-dfs-mrv"
```

**Mesuré — stock de RÉFÉRENCE : 126 287 possibilités d'un serveur de production**, produites
par de vrais clients (et non par `expand_datas_to_level`, dont les possibilités sont des
suites d'expansion peu représentatives). Échantillon de 2 000 prises 1 sur 63, comparaison
APPARIÉE : même stock, même échantillon, même budget, seul le moteur de la preuve change.
Contrôle superficiel identique aux six lignes : 22,1 % de mortes, 237 cases examinées par
possibilité.

| Budget DFS | Moteur | Fermées par la preuve | Total éliminé | Nœuds DFS | Temps |
|---|---|---|---|---|---|
| 1 000 | ordre fixe | 8,3 % | 30,4 % | 1 404 859 | 0,129 s |
| 1 000 | **MRV** | **34,8 %** | **56,8 %** | 888 745 | 1,081 s |
| 10 000 | ordre fixe | 10,0 % | 32,0 % | 13 746 435 | 1,177 s |
| 10 000 | **MRV** | **35,6 %** | **57,7 %** | 8 540 962 | 8,998 s |
| 100 000 | ordre fixe | 11,7 % | 33,8 % | 133 804 294 | 11,521 s |
| 100 000 | **MRV** | **36,0 %** | **58,1 %** | 84 108 383 | 85,575 s |

**Quatre lectures, dont une qui contredit une hypothèse de départ :**

1. **×3 à ×4 de fermetures à budget égal** (×4,2 à 1 000, ×3,6 à 10 000, ×3,1 à 100 000),
   soit **+24 à +26 points de stock éliminé** : 32,0 % → 57,7 % au budget 10 000. Sur un
   stock de 126 287 possibilités, l'écart représente ~32 000 possibilités que l'ordre fixe
   laisse en circulation et que MRV retire.

2. **Le plafond de l'ordre fixe n'est pas une question de budget.** Multiplier le budget par
   100 lui fait gagner 3,4 points (8,3 → 11,7 %) ; MRV en gagne 1,2 (34,8 → 36,0 %). LES DEUX
   moteurs plafonnent — mais pas au même niveau, et aucun budget ne comble l'écart. Ce que
   MRV achète n'est donc pas de la vitesse, c'est un **niveau d'élimination inatteignable
   autrement**.

3. **DOMINATION STRICTE de `MRV@1000` sur `fixe@100000`** : 56,8 % contre 33,8 % de stock
   éliminé, en **1,08 s contre 11,5 s** — 1,7× plus d'élimination pour 10,7× moins de CPU.
   C'est la comparaison qui tranche, et le budget d'exploitation qu'elle désigne est
   **1 000** (pour les deux moteurs, d'ailleurs : au-delà, chacun paie ×100 pour quelques
   points).

4. **Correction — MRV ne coûte PAS moins cher par fermeture sur ce stock.** À budget égal :
   1,56 ms par fermeture contre 0,77 ms pour l'ordre fixe (budget 1 000), soit ~2× PLUS. Et
   en fermetures par seconde de CPU, l'ordre fixe est même devant à chaque budget (1 295/s
   contre 643/s à budget 1 000). Ce ratio-là est trompeur pris isolément : il compare des
   moteurs qui ne s'arrêtent pas au même endroit. L'ordre fixe ferme vite les sous-arbres
   FACILES et bute ensuite sur un plafond ; MRV ferme aussi les autres, plus chers par
   nature. Le KPI qui décide est le **coût pour atteindre un niveau d'élimination donné**
   (lecture 3), pas le débit de fermetures — un `fixe@1000` très rapide qui laisse 70 % du
   stock en circulation ne rend pas le service attendu d'un pruner.

**Mesure secondaire, stock plus petit et différemment produit** (3 658 possibilités, serveur
`--expand-level 3` alimenté ~3 min par un client à ordre fixe, conteneur 4 cœurs — c'est le
stock de la première version de cette section) : même conclusion, amplitude plus forte
encore — 12,6 % → 60,2 % de fermetures à budget 1 000, total éliminé 23,0 % → 70,6 %, et là
MRV coûtait AUSSI moins cher par fermeture (0,49 ms contre 0,84 ms). L'écart entre les deux
stocks tient à leur composition (10,4 % de mortes au contrôle superficiel et 370 cases
examinées par possibilité contre 22,1 % et 237) : un stock d'expansion contient beaucoup de
sous-arbres presque morts, un stock de clients contient des possibilités plus avancées et
plus dures. **C'est le stock de production (126 287) qui fait foi** ; le petit ne sert plus
qu'à montrer que le sens du résultat ne dépend pas du mode de production du stock.

**Décision : opt-in, défaut inchangé** — même prudence de déploiement que
`MRV_DEFAULT_ENABLED` (§4.7) et `PRUNER_DFS_BUDGET_DEFAULT` (§4.6b), et pour une raison
supplémentaire propre à cette PR : le mécanisme est de toute façon inerte tant que
`pruner_dfs_budget` vaut 0, c'est-à-dire tant que l'opérateur n'a pas déjà pris la première
décision. Recommandation d'exploitation pour une machine puissante dédiée au prunage :
`ETII_PRUNER_DFS_MRV=1` + `prunerDfsBudget 1000` (et non `10000`, la valeur de §4.6b : les
deux moteurs plafonnent au-delà de 1 000 nœuds, et `MRV@1000` domine strictement
`fixe@100000` — lecture 3 ci-dessus), après vérification du profil de profondeur du stock
(`GET /api/v1/stock-distribution`).

**Ce que cette PR ne fait PAS, et pourquoi.** Le « repassage à budget croissant » du
constat 1 n'est pas implémenté : il exigerait un PALIER mémorisé par possibilité (l'octet
`checked` pourrait le porter — toute valeur non nulle conserve la sémantique actuelle du
routage, `datamanager.c` testant `== 1`, donc sans bump de `VERSION`) et un chemin de retour
`checked → unchecked` (aujourd'hui `scroll_from_local_tocheck` ne sert que le pool non
vérifié, et une possibilité n'est donc contrôlée qu'une seule fois dans toute sa vie). Sans
palier mémorisé, chaque passage repaierait les nœuds du précédent. À mesurer séparément, et
seulement si le stock sature pendant que le CPU reste libre : les rendements de §4.6b sont
déjà nettement décroissants d'un palier au suivant (+4,6 pt à 10 000, +5,6 pt à 1 000 000).

**Corollaire à ne pas oublier — élaguer POUR SOI ne paie presque rien en MRV.** Le moteur
MRV embarque, à chaque nœud, le balayage global de case morte (`mrv_choose_cell`) : c'est le
même test que `possibility_all_has_a_next_counted`, joué en continu, et une grande partie
des réfutations MRV coûtent 1 nœud pour cette raison (§4.7). Pré-élaguer ce qu'une machine
MRV va étudier elle-même est donc largement redondant. Le rendement d'une passe d'élagage
supplémentaire est INVERSEMENT proportionnel à la force du moteur qui la suit : une machine
puissante élague utilement pour LES AUTRES (le stock serveur, avant distribution), pas pour
elle-même.

## 5. Arbitrages tranchés

- **Une condition nécessaire, jamais une heuristique.** Un faux positif jette
  silencieusement la solution et ne se manifeste par aucun symptôme observable. Chaque
  élagage est livré avec un test unitaire à deux volets : un plateau où il **doit** tirer,
  un plateau où il **ne doit pas** tirer. Aucune mesure de débit ne remplace ce verrou.
- **Pas de bump de `VERSION`**, y compris pour l'implémentation complète de 4.7 : tranché
  par la mesure et non par principe — les paquets délégués par un client MRV sont
  re-canonisés avant émission (`bt_canonicalize_packet`, §4.7/PR 10), donc indiscernables de
  ceux d'un client à ordre fixe. Un paquet reste un état de plateau ; seul le sous-ensemble
  exploré change. C'est le changement de `directions[]` qui avait forcé le bump v11, parce
  qu'il redéfinissait le sens d'`alloc`.
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
- **Pas de conflit de singletons — CONFIRMÉ sur stock réel, raison originale CORRIGÉE
  (§4.4).** Implémenté, mesuré une première fois à −9 % de débit / 0 déclenchement sur le
  protocole synthétique, reverté. Remesuré post-PR 10 sur stock réel via le banc de
  réfutation (`--engines fixe+singleton`) : le mécanisme tire réellement (35 056 à 134 565
  déclenchements selon le stock, sur des échantillons de 120 racines) mais EXCLUSIVEMENT
  dans des sous-arbres trop grands pour fermer dans les budgets testés — jamais d'effet sur
  le sous-ensemble de racines réellement fermées. Coût confirmé sur trois mesures
  indépendantes : −9,5 à −11,4 % de débit agrégé, cohérent avec la mesure de 2024. Décision
  inchangée (ne pas fusionner), diagnostic corrigé (« ne se déclenche jamais » → « se
  déclenche mais jamais là où ça compte »).
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
- **4.6b (DFS à budget du pruner) : code conservé, remesuré POST-PR 10, mesurément
  bénéfique — défaut de déploiement inchangé (`0`), décision opérateur.** La mesure
  originale (**0 % de fermeture** à n'importe quel budget testé) reposait sur un stock
  synthétique trop peu profond — même erreur de méthode que le « mur à `max_result` ≈ 74 »
  corrigé en §4.7. Rejouée sur du VRAI stock serveur via `--pruner-profile`
  (`tests/bench/bench_refutation.c`) rejouant le pipeline réel `autoprune_step` :
  la preuve DFS ferme **+4,6 à +5,6 points de pourcentage** au-delà du contrôle
  superficiel gratuit (déjà 50,2 % à lui seul), reproduit sur un second stock. Voir
  §4.6b pour le détail complet et la table de mesure. La garde reste gratuite une fois
  désactivée, donc le code reste opt-in ; `PRUNER_DFS_BUDGET_DEFAULT` reste à `0` pour la
  même raison de prudence de déploiement que `MRV_DEFAULT_ENABLED` (§4.7) — mesurément
  bénéfique n'est pas encore décidé comme défaut d'une flotte déployée.
- **4.10 (moteur de la preuve bornée du pruner) : MRV implémenté, opt-in
  (`ETII_PRUNER_DFS_MRV=1`), défaut inchangé.** `search_packet_backtracking_budgeted`
  appelait `search_packet_backtracking_core` inconditionnellement : la preuve du pruner
  tournait en ordre fixe même sous `ETII_MRV=1`, alors que la mesure de réfutation de §4.7
  désigne MRV comme le meilleur moteur pour ce travail précis. Mesuré à l'A/B
  (`--pruner-dfs-mrv` du banc) sur un stock de PRODUCTION de 126 287 possibilités produites
  par de vrais clients, échantillon de 2 000 : **×3 à ×4 de fermetures à budget égal**
  (10,0 → 35,6 points à budget 10 000 ; 32 % → 58 % du stock éliminé). Aucun budget ne comble
  l'écart — les deux moteurs plafonnent, à des niveaux différents (l'ordre fixe gagne 3,4
  points en multipliant le budget par 100). `MRV@1000` **domine strictement** `fixe@100000` :
  56,8 % contre 33,8 % de stock éliminé pour 10,7× moins de CPU, d'où un budget
  d'exploitation recommandé de 1 000. À noter, contre l'intuition : à budget égal MRV coûte
  ~2× PLUS par fermeture sur ce stock (1,56 ms contre 0,77 ms) — le débit de fermetures est
  un KPI trompeur ici, seul compte le coût pour atteindre un niveau d'élimination donné. Le mauvais cas connu
  de MRV (§4.7 : 60× plus cher sur des racines vivantes) est ici borné par
  `pruner_dfs_budget`. Drapeau distinct de `mrv_enabled` À DESSEIN : réfuter et explorer
  n'ont ni le même métier ni le même verdict. Défaut à 0 par prudence de déploiement, comme
  §4.6b/§4.7 — et de toute façon inerte tant que `prunerDfsBudget` vaut 0.
- **4.8 (ordre des candidats dans l'arène) : `rare_first` adopté, inconditionnel.**
  Mesuré au banc (20 M nœuds × 5 répétitions, A/B à ordre alterné, 3 baselines et 3
  mesures `rare_first` non recouvrantes) : **+3,2 % de débit médian moyen**, taux
  d'élagage forward-check modifié (45,6 % → 42,2 %, signe d'un arbre de forme différente)
  mais `max_result` inchangé (74, à N nœuds identique dans les 8 mesures) — un vrai gain
  de progrès, pas un artefact de mesure. `common_first` mesuré aussi (+2,1 % en moyenne,
  mais chevauchement partiel avec `rare_first`, signal moins net) : non retenu. Contraste
  avec §4.2/§4.3/§4.4/§4.6b : ici le mécanisme est gratuit ET bénéfique, donc adopté sans
  laisser d'interrupteur — cf. §4.8 pour le détail complet des mesures.
- **4.5 (propagation des cases forcées) : implémenté sans concession, testé, écarté.**
  Mesuré au banc (20 M nœuds × 5 répétitions, A/B à ordre alterné) : **−40,4 % de débit**,
  et `max_result` légèrement inférieur à budget de nœuds égal (73 contre 74) — pire que
  l'absence du mécanisme, pas seulement plus lent. Cas différent de tous les précédents :
  le taux d'élagage forward-check ON tombe à la MOITIÉ de OFF (≈23 % contre ≈42 %), preuve
  que le mécanisme intercepte réellement une grosse part de ce que le forward-check aurait
  détecté de toute façon — un recoupement structurel comme §4.3, mais avec un coût par
  placement bien plus élevé (chaque case forcée examinée paie un lookup en plus de celui
  déjà fait par `bt_forward_check` sur le même périmètre de voisines, les deux fonctions
  étant restées volontairement séparées). Code entièrement retiré, comme §4.2/§4.3/§4.4 —
  cf. §4.5 pour le détail complet et la piste de fusion non essayée qui pourrait changer
  cette conclusion.
- **4.7 (ordre dynamique MRV) : implémenté et mesuré favorable, devenu le moteur UNIQUE (PR3 de mrv_moteur_unique.md).** Prototype scopé d'abord
  (PR 9 : `max_result` 74 → **180** à 5 M nœuds, mais −99,7 % de débit et aucune délégation
  possible — conservé sans être déployé), puis implémentation complète (PR 10) : choix de
  case ramené d'un balayage naïf de tout le plateau à la seule frontière comptée par
  `popcount` (**×35 de débit** : 23 k → 812 k nœuds/s), délégation rétablie par
  re-canonisation des paquets émis (donc **aucun bump de `VERSION`**, flotte mixte
  possible), `max_result` **186** à 2 M nœuds contre 74 pour l'ordre fixe — lequel restait à
  74 même avec 10× plus de nœuds, c'est-à-dire plus de temps mural. `ETII_MRV=0` conservait
  l'ordre fixe pour les mesures A/B et un repli, jusqu'à sa suppression en PR3 (mrv_moteur_unique.md) :
  MRV est désormais le seul moteur, `mrv_enabled`/`ETII_MRV` n'existent plus. Conséquence à
  ne pas oublier : §4.4, §4.5 et §4.6b ont été écartés/désactivés à cause du mur à 74, qui a
  bougé — à remesurer, cf. §4.7.

- **Pas de table de région sur les zones d'angle, ni d'élimination par le cadre (§4.9).**
  Écartée **sans implémentation**, cas unique dans ce document : quatre mesures statiques
  suffisent. La table est pourtant tractable (2 633 221 remplissages du 3×3 d'angle sans
  indice, 3 029 à 3 891 avec les indices officiels) et bon marché à interroger (multiplicité
  maximale de 30 zones par signature de frontière) — mais elle ne peut jamais être
  interrogée utilement : une zone complète et valide est **toujours** dans la table, et
  `directions[]` termine chaque zone 20 à 75 niveaux **avant** que sa frontière existe.
  L'élimination par résolution d'un cadre complet ne rejette rien non plus : un cadre est un
  circuit eulérien d'un multigraphe 24-régulier à 5 sommets, 300/300 quadruplets de zones se
  complètent, médiane 62 nœuds. À ne pas reproposer sans avoir invalidé la mesure 3 de §4.9 ;
  la seule condition qui rouvrirait la famille est un ordre où une zone peut être entourée
  avant d'être remplie, c'est-à-dire MRV.

## 6. Points laissés ouverts

- ~~**4.2 : compteurs ou partition de l'arène ?**~~ Tranché par élimination : les compteurs
  (variante affaiblie, seule testée) sont écartés (§4.2, −14 % de débit, 0 déclenchement).
  La partition reste ouverte et devient la SEULE variante encore candidate — elle
  échappe au risque de fixture qui a motivé l'affaiblissement des compteurs, en empêchant
  la mauvaise pièce d'être candidate plutôt qu'en la rejetant après coup.
- ~~**4.1 : garder ou non la fenêtre `c+1 … c+2` ?**~~ Tranché : non — voir §4.1, mesure à
  l'appui (−0,06 point de taux d'élagage sans la fenêtre résiduelle, effet négligeable).
- ~~**4.4 : le conflit de singletons est-il rentable ?**~~ Tranché deux fois : non, en 2024
  sur protocole synthétique (0 déclenchement, −9 %) ; **remesuré post-PR 10 sur stock réel**
  (§4.4) — il se déclenche bien (dizaines de milliers de fois par échantillon) mais jamais
  dans un sous-arbre fermable, coût confirmé (−9,5 à −11,4 %). Même verdict, raison corrigée.
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
| 2 | ~~**4.4** conflit de singletons dans le même balayage~~ **écarté, raison corrigée post-PR 10** | faible | remesuré sur stock réel (§4.4) : se déclenche réellement (35 k–135 k fois/échantillon) mais jamais dans un sous-arbre fermable ; coût confirmé −9,5 à −11,4 % |
| 3 | ~~**4.2** contrainte de type coin/bord/intérieur (compteurs)~~ **écarté** | faible | non rentable (mesuré, §4.2 : −14 %, 0 déclenchement) ; partition de l'arène reste ouverte |
| 4 | ~~**4.3** comptage global couleur (implémentation complète)~~ **écarté** | faible | non rentable (mesuré, §4.3 : −24 %, recoupe le forward-check malgré ≈47-49 % de déclenchement) |
| 5 | ~~**4.6a** point fixe dans le balayage du pruner~~ **livré** | faible | rentable (mesuré, §4.6a : stock réel 1193→950 en 1 appel contre 2 pour `master`, ~1,7× de surcoût par appel largement absorbé) |
| 6 | ~~**4.6b** DFS à budget dans le pruner (+ réglage du budget)~~ **implémenté, testé, remesuré favorable post-PR 10, défaut inchangé** | moyen | code conservé (opt-in), mesure originale (0 % de fermeture) corrigée : +4,6 à +5,6 pt de fermetures sur stock réel via `--pruner-profile` (§4.6b) — `PRUNER_DFS_BUDGET_DEFAULT` reste 0, bascule laissée à l'opérateur comme pour `MRV_DEFAULT_ENABLED` |
| 7 | ~~**4.8** ordre des candidats dans l'arène (expérience)~~ **adopté** | faible | `rare_first` adopté inconditionnellement (mesuré, §4.8 : +3,2 %, taux d'élagage changé mais `max_result` inchangé) |
| 8 | ~~**4.5** propagation des forcées dans la boucle chaude~~ **écarté** | moyen | non rentable (mesuré, §4.5 : −40,4 %, `max_result` inférieur à budget égal) — recoupe le forward-check, coût de lookup doublé sur le même périmètre de voisines |
| 9 | ~~**4.7** ordre dynamique MRV (prototype scopé)~~ **concluant** | élevé | validé (mesuré, §4.7 : `max_result` 74→180 à 5 M nœuds) — délégation désactivée dans le prototype, non déployable en l'état ; cache incrémental + re-canonisation restent à faire |
| 10 | ~~**4.7** ordre dynamique MRV (implémentation complète)~~ **mesuré favorable ; devenu le moteur UNIQUE (PR3 de mrv_moteur_unique.md), `mrv_enabled`/`ETII_MRV` supprimés** | élevé | coût de réfutation ~4× meilleur sur stock réel à CPU égal (§4.7) — frontière + `popcount`, re-canonisation des paquets délégués, pas de bump de `VERSION` ; rouvre §4.4/§4.5/§4.6b |
| 11 | ~~**4.10** moteur MRV pour la preuve bornée du pruner (+ `--pruner-dfs-mrv` au banc)~~ **mesuré favorable ; rendu PERMANENT (PR3 de mrv_moteur_unique.md), `pruner_dfs_mrv`/`ETII_PRUNER_DFS_MRV` supprimés** | faible | mesuré ×3 à ×4 de fermetures à budget égal sur un stock de production de 126 287 possibilités (§4.10 : 32 % → 58 % de stock éliminé, `MRV@1000` dominant `fixe@100000`) ; budget recommandé 1 000 |

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

## Annexe — scripts des mesures de §4.9

Reproductibles depuis la racine du dépôt, sans compilation ni exécution du solveur.

**Comptage exact des zones d'angle ancrées sur les indices** (mesure 1) — retrouve
3 215 / 3 891 / 3 447 / 3 029 en ~0,3 s. L'indice n'est posé qu'en dernière case du
balayage, mais ses deux faces internes sont propagées en avance sur les deux voisines
(`(1,2)` et `(2,1)`), ce qui suffit à garder l'arbre minuscule.

```python
T, R, B, L = 0, 1, 2, 3
rows = [l.split() for l in open('data/pieces.csv').read().splitlines()[1:] if l.strip()]
f0 = {int(r[0]): [int(r[1]), int(r[4]), int(r[3]), int(r[2])] for r in rows}  # top,right,bottom,left
def rot(v, n):                       # part.c/rotatePart, n quarts de tour
    v = list(v)
    for _ in range(n):
        v = [v[L], v[T], v[R], v[B]]
    return v
tiles = [(i, r, rot(f0[i], r)) for i in sorted(f0) for r in range(4)]

# indices de first_possibility() : (piece, rotation, coin cx, cy, sens sx, sy)
HINTS = [(208, 3, 0, 0, 1, 1), (255, 3, 15, 0, -1, 1),
         (181, 3, 0, 15, 1, -1), (249, 0, 15, 15, -1, -1)]

for pid, prot, cx, cy, sx, sy in HINTS:
    BX, FX = (L, R) if sx > 0 else (R, L)
    BY, FY = (T, B) if sy > 0 else (B, T)
    hint = rot(f0[pid], prot)
    idx = {}
    for pi, pr, v in tiles:
        m = sum(1 << k for k in range(4) if v[k] == 0)   # masque des faces grises
        idx.setdefault((m, v[BX], v[BY]), []).append((pi, v))
    total, g = 0, [[None] * 3 for _ in range(3)]
    def rec(k, used):
        global total
        i, j = divmod(k, 3)
        if (i, j) == (2, 2):
            if pid not in used and hint[BX] == g[2][1][FX] and hint[BY] == g[1][2][FY]:
                total += 1
            return
        need_x = 0 if j == 0 else g[i][j - 1][FX]
        need_y = 0 if i == 0 else g[i - 1][j][FY]
        mask = ((1 << BX) if j == 0 else 0) | ((1 << BY) if i == 0 else 0)
        for pi, v in idx.get((mask, need_x, need_y), ()):
            if pi in used:                                continue
            if (i, j) == (1, 2) and v[FY] != hint[BY]:    continue   # face vue par l'indice
            if (i, j) == (2, 1) and v[FX] != hint[BX]:    continue
            g[i][j] = v
            rec(k + 1, used | {pi})
    rec(0, frozenset())
    print("coin (%2d,%2d) / indice %3d r%d : %6d remplissages" % (cx, cy, pid, prot, total))
```

**Géométrie du parcours** (mesure 3) — la mesure décisive : la zone est toujours terminée
avant que sa frontière existe.

```python
import re
src = open('src/app/static_variables.c').read()
grab = lambda n: [int(v) for v in re.search(
    r'uint8_t ' + n + r'\[ETERN_PARTS\] = \{(.*?)\};', src, re.S
).group(1).replace('\n', '').split(',') if v.strip()]
dirx, diry = grab('dirx'), grab('diry')
idx = {(x, y): c for c, (x, y) in enumerate(zip(dirx, diry))}
for cx, cy in ((0, 0), (15, 0), (15, 15), (0, 15)):
    sx, sy = (1 if cx == 0 else -1), (1 if cy == 0 else -1)
    zone = [idx[(cx + sx * a, cy + sy * b)] for a in range(3) for b in range(3)]
    bnd = [idx[(cx + sx * 3, cy + sy * b)] for b in range(3)] + \
          [idx[(cx + sx * a, cy + sy * 3)] for a in range(3)]
    print("coin (%2d,%2d) : zone finie a l'index %3d, frontiere connue a %3d, "
          "faces connues avant : %d/6"
          % (cx, cy, max(zone), max(bnd), sum(1 for b in bnd if b < max(zone))))
```

Les mesures 2 (signatures de frontière, table sans indice à 2 633 221 entrées) et 4
(complétion de cadre) demandent une énumération de plusieurs millions de nœuds : elles ont
été faites en C, hors dépôt, sur les mêmes conventions de faces que ci-dessus.

# Élagage de la recherche : pistes au-delà du forward-check

**Statut : en cours d'implémentation.** PR 1 (§4.1) livrée et mesurée — voir
[autosearch_step.md §1.3 ter](../autosearch_step.md#13-ter-bt_forward_check--les-voisines-de-la-pièce-posée-pas-une-fenêtre-de-parcours)
pour le comportement actuel. PR 2 (§4.4) évaluée et **écartée** après mesure (code absent
de `master`, raisonnement et chiffres conservés en §4.4). Les pistes 3 à 9 restent des
propositions non implémentées.

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

### 4.2 Contrainte de type coin / bord / intérieur

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

**Deux implémentations, toutes deux à coût nul :**

- **compteurs** : maintenir `coins_libres`/`bords_libres` et
  `cases_coin_vides`/`cases_bord_vides` ; toute inégalité ⇒ branche morte. Deux
  comparaisons par placement, et le cas « pièce de coin sur une case de bord » est couvert
  par la même mécanique ;
- **partition de l'arène** : ranger les pièces intérieures en tête de chaque compartiment
  et stocker un `n_interieur` par bucket. Une case intérieure n'itère que le préfixe :
  zéro test par candidat, et les balayages du forward-check raccourcissent aussi.

La seconde est préférable (elle *supprime* le travail au lieu de le rejeter) mais touche
[`part.c`](../../src/core/part.c) et l'invariant « `packed` est purement redondant sur
`flat` » — arbitrage à trancher en §6.

### 4.3 Comptage global couleur : demande de frontière vs stock disponible

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

**Coût.** Un placement touche au plus 8 compteurs (4 faces retirées du stock, ≤ 4
demandes créées ou consommées) ⇒ **O(1) incrémental**, annulation symétrique au
backtrack, ~8 comparaisons. Le gris (`0`) se traite à part et plus fortement : c'est une
**égalité**, `disponible[0]` doit valoir exactement le nombre de faces sortantes des cases
de bord encore vides.

**Gain attendu : inconnu.** Livrer avec un compteur dédié (`count_pruned`) pour l'arbitrer
sur mesure plutôt que sur intuition : si le test ne tire jamais, il se retire aussi
facilement qu'il s'ajoute.

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

**a) Itérer jusqu'au point fixe.** `possibility_all_has_a_next_counted` fait **une seule
passe** : une pièce forcée en fin de balayage peut rendre morte une case déjà validée en
début de passe, et personne ne revient dessus. Relancer tant qu'un placement a eu lieu est
strictement plus fort, pour quelques passes de plus — un coût négligeable dans un
processus dont c'est l'unique travail.

**b) DFS à budget de nœuds.** Rejouer `search_packet_backtracking` avec un plafond (ordre
de grandeur : 10 000 nœuds). Si le sous-arbre se **ferme** dans le budget, la possibilité
est définitivement morte : retirée du stock, jamais redistribuée. Si le budget est
atteint, elle est conservée `checked` comme aujourd'hui. Aucun faux positif possible, et
c'est bien calibré sur le stock réel : la délégation cède les frères les **plus profonds**
([`bt_materialize_pending`](../../src/core/etii_search.c)), dont les sous-arbres sont les
plus petits. Le pruner cesse d'être un filtre pour devenir un finisseur de petits
sous-arbres.

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

### 4.8 Ordre des candidats dans l'arène (n'élague rien, mais gratuit)

Trier chaque compartiment **à la construction** — pièce exposant les couleurs les plus
rares en premier, ou l'inverse — coûte **zéro à l'exécution** et change la vitesse à
laquelle les branches mortes s'épuisent. Sans effet sur le format de paquet : les indices
`next_s` sont purement locaux à un client et ne transitent jamais. Expérience à faible
risque, à mener au banc.

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
- **Ne pas retoucher `directions[]` dans ce cadre.** L'ordre actuel a été choisi pour
  éliminer tôt et son changement impose un bump de protocole ; les pistes ci-dessus
  s'appliquent toutes à ordre constant.

## 6. Points laissés ouverts

- **4.2 : compteurs ou partition de l'arène ?** La partition supprime le travail au lieu de
  le rejeter, mais ajoute un champ par compartiment et met en tension l'invariant
  « `packed` est purement redondant sur `flat` ». Trancher au vu du coût mesuré des
  compteurs.
- ~~**4.1 : garder ou non la fenêtre `c+1 … c+2` ?**~~ Tranché : non — voir §4.1, mesure à
  l'appui (−0,06 point de taux d'élagage sans la fenêtre résiduelle, effet négligeable).
- ~~**4.4 : le conflit de singletons est-il rentable ?**~~ Tranché : non, dans l'état actuel
  de l'arbre exploré — voir §4.4, implémenté/mesuré/reverté (−9 % de débit, 0
  déclenchement sur 500 M nœuds).
- **4.3 : le test tire-t-il jamais ?** Aucune estimation *a priori* fiable ; le compteur
  livré avec la PR tranchera.
- **4.6b : quel budget de nœuds ?** Dépend du profil de profondeur du stock serveur, que
  `GET /api/v1/stock-distribution` expose déjà — à relever sur un serveur réel avant de
  fixer une valeur, et à rendre réglable (console + fichier de configuration client) plutôt
  que codée en dur.
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
| 3 | **4.2** contrainte de type coin/bord/intérieur | faible | compteurs ou partition |
| 4 | **4.3** comptage global couleur + compteur `count_pruned` | faible | conserver ou retirer |
| 5 | **4.6a** point fixe dans le balayage du pruner | faible | — |
| 6 | **4.6b** DFS à budget dans le pruner (+ réglage du budget) | moyen | valeur du budget, exposition en configuration |
| 7 | **4.8** ordre des candidats dans l'arène (expérience) | faible | adopter ou classer |
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

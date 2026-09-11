# Recherche par anneaux intérieurs, bordure différée et guidée par budget de couleur

**Statut** : proposition — **écartée sans implémentation de production**, tranchée par
la mesure seule (`tests/tools/border_ring_conditioned.c`, comptage/génération pur,
aucun changement sous `src/`).

## Objectif

Variante de [recherche_interieur_budget_couleur.md](recherche_interieur_budget_couleur.md) :
plutôt que de greffer un contrôle de budget de couleur sur l'ordre de recherche MRV
existant (proposition d'origine, tranchée séparément — voir Antériorité), **changer
l'ordre de recherche lui-même**. Poser les anneaux intérieurs en partant du premier
anneau (adjacent à la bordure) vers le centre, différer la pose littérale des 60 pièces
de bordure, et à chaque anneau posé vérifier par comptage qu'une bordure — et un anneau
suivant — restent faisables. L'intention : éliminer la multiplicité d'arbre due aux
pièces de bordure (56 + 4) de la recherche principale, et remplacer leur pose explicite
par un contrôle de faisabilité pendant la pose de l'intérieur.

## Antériorité

- [recherche_interieur_budget_couleur.md](recherche_interieur_budget_couleur.md) :
  le même budget de couleur, greffé sur l'ordre MRV actuel plutôt que sur un ordre
  forcé — tranchée séparément (0 violation sur stock de production réel, la fenêtre de
  tir est quasi nulle sous MRV parce que `bt_frontier_init` compte le bord de grille
  comme contrainte et pose donc la bordure tôt par construction).
- Branche non mergée `border-mass-walker-design`
  (`tests/tools/border_ring_dp.{c,h}`, `docs/conception/border_mass.md`) : la masse
  **inconditionnelle** des anneaux de bordure valides dépasse 9,2×10¹⁸ — un comptage de
  circuits eulériens sur un multigraphe 24-régulier à 5 sommets (les 5 couleurs de
  cadre, 18–22), qui laisse **explicitement** la couleur intérieure de chaque pièce de
  bord en wildcard (jamais vérifiée). Ce chiffre ne dit donc rien de la question posée
  ici : combien de complétions restent-elles une fois l'intérieur connu ?
- `elagage_recherche.md` §4.9 : sous MRV, remplir une zone d'angle 3×3 est quasi gratuit
  (médiane 62 nœuds pour 40 cases, 300/300 quadruplets se complètent) — la bordure n'est
  pas le point coûteux de l'arbre actuel, ce que ce document confirme et affine (§ ci-
  dessous : les coins ne sont pas contraignants, la couleur intérieure l'est).

## Le mécanisme envisagé, et pourquoi la question n'était pas déjà répondue

Chaque pièce de bord non-coin (56) a une couleur intérieure ; l'offre par couleur est
connue à l'avance (1 à 6 pièces sur `data/pieces.csv`, très inégale). La proposition
d'origine ne vérifiait que la **demande agrégée** contre l'offre agrégée, jamais la
demande **positionnelle** (quelle couleur exacte à quelle case précise) ni sa
combinaison avec la continuité des couleurs de cadre entre pièces de bord voisines. Ces
deux éléments réunis (identité positionnelle + chaînage de cadre) sont strictement plus
forts que ce qui a déjà été mesuré et clos — d'où une mesure séparée avant tout code.

## Mesure décisive

Outil : `tests/tools/border_ring_conditioned.c` (`make border-ring-conditioned`),
comptage/génération pur, geométrie et DP réimplémentés en autonomie (aucune dépendance à
la branche `border-mass-walker-design`). Validé par trois contrôles indépendants avant
d'être fait confiance (`--selftest`, sur `eternityII.back`, 13 739 possibilités
réelles) :

- géométrie/forme/adjacence de bordure : 329 946 + 223 583 + 200 912 vérifications sur
  données réelles, toutes passées ;
- témoin négatif : le DFS de complétion, sans aucun épinglage de couleur, ne converge
  **pas** sous 300 000 nœuds (cohérent avec la masse inconditionnelle > 9,2×10¹⁸) ;
- témoin positif : sur une racine réelle à bordure 100 % posée (345/13 739 dans le
  stock), chaque pièce réellement utilisée est bien candidate à sa position, et la
  chaîne d'adjacence (fermeture du cycle comprise) est acceptée de bout en bout.

**Passe 1 — racines réelles du stock.** 0/13 739 possibilités ont un premier anneau
intérieur (52 cases) complet. Confirme, à l'échelle de la population entière et pas
d'un échantillon, ce que §2.3 de `recherche_interieur_budget_couleur.md` avait déjà
observé sur un seul plateau : MRV pose bordure et anneau au fil de l'eau, jamais anneau
fermé puis bordure.

**Passe 2 — anneaux synthétiques aveugles.** Faute de racine réelle exploitable, un
premier anneau intérieur valide et complet est généré (pièces réelles, backtracking,
seule la cohérence de l'anneau avec lui-même est exigée — aucun égard pour la rareté des
couleurs de bordure). Sur 150 échantillons (100 puis 50, deux exécutions), **150/150
infaisables** — DFS de complétion exhaustif à chaque fois (ni budget de nœuds ni
plafond de complétions atteints, donc une vraie preuve d'impossibilité, pas une
recherche interrompue trop tôt).

**Passe 3 — anneaux synthétiques conscients de la bordure.** Le vrai test du mécanisme
proposé : le budget de couleur restant guide la pose du premier anneau (rejet immédiat
de toute pose qui ferait dépasser l'offre restante d'une couleur — condition nécessaire
de Hall, appliquée **en construisant**, pas vérifiée après coup). Un garde-fou interne
vérifie à chaque échantillon que la demande extraite respecte bien le quota imposé
pendant la génération (jamais déclenché). Sur 80 échantillons (30 puis 50, deux
exécutions), **80/80 infaisables**, toujours sans épuisement de budget ni de plafond.

## Interprétation

Le budget de couleur (agrégé, ou même respecté position par position pendant la
construction) est une condition **nécessaire mais loin d'être suffisante**. Le vrai
verrou est la **continuité des couleurs de cadre** (les 5 couleurs 18–22, chaînées
pièce à pièce autour des 60 positions de bordure) combinée à l'identité positionnelle
de la couleur intérieure exigée à chacune des 56 positions non-coin. Fixer la couleur
intérieure exigée à chaque position pénalise très fortement l'espace de cadres valides,
au point de le vider systématiquement dans les échantillons testés — y compris quand la
construction de l'anneau est elle-même disciplinée par l'offre restante.

Cela répond directement à l'objection initialement soulevée sur les coins pendant la
discussion : les coins eux-mêmes ne sont pas spécialement contraignants (leurs couleurs
de cadre se recouvrent deux à deux, `elagage_recherche.md` §4.9 mesure un remplissage
quasi sans retour arrière) — le verrou est ailleurs, dans le chaînage de cadre sur
l'ensemble du pourtour, pas dans une zone particulière.

### Un budget plus fin : diversité de cadre par couleur intérieure

Entre le budget agrégé (« combien de pièces de cette couleur ») déjà mesuré ci-dessus et
la mesure décisive complète (DFS conditionné), il existe un budget intermédiaire, statique
et gratuit : pour chaque couleur intérieure, combien de paires de couleurs de cadre
*distinctes* ses pièces candidates offrent-elles ? Recensement sur `data/pieces.csv` (56
pièces de bord non-coin) :

| couleur | offre (pièces) | paires de cadre distinctes |
|---|---|---|
| **12** | **1** | **1** — pièce 38, cadre (20,22) uniquement |
| **17** | **1** | **1** — pièce 17, cadre (21,22) uniquement |
| 3, 10, 13 | 2 | 2 |
| 1, 2, 5, 11 | 3 | 3 |
| 15 | 3 | 2 (deux pièces partagent le même cadre) |
| 4, 7, 8, 16 | 4 | 4 |
| 6 | 5 | 4 (deux pièces partagent un cadre) |
| 9, 14 | 6 | 6 |

Pour une couleur rare (12, 17), il n'y a pas qu'une seule pièce disponible — il y a une
**seule transition de cadre possible**, point : dès qu'une position de l'anneau exige
cette couleur, la pièce ET son cadre sont entièrement figés, sans aucune marge pour
s'accorder avec les voisines de bordure. Ce n'est pas un budget à vérifier séparément du
DFS conditionné (couleur intérieure et paire de cadre sont deux attributs de la **même**
pièce, pas deux degrés de liberté indépendants) — c'est le mécanisme précis qui explique
*pourquoi* le DFS conditionné échoue systématiquement : ces couleurs rares imposent des
points de passage figés dans le circuit de cadre.

### Le blocage est global, pas local : aucune paire de couleurs de cadre n'est absente

Question naturelle à ce stade : est-ce qu'il existe des paires de couleurs de cadre
*totalement incompatibles* (aucune pièce ne les relie jamais), ce qui expliquerait le
verrou par un simple « trou » dans le graphe ? Vérifié sur les 60 pièces de bordure (56
+ 4 coins), les 15 paires possibles de couleurs de cadre (5 couleurs, répétitions
comprises) — notation `bord+coin` :

```
(18,18):2  (18,19):1+1  (18,20):6+1  (18,21):6  (18,22):5
(19,19):4  (19,20):4     (19,21):4+2  (19,22):4
(20,20):3  (20,21):3     (20,22):4
(21,21):2  (21,22):5
(22,22):3
```

**Aucune des 15 paires n'est absente.** Le graphe des couleurs de cadre est complet :
n'importe quelle couleur peut en théorie succéder à n'importe quelle autre. Cohérent avec
la masse inconditionnelle astronomique (9,2×10¹⁸) — une transition manquante aurait borné
le nombre de circuits bien plus bas.

Ça élimine l'explication la plus simple (« telle paire de couleurs est structurellement
interdite ») et précise la nature du verrou : il n'est **pas local** (chaque transition
individuelle reste toujours possible), il est **global**. Il faut faire tenir
*simultanément*, dans un seul circuit qui utilise chaque pièce une fois exactement, tous
les points d'ancrage figés qu'imposent les couleurs rares (le tableau ci-dessus). Chaque
transition prise séparément existe ; c'est leur assemblage conjoint, sans réutiliser une
pièce et sans rompre la boucle, qui échoue — structurellement le même genre de problème
qu'un circuit eulérien sous contraintes de sommets étiquetés : localement libre partout,
globalement sur-contraint.

### Passe 4 — contrôle inverse : l'asymétrie est directionnelle, pas un artefact du DFS

Question naturelle après un 0/230 aussi net (Passes 2 et 3) : est-ce que le DFS de
complétion de bordure (`brc_dfs`) est simplement incapable de trouver une complétion même
quand il en existe une ? Pour trancher, `border_ring_conditioned` fait maintenant le
chemin **inverse** des passes 1-3 : partir d'une bordure **réelle et complète** (60/60,
345/13 739 sur `eternityII.back`), et essayer de remplir le premier anneau intérieur à
partir de zéro — sans regarder l'intérieur réellement posé dans ce paquet (ignoré), sans
indice, seule la bordure fixe contraint (plus la cohérence de l'anneau avec lui-même).

**Résultat sur les 345/345 bordures réelles complètes du stock : 345/345 admettent un
anneau intérieur complet**, trouvé en 134 à 217 nœuds de DFS (quasiment aucun retour
arrière) — 0 prouvé infaisable, 0 inconclusif. L'exact miroir inversé des passes 2/3 :
anneau → bordure échoue 230/230, bordure → anneau réussit 345/345.

**Témoin** : le premier anneau ainsi obtenu est ensuite rebouclé dans la méthode des
passes 1-3 elle-même (extraction de la demande positionnelle + `brc_dfs`), pour vérifier
que cette méthode retrouve bien la bordure réelle qui a servi à le construire — sinon un
0/230 pourrait signaler un bug de méthode plutôt qu'un verrou réel du puzzle :

- la bordure réelle satisfait **exactement** la demande extraite de l'anneau généré, et
  toute sa chaîne d'adjacence (fermeture du cycle comprise) — vérifié position par
  position, indépendamment du DFS ;
- le DFS de bordure (même code que les passes 1-3) trouve **2 complétions** en 193 nœuds,
  sans épuisement de budget ni de plafond — donc un résultat exhaustif : il n'existe que 2
  bordures valides pour cet anneau précis, et la bordure réelle en fait partie.

La méthode n'est donc pas aveugle à une solution qui existerait : quand on lui donne un
cas construit pour avoir une solution, elle la trouve, et prouve qu'il n'y en a que 2 au
total — cohérent avec « verrou global, points d'ancrage figés » plutôt qu'avec un DFS
défaillant. Le 0/230 des passes 2/3 est un vrai zéro.

L'asymétrie elle-même est cohérente avec le reste de l'étude : les 196 pièces intérieures
et leurs 17 couleurs offrent beaucoup de marge locale pour accorder un anneau à une
bordure déjà fixée (5 couleurs de cadre seulement, offre 21-24 chacune) ; mais l'inverse —
fixer l'anneau puis chaîner 60 pièces de bordure autour de 56 couleurs intérieures
épinglées positionnellement (offre 1-6 par couleur) — est bien plus rigide. C'est
précisément le sens de construction que choisit MRV (bordure portée tôt par la contrainte
de grille, cf. Antériorité) et l'inverse de celui que proposait ce mécanisme.

Reproductible : `make border-ring-conditioned BORDER_RING_COND_ARGS="data/pieces.csv
eternityII.back"` (passe 4 incluse par défaut, ~0,15 s pour les 345 bordures + le témoin).

## Décision

**Ne pas implémenter** l'ordre « anneaux intérieurs d'abord, bordure différée », guidé
par budget de couleur ou par budget de couleur positionnel : la mesure décisive
(mesure A du plan d'étude) est défavorable avant même d'avoir construit le harnais de
simulation d'ordre forcé (mesure C, jamais entamée — sans objet si la prémisse de la
mesure A échoue). Un mécanisme de guidage qui viserait directement la continuité de
cadre (plutôt que le seul budget de couleur intérieure) reste concevable en théorie,
mais n'a aucun rapport avec la proposition telle que formulée ici et sortirait du cadre
de cette étude.

L'outil de mesure (`tests/tools/border_ring_conditioned.c`) est conservé : comptage pur,
coût nul hors invocation explicite, réutilisable si un stock au profil différent ou une
variante du mécanisme méritait un jour d'être re-testée.

## Points laissés ouverts

- Les échantillons testés (150 aveugles + 80 conscients de la bordure) restent des
  échantillons, pas une preuve exhaustive — l'infaisabilité à 100 % sur cette taille
  d'échantillon est un signal fort, pas une démonstration que TOUT anneau valide est
  infaisable pour la bordure.
- Un mécanisme de construction guidé directement par la continuité de cadre (plutôt que
  par le seul budget de couleur intérieure) n'a pas été testé — hors du périmètre de la
  proposition étudiée ici.

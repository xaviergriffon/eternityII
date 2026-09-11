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

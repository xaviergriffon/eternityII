# Recherche sans bordure explicite, guidée par un budget de couleur restant

**Statut** : proposition — idée explorée en discussion, non implémentée, aucune
mesure sur stock réel effectuée à ce stade.

## Objectif

Aujourd'hui, la recherche (MRV) pose la bordure comme n'importe quelle autre
case, dans le même arbre que l'intérieur. L'idée proposée ici est de ne
**pas** traiter la bordure explicitement dans la boucle de recherche
principale, et de la remplacer par une **contrainte de budget de couleur
restant** appliquée aux cases adjacentes à la bordure — réduisant l'espace de
recherche actif de 256 à 196 cases (ou 191 en tenant compte des 5 indices
officiels, cf. `data/indices.csv`).

## Le mécanisme envisagé

Chaque pièce de bord non-coin (56 sur le puzzle 256 pièces, cf.
`BORDER_RING_LEN`/`core/part.h`) a exactement une face « grise » (couleur 0,
tournée vers l'extérieur), deux faces qui la relient à ses voisines de
bordure, et une face tournée vers l'intérieur du plateau — celle qui touche
la case adjacente à la bordure (le premier anneau intérieur). Le nombre de
pièces de bord portant une couleur intérieure donnée est **fini et connu à
l'avance** (calculable statiquement depuis `data/pieces.csv`).

Ceci fournit une contrainte de type *global cardinality constraint*, que le
forward-check actuel ne voit pas (il raisonne « un candidat existe-t-il
localement ? », jamais « combien de pièces de cette couleur reste-t-il au
total ? ») :

- si une case du premier anneau intérieur exige, côté bordure, une couleur
  qui n'apparaît **dans aucune** pièce de bord, la branche est infaisable ;
- si toutes les pièces de bord d'une couleur donnée sont déjà placées
  ailleurs sur le plateau (couleur « épuisée »), toute case encore vide qui
  exigerait cette couleur devient infaisable.

L'idée est d'utiliser cette contrainte comme substitut à la pose explicite de
la bordure : le plateau intérieur (196/191 cases) serait résolu en premier,
et la bordure reconstruite/validée après coup contre cette contrainte plutôt
que posée case par case dans le même arbre de recherche.

## Ce qui reste à établir avant toute implémentation

- **La bordure n'est pas un sous-problème déjà résolu et précalculable.** Une
  étude séparée, en cours, cherche à mesurer exactement le nombre
  d'arrangements de bordure valides sur le vrai jeu de 256 pièces ; on sait
  déjà que ce nombre dépasse 9,2×10¹⁸ (au point de nécessiter un type de
  comptage sur 128 bits plutôt que 64), mais le chiffre exact n'est pas
  encore établi. Il ne faut donc pas construire cette proposition sur
  l'hypothèse qu'« il suffit d'injecter la solution de bordure connue » —
  elle sert de pruner généraliste, pas de raccourci vers un problème déjà
  clos.
- **Interaction avec les indices officiels** : les 5 indices connus
  (`data/indices.csv`) ne sont pas positionnés sur le premier anneau
  intérieur lui-même — ils sont un cran plus loin (le deuxième anneau, cf.
  leurs coordonnées `(2,2)`, `(13,2)`, `(2,13)`, `(13,13)` et le centre
  `(7,8)`). La contrainte de budget de couleur ne recouperait donc pas
  directement les indices ; reste à vérifier de combien elle resserre
  effectivement le premier anneau une fois les indices déjà posés.
- **Où appliquer le contrôle** : au forward-check à chaque placement voisin
  de la bordure, ou seulement au pruner (vérification bornée de fermeture) ?
  Un contrôle trop fréquent (à chaque case) coûterait un budget à maintenir
  et revalider en incrémental ; un contrôle trop rare perdrait l'essentiel du
  gain.
- **Mesure attendue avant tout code** : sur le stock de production réel,
  quelle fraction des racines profite effectivement de cette contrainte
  (couleurs déjà épuisées côté bordure alors que la case adjacente n'est pas
  encore posée) ? Sans cette mesure, impossible de juger si le gain justifie
  la complexité ajoutée — plusieurs pistes de pruning déjà tranchées dans
  [elagage_recherche.md](elagage_recherche.md) ont été écartées précisément
  faute de déclenchement réel malgré un raisonnement solide sur le papier
  (cf. §4.9, zones d'angle / pattern database).

## Arbitrages non tranchés

Rien n'est encore décidé — ce document sert à ne pas reperdre l'idée, pas à
figer une direction :

- reconstruire la bordure après coup contre la contrainte, vs. la garder
  dans l'arbre de recherche mais l'y traiter en dernier (après l'intérieur) ;
- comptabilité du budget de couleur incrémentale (mise à jour à chaque
  placement/retrait) vs. recalculée à la demande ;
- portée de la contrainte : uniquement le premier anneau intérieur, ou toute
  case du plateau qui pourrait — par transitivité via `forward_check_next_k`
  — être affectée par une couleur de bord épuisée.

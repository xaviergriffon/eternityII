# Expansion : la seconde rotation d'une pièce était perdue

**Statut** : résolue — cause prouvée, corrigée dans `search_possiblity_light`
(`src/core/possibility.c`), verrouillée par deux tests.

## Symptôme

Aucun. Rien ne le signalait : aucun message, aucun compteur. Le stock était
simplement plus petit que l'arbre qu'il prétend couper. Trouvé à la relecture
du code, en préparant un élagage des enfants morts dans l'expansion.

## Cause

`search_possiblity_light` développe un plateau d'une pièce : pour chaque entrée
(pièce, rotation) du compartiment de la case choisie par MRV, si la pièce est
libre, un enfant est créé. La boucle bâtissait chaque enfant **en copiant
l'enfant précédent** : pièce précédente retirée, nouvelle pièce posée. Le test
« pièce libre ? » lisait **cette copie**, où la pièce du dernier enfant créé
était encore marquée utilisée :

```c
if (!is_face_used(currPossibility->b_faceused, position)) {   // currPossibility = enfant précédent
    put_possibility(result, currPossibility);                  // copie de l'enfant précédent
    ...
    set_face_used(currPossibility->b_faceused, lastId - 1, 0); // retire la pièce précédente
```

Une pièce qui s'emboîte dans la case sous deux rotations apparaît deux fois dans
le compartiment, et ses rotations y sont contiguës. La seconde était donc
sautée. Ce n'est pas un doublon : c'est un autre plateau, valide, perdu avec tout
son sous-arbre.

Le cas mord surtout sur une case à un seul côté contraint : une pièce qui porte
deux fois la couleur demandée s'y pose de deux façons. Une case de bord n'est
jamais touchée, car la face grise fixe la rotation. Le défaut date du passage
au masque de pièces utilisées (`ea27a16`).

## Faits établis

**Arbre exact contre ce que l'ancien code atteignait, depuis la genèse.** Le
comptage est un parcours en profondeur sans stockage. À chaque nœud, il énumère
toutes les entrées libres du compartiment de la case MRV. Une arête est marquée
perdue quand l'ancien code la sautait : pièce égale à celle du dernier enfant
qu'il créait.

| Niveau (pièces posées) | Arbre exact | Atteint par l'ancien code | Production (`events.log`, passes du 05/10) | Perdu |
|---|---|---|---|---|
| 12 | 13 398 | 13 326 | — | 0,54 % |
| 14 | 144 356 | 142 415 | 142 415 | 1,35 % |
| 15 | 269 980 | 264 612 | 264 612 | 1,99 % |
| 16 | 219 869 | 215 037 | 215 037 | 2,20 % |
| 17 | 519 031 | 507 617 | 507 617 | 2,20 % |
| 18 | 1 097 434 | 1 075 265 | 1 075 265 | 2,02 % |
| 19 | 5 796 428 | 5 663 377 | 5 663 377 | 2,30 % |
| 20 | 19 298 357 | 18 816 755 | 18 816 755 | 2,50 % |
| 21 | 62 373 441 | 60 862 328 | 60 862 328 | 2,42 % |

L'émulation de l'ancien code reproduit **à l'unité** les comptes de production
à chaque niveau comparé. C'est ce qui établit à la fois l'instrument et la cause.

**Au niveau 25, sur le stock de production.** L'échantillon compte 200 000
possibilités, une sur dix des deux premiers millions du `.back` du 11/10.
- Enfants attendus : 877 534. Produits par l'ancien code : 873 772.
- 3 762 perdus, soit **0,43 % par passe** ; 1,9 % des parents sont touchés.
- Aucun doublon exact (même pièce, même rotation) dans les compartiments.
- 3 717 des pertes suivent immédiatement une rotation de la même pièce.
- Les 45 autres en sont séparées par une entrée sautée (pièce déjà posée) : l'ancien code jugeait sur le dernier enfant CRÉÉ, pas sur l'entrée précédente.
- Les 3 717 plateaux perdus contrôlés passent tous `check_possibility` : ce sont des plateaux valides.

## Conséquences

- **Un stock développé avant le correctif n'est pas une coupe complète de l'espace.**
  - Il manque 2,42 % de l'arbre au niveau 21.
  - L'estimation est d'environ 4 à 5 % au niveau 26 : 2,42 % mesurés au niveau 21, plus environ 0,4-0,5 % par niveau ensuite (mesuré à 0,43 % au niveau 25). Ce cumul n'est pas mesuré.
  - Les sous-arbres manquants n'ont jamais été distribués ; la solution peut s'y trouver.
- **Les extrapolations de `make bench-sample` sur un tel stock** (part réfutable, coût d'épuisement) portent sur une population amputée de cette fraction.
- **Les sous-arbres perdus ne se reconstituent pas d'eux-mêmes.** Leurs parents ont été développés et retirés du stock. Deux façons de les retrouver :
  - refaire l'expansion depuis la genèse avec un binaire corrigé ;
  - produire seulement le complément, c'est-à-dire les nœuds du niveau visé dont le chemin depuis la genèse emprunte au moins une arête perdue. Cela coûte un parcours de l'arbre jusqu'au niveau visé, mais sans le stocker. Ce n'est pas implémenté.

## Correctif

Chaque enfant est désormais une copie du **parent** plus sa seule pièce, et la
disponibilité de la pièce se lit sur le parent. Le coût est inchangé : une
copie de paquet par enfant, comme avant.

Deux tests le verrouillent (`tests/core/test_possibility.c`). Tous deux échouent
sur l'ancienne boucle (contre-épreuve faite) :
- **`search_light_keeps_every_rotation_of_a_free_piece`** : une pièce présente sous deux rotations donne deux enfants, et chacun porte le masque du parent plus sa seule pièce.
- **`search_light_children_match_bucket_enumeration_on_real_map`** : il compare, sur la vraie map 256, des descentes pseudo-aléatoires depuis la genèse (indices posés) à une énumération indépendante du compartiment. Il exige d'avoir rencontré le cas corrigé, sinon l'égalité ne prouverait rien.

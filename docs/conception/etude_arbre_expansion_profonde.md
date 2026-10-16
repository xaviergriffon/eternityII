# Expansion profonde et étude de l'arbre

**Statut** : en cours d'implémentation. PR 1/4 livrée : élagage exact des enfants morts. Le
correctif des [rotations perdues](../investigations/expansion_rotations_perdues.md) a été
trouvé en route et livré à part.

## Objet

Le stock serveur est développé niveau par niveau (`expand N`) jusqu'au niveau 26. L'objectif
n'est pas seulement de distribuer du travail. On veut aussi **étudier l'arbre** :
- sa taille par niveau ;
- les points communs et les tendances des racines ;
- d'éventuels chemins plus prometteurs ;
- une estimation de la durée d'exploration totale, à partir d'un niveau (~100) où chaque racine
  s'épuise en quelques jours.

Ce document consigne pourquoi la matérialisation d'un niveau complet atteint son mur, ce qu'on
y gagne malgré tout, et le protocole des études. Les seuils de décision sont écrits **avant**
les mesures.

## Ce que dit la production (`events.log`, octobre 2026)

| Niveau | Parents développés | Sans suite | Enfants par parent |
|---|---|---|---|
| 21 | 18,8 M | 11,1 % | 3,2 |
| 22 | 61 M | 15,1 % | 4,5 |
| 23 | 272 M | 8,8 % | 3,0 |
| 24 | 813 M | 12,8 % | 2,8 |
| 25 (après balayage pruner) | 1,37 G | 0 % | 5,7 |
| 26 (passe en cours) | 5,1 G | 0 % | 4,4 |

Le workflow d'un niveau est le suivant :
1. expansion ;
2. balayage complet par les pruners (~39 h pour 8,1 G de possibilités, −17 %) ;
3. sauvegarde puis restauration ;
4. `resetChecked` (~75 min) ;
5. niveau suivant.

**Le mur.**
- Le stock est multiplié par 3 à 4 à chaque niveau, et ce facteur s'applique aussi au disque, au
  temps d'expansion, au balayage et à la sauvegarde.
- La passe du niveau 26 a duré plus de 80 h.
- Le spill occupe 275 Go pour 23,3 G possibilités (11,8 octets par possibilité). Le niveau 27
  (~100 G possibilités) ferait environ 1,2 To.
- Le `.back` du niveau 25 pèse 232 Go (34,5 octets par possibilité). Une sauvegarde autonome du
  niveau 26 (~920 Go) ne tient sur aucun des disques du serveur.

**Le niveau 100 par matérialisation est hors de portée.** Il reste environ 74 niveaux à
franchir. Le modèle de branchement du §4.9 de [elagage_recherche.md](elagage_recherche.md)
(4R/17², de ~3,2 à ~2,2 candidats par case) donne de l'ordre de 10⁴⁰ nœuds. C'est un ordre de
grandeur, que l'étude S1 doit mesurer. Aucun facteur constant n'y change rien : une compression
×4 ou un élagage de −20 % achète au plus un niveau. Les questions de l'objet relèvent donc de
l'**échantillonnage**, pas de la matérialisation.

## PR 1 — élagage exact des enfants morts (livrée)

`search_possiblity_light_pruned` ne produit pas un enfant dont une case vide contrainte n'a plus
aucun candidat libre. Deux cases seulement peuvent le devenir :
- **(a)** une case dont la pièce posée était l'unique candidat chez le parent. Ces singletons
  sont recueillis au passage par le balayage MRV du parent.
- **(b)** une voisine de la case remplie, dont la clé change.

Toute autre case garde sa clé et perd au plus une pièce. Le verdict est celui d'un balayage
complet de l'enfant, pour au plus quatre comptes arrêtés au premier candidat. Un oracle sur la
vraie map le vérifie à chaque nœud de descentes pseudo-aléatoires. Sous MRV, (a) seule ne tue
presque jamais : il faut que deux cases partagent le même candidat unique. Un test dédié la
couvre.

**Mesuré sur 200 000 parents du niveau 25** (stock de production, déjà passé au pruner), en
A/B dans un même processus, ordre alterné, 3 répétitions :

| | µs par parent | Enfants produits |
|---|---|---|
| Sans élagage | 13,89 | 877 534 |
| Avec élagage | 14,23 (+2,5 %) | 781 170 (**−10,98 %**) |

Le temps d'une passe entière n'est pas mesuré. L'insertion pèse ~20 % d'une passe, donc 11 %
d'insertions en moins devraient compenser les +2,5 % de développement ; c'est une estimation.
Au niveau 26, le balayage pruner avait retiré 17 %. La part restante (singletons forcés, preuve
bornée, consommation par les clients) est ce que la PR 2 doit chiffrer.

## PR 2 — mesure préalable

S'appuie sur `make bench-sample` (tirage uniforme en réservoir, fermeture bornée, Wilson) et sur
`make bench-ram-tier`.

- **Lire un vrai stock non balayé.** Ajouter une source `--from-spill <segments…>` aux deux bancs,
  sur des **copies** de segments, jamais sur un serveur vivant. Le décodeur d'en-tête de trame de
  `core/stock_spill.c` est à exposer, pas à dupliquer.
- **Valeur du balayage pruner après la PR 1**, mesurée sur des enfants de niveau 26 :
  - part modifiée par le forçage des singletons ;
  - part fermée par la preuve bornée à 100, 1 000 et 10 000 nœuds.
  
  **Décision écrite d'avance :** si la preuve bornée ferme moins de 1 %, le balayage entre deux
  niveaux est retiré du workflow. Cela ne demande aucun code, seulement la doc d'exploitation.
- **Codecs candidats sur les blocs réels** (frères contigus) :
  - `zstd-1` (actuel), `zstd-3`, `zstd-9` ;
  - écart au précédent puis zstd (PR 3).
  
  Mesurer octets par possibilité, débits pack/unpack et vérifier le multi-ensemble.
  **Seuil pour la PR 3 :** au moins ×2 contre `zstd-1`, avec un unpack d'au moins 1 M
  possibilités/s par fil.

## PR 3 — codec de bloc différentiel (si la PR 2 franchit le seuil)

Deux frères ne diffèrent que d'une case. Un bloc est atomique, donc l'objection de
[format_stock_compact.md](../format_stock_compact.md) (couplage à l'enregistrement précédent
dans un fichier restaurable partiellement) ne s'y applique pas.

- Nouveau codec `STOCK_TIER_CODEC_DELTA_ZSTD` dans `stock_tier_pack`/`_unpack` :
  - premier enregistrement tel quel ;
  - puis, pour chaque suivant, l'écart avec le précédent (en-tête, bitmap, cases modifiées) ;
  - le tout passé dans zstd.
- `unpack` rend exactement les octets canoniques.
- Tout le reste passe par pack/unpack, donc le changement est transparent. Les trames de codec 0
  et 1 restent lisibles.

**Suite possible, hors de cette série :** un corps de `.back` fait de trames de l'étage
(sauvegarde par copie de trames). C'est ce qui rendrait sauvegardable un stock du niveau 26.

## PR 4 — étude de l'arbre par échantillonnage

**Outil.** Étendre `bench_sample` d'un estimateur de Knuth : sa doc constate qu'un coût
d'épuisement sur des racines peu profondes n'est qu'une borne basse sans lui.

- Une sonde descend de la racine par `search_possiblity_light_pruned`, c'est-à-dire exactement
  l'arbre de l'expansion, en tirant un enfant au hasard.
- Son poids est le produit des branchements rencontrés. On accumule ΣW et ΣW² par niveau, ce
  qui donne `N̂(L) ±` erreur-type, la survie par niveau et le niveau de mort.
- Le cœur pur est testé dans `make test`, avec un contrôle d'exactitude sur le 4×4, énumérable.

**Validation in situ, préalable à toute conclusion.** Depuis la genèse, `N̂` doit retrouver les
comptes exacts du parcours de l'[investigation](../investigations/expansion_rotations_perdues.md)
(62 373 441 au niveau 21), puis les ordres de grandeur de la production aux niveaux 25-26.

**Études**
- **S1 — Profil de l'arbre :** `N̂(L)` de 26 à 256, branchement, niveau du pic, taille totale.
- **S2 — Temps total :**
  - pour L = 100, 110, 120 et 130, tirer des racines par sonde et mesurer leur coût
    d'épuisement réel (preuve bornée, gros budget) ;
  - en déduire `T ≈ N̂(L) × coût(L) / débit du parc`.
  
  Une racine censurée par le budget fait de `T` une borne basse, comme dans `bench_sample`.
- **S3 — Promesse par racine :**
  - K sondes par racine sur un échantillon uniforme du niveau 26 ;
  - masse estimée aux niveaux 150 et 200 ;
  - mesurer la concentration : quelle part de la masse profonde porte le meilleur 1 % des racines.
- **S4 — Points communs :** corréler cette promesse avec des traits du plateau :
  - pièce et rotation par case ;
  - équilibre des couleurs de la frontière contre les pièces restantes ;
  - nombre de cases de frontière mono-contraintes ;
  - pièces de bord consommées.
- **S5 — Validation « côté trouver »** sur des clones à solution plantée
  ([banc_resolution_clones.md](banc_resolution_clones.md)) :
  - question : la racine qui contient la solution est-elle mieux classée par S3 que le hasard ?
  - **règle fixée d'avance :** rang médian sous 25 % sur 30 instances, test des signes avec
    p < 0,01 ;
  - appliquer les trois règles de méthode du §7 de
    [banc_resolution_clones.md](banc_resolution_clones.md) : mesurer le temps et pas seulement
    les nœuds, compter les paires indécises comme indécises, ne rien conclure sur une base
    censurée.
  
  Si S5 est positive, il en sort une piste concrète : servir les racines par promesse.

## Pistes closes, à ne pas rouvrir sans relire leur verdict

- Ordre des valeurs : §7 de [banc_resolution_clones.md](banc_resolution_clones.md).
- Backjumping dirigé par conflit et tables de zone : §4.11 et §4.9 de
  [elagage_recherche.md](elagage_recherche.md).
- Croix séparatrice : [croix_separatrice_ordre_variables.md](croix_separatrice_ordre_variables.md).
- Comptage couleur global dans la boucle chaude : §4.3 et §4.15 de
  [elagage_recherche.md](elagage_recherche.md).

## Points ouverts

- **Le stock de production antérieur au correctif des rotations reste amputé** : 2,42 % de
  l'arbre au niveau 21, environ 4 à 5 % estimés au niveau 26. Deux voies sont possibles :
  - refaire l'expansion avec un binaire corrigé ;
  - produire seulement le complément, c'est-à-dire les nœuds du niveau visé dont le chemin
    emprunte une arête perdue. Cela coûte un parcours de l'arbre jusqu'au niveau visé, sans le
    stocker. Ce n'est pas implémenté.

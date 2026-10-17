# Expansion profonde et étude de l'arbre

**Statut** : en cours d'implémentation. PR 1/4 livrée : élagage exact des enfants morts.
PR 2 faite (mesures) : le balayage pruner retire encore 11,2 % du stock, tout en quelques
nœuds, et le codec différentiel est écarté. Preuve courte dans l'expansion livrée : elle ferme
12,3 % des enfants, après quoi le balayage ne retire plus que 0,14 %. PR 4 : estimateur de Knuth
livré et validé. S1-S3 sont mesurées : ~10⁴⁰ nœuds au niveau 100, pic vers 140, et 1 % des racines
du niveau 26 porte un tiers de l'arbre au niveau 60, de façon reproductible. S5 est positive :
sur 120 clones neufs, la racine de la solution est classée en médiane dans les 4-6,5 % de tête
par sa masse à mi-profondeur, 30 fois sur 30 dans chacune des quatre cellules. L'exploration
par masse, elle, ne gagne pas de temps : elle perd contre la production dans les quatre
cellules, et même gratuite elle ne bat pas un ordre aléatoire. Reste S4. Le
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

## PR 2 — mesure préalable (faite)

Deux bancs étendus, aucun changement de comportement en production :
- **`make bench-sample`** classe chaque racine tirée en trois parts disjointes
  (`sample_sweep_tally`, testé) :
  - morte à l'insertion, que l'expansion écarte depuis la PR 1 ;
  - tuée par le seul contrôle bon marché du pruner ;
  - fermée par la preuve bornée sous 100, 1 000 et 10 000 nœuds.
- **`make bench-ram-tier --from-spill`** lit les blocs **réels** d'un débordement par le
  décodeur du débordement lui-même (`stock_spill_read_segment`, lecture seule, testé). Il
  compare zstd à plusieurs niveaux, l'écart au précédent, et le tri du bloc.

### Valeur résiduelle du balayage pruner

**Échantillon :** 20 000 racines tirées uniformément (réservoir, graine 26) des
26 649 021 669 du stock du niveau 26 (`.back` du 16/10, 100 % non vérifiées, 99,46 % à 26
pièces). Plafond de 10⁶ nœuds par racine, 16 processus.

| Part du stock (IC de Wilson 95 %) | |
|---|---|
| Morte à l'insertion (l'expansion l'écarte désormais) | **9,47 %** [9,07 ; 9,88] |
| Tuée par le seul contrôle bon marché du pruner | 3,01 % [2,78 ; 3,26] |
| … plus celles fermées par la preuve bornée en ≤ 100 nœuds | **11,22 %** [10,79 ; 11,67] |
| … à 1 000, 10 000 et 10⁶ nœuds | 11,22 % (une seule fermeture de plus, à 848 721 nœuds) |
| Encore ouvertes à 10⁶ nœuds | 79,3 % |

**Toute la mort de ce stock est PEU PROFONDE.**
- Les 1 644 racines fermées par la preuve bornée, hors contrôle bon marché, le sont en 2 nœuds
  en médiane, et toutes en 16 nœuds au plus sauf une.
- Au-delà, rien ne se ferme jusqu'à 10⁶ nœuds.

Cela corrige l'*a priori* de ce document, qui disait qu'aucune fermeture n'était mesurée sous
100 pièces. Ici, 8,2 % du stock du niveau 26 meurt en quelques nœuds sans être mort au contrôle
immédiat.

**Décision, selon la règle écrite d'avance.** La preuve bornée ferme plus de 1 % : le balayage
entre deux niveaux **garde une valeur**, 11,2 % du stock même après la PR 1. En revanche, sa
forme est disproportionnée. Il fait passer tout le stock sur TCP par les pruners (39 h pour
8,1 G de possibilités au niveau 25), alors que ce qu'il retire coûte quelques nœuds par racine.

Mesuré sur ces racines :
- une racine écartée à l'insertion coûte ~8 µs ;
- une fermeture coûte ~11 µs en médiane ;
- un nœud coûte ~0,93 µs.

Une preuve plafonnée à 16 nœuds coûterait au plus ~23 µs par enfant. Sur les 25,75 G
enfants du niveau 26, cela fait ~7 cœurs-jours, contre ~17 cœurs-jours pour la passe
elle-même et bien davantage pour le balayage. C'est une estimation à vérifier en A/B. D'où la
PR suivante : une preuve bornée courte dans l'expansion, sur chaque enfant retenu par la PR 1
(voir [ci-dessous](#preuve-courte-dans-lexpansion-livrée-à-la-suite-de-la-pr-2) ; le plafond
mesuré est de 8 nœuds, pas 16).

### Codecs sur les blocs réels

Les mesures portent sur 2 M possibilités, un segment sur 9 000 du débordement du niveau 26,
en blocs de 64 Kio. Détail dans [tests_et_ci.md](../tests_et_ci.md#banc-de-létage-ram-compressé-make-bench-ram-tier).

| Codec | Octets stockés par possibilité |
|---|---|
| Trames de production (zstd -1) | 12,51 |
| zstd -3 / -9 | 12,3 / 11,9 (éviction ×6 plus lente à -9) |
| Bloc trié + zstd -1 | 11,4 (éviction ×7 plus lente) |
| Écart au précédent + zstd -1 / -9 | 15,6 / 13,8 — **pire** |
| Trié + écart + zstd -1 | 13,9 — **pire** |

L'écart au précédent perd parce que deux enregistrements consécutifs diffèrent de **8,5 cases**
en moyenne, pas d'une seule. Un bloc enchaîne les fratries de parents sans lien entre eux
(~34 cases d'écart entre deux parents), et zstd trouve ses références plus loin dans sa
fenêtre.

**Décision, selon la règle écrite d'avance :** aucun candidat n'approche ×2 (il faudrait
6,25 octets au plus). **La PR 3 n'est pas lancée.**

À noter au passage : le `.back` du niveau 26 pèse 318,5 Go, soit 12 octets par possibilité.
Le débordement y est recopié dans l'ordre des blocs, ce qui le rend aussi compact que le disque.
Le `.back` du niveau 25 (232 Go, 34,5 octets) avait été écrit avant que l'expansion range ses
enfants en blocs.

## Preuve courte dans l'expansion (livrée, à la suite de la PR 2)

Chaque enfant gardé par l'élagage exact passe la preuve bornée du pruner, délégation coupée,
plafonnée à `EXPAND_PROOF_BUDGET` nœuds (`etii_expand_short_proof`, `core/etii_search.c`). Un
enfant prouvé mort n'est pas inséré. La preuve est **injectée** dans `core/datamanager.c`
(`datamanager_set_expand_proof_hook`, posée par `runserver`) : un binaire qui lie le stock sans
le moteur reste valide, et un test peut injecter une preuve factice.

**Sûreté**
- `BT_CORE_EXHAUSTED` est une preuve exacte. Un plafond atteint ou un arrêt demandé garde
  l'enfant.
- Près de la fin du plateau (au plus `EXPAND_PROOF_BUDGET` cases vides), la preuve n'est pas
  tentée. Le moteur enregistre une solution puis poursuit, et pourrait finir « épuisé » :
  l'enfant qui la porte serait écarté.
- Un test confirme chaque fermeture sur la vraie map par un développement exhaustif
  indépendant. Contre-épreuve faite : traiter un plafond atteint comme une mort le fait échouer.

**Plafond, mesuré** sur les 48 915 enfants que l'élagage exact garde parmi ceux de 20 000
parents tirés uniformément du niveau 26 (passage 26 → 27) :

| Plafond | Enfants fermés | Coût par enfant | Développement par parent |
|---|---|---|---|
| — | — | — | 13,3 µs |
| 4 | 11,05 % | 7,1 µs | 30,7 µs |
| **8** | **12,27 %** | **8,9 µs** | **35,1 µs** |
| 16 | 12,34 % | 12,5 µs | 43,8 µs |
| 32 | 12,36 % | 19,7 µs | 61,3 µs |

8 est le coude : au-delà, on paie sans rien fermer.

Sur les ~26 G enfants d'une passe 26 → 27, la preuve coûte ~2,7 cœurs-jours, contre ~17
cœurs-jours pour la passe du niveau 26. C'est une estimation, pas une passe mesurée. La preuve
n'alloue rien : elle profite de `--expand-threads` sans buter sur l'arène malloc unique.

**Valeur résiduelle du balayage, mesurée après elle** sur les mêmes enfants :
- le contrôle bon marché ne retire plus rien (0 %) ;
- la preuve à 10 000 nœuds retire **0,14 %**.

Sous le seuil de 1 % écrit d'avance : **le balayage pruner entre deux niveaux, avec la
sauvegarde/restauration et le `resetChecked` qui l'accompagnent, n'a plus d'objet** pour une
expansion faite avec cette version.

## PR 3 — codec de bloc différentiel (écartée par la mesure)

L'idée était la suivante. Deux frères ne diffèrent que d'une case, et un bloc est atomique :
l'objection de [format_stock_compact.md](../format_stock_compact.md) (couplage à
l'enregistrement précédent dans un fichier restaurable partiellement) ne s'y appliquait pas.

La mesure de la PR 2 l'écarte : l'écart au précédent fait **moins bien** que zstd seul. Le
candidat reste dans `bench_ram_tier` (`dzstd-N`, `sdzstd-N`) pour qu'on ne le reprogramme pas.
**Seule piste non mesurée :** une forme ARBORESCENTE, c'est-à-dire un parent stocké une fois
plus les pièces de ses enfants. Ce n'est plus un codec de bloc : il faudrait que l'étage range
des fratries entières au lieu d'enregistrements indépendants.

## PR 4 — étude de l'arbre par échantillonnage (outil livré, S1-S3 mesurées)

**Outil.**
- `tests/bench/tree_probe.{h,c}`, testé par `tree_probe_suite`. Une sonde descend de la
  racine en tirant un enfant uniformément parmi ceux que produit l'expansion
  (`search_possiblity_light_pruned`, l'arbre exact sans nœud mort).
- Son poids est le produit des branchements rencontrés (Knuth, 1975). Son espérance au
  niveau L est exactement le nombre de nœuds du niveau L.
- La dernière case est comptée, pas développée : chaque complétion est une solution, et
  `search_possiblity_light` termine le processus sur un plateau complet.
- `make bench-sample` gagne trois options :
  - `--probe` (depuis `--genesis` ou depuis un échantillon) ;
  - `--cost-level L` : preuve bornée du nœud atteint au niveau L, dont l'espérance poids × coût
    est le coût d'épuisement de tout le niveau ;
  - `--mass-levels` : masses estimées par racine, écrites dans le CSV.

**Exactitude, dans `make test`.**
- Le profil estimé retrouve, à 5 erreurs types près, l'énumération COMPLÈTE de deux arbres :
  le 4×4 entier, solutions comprises (build 16), et le haut de l'arbre depuis la genèse
  jusqu'au niveau 13 (build 256).
- Contre-épreuves faites : un tirage non uniforme (toujours le premier enfant) et un poids
  faux (b + 1) le font échouer dans les deux builds.

**Validation in situ.** Un million de sondes depuis la genèse, 11 s. Elles retrouvent les
comptes exacts du parcours de l'[investigation](../investigations/expansion_rotations_perdues.md),
moins les nœuds morts, à 1-1,2 erreur type à chaque niveau :

| Niveau | Exact | Estimé |
|---|---|---|
| 16 | 165 687 | 165 200 ± 713 |
| 18 | 1 023 395 | 1 021 000 ± 2 730 |
| 20 | 17 143 243 | 17 070 000 ± 62 400 |

Au niveau 26, l'estimation donne 2,56·10¹⁰ ± 0,6 %. Le stock de production compte 26,65 G
possibilités, dont 9,5 % de nœuds morts et auquel manquent environ 4-5 % de rotations perdues,
soit environ 25 G comparables : c'est cohérent.

### S1 — profil de l'arbre (mesuré)

Deux millions de sondes depuis la genèse jusqu'au bout, 33 s sur 32 processus :

| Niveau | Nœuds estimés | Erreur type | Sondes qui l'atteignent |
|---|---|---|---|
| 26 | 2,56·10¹⁰ | 0,6 % | 41,8 % |
| 30 | 2,14·10¹² | 0,9 % | 34,7 % |
| 40 | 2,7·10¹⁷ | 44 % | 23,5 % |
| 60 | 1,0·10²⁶ | 27 % | 10,6 % |
| 80 | 3,1·10³³ | 21 % | 3,7 % |
| 100 | 1,6·10⁴⁰ | 65 % | 0,84 % |
| 120 | 4,0·10⁴⁴ | 43 % | 0,09 % |
| 135-145 | ~10⁴⁶-10⁴⁷ | > 75 % | < 0,01 % |
| Total | 4,4·10⁴⁸ | 85 % | — |

**Lecture.**
- Le profil est fiable jusque vers le niveau 35, et l'ordre de grandeur jusque vers 120.
- Le niveau 100 confirme le modèle du §4.9 (~10⁴⁰).
- Le pic se situe vers 135-145 pièces. Au-delà, presque aucune sonde n'arrive : la variance
  de Knuth est lourde, et resserrer ces niveaux demanderait un échantillonnage stratifié par
  niveau (non fait).

### S2 — coût d'épuisement d'un niveau (mesuré)

Un million de sondes arrêtées au niveau L, puis la preuve bornée du pruner (2·10⁶ nœuds, ~0,76
M nœuds/s par processus) sur chaque nœud atteint :

| Niveau L | Preuves lancées | Au plafond | Coût d'épuisement de tout le niveau |
|---|---|---|---|
| 100 | 8 597 | 1 480 | **au moins** 5·10⁴⁴ nœuds |
| 110 | 3 041 | 62 | au moins 4,8·10⁴⁶ |
| 120 | 937 | 1 | au moins 3,4·10⁴⁹ (erreur type ~100 %) |
| 130 | 216 | 0 | **3,6·10⁴⁷ ± 1,8·10⁴⁷**, une estimation |

Au niveau 130, cela fait ≈ 1,5·10³⁴ cœurs-années au débit mesuré. Les bornes basses aux niveaux
100 à 120 viennent des racines au plafond, et le niveau 120 ne repose que sur 937 sondes.
**L'idée « atteindre un niveau où une racine s'épuise en jours » est donc bien réalisée vers 100-130 :**
- les racines s'y ferment presque toutes sous 2·10⁶ nœuds ;
- mais elles sont 10⁴⁰ à 10⁴⁶.

Le temps total est hors d'échelle de plus de 30 ordres de grandeur. Un recensement exhaustif
est exclu ; ce qui compte est l'ORDRE dans lequel l'arbre est exploré (S3-S5).

### S3 — promesse par racine (mesurée, première lecture)

Échantillon : les 20 000 racines tirées uniformément du niveau 26. On fait 50 sondes par racine,
**deux fois avec des graines indépendantes**, et on compare les deux estimations racine par
racine. Sans cette contre-épreuve, le bruit de Knuth fabrique à lui seul une concentration
spectaculaire (le meilleur 1 % porte 96,5 % de la masse du niveau 100 dans un seul tirage).

| Niveau | Rang de Spearman entre les deux tirages (racines vivantes) | Part de la masse du tirage B portée par le meilleur 1 % du tirage A |
|---|---|---|
| 30 | 0,995 | 12,8 % |
| 40 | 0,928 | 18,8 % |
| 60 | 0,592 | **32,3 %** |
| 100 | −0,15 | non concluant (bruit) |

**Lecture.**
- Les racines diffèrent réellement, et de façon reproductible : jusqu'au niveau 60, **1 % des
  racines porte environ un tiers de l'arbre** (32 fois leur part uniforme).
- À 50 sondes, la masse au-delà de 60 n'est que du bruit ; la lire demande plus de sondes par
  racine.
- La masse n'est pas encore une probabilité de SOLUTION. C'est la question de S5, et rien de
  S3 ne doit être appliqué sans elle.

### Restent à faire

- **Exploration par masse, en temps :** mesurée ci-dessous, négative.
- **S4 — Points communs :** corréler la masse au niveau 60 (la mesure reproductible) avec des
  traits du plateau :
  - pièce et rotation par case ;
  - équilibre des couleurs de la frontière contre les pièces restantes ;
  - nombre de cases de frontière mono-contraintes ;
  - pièces de bord consommées.
### S5 — la masse prédit-elle la solution ? Protocole écrit AVANT la mesure

**Question.** Sur des clones à solution plantée
([banc_resolution_clones.md](banc_resolution_clones.md)), la racine qui contient la solution
est-elle mieux classée par sa masse estimée que par le hasard ?

**Protocole, fixé avant toute exécution.**
- **Instances :** les quatre cellules calibrées du §6.4 de
  [banc_resolution_clones.md](banc_resolution_clones.md) (`n10k14`, `n10k13`, `n12k22`,
  `n12k20`), graines 1 à 30, cinq indices, générées par `tools/gen_clone.py`.
- **Coupe :** depuis la genèse, l'arbre de l'expansion (élagage exact) est énuméré ENTIER
  jusqu'au premier niveau L₀ qui compte au moins 1 000 nœuds. Ces nœuds sont les racines.
- **Racine de la solution :** la seule racine dont toutes les cases posées coïncident avec la
  solution plantée. Si aucune ne coïncide, l'instance est en erreur et le banc le signale.
- **Score d'une racine :** la masse estimée au niveau L₀ + ⌈0,133 · N⌉, soit 14 niveaux sous la
  racine en 10×10 et 20 en 12×12. C'est la profondeur relative de 26 → 60 au 16×16, celle où
  S3 a trouvé un classement reproductible. Le score est la moyenne de deux tirages à graines
  indépendantes, de 50 sondes chacun. Les autres profondeurs et la masse au dernier niveau sont
  rapportées sans décider.
- **Mesure par instance :** le rang centile de la racine de la solution, c'est-à-dire la part
  des racines de score strictement supérieur plus la moitié des ex aequo, sur [0, 1[. 0 veut
  dire en tête. On rapporte aussi le Spearman entre les deux tirages : une masse que deux
  tirages ne reproduisent pas ne peut rien classer.
- **Règle de décision, PAR CELLULE :** **positive** si le rang centile médian sur les 30
  instances est sous 25 % ET si le test des signes bilatéral (instances sous 50 % contre au-dessus)
  donne p < 0,01. Une cellule négative ou indécise est rapportée comme telle.
- **Interprétation :**
  - positive dans les quatre cellules : la piste « servir les racines par masse » est ouverte,
    et devra encore gagner en TEMPS à la recherche réelle (§7 de banc_resolution_clones.md) ;
  - positive dans certaines cellules seulement : rien n'est adopté, la dépendance au régime
    est le résultat ;
  - négative partout : la masse décrit l'arbre mais ne localise pas la solution, et S3 n'a pas
    d'usage pour l'ordre d'exploration.

### S5 — campagne de confirmation : protocole écrit AVANT la mesure

La première campagne (graines 1-30, résultats ci-après, consignés après la mesure) a révélé un
mode d'échec. À la profondeur primaire, 50 sondes ne descendent pas assez loin sous beaucoup de
racines : la racine de la solution finit ex aequo à masse nulle, avec rang ≈ 0,5-0,6. Le score
à mi-profondeur, secondaire et donc non décisionnel, a semblé n'avoir pas ce défaut. C'est une
observation **après coup**, qui ne peut pas conclure sur les instances qui l'ont suggérée.

**Confirmation, fixée avant exécution :**
- mêmes quatre cellules, graines **31 à 60**, jamais vues ;
- score **primaire** : la masse à **mi-profondeur**, niveau L₀ + ⌈⌈0,133 · N⌉ / 2⌉, soit 7
  niveaux en 10×10 et 10 en 12×12 ;
- même règle par cellule : médiane < 25 % ET test des signes p < 0,01 ;
- les autres scores sont rapportés sans décider.

### S5 — résultats (consignés après la mesure, protocoles inchangés)

L'outil est `make bench-s5`, qui lance `tests/bench/s5_campaign.sh` et le mode
`bench_sample --solution`. Une instance prend moins d'une seconde.
- La coupe exacte compte de 1 000 à 5 000 racines, au niveau 11-12, ou 19 quand la genèse est
  étroite.
- Il y a 50 sondes par racine et par tirage.
- La racine de la solution est identifiée de façon unique sur les 240 instances. Un test le
  garantit : les sous-arbres d'une coupe sont disjoints et couvrent l'arbre.

**Campagne 1 (graines 1-30), score primaire à la profondeur L₀ + ⌈0,133·N⌉ :**

| Cellule | Rang médian | Sous 50 % | p | Spearman médian | Verdict |
|---|---|---|---|---|---|
| n10k13 | 5,6 % | 29/30 | 6·10⁻⁸ | 0,93 | positive |
| n10k14 | 4,4 % | 30/30 | 2·10⁻⁹ | 0,91 | positive |
| n12k20 | 4,5 % | 24/30 | 0,0014 | 0,54 | positive |
| n12k22 | 2,2 % | 20/30 | 0,099 | 0,51 | **non positive** |

**Le mode d'échec est identifié, et ce n'est pas un mauvais classement.**
- Les 16 instances au-dessus de 50 % ont toutes un rang entre 0,51 et 0,61.
- À cette profondeur, 50 sondes ne descendent pas assez loin sous la plupart des racines : la
  racine de la solution finit ex aequo, à masse NULLE, avec la majorité des autres.
- Les Spearman médians de 0,5 en 12×12 le disaient déjà.
- Le score à mi-profondeur, secondaire, n'avait pas ce défaut : rang médian 3-10 %, 29 ou 30
  instances sur 30 sous 50 % dans chaque cellule.

C'était une observation après coup, d'où la confirmation.

**Campagne de confirmation (graines 31-60, jamais vues), score primaire à mi-profondeur
L₀ + ⌈⌈0,133·N⌉/2⌉, protocole consigné AVANT exécution :**

| Cellule | Rang médian | Sous 50 % | p | Spearman médian | Verdict |
|---|---|---|---|---|---|
| n10k13 | 6,2 % | **30/30** | 2·10⁻⁹ | 0,994 | **positive** |
| n10k14 | 6,5 % | **30/30** | 2·10⁻⁹ | 0,994 | **positive** |
| n12k20 | 4,4 % | **30/30** | 2·10⁻⁹ | 0,952 | **positive** |
| n12k22 | 3,9 % | **30/30** | 2·10⁻⁹ | 0,937 | **positive** |

**Verdict S5 : positive dans les quatre cellules.**
- Sur 120 instances neuves, la racine qui porte la solution est TOUJOURS dans la moitié
  supérieure du classement par masse estimée à mi-profondeur.
- Elle est en médiane dans les 4 à 6,5 % de tête, parmi 1 000 à 5 000 racines.
- Le score est reproductible d'un tirage à l'autre (Spearman 0,94-0,99).
- C'est cohérent avec le modèle : un plateau partiel compatible avec une solution complète
  évite par construction les contradictions précoces, et porte donc plus de descendants.

**Ce que ce résultat ne dit pas encore.**
1. **La masse classe, mais elle ne fait pas encore gagner de temps.** Selon le §7 de
   [banc_resolution_clones.md](banc_resolution_clones.md), une piste ne s'adopte que si elle
   gagne en TEMPS à la recherche réelle. Il faut payer les sondes, puis explorer les racines
   par masse décroissante, et comparer le temps jusqu'à la solution à l'ordre de production,
   appariés sur les mêmes instances. C'est la mesure suivante.
2. **Le transfert au 16×16 se fait avec la réserve du §6.4** de banc_resolution_clones.md :
   aucune cellule n'a les compartiments du vrai puzzle.
3. **L'échelle d'un niveau 26 de production n'a pas été testée.** Il compte des milliards de
   racines contre quelques milliers ici, et une coupe à quelques niveaux sous la genèse. S3 y
   a toutefois montré la même propriété préalable : une masse reproductible entre tirages.

### Exploration par masse : gagne-t-elle du TEMPS ? Protocole écrit AVANT la mesure

S5 établit que la masse **classe** la racine de la solution très haut. Le §7 de
[banc_resolution_clones.md](banc_resolution_clones.md) exige davantage pour adopter quoi que ce
soit : gagner en TEMPS jusqu'à la solution, sondes comprises, contre la production, sur les
mêmes instances.

**Bras**, joués à la suite sur chaque instance, dans le même processus parent (alternance
structurelle du §7). Chaque exécution est un fils où `stop_on_solution` sort réellement, comme
dans `bench_solve`. Tous les bras tournent sur un seul cœur.
- **A — production :** la recherche depuis la genèse (`search_packet_backtracking_mrv`, ordre
  d'arène).
- **B — coupe, ordre naturel :** les racines de la coupe exacte de S5 (premier niveau à au moins
  1 000 nœuds), explorées entièrement l'une après l'autre dans l'ordre d'énumération.
- **C — coupe, ordre aléatoire :** la même coupe, permutée (graine de l'instance). C'est le
  contrôle qui sépare l'effet de l'ORDRE de l'effet de la coupe elle-même.
- **D — coupe, ordre par masse :** la même coupe, par masse décroissante à mi-profondeur
  (L₀ + ⌈⌈0,133·N⌉/2⌉, le score confirmé par S5), estimée par **20 sondes par racine**.
  **Le temps des sondes compte dans le temps de D.** C'est moins que les 2 × 50 de S5, parce
  que les sondes se paient ici ; le Spearman de 0,94-0,99 entre tirages de S5 laisse de la marge.

**Instances :** les quatre cellules, graines **61 à 90**, jamais vues. Plafond de **2·10⁷
nœuds** de moteur par exécution, comme au §7.

**Lecture d'une paire (X, Y), en temps :**
- les deux résolues : le plus rapide gagne ;
- une seule résolue : elle gagne si son temps est inférieur au temps de l'autre à son plafond,
  sinon la paire est indécise ;
- aucune résolue : la paire est indécise.

Les paires indécises sont comptées et rapportées, jamais attribuées.

**Règle de décision, PAR CELLULE :** **D adopte-able** si D bat A au test des signes bilatéral
sur les paires décidées avec p < 0,01, ET si D bat C de même. L'adoption suppose les quatre
cellules ; sinon le résultat est rapporté tel quel. B contre A et C contre A sont rapportés pour
lire l'effet de la coupe seule. Les nœuds sont rapportés, mais ne décident pas.

### Exploration par masse — résultat (consigné après la mesure, protocole inchangé) : NÉGATIF

L'outil est `make bench-mass-order` : `tests/bench/bench_mass_order.c`, plus
`tests/bench/mass_order_campaign.sh`. Les mesures portent sur 120 instances neuves (graines
61-90), 4 bras par instance. Toutes les exécutions ont abouti : aucune paire indécise.

| Cellule | Temps médian A / D | D contre A | D contre C | B contre A | C contre A | Verdict |
|---|---|---|---|---|---|---|
| n10k13 | 0,19 s / 1,56 s | 0-30, p = 2·10⁻⁹ | 14-16, p = 0,86 | 2-28 | 4-26 | **D perd contre A** |
| n10k14 | 0,04 s / 0,96 s | 0-30, p = 2·10⁻⁹ | 1-29 | 5-25 | 2-28 | **D perd contre A** |
| n12k20 | 0,51 s / 4,04 s | 1-29, p = 6·10⁻⁸ | 10-20, p = 0,099 | 5-25 | 3-27 | **D perd contre A** |
| n12k22 | 0,04 s / 0,71 s | 0-30, p = 2·10⁻⁹ | 0-30 | 5-25 | 8-22 | **D perd contre A** |

(gagne-perd, test des signes bilatéral ; la production A gagne presque toujours)

**Verdict : l'exploration par masse n'est pas adoptable, et perd dans les quatre cellules.**
La racine de la solution est pourtant classée en médiane dans les 2 à 16 % de tête par ces
20 sondes. Trois raisons, mesurées :
1. **Les sondes coûtent plus que la recherche entière.** Elles prennent 0,55 à 0,66 s en
   médiane, alors que la production trouve en 0,04 à 0,5 s. Sur ces instances, aucun
   classement payé ce prix ne peut gagner.
2. **Même gratuites, elles ne font pas mieux que le hasard.** Sans le temps des sondes, D
   contre C donne 17-13, 14-16, 15-15 et 21-9 (p ≥ 0,043), à peu près pareil en nœuds.
   - La masse désigne la racine de la solution, mais une racine massive est avant tout un
     GRAND sous-arbre : les racines placées avant celle de la solution coûtent cher.
   - Si les solutions croissent comme la taille, trier par taille ne change pas la densité
     rencontrée : c'est la **densité de solutions par nœud** qu'il faudrait viser, pas la masse.
3. **La coupe elle-même perd lourdement contre la production.** B et C demandent 5 à 10 fois
   plus de nœuds que A (médianes A 3,0·10⁵ contre C 2,7·10⁶ en n10k13, 5,7·10⁴ contre 5·10⁵ en
   n10k14). Explorer racine après racine, chacune repartant d'un état du moteur neuf, abandonne
   ce que l'ordre de production fait de mieux depuis la genèse.

**Ce que cela clôt.**
- Servir les racines d'un stock par masse décroissante n'est pas une piste d'accélération,
  sous cette forme.
- S5 reste un fait sur la STRUCTURE de l'arbre : la solution vit dans les régions massives.
- Ce n'est pas un levier d'ordre, tant qu'un score ne distingue pas la densité de solutions de
  la taille.
- **Une piste distincte, non mesurée :** un score de densité, par exemple la masse au dernier
  niveau rapportée à la masse totale, que les sondes actuelles ne voient pas en 10×10.

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

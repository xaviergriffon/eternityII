# Forme compacte d'une possibilité (`core/packet_codec.{h,c}`)

Référence technique de la forme compacte d'un `possibility_packet` : pourquoi
elle existe, ce qu'elle mesure, les formes concurrentes écartées et les pistes à
ne pas rejouer sans lire la raison.

- Le **format** lui-même (disposition des octets) et ses **invariants** sont
  dans [src/core/packet_codec.h](../src/core/packet_codec.h), au plus près du
  codec.
- La **surface utilisateur** (compatibilité des `.back`, refus d'une géométrie
  étrangère) est dans [Utilisation](utilisation.md#format-compact).
- Les **tests** qui verrouillent tout ça, et la méthode de la campagne de
  mesure, sont dans
  [Tests et CI](tests_et_ci.md#compaction-du-stock-disque-et-mémoire-corepacket_codecc).

## Pourquoi

`struct possibility_packet` pèse 576 octets quel que soit le remplissage du
plateau, alors qu'une possibilité du stock n'a presque aucune case occupée.
Mesuré sur un stock de production réel (`eternityII.back`, 3 407 891
possibilités, 1 963 Mo) : **19,2 pièces posées en moyenne sur 256**, soit un
plateau vide à 92,5 % payé au prix fort.

Trois redondances s'y ajoutent, vérifiées sur ces 3,4 M paquets sans une seule
exception :

1. **`b_faceused` (34 o) est intégralement déductible de `grid`** — 0
   divergence sur 3 407 891. Reconstruit au décodage : bit
   `(v - 1) % ETERN_PARTS` levé pour chaque case portant une valeur `v >= 1`.
2. **`alloc` vaut exactement le nombre de cases non vides** — 0 divergence.
   C'est le `popcount` du bitmap.
3. **13 octets de bourrage par paquet** (trou d'alignement 518-527 et queue
   563-575) partaient tels quels dans le `.back`, valeurs indéterminées
   comprises, parce que la sauvegarde faisait un `fwrite` du struct. Le codec
   sérialise donc CHAMP PAR CHAMP, en petit-boutiste explicite — même règle que
   `tests/tools/ring_codec.c`, et pour la même raison (cf. AGENTS.md, « never
   memcmp/hash the raw struct »).

**Conséquence assumée : le stock porte une forme CANONIQUE.** Un paquet dont
`alloc`/`b_faceused` contredit sa grille ne revient pas identique d'un
aller-retour, et ne correspond donc plus dans l'index du pool analysé. La
production n'en produit pas ; des fixtures de test en produisaient, et ont été
rendues cohérentes plutôt que le format rendu sans perte.

## Mesures

Sur le `.back` de production ci-dessus : **1 963 Mo → 222 Mo, soit ×8,83**,
aller-retour exact sur les 3 407 891 possibilités, zéro divergence. Une
possibilité y pèse 65 octets en moyenne, et jamais plus de
`PACKET_CODEC_MAX_BYTES` (390 sur le puzzle 256, contre 576).

Débit : **2,84 M paquets/s à l'encodage, 2,00 M/s au décodage** — environ 1,2 s
de processeur pour sauvegarder ce stock entier, négligeable devant ses E/S.

**La forme compacte est aussi celle des deux pools de STOCK en mémoire**
(`init_file_variable`, `core/datamanager.c`) : 632 → 121,2 octets par
possibilité, **2154 Mo → 413 Mo (×5,21)** sur le même stock réel. Le pool
ANALYSÉ, lui, reste en paquets bruts : son chemin chaud est une déduplication à
chaque acquittement, qu'un décodage par candidat comparé taxerait pour une
économie sans objet (il est borné par les possibilités en vol chez les clients).

**Ni un client connecté ni un pruner n'exécute le codec** — un client envoie des
paquets entiers sur TCP (`put_to_server`), un pruner acquitte dans le pool
analysé brut. Le codec tourne dans le process SERVEUR, et dans le client sans
serveur du mode `test`. C'est ce qui rend la compaction RAM gratuite pour eux, et
c'est mesuré, pas supposé : −3,3 %/−0,4 % sur l'expansion de démarrage (trafic de
stock pur), +1,6 % sur les possibilités servies à un pruner, les +0,52 µs par
possibilité étant remboursés par l'écriture de 65 octets au lieu de 576. Le banc
côté client ne tranche rien sous ±10 % (il est gouverné par la recherche) —
mesures et réserve dans
[Tests et CI](tests_et_ci.md#campagne-de-non-régression-client-et-pruner).

## Formes concurrentes : deux écartées

Trois formes ont été prototypées et mesurées sur ces mêmes 3,4 M paquets réels.
Les deux premières gagnent sur ce stock-ci et **perdent sur un plateau profond**,
ce qui en fait un piège — un stock de racines profondes les aurait fait
régresser :

| Forme | Moyenne | Ratio | Pire cas (plateau plein) |
|---|---|---|---|
| Liste `(case, id, rot)` sur 18 bits | 47,5 o | ×12,1 | **580 o** (pire que 576) |
| Liste alignée, 3 o par pièce | 61,6 o | ×9,35 | **772 o** |
| **Bitmap + plan de valeurs (retenue)** | 65,2 o | **×8,83** | **390 o** |

La forme retenue est la seule **bornée sous la taille actuelle** : elle ne peut
pas régresser, quel que soit le profil de profondeur du stock. C'est vérifié à la
compilation (`packet_codec_never_larger_than_raw`) et par un test.

## Le cas des segments de débordement : des trames de blocs

`core/stock_spill.c` emploie la même forme, rangée en **trames** : chaque trame est un
bloc de l'étage RAM (`core/stock_tier.h`, jusqu'à 64 Kio d'enregistrements compacts)
sous sa forme stockée — compressée par zstd -1 sous `make ZSTD=1` —, encadrée d'un
en-tête de 20 octets et d'un pied de 12. Un bloc de l'étage part sur disque **tel
quel**, sans décodage ni recompression.

Mesuré sur 5 M possibilités du stock de production (deux régions distinctes du
fichier, mêmes chiffres) :

| Forme d'un segment | Octets par possibilité | Contre l'ancien pas fixe |
|---|---|---|
| Pas fixe `PACKET_CODEC_MAX_BYTES` (avant) | 390 | — |
| Trames, sans zstd | 70,05 | ×5,6 |
| Trames, `make ZSTD=1` | 31,39 | **×12,4** |

La mise en trame elle-même coûte 32 octets par ~930 possibilités (0,03 octet
chacune). Un stock de 600 M possibilités tout entier sur disque passe de ~234 Go à
~19 Go.

Les segments étaient auparavant à **pas constant** (−32 % au lieu de −88,7 %), et
c'était délibéré : la sûreté du débordement — « peek puis commit », troncature du
segment de tête par décalage d'octets, « tout segment sous le sommet est exactement
plein » — était de l'arithmétique d'octets à pas constant, alors que des
enregistrements de taille variable l'auraient remplacée par un parcours arrière où un
octet de longueur faux désaligne tout en silence. La trame lève l'objection **sans
revenir au pas variable** : elle est l'unité atomique, écrite et relue en entier,
jamais entamée, comme un bloc de l'étage en RAM. Le pied donne la longueur de la trame
qu'il termine (dépilement par le haut, sans index), l'en-tête celle de la trame qu'il
ouvre (lecture par le bas : expansion, cliché) ; les deux se contrôlent l'un l'autre,
et le contenu d'un bloc est revalidé (pavage exact, somme de contrôle zstd) avant
d'être rendu. Toute l'arithmétique de la pile porte désormais sur des **frontières de
trame** :

- le sommet logique (`tail_bytes`) est toujours une fin de trame, et l'éviction
  recale le fichier dessus avant d'ajouter (`spill_trim_segment_to_tail`) ;
- un segment n'accepte plus de trame au-delà de `STOCK_SPILL_SEGMENT_RECORDS`
  possibilités (131 072) ; quand la pile roule, le segment quitté est d'abord ramené
  à son sommet logique : **tout segment sous le sommet est immuable et à sa taille
  logique**, ce qui permet de le lier dans un cliché et de reprendre ses compteurs
  sur le disque quand il redevient sommet ;
- une restauration de cliché sans collision relit les en-têtes et les pieds de chaque
  segment lié, et refuse le groupe si le compte diffère de celui du manifeste.

Conséquence assumée : le rechargement et la lecture d'expansion prennent des trames
**entières** — un pas de rechargement peut dépasser son budget d'une trame (au plus
~1 900 possibilités). Autre conséquence : des segments écrits par un binaire `ZSTD=1`
ne se relisent qu'avec zstd. Un `.back`, lui, n'en dépend pas : une sauvegarde
autonome recopie le débordement **décompressé**, en forme compacte.

Un cliché de débordement en manifeste v2 (forme compacte à pas fixe) ou v1
(`possibility_packet` bruts) désigne des segments hérités : ils sont relus au pas de
leur format, puis réécrits en trames à la restauration, jamais liés.

Une **sauvegarde autonome** (`backup` manuel, arrêt sur solution) recopie ces
segments dans le `.back`, enregistrement par enregistrement, décompressés. Son en-tête porte alors le drapeau `PACKET_CODEC_FILE_FLAG_COMPLETE`
(octet `PACKET_CODEC_FILE_FLAGS_OFFSET` = 18, réservé et nul jusque-là) : « ce
fichier porte tout le stock, ne cherchez aucun cliché à côté ». Aucun bump de
`PACKET_CODEC_FILE_VERSION` : un fichier antérieur se relit « sans drapeau », et un
binaire antérieur, qui ignore cet octet, relit un fichier drapeauté intégralement.
Voir [Utilisation](utilisation.md#débordement-sur-disque-du-stock---stock-spill-dir).

## Pistes ÉCARTÉES — ne pas les rejouer sans lire la raison

- **Encodage différentiel** (un paquet décrit par rapport à son prédécesseur).
  Tentant, `.back` et segments étant purement séquentiels, mais son gain marginal
  au-dessus de la forme bitmap est petit et il couple chaque paquet à son voisin
  dans un fichier qui doit survivre à une restauration PARTIELLE.
- **Décomposer la valeur d'une case en (identifiant, rotation)** : un bit de moins
  par case (`id_for_rotated_part` couvre exactement `[1, 4 × ETERN_PARTS]`, soit
  10 bits sur le puzzle 256), mais cette forme ne sait représenter que les valeurs
  LÉGALES. Essayée, puis retirée : elle refusait `0`, que produit un `memset(0)`,
  et cassait une demi-douzaine de fixtures — le coût de la rigueur retombait sur
  les appelants plutôt que sur la corruption qu'elle prétendait attraper. **Un
  sérialiseur ne juge pas la légalité de ce qu'on lui confie** ; seul l'ILLISIBLE
  est refusé (valeur négative autre que `-2`, valeur au-delà de la dernière
  rotation).
- **Compacter le format de FIL** (`INST_ADD`/`INST_GET`) : diviserait la bande
  passante par ~9, mais impose un bump de `VERSION` (poignée de main en
  correspondance exacte). Ce codec n'est **pas** un format de fil — le protocole
  continue d'échanger des `possibility_packet` bruts. Même arbitrage explicite que
  `tests/tools/ring_codec.h` : compacité d'abord, compatibilité plus tard si le
  besoin se confirme.
- **Compacter le stock local du CLIENT** (« étage 3 » du plan d'origine) :
  ABANDONNÉ après mesure. Le stock local est borné par `max_stock_per_thread` et la
  vraie empreinte d'un client est la map partagée en copie sur écriture ; le seul
  autre morceau qui pèse est le tampon de SORTIE `aposs->possibilities`
  (`max_stock_by_thread` × 576 octets par fork, non partagés), qu'encoder coûterait
  sur un chemin semi-chaud. Le client mono (mode `test`, petits formats) est le seul
  à exécuter le codec côté client, et il n'a pas de problème de mémoire.

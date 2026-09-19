# Compaction du stock — mémoire serveur, mémoire client, stockage disque

**Statut** : **étage 1 livré** (stockage disque : `.back` et segments de
débordement, cf. [utilisation.md](../utilisation.md#format-compact)). Les
étages 2 et 3 sont des **propositions non implémentées** — ce document est la
référence de ce qui a été mesuré, pas du comportement actuel (cf.
[README de ce répertoire](README.md)).

## D'où ça vient

La branche `border-mass-walker-design` (étude du walker de bordure, non fusionnée) a produit, chemin faisant, une série de
solutions aux problèmes de mémoire et d'espace disque qu'elle rencontrait :
format compact `packed6` pour les anneaux, forme disque plus étroite que la
forme RAM pour les états de la DP, tri externe, écriture tamponnée, outil de
vérification à mémoire bornée. La question posée était : lesquelles se
transposent au stock, et pour quel gain ?

Trois d'entre elles se transposent, et deux ne se transposent pas.

| Solution de la branche | Levier là-bas | Transposable au stock ? |
|---|---|---|
| `ring_codec` packed6 (rotation déductible, index local, taille fixe, magie+version) | x12,8 disque | **Oui, le principe** — c'est la matrice de l'étage 1 |
| Forme disque étroite ≠ forme RAM (`bd_entry` 32→24 o) | -25 % d'octets = -25 % de temps en régime write-bound | **Oui, directement** |
| Disposition de champ calculée sur le domaine réel, échec bruyant si ça déborde | 111 → 32 o par état | **Oui, l'esprit** |
| Écriture tamponnée, tampon multiple exact de l'enregistrement | x22 | **Oui** |
| `check_rings` : valider un fichier à mémoire bornée | — | **Oui**, rien d'équivalent côté stock |
| Tri externe (niveau = tableau trié, pas table de hachage) | garde la fusion au-delà de la RAM | **Non** : le stock n'est pas un niveau de DP |
| Masques 64 bits `bw_fastmap` | x1,66 | **Non** : c'est déjà `bucket_id_mask` dans le moteur principal |
| Scission d'un niveau en fragments | — | **Non**, écartée avec preuve dans la branche |

## Ce que dit le stock réel

Mesuré sur `eternityII.back`, un stock de production : **3 407 891
possibilités, 1 963 Mo**, profondeur **19,2 pièces posées sur 256** en moyenne
(min 19, max 152 ; 88 % pile à 19).

Trois redondances, vérifiées sur les 3,4 M paquets sans une seule exception :

- **`b_faceused` (34 o) est 100 % déductible de `grid`** — 0 divergence.
- **`alloc` vaut exactement le nombre de cases non vides** — 0 divergence.
- **`min_candidats` n'est jamais renseigné** dans ce fichier, et `checked` vaut
  1 partout.

Et un défaut de forme : `fwrite` de la structure écrivait **13 octets de
bourrage indéterminé par paquet** (trou d'alignement 518-527, queue 563-575),
soit 4 % du fichier en octets qui ne veulent rien dire — exactement ce que la
règle « jamais un `fwrite` de struct » de `ring_codec.h` interdit.

Le fond du problème : **un plateau vide à 92,5 % coûte le même prix qu'un
plateau plein.** C'est la même observation qui a fait passer un anneau de
bordure de 576 à 45 octets dans la branche.

## Les formes mesurées

Quatre prototypes, sur les mêmes 3,4 M paquets réels, aller-retour exact
vérifié à chaque fois :

| Forme | Moyenne | Ratio | Pire cas (plateau plein) | encode | decode |
|---|---|---|---|---|---|
| Actuelle (brute) | 576 o | — | 576 o | — | — |
| Liste `(case, id, rot)` sur 18 bits | 47,5 o | x12,1 | **580 o** | 1,1 M/s | 2,4 M/s |
| Liste alignée, 3 o par pièce | 61,6 o | x9,35 | **772 o** | 3,7 M/s | 9,2 M/s |
| **Bitmap + plan de valeurs (retenue)** | 65,2 o | **x8,83** | **390 o** | 2,8 M/s | 2,0 M/s |

Les deux formes « liste » sont plus petites sur ce stock-ci et **plus grosses
que 576 octets sur un plateau profond** : un stock de racines profondes les
ferait régresser. La forme retenue est la seule **bornée sous la taille
actuelle**, propriété vérifiée à la compilation et par un test.

## Étage 1 — stockage disque (LIVRÉ)

`core/packet_codec.{h,c}`, branché sur `backup`/`backup_analysed`/
`consistent_backup`/`import`/`import_analysed` (taille variable) et sur les
segments de `core/stock_spill.c` (même forme, **pas fixe**).

Résultat mesuré bout en bout sur le stock de production :
**1 962 945 216 → 222 314 458 octets**, 3 407 891 possibilités relues à
l'identique. Côté débordement : **-32 %**, moins spectaculaire parce que le pas
fixe impose de réserver la taille du plateau plein — arbitrage détaillé dans
[utilisation.md](../utilisation.md#débordement-sur-disque-du-stock---stock-spill-dir)
et dans `spill_record_bytes` (`core/stock_spill.c`).

Aucun impact sur la recherche : le codec ne travaille qu'à la frontière d'E/S.

`bench_refutation --from-back` a dû être repris au passage : il lisait le `.back`
au pas de 576 octets, ce qui aurait silencieusement fabriqué des plateaux
absurdes sur un fichier compact. Il détecte désormais le format comme `import`.
C'est le genre de lecteur qu'un changement de format d'échange doit chercher
explicitement — rien ne l'aurait signalé.

### Ce qui reste ouvert à cet étage

- **Enregistrements de taille variable dans les segments de débordement**
  (x8,8 au lieu de x1,5). Demande de remplacer l'arithmétique d'octets à pas
  constant du débordement par un parcours arrière à suffixe de longueur — sur
  le mécanisme dont le contrat est « aucune possibilité perdue ». À ne faire
  qu'avec une mesure qui le justifie.
- **Tampon d'écriture multiple exact de l'enregistrement.** `backup` pose bien
  un `setvbuf` d'un mégaoctet, mais 1 Mio n'est pas un multiple de la taille
  d'un enregistrement : un `kill` pendant une sauvegarde peut donc couper au
  milieu d'un enregistrement. La branche a résolu exactement ça pour les
  anneaux (`bm_rings_setvbuf`) — ce que perd un `kill` devient alors au pire
  un tampon d'enregistrements ENTIERS.
- **Un outil de vérification à mémoire bornée**, calqué sur `check_rings` :
  cohérence, comptage, doublons sur un `.back`.

## Étage 2 — mémoire serveur (NON IMPLÉMENTÉ)

Stocker la forme compacte **dans les pools** eux-mêmes, pas seulement sur
disque. Aujourd'hui une possibilité résidente coûte
`datamanager_bytes_per_possibility()` = `sizeof(Element)` (24) +
`sizeof(struct possibility_packet)` (576) + 2 surcoûts d'allocation, soit
environ **632 octets** dont 91 % de paquet.

Gain attendu : **~110 octets par possibilité, soit x5,7 de stock à plafond RAM
égal** — et un débordement disque repoussé d'autant. Coût en temps : 285 ns à
l'ADD et 200 ns au GET d'après les prototypes, à comparer au `memcpy` de 576
octets et à l'aller-retour TCP déjà payés sur ces chemins.

Le vrai coût est en travail : **57 accès directs à `currElement->value`** dans
`core/datamanager.c` (plus quelques-uns dans `possibility.c`/`stock_spill.c`)
devraient passer par un décodage. C'est contenu à un fichier, mais c'est le
cœur d'un mécanisme déjà chargé — à faire après l'étage 1, jamais avant.

## Étage 3 — mémoire client (NON IMPLÉMENTÉ)

Même forme pour le stock local du client. **Aucun impact sur la recherche**, et
c'est démontrable : `search_packet_backtracking_mrv` fait un `memcpy` de la
racine vers `board` au début puis ne retouche plus jamais le paquet — un
décodage par racine, amorti sur des millions de nœuds.

Mais le gain est faible : le stock local est borné par `max_stock_per_thread`,
et la vraie empreinte d'un client est la map partagée en copie sur écriture.
Le seul autre morceau qui pèse est le tampon `aposs->possibilities`
(`max_stock_by_thread` x 576 octets **par fork, non partagés**) — et c'est un
tampon de SORTIE, donc l'encoder coûterait sur un chemin semi-chaud.

**Recommandation : faire l'étage 3 par héritage de l'étage 2 (même codec, même
stock local), et ne PAS toucher aux tampons de fork.**

## Pistes écartées, avec la raison

- **Encodage différentiel** (le « 6,4 o par anneau, x90 » écarté dans la
  branche parce qu'il perd l'accès aléatoire). Tentant ici puisque `.back` et
  segments sont purement séquentiels — mais son gain marginal au-dessus de la
  forme bitmap est petit, et il couplerait chaque paquet à son prédécesseur
  dans un fichier à son prédécesseur
  dans un fichier qui doit survivre à une restauration partielle.
- **Décomposer la valeur d'une case en (identifiant, rotation)** : un bit de
  moins par case, mais ne sait représenter que les valeurs légales. Essayé,
  puis retiré — voir la section suivante.
- **Compacter le format de FIL** (INST_ADD/INST_GET). Diviserait la bande
  passante par ~9, mais impose un bump de `VERSION` (poignée de main en
  correspondance exacte). Même arbitrage explicite que `ring_codec.h` :
  compacité d'abord, compatibilité plus tard si le besoin se confirme.

## Une leçon de méthode, payée en allers-retours

La première version du codec **refusait toute valeur de case hors du domaine
légal** (`-2`, ou un identifiant de pièce pivotée valide), au nom de
l'intégrité : un paquet corrompu ne devait pas se sauvegarder sous une forme
que le décodeur relirait sans broncher.

Elle a fait échouer une demi-douzaine de fixtures de test réparties sur quatre
fichiers, toutes construisant un paquet par `memset(0)` — idiome répandu dans
cette base pour dire « le contenu n'importe pas ici ». Chaque échec appelait
une correction de test pour faire passer le format.

C'était le signal, et il a été suivi trop tard : **un sérialiseur n'a pas à
juger de la légalité de ce qu'on lui confie.** Le domaine a été élargi à
`[0, 4 x ETERN_PARTS]` pour une case non vide — 1 bit de plus par case, x8,83
au lieu de x9,56 — et le refus ne porte plus que sur ce qui est réellement
irreprésentable. La validation d'un plateau reste possible, mais ailleurs.

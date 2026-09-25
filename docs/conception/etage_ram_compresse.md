# Étage RAM compressé du stock, avant le débordement disque

**Statut : en cours d'implémentation (PR 1/3).** La structure de données existe
(`core/stock_tier.{h,c}`, tests `tests/core/test_stock_tier.c`), mais n'est branchée nulle
part : le comportement du serveur est inchangé. Les mesures ci-dessous viennent de
`make bench-ram-tier` ([Tests et CI](../tests_et_ci.md#banc-de-létage-ram-compressé-make-bench-ram-tier)).

## Le constat

Une possibilité du stock pèse **70 octets** en forme compacte
([format_stock_compact.md](../format_stock_compact.md)), mais **112 octets** en RAM. La
différence tient au rangement : chaque possibilité est un maillon `Element` d'une liste
doublement chaînée (`core/lifo.h`), avec deux pointeurs, une longueur et un en-tête de
chunk malloc. **38 % de la RAM du stock sert au chaînage**, pas aux données.

C'est le seul rangement possible pour la partie **chaude** du stock : on y dépile par la
queue, on y insère, on y retire au milieu (purge des descendants, bail récupéré), on la trie.
La partie **froide**, elle, ne sert qu'à deux choses : partir sur disque par la tête
(éviction), en revenir par blocs (rechargement). Pour elle, le chaînage est un pur coût.

Le stock de production en montre l'échelle : son `.back` pèse 18,35 Go, soit environ
**262 millions de possibilités** à 70 octets. L'exemple d'`events.log` de
[utilisation.md](../utilisation.md#débordement-sur-disque-du-stock---stock-spill-dir) décrit
une expansion de cette taille sous un plafond de 42 Go, avec l'essentiel du stock sur disque.
En liste chaînée, ce stock demanderait 29 Go de maillons, alors que ses données n'en
représentent que 18.

## Mesures

`make bench-ram-tier` sur le `.back` de production (18,35 Go, environ 262 M de possibilités,
toutes à 23 pièces posées). L'échantillon compte 2 M de possibilités, lues à trois endroits du
fichier (début, milieu, fin), puis relues **mélangées** dans un ordre aléatoire. Les quatre
séries donnent les mêmes chiffres à ±0,1 octet près, et des débits à quelques % près.

Protocole : chaque configuration tourne dans un processus fils. Le banc remplit une `File`
exactement comme le pool (`init_file_variable` + `put_sized`), puis l'**évince** par la tête
(`scroll_fifo_sized`, comme le débordement) vers des blocs. Il la **recharge** ensuite, bloc le
plus récent d'abord : décompression, redécoupage, `put_sized`. Enfin il vérifie le résultat :
même compte et même multi-ensemble d'enregistrements (somme de hachages), sinon il sort en
échec. Deux sabotages (un bit du bitmap, un bit d'une valeur) sont bien détectés. Les
octets comptés sont ceux que l'allocateur tient (`mallinfo2` pour la liste, `malloc_usable_size`
plus l'en-tête de chunk pour les blocs), et non la seule charge utile.

| Rangement | Bloc | Octets/poss. | vs liste | Éviction (M poss./s) | Rechargement (M poss./s) |
|---|---|---|---|---|---|
| Liste chaînée (aujourd'hui) | — | 112,0 | ×1,00 | — | 5,4 (insertion) |
| Blocs contigus, sans compression | 64 Kio | 70,0 | **×1,60** | 19,7 | 4,6 |
| Blocs + lz4 | 64 Kio | 36,6 | ×3,06 | 4,2 | 4,0 |
| Blocs + zstd -1 | 16 Kio | 32,7 | ×3,43 | 2,3 | 2,9 |
| **Blocs + zstd -1** | **64 Kio** | **31,4** | **×3,57** | **2,7** | **3,1** |
| Blocs + zstd -1 | 256 Kio | 29,7 | ×3,77 | 2,6 | 3,2 |
| Blocs + zstd -3 | 256 Kio | 26,5 | ×4,23 | 1,7 | 3,1 |

Trois lectures :

1. **Rien qu'en supprimant le chaînage, on gagne ×1,6, sans aucune compression.** Les
   blocs non compressés coûtent 70,0 octets par possibilité, c'est-à-dire la charge utile
   seule, et l'éviction vers un bloc brut n'est qu'une copie (19,7 M/s).
2. **La forme compacte se compresse encore d'un facteur ×2,2 environ.** La redondance vient
   de la **population** : les mêmes cases sont occupées d'une possibilité à l'autre, les mêmes
   pièces reviennent. Elle ne doit rien à la parenté entre voisins, puisque l'échantillon
   mélangé se compresse exactement autant. C'est la bonne nouvelle : le ratio ne dépend pas de
   l'ordre dans lequel les clients déposent, et la tête d'une file, qui entrelace leurs
   dépôts, le garde.
3. **Le rechargement est dominé par la réinsertion, pas par la décompression.** Sans
   compression il tourne à 4,6 M/s, avec zstd -1 à 3,1 M/s. L'essentiel du temps part dans
   `put_sized`, c'est-à-dire un malloc par possibilité. Le seul autre rythme à comparer est
   le tick du débordement, 4 096 possibilités toutes les 100 ms (41 000/s) : le codec est
   hors de cause d'un facteur 60.

Projection sur le stock de production (262 M possibilités) :

| Rangement | RAM pour tout le stock |
|---|---|
| Liste chaînée | 29,4 Go |
| Blocs sans compression | 18,3 Go |
| Blocs + lz4, 64 Kio | 9,6 Go |
| **Blocs + zstd -1, 64 Kio** | **8,2 Go** |

Sous un plafond de 42 Go, **tout le stock tiendrait en RAM**, avec zstd comme sans
compression, là où la liste chaînée en envoie aujourd'hui la plus grande part sur disque, relue
et réécrite à chaque passe d'expansion.

Quant à zlib, un premier essai (Python, niveau 1, blocs de 64 Kio) donne ×2,24 pour
45 Mo/s environ, soit 0,6 M poss./s. Il est plus lent que zstd -1 et compresse moins : écarté.

## Proposition

### Trois étages par file

```
 queue (chaud)                                                         tête (froid)
 ┌──────────────────────────┐   éviction   ┌──────────────────┐   éviction   ┌────────────┐
 │ liste chaînée (Element)  │ ───────────▶ │ blocs RAM (pile) │ ───────────▶ │ segments   │
 │ inchangée                │ ◀─────────── │ compressés       │ ◀─────────── │ disque     │
 └──────────────────────────┘ rechargement └──────────────────┘ rechargement └────────────┘
```

- **Éviction** : là où le débordement écrit aujourd'hui sur disque, il emballe les
  possibilités les plus froides (tête de file, `scroll_fifo_sized`) dans un bloc de 64 Kio,
  le compresse et l'empile. Quand l'étage dépasse sa part du plafond, ce sont ses blocs les
  **plus anciens** (le bas de la pile) qui partent sur disque. L'ordre reste cohérent : tout
  ce qui est sur disque est plus ancien que tout ce qui est dans l'étage.
- **Rechargement** : on prend d'abord le bloc du haut de l'étage, puis le disque, comme
  aujourd'hui. L'ordre LIFO du stock est conservé.
- **Le bloc est l'unité atomique.** Il est décompressé en entier, tous ses enregistrements
  sont insérés, et il n'est libéré qu'ensuite : c'est le « peek puis commit » du débordement.
  Un bloc porte un en-tête minimal (nombre d'enregistrements, octets bruts, octets stockés),
  et la longueur de chaque enregistrement se déduit de son bitmap
  (`packet_codec_peek_placed`). Dans un bloc, les enregistrements peuvent donc être de taille
  variable **sans** l'arithmétique d'octets à pas variable qui a fait écarter cette option
  pour les segments ([format_stock_compact.md](../format_stock_compact.md#le-cas-des-segments-de-débordement--pas-fixe-délibérément)) :
  on ne tronque jamais un bloc, on le consomme en entier ou pas du tout.
- **La liste chaude ne change pas.** Toutes les opérations par maillon (tri, purge des
  descendants, déduplication, rééquilibrage, baux) continuent de travailler sur elle, sans
  modification.

### Où vit le code

Un module `core/stock_tier.{h,c}` pur : pile de blocs par file, emballage, déballage, codec.
C'est `core/stock_spill.c` qui le pilote, puisqu'il possède déjà le thread, les seuils
90/75/25 %, le crochet de dégagement (`stock_spill_relieve`) et la fenêtre de maintenance. La
règle de couche reste la même : `stock_tier.c` peut dépendre de `datamanager.h`, jamais
l'inverse. En particulier, `put_to_pool` continue d'ignorer l'existence de l'étage.

### Codec

- **PR sans dépendance** : blocs non compressés (×1,6).
- **zstd niveau 1, blocs de 64 Kio**, activé par `make ZSTD=1` sur le modèle de `NCURSES=1`
  et `CUDA=1`. Sans cette option, la compilation ne change pas et l'étage reste en blocs
  bruts. Les blocs de 256 Kio gagnent 5 % de ratio, mais quadruplent l'unité de
  rechargement (≈ 3 700 possibilités au lieu de ≈ 930).

## Invariants à tenir

1. **L'étage compte dans l'occupation.** Les octets des blocs entrent dans
   `datamanager_resident_bytes`, donc dans `--stock-max-ram`, `stockMemory` et
   `GET /api/v1/stats`. Sans cela, le plafond ne plafonne plus rien : c'est le même piège
   que la file de travail d'expansion non comptée (AGENTS.md, *RAM cap & disk spillover*).
   Comme `core/` ne peut pas lire `stock_tier.c`, le compteur doit être injecté (un crochet,
   comme `datamanager_set_ram_relief_hook`) ou tenu par `datamanager` lui-même via une
   interface étroite. L'invariant est le même que pour `File.bytes` : un compteur **par
   file**, pas seulement un total.
2. **Aucune perte.** Un bloc n'est libéré qu'après l'insertion confirmée de tous ses
   enregistrements (vers la liste ou vers le disque). Si un bloc ne peut pas être
   compressé, il est gardé brut, jamais jeté.
3. **Fenêtre de maintenance** : l'étage est inerte pendant la fenêtre, exactement comme
   `stock_spill_step`. En revanche, il est utilisable par le crochet de dégagement, puisque
   le détenteur de la fenêtre doit pouvoir faire de la place lui-même.
4. **Expansion** : une passe lit aujourd'hui le stock débordé par le bas de chaque pile,
   sous une frontière posée au début de la passe
   (`stock_spill_expansion_begin/take/end`). L'étage fait partie de cette pile : la passe le
   lit après le disque (le bas de la pile), sous la même frontière, et ses propres enfants
   évincés montent au-dessus. **C'est le point le plus délicat**, et il n'est pas
   facultatif : sans lui, l'étage ne serait jamais développé, comme le disque l'était avant
   que l'expansion apprenne à le lire.
   Pendant une expansion, l'étage ne recharge pas (même règle que le disque).
5. **Sauvegarde et restauration** : l'étage est en RAM, donc perdu en cas d'arrêt brutal.
   Toute sauvegarde doit l'inclure, y compris l'autosauvegarde : le cliché du disque par
   liens physiques n'a pas d'équivalent pour de la RAM. `consistent_backup` décompresse
   l'étage vers le `.back` dans la fenêtre de gel, comme la sauvegarde autonome (#342)
   recopie le débordement disque. Le `.back` reste au format compact actuel, lisible par tout
   binaire existant. Coût : décompresser tout l'étage, soit
   environ 262 M possibilités à plus de 3 M/s, moins de 90 s de CPU pour le stock de
   production. C'est à comparer aux 18 Go d'écriture que la sauvegarde paie déjà.
6. **Parcours complets du pool** (`checkOrigin`, histogramme par niveau, `sortAsc*`) :
   l'étage a le même statut que le disque, qu'ils ne couvrent pas aujourd'hui
   ([checkorigin_stock_deborde.md](checkorigin_stock_deborde.md)). Une différence est à
   signaler : sous un plafond **sans** `--stock-spill-dir`, le débordement est inerte et ces
   parcours voient tout le stock ; si l'étage y est actif, ce n'est plus le cas. Chacun doit
   soit décompresser l'étage (l'histogramme n'a besoin que de `packet_codec_peek_placed`),
   soit avertir comme le fait `checkOrigin` pour le disque.
7. **Le codec ne tourne que sur le serveur.** Le client et le pruner n'en exécutent aucun,
   comme pour la forme compacte : la dépendance `ZSTD=1` ne concerne que le binaire serveur,
   et un client sans libzstd reste possible.

## Découpage en PR

1. **`core/stock_tier.{h,c}`, isolé** — **livrée.** Pile de blocs, emballage, déballage,
   blocs bruts. Tests unitaires : aller-retour exact, bloc consommé en entier, refus d'un bloc
   incohérent, compteur d'octets par file. Rien n'est branché. L'API a été taillée pour la
   PR 2 : un bloc entre sous forme d'octets bruts déjà concaténés (`stock_tier_push`), ce que
   produira un drainage de la tête de file en forme compacte ; il se relit sans être retiré
   (`stock_tier_block_unpack`), puis se retire par le haut (rechargement) ou par le bas
   (transfert vers le disque, lecture par une expansion) ; `stock_tier_block_above` parcourt
   la pile du bas vers le haut sans rien retirer (sauvegarde). Chaque bloc porte déjà un octet
   de codec (`STOCK_TIER_CODEC_RAW`), pour que la PR 3 n'ait pas à changer sa structure.
2. **Branchement, blocs bruts** : éviction et rechargement via le débordement, comptage dans
   `datamanager_resident_bytes`, sauvegarde et restauration, lecture par l'expansion,
   affichage de l'étage dans `stockMemory` et `GET /api/v1/stats`. Gain ×1,6 sans dépendance.
   Ces pièces doivent arriver **ensemble** : un étage non sauvegardé ou non développé par
   l'expansion serait une régression.
3. **`make ZSTD=1`** : codec zstd niveau 1, repli en blocs bruts sans l'option, job CI
   compilant la variante. Gain ×3,6.

## Alternatives non retenues

- **Remplacer la liste chaînée de TOUT le pool par une liste de blocs** (liste déroulée).
  Ce serait ×1,6 sur tout le stock, pas seulement sur sa partie froide. Mais toutes les
  opérations par maillon (retrait au milieu, déplacement, tri, index du pool analysé)
  seraient à réécrire. L'étage obtient le même gain sur la partie froide, qui est la plus
  grosse, sans toucher à aucune d'elles.
- **zlib** : plus lent que zstd -1 et moins bon en ratio (voir plus haut).
- **Compresser la liste chaude** : chaque accès par maillon paierait un décodage, pour la
  partie du stock qui justement bouge.

## Points ouverts

- **Partage du plafond entre la liste chaude et l'étage.** L'étage garde-t-il une part
  fixe (option), ou reçoit-il tout ce que la liste ne retient pas au-dessus d'un plancher
  de possibilités chaudes ? La liste doit garder de quoi servir les GET sans recharger à
  chaque tick.
- **L'étage sans `--stock-spill-dir`.** Il donnerait au plafond seul un recours sans
  disque. Faut-il l'activer par défaut sous `--stock-max-ram` ?
- **Dictionnaire zstd.** La redondance étant statistique (point 2 des mesures), un
  dictionnaire entraîné sur un échantillon du stock pourrait donner à des blocs de 16 Kio le
  ratio de blocs de 256 Kio. Non mesuré.
- **lz4 embarqué ou zstd en dépendance ?** lz4 tient en un fichier source (BSD) qu'on
  pourrait embarquer dans le dépôt, sans dépendance système : ×3,06 contre ×3,57.
- **Des segments disque faits de blocs.** Les segments sont à pas fixe de 390 octets par
  possibilité, parce que leur sûreté repose sur l'arithmétique à pas constant. Un segment
  constitué de blocs de l'étage, consommés en entier comme en RAM, garderait une unité
  atomique **et** diviserait le disque par environ 12 (390 octets contre 31). C'est une suite
  naturelle, hors du périmètre de cette proposition.
- **macOS** : le banc mesure l'allocateur glibc (`mallinfo2`) et ne compile que sous Linux.
  Le serveur de production tourne sous Linux.

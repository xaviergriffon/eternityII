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

## Étage 2 — mémoire serveur (LIVRÉ)

Stocker la forme compacte **dans les pools** eux-mêmes, pas seulement sur
disque. Aujourd'hui une possibilité résidente coûte
`datamanager_bytes_per_possibility()` = `sizeof(Element)` (24) +
`sizeof(struct possibility_packet)` (576) + 2 surcoûts d'allocation, soit
environ **632 octets** dont 91 % de paquet.

**Mesuré sur le stock de production** (3 407 891 possibilités) : **632 → 121,2
octets par possibilité, soit x5,21** — 2154 Mo deviennent 413 Mo. Le
débordement disque est repoussé d'autant.

Trois changements, dans cet ordre :

1. **Isolation** — les 57 accès directs à `Element.value` de `core/datamanager.c`
   passent derrière une API d'OPÉRATIONS (`element_hash`, `element_equals`,
   `element_placed`, `element_load`…), jamais des accesseurs : un accesseur
   rendant un `possibility_packet *` aurait figé la représentation aussi
   sûrement qu'avant, puisqu'il suppose qu'un paquet décodé existe et survit au
   retour.
2. **Plafond en octets** — sans quoi le gain serait invisible : `--stock-max-ram`
   convertissait une fois pour toutes les Mo en NOMBRE de possibilités, au tarif
   d'une constante de 632 octets. Chaque ajout est désormais confronté aux
   octets réellement résidents.
3. **Stockage compact** — `Element` porte sa charge utile EN PLACE (tableau
   souple), ce qui supprime une allocation par possibilité ET autorise des
   tailles différentes dans une même file.

**Le pool ANALYSÉ reste en forme brute, délibérément.** Le gain est dans le
stock (millions de possibilités) ; le pool analysé est borné par les
possibilités en vol chez les clients — c'est pourquoi `--stock-max-ram` ne l'a
jamais couvert. Et son chemin chaud est la déduplication à chaque
acquittement, optimisée exprès (index de `add_possibility_analysed`) : la forme
compacte y ferait payer un décodage par candidat comparé, pour une économie
sans objet. L'API d'éléments accepte les deux formes, discriminées par la
LONGUEUR — sans ambiguïté possible, un enregistrement compact étant toujours
strictement plus court qu'un paquet entier.

### Deux bugs trouvés en rendant la suite verte

- **`removeNoNext` décrémentait le compteur d'octets de `sizeofvalue`**, qui
  vaut 0 pour une file de pool : le compteur dérivait vers le haut à chaque
  élagage, donc le plafond RAM se resserrait tout seul, sans que rien ne le
  signale.
- **Un `memset(0)` produit un plateau COMPLET**, la case vide valant `-2` et non
  `0`. Dès que `alloc` est déduit de la grille, un tel paquet est une SOLUTION —
  et les chemins qui en rencontrent une appellent `exit()` sous
  `--stop-on-solution`. Le runner de tests sortait alors en plein milieu, avec
  un code 0 et sans ligne de résumé : un faux succès. `tests/packet_fixture.h`
  existe pour que ça ne puisse plus arriver.

### Performance client et pruner : mesurée, pas déduite

La compaction échange de la mémoire contre du calcul. La question posée était
donc : est-ce que le CLIENT ou le PRUNER y perdent ? L'objectif n'était pas un
gain, seulement l'absence de dégradation notable.

**Premier constat, structurel : ni l'un ni l'autre n'exécute le codec.** La
forme compacte est confinée aux deux pools de STOCK (`init_file_variable`,
`core/datamanager.c`) ; le pool analysé reste en paquets bruts. Un client
connecté à un serveur envoie un `possibility_packet` entier sur TCP
(`put_to_server`) et n'écrit dans son pool local que sur refus du serveur ; un
pruner reçoit des lots bruts et acquitte dans le pool analysé. Le codec ne
tourne donc que **dans le processus serveur** (et dans le client mono du mode
`test`, sans serveur). Le risque pour un client est indirect : un serveur qui
sert moins vite l'affame.

**Coût absolu du codec** (200 000 paquets, 3 répétitions, aller-retour
encodage + décodage, comparé au `memcpy` du struct entier que le stock payait
avant) :

| pièces posées | octets/enr. | encode+decode | `memcpy` aller-retour |
|---|---|---|---|
| 8 | 49 | 0,81 µs | 0,40 µs |
| **19** (moyenne du stock réel) | 65 | **0,91 µs** | 0,39 µs |
| 130 | 217 | 1,53 µs | 0,38 µs |
| 255 | 389 | 2,28 µs | 0,39 µs |

Soit **+0,52 µs par possibilité traversant le stock** à la profondeur réelle de
production.

**Bout en bout, ordre ALTERNÉ entre variantes** — la machine dérive
thermiquement de plusieurs pour cent à l'heure, donc « toutes les répétitions de
A puis toutes celles de B » compare deux températures autant que deux codes —
binaires construits une fois et rejoués :

| régime | instrument | master | compact | écart |
|---|---|---|---|---|
| **trafic de stock PUR** — expansion au démarrage, niveau 14 (142 415 possibilités), 11 répétitions | temps de l'expansion | 1,350 s | **1,306 s** | **−3,3 %** |
| idem, niveau 18 (1 075 265 possibilités), 7 répétitions | temps de l'expansion | 15,987 s | **15,928 s** | −0,4 % |
| **pruner** — serveur + pruner 4 forks, fenêtre 65 s, 3 répétitions | possibilités servies (`GET_TO_CHECK`, décodage) | 73 400 | **74 600** | +1,6 % |
| idem | ADD encaissés (encodage) | 53 053 | **53 985** | +1,8 % |
| **client** — serveur + client 4 forks, fenêtre 65 s, 8 répétitions | ADD encaissés | 31 850 | 31 500 | −1,1 % |

**Aucune dégradation, et un léger gain là où le codec pèse le plus.** Ce n'est
pas paradoxal : les 0,5 µs d'encodage sont remboursés par les 65 octets écrits
au lieu de 576, une allocation de moins par possibilité (charge utile portée
en place) et un cache bien mieux utilisé.

Deux précautions de lecture, qui sont le vrai enseignement de cette campagne :

- **Les écarts positifs ci-dessus ne sont pas des gains à annoncer.** Ils sont
  de l'ordre du bruit de l'instrument ; seul le signe compte, et il exclut la
  dégradation.
- **Le banc CLIENT ne résout rien en dessous de ±10 %** : son étendue relative
  est de 14 % (master) et 34 % (compact) sur 8 répétitions, parce que son débit
  est gouverné par la recherche, chaotique, et non par le stock — 490 ADD/s,
  contre ~2 000 opérations de codec par seconde dans le régime pruner. Son
  −1,1 % médian ne peut donc pas être imputé au codec : le MÊME codec, sollicité
  quatre fois plus fort, ne coûte rien. C'est l'expansion, quatre fois plus
  précise et purement stock, qui répond pour lui. Un run a même dû être
  diagnostiqué plutôt que moyenné (133 969 possibilités produites contre 142–147 k
  ailleurs) : collision sur le port 2020, non paramétrable en CLI, avec un autre
  `eternityII` de la machine.

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

## Tranché : `alloc` est une donnée DÉRIVÉE, jamais stockée

La forme compacte ne stocke ni `alloc` ni `b_faceused` : elle les reconstruit
depuis la grille au décodage. Tant que ça ne concernait que le disque, la
question ne se posait pas — `import()` recompte de toute façon `alloc` sans
condition sur toute possibilité restaurée. Le stockage EN MÉMOIRE la pose
frontalement : un pool qui redérive un champ **réécrit** ce qu'on lui a confié.

La tentation était de stocker `alloc` quand il contredit la grille (faisable
pour zéro octet, dans un octet inutilisé plus un bit libre). Elle a été écartée
après vérification des faits :

1. **Les onze écritures de `alloc` en production sont, à une près, littéralement
   `possibility_placed_count(...)`.** La onzième (`generate_possibility_packet`)
   pose 0 et est normalisée avant que le paquet n'atteigne quoi que ce soit de
   durable.
2. **`import()` recompte `alloc` sans condition** sur chaque possibilité
   restaurée, documenté comme idempotent : le projet traite déjà un `alloc`
   stocké comme non digne de confiance.
3. **`AGENTS.md` le définit** comme « nombre de cases non vides de la grille ».
4. **0 divergence sur 3 407 891 possibilités de production.**

Stocker `alloc`, c'était donc conserver une valeur que la base de code répare
déjà partout ailleurs — et faire diverger la forme disque de la forme mémoire,
alors que les segments de débordement sont les deux à la fois.

**Conséquence assumée** : le pool range une forme CANONIQUE. Une possibilité
dont `alloc` ou `b_faceused` contredit sa grille n'en ressort pas identique, et
ne se retrouve donc plus dans l'index du pool analysé — sa déduplication compare
`x`, `y`, `alloc`, les pièces utilisées et le plateau. Production n'en produit
pas ; une cinquantaine de fixtures de test, si — elles construisent un paquet
par `memset(0)` puis lui posent un `alloc`, or une grille à zéro n'a aucune case
vide (la case vide vaut `-2`) et annonce donc `ETERN_PARTS` pièces posées. Ces
fixtures sont à rendre cohérentes (`fixture_packet`), pas le format à rendre
lossless.

Le seul chemin de production où un `alloc` déclaré peut contredire sa grille est
`read_from_json` (commande `loadJson`, valeurs fournies par un opérateur) : il
est désormais normalisé à l'insertion, exactement comme `import()` normalise un
`.back`. C'est une cohérence gagnée, pas une régression.

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

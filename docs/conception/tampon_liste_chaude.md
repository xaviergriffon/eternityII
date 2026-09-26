# Liste chaude du stock : un tampon en nombre de possibilités, pas en pourcentage du plafond

**Statut : proposition.** Rien de ce qui suit n'est implémenté. Le comportement actuel est
décrit dans [Utilisation](../utilisation.md#étage-ram-en-blocs---stock-hot-floor---stock-hot-reload) ;
l'étage RAM lui-même, ses mesures et ses invariants, dans
[etage_ram_compresse.md](etage_ram_compresse.md). Ce document répond au premier de ses
points ouverts (« partage du plafond entre la liste chaude et l'étage »), laissé en
suspens quand l'étage a été branché.

## Le constat

Sous `--stock-max-ram`, le stock vit sur trois étages, dans l'ordre de pile : la **liste
chaude** (maillons de liste chaînée, ≈ 120 octets par possibilité), l'**étage RAM** (blocs
de 64 Kio, 70 octets, **31** sous `make ZSTD=1`), puis le **disque**. Tous les seuils qui
pilotent les passages d'un étage à l'autre sont des **pourcentages du plafond** :

| Seuil | Défaut | Rôle |
|---|---|---|
| `--stock-hot-floor` | 25 % | la liste au-dessus part dans l'étage (compression proactive) |
| `--stock-hot-reload` | 10 % | la liste en dessous recharge un bloc de l'étage |
| `STOCK_SPILL_HIGH_PERCENT` | 90 % | l'occupation au-dessus envoie l'étage sur disque |
| `STOCK_SPILL_LOW_PERCENT` | 75 % | fin de l'éviction disque |
| `STOCK_SPILL_RELOAD_PERCENT` | 25 % | rechargement disque, chemin sans étage seulement |

Depuis la PR #350, les deux premiers sont en outre **partagés entre les deux pools** selon
leur demande de la dernière minute (`stock_spill_pool_shares`), chaque pool gardant au
moins 10 % (`STOCK_SPILL_POOL_SHARE_MIN_PERMILLE`).

Les trois derniers ont un sens en pourcentage : ils protègent le plafond. Les deux premiers
n'en ont pas. **À quoi sert la liste chaude ?** À servir les `GET` sans attendre une
décompression : c'est un **tampon de latence**, dont la bonne taille dépend du débit de la
demande et du temps de rechargement d'un bloc — deux grandeurs qui ne changent pas quand
on relève le plafond. Exprimée en pourcentage, sa taille croît avec le plafond, pour rien.

### Ce que ça coûte, chiffré

*Calcul, non mesuré* — à partir des coûts mesurés par `make bench-ram-tier` (120 octets en
liste, 31 dans l'étage zstd, 70 sans compression) et d'un stock assez grand pour remplir le
plafond jusqu'à 90 % :

| Plafond 42 Go, à 90 % | Liste chaude | Étage (zstd, 31 o) | Stock tenu avant le disque |
|---|---|---|---|
| Aujourd'hui (plancher 25 %) | 10,5 Go ≈ 87 M poss. | 27,3 Go ≈ 880 M | **≈ 970 M** |
| Tampon de 2 M poss. par pool | 0,48 Go ≈ 4 M | 37,3 Go ≈ 1 200 M | **≈ 1 200 M (+25 %)** |

Sans compression (étage à 70 octets), le gain tombe à environ +12 % (≈ 480 M → ≈ 540 M).

Le chiffre qui compte le plus aujourd'hui n'est d'ailleurs pas la capacité. Le stock de
production (≈ 262 M possibilités) **tient en RAM dans les deux cas** sous 42 Go. Mais
aujourd'hui il y occupe ≈ 16 Go (10,5 Go de liste pour 87 M possibilités, 5,4 Go d'étage
pour le reste), contre ≈ 8,5 Go avec un tampon de 2 M par pool : **≈ 7 Go de RSS rendus**,
pour un service identique.

**Le tampon actuel est démesuré au regard de son rôle.** 87 M possibilités, c'est de l'ordre
d'une vingtaine de minutes de demande pour 80 forks pruner (ordre de grandeur tiré du banc
`INST_ADD_BATCH`, ≈ 900 possibilités/s par fork — à confirmer en production, voir
*Mesures préalables*). Le rechargement d'un bloc, lui, se compte en microsecondes : le codec
recharge 3,1 M possibilités/s, un facteur 60 au-dessus du tick du débordement
([etage_ram_compresse.md](etage_ram_compresse.md#mesures)).

**Aux petits plafonds, rien ne change.** À 1 Go, le plancher de 25 % vaut ≈ 2 M
possibilités, soit à peu près le tampon proposé. Le point de bascule se situe vers 1 à 2 Go ;
en dessous, c'est la borne de sécurité (plus bas) qui décide, et elle reproduit le
comportement actuel. La mesure réelle de l'étage (30 M possibilités sous 4 200 Mo,
[utilisation.md](../utilisation.md#étage-ram-en-blocs---stock-hot-floor---stock-hot-reload))
s'y stabilise avec 1 050 Mo de liste chaude, exactement ses 25 % : c'est déjà un tampon de
≈ 8 M possibilités.

### La complexité qui en découle

Le partage entre pools de la PR #350 existe **parce que** le tampon est gros. Un pool que
personne ne lit (le vérifié, quand seuls des pruners tournent) tenait la moitié du plancher
en liste chaînée, la forme la plus chère : à 42 Go, 5 Go immobilisés. D'où la mesure de la
demande servie et insatisfaite, les parts en ‰, leur minimum, et la borne de part dans
l'éviction pour que la compression ne reprenne pas ce que le rechargement vient de remonter.
Même avec tout cela, le pool inactif garde 10 % du plancher, soit ≈ 1 Go à 42 Go.

Avec un tampon de l'ordre du million de possibilités, **chaque pool peut recevoir le sien
entier** : un pool inactif immobilise ≈ 120 à 240 Mo, sans conséquence. Le couplage entre
pools disparaît, et avec lui la raison d'être de la mesure de demande.

## Proposition

### Deux seuils en nombre de possibilités, par pool

- **`H` — taille haute du tampon**, en possibilités, **par pool**. Au-dessus, la tête froide
  de la liste de ce pool part dans l'étage, jusqu'à revenir à `H`.
- **`L` — taille basse**, en possibilités, par pool. En dessous, l'étage **de ce pool**
  recharge jusqu'au milieu `(L + H) / 2` ; si l'étage de ce pool est vide, c'est son disque
  qui recharge (règle déjà en place depuis la PR #350 : le disque ne passe jamais
  par-dessus l'étage du même pool).

Le tampon oscille donc en permanence entre `L` et `H` : il est « toujours presque plein »,
et tout le reste de la RAM sous le plafond revient à l'étage.

Ordre de grandeur des défauts, **à confirmer par les mesures préalables** : `H` = 1 M
(≈ 120 Mo par pool), `L` = 250 k. À 100 000 `GET`/s, `L` couvre 2,5 s de demande, soit 25
ticks du débordement ; `H − L` vaut plusieurs centaines de blocs, loin de l'aller-retour
liste ↔ étage.

Le disque garde ses seuils actuels, qui eux protègent le plafond : éviction au-dessus de
90 % de l'occupation totale, jusqu'à 75 %.

### Ce qui reste en octets, et pourquoi ce n'est pas une entorse à l'invariant

AGENTS.md pose que **rien ne décide sur un nombre de possibilités** pour le plafond : le
critère est toujours `datamanager_resident_bytes` contre `datamanager_ram_limit_bytes`. Cet
invariant ne bouge pas. `H` et `L` ne sont **pas** un plafond mémoire, mais un réglage de
latence. Ils se comparent au **compte** de la liste de chaque pool (`file_size` /
`file_checked_size` sommés sur les files), jamais convertis en octets par un tarif moyen.

La protection mémoire, elle, reste en octets et prend deux formes :

- **Borne de sécurité pour les petits plafonds.** Un tampon de 1 M possibilités par pool
  pèse 240 Mo, plus que les plafonds de 50 Mo qu'on utilise en test réel. La liste d'un pool
  est donc aussi comprimée dès qu'elle dépasse **12,5 % du plafond en octets**, soit 25 % pour
  les deux pools : c'est l'ancien plancher, qui ne joue plus qu'en garde-fou. La condition de
  compression est un OU : `compte > H` **ou** `octets > 12,5 % du plafond`.
- **Le disque** à 90 %/75 %, inchangé.

### Le rechargement doit suivre la demande

C'est la contrainte que le gros tampon masquait. Le fil du débordement tourne toutes les
100 ms avec un budget de 4 096 possibilités (`STOCK_SPILL_BLOCK_PACKETS`), soit ≈ 41 000
possibilités/s au plus. Avec 80 forks pruner à ≈ 900/s, la demande (≈ 72 000/s) le dépasse
déjà. Aujourd'hui, 87 M possibilités de liste en amortissent l'écart pendant des minutes.
Avec un tampon de 250 k, la liste se viderait en quelques secondes.

Deux réponses, dans cet ordre :

1. **Un budget de rechargement proportionnel au manque** : recharger en un pas de quoi
   ramener le pool au milieu `(L + H) / 2`, dans une limite haute (de l'ordre de
   `STOCK_TIER_PROACTIVE_FACTOR` × 4 096, comme la compression proactive). Le codec suit
   à 3,1 M/s ; seule la durée pendant laquelle le pas tient `g_tier_mutex` est à surveiller.
2. **Si la mesure le montre nécessaire** : un rechargement **à la demande**, où un `GET` qui
   voit sa liste passer sous `L` réveille le fil du débordement au lieu d'attendre la fin de
   ses 100 ms. `core/datamanager.c` ne dépend pas de `core/stock_spill.c`, donc ce réveil est
   un crochet injecté, sur le modèle de `datamanager_set_ram_relief_hook`.

### Ce qui disparaît

- Le partage des seuils entre pools : `stock_spill_pool_shares`, `g_pool_share`,
  `pool_share_of`, `pool_most_over_share`, `STOCK_SPILL_POOL_SHARE_MIN_PERMILLE`. La
  compression prend simplement la file la plus pleine du pool qui dépasse `H`.
- `--stock-hot-floor` / `--stock-hot-reload` en pourcentage, remplacés par les deux options
  en nombre (voir *Arbitrages*).
- `datamanager_pool_demand_last_1m` et sa comptabilité (servi + insatisfait) n'ont plus de
  décideur. On peut les garder comme télémétrie (`statistic`, `GET /api/v1/stats`) ou les
  retirer ; voir *Points ouverts*.

## Invariants à tenir

Ce sont ceux de l'étage, inchangés — la proposition ne touche qu'aux seuils :

- **L'ordre de pile de bout en bout** : liste → sommet de l'étage → bas de l'étage vers le
  disque, et retour dans l'ordre inverse ; le disque ne recharge jamais par-dessus l'étage
  **du même pool**.
- **Le rechargement est piloté par la liste de chaque pool**, jamais par l'occupation totale
  ni par la somme des deux listes (la leçon de la PR #350).
- **Pas de rechargement pendant une expansion**, pas de rechargement au-dessus du seuil
  haut de 90 %.
- **L'étage compte dans l'occupation** (`datamanager_resident_bytes`).
- **Le plafond reste en octets** ; `H` et `L` ne sont jamais convertis en octets par un
  tarif moyen.
- **Le rechargement s'arrête au milieu de `L` et `H`**, sans quoi la compression repartirait
  aussitôt.

## Conséquences à accepter ou à traiter

- **Les commandes console qui ne voient que la liste perdent encore en portée** :
  `checkOrigin`, `removeNoNext`, `sortAsc` / `sortAscFiles`, l'histogramme
  `GET /api/v1/stock-distribution`. À 42 Go, elles ne voient déjà qu'environ 9 % du stock
  (87 M possibilités sur ≈ 970 M) ; avec le tampon, ce serait moins de 0,5 %. C'est un
  problème qui existe déjà et que la proposition rend évident. Il se traite à part : voir
  [checkorigin_stock_deborde.md](checkorigin_stock_deborde.md) pour `checkOrigin`, et le
  même raisonnement vaut pour les trois autres. Il faut au minimum que leur sortie annonce
  la part du stock qu'elles n'ont pas lue.
- **Chaque possibilité qui dure passe par l'étage.** Aujourd'hui, le quart du plafond
  qu'occupe la liste en est dispensé. Comme les `GET` servent le haut de la pile (le plus
  récent, dans la liste), l'essentiel du service reste sans décompression. Le coût
  supplémentaire est un surcroît de compression ; à 330 000/s de compression proactive,
  il n'est à surveiller qu'en rafale.
- **L'expansion produit en rafale.** Ses enfants arrivent dans la liste plus vite que la
  compression ne peut les ranger : la liste dépassera `H` pendant une passe. Ce n'est pas
  dangereux, puisque le seuil de 90 % et le disque rattrapent, mais le débit d'une passe est
  à mesurer avant/après (voir *Mesures préalables*). L'expansion lit déjà l'étage et le
  disque comme source (`stock_spill_expansion_begin/take/end`) : une liste plus petite ne
  change pas sa logique, seulement la part qu'elle trouve en liste.
- **Le pool inactif garde son tampon plein** (`H` possibilités, ≈ 120 Mo). C'est voulu : une
  bascule prunage → recherche repart d'une liste pleine, ce que la PR #350 obtenait par son
  minimum de 10 %.

## Mesures préalables

Avant d'écrire la PR 1, deux chiffres de production, sans nouveau code :

1. **Débit de `GET` par pool**, en pointe et en régime établi : les compteurs de
   `core/stock_rate.c` (console `statistic`, `GET /api/v1/stats`) le donnent déjà à la
   minute. C'est ce qui fixe `L` et le budget de rechargement.
2. **Temps de tenue de `g_tier_mutex`** pour un rechargement de 32 768 possibilités
   (8 × 4 096) : c'est ce qui borne le budget par pas. Ce temps s'estime à partir des 3,1 M/s
   du banc, mais c'est à confirmer sur le serveur, sous charge de connexions.

Et une mesure de non-régression à refaire après la PR 1, sur le stock de production :
durée d'une passe d'expansion et RSS stabilisé sous `--stock-max-ram 42000`, avant/après.

## Arbitrages

- **Par pool, pas total.** Un tampon total partagé réintroduirait exactement le couplage
  que la PR #350 a dû arbitrer par la demande. Par pool, les deux décisions sont
  indépendantes, pour un coût de `H` possibilités sur le pool inactif.
- **En nombre de possibilités, pas en secondes de demande.** Un tampon en secondes
  (`H = débit_GET × T`) collerait mieux au rôle, mais réintroduit une mesure d'horloge et le
  démarrage à froid d'une mesure qui se nourrit de ce qu'elle règle (le travers qui a imposé
  de compter la demande insatisfaite dans la PR #350). Le nombre est fixe, testable, et le
  budget de rechargement proportionnel au manque absorbe les pics. À reconsidérer si les
  mesures montrent une demande qui varie de plusieurs ordres de grandeur.
- **De nouvelles options, pas un changement d'unité des anciennes.** Un fichier de
  configuration existant contenant `stock_hot_floor=25` voudrait dire 25 possibilités si
  l'unité changeait en silence. Proposition de noms : `--stock-hot-max <n>` (`H`) et
  `--stock-hot-min <n>` (`L`), avec les clés `stock_hot_max` / `stock_hot_min` dans
  `--config-file`. Les anciennes options et clés sont **refusées bruyamment**, avec un
  message qui nomme leurs remplaçantes, plutôt qu'ignorées.
- **`L < H`, et `H − L` d'au moins plusieurs blocs**, sinon un couple incohérent est
  journalisé et remplacé par les défauts, comme aujourd'hui.

## Découpage en PR

1. **Seuils en nombre, par pool.** `--stock-hot-max` / `--stock-hot-min` (entrée dans
   `cli_topics[]`, `server_config`), compression au-dessus de `H` ou de la borne en octets,
   rechargement sous `L` jusqu'au milieu, budget proportionnel au manque. Suppression du
   partage entre pools. Tests (tous en contre-épreuve) :
   - le tampon d'un pool ne dépend pas du plafond : même compte en liste sous 1 Go et
     sous 40 Go ;
   - un pool inactif garde `H` sans affamer l'autre ;
   - une demande de plus de 41 000 possibilités par pas est rechargée en un pas (échoue avec
     le budget fixe actuel) ;
   - la borne en octets s'applique sous un petit plafond (échoue si la condition n'est que
     sur le compte) ;
   - un ancien fichier de configuration avec `stock_hot_floor` est refusé, pas relu en
     nombre.
   Docs : `utilisation.md`, `etage_ram_compresse.md`, `README.md`, `AGENTS.md`.
2. **Rechargement à la demande** (crochet de réveil depuis le `GET`), **seulement** si la
   mesure après la PR 1 montre des `GET` à vide avec un étage non vide.
3. **Facultative : retirer le chemin disque sans étage.** L'étage est actif dès qu'un
   plafond est posé ; le chemin sans étage (`g_pool_reloading[]`, hystérésis 25 %/75 %,
   `STOCK_SPILL_RELOAD_PERCENT`) ne tourne plus qu'en test, via
   `stock_spill_set_tier_enabled_for_tests(0)`. Le supprimer impose de porter les tests
   historiques du disque sur le chemin avec étage.

## Alternatives non retenues

- **Garder les pourcentages et baisser les défauts** (par exemple 2 %/1 %). Ça règle 42 Go,
  mais pas 4 Go (où 2 % ne tient plus que ≈ 700 k possibilités) ni 400 Go. Le défaut de fond
  reste : une grandeur de latence exprimée en proportion de la mémoire.
- **Garder un tampon total et le partage par la demande de la PR #350, en nombre.** Ça
  conserve toute la mécanique de partage alors que la petite taille du tampon la rend
  inutile.

## Points ouverts

- **Compresser sans plafond.** Un serveur lancé sans `--stock-max-ram` garde tout son stock
  en liste chaînée (`stock_spill_step` sort dès que le plafond vaut 0) : c'est le cas des
  ~30 Go de RSS. Avec un tampon en nombre, la compression au-delà de `H` n'a plus besoin
  d'un plafond pour se définir. Mais l'étage (sauvegarde, restauration, expansion) serait
  alors actif pour tous les serveurs, ce qui dépasse le périmètre de ce document.
- **Garder ou retirer la mesure de demande par pool** (`datamanager_pool_demand_last_1m`)
  une fois qu'elle ne décide plus rien : elle garde une valeur de télémétrie, en particulier
  sa part insatisfaite (des `GET` revenus vides).
- **Les défauts de `H` et `L`**, à fixer sur les mesures préalables.

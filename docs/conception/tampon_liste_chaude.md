# Liste chaude du stock : un tampon en nombre de possibilités, pas en pourcentage du plafond

**Statut : en cours d'implémentation — PR 1/3 et 2/3 livrées** (seuils en nombre par
pool, `--stock-hot-max`/`--stock-hot-min`, partage entre pools retiré ; rechargement à la
demande). La PR 3 reste une proposition, et les *Mesures préalables* n'ont pas encore été
faites : les défauts de `H`
et `L` sont ceux proposés ici, à confirmer. Le comportement actuel est décrit dans
[Utilisation](../utilisation.md#étage-ram-en-blocs---stock-hot-max---stock-hot-min) ; la
section *Le constat* ci-dessous décrit l'état d'AVANT la PR 1. L'étage RAM lui-même, ses
mesures et ses invariants, sont dans
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
[utilisation.md](../utilisation.md#étage-ram-en-blocs---stock-hot-max---stock-hot-min))
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

## Arbitrages tranchés à l'implémentation (PR 1)

- **La borne en octets a un pendant pour le rechargement** : 5 % du plafond par pool (la
  moitié des 10 % de l'ancien `--stock-hot-reload`). Sous un petit plafond, le compte de la
  liste reste toujours sous `L` : sans ce pendant, le rechargement repartirait à chaque
  tick jusqu'à la borne, que la compression reprendrait aussitôt. D'où un ET à l'entrée
  (`compte < L` et `octets < 5 %`) et un OU à l'arrêt (milieu de `L` et `H`, ou de 5 % et
  12,5 %) — le symétrique du OU de la compression. Constantes
  `STOCK_TIER_HOT_GUARD_PERMILLE` (125) et `STOCK_TIER_HOT_GUARD_RELOAD_PERMILLE` (50),
  `core/stock_spill.h`.
- **« Plusieurs blocs » d'écart = `STOCK_TIER_HOT_GAP_MIN`, 4 × 4 096 possibilités.** Un
  couple plus serré est journalisé et remplacé par les défauts. Les tests, qui manipulent
  quelques dizaines de possibilités, posent leur tampon par
  `stock_spill_set_hot_buffer_for_tests`, sans ce contrôle.
- **Budget de rechargement** : le manque (jusqu'au milieu), borné à
  `STOCK_TIER_PROACTIVE_FACTOR` × le budget du pas, et traduit en possibilités au tarif
  observé de la liste pour la borne en octets ; le rechargement de l'étage revérifie
  l'arrêt à chaque bloc. Par pool : les deux pools ont chacun ce budget.
- **Les anciennes options sont refusées au démarrage** (`main()` sort en échec et nomme
  `--stock-hot-max`/`--stock-hot-min`). Les anciennes clés du fichier suivent la règle du
  chargement tolérant de `--config-file` : la ligne est refusée avec une erreur qui nomme
  les remplaçantes (`SERVER_CONFIG_LINE_OBSOLETE_KEY`), le reste du fichier s'applique.
- **Le chemin disque SANS étage** (tests seulement) garde son hystérésis 25 %/75 % par
  pool, la moitié chacun quand les deux ont du stock — le partage d'avant la mesure de
  demande. La PR 3 le supprimera.
- **La mesure de demande par pool est gardée**, sans décider de rien
  (`datamanager_pool_demand_last_1m`, point ouvert ci-dessous) : c'est sa part
  insatisfaite qui dira s'il faut la PR 2. Elle n'est pas encore exposée.

## Ce qu'a montré le premier essai (PR 2)

Premier essai avec des pruners, `--stock-hot-max 2000000 --stock-hot-min 200000` : la liste
non vérifiée tombait à 0 et mettait un moment à se recharger — la condition posée pour la
PR 2 (« des `GET` à vide avec un étage non vide »). Deux causes, traitées séparément :

- **Un défaut de la PR 1**, corrigé avec elle : un pas du débordement faisait la
  compression OU le rechargement. En prunage, les retours des pruners font dépasser son
  maximum au pool vérifié presque à chaque tick ; sa compression terminait le pas, et le
  pool non vérifié n'était rechargé qu'aux rares ticks sans retour. Compression et
  rechargement portent sur des pools différents et se suivent désormais dans le même pas
  (`tier_reload_is_not_preempted_by_the_other_pools_compression`).
- **La latence du tick**, traitée par la PR 2 : un `GET` qui fait passer la liste de son
  pool sous son seuil réveille le fil du débordement (`stock_spill_note_demand`, injecté
  par `datamanager_set_stock_demand_hook`), au plus un pas toutes les 10 ms
  (`STOCK_SPILL_WAKE_MIN_MS`). Le réveil ne prend qu'un petit mutex, jamais l'étage ni le
  disque : un `GET` n'attend jamais une sauvegarde.

Restent des cas où le rechargement est **bloqué**, pas en retard : pendant une sauvegarde
(maintenance), une expansion, ou une éviction vers le disque (occupation entre 90 % et
75 %). Plutôt que de les lever à l'aveugle, chaque famine — liste vide devant un stock en
étage ou sur disque — est désormais journalisée avec ce qui la bloquait et sa durée, et
comptée dans `stockMemory`. C'est la mesure qui dira s'il faut aller plus loin.

Elle l'a dit pour l'éviction : en prunage, une famine de 3 à 48 s à chaque éviction, une
toutes les ~53 min (production, 08-09/10). Le rechargement d'un pool affamé se fait
désormais dans le même pas que l'éviction (cf. [utilisation.md](../utilisation.md#étage-ram-en-blocs---stock-hot-max---stock-hot-min)).

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
- **Exposer ou retirer la mesure de demande par pool** (`datamanager_pool_demand_last_1m`).
  Gardée par la PR 1 mais lue par personne : il faut l'exposer (`statistic`,
  `GET /api/v1/stats`) pour que sa part insatisfaite (des `GET` revenus vides) puisse
  trancher la PR 2, ou la retirer.
- **Les défauts de `H` et `L`**, à fixer sur les mesures préalables.

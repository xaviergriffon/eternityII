# MRV comme moteur unique : le curseur de parcours cesse d'être le référentiel d'état

**Statut : proposition.** Aucune ligne de code écrite. Ce document ne rediscute pas
*si* MRV est meilleur — c'est tranché ailleurs, par mesure : [§4.7](elagage_recherche.md#47-ordre-de-variable-dynamique-mrv--implémenté-et-mesuré-favorable-pr-10-pas-encore-le-défaut-de-déploiement)
pour la recherche (coût de réfutation sur stock réel) et [§4.10](elagage_recherche.md#410-moteur-de-la-preuve-bornée-du-pruner--mrv-plutôt-quordre-fixe--implémenté-opt-in)
pour la preuve bornée du pruner (×3–×4 de fermetures à budget égal). Il traite la
**conséquence** de ces deux verdicts : si MRV devient le moteur unique, `directions[]`
n'est plus un ordre d'exploration, et le champ `alloc` — que tout le programme lit comme
« où en est ce plateau » — ne veut plus rien dire. Ce document mesure l'ampleur du
problème, tranche entre quatre options, et décrit la cible.

Deux drapeaux existent aujourd'hui, tous deux à 0 par défaut et **délibérément
indépendants** : `mrv_enabled` (`ETII_MRV=1`, la recherche) et `pruner_dfs_mrv`
(`ETII_PRUNER_DFS_MRV=1`, la preuve du pruner). La proposition est de les supprimer
tous les deux en même temps que le moteur à ordre fixe.

---

## 1. Question posée

> En ordre fixe, toute recherche a un état comparable : `alloc` désigne à la fois le
> nombre de pièces posées et la prochaine case à traiter. En MRV, on re-canonise le
> paquet sur le parcours fixe avant de l'émettre, mais `alloc` n'est plus représentatif
> de l'état. Que garde-t-on comme référentiel ?

Quatre réponses possibles, examinées en §3 :

| | Option | Verdict |
|---|---|---|
| A | `alloc` devient le **nombre de pièces posées** ; `directions[]` est rétrogradé en simple énumération de cases | **retenue** |
| B | Chercher un parcours fixe **au plus proche de ce que fait MRV en moyenne** | écartée, §3.2 |
| C | Garder un parcours fixe **le plus simple possible**, sans recherche d'optimisation | conséquence de A, pas une option concurrente — §3.3 |
| D | Garder les **deux** moteurs et deux sémantiques d'`alloc` | écartée, §3.4 |

---

## 2. Mesures : ce que vaut réellement le curseur

Toutes les mesures de cette section portent sur le **stock de production réel**
`eternityII.back` (72 741 312 octets / 576 = **126 287 possibilités**), le même que
celui de [§4.10](elagage_recherche.md#410-moteur-de-la-preuve-bornée-du-pruner--mrv-plutôt-quordre-fixe--implémenté-opt-in).
Harnais reproductible en annexe.

### 2.1 L'invariant « position = état » est DÉJÀ faux aujourd'hui, en ordre fixe

Le premier réflexe est de traiter le problème comme une régression introduite par MRV.
Il ne l'est pas : sur le stock réel, produit par le moteur à ordre fixe,

```
alloc ≠ nombre de pièces posées sur 126 256 / 126 287 paquets  (99,98 %)
écart : 1 sur 21,2 %  |  2 sur 22,3 %  |  3 sur 45,0 %  |  4 sur 11,1 %  |  ≥5 sur 0,4 %
alloc  : min 4   max 167   moyenne 43,45
posées : min 9   max 167   moyenne 45,95
```

Cause identifiée sans ambiguïté — les cases remplies **au-delà** du curseur sont
toujours les mêmes :

| indice de parcours | case | fréquence |
|---|---|---|
| 156 | (7,8) | 100,0 % |
| 55 | (2,13) | 78,8 % |
| 40 | (13,13) | 56,4 % |
| 17 | (13,2) | 11,3 % |

Ce sont les **indices officiels**, posés en dur par `first_possibility()` à des cases
que `directions[]` n'atteint que très tard. Autrement dit `alloc` est, déjà et depuis
toujours, une **borne inférieure** du nombre de pièces posées, pas ce nombre.

Ce qui sauve la lecture aujourd'hui, c'est que le décalage est petit et quasi constant
(1 à 4) : classer par `alloc` revient à classer par nombre de pièces. C'est cette
propriété-là, et elle seule, qui va disparaître.

### 2.2 En MRV, ce n'est plus un décalage, c'est une perte de signal

Descente MRV depuis le plateau vide, `bt_canonicalize_packet` appliqué au plateau le
plus profond atteint :

| budget de la descente | pièces posées | premier trou du parcours | `alloc` re-canonisé |
|---|---|---|---|
| 100 k nœuds | 180 | 12 | **12** |
| 1 M nœuds | 182 | 12 | **12** |
| 5 M nœuds | 186 | 8 | **8** |
| 20 M nœuds | 186 | 8 | **8** |

**186 pièces posées → `alloc = 8`.**

Le cas de production est moins extrême, parce que les racines viennent du stock (déjà
préfixées) et qu'un fil délègue au moins tous les `DELEGATE_MIN_INTERVAL_MS` (500 ms,
vérifié tous les `DELEGATE_CHECK_INTERVAL_NODES` = 1 M nœuds). Mesure appariée sur
**25 racines tirées du stock réel** (pas de 4001), un intervalle de délégation chacune
(1 M nœuds de MRV) :

```
posées            moyenne 126,5
alloc re-canonisé moyenne  54,3
écart             moyenne  72,2   (min 1, max 178)
```

L'écart minimal de 1 correspond aux sous-arbres refermés presque tout de suite (morts) ;
l'écart de 178 à une plongée réelle. Le curseur ne dégrade pas la mesure de progression,
il l'annule : deux paquets à `alloc = 32` peuvent porter 33 pièces ou 210.

Point important pour la suite : `alloc` re-canonisé reste **monotone** le long d'une
branche et n'est jamais inférieur à celui de la racine. C'est donc une borne inférieure
*saine*, seulement inexploitable — ce n'est pas un bug de `bt_canonicalize_packet`,
c'est la définition même du curseur qui a cessé d'être informative.

### 2.3 Deux mécanismes de production sont déjà dégradés par là

Ce n'est pas qu'un problème de tableau de bord. Quatre sites de production lisent
`alloc` comme un point de départ dans `directions[]` :

| Site | Boucle | Effet sur un paquet MRV re-canonisé |
|---|---|---|
| [`forward_check_next_k`](../../src/core/possibility.c) (`possibility.c:867`) | `c = alloc … alloc+K` | **fenêtre gaspillée** — voir ci-dessous |
| [`check_possibility`](../../src/core/possibility.c) (`possibility.c:985`) | `p = 0 … alloc` | **validation partielle** — voir ci-dessous |
| [`possibility_all_has_a_next_counted`](../../src/core/possibility.c) (`possibility.c:555`) et [`gpu_pruner.cu:161`](../../src/app/gpu_pruner.cu) | `c = alloc … ETERN_PARTS` | correct (le balayage va jusqu'au bout), mais rebalaye ~40 cases pleines de plus par appel |
| [`search_possiblity_light`](../../src/core/possibility.c) (`possibility.c:709`) | case suivante = `dirx[alloc+1]` | **expansion à contresens** — voir ci-dessous |

**`forward_check_next_k`** inspecte les `FORWARD_CHECK_K` (6) cases du parcours à partir
du curseur, en sautant celles déjà remplies. Son **seul** appelant est
`bt_materialize_pending` — c'est-à-dire exactement le chemin de délégation, donc
exactement les paquets re-canonisés. Nombre de cases réellement étudiées sur les 25
racines ci-dessus :

```
racines où MRV a plongé (écart > 80)  : 2,69 cases utiles sur 6
sous-arbres refermés tôt (écart ≤ 15) : 5,25 cases utiles sur 6
```

Sur le travail délégué — celui qui part chez les autres clients — **plus de la moitié de
la fenêtre de forward-check tombe sur des cases déjà pleines**. Le filtre appliqué au
travail sortant ne filtre presque plus.

**`check_possibility`** ne contrôle la cohérence de couleur que des `alloc` premières
cases du parcours. Sur les paquets mesurés ci-dessus : **32 cases validées sur 180
posées** (18 %). Ses appelants incluent `datamanager.c:1013` et `datamanager.c:1709`,
c'est-à-dire la porte d'entrée du stock serveur : le serveur valide 18 % du plateau
qu'un client MRV lui envoie.

**`search_possiblity_light`** développe les fils d'un paquet en posant une pièce sur
`directions[alloc]` puis en avançant le curseur sur `directions[alloc+1]`. Son appelant
notable est `expand_datas_to_level` (`datamanager.c:3139`), l'expansion anti-famine du
démarrage serveur : elle fabrique donc du stock **le long du parcours fixe** pendant que
les clients l'explorent par contrainte. Sur un paquet MRV re-canonisé, elle branche sur
la case d'indice 8 d'un plateau qui en porte 186 — la case que MRV avait précisément
choisi de ne pas traiter.

---

## 3. Arbitrage

### 3.1 Option A — `alloc` = nombre de pièces posées — RETENUE

`directions[]`/`dirx[]`/`diry[]` survivent, mais uniquement comme **ordre d'énumération
des cases** (déterministe, stable, utilisé pour parcourir une grille et pour départager
des ex æquo). Ils cessent d'être un ordre d'exploration et un référentiel d'état.

Ce qu'il faut convertir, site par site :

| Site | Aujourd'hui | Cible |
|---|---|---|
| `possibility_packet.alloc` | curseur dans `directions[]` | **nombre de cases non vides** |
| `possibility_packet.x` / `.y` | prochaine case à traiter | sans objet (MRV recalcule) — conservés inutilisés ou libérés |
| `normalize_possibility_packet`, `bt_canonicalize_packet` | recalent `alloc` sur le premier trou | disparaissent ; `alloc` se recalcule par comptage |
| `mrv_count_placed` | contournement du problème | devient la définition de `alloc` |
| `forward_check_next_k` | `K` prochaines cases du parcours | `K` premières cases **vides** (ou les `K` plus contraintes) |
| `check_possibility` | `alloc` premières cases du parcours | **toutes** les cases non vides |
| `possibility_all_has_a_next_counted`, `gpu_pruner.cu` | balayage depuis `alloc` | balayage des cases vides |
| `search_possiblity_light` (donc `expand_datas_to_level`) | branche sur `directions[alloc]`, avance à `alloc+1` | branche sur la case choisie par `mrv_choose_cell` |
| `is_origin_of` | préfixe de parcours | inclusion de plateaux (les cases de l'ancêtre sont un sous-ensemble) |
| `expand_datas_to_level` (**niveau cible**), `sortAsc`/`sortDesc`, `stockDistribution`, `max_result`, champ `alloc` de l'API HTTP | niveau de curseur | nombre de pièces posées — **ce que le lecteur croyait déjà lire** |

Quatre de ces conversions sont des corrections, pas des adaptations : `check_possibility`
passe de 18 % à 100 % de couverture, `forward_check_next_k` récupère sa fenêtre entière,
`possibility_all_has_a_next_counted` arrête de rebalayer des cases pleines, et
`expand_datas_to_level` cesse de fabriquer du stock sur des cases que le moteur n'aurait
pas choisies.

`compare_possibility` n'est **pas** concerné : contrairement à ce que son voisinage
laisse croire, il compare la grille entière, pas un préfixe. L'index de hachage du
`datamanager` (`datamanager.c:838`) reste cohérent avec lui.

### 3.2 Option B — un parcours fixe « au plus proche du MRV moyen » — ÉCARTÉE

L'idée serait de conserver le curseur en choisissant une permutation qui suit
approximativement ce que MRV fait. Elle est contradictoire avec la raison même pour
laquelle MRV gagne.

Preuve directe dans les mesures de §2.2 : le premier trou du parcours se stabilise à
l'indice **8 à 12** quel que soit le budget, alors que MRV pose 180 à 186 pièces. MRV ne
« prend pas de l'avance » sur le parcours, il **refuse durablement** d'y toucher : il va
là où la contrainte est, pas là où le parcours est. Un ordre statique ne peut pas suivre
un critère qui dépend de l'état du plateau — s'il le pouvait, il serait l'ordre fixe, et
l'ordre fixe est le moteur mesuré perdant en [§4.7](elagage_recherche.md) (réfutation) et
[§4.10](elagage_recherche.md) (×3–×4 de fermetures en moins à budget égal).

Corollaire opérationnel : la **PR #231** (« nouvel ordre de parcours 256 par score de
risque », −1,3 % de score de risque, bump `VERSION` v13) optimise un composant que la
présente proposition rétrograde en étiquette. À geler. Si elle était fusionnée puis
suivie de cette proposition, le projet dépenserait **deux** ruptures de compatibilité
là où une seule est nécessaire (cf. §5).

### 3.3 Option C — un parcours fixe simple et non optimisé

Ce n'est pas une option concurrente de A : c'en est la conséquence. Une fois `alloc`
défini par comptage, `directions[]` ne pilote plus rien et son contenu n'a plus d'effet
mesurable sur la recherche. Le garder tel quel est le choix par défaut — le remplacer par
un balayage trivial (`(x,y)` en ordre ligne/colonne) serait une simplification cosmétique
qui coûterait une adaptation de tests pour zéro gain. **Ne rien y toucher.**

### 3.4 Option D — garder les deux moteurs — ÉCARTÉE

Maintenir `mrv_enabled` et `pruner_dfs_mrv` implique de maintenir deux sémantiques
d'`alloc` sur le même champ du même paquet, échangées sur le même fil, entre des clients
qui ne savent pas quel moteur a produit ce qu'ils reçoivent. C'est précisément l'état
actuel, et §2.3 en chiffre le prix. La re-canonisation avait été conçue comme la
passerelle entre les deux mondes ([§4.7](elagage_recherche.md), « re-canonisation aux
frontières de délégation ») ; elle a rempli son office — permettre de déployer MRV sans
bump de `VERSION` le temps de le mesurer — et le résultat de la mesure est qu'il faut
maintenant supprimer le monde d'en face.

---

## 4. Ce que la bascule fait perdre, et la compensation proposée

**Ce qu'on perd : l'homogénéité des strates.** Aujourd'hui « niveau 120 » désigne un
ensemble de plateaux qui ont *les mêmes cases* remplies. Demain, deux plateaux à 120
pièces posées auront des formes différentes. C'est un vrai coût pour tout ce qui raisonne
par niveau : `expand_datas_to_level`, `sortDesc`, la lecture de `stockDistribution`.

Il faut être lucide : MRV a **déjà** détruit cette homogénéité, le curseur ne faisait que
la masquer derrière un nombre. La bascule ne crée pas la perte, elle la rend visible.

**La compensation : une seconde coordonnée, déjà calculée gratuitement.**
Le nombre de pièces posées ne dit rien de la difficulté. `mrv_choose_cell` calcule déjà,
à chaque nœud, le **nombre minimal de candidats sur les cases vides** — c'est le score
MRV lui-même. Le conserver dans le paquet donne un couple `(posées, min_candidats)` :

- deux plateaux à 120 pièces avec `min_candidats` 1 et 12 ne valent manifestement pas la
  même chose — la strate redevient discriminante, et pour une bonne raison cette fois ;
- deux plateaux à « curseur 120 » n'étaient comparables que **comptablement** ;
- `stockDistribution` devient un histogramme 2D réellement prédictif, et
  `sortDesc` peut trier sur la difficulté plutôt que sur la profondeur.

Coût sur le fil : **nul**. `struct possibility_packet` fait 576 octets pour 549 octets de
champs utiles — **27 octets de bourrage** (12 après `alloc`, 2 après `b_faceused`, 13 en
queue). Un champ supplémentaire ne change pas la taille du paquet.

Cette seconde coordonnée est un **ajout, pas un prérequis** : elle peut faire l'objet
d'une PR distincte après la bascule.

---

## 5. Compatibilité et migration

Le champ `alloc` change de sens sans changer de type ni de position : un client v12 et un
client v13 se comprendraient sur le fil tout en désynchronisant silencieusement l'état du
plateau. C'est exactement la situation qui a motivé le bump v11 — donc **`VERSION` → 13**,
refus explicite au handshake.

**Migration du stock existant** — le `.back` de production fait 72 Mo / 126 287
possibilités. La conversion est un simple recomptage (`alloc = nombre de cases non
vides`), sans perte : aucun paquet n'est rejeté, aucune possibilité n'est abandonnée,
conformément au principe « aucune possibilité perdue, tout refus a un plan de secours ».
Deux formes possibles, à trancher (§7) : un utilitaire hors ligne, ou une détection à la
restauration (`restore` recompte si le fichier est marqué v12).

**Les fichiers `.back` déjà spillés sur disque** suivent le même chemin : le
`.spillcount` sidecar reste valide, seul le contenu des segments est recompté.

---

## 6. Arbitrages tranchés

1. **`alloc` devient le nombre de pièces posées.** Raison : mesuré, le curseur vaut 54,3
   en moyenne pour 126,5 pièces posées, avec un écart allant jusqu'à 178. Ce n'est pas
   une mesure dégradée, c'est une mesure absente.
2. **Aucune recherche d'un parcours fixe « proche du MRV ».** Raison : §2.2 montre que
   MRV laisse structurellement les indices 8 à 32 du parcours vides ; un ordre statique
   qui suivrait MRV serait un ordre statique, et c'est le moteur mesuré perdant.
3. **`directions[]` reste tel quel**, rétrogradé en énumération. Raison : son contenu
   n'a plus d'effet mesurable ; le modifier coûterait des tests pour zéro gain.
4. **PR #231 gelée.** Raison : −1,3 % sur un composant qui ne pilote plus rien, au prix
   d'un bump `VERSION` qui doit servir à la bascule sémantique.
5. **Un seul bump, v13.** Raison : une seule rupture de compatibilité pour les
   utilisateurs et un seul point de migration du stock.
6. **`mrv_enabled` et `pruner_dfs_mrv` disparaissent**, ils ne deviennent pas « à 1 par
   défaut ». Raison : maintenir le moteur à ordre fixe, c'est maintenir les deux
   sémantiques d'`alloc` que ce document supprime — un interrupteur laissé en place
   serait un chemin de code non testé menant à un paquet mal étiqueté.
7. **`compare_possibility` et l'index de hachage du `datamanager` ne sont pas touchés.**
   Raison : vérifié, ils comparent la grille entière, pas un préfixe de parcours.

---

## 7. Points laissés ouverts

- **Forme du nouveau `forward_check_next_k`** : « les `K` premières cases vides » (simple,
  ordre d'énumération) ou « les `K` cases les plus contraintes » (cohérent avec MRV, mais
  suppose le score déjà calculé au site d'appel — il l'est dans le moteur, pas dans
  `bt_materialize_pending`). À mesurer, avec `make bench-refutation` comme juge.
- **Migration — tranché en PR2** : ni l'un ni l'autre des deux options envisagées ci-dessus.
  Un recomptage systématique et **inconditionnel** à chaque lecture (`import`/`restore`,
  pool stock et pool analysé, rechargement d'un segment de débordement), sans détection de
  version de fichier. Raison : le recomptage (`alloc = possibility_placed_count(packet)`) est
  **idempotent** — appliqué à un paquet déjà correct (écrit par du code v13), il ne change
  rien. Il n'y a donc aucun besoin de savoir si un fichier est « v12 » ou « v13 » : recompter
  systématiquement gère les deux cas uniformément, pour toujours, sans marqueur de format à
  maintenir ni logique de version à faire évoluer au prochain bump. Plus simple que les deux
  options envisagées, et respecte le critère de succès de PR2 (§8) sans distinction de cas.
  Seul le réseau (`INST_ADD`/`INST_GET`) reste hors recomptage : un client v13 produit déjà
  `alloc` correct par construction, recompter à réception serait un travail inutile sur le
  chemin chaud — seuls les fichiers disque écrits par du code antérieur à la bascule ont besoin
  d'être recomptés au chargement.
- **Devenir de `x`/`y`** : conservés inutilisés (paquet inchangé, zéro risque) ou retirés
  (2 octets, sans effet sur la taille à cause du bourrage). Sans enjeu, à trancher au
  moment de l'écriture.
- **Seconde coordonnée `min_candidats`** (§4) : PR distincte, après la bascule.
- **Le pruner GPU** (`gpu_pruner.cu`) suit la même conversion ; à vérifier avec
  `VERIFY=1` (parité CPU/GPU) sur du matériel Jetson, pas sur le Mac.

---

## 8. Découpage en PR proposé

| PR | Contenu | Critère de succès |
|---|---|---|
| 1 | **Sémantique** : `alloc` = comptage. Conversion des 5 sites de `directions[]`, suppression de `normalize_possibility_packet`/`bt_canonicalize_packet`, `VERSION` → 13. Le moteur à ordre fixe est **conservé** et adapté, les drapeaux restent. | `make test` + `make test-integration` verts ; `check_possibility` couvre 100 % des cases posées (test dédié sur un plateau troué) |
| 2 | **Migration du stock** : recomptage du `.back` (forme tranchée en §7), segments spillés inclus. | restauration du `.back` de production 126 287 → 126 287, zéro rejet |
| 3 | **Bascule** : MRV par défaut pour la recherche **et** pour la preuve du pruner ; suppression de `mrv_enabled`, `pruner_dfs_mrv` et du moteur à ordre fixe. | `make bench-refutation` : pas de régression du coût de fermeture vs. `ETII_MRV=1 ETII_PRUNER_DFS_MRV=1` sur `master` |
| 4 | **Tableau de bord** : `stockDistribution`, `sortDesc`, `expand`, API HTTP relus en « pièces posées » ; documentation. | histogramme du stock de production cohérent avec un recomptage indépendant |
| 5 | *(optionnel, après coup)* seconde coordonnée `min_candidats` (§4). | histogramme 2D, `sortDesc` par difficulté |

L'ordre est contraint : PR 1 avant PR 3, sinon la bascule se fait sur une sémantique
d'`alloc` encore ambiguë. PR 2 doit précéder tout redémarrage sur le stock de production.

---

## Annexe — harnais des mesures

### A. §2.1 — écart `alloc` / pièces posées sur un `.back` (autonome)

Se compile contre le vrai `static_variables.c`, donc contre le vrai `directions[]` :

```sh
gcc -O2 -Isrc -o an an.c src/app/static_variables.c && ./an eternityII.back
```

```c
#include <stdio.h>
#include <string.h>
#include "core/possibility.h"
extern uint8_t dirx[ETERN_PARTS], diry[ETERN_PARTS];
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    struct possibility_packet p;
    long n = 0, gap[16] = {0}, beyond[ETERN_PARTS] = {0};
    while (fread(&p, sizeof p, 1, f) == 1) {
        n++;
        int placed = 0;
        for (int x = 0; x < ETERN_SIZE; x++)
            for (int y = 0; y < ETERN_SIZE; y++)
                if (p.grid[x][y] != -2) placed++;
        int fh = ETERN_PARTS;
        for (int i = 0; i < ETERN_PARTS; i++)
            if (p.grid[dirx[i]][diry[i]] == -2) { fh = i; break; }
        int g = placed - fh; if (g < 0) g = 0; if (g > 15) g = 15;
        gap[g]++;
        for (int i = fh + 1; i < ETERN_PARTS; i++)
            if (p.grid[dirx[i]][diry[i]] != -2) beyond[i]++;
    }
    printf("n=%ld\n", n);
    for (int i = 0; i < 16; i++)
        if (gap[i]) printf("  ecart %2d : %6ld (%.1f%%)\n", i, gap[i], 100.0 * gap[i] / n);
    for (int i = 0; i < ETERN_PARTS; i++)
        if (beyond[i] * 20 > n)
            printf("  idx=%3d case=(%d,%d) rempli au-dela du curseur : %.1f%%\n",
                   i, dirx[i], diry[i], 100.0 * beyond[i] / n);
    return 0;
}
```

### B. §2.2 et §2.3 — écart et fenêtre de forward-check sous MRV

Réutilise [`tests/bench/bench_refutation.c`](../../tests/bench/bench_refutation.c), qui
inclut déjà l'unité de compilation complète `core/etii_search.c` (les deux moteurs y sont
`static`) et sait fabriquer un plateau profond par descente MRV. Deux modifications
locales, dans la branche « racines fabriquées » de `main()` :

1. **§2.2, colonne « descente depuis le plateau vide »** — après le
   `best_board_get(&g_search_best_board, &deep, …)` qui suit
   `search_packet_backtracking_mrv(&g_client, &root, g_idParts, seed_nodes, 0, &seeded)`,
   afficher `placed_count(&deep)`, l'indice du premier trou de `directions[]`, et
   `alloc` après `bt_canonicalize_packet` sur une copie. Rejouer avec
   `--seed-nodes 100000 / 1000000 / 5000000 / 20000000`.

2. **§2.2 racines réelles et §2.3** — remplacer la racine vide par une boucle sur les
   paquets d'un `.back` (un sur 4001, 25 racines), chacun passé par
   `normalize_possibility_packet` puis exploré par
   `search_packet_backtracking_mrv(&g_client, &pkt, g_idParts, 1000000, 0, &nn)` avec
   `best_board_init(&g_search_best_board)` **avant chaque racine**. Pour chaque plateau
   profond obtenu, relever `placed_count`, le premier trou, l'`alloc` re-canonisé, et le
   nombre de cases encore vides dans `[alloc, alloc + FORWARD_CHECK_K)` — cette dernière
   colonne est la mesure de §2.3.

Le budget de 1 M nœuds n'est pas arbitraire : c'est
`DELEGATE_CHECK_INTERVAL_NODES`, donc le travail minimal qu'un fil accomplit entre deux
délégations. C'est la quantité de dérive qu'un paquet émis subit réellement.

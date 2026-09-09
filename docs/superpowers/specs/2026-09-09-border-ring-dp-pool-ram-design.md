# `border_ring_dp` — pool de workers unifié pour la RAM et les fragments

**Date** : 2026-09-09
**Statut** : proposé pour approbation
**Auteur** : Xavier Griffon, avec Claude Code
**Dépend de** : [docs/superpowers/specs/2026-09-06-border-mass-parallel-design.md](2026-09-06-border-mass-parallel-design.md) (`--forks`) et le mécanisme `--dp-max-ram-mo` / scission LIFO déjà en place (`tests/tools/border_ring_dp.c`, commit `bcd6ca8`) — cette spec le remplace en partie, elle ne part pas de zéro.

## Contexte

Un run réel sur `data/pieces.csv` (256 pièces), lancé sur une machine à
2×10 cœurs / 48 Go de RAM avec `--dp --forks 10 --dp-max-ram-mo 4096`, n'a
pas terminé en 9h et montre deux problèmes distincts dans son log :

1. **La RAM réelle dépasse largement le budget configuré.** Le log montre un
   niveau passer de 2,32 Go (position 27) à 9,28 Go (position 28) — plus du
   double du seuil de scission observé ailleurs (~4,64 Go) — parce que la
   vérification de taille n'a lieu qu'**après** la construction complète du
   niveau suivant. Deux effets s'additionnent, jamais comptés ensemble
   aujourd'hui : l'ancien niveau reste alloué pendant toute la construction du
   nouveau (`bd_level_free(&cur)` n'a lieu qu'après), et chacun des
   `nb_workers` enfants de `bd_transition_parallel` construit sa propre table
   locale **non dédupliquée** avec celles des autres — leur somme peut
   dépasser sensiblement la taille du niveau final fusionné, et grossit avec
   `nb_workers`. C'est directement ce qui a forcé Xavier à descendre à
   4 Go/10 cœurs actifs sur une machine à 48 Go/20 cœurs : plus on ajoute de
   cœurs, plus l'écart entre RAM configurée et RAM réelle s'aggrave.
2. **Les fragments ne bénéficient d'aucun parallélisme entre eux.**
   `bd_run_opening` traite la pile LIFO **un fragment à la fois** : le
   parallélisme (`bd_transition_parallel`) ne s'applique qu'À L'INTÉRIEUR
   d'un fragment tant que ses effectifs restent au-dessus de
   `bd_fork_min_states` (50 000, une constante fixe indépendante de la RAM et
   du nombre de workers) ; en dessous, un fragment est traité en
   monoprocessus, puis le suivant est dépilé. Le log confirme que c'est le
   vrai goulot de ce run : la séquence de positions 44→59 (petits effectifs,
   quelques centaines à ~2 M d'états) **se répète 3200 fois**, soit 3200+
   fragments indépendants traités un par un sur un seul cœur pendant que les
   9 autres cœurs actifs ne font rien.

## Objectif

Remplacer les deux mécanismes séparés actuels (fork interne à une transition
via `bd_transition_parallel`, traitement séquentiel des fragments via la pile
LIFO locale à `bd_run_opening`) par **un seul coordinateur** qui bascule entre
deux modes mutuellement exclusifs selon la profondeur de la file d'attente, et
qui borne la RAM réelle — pas juste la taille nominale d'un niveau — au budget
`--dp-max-ram-mo` donné.

## Non-objectifs

- Fork imbriqué (un fragment-worker qui re-forke lui-même des sous-workers).
  Discuté et écarté : la comptabilité de cœurs partagée que ça demanderait
  n'est pas justifiée face au gain — les deux modes restent strictement
  exclusifs, jamais superposés.
- Rendre exacte la marge de sécurité du mode 1 (RAM pendant
  `bd_transition_parallel`). Elle dépend du taux de collision de clés entre
  workers, connu seulement une fois le calcul fait — reste un facteur
  heuristique conservateur, comme `bd_shard_target_bytes` aujourd'hui.
  Seule la marge du mode 2 (rechargement d'un fragment déjà sur disque) est
  rendue exacte par cette spec — elle l'est parce que l'information
  nécessaire (taille exacte du fragment sérialisé) est déjà disponible sans
  calcul supplémentaire.
- Threads : fork reste le mécanisme de parallélisme (voir « Pourquoi fork,
  pas des threads » ci-dessous) — pas de nouvelle discussion à rouvrir en
  implémentation.
- Répartition dynamique du budget par slot réellement actif (`budget /
  nb_slots_actifs`, recalculé à chaque changement) — le calcul retenu utilise
  toujours `budget / nb_workers` (le pire cas, tous les slots occupés), plus
  simple et sûr par construction, quitte à sous-utiliser un peu la RAM quand
  moins de slots sont actifs.

## Pourquoi fork, pas des threads

Question posée explicitement pendant le brainstorming ; réponse actée ici
pour ne pas la rouvrir en implémentation :

1. **L'isolation mémoire est une fonctionnalité, pas un coût.** Toute cette
   conception repose sur le fait qu'un job (process) a une RAM qui lui est
   propre et mesurable de l'extérieur. Avec des threads, un job qui dépasse
   son budget partage le même tas que ses voisins — rien ne l'empêche de les
   affamer avant que le coordinateur ait pu réagir.
2. **Isolation de panne.** `exit(1)` sur une allocation impossible ou un
   `fwrite` en échec est la politique d'échec voulue partout dans ce fichier.
   En process séparé, ça ne tue que CE job ; le coordinateur le détecte via
   `waitpid` et arrête proprement le reste. En thread, ça tue tout le run —
   coûteux à perdre sur un calcul de plusieurs heures.
3. **Zéro synchronisation à écrire ou auditer.** La pile de fragments ne vit
   que dans le coordinateur (mono-thread) ; les workers communiquent par
   fichiers + code de sortie, jamais par mémoire partagée — aucun mutex,
   aucune structure lock-free.
4. **Le coût de `fork()` n'est pas le goulot mesuré.** Les fragments de la
   fin du log s'exécutent en une fraction de seconde à quelques secondes ;
   le coût d'un `fork()`/`waitpid()` (de l'ordre de la milliseconde) est
   négligeable face à ça et totalement négligeable face aux 9h du run.
5. **Cohérence avec le reste du fichier** : `bd_transition_parallel` et
   `bd_compact_dir` utilisent déjà fork+fichier+`waitpid`, jamais de threads.

## Architecture

### Deux modes, jamais superposés

- **Mode 1 (« un seul gros travail »)** — actif quand la pile de fragments en
  attente contient strictement moins de `nb_workers` éléments. Un seul
  fragment (ou l'ouverture initiale à la toute première itération) est actif,
  et A LE DROIT d'utiliser `bd_transition_parallel` pour SA transition de
  niveau (comportement quasi inchangé par rapport à aujourd'hui).
- **Mode 2 (« plusieurs fragments »)** — actif quand la pile contient au
  moins `nb_workers` éléments. Le coordinateur lance jusqu'à `nb_workers`
  fragments EN PARALLÈLE, un process par fragment, **chacun strictement
  monoprocessus** (jamais de `bd_transition_parallel` ni de `bd_compact_dir`
  interne tant qu'on est dans ce mode).

Le coordinateur réévalue le mode à chaque fin de job (voir boucle
d'ordonnancement plus bas) — ce n'est jamais une décision figée au départ.

### Comportement uniforme d'un job (mode 1 ou 2)

Qu'il s'agisse du job solo du mode 1 ou d'un fragment du mode 2, un job reçoit
en paramètre : l'état de départ (chargé depuis un fichier fragment, ou l'état
d'ouverture initial pour le tout premier job), sa position de reprise, un
budget RAM effectif, et un indicateur « autorisé à utiliser
`bd_transition_parallel` » (vrai seulement en mode 1).

```
charger le niveau depuis le fragment (ou utiliser l'état d'ouverture initial)
pour position = reprise+1 .. BORDER_RING_LEN-1 :
    construire le niveau suivant :
        - si autorisé et effectifs courants >= bd_fork_min_states : bd_transition_parallel
        - sinon : bd_transition_range séquentiel
    si taille(niveau suivant) dépasse le budget effectif de CE job :
        scinder en K fragments (bd_level_to_shards — compaction SÉQUENTIELLE,
        jamais bd_compact_dir forké, si ce job est en mode 2)
        écrire un fichier résultat : total=0, dossier + nombre de fragments produits
        exit(0)   # ne garde JAMAIS un fragment pour lui-même
finaliser (bd_finalize_range) -> total
écrire un fichier résultat : total=X, 0 fragment
exit(0)
```

Un job ne garde jamais de fragment pour lui-même après une scission — il
repousse TOUS les fragments produits (K, pas K-1) vers la pile partagée du
coordinateur, qui les redistribue au prochain slot libre. C'est le
coordinateur seul qui possède la pile ; un job ne la touche jamais
directement — c'est ce qui rend toute la communication réductible à
« fichier + code de sortie », sans synchronisation partagée.

### Comptabilité RAM

**Mode 2 — marge de rechargement exacte, pas une estimation.** Chaque
fragment canonique (`shard_<d>.bin`) porte déjà un en-tête de 12 octets
(`key_len` int32 + `count` uint64, format `bd_level_write_file` existant).
`bd_level_load_file` recharge toujours ce fragment avec la même règle de
dimensionnement que `bd_level_init` : capacité = plus petite puissance de 2
≥ `count*2+16`. La taille RAM exacte qu'occupera CE fragment une fois
rechargé est donc entièrement déterminée par ces deux nombres, sans jamais
charger le fragment lui-même :

```
capacity = next_pow2(max(16, count*2 + 16))
bytes    = capacity * (key_len + 8) + ceil(capacity / 8)
```

Nouvelle fonction pure `bd_estimate_reload_bytes(int32_t key_len, uint64_t
count)` dans `border_ring_dp.c`, appelée par le coordinateur pour chaque
fragment candidat au lancement (lecture de 12 octets, `fopen`/`fread`/
`fclose`, coût négligeable). Le coordinateur ne lance un fragment que si
`bd_estimate_reload_bytes(...)` + somme des tailles déjà actives sur les
autres slots reste sous `budget / nb_workers` (part FIXE, pas renégociée
selon le nombre de slots réellement occupés à l'instant T — cf.
Non-objectifs) ; sinon il attend qu'un slot se libère, même principe que
`expand_datas_to_level` (AGENTS.md § RAM cap) : on attend, on ne perd jamais
un fragment.

**Mode 1 — marge heuristique conservatrice, documentée comme telle.** Le
seuil de scission effectif réserve de la marge pour (a) la co-résidence
ancien+nouveau niveau pendant toute la durée d'une transition (diviser le
budget par 2 suffit : si chaque niveau est lui-même vérifié contre ce seuil
réduit, ancien ≤ budget/2 et nouveau ≤ budget/2 au pire) et (b) la
non-déduplication entre les `nb_workers` tables locales de
`bd_transition_parallel` — un facteur additionnel conservateur, sa valeur
exacte à ajuster empiriquement pendant l'implémentation (même démarche que
le `/3` actuel de `bd_shard_target_bytes`), documenté comme heuristique et
non comme un calcul exact.

### Ordonnancement

- File d'attente = une **pile LIFO unique**, partagée, vivant uniquement
  dans le coordinateur — même structure que l'actuel `bd_pending_stack`,
  juste consommée par plusieurs poppers au lieu d'un seul. Ordre LIFO
  conservé pour la même raison que documentée aujourd'hui : borner la
  profondeur de pile par le nombre de positions de l'anneau, pas par la
  largeur de l'espace d'états.
- Boucle du coordinateur : `waitpid(-1, ...)` bloquant sur n'importe quel
  enfant terminé → lit son fichier résultat → cumule le total et/ou empile
  les nouveaux fragments qu'il a produits → réévalue le mode actif
  (profondeur de pile ≥ `nb_workers` ou non) → relance un job dans le slot
  libéré si la pile n'est pas vide et que le budget RAM le permet (mode 2),
  ou lance/poursuit le job solo (mode 1) — sinon laisse le slot vacant
  jusqu'à la prochaine fin de job.
- Tout premier job (l'ouverture, avant toute scission) : toujours mode 1,
  rien d'autre à paralléliser à cet instant.

### Échecs et interruption

Identique à l'existant (`bd_abort_workers`, `bm_abort_workers`) : un job qui
échoue (statut non nul, fichier résultat illisible/absent) déclenche l'arrêt
bruyant (`SIGTERM` puis `waitpid`) de tous les jobs frères actifs et un
`exit(1)` — jamais un total partiel affiché comme définitif. `Ctrl-C` reste
géré par le comportement par défaut du groupe de process (aucun
`setpgid`/`setsid`), inchangé.

## Tests

- `bd_estimate_reload_bytes(key_len, count)` : fonction pure, testée avec des
  couples connus, PLUS une vérification croisée directe contre un vrai
  `bd_level_init`+`bd_level_bytes` sur les mêmes paramètres — verrouille que
  la formule ne diverge jamais silencieusement de l'allocation réelle faite
  par `bd_level_load_file`.
- Fonction de choix de mode (profondeur de pile vs `nb_workers` → mode 1 ou
  mode 2) : pure, testée sans fork.
- `border_ring_count_dp_matches_brute_force_when_forked` et les tests
  « sharded to disk » existants (`test_border_ring_dp.c`) : adaptés pour
  vérifier que le total reste identique une fois le pool de workers en place
  — non-régression sur la propriété centrale (le total ne dépend pas du
  nombre de workers ni du mode choisi).
- Nouveau test dédié : une fixture qui force au moins `nb_workers` fragments
  simultanément en attente (seuils de scission abaissés comme les tests
  existants) et vérifie que plusieurs jobs tournent réellement en parallèle
  en mode 2 (ex. compter les process actifs simultanément via un hook
  test-only, ou vérifier que le total est correct alors que le nombre de
  fragments dépasse largement `nb_workers`, exerçant plusieurs vagues de
  dispatch).
- La boucle fork/dispatch du coordinateur elle-même : non testée
  unitairement (comme aujourd'hui, comme `gen_root.c`), vérifiée par smoke
  test manuel : `--forks 1` vs `--forks 4` sur le jeu 16 pièces avec un
  `--dp-max-ram-mo` assez bas pour forcer plusieurs scissions, même total
  dans les deux cas.

## Build

Aucune nouvelle cible Makefile — reste dans `TEST_MODULES` / `border-mass`.

## Documentation à mettre à jour

- `docs/tests_et_ci.md` : la section `--dp` (§ « Scission par pile LIFO »,
  lignes ~380-463) décrit le mécanisme actuel (un seul fragment actif,
  `bd_shard_target_bytes = budget/(workers×3)`) — à remplacer par la
  description du pool à deux modes, la formule exacte de
  `bd_estimate_reload_bytes`, et le nouveau chiffre mesuré (temps/RAM réels)
  une fois le run relancé avec le correctif.
- `docs/superpowers/specs/2026-09-06-masse-bordure-design.md` /
  `2026-09-06-border-mass-parallel-design.md` : ajouter un renvoi vers cette
  spec pour la partie `--dp`.

## Critères de succès

- `make test` reste vert (nouveaux tests inclus), sur les deux tailles de
  puzzle.
- Sur une fixture forçant plusieurs scissions et au moins `nb_workers`
  fragments en attente, le total reste identique à `--forks 1` séquentiel.
- Mesuré sur `data/pieces.csv` (256 pièces) sur la machine cible : la RAM
  réelle observée (RSS cumulée) ne dépasse plus significativement
  `--dp-max-ram-mo` (marge du mode 2 exacte par construction ; marge du
  mode 1 documentée comme heuristique mais mesurée pour rester proche du
  budget). **Le temps d'exécution total n'est pas un critère chiffré de
  cette spec** — Xavier relance lui-même le run réel.

## Risques connus, assumés

- La marge heuristique du mode 1 peut nécessiter un ajustement après mesure
  sur un vrai run — documentée comme telle dès le départ, pas présentée
  comme une garantie exacte (contrairement à la marge du mode 2).
- `budget / nb_workers` (part fixe par slot, non renégociée) peut
  sous-utiliser la RAM disponible quand peu de slots sont actifs (ex. juste
  après le tout premier split, avant que la pile ne se remplisse) — accepté
  pour la simplicité, cf. Non-objectifs.

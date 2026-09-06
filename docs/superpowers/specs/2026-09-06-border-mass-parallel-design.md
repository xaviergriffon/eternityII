# `border_mass` multi-cœur — parcours par forks

**Date** : 2026-09-06
**Statut** : proposé pour approbation
**Auteur** : Xavier Griffon, avec Claude Code
**Dépend de** : [PR #298](https://github.com/xaviergriffon/eternityII/pull/298) (`border_walk.h/.c`, `border_mass.c`) — cette spec suppose ces fichiers déjà en place, mais la PR est encore ouverte (non mergée) au moment de l'écriture. Ce travail vit donc sur une branche empilée par-dessus `border-mass-walker-design`, pas sur `master`.

## Contexte

Le walker de bordure séquentiel (`border_mass`, PR #298) ne termine pas en
15 minutes sur les données réelles (256 pièces). Un spike (jetable, non
committé) a validé une piste : réordonner le parcours pour poser les **4
coins d'abord** — il n'existe que 4 pièces à 2 faces nulles adjacentes dans
les données réelles, donc le facteur de branchement des 4 premières étapes
est minuscule (24 combinaisons au total pour les 4 coins, contre des
dizaines de candidats possibles à une case de bord arbitraire). Vérifié sur
le puzzle 16 pièces : toujours exactement 4 solutions trouvées (la propriété
« N est déjà la masse totale », établie dans PR #298, survit au
réordonnement — argument détaillé plus bas).

Le réordonnement seul aide mais ne suffit pas forcément à rendre le jeu 256
pièces traitable en un temps raisonnable sur un seul cœur. L'objectif de
cette spec : paralléliser sur plusieurs cœurs via `fork()` — le paradigme
déjà utilisé par le reste du projet pour exploiter le parallélisme machine
— afin que l'utilisateur puisse lancer lui-même la résolution réelle sur une
machine avec beaucoup de cœurs.

## Objectif

1. Ajouter l'ordre « coins d'abord » comme alternative à l'ordre séquentiel
   existant dans `border_walk.c`, sans changer le comportement de l'ordre
   existant ni des tests déjà mergés dans PR #298.
2. Ajouter une étape d'expansion en largeur (BFS) qui produit un ensemble
   d'états partiels — la « frontière » — assez nombreux pour répartir le
   travail entre plusieurs process.
3. `border_mass` fork `N` process (option `--forks N`, défaut : nombre de
   cœurs détecté), chacun reprenant le DFS séquentiel depuis sa part de la
   frontière, et le process parent additionne les résultats.
4. L'utilisateur lance lui-même la résolution réelle une fois l'outil livré
   — cette spec ne prescrit ni ne mesure le résultat final sur les données
   256 pièces.

## Non-objectifs

- Répartition dynamique du travail (vol de travail façon serveur/client
  `INST_NEED_WORK`) — la répartition est statique, décidée une fois à
  l'expansion de la frontière. Si un déséquilibre de charge s'avère réel en
  pratique (mesuré, pas supposé), il pourra être traité dans un futur
  sous-projet.
- Réutilisation de `fork_gate.c`/`fork_orchestrator.c` — voir « Pourquoi pas
  l'infrastructure de fork existante » ci-dessous.
- Reprise sur interruption / sauvegarde d'état partiel — un `Ctrl-C` arrête
  tout (comportement par défaut du groupe de process, voir « Interruption »).
- Génération de racines de stock (sous-projet 2, toujours hors scope).

## Pourquoi pas l'infrastructure de fork existante

`fork_gate.c` résout un problème précis : forker un nouveau process de
recherche alors que **d'autres threads du process parent tournent déjà**
(checker, réception IPC, canal de contrôle, console) — sans coordination, un
thread qui détient un verrou stdio/logger au moment du `fork()` le transmet
verrouillé à l'enfant, qui bloque indéfiniment. `border_mass` est un outil
batch **mono-thread** : aucun thread concurrent ne tourne au moment du
`fork()`, donc ce problème ne se pose pas. Réutiliser `fork_gate.c`
importerait de la complexité et un graphe de dépendances (déjà rencontré au
link de `border_mass` dans PR #298 — `TEST_MODULES` tire `datamanager.c` et
tout le reste) pour un problème inexistant ici.

Passage en revue explicite des invariants de fork-safety documentés dans
AGENTS.md, pour justifier lesquels s'appliquent :

| Invariant | S'applique ? | Pourquoi |
|---|---|---|
| Aucun thread du parent ne tourne pendant `fork()` | Trivialement respecté | `border_mass` est mono-thread avant le fork des workers. |
| Ne jamais appeler `fork_gate_release_quiesce` depuis l'enfant | N/A | N'utilise pas `fork_gate.c`. |
| Ne jamais `flockfile(stdout/stderr)` autour d'un `fork()` | N/A | `border_mass` n'appelle jamais `flockfile`. |
| `fflush(NULL)` peut bloquer si un thread console est en `fgetc()` | N/A | Aucun thread console, aucune lecture bloquante sur `stdin`. |
| L'enfant hérite de la chaîne `atexit()` du parent (ncurses…) | Dormant | `border_mass` ne lie pas `logger_ncurses.c` et n'appelle aucune fonction qui enregistre un `atexit` (ni `local_socket.c`, ni la construction ncurses) — sa chaîne `atexit()` est vide au moment du fork. À revérifier si une future modification ajoute un tel appel. |
| L'enfant ne doit jamais supprimer le socket Unix du parent | N/A | `border_mass` n'ouvre aucun socket local. |
| Ne jamais `SA_RESTART` sur `SIGINT` dans un enfant | N/A | `border_mass` n'installe aucun gestionnaire de signal — comportement par défaut du noyau partout, voir « Interruption ». |

Conclusion : le seul invariant réellement en jeu (« aucun thread parent ne
tourne pendant le fork ») est respecté par construction, sans coordination
nécessaire. Un `fork()` + `pipe()` + `waitpid()` nu est suffisant et plus
simple à auditer que de brancher sur `fork_gate.c`.

## Architecture

### `tests/tools/border_walk.h` / `.c` (cœur pur, étendu)

Nouvelles fonctions, additives — **aucune signature existante ne change**
(les tests de PR #298 restent valides tels quels) :

```c
/**
 * @brief Remplit `order` avec les BORDER_RING_LEN cases du pourtour, les 4
 * coins en premier (dans leur ordre `border_ring_order`), puis les cases de
 * bord dans leur ordre `border_ring_order` habituel.
 *
 * Ne change aucune propriété de comptage de `border_walk_count_ordered` :
 * le raisonnement « N est déjà la masse » (aucun voisin posé à la toute
 * première case, donc n'importe lequel des 4 coins peut ouvrir la
 * recherche) tient toujours — les 4 coins sont simplement TOUS posés tôt au
 * lieu qu'un seul le soit et que les 3 autres se déduisent en refermant le
 * cycle. Vérifié empiriquement sur le puzzle 16 pièces (spike jetable,
 * toujours 4 solutions trouvées, identique à l'ordre séquentiel).
 */
void border_corners_first_order(int8_t order[BORDER_RING_LEN][2]);

/**
 * @brief Variante de `border_walk_count` acceptant un ordre de parcours
 * explicite, et un état de départ optionnel (pour reprendre depuis un état
 * partiel produit par `border_walk_expand_frontier`).
 *
 * `border_walk_count` (existant, PR #298) devient un simple appel à cette
 * fonction avec `border_ring_order()` et un état vide — comportement
 * inchangé, aucun test existant à retoucher.
 *
 * @param order       Ordre de parcours des BORDER_RING_LEN cases.
 * @param start_depth Index dans `order` à partir duquel continuer (0 pour un
 *                     parcours complet depuis un plateau vide).
 * @param start_state Si non NULL, état du plateau déjà posé jusqu'à
 *                     `start_depth` (copié — jamais modifié par l'appelant
 *                     après l'appel). Si NULL, part d'un plateau vide.
 */
long long border_walk_count_ordered(map_big_array *map,
                                     struct array_part *all_rotate_parts,
                                     const int8_t order[BORDER_RING_LEN][2],
                                     int start_depth,
                                     const struct possibility_packet *start_state,
                                     border_ring_found_cb on_found, void *ctx);

/**
 * @brief Étend le plateau en largeur (BFS, un niveau à la fois) jusqu'à ce
 * que le nombre d'états partiels atteigne `target_partitions`, ou que
 * `BORDER_RING_LEN` soit atteint (jeu de pièces trop petit pour produire
 * assez de partitions — les états complets rencontrés en chemin sont
 * comptés directement via `on_complete`, jamais renvoyés comme partiels).
 *
 * Analogue en miniature de `expand_datas_to_level` (`core/datamanager.c`) :
 * même idée (peupler un ensemble d'états à répartir), sans stock ni
 * persistance — tout tient en mémoire, le temps de l'appel.
 *
 * @param target_partitions Nombre d'états partiels visés (peut être
 *                           dépassé au dernier niveau expansé : un niveau
 *                           entier est toujours développé, jamais coupé en
 *                           cours de route, pour ne pas biaiser vers les
 *                           premières branches explorées).
 * @param on_partial         Appelé pour chaque état partiel de la frontière
 *                            finale, avec sa profondeur (index dans `order`
 *                            à partir duquel un `border_walk_count_ordered`
 *                            doit reprendre).
 * @param on_complete         Appelé pour chaque anneau complet trouvé
 *                            PENDANT l'expansion (jeu de pièces trop petit
 *                            pour atteindre `target_partitions` sans
 *                            épuiser l'arbre) — ne jamais compter ces
 *                            anneaux une deuxième fois côté appelant.
 * @return                    Nombre d'anneaux comptés via `on_complete`
 *                             pendant l'expansion elle-même (0 dans le cas
 *                             courant où la frontière est atteinte avant
 *                             d'épuiser l'arbre).
 */
typedef void (*border_partial_cb)(const struct possibility_packet *partial_state,
                                   int depth, void *ctx);

long long border_walk_expand_frontier(map_big_array *map,
                                       struct array_part *all_rotate_parts,
                                       const int8_t order[BORDER_RING_LEN][2],
                                       int target_partitions,
                                       border_partial_cb on_partial, void *partial_ctx,
                                       border_ring_found_cb on_complete, void *complete_ctx);
```

### `tests/tools/border_mass.c` (enveloppe CLI, étendue)

Nouvelle option `--forks N` (avant les deux arguments positionnels
existants ; défaut : `sysconf(_SC_NPROCESSORS_ONLN)`, borné à 1 minimum).

Séquence :
1. Charger pièces + indices, vérifier la précondition (inchangé).
2. Construire `map`/`all_rotate_parts` **une seule fois**, avant tout fork —
   même principe que `main.c` pour le process serveur/client réel (« builds
   the shared map before forking », AGENTS.md) : les enfants héritent la
   map en COW, aucune reconstruction redondante.
3. `border_walk_expand_frontier(..., target_partitions = forks * 8, ...)` —
   le facteur 8 donne à chaque worker plusieurs partitions indépendantes
   pour amortir un déséquilibre de charge entre sous-arbres, sans complexité
   d'ordonnancement dynamique. Les anneaux comptés pendant l'expansion
   (`on_complete`, cas des tout petits jeux de pièces) s'ajoutent
   directement au total.
4. Répartir les états de la frontière en `N` tranches contiguës aussi
   égales que possible (`frontier[w::N]` façon round-robin, pas un simple
   découpage en blocs contigus — un déséquilibre systématique par sous-arbre
   voisin serait sinon concentré sur un seul worker).
5. Pour chaque worker `w` (0..N-1) : créer un pipe dédié, `fork()`. Dans
   l'enfant : fermer l'extrémité lecture du pipe, appeler
   `border_walk_count_ordered` sur la tranche assignée (accumulée en une
   seule valeur, pas un `on_found` par anneau — inutile de faire remonter
   chaque anneau individuellement en phase 1), écrire le total en ASCII
   décimal (`dprintf(fd, "%lld\n", total)`) sur l'extrémité écriture,
   `exit(0)`. Dans le parent : fermer l'extrémité écriture, conserver
   l'extrémité lecture.
6. Le parent attend chaque enfant (`waitpid`), lit chaque pipe, additionne.
7. Affiche le total final (même format qu'aujourd'hui : une ligne, la masse
   totale).

### Progression pendant l'exécution

Chaque worker journalise sur `stderr` une ligne PAR PARTITION TERMINÉE
(numéro de partition, total courant) plutôt qu'un point de contrôle
périodique en temps ou en nœuds visités (que l'idée initiale envisageait,
à la `ETII_BENCH_NODES`) — plus simple, et n'exige pas d'ajouter un nouveau
paramètre de callback de progression à `border_walk_count_ordered`, déjà
stable et testé. Compromis assumé : une partition inhabituellement grosse
(le déséquilibre de charge documenté dans les Risques ci-dessous) peut
laisser un worker silencieux un long moment. Si cela s'avère gênant en
pratique, un point de contrôle en nombre de nœuds à l'intérieur même du
DFS resterait la voie la plus simple pour y remédier — non implémenté ici.

### Interruption

Aucun gestionnaire de signal custom. `Ctrl-C` envoie `SIGINT` au groupe de
process du terminal, qui inclut par défaut le parent ET tous les enfants
forkés (aucun `setpgid`/`setsid` n'est appelé) — comportement standard
suffisant : toute la hiérarchie s'arrête d'un coup. Documenté explicitement
pour qu'un futur lecteur ne "corrige" pas cette absence de gestionnaire en
pensant qu'elle est un oubli.

## Tests

Toute la logique testable (ordre, comptage paramétré, expansion de
frontière) vit dans `border_walk.c`, pure, testée dans
`test_border_walk.c` :

- `border_corners_first_order` : mêmes propriétés géométriques que
  `border_ring_order` (mêmes BORDER_RING_LEN cases, chacune une fois), PLUS
  les 4 premières entrées sont des coins.
- `border_walk_count_ordered` appelé avec `border_ring_order()`,
  `start_depth=0`, `start_state=NULL` : résultat identique à
  `border_walk_count` existant, sur les deux fixtures déjà utilisées (0 sans
  pièce de bord, 4 pour l'anneau unique construit à la main) — verrouille
  la non-régression de PR #298.
- `border_walk_count_ordered` appelé avec `border_corners_first_order()` :
  même résultat (4) sur la même fixture — verrouille que le réordonnement
  ne change pas le compte.
- `border_walk_expand_frontier` puis reprise via `border_walk_count_ordered`
  sur chaque état partiel (avec `start_depth`/`start_state`) : la somme des
  comptes de reprise, plus les complétions trouvées pendant l'expansion,
  égale le compte d'un `border_walk_count_ordered` direct sans expansion —
  c'est la propriété centrale que la parallélisation doit préserver.
- Cas dégénéré : `target_partitions` très grand sur la petite fixture (12
  cases) — l'expansion doit s'arrêter à `BORDER_RING_LEN` sans dépasser,
  et toutes les complétions doivent être comptées via `on_complete`, la
  frontière finale restant vide ou petite.

Le fork/pipe/wait de `border_mass.c` n'est pas testé unitairement (comme
`gen_root.c`, aucun test) — vérifié par un smoke test manuel : lancer avec
`--forks 1` et `--forks 4` sur le jeu 16 pièces, confirmer un total
identique (4) dans les deux cas.

## Build

Aucune nouvelle cible Makefile — `border-mass` existe déjà (PR #298),
`border_walk.c` reste dans `TEST_MODULES`.

## Documentation à mettre à jour

- `docs/tests_et_ci.md` : section `border_mass` existante, ajouter `--forks`
  et le principe de parallélisation.
- `tests/README.md` : paragraphe `border_mass` existant, même ajout bref.

## Critères de succès

- `make test` reste vert (nouveaux tests inclus), dans les deux tailles de
  puzzle.
- `--forks 1` et `--forks 4` (ou plus) donnent le même total sur le jeu 16
  pièces réel.
- L'outil compile et s'exécute sans erreur avec `--forks` sur le jeu 256
  pièces — **le temps d'exécution réel sur les données 256 pièces n'est ni
  mesuré ni un critère de cette spec** : Xavier lance lui-même cette
  résolution.

## Risques connus, assumés

- Le déséquilibre de charge entre partitions (facteur 8 arbitraire, pas
  mesuré) peut laisser certains workers finir bien avant d'autres — accepté
  pour cette première version, cf. Non-objectifs.
- `border_walk_expand_frontier` garde toute la frontière en mémoire
  (`possibility_packet`, 576 octets chacun) — même à `target_partitions` =
  quelques milliers, negligeable (quelques Mo). Pas borné explicitement :
  si un jeu de pièces exotique produisait une frontière énorme avant
  d'atteindre la cible (peu probable vu la croissance rapide du facteur de
  branchement hors des coins), ce serait un signal à mesurer, pas à
  anticiper ici.

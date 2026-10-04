/**
 * @file stock_spill.h
 * @brief Débordement sur disque du stock serveur (`--stock-max-ram`,
 *        `core/datamanager.h`).
 *
 * Le plafond RAM (`put_to_pool`, `datamanager.c`) refuse tout ADD au-delà du
 * budget — un mur dur. Ce module y ajoute un recours : la possibilité la plus
 * froide (tête de file, `scroll_fifo`) est écrite sur disque plutôt que
 * refusée, et rechargée si la RAM se libère. Le plafond lui-même reste
 * inchangé, filet de sécurité si l'éviction ne suit pas un pic d'ADD.
 *
 * **Hors périmètre** : le chemin chaud ADD/GET ne change pas — tout se fait
 * dans `spill_thread`, au tick périodique ; un GET sur une file vidée en RAM
 * reçoit K=0 (normal depuis la v7) et le rechargement suit au tick suivant. Le
 * pool analysé n'est jamais concerné.
 *
 * Chaque (pool, file) déborde dans sa propre pile de segments numérotés
 * (`spill_<u|c>_<file>_<seq>.dat`) : l'éviction empile en haut, le rechargement
 * dépile du haut — jamais de compactage ni de réécriture d'un segment plein.
 * Un segment est une suite de TRAMES, chacune un bloc de l'étage RAM
 * (`core/stock_tier.h`) sous sa forme stockée — compressée par zstd sous
 * `make ZSTD=1`. Un bloc de l'étage part sur disque tel quel, sans décodage ;
 * une trame s'écrit et se relit en entier, jamais entamée (`spill_frame_t`,
 * `stock_spill.c`).
 *
 * Le débordement survit à un `backup`/`restore` : recopié DANS le `.back` par
 * une sauvegarde autonome (console, HTTP, arrêt sur solution —
 * `stock_spill_embed_snapshot`), ou cliché par liens physiques à côté pour
 * l'autobackup (`stock_spill_snapshot`/`_restore_snapshot`).
 * `stock_spill_prepare_restore` choisit entre les deux d'après le fichier.
 */
#ifndef eternityII_stock_spill_h
#define eternityII_stock_spill_h

#include <stdio.h>
#include <time.h>

#include "core/datamanager.h"

/// Pool cible — même convention que `want_checked` dans `put_to_pool`
/// (`datamanager.c`) : 0 = non vérifié, 1 = vérifié.
#define STOCK_SPILL_POOL_UNCHECKED 0
#define STOCK_SPILL_POOL_CHECKED 1

/// Possibilités au-delà desquelles un segment n'accepte plus de trame (une
/// trame n'est jamais coupée : un segment en tient au plus autant, sauf une
/// trame seule plus grosse). Compté en possibilités, pas en octets : c'est
/// l'unité de lecture d'une passe d'expansion (un segment entier), qui doit
/// tenir dans sa file de travail quel que soit le taux de compression.
#define STOCK_SPILL_SEGMENT_RECORDS (128 * 1024)

/// Nombre de possibilités déplacées par appel d'éviction/rechargement (un
/// « bloc ») — le thread de débordement en fait un par tick (100 ms).
#define STOCK_SPILL_BLOCK_PACKETS 4096

/// Période du fil du débordement (`spill_thread`, app/etii_server.c) quand
/// rien ne le réveille, et écart minimal entre deux pas même réveillé par la
/// demande (`stock_spill_wait_next_step`).
#define STOCK_SPILL_TICK_MS 100
#define STOCK_SPILL_WAKE_MIN_MS 10

/// Répertoire de débordement par défaut (option CLI `--stock-spill-dir`),
/// même convention que `machine_uid_file_path`/`stock_max_ram_mb` : chemin
/// littéral par défaut, jamais alloué, jamais libéré.
#define STOCK_SPILL_DIR_DEFAULT "./eternityii-spill"

/// Seuils d'hystérésis, en pourcentage du plafond RAM
/// (`datamanager_ram_limit_bytes()`, en OCTETS) — cf. la doc de `stock_spill_step`.
#define STOCK_SPILL_HIGH_PERCENT 90
#define STOCK_SPILL_LOW_PERCENT 75
#define STOCK_SPILL_RELOAD_PERCENT 25

/// Tampon de la liste chaude, en POSSIBILITÉS, PAR POOL (`--stock-hot-max`,
/// `--stock-hot-min`) : au-dessus de `max`, la tête froide de la liste du pool
/// part dans l'étage RAM en blocs ; sous `min`, le sommet de l'étage de ce pool
/// remonte, jusqu'au milieu des deux. Un réglage de LATENCE (servir les GET
/// sans décompression), pas un plafond mémoire : jamais converti en octets
/// (docs/conception/tampon_liste_chaude.md).
#define STOCK_TIER_HOT_MAX_DEFAULT 1000000
#define STOCK_TIER_HOT_MIN_DEFAULT 250000
/// Écart minimal entre les deux : un rechargement dépasse son arrêt d'au plus
/// un bloc d'étage ; plusieurs blocs d'écart laissent la compression hors de
/// portée du rechargement qui vient de finir.
#define STOCK_TIER_HOT_GAP_MIN (4 * STOCK_SPILL_BLOCK_PACKETS)

/// Borne de sécurité de la liste de CHAQUE pool, en ‰ du plafond RAM (en
/// octets) : la liste est aussi comprimée au-delà, quel que soit son compte —
/// un tampon d'un million de possibilités pèse plus que les petits plafonds
/// de test. 125 ‰ par pool, soit les 25 % de l'ancien plancher pour les deux.
#define STOCK_TIER_HOT_GUARD_PERMILLE 125
/// Pendant de la borne pour le rechargement : sous un petit plafond, la liste
/// d'un pool ne recharge que sous ce seuil (en plus d'être sous `min`), et
/// s'arrête au milieu de lui et de la borne.
#define STOCK_TIER_HOT_GUARD_RELOAD_PERMILLE 50

/// Budget de la compression PROACTIVE (liste au-dessus de son tampon, sous le
/// seuil haut) ET du rechargement de la liste d'un pool, en multiple du budget
/// d'un pas : 8 × 4096 possibilités par tick de 100 ms, ~330 000/s — loin sous
/// les 2,7 M/s de zstd -1 et les 3,1 M/s du décodage. Un stock restauré de
/// 300 M possibilités rejoint son tampon en une douzaine de minutes plutôt
/// qu'en une heure et demie ; un rechargement ramène un pool au milieu de son
/// tampon en un pas tant que le manque tient dans ce budget, au lieu des
/// ~41 000 possibilités/s d'un budget fixe de 4096 par tick.
#define STOCK_TIER_PROACTIVE_FACTOR 8

/// Budget du transfert étage → disque d'un pas, en multiple du budget du pas :
/// 16 × 4096 = 65 536 possibilités, ~64 blocs de 64 Kio écrits en un seul
/// `fopen`/`fsync` par segment touché (`TIER_DISK_BATCH_BLOCKS`). Au budget
/// d'un pas (4096), un transfert n'écrivait que 4 ou 5 blocs : un `fsync` par
/// 4096 possibilités, sous `g_tier_mutex`. C'était la seule sortie de l'étage
/// pendant une passe d'expansion sous plafond — dégagement à chaque ADD refusé
/// compris —, et elle bornait la passe. Même ordre de grandeur que le lot
/// disque d'une restauration (`IMPORT_DISK_BATCH_PACKETS`).
#define STOCK_TIER_DISK_FACTOR 16

/// Octets de liste libérés par l'éviction au-delà desquels la mémoire est
/// rendue au système (`malloc_trim`, glibc), au plus une fois par
/// `STOCK_TIER_TRIM_MIN_INTERVAL_SEC`.
#define STOCK_TIER_TRIM_BYTES (512ULL * 1024 * 1024)
#define STOCK_TIER_TRIM_MIN_INTERVAL_SEC 10

/// Décision pure du `malloc_trim` : assez d'octets libérés depuis le dernier,
/// et assez de temps écoulé (`last == 0` : jamais encore).
int stock_spill_should_trim(unsigned long long pending_bytes, time_t now, time_t last,
                            unsigned long long threshold_bytes);

/**
 * @brief Initialise le module de débordement : prépare le répertoire cible
 *        et purge les segments résiduels d'un précédent démarrage.
 *
 * Appelée une seule fois, côté serveur, avant `create_spill_thread` et toute
 * expansion `--expand-level` — `nb_files` doit déjà être la valeur finale de
 * `nb_file_possibility`.
 *
 * Dégradation gracieuse, jamais fatale : si le répertoire ne peut être créé
 * ni utilisé, le module reste désactivé pour tout le process —
 * `stock_spill_step` devient un no-op silencieux, le plafond RAM reste un
 * mur dur sans recours. Erreur journalisée une seule fois.
 *
 * La purge ne supprime QUE les fichiers correspondant exactement au motif
 * `spill_[uc]_<n>_<n>.dat`, jamais un effacement générique du répertoire. Si
 * des segments non vides sont purgés, le nombre perdu est journalisé
 * explicitement — perte de données réelle, jamais silencieuse.
 *
 * @param dir       Répertoire cible (`NULL` ⇒ `STOCK_SPILL_DIR_DEFAULT`).
 * @param nb_files  Nombre de files de stock actives (`nb_file_possibility`).
 */
void stock_spill_configure(const char *dir, int nb_files);

/**
 * @brief Tampon de la liste chaude de chaque pool, en possibilités, qui pilote
 *        l'étage RAM en blocs — cf. `STOCK_TIER_HOT_MAX_DEFAULT` et
 *        `STOCK_TIER_HOT_MIN_DEFAULT`.
 *
 * L'étage lui-même (docs/conception/etage_ram_compresse.md) existe dès que
 * `stock_spill_configure` a tourné, que le répertoire de débordement soit
 * utilisable ou non ; il n'agit que sous un plafond `--stock-max-ram`.
 *
 * @return 0, ou -1 si le couple est incohérent (il faut `1 <= hot_min` et
 *         `hot_max - hot_min >= STOCK_TIER_HOT_GAP_MIN`) : les défauts sont
 *         alors appliqués.
 */
int stock_spill_configure_tier(int hot_max, int hot_min);

/// Possibilités actuellement dans l'étage RAM en blocs, tous pools et files.
unsigned long long stock_spill_tier_packets(void);

/// Octets que l'étage RAM tient (comptés dans le plafond).
unsigned long long stock_spill_tier_bytes(void);

/// Même découpage par pool (`STOCK_SPILL_POOL_UNCHECKED`/`_CHECKED`) : la
/// somme des deux pools vaut `stock_spill_tier_packets`/`_tier_bytes`. Lus
/// sans le verrou de l'étage, comme les totaux (`check`, `stockMemory`,
/// `GET /api/v1/stats` ne doivent pas attendre une sauvegarde).
unsigned long long stock_spill_tier_pool_packets(int is_checked);
unsigned long long stock_spill_tier_pool_bytes(int is_checked);

/// Crochets à brancher par `datamanager_set_ram_tier_hooks` : sauvegarde et
/// restauration voient l'étage comme une partie du stock.
const datamanager_ram_tier_hooks_t *stock_spill_ram_tier_hooks(void);

/// Réservée aux tests : nombre de fils qui compressent les blocs d'un import
/// direct (0 : un de moins que les cœurs, au plus 16).
void stock_spill_set_import_workers_for_tests(int workers);

/// Réservée aux tests : les fils de compression d'un import retiennent leurs
/// blocs jusqu'au premier essai du disque qui ne trouve rien à envoyer, qui
/// les relâche et attend qu'ils soient chaînés — rend déterministe la fenêtre
/// où un bloc est chaîné entre l'essai du disque et la lecture des blocs en vol.
void stock_spill_set_import_hold_until_disk_miss_for_tests(int on);

/**
 * @brief Un pas incrémental d'éviction OU de rechargement (jamais les deux
 *        pour une même liste dans le même appel), selon la position de
 *        l'occupation RAM résidente par rapport à trois seuils, avec
 *        hystérésis :
 *
 * | Seuil | % du plafond RAM | Effet |
 * |---|---|---|
 * | Haut | 90 | Bascule en mode ÉVICTION si pas déjà actif |
 * | Bas | 75 | Sort du mode ÉVICTION si actif ; sort AUSSI du mode RECHARGEMENT si actif |
 * | Rechargement | 25 | Bascule en mode RECHARGEMENT (si un débordement existe) |
 *
 * L'hystérésis évite le battement : sans deux seuils distincts pour entrer
 * puis sortir d'un mode, une occupation oscillant autour d'un seuil unique
 * ferait alterner écriture/lecture à chaque tick. Les deux modes partagent
 * le même seuil de sortie (75 %) mais ont chacun leur propre seuil d'entrée
 * (90 % / 25 %) — le rechargement ne peut pas réutiliser son propre seuil
 * d'entrée comme seuil de sortie, sous peine de s'arrêter après un seul bloc
 * rechargé dès qu'il dépasse ces 25 %. État recalculé à chaque appel depuis
 * l'occupation actuelle, jamais persisté.
 *
 * Ce tableau décrit le chemin SANS étage (tests historiques). Avec l'étage, la
 * liste de chaque pool est tenue à son tampon (`stock_spill_configure_tier`) :
 * comprimée au-delà de `hot_max` possibilités ou de
 * `STOCK_TIER_HOT_GUARD_PERMILLE` du plafond, rechargée — étage du pool, puis
 * son disque — sous `hot_min` (et sous `STOCK_TIER_HOT_GUARD_RELOAD_PERMILLE`),
 * jusqu'au milieu, avec un budget proportionnel au manque (jusqu'à
 * `STOCK_TIER_PROACTIVE_FACTOR` × `max_packets`). Compression et rechargement
 * peuvent se suivre dans un même pas : ils portent alors sur des pools
 * différents, et la compression de l'un ne prive jamais l'autre de son
 * rechargement. Le seuil haut (90 %) ne sert plus qu'à envoyer le bas de
 * l'étage sur disque, jusqu'à 75 %, par lots de `STOCK_TIER_DISK_FACTOR` ×
 * `max_packets` (le retour peut donc dépasser `max_packets`) ; pendant cette
 * éviction, rien ne recharge.
 *
 * No-op silencieux si le module est désactivé, si le plafond RAM est
 * illimité, ou pendant une sauvegarde/restauration en cours (évite qu'une
 * possibilité migre RAM/disque pendant un cliché).
 *
 * Pas de RECHARGEMENT pendant une expansion (`datamanager_is_expansion_active`) :
 * ce qui remonterait n'est pas développé par la passe en cours et repartirait
 * sur disque dès qu'elle remplit le pool. L'éviction reste active.
 *
 * @param max_packets Budget de cet appel.
 * @return Nombre de possibilités effectivement déplacées, 0 si rien à faire.
 */
int stock_spill_step(int max_packets);

/**
 * @brief Le dernier `stock_spill_step` a-t-il laissé du travail en attente ?
 *
 * Vrai quand ce pas a déplacé quelque chose et qu'une liste reste au-dessus de
 * son tampon, ou qu'une éviction reste au-dessus du seuil bas : le fil du
 * débordement enchaîne alors le pas suivant après `STOCK_SPILL_WAKE_MIN_MS`
 * au lieu de `STOCK_SPILL_TICK_MS`. Le budget d'un pas reste borné (le verrou
 * d'une file ou de l'étage n'est jamais tenu plus longtemps), c'est la
 * cadence qui suit l'arrivée. Au tick fixe, la compression plafonnait à
 * 8 × 4096 possibilités par 100 ms (~30 Mo de liste par seconde, mesuré en
 * production à rythme constant : le signe d'un budget saturé), quel que soit
 * le débit des ADD. Faux après un pas qui n'a rien déplacé : le tick normal
 * reprend. Un dégagement (`stock_spill_relieve`) ne le modifie pas.
 */
int stock_spill_step_has_backlog(void);

/**
 * @brief Signale une demande servie dans le pool `is_checked` : si sa liste
 *        est passée sous son seuil de rechargement (`--stock-hot-min`, et sa
 *        borne en octets), réveille le fil du débordement au lieu d'attendre
 *        son tick suivant.
 *
 * Branchée sur les GET par `datamanager_set_stock_demand_hook` (le datamanager
 * ne dépend pas de ce module). Appelée hors de tout verrou de pool ; ne prend
 * ni l'étage ni le disque — un GET ne doit jamais attendre une sauvegarde —
 * seulement un petit mutex de réveil, et seulement quand la liste est sous
 * son seuil et qu'aucun réveil n'est déjà en attente. Sans effet sans plafond
 * ou sans étage.
 */
void stock_spill_note_demand(int is_checked);

/**
 * @brief Attend le pas suivant du fil du débordement : au plus `timeout_ms`,
 *        moins si `stock_spill_note_demand` le réveille, mais jamais moins de
 *        `STOCK_SPILL_WAKE_MIN_MS` — un rechargement bloqué (sauvegarde,
 *        expansion) ne doit pas faire tourner le fil au rythme des GET.
 *
 * @return 1 si un réveil par la demande a mis fin à l'attente, 0 sinon.
 */
int stock_spill_wait_next_step(int timeout_ms);

/// Réveils par la demande depuis le démarrage (`stock_spill_note_demand`).
unsigned long long stock_spill_demand_wakes(void);

/// Famines du pool `is_checked` depuis le démarrage : passages de sa liste à
/// vide alors que ce pool avait du stock dans l'étage ou sur disque. Chacune
/// est journalisée dans `events.log` à l'entrée — avec ce qui bloquait le
/// rechargement (sauvegarde, expansion, éviction vers le disque) — et à la
/// sortie, avec sa durée.
unsigned long long stock_spill_starvations(int is_checked);

/**
 * @brief Même pas incrémental que `stock_spill_step`, mais SANS l'abandon
 *        pendant une fenêtre de maintenance.
 *
 * Réservé à un appelant qui DÉTIENT lui-même cette fenêtre et pilote
 * l'éviction depuis son propre fil — en pratique `import()`
 * (`core/datamanager.c`), branché via `datamanager_set_ram_relief_hook`.
 *
 * `stock_spill_step` refuse de travailler sous maintenance parce qu'une
 * éviction CONCURRENTE ferait migrer une possibilité au milieu d'une capture.
 * Ici il n'y a aucune concurrence : l'appelant est le seul à écrire dans le
 * stock à cet instant, et l'éviction s'intercale entre deux de ses insertions.
 * Sans cette porte, un `restore` sous plafond RAM se bloquerait indéfiniment —
 * `restore_apply` (`ui/command_lines.c`) pose la fenêtre pour TOUTE la
 * séquence, donc le débordement y serait muet et la place ne se libérerait
 * jamais.
 *
 * @param max_packets Plafond de possibilités déplacées en un appel.
 * @return            Nombre effectivement déplacé.
 */
int stock_spill_relieve(int max_packets);

/**
 * @brief Nombre total de possibilités actuellement déportées sur disque,
 *        tous pools et toutes files confondus.
 *
 * Lecture cohérente à l'instant de l'appel, mais un thread de débordement
 * concurrent peut la faire évoluer l'instant d'après.
 */
unsigned long long stock_spill_total_packets(void);

/**
 * @brief Source disque d'une passe d'expansion (`datamanager_set_expansion_disk_source`).
 *
 * `begin` fige, pour chaque pile NON vérifiée, son sommet au début de la passe
 * (la frontière) ; `take` lit le segment du BAS d'une pile (le plus ancien) tant
 * qu'il n'est pas au-dessus de cette frontière, remet chacune de ses
 * possibilités à `sink`, puis le supprime ; `end` lève la frontière. Le segment
 * de frontière est lu en entier : ce qu'il contenait au début de la passe part
 * à développer (`develop` = 1), ce que la passe y a ajouté est rendu tel quel
 * (`develop` = 0) — au plus un segment par file et par passe relu pour rien.
 *
 * Par le bas et sous la frontière : les enfants que la passe évince vont au
 * sommet, au-dessus, et ne sont donc jamais repris par la même passe — même
 * garantie que le drainage du pool RAM. Segment entier seulement : la pile
 * reste d'un seul tenant, seul `first_seq` avance, et l'invariant « tout
 * segment sous le sommet est immuable, à sa taille logique » tient.
 *
 * « Peek puis commit » : le segment n'est supprimé qu'une fois toutes ses
 * possibilités remises à `sink`. Sur échec (lecture, décodage, `sink`), rien
 * n'est supprimé et `take` rend -1 : l'appelant retire de sa file ce que `sink`
 * a déjà reçu.
 *
 * @param max_records Place disponible, en possibilités : un segment plus gros
 *                    n'est pas lu (`DATAMANAGER_DISK_TAKE_NO_ROOM`). 0 sert à
 *                    demander s'il reste quelque chose à consommer.
 * @return Nombre de possibilités remises (> 0), 0 s'il ne reste rien sous la
 *         frontière, `DATAMANAGER_DISK_TAKE_NO_ROOM`, ou -1 sur échec.
 */
void stock_spill_expansion_begin(void);
int stock_spill_expansion_take(datamanager_expansion_sink_fn sink, void *ctx, unsigned long long max_records);
void stock_spill_expansion_end(void);
/// Possibilités sur disque, et cumuls d'éviction/de rechargement depuis le démarrage.
void stock_spill_expansion_stats(datamanager_spill_stats_t *out);

/**
 * @brief Nombre total de fichiers de segment actuellement sur disque, tous
 *        pools et toutes files confondus.
 */
unsigned long long stock_spill_total_segments(void);

/// Possibilités et segments sur disque d'UN pool (`STOCK_SPILL_POOL_UNCHECKED`/
/// `_CHECKED`), toutes files confondues ; 0 si le débordement est désactivé.
unsigned long long stock_spill_pool_packets(int is_checked);
unsigned long long stock_spill_pool_segments(int is_checked);

/**
 * @brief Produit/actualise un cliché durable du débordement, dans le
 *        sous-répertoire `snapshot_subdir` de `--stock-spill-dir`.
 *
 * Précondition (jamais vérifiée ici) : l'appelant doit garantir qu'aucune
 * éviction/rechargement concurrent n'a lieu pendant l'appel — en pratique
 * appelée uniquement depuis `consistent_backup` pendant sa fenêtre de
 * maintenance, qui fait déjà de `stock_spill_step` un no-op.
 *
 * Incrémental et idempotent : chaque segment plein est dupliqué par `link()`
 * (O(1)), comparé par inode à l'entrée existante pour ne relier que les
 * segments nouveaux ou renumérotés (comparer l'inode et pas seulement le
 * nom détecte un segment rechargé puis réévincé sous le même numéro avec un
 * contenu différent). Le segment de queue (partiel, encore mutable) est
 * toujours une copie fraîche, jamais un lien — sinon une éviction
 * ultérieure muterait le cliché déjà publié. Repli sur copie octet si
 * `link()` échoue (EXDEV).
 *
 * Termine par l'écriture atomique (`.tmp` + `rename`) d'un manifeste texte
 * que `stock_spill_restore_snapshot` relit.
 *
 * No-op silencieux si le module est désactivé ou `snapshot_subdir` est
 * `NULL`. Échec de création du sous-répertoire : `log_error`, le cliché est
 * sauté pour cet appel (la sauvegarde RAM appelante reste valide).
 *
 * @return Nombre total de possibilités déportées à l'instant du cliché — ce
 *         que `consistent_backup` écrit dans `<stock_filename>.spillcount`
 *         pour que `restore` détecte une restauration partielle plutôt que
 *         de la tolérer en silence.
 */
unsigned long long stock_spill_snapshot(const char *snapshot_subdir);

/**
 * @brief Reconstruit intégralement l'état de débordement vivant à partir du
 *        cliché `snapshot_subdir`, en remplaçant tout ce qui s'y trouvait.
 *
 * Doit être appelée AVANT `import()`/`restore()` — jamais après : un import
 * qui déborde doit compléter les segments déjà remis en place, jamais les
 * écraser. `stock_spill_configure` doit déjà avoir tourné pour ce process.
 *
 * Manifeste absent/illisible : tolérant, aucune action — un `.back` sans
 * cliché de débordement associé est un cas normal.
 *
 * **Re-séquencement si `--stock-files` a changé** : chaque entrée
 * `(pool, ancienne_file)` est reportée sur la file vivante
 * `ancienne_file %% nb_file_possibility_courant`. Sans collision (cas
 * courant) : pur `link()`, aucun déplacement de données — chaque segment
 * placé est relu trame par trame pour vérifier le compte du manifeste. Avec
 * collision (`--stock-files` réduit, plusieurs anciennes files convergent),
 * ou depuis un cliché à pas fixe d'un binaire antérieur (manifeste v1/v2) :
 * chaque source est relue et réécrite en trames comme une éviction normale.
 *
 * Un segment `.dat` listé par le manifeste mais absent du disque n'est
 * jamais silencieusement ignoré : sans collision, le groupe
 * `(pool, ancienne_file)` entier est invalidé et nettoyé ; avec collision,
 * seule la source en défaut est amputée. Le total renvoyé reflète toujours
 * ce qui a été réellement placé sur disque, jamais ce que promettait le
 * manifeste — ce qui permet à `restore_apply` de détecter l'anomalie via
 * `<stock_filename>.spillcount`.
 *
 * @return Nombre total de possibilités effectivement remises en place — à
 *         comparer par l'appelant avec `<stock_filename>.spillcount`.
 */
unsigned long long stock_spill_restore_snapshot(const char *snapshot_subdir);

/**
 * @brief Recopie le cliché `snapshot_subdir` dans `out` (enregistrements de
 *        `.back` compacté), puis supprime ce cliché — succès ou échec.
 *
 * `consistent_backup_spill_embed_fn` de `consistent_backup_self_contained`,
 * appelée APRÈS la libération des files : rien ici ne tient de verrou de
 * pool. Le `checked` de chaque possibilité est celui de sa pile d'origine.
 * Lecture trame par trame, jamais un segment entier décodé d'un coup : le
 * `.back` reste au format compact, indépendant du codec des segments.
 *
 * @param out_written Sur retour : possibilités effectivement écrites.
 * @return 0 si tout le manifeste a été recopié (ou module inactif, 0 écrite),
 *         -1 au premier segment manquant/tronqué/illisible ou compte différent
 *         de celui du manifeste.
 */
int stock_spill_embed_snapshot(const char *snapshot_subdir, FILE *out, unsigned long long *out_written);

/// Supprime le cliché `snapshot_subdir` (segments + manifeste + répertoire).
/// Sert à retirer `CONSISTENT_BACKUP_DEFAULT_SNAPSHOT` une fois le `.back` par
/// défaut remplacé par une sauvegarde autonome : il n'appartient plus à aucun
/// fichier, et ses liens physiques retenaient des segments déjà rechargés.
void stock_spill_drop_snapshot(const char *snapshot_subdir);

/// Supprime tout le débordement VIVANT (segments et descripteurs) — prélude
/// à la restauration d'une sauvegarde autonome. No-op si le module est inactif.
void stock_spill_discard_live(void);

/**
 * @brief Prépare le débordement disque à la restauration de `stock_filename`,
 *        AVANT `restore()` — à appeler sous la fenêtre de maintenance.
 *
 * Sauvegarde autonome (`datamanager_backup_is_complete`) : vide le
 * débordement vivant, rien d'autre — le fichier porte tout, et l'import
 * redéborde lui-même ce qui ne tient pas sous le plafond RAM courant.
 * Sinon : remet en place le cliché que nomme `<stock_filename>.spillcount`
 * (`CONSISTENT_BACKUP_DEFAULT_SNAPSHOT` à défaut) et compare le compte
 * récupéré au compte sauvegardé.
 *
 * @return 0, ou -1 si le cliché restauré est INCOMPLET (journalisé) — la
 *         restauration du résident doit continuer, mais être signalée en échec.
 */
int stock_spill_prepare_restore(const char *stock_filename);

#endif

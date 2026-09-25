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
 * Les segments portent la forme compacte à pas FIXE (`spill_record_bytes`,
 * `stock_spill.c`), pas des paquets bruts.
 *
 * Le débordement survit à un `backup`/`restore` (console, HTTP, autobackup,
 * arrêt sur solution) — voir `stock_spill_snapshot`/`_restore_snapshot`.
 */
#ifndef eternityII_stock_spill_h
#define eternityII_stock_spill_h

/// Pool cible — même convention que `want_checked` dans `put_to_pool`
/// (`datamanager.c`) : 0 = non vérifié, 1 = vérifié.
#define STOCK_SPILL_POOL_UNCHECKED 0
#define STOCK_SPILL_POOL_CHECKED 1

/// Taille cible d'un segment plein, en octets (~106 000 possibilités sur le
/// puzzle 256 pièces). Le dernier segment d'une pile est seul autorisé à
/// être plus petit (partiel, en cours de remplissage).
#define STOCK_SPILL_SEGMENT_BYTES (64 * 1024 * 1024)

/// Nombre de possibilités déplacées par appel d'éviction/rechargement (un
/// « bloc ») — le thread de débordement en fait un par tick (100 ms).
#define STOCK_SPILL_BLOCK_PACKETS 4096

/// Répertoire de débordement par défaut (option CLI `--stock-spill-dir`),
/// même convention que `machine_uid_file_path`/`stock_max_ram_mb` : chemin
/// littéral par défaut, jamais alloué, jamais libéré.
#define STOCK_SPILL_DIR_DEFAULT "./eternityii-spill"

/// Seuils d'hystérésis, en pourcentage du plafond RAM
/// (`datamanager_ram_limit_bytes()`, en OCTETS) — cf. la doc de `stock_spill_step`.
#define STOCK_SPILL_HIGH_PERCENT 90
#define STOCK_SPILL_LOW_PERCENT 75
#define STOCK_SPILL_RELOAD_PERCENT 25

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
 * @brief Un pas incrémental d'éviction OU de rechargement (jamais les deux
 *        dans le même appel), selon la position de l'occupation RAM
 *        résidente par rapport à trois seuils, avec hystérésis :
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
 * segment sous le sommet est plein » tient.
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
 * courant) : pur `link()`, aucun déplacement de données. Avec collision
 * (`--stock-files` réduit, plusieurs anciennes files convergent) : chaque
 * source est relue et réempaquetée comme une éviction normale, pour ne
 * jamais violer l'invariant « tout segment sous le sommet est plein ».
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

#endif

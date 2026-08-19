/**
 * @file stock_spill.h
 * @brief Débordement sur disque du stock serveur (PR2 de la série plafond
 *        RAM — voir `--stock-max-ram`, `core/datamanager.h`).
 *
 * PR1 (déjà livré) plafonne le nombre de possibilités RÉSIDENTES en RAM,
 * refusant tout ADD au-delà du budget (`stock_max_ram_packets`,
 * `datamanager.c`) — un mur dur, sans recours. Ce module ajoute un recours :
 * une fois le budget approché, la possibilité la plus FROIDE (la plus
 * ancienne, jamais servie — tête de la file, `scroll_fifo`) est écrite dans
 * un fichier de « segment » sur disque plutôt que d'être refusée, et
 * rechargée plus tard si la RAM se libère et qu'un débordement existe. Le
 * plafond RAM lui-même (`put_to_pool`) est INCHANGÉ par ce module : il reste
 * le filet de sécurité si l'éviction ne suit pas assez vite un pic d'ADD.
 *
 * **Portée exacte de ce module — ce qu'il NE fait PAS** :
 * - Aucune conscience de sauvegarde/restauration : les segments écrits ici
 *   sont des fichiers de travail, jamais liés à `consistent_backup`/`restore`
 *   (`datamanager.c`). Au redémarrage, `stock_spill_configure` PURGE tout
 *   segment résiduel — le débordement ne survit PAS à un redémarrage tant
 *   que ce travail (PR3, cohérence sauvegarde/restauration) n'est pas livré.
 * - Aucun changement du chemin chaud ADD/GET (`put_to_pool`/`scroll_from_pool`,
 *   `datamanager.c`) : tout le travail se fait dans un thread dédié
 *   (`spill_thread`, `app/etii_server.c`), au tick périodique. Un GET qui
 *   tombe sur une file vidée en RAM (tout son contenu déporté) reçoit
 *   simplement K=0 — réponse déjà normale et supportée du protocole depuis
 *   la v7 — le rechargement suit au tick suivant (~100 ms).
 * - Le pool ANALYSÉ n'est jamais concerné (cf. `stock_max_ram_mb`,
 *   `app/static_variables.h`, pour le raisonnement).
 *
 * Format des segments : un flux brut de `struct possibility_packet`,
 * strictement identique au format `.back` (`datamanager.c`, `backup()`) —
 * aucun en-tête, aucun index, le nombre de possibilités se déduit de la
 * taille du fichier. Chaque (pool, file de stock) déborde dans sa propre
 * PILE de segments numérotés (`spill_<u|c>_<file>_<seq>.dat`,
 * `STOCK_SPILL_SEGMENT_BYTES` chacun sauf le dernier, partiel) : l'éviction
 * empile en haut de pile (numéro croissant), le rechargement dépile depuis
 * le haut (numéro décroissant) — jamais de compactage, jamais de réécriture
 * d'un segment déjà plein.
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
/// (`datamanager_ram_limit_packets()`) — cf. la doc de `stock_spill_step`.
#define STOCK_SPILL_HIGH_PERCENT 90
#define STOCK_SPILL_LOW_PERCENT 75
#define STOCK_SPILL_RELOAD_PERCENT 25

/**
 * @brief Initialise le module de débordement : prépare le répertoire cible
 *        et purge les segments résiduels d'un précédent démarrage.
 *
 * Appelée une seule fois, côté SERVEUR uniquement (`runserver`,
 * `app/etii_server.c`), avant `create_spill_thread` et avant toute
 * expansion `--expand-level` — `nb_files` doit déjà être la valeur finale de
 * `nb_file_possibility` (fixée par `datamanager_configure_stock_files` avant
 * tout fork, jamais modifiée ensuite).
 *
 * Dégradation gracieuse, jamais fatale : si le répertoire ne peut être créé
 * ni utilisé (permissions, chemin invalide, disque plein), le module reste
 * DÉSACTIVÉ pour tout le process — `stock_spill_step` devient un no-op
 * silencieux, le plafond RAM (PR1) reste alors un mur dur sans recours,
 * comme avant ce module. Une erreur est journalisée une seule fois, à ce
 * moment-là.
 *
 * La purge ne supprime QUE les fichiers correspondant EXACTEMENT au motif
 * `spill_[uc]_<n>_<n>.dat` — jamais un effacement générique du répertoire,
 * qui peut être fourni tel quel par l'opérateur. Si des segments non vides
 * sont purgés, le nombre de possibilités perdues est journalisé
 * explicitement (`log_error` : c'est une perte de données réelle tant que
 * PR3 n'existe pas, jamais silencieuse).
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
 * L'hystérésis évite le battement : sans deux seuils distincts pour ENTRER
 * puis SORTIR d'un mode, une occupation qui oscille juste autour d'un seuil
 * unique ferait alterner écriture/lecture de segment à chaque tick pour
 * rien. Les DEUX modes partagent le même seuil de SORTIE (75 %, « Bas ») —
 * l'éviction y DESCEND, le rechargement y REMONTE — mais ont chacun leur
 * propre seuil d'ENTRÉE (90 % / 25 %) : le rechargement ne peut donc jamais
 * réutiliser son propre seuil d'entrée (25 %) comme seuil de sortie, sous
 * peine de s'arrêter après un seul bloc rechargé dès qu'il dépasse ces 25 %
 * — souvent le cas dès le premier bloc, `STOCK_SPILL_BLOCK_PACKETS` valant
 * fréquemment déjà plus que 25 % d'un petit plafond. L'état (mode courant)
 * est interne à ce module,
 * recalculé à chaque appel à partir de l'occupation ACTUELLE — jamais
 * persisté, jamais un compteur incrémental maintenu en parallèle de
 * `datamanager_resident_packets()` (qui reste l'unique source de vérité).
 *
 * No-op silencieux (retourne 0) si le module est désactivé
 * (`stock_spill_configure` a échoué), si `datamanager_ram_limit_packets()`
 * vaut 0 (illimité — le débordement n'a de sens que sous un plafond), ou
 * pendant une sauvegarde/restauration en cours (`maintenance`,
 * `datamanager.c` — évite qu'une possibilité migre entre RAM et disque
 * pendant qu'un cliché est en train d'être pris).
 *
 * @param max_packets Budget de CET appel (le thread périodique passe
 *                     `STOCK_SPILL_BLOCK_PACKETS` ; la commande console
 *                     `spill [n]` peut le surcharger pour un pas immédiat).
 * @return             Nombre de possibilités effectivement déplacées (dans
 *                      un sens ou dans l'autre), 0 si rien à faire ce tick.
 */
int stock_spill_step(int max_packets);

/**
 * @brief Nombre total de possibilités actuellement déportées sur disque,
 *        tous pools et toutes files confondus.
 *
 * Lecture protégée par le verrou interne du module — cohérente à l'instant
 * de l'appel, mais un thread de débordement concurrent peut la faire évoluer
 * l'instant d'après (même convention d'estimation que `datas_size()`,
 * `core/datamanager.c`).
 */
unsigned long long stock_spill_total_packets(void);

/**
 * @brief Nombre total de fichiers de segment actuellement sur disque, tous
 *        pools et toutes files confondus.
 */
unsigned long long stock_spill_total_segments(void);

#endif

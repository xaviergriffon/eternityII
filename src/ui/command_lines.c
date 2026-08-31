#include "ui/command_lines.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdarg.h>
#include <signal.h>
#include <time.h>
#include <limits.h>

#include "ui/logger.h"
#include "core/datamanager.h"
#include "core/stock_spill.h"
#include "net/local_socket.h"
#include "core/readdata.h"
#include "ui/command_match.h"
#include "app/app_static_variables.h"
#include "app/control_registry.h"
#include "app/known_clients_registry.h"
#include "net/control_protocol.h"
#include "core/best_board.h"
#include "app/client_config.h"
#include "app/server_config.h"
#include "app/fork_orchestrator.h"
#include "app/etii_server.h"

#define DEF_FILE "./eternityII.back"
#define DEF_ANALYSE_FILE "./eternityII-in_analyse.back"
#define DEF_BEST_BOARD_FILE "./eternityII-best_board.back"
#define DEF_KNOWN_CLIENTS_FILE "./eternityII-known_clients.back"
#define NB_COMMANDS 63
/// Taille du tampon de construction des textes d'aide (aide générale comprise).
#define HELP_BUFFER_SIZE 16384

/**
 * @brief Catégories de commandes, pour l'aide (`help`, `help <catégorie>`).
 */
typedef enum {
    CMD_CAT_GENERAL = 0,
    CMD_CAT_SEARCH,
    CMD_CAT_STOCK,
    CMD_CAT_BACKUP,
    CMD_CAT_DIAG,
    CMD_CAT_CLIENTS,
    CMD_CAT_COUNT
} command_category;

/// Libellés affichés comme titres de section dans l'aide.
static const char *category_labels[CMD_CAT_COUNT] = {
    "Général",
    "Recherche & régulation",
    "Stock & files",
    "Sauvegarde & restauration",
    "Diagnostic & vérification",
    "Pilotage des clients (serveur)"
};

/// Mots-clés acceptés par `help <catégorie>` (comparés sans tenir compte de la casse).
static const char *category_keywords[CMD_CAT_COUNT] = {
    "general",
    "recherche",
    "stock",
    "sauvegarde",
    "diagnostic",
    "clients"
};

/**
 * @brief Définition d'une commande prise en charge
 */
typedef struct
{
    /// nom de l'instruction
    char *command;
    /// fonction à exectuer pour la commande (NULL pour un alias)
    int (*interpreter)(void);
    ///indique si la commande doit être transmise aux threads fils
    int8_t send_to_childs;
    /// catégorie d'aide (command_category)
    uint8_t category;
    /// 1 si la commande n'a d'effet utile que côté serveur
    uint8_t server_only;
    /// syntaxe avec arguments (NULL : la commande n'en prend pas)
    const char *usage;
    /// description en une ligne, affichée dans l'aide générale
    const char *summary;
    /// complément affiché par `help <commande>` (NULL : rien de plus)
    const char *details;
    /// nom canonique si cette entrée est un alias (NULL : entrée canonique)
    const char *alias_of;
} command_description;

int sort_ascending_interpreter(void);
int sort_ascending_files_interpreter(void);
int sort_descending_files_interpreter(void);
int sort_descending_interpreter(void);
int max_stock_by_thread_interpreter(void);
int pruner_batch_interpreter(void);
int pruner_dfs_budget_interpreter(void);
int limit_interpreter(void);
int exit_interpreter(void);
int check_interpreter(void);
int backup_interpreter(void);
int restore_interpreter(void);
int import_interpreter(void);
int loadjson_interpreter(void);
int print_interpreter(void);
int sortdm_interpreter(void);
int split_interpreter(void);
int regroup_interpreter(void);
int checkdatas_interpreter(void);
int check_duplicate_interpreter(void);
int check_origin_interpreter(void);
int checkfiles_interpreter(void);
int printfile_interpreter(void);
int checkfile_interpreter(void);
int checkdirections_interpreter(void);
int rmnonext_interpreter(void);
int expand_interpreter(void);
int printanalysed_interpreter(void);
int restockanalysed_interpreter(void);
int min_interpreter(void);
int help_interpreter(void);
int clear_interpreter(void);
int statistic_interpreter(void);
int pause_interpreter(void);
int resume_interpreter(void);
int clients_interpreter(void);
int clients_stats_interpreter(void);
int clients_cmd_interpreter(void);
int clients_roles_interpreter(void);
int known_clients_interpreter(void);
int clients_work_interpreter(void);
int lease_duration_interpreter(void);
int rebalance_interpreter(void);
int stock_memory_interpreter(void);
int stock_max_ram_interpreter(void);
int spill_interpreter(void);
int config_interpreter(void);
int config_save_interpreter(void);
int start_interpreter(void);
int stop_forks_interpreter(void);
int config_apply_interpreter(void);

/**
 * @brief Commandes prises en charge (entrées canoniques puis alias).
 *
 * Source unique de vérité de l'aide interactive : catégorie, usage, résumé et
 * complément sont lus par `help`/`help <sujet>` et par le rappel d'usage
 * automatique de `do_command_line` (retour CMD_ERR_USAGE). docs/console.md
 * reprend le même contenu — tenir les deux en cohérence.
 */
static command_description commands[NB_COMMANDS] = {
    {"help", help_interpreter, 0, CMD_CAT_GENERAL, 0, "help [commande|catégorie]",
     "affiche l'aide (générale, d'une commande ou d'une catégorie)", NULL, NULL},
    {"exit", exit_interpreter, 0, CMD_CAT_GENERAL, 0, NULL,
     "arrête proprement le programme",
     "En mode client, attend la fin de tous les processus de recherche avant de quitter.", NULL},
    {"clear", clear_interpreter, 0, CMD_CAT_GENERAL, 0, NULL,
     "efface l'écran de la console (raccourci : Ctrl-L)",
     "Aucune autre commande n'efface l'écran : l'affichage ne défile que par les sorties.\n"
     "Le contenu n'est pas perdu : en mode ANSI il part dans le scrollback natif du\n"
     "terminal (molette / Cmd+↑), en mode ncurses il reste accessible via PgUp.", NULL},
    {"config", config_interpreter, 0, CMD_CAT_GENERAL, 0, "config [clé valeur]",
     "affiche (et, hors serveur, prépare) la configuration (nb_forks, serveur, fichier de pièces, ...)",
     "Client/pruner, sans argument : affiche l'état de l'orchestrateur\n"
     "(WAITING_CONFIG/COUNTDOWN/CONFIGURING/RUNNING/...), la configuration\n"
     "EFFECTIVE (celle réellement en vigueur) et la configuration EN PRÉPARATION.\n"
     "N'annule pas le décompte. Client/pruner, avec <clé> <valeur> : écrit dans la\n"
     "configuration en préparation (clés : nb_forks, server_host, parts_file,\n"
     "max_stock_by_thread, limit, pruner_batch, dfs_budget) et ANNULE\n"
     "DÉFINITIVEMENT le décompte d'auto-démarrage — `start` consomme toujours la\n"
     "configuration EFFECTIVE, pas celle en préparation.\n"
     "Serveur, sans argument : affiche la configuration EFFECTIVE du serveur (clés :\n"
     "nb_threads, parts_file, expand_level, expand_max_stock, expand_max_levels,\n"
     "http_port, http_token_file, stock_files, stock_max_ram, stock_spill_dir,\n"
     "rebalance_budget, tcp_timeout, auto_roles, stop_on_solution, headless).\n"
     "Serveur, avec <clé> <valeur> : REFUSÉE — le serveur n'a pas de configuration\n"
     "\"en préparation\" à appliquer à chaud (pas de `configApply` côté serveur) ;\n"
     "éditer le fichier `--config-file` puis redémarrer reste le chemin pour une clé\n"
     "sans commande console dédiée (voir `stockMaxRam`/`spill`/`rebalance`/\n"
     "`leaseDuration`/`clientsRoles` pour celles qui en ont une).", NULL},
    {"configSave", config_save_interpreter, 0, CMD_CAT_GENERAL, 0, NULL,
     "écrit la configuration effective dans le fichier de configuration",
     "Client/pruner : écrit la configuration EFFECTIVE, avec toute valeur EN\n"
     "PRÉPARATION (`config <clé> <valeur>`) superposée par-dessus — c'est ainsi\n"
     "qu'une valeur préparée prend effet au prochain démarrage. Fichier par défaut\n"
     "./eternityii-client.conf (option --config-file <chemin>).\n"
     "Serveur : écrit la configuration EFFECTIVE du serveur telle quelle (pas de\n"
     "configuration \"en préparation\" côté serveur). Fichier par défaut\n"
     "./eternityii-server.conf (même option --config-file <chemin>).\n"
     "Dans les deux cas : écriture atomique (.tmp puis rename, comme « backup »).", NULL},
    {"start", start_interpreter, 0, CMD_CAT_GENERAL, 0, NULL,
     "fork immédiat des process de recherche avec la configuration effective",
     "Sans effet si déjà en RUNNING (erreur explicite). Sinon, démarre immédiatement\n"
     "sans attendre un éventuel décompte d'auto-démarrage (COUNTDOWN) — même chemin\n"
     "de code que ce décompte à échéance. Consomme la configuration EFFECTIVE, pas\n"
     "celle en préparation par `config <clé> <valeur>` (cf. commande `config`).", NULL},
    {"stopForks", stop_forks_interpreter, 0, CMD_CAT_GENERAL, 0, NULL,
     "arrête les process de recherche sans quitter ce process",
     "Erreur si aucun fork n'est en cours d'exécution. SIGINT à chaque fils, puis\n"
     "escalade SIGTERM (+5s) et SIGKILL (+10s) si nécessaire — jamais bloquant plus\n"
     "de ~10s. Le process reste vivant après (console, canal de contrôle, API HTTP\n"
     "restent actifs) : état ramené à WAITING_CONFIG, en attente d'un nouveau `start`.", NULL},
    {"configApply", config_apply_interpreter, 0, CMD_CAT_GENERAL, 0, NULL,
     "applique la configuration en préparation, à chaud si possible",
     "Erreur si aucun fork n'est en cours d'exécution. Si seules des clés à chaud\n"
     "(max_stock_by_thread/limit/pruner_batch/dfs_budget) sont préparées : appliquées immédiatement\n"
     "et diffusées aux fils en cours par IPC, sans interruption. Si nb_forks/server_host/\n"
     "parts_file est préparé (cf. `client_config_diff`) : arrête les fils (comme\n"
     "`stopForks`), reconstruit les tableaux de fils et/ou la map de recherche partagée,\n"
     "puis reforke automatiquement avec la nouvelle configuration.", NULL},

    {"pause", pause_interpreter, 1, CMD_CAT_SEARCH, 0, NULL,
     "met la recherche en pause administrative (locale + clients connectés)",
     "Ne se lève que par « resume » (distincte de la pause de régulation posée par « limit »).\n"
     "Côté serveur : diffusée à tous les clients connectés et mémorisée pour ceux qui se\n"
     "connecteront ensuite.", NULL},
    {"resume", resume_interpreter, 1, CMD_CAT_SEARCH, 0, NULL,
     "lève la pause administrative",
     "Ne lève que la pause posée par « pause » ; la pause de régulation de débit reste\n"
     "gérée automatiquement par le régulateur.", NULL},
    {"limit", limit_interpreter, 1, CMD_CAT_SEARCH, 0, "limit <n>",
     "borne le débit de recherche à <n> coups/s (0 = illimité)", NULL, NULL},
    {"maxStockByThread", max_stock_by_thread_interpreter, 1, CMD_CAT_SEARCH, 0, "maxStockByThread <n>",
     "fixe le stock maximum de possibilités par thread", NULL, NULL},
    {"prunerBatch", pruner_batch_interpreter, 1, CMD_CAT_SEARCH, 0, "prunerBatch <n>",
     "fixe la taille de lot d'échange du pruner",
     "Bornée à [1, PRUNER_BATCH_MAX] pour maîtriser la mémoire du pruner et les tampons GPU.", NULL},
    {"prunerDfsBudget", pruner_dfs_budget_interpreter, 1, CMD_CAT_SEARCH, 0, "prunerDfsBudget <n>",
     "fixe le budget de nœuds de la preuve de fermeture bornée du pruner (§4.6b)",
     "Une possibilité jugée vivante par le contrôle superficiel mais pas encore `checked`\n"
     "est rejouée par un backtracking RÉEL plafonné à <n> nœuds ; si ce budget suffit à\n"
     "épuiser tout son sous-arbre, elle est prouvée morte (aucun faux positif possible :\n"
     "même code que la recherche réelle) et jamais redistribuée -- sinon, comportement\n"
     "inchangé (conservée, marquée checked). <n> <= 0 désactive ce contrôle supplémentaire\n"
     "(comme « limit 0 »). Bornée à [0, PRUNER_DFS_BUDGET_MAX]. DÉSACTIVÉ PAR DÉFAUT (0) :\n"
     "mesuré sans gain sur le stock réel actuel (mur structurel max_result ~74/256)\n"
     "-- opt-in, pas un défaut prudent.", NULL},

    {"sortAsc", sort_ascending_interpreter, 0, CMD_CAT_STOCK, 0, NULL,
     "trie le stock par ordre croissant (moins avancées d'abord)", NULL, NULL},
    {"sortAscFiles", sort_ascending_files_interpreter, 0, CMD_CAT_STOCK, 0, NULL,
     "trie chaque file par ordre croissant, sans les regrouper",
     "Comme « sortAsc », mais sans fusionner les files entre elles au préalable :\n"
     "chacune des files de stock est triée en place (moins avancées d'abord, dans\n"
     "cette file). Comme la consommation (GET) se fait par la fin de chaque file,\n"
     "l'effet est de consommer en priorité, sur TOUTES les files, les possibilités\n"
     "ayant le plus de cases posées -- sans concentrer le trafic sur une seule file\n"
     "comme le ferait « sortAsc ».", NULL},
    {"sortDesc", sort_descending_interpreter, 0, CMD_CAT_STOCK, 0, "sortDesc [n]",
     "trie par ordre décroissant, toutes les files ou la file <n>", NULL, NULL},
    {"sortDescFiles", sort_descending_files_interpreter, 0, CMD_CAT_STOCK, 0, NULL,
     "trie chaque file par ordre décroissant, sans les regrouper",
     "Comme « sortDesc » (sans argument), mais sans fusionner les files entre elles\n"
     "au préalable : chacune des files de stock est triée en place (plus avancées\n"
     "d'abord, dans cette file). Comme la consommation (GET) se fait par la fin de\n"
     "chaque file, l'effet est de consommer en priorité, sur TOUTES les files, les\n"
     "possibilités ayant le MOINS de cases posées -- sans concentrer le trafic sur\n"
     "une seule file comme le ferait « sortDesc ».", NULL},
    {"sortDescMulti", sortdm_interpreter, 0, CMD_CAT_STOCK, 0, NULL,
     "trie toutes les files en parallèle (multi-thread)", NULL, NULL},
    {"split", split_interpreter, 0, CMD_CAT_STOCK, 0, NULL,
     "répartit les possibilités entre les différentes files", NULL, NULL},
    {"regroup", regroup_interpreter, 0, CMD_CAT_STOCK, 0, NULL,
     "regroupe toutes les possibilités dans une seule file", NULL, NULL},
    {"removeNoNext", rmnonext_interpreter, 1, CMD_CAT_STOCK, 0, NULL,
     "supprime les possibilités sans continuation possible (élagage)", NULL, NULL},
    {"expand", expand_interpreter, 0, CMD_CAT_STOCK, 0, "expand <niveau>",
     "développe le stock jusqu'au niveau de curseur <niveau> (anti-famine)",
     "Place une pièce candidate sur la case suivante de chaque possibilité, jusqu'au niveau\n"
     "demandé — utile côté serveur quand le stock distribuable s'est raréfié. Borné en\n"
     "profondeur (expand_max_levels, réglable via --expand-max-levels au démarrage) et en\n"
     "volume (expand_max_stock, réglable via --expand-max-stock) ; niveau 3-4 recommandé.", NULL},
    {"restockAnalysed", restockanalysed_interpreter, 0, CMD_CAT_STOCK, 0, NULL,
     "remet les possibilités en cours d'analyse dans le stock", NULL, NULL},
    {"rebalance", rebalance_interpreter, 0, CMD_CAT_STOCK, 0, "rebalance [n]",
     "rééquilibre le stock : file la plus pleine -> la plus vide",
     "Déplace jusqu'à <n> possibilités (défaut rebalance_budget, réglable via\n"
     "--rebalance-budget au démarrage) de la file la plus pleine vers la plus\n"
     "vide, pour les deux pools (non vérifié et vérifié) indépendamment --\n"
     "enchaîne autant de paires que le budget le permet, pas un seul pas isolé.\n"
     "Même appel que celui automatique de chaque tour serveur (10 s) — utile\n"
     "pour forcer un rééquilibrage immédiat plutôt que d'attendre plusieurs\n"
     "tours. Contrairement à split, borné par <n> : peut s'arrêter avant un\n"
     "équilibre complet sur un très gros déséquilibre.", NULL},
    {"stockMemory", stock_memory_interpreter, 0, CMD_CAT_STOCK, 1, NULL,
     "affiche le plafond RAM du stock, l'occupation actuelle et le débordement disque",
     "Deux pools comptés ensemble (non vérifié + vérifié), jamais le pool\n"
     "analysé (borné autrement : baux d'expiration, clients en vol -- cf.\n"
     "leaseDuration). « illimité » si aucun plafond n'est fixé (défaut, ou\n"
     "stockMaxRam 0). L'occupation est une ESTIMATION (sizeof(Element) +\n"
     "sizeof(possibility_packet) + surcoût d'allocateur par possibilité,\n"
     "jamais un relevé RSS réel du process) -- même chiffre que celui exposé\n"
     "par GET /api/v1/status (stock_ram_limit_mb / stock_ram_used_mb). Affiche\n"
     "aussi le débordement sur disque (--stock-spill-dir), toujours présent même\n"
     "à 0, et le TOTAL (résident + déporté) -- même paire que GET /api/v1/stats\n"
     "(stock_spilled_packets / stock_spill_segments).", NULL},
    {"stockMaxRam", stock_max_ram_interpreter, 0, CMD_CAT_STOCK, 1, "stockMaxRam <mo>",
     "fixe à chaud le plafond RAM (Mo) des deux pools de stock (--stock-max-ram)",
     "<mo> <= 0 désactive le plafond (illimité), même convention que `limit 0`\n"
     "et `leaseDuration 0` -- PAS une erreur d'usage, contrairement à un\n"
     "argument entièrement absent. Converti UNE SEULE FOIS en nombre de\n"
     "possibilités (l'unité réellement comparée par chaque ADD) : resserrer le\n"
     "plafond en dessous de l'occupation actuelle ne fait RIEN à ce qui est\n"
     "déjà résident -- seuls les ADD futurs sont refusés jusqu'à repasser sous\n"
     "le plafond. Ne couvre pas le pool analysé (cf. stockMemory).", NULL},
    {"spill", spill_interpreter, 0, CMD_CAT_STOCK, 1, "spill [n]",
     "déclenche immédiatement un pas de débordement/rechargement sur disque",
     "Même appel que celui automatique du thread de débordement (100 ms) --\n"
     "utile pour forcer un pas immédiat plutôt que d'attendre le prochain tick.\n"
     "<n> optionnel : budget de possibilités pour CE pas (défaut\n"
     "STOCK_SPILL_BLOCK_PACKETS, 4096). Sens automatique : évince la file la\n"
     "plus pleine si l'occupation RAM dépasse 90 % du plafond --stock-max-ram\n"
     "(jusqu'à redescendre à 75 %), recharge la file la plus chargée sur disque\n"
     "si elle redescend sous 25 % (jusqu'à remonter à 75 %). No-op silencieux\n"
     "sans --stock-max-ram (illimité) ou sans\n"
     "--stock-spill-dir utilisable (voir stockMemory pour l'état courant).", NULL},
    {"min", min_interpreter, 1, CMD_CAT_STOCK, 0, NULL,
     "affiche le niveau minimal de pièces placées dans les files", NULL, NULL},

    {"backup", backup_interpreter, 1, CMD_CAT_BACKUP, 0, NULL,
     "sauvegarde les files dans les fichiers .back",
     "Écrit ./eternityII.back, ./eternityII-in_analyse.back, ./eternityII-best_board.back\n"
     "et ./eternityII-known_clients.back (noms suffixés du pid côté client).", NULL},
    {"restore", restore_interpreter, 1, CMD_CAT_BACKUP, 0, "restore [fichier [fichier_analyse]]",
     "restaure les files depuis les fichiers .back (remplace le stock)",
     "La recherche est suspendue pendant le remplacement. Sans argument :\n"
     "./eternityII.back et ./eternityII-in_analyse.back. Le meilleur plateau connu et\n"
     "le cumul par machine (./eternityII-best_board.back, ./eternityII-known_clients.back)\n"
     "sont rechargés en plus, sans argument dédié ; leur absence n'empêche pas la\n"
     "restauration du stock.", NULL},
    {"import", import_interpreter, 0, CMD_CAT_BACKUP, 0, NULL,
     "importe les fichiers .back en plus du stock courant",
     "Contrairement à « restore », le stock courant n'est pas vidé.", NULL},
    {"loadJson", loadjson_interpreter, 0, CMD_CAT_BACKUP, 0, NULL,
     "importe une possibilité depuis une chaîne JSON", NULL, NULL},

    {"check", check_interpreter, 0, CMD_CAT_DIAG, 0, NULL,
     "affiche le dernier rapport de statistiques", NULL, NULL},
    {"print", print_interpreter, 0, CMD_CAT_DIAG, 0, "print [fichier]",
     "affiche l'état du data manager (files, tailles)",
     "Avec [fichier] : exporte le dump JSON complet dans ce fichier au lieu de la\n"
     "console (évite qu'un gros stock ne déborde le pad de sortie en ncurses).", NULL},
    {"printFile", printfile_interpreter, 0, CMD_CAT_DIAG, 0, "printFile <n> [fichier]",
     "affiche le contenu de la file numéro <n>",
     "Avec [fichier] : exporte cette file au format JSON dans ce fichier au lieu de\n"
     "la console.", NULL},
    {"printAnalysed", printanalysed_interpreter, 1, CMD_CAT_DIAG, 0, "printAnalysed [fichier]",
     "affiche les possibilités en cours d'analyse",
     "Avec [fichier] : exporte le dump JSON complet dans ce fichier au lieu de la\n"
     "console. Propagée aux process fils : en mode client, le nom est suffixé du\n"
     "pid (comme « backup ») pour que chaque process écrive dans son propre fichier.", NULL},
    {"statistic", statistic_interpreter, 0, CMD_CAT_DIAG, 0, NULL,
     "affiche des statistiques détaillées sur le contenu des files", NULL, NULL},
    {"checkDatas", checkdatas_interpreter, 0, CMD_CAT_DIAG, 0, NULL,
     "vérifie l'intégrité des possibilités stockées", NULL, NULL},
    {"checkDuplicate", check_duplicate_interpreter, 0, CMD_CAT_DIAG, 0, NULL,
     "recherche les doublons dans les files", NULL, NULL},
    {"checkOrigin", check_origin_interpreter, 0, CMD_CAT_DIAG, 0, "checkOrigin [purge]",
     "vérifie qu'aucune possibilité en stock n'est la racine d'une autre",
     "Une possibilité dont TOUTES les cases posées se retrouvent à l'identique dans une\n"
     "autre, plus profonde, est la racine de celle-ci : le sous-arbre de la seconde est\n"
     "déjà couvert par la première, le travail est fait deux fois. Balaie les deux pools\n"
     "(non vérifié ET vérifié) et rapporte chaque relation trouvée (100 lignes de détail\n"
     "au plus, puis le seul total).\n"
     "Avec « purge » : supprime chaque DESCENDANT et garde la RACINE, jamais l'inverse.\n"
     "Le sous-arbre du descendant est inclus dans celui de sa racine, donc aucune branche\n"
     "de recherche n'est perdue ; et comme on ne sait pas ce qui a produit ces paires, on\n"
     "ne fait pas confiance à l'état intermédiaire — le travail est à refaire depuis la\n"
     "racine. Coût assumé : un descendant « checked » emporte son prunage déjà payé.\n"
     "Lancer « backup » ensuite pour graver l'état purgé. Sans argument, le stock n'est\n"
     "jamais modifié.\n"
     "Coût en O(n²) sur la taille du stock : commande de diagnostic, pas de routine.\n"
     "Ne balaie que le stock RÉSIDENT — ce qui a débordé sur disque (« spill ») est\n"
     "signalé mais pas comparé.", NULL},
    {"checkFiles", checkfiles_interpreter, 0, CMD_CAT_DIAG, 0, NULL,
     "vérifie l'intégrité de toutes les files", NULL, NULL},
    {"checkFile", checkfile_interpreter, 0, CMD_CAT_DIAG, 0, "checkFile <n>",
     "vérifie l'intégrité de la file numéro <n>", NULL, NULL},
    {"checkDirections", checkdirections_interpreter, 0, CMD_CAT_DIAG, 0, NULL,
     "vérifie la cohérence du tableau de parcours", NULL, NULL},

    {"clients", clients_interpreter, 0, CMD_CAT_CLIENTS, 1, NULL,
     "liste les sessions de contrôle actives (session_no, libellé, ip, pid, mode, forks, identité)", NULL, NULL},
    {"clientsStats", clients_stats_interpreter, 0, CMD_CAT_CLIENTS, 1, NULL,
     "demande leurs statistiques à tous les clients connectés", NULL, NULL},
    {"clientsCommand", clients_cmd_interpreter, 0, CMD_CAT_CLIENTS, 1,
     "clientsCommand [--to <session_no|client_uid|label>] <commande...>",
     "pousse une commande à un client précis ou à tous les clients connectés",
     "Liste blanche : pause, resume, limit, maxStockByThread, prunerBatch, prunerDfsBudget.\n"
     "Toute autre commande est refusée sans être diffusée, avec ou sans --to.\n"
     "Sans --to : diffusion à toutes les sessions de contrôle actives (comportement\n"
     "historique). Avec --to <cible> : n'atteint QUE la session désignée, par son\n"
     "session_no (entier, cf. commande « clients »), son client_uid (hexadécimal\n"
     "complet) ou son label déclaré (--name) -- la cible ne doit pas contenir\n"
     "d'espace. Un session_no ou client_uid qui ne désigne plus aucune session\n"
     "active (client déconnecté ou remplacé) est refusé, jamais redirigé vers un\n"
     "autre client ; un label partagé par plusieurs sessions actives est refusé\n"
     "comme ambigu.", NULL},
    {"clientsRoles", clients_roles_interpreter, 0, CMD_CAT_CLIENTS, 1,
     "clientsRoles [--to <session_no|client_uid|label>] <nb_pruner>",
     "fixe le dosage recherche/contrôle (pruner_forks) d'un client précis ou de tous les clients connectés",
     "Compose « config pruner_forks <nb_pruner> » + « configApply » -- même\n"
     "résolution de cible que clientsCommand --to (session_no, client_uid, ou\n"
     "label). <nb_pruner> est le\n"
     "nombre de forks affectés au CONTRÔLE parmi le nb_forks de CHAQUE client\n"
     "touché ; une valeur hors [0, nb_forks] est clampée côté client\n"
     "(resolve_pruner_forks), jamais ici -- le serveur ne connaît pas le nb_forks\n"
     "de la cible. Sans --to : diffusion à toutes les sessions actives. Le dosage\n"
     "envoyé est aussi MÉMORISÉ par machine (machine_uid, pas client_uid ni\n"
     "session_no) : une machine touchée qui se reconnecte ou redémarre le reçoit\n"
     "automatiquement à sa prochaine connexion, sans qu'il faille rejouer la\n"
     "commande -- même mécanisme que pause/resume (g_desired_pause_state).", NULL},
    {"knownClients", known_clients_interpreter, 0, CMD_CAT_CLIENTS, 1, NULL,
     "liste les machines connues (cumul, statut connecté/déconnecté)",
     "Distinct de « clients » : cette liste survit à la déconnexion (une machine reste\n"
     "visible, marquée déconnectée) et cumule, par machine_uid, le nombre de connexions,\n"
     "le débit de prunage (checked/removed) et le meilleur résultat jamais rapportés --\n"
     "toutes exécutions de processus confondues. Persisté dans ./eternityII-known_clients.back\n"
     "(voir « backup »/« restore ») : un redémarrage du serveur ne remet pas ce cumul à zéro.", NULL},
    {"clientsWork", clients_work_interpreter, 0, CMD_CAT_CLIENTS, 1, "clientsWork <session_no|client_uid|label>",
     "affiche ce qu'un client précis détient actuellement en cours d'analyse",
     "Résolution de la cible identique à « clientsCommand --to » (refusée si inconnue,\n"
     "déconnectée ou ambiguë). Lecture pure de l'attribution enregistrée côté serveur au\n"
     "moment où la possibilité a été servie (INST_GET/INST_GET_TO_CHECK[_BATCH]) : aucun\n"
     "aller-retour réseau vers le client, aucune commande poussée.", NULL},
    {"leaseDuration", lease_duration_interpreter, 0, CMD_CAT_CLIENTS, 1, "leaseDuration <n>",
     "fixe la durée (secondes) du bail à expiration des possibilités attribuées",
     "Passé ce délai ET si le client n'a plus de session de contrôle active (déconnexion\n"
     "confirmée), une possibilité attribuée (clientsWork) est rendue automatiquement au\n"
     "stock non vérifié -- un client mort (kill -9, coupure réseau, panne) ne gèle plus sa\n"
     "part indéfiniment. Un client toujours connecté (canal de contrôle vivant) n'expire\n"
     "JAMAIS, quelle que soit la durée d'analyse -- ce délai n'est qu'un minorant avant la\n"
     "première vérification de vivacité, pas un budget de temps garanti. Balayage borné,\n"
     "au rythme du tour de statistiques serveur (10 s). <n> <= 0 désactive le bail (comme\n"
     "« limit 0 » pour la régulation de débit). N'affecte que les possibilités attribuées\n"
     "APRÈS ce changement ; défaut : ANALYSED_LEASE_DEFAULT_SECONDS (300 s).", NULL},

    /* Alias : résolus vers l'entrée canonique par find_command. Les noms
       historiques abrégés (sorta, rmnonext, …) restent acceptés ici ; les
       anciens noms tout-minuscule (printfile, checkdatas, …) n'ont pas besoin
       d'alias, la correspondance ignorant déjà la casse. */
    {"?", NULL, 0, CMD_CAT_GENERAL, 0, NULL, NULL, NULL, "help"},
    {"quit", NULL, 0, CMD_CAT_GENERAL, 0, NULL, NULL, NULL, "exit"},
    {"cls", NULL, 0, CMD_CAT_GENERAL, 0, NULL, NULL, NULL, "clear"},
    {"stats", NULL, 0, CMD_CAT_DIAG, 0, NULL, NULL, NULL, "statistic"},
    {"sorta", NULL, 0, CMD_CAT_STOCK, 0, NULL, NULL, NULL, "sortAsc"},
    {"sortaf", NULL, 0, CMD_CAT_STOCK, 0, NULL, NULL, NULL, "sortAscFiles"},
    {"sortdf", NULL, 0, CMD_CAT_STOCK, 0, NULL, NULL, NULL, "sortDescFiles"},
    {"sortd", NULL, 0, CMD_CAT_STOCK, 0, NULL, NULL, NULL, "sortDesc"},
    {"sortdm", NULL, 0, CMD_CAT_STOCK, 0, NULL, NULL, NULL, "sortDescMulti"},
    {"rmnonext", NULL, 0, CMD_CAT_STOCK, 0, NULL, NULL, NULL, "removeNoNext"},
    {"prune", NULL, 0, CMD_CAT_STOCK, 0, NULL, NULL, NULL, "removeNoNext"},
    {"clientsCmd", NULL, 0, CMD_CAT_CLIENTS, 1, NULL, NULL, NULL, "clientsCommand"}
};

/** @brief Interpréteur de la commande `sortAsc` (alias `sorta`) : tri ascendant des possibilités. */
int sort_ascending_interpreter(void) {
    return sort_ascending();
}

/** @brief Interpréteur de la commande `sortAscFiles` (alias `sortaf`) : tri ascendant de chaque file, sans regroupement. */
int sort_ascending_files_interpreter(void) {
    return sort_ascending_files();
}

/** @brief Interpréteur de la commande `sortDesc [n_file]` (alias `sortd`) : tri descendant, optionnellement sur un seul fichier. */
int sort_descending_interpreter(void) {
    char *arguments = strtok(NULL, " ");
    if (arguments == NULL) {
        return sort_descending();
    }
    
    int n_file = atoi(arguments);
    sort_d_mono(&n_file);
    return 0;
}

/** @brief Interpréteur de la commande `sortDescFiles` (alias `sortdf`) : tri descendant de chaque file, sans regroupement. */
int sort_descending_files_interpreter(void) {
    return sort_descending_files();
}

/** @brief Interpréteur de `maxStockByThread <n>` : fixe la limite de possibilités par thread. */
int max_stock_by_thread_interpreter(void) {
    char *arguments = strtok(NULL, " ");
    if (arguments != NULL) {
        max_stock_by_thread = atoi(arguments);
        return 0;
    }
    return CMD_ERR_USAGE;
}

/**
 * @brief Voir la doc dans command_lines.h.
 */
int pruner_batch_clamp(int v) {
    if (v < 1) {
        return 1;
    }
    if (v > PRUNER_BATCH_MAX) {
        return PRUNER_BATCH_MAX;
    }
    return v;
}

/**
 * @brief Interpréteur de `prunerBatch <n>` : fixe la taille de lot d'échange du
 *        pruner (nombre de possibilités demandées/acquittées par aller-retour).
 *
 * Propagée aux process enfants (send_to_childs = 1). Bornée à [1, PRUNER_BATCH_MAX]
 * pour maîtriser la mémoire du pruner et les tampons GPU.
 */
int pruner_batch_interpreter(void) {
    char *arguments = strtok(NULL, " ");
    if (arguments != NULL) {
        pruner_batch_size = pruner_batch_clamp(atoi(arguments));
        return 0;
    }
    return CMD_ERR_USAGE;
}

/**
 * @brief Voir la doc dans command_lines.h.
 */
int pruner_dfs_budget_clamp(int v) {
    if (v < 0) {
        return 0;
    }
    if (v > PRUNER_DFS_BUDGET_MAX) {
        return PRUNER_DFS_BUDGET_MAX;
    }
    return v;
}

/**
 * @brief Interpréteur de `prunerDfsBudget <n>` : fixe le budget de nœuds de la
 *        preuve de fermeture bornée du pruner CPU (§4.6b).
 *
 * Propagée aux process enfants (send_to_childs = 1). Bornée à
 * [0, PRUNER_DFS_BUDGET_MAX] ; `<n> <= 0` désactive ce contrôle supplémentaire
 * (même convention que `limit 0`).
 */
int pruner_dfs_budget_interpreter(void) {
    char *arguments = strtok(NULL, " ");
    if (arguments != NULL) {
        pruner_dfs_budget = pruner_dfs_budget_clamp(atoi(arguments));
        return 0;
    }
    return CMD_ERR_USAGE;
}

/** @brief Interpréteur de `limit <n>` : fixe le débit maximum de recherche par seconde. */
int limit_interpreter(void) {
    char *arguments = strtok(NULL, " ");
    if (arguments != NULL) {
        max_search_by_sec = atoi(arguments);
        return 0;
    }

    return CMD_ERR_USAGE;
}

/**
 * @brief Interpréteur de `check` : affiche le dernier rapport de statistiques `lastcheck`.
 *
 * `lastcheck` est republié toutes les 10 secondes par un thread de
 * statistiques (`check_server`/`check_client_threads`, via
 * `lastcheck_publish()`), potentiellement pendant que cette commande
 * s'exécute depuis le thread console. On prend donc `lastcheck_mutex` pour
 * copier le rapport dans un buffer local (`strdup`), et on logge cette copie
 * une fois le verrou relâché : sans cela, une lecture concurrente au swap
 * pointeur/free pourrait déréférencer un buffer déjà libéré (use-after-free)
 * ou encore en cours de remplissage.
 *
 * N'efface plus l'écran : l'effacement est réservé à la commande `clear`
 * (aucune commande ne doit effacer implicitement — politique d'affichage).
 */
int check_interpreter(void) {
    pthread_mutex_lock(&lastcheck_mutex);
    char *report_copy = lastcheck != NULL ? strdup(lastcheck) : NULL;
    pthread_mutex_unlock(&lastcheck_mutex);

    log_info("%s\n", report_copy != NULL ? report_copy : "");
    free(report_copy);
    return 0;
}

/** @brief Interpréteur de `clear` (alias `cls`, raccourci Ctrl-L) : efface l'écran, le contenu reste dans le scrollback (ANSI) ou le pad (ncurses). */
int clear_interpreter(void) {
    clear_console();
    return 0;
}

/** @brief Nom d'affichage d'un état d'orchestrateur (commande console `config`). */
static const char *orch_state_name(orch_state_t s) {
    switch (s) {
    case ORCH_WAITING_CONFIG: return "WAITING_CONFIG";
    case ORCH_COUNTDOWN:      return "COUNTDOWN";
    case ORCH_CONFIGURING:    return "CONFIGURING";
    case ORCH_RUNNING:        return "RUNNING";
    case ORCH_STOPPING:       return "STOPPING";
    case ORCH_APPLYING:       return "APPLYING";
    case ORCH_EXITING:        return "EXITING";
    }
    return "?";
}

/**
 * @brief Interpréteur de `config` (sans argument) / `config <clé> <valeur>`.
 *
 * Branche sur `server` (côté serveur, aucun équivalent positionnel) :
 *
 * - SERVEUR, sans argument : affiche la configuration EFFECTIVE du serveur
 *   (`server_config_capture_effective`/`server_config_format`) et le fichier
 *   `--config-file` en vigueur. Pas d'état d'orchestrateur ni de
 *   configuration "en préparation" — le serveur n'en a pas.
 * - SERVEUR, avec `<clé> <valeur>` : refusée (`-1`) — le serveur n'a pas de
 *   configuration "en préparation" à appliquer à chaud (`configApply` n'existe
 *   pas côté serveur, cf. `command_is_client_only`) ; seule `configSave`
 *   persiste la configuration EFFECTIVE actuelle. Éditer le fichier
 *   `--config-file` puis redémarrer reste le chemin pour changer une valeur
 *   qui n'a pas de commande console dédiée (`stockMaxRam`, `spill`,
 *   `rebalance`, `leaseDuration`, `clientsRoles`, …).
 * - CLIENT/PRUNER, sans argument : affiche l'état de l'orchestrateur
 *   (`fork_orchestrator_snapshot`), la configuration EFFECTIVE (celle
 *   réellement en vigueur — voir `client_config_capture_effective`, reflète
 *   aussi un `limit`/`maxStockByThread`/`prunerBatch` déjà exécuté depuis
 *   cette même console) et la configuration EN PRÉPARATION
 *   (`fork_orchestrator_format_staged_config`). N'annule pas le décompte.
 * - CLIENT/PRUNER, avec deux arguments (`strtok` — même convention que
 *   `limit`/`prunerBatch` : un token par espace, pas de valeur contenant un
 *   espace) : synthétise une ligne `clé = valeur` et la délègue à
 *   `fork_orchestrator_stage_config_line` (réutilise `client_config_parse_line`,
 *   jamais de logique de validation dupliquée) — écrit dans la configuration
 *   en préparation et annule DÉFINITIVEMENT le décompte, seulement si la
 *   ligne est acceptée (une faute de frappe ne doit pas faire perdre
 *   l'auto-démarrage). Un seul argument (clé sans valeur) : `CMD_ERR_USAGE`.
 */
int config_interpreter(void) {
    char *key = strtok(NULL, " ");

    if (server) {
        if (key != NULL) {
            log_error("config : \"config <clé> <valeur>\" n'est pas supporté en mode serveur "
                      "(pas de configuration \"en préparation\" à appliquer à chaud) -- éditez "
                      "le fichier %s puis redémarrez, ou utilisez configSave pour figer l'état "
                      "courant\n", server_config_file_path);
            return -1;
        }
        server_config_t cfg;
        server_config_capture_effective(&cfg);
        char buf[1024];
        server_config_format(&cfg, buf, sizeof(buf));
        server_config_free(&cfg);

        log_info("config : configuration effective (fichier : %s) :\n%s",
                  server_config_file_path,
                  buf[0] != '\0' ? buf : "  (aucune valeur)\n");
        return 0;
    }

    if (key != NULL) {
        char *value = strtok(NULL, " ");
        if (value == NULL) {
            return CMD_ERR_USAGE;
        }
        char line[600];
        snprintf(line, sizeof(line), "%s = %s", key, value);
        client_config_line_status_t status = fork_orchestrator_stage_config_line(line);
        switch (status) {
        case CLIENT_CONFIG_LINE_SET:
            log_info("config : \"%s\" préparé (%s = %s) — décompte annulé s'il était en cours\n",
                      key, key, value);
            return 0;
        case CLIENT_CONFIG_LINE_UNKNOWN_KEY:
            log_error("config : clé inconnue \"%s\"\n", key);
            return -1;
        case CLIENT_CONFIG_LINE_INVALID_VALUE:
            log_error("config : valeur invalide pour \"%s\" : \"%s\"\n", key, value);
            return -1;
        case CLIENT_CONFIG_LINE_IGNORED:
            return 0;
        }
        return -1;
    }

    orch_state_t state;
    long countdown_remaining_ms;
    fork_orchestrator_snapshot(&state, &countdown_remaining_ms);

    char state_line[80];
    if (state == ORCH_COUNTDOWN && countdown_remaining_ms > 0) {
        snprintf(state_line, sizeof(state_line), "%s (auto-démarrage dans %llds)",
                  orch_state_name(state),
                  (long long)((countdown_remaining_ms + 999) / 1000));
    } else {
        snprintf(state_line, sizeof(state_line), "%s", orch_state_name(state));
    }

    client_config_t cfg;
    client_config_capture_effective(&cfg, g_client_server_host);
    char buf[1024];
    client_config_format(&cfg, buf, sizeof(buf));
    client_config_free(&cfg);

    char staged_buf[1024];
    fork_orchestrator_format_staged_config(staged_buf, sizeof(staged_buf));

    log_info("config : état=%s\n"
              "configuration effective (fichier : %s) :\n%s"
              "configuration en préparation :\n%s",
              state_line, client_config_file_path,
              buf[0] != '\0' ? buf : "  (aucune valeur)\n",
              staged_buf[0] != '\0' ? staged_buf : "  (aucune valeur)\n");
    return 0;
}

/**
 * @brief Interpréteur de `start` : fork immédiat avec la configuration
 *        EFFECTIVE (pas celle en préparation, cf. commande `config`).
 *
 * Poste `EV_START` (même chemin de code qu'un décompte de COUNTDOWN écoulé)
 * via `fork_orchestrator_post_event`, qui applique la transition
 * SYNCHRONEMENT : le résultat (erreur si déjà en RUNNING) est donc connu
 * immédiatement, sans latence de sondage.
 */
int start_interpreter(void) {
    orch_actions_t actions;
    fork_orchestrator_post_event(EV_START, &actions);
    if (actions.error == ORCH_ERR_ALREADY_RUNNING) {
        log_error("start : la recherche est déjà en cours d'exécution\n");
        return -1;
    }
    log_info("start : démarrage demandé\n");
    return 0;
}

/**
 * @brief Interpréteur de `stopForks` : arrête les fils de recherche sans
 *        quitter ce process (console, canal de contrôle, API HTTP restent
 *        actifs).
 *
 * Poste `EV_STOP_FORKS` (`fork_orchestrator_post_event`, transition
 * SYNCHRONE) : erreur immédiate si aucun fork n'est en cours d'exécution
 * (`ORCH_ERR_NOT_RUNNING`). La séquence d'arrêt/escalade/récolte elle-même
 * s'exécute sur le thread orchestrateur (`fork_orchestrator_run`), jamais sur
 * ce thread console — cette commande ne fait que la DEMANDER.
 */
int stop_forks_interpreter(void) {
    orch_actions_t actions;
    fork_orchestrator_post_event(EV_STOP_FORKS, &actions);
    if (actions.error == ORCH_ERR_NOT_RUNNING) {
        log_error("stopForks : aucun fork en cours d'exécution\n");
        return -1;
    }
    log_info("stopForks : arrêt des process de recherche demandé\n");
    return 0;
}

/**
 * @brief Interpréteur de `configApply` : applique la configuration en
 *        préparation (`config <clé> <valeur>`), à chaud si possible.
 *
 * `client_config_diff` (via `fork_orchestrator_diff_staged_config`) décide
 * entre les deux chemins :
 *  - `HOT_ONLY` (aucune clé stagée parmi nb_forks/server_host/parts_file) :
 *    application immédiate aux globales du parent + diffusion IPC aux fils
 *    déjà en cours (`fork_orchestrator_apply_hot_staged_config`), sans jamais
 *    interrompre la recherche.
 *  - `NEEDS_RESTART` : poste `EV_RESTART` (erreur immédiate si aucun fork
 *    n'est en cours d'exécution) — l'arrêt, la reconstruction et le re-fork
 *    s'exécutent ensuite sur le thread orchestrateur, cette commande ne fait
 *    que le déclencher.
 */
int config_apply_interpreter(void) {
    // Vérifié EN PREMIER, indépendamment du verdict de client_config_diff :
    // sans ce garde-fou, un configApply HOT_ONLY (ex. valeur stagée identique
    // à l'effective) alors qu'AUCUN fork n'existe (ORCH_WAITING_CONFIG,
    // stopForks déjà passé par là) rapportait à tort "configuration à chaud
    // appliquée, aucun redémarrage nécessaire" sans jamais avoir démarré quoi
    // que ce soit — trouvé en testant manuellement stopForks puis configApply
    // avec une valeur inchangée. `clientsWork`/`clientsCommand` n'ont pas ce
    // problème : ce sont des commandes SERVEUR, sans notion d'orchestrateur
    // local.
    orch_state_t state;
    fork_orchestrator_snapshot(&state, NULL);
    if (state != ORCH_RUNNING) {
        log_error("configApply : aucun fork en cours d'exécution -- utilisez "
                  "\"config\" puis \"start\"\n");
        return -1;
    }

    client_config_t effective;
    client_config_capture_effective(&effective, g_client_server_host);
    client_config_diff_t diff = fork_orchestrator_diff_staged_config(&effective);
    client_config_free(&effective);

    if (diff == CLIENT_CONFIG_DIFF_NEEDS_RESTART) {
        // Revérifié ICI, sur le chemin de reconfiguration à CHAUD : le
        // garde-fou --gpu + --pruner-forks (gpu_pruner_forks_conflict,
        // app_runtime.h) n'est autrement évalué qu'une fois, avant le tout
        // premier fork() du process (handle_client/main.c). Sans ce test,
        // un `configApply` NEEDS_RESTART -- déclenché en console, ou poussé
        // à distance par `clientsRoles`/`--auto-roles` via le canal de
        // contrôle -- pouvait re-forker un client `pruner --gpu` avec un
        // pruner_forks stagé différent de nb_forks : exactement l'état que
        // le garde-fou de démarrage rend impossible, mais silencieusement
        // ici (aucun refus, aucun log_error).
        if (fork_orchestrator_staged_gpu_pruner_conflict()) {
            log_error("configApply : refusé -- la configuration en préparation rendrait "
                      "pruner_forks incompatible avec le pruner GPU actif (--gpu) ; le "
                      "contexte CUDA n'est initialisé qu'une fois par process, retirer "
                      "pruner_forks de la configuration en préparation ou l'aligner sur "
                      "nb_forks\n");
            return -1;
        }

        orch_actions_t actions;
        fork_orchestrator_post_event(EV_RESTART, &actions);
        if (actions.error == ORCH_ERR_NOT_RUNNING) {
            // Course rare : les fils sont morts entre le snapshot ci-dessus
            // et ce post_event (ex. --stop-on-solution). Même message, même
            // code d'erreur : l'opérateur voit la même chose dans les deux cas.
            log_error("configApply : aucun fork en cours d'exécution -- utilisez "
                      "\"config\" puis \"start\"\n");
            return -1;
        }
        log_info("configApply : redémarrage à chaud demandé (nb_forks/server_host/"
                  "parts_file modifié)\n");
        return 0;
    }

    fork_orchestrator_apply_hot_staged_config();
    log_info("configApply : configuration à chaud appliquée, aucun redémarrage nécessaire\n");
    return 0;
}

/**
 * @brief Interpréteur de `configSave` : écrit la configuration EFFECTIVE
 *        (serveur ou client, selon `server`) dans le fichier `--config-file`
 *        (écriture atomique).
 *
 * Côté serveur : pas de configuration "en préparation" à superposer (aucun
 * équivalent de `config <clé> <valeur>` là-bas) — `server_config_capture_effective`
 * suffit seule.
 */
int config_save_interpreter(void) {
    if (server) {
        server_config_t cfg;
        server_config_capture_effective(&cfg);
        int rc = server_config_save(server_config_file_path, &cfg);
        server_config_free(&cfg);

        if (rc == 0) {
            log_info("configSave : configuration écrite dans \"%s\"\n", server_config_file_path);
        } else {
            log_error("configSave : échec de l'écriture dans \"%s\"\n", server_config_file_path);
        }
        return rc;
    }

    client_config_t cfg;
    client_config_capture_effective(&cfg, g_client_server_host);
    // Superpose la configuration EN PRÉPARATION (config <clé> <valeur>) sur
    // l'effective : sans ceci, une valeur préparée puis sauvegardée était
    // perdue au redémarrage suivant (configSave ne capturait jamais que
    // l'effective).
    fork_orchestrator_merge_staged_config(&cfg);

    int rc = client_config_save(client_config_file_path, &cfg);
    client_config_free(&cfg);

    if (rc == 0) {
        log_info("configSave : configuration écrite dans \"%s\"\n", client_config_file_path);
    } else {
        log_error("configSave : échec de l'écriture dans \"%s\"\n", client_config_file_path);
    }
    return rc;
}

/** @brief Interpréteur de `backup` : sauvegarde les files de possibilités dans les fichiers `.back`. */
int backup_interpreter(void) {
    log_info("start backup\n");
    char *def_file = DEF_FILE;
    char *def_analyse_file = DEF_ANALYSE_FILE;
    char *def_best_board_file = DEF_BEST_BOARD_FILE;
    char *def_known_clients_file = DEF_KNOWN_CLIENTS_FILE;
    int isServer = server;
    if (isServer == 0) {
        char *temp = malloc(sizeof(char) *(strlen(def_file) + 11));
        sprintf(temp, "%s_%i", def_file, getpid());
        def_file = temp;
        temp = malloc(sizeof(char) * (strlen(def_analyse_file)+ 11));
        sprintf(temp, "%s_%i", def_analyse_file, getpid());
        def_analyse_file = temp;
        temp = malloc(sizeof(char) * (strlen(def_best_board_file)+ 11));
        sprintf(temp, "%s_%i", def_best_board_file, getpid());
        def_best_board_file = temp;
        temp = malloc(sizeof(char) * (strlen(def_known_clients_file)+ 11));
        sprintf(temp, "%s_%i", def_known_clients_file, getpid());
        def_known_clients_file = temp;
    }
    int rba = 0;
    // "snapshot" (débordement disque) : sans effet côté client
    // (stock_spill n'est jamais configuré hors du rôle serveur — la
    // fonction est un no-op silencieux via son propre g_spill_enabled).
    int rb = consistent_backup(def_file, def_analyse_file, &rba, "snapshot", stock_spill_snapshot);
    if (rb == BACKUP_SKIPPED_MAINTENANCE) {
        log_info("backup de %s sauté (maintenance en cours)\n", def_file);
    } else if (rb != BACKUP_OK) {
        log_info("backup de %s échoué\n", def_file);
    }
    if (rba == BACKUP_SKIPPED_MAINTENANCE) {
        log_info("backup de %s sauté (maintenance en cours)\n", def_analyse_file);
    } else if (rba != BACKUP_OK) {
        log_info("backup de %s échoué\n", def_analyse_file);
    }
    // Représentation du meilleur plateau connu (pas seulement max_result) :
    // même commande console, fichier dédié (cf. core/best_board.h). Absent des
    // codes BACKUP_SKIPPED_MAINTENANCE/BACKUP_OK (best_board_save n'a pas de
    // section « maintenance » à sauter, cf. best_board.c) : 0 = succès.
    if (best_board_save(&g_server_best_board, def_best_board_file) != 0) {
        log_info("backup de %s échoué\n", def_best_board_file);
    }
    // Cumul par machine : même cadence que
    // le reste du stock, fichier dédié (cf. app/known_clients_registry.h).
    if (known_clients_registry_save(def_known_clients_file) != 0) {
        log_info("backup de %s échoué\n", def_known_clients_file);
    }
    log_info("backup ended\n");
    if (isServer == 0) {
        free(def_file);
        free(def_analyse_file);
        free(def_best_board_file);
        free(def_known_clients_file);
    }
    return 0;
}

/** @brief Interpréteur de `exit` : arrête proprement le programme (signal SIGINT aux enfants en mode client). */
int exit_interpreter(void) {
    // Trace explicite de la demande d'arrêt : ce chemin appelle exit()
    // directement plus bas (mode serveur, ou une fois les fils du client
    // récoltés) et ne repasse donc jamais par le log de fin de main() —
    // sans cette ligne, un `exit` déclenché à distance (canal de contrôle,
    // API HTTP `POST /api/v1/command`) ne laissait aucune trace de LA CAUSE
    // de l'arrêt dans les logs, uniquement son effet.
    log_info("exit : arrêt du programme demandé\n");
    request = REQUEST_STOP;
    if (server == 0) {
        if (parent_pid == getpid()) {
            // On attend que les threads enfants se terminent
            if (childrens_pid != NULL) {
#ifdef DEBUG_SIGNAL
                log_info("send signal to child\n");
#endif // DEBUG_SIGNAL
                for (int c = 0; c < NB_THREADS; c++) {
                    // On attend que le thread se termine
                    pid_t childrenPid = childrens_pid[c];
                    // On envoie le signal d'interruption
                    if (childrenPid > 0) {
                        kill(childrenPid, SIGINT);
                    }
                }
#ifdef DEBUG_SIGNAL
                log_info("send signal to child done\n");
#endif // DEBUG_SIGNAL
            }

            int cptloop = 0;
            int remaining;
            // Escalade SIGTERM/SIGKILL si le SIGINT initial ne suffit pas —
            // même barème que `orchestrator_do_stop_forks` (stopForks/
            // configApply, cf. fork_orchestrator.h), qui ESCALADE déjà pour
            // exactement cette raison : sans elle, cette boucle attendait
            // INDÉFINIMENT (aucune borne de temps, contrairement à la
            // séquence d'arrêt de l'orchestrateur) un fork qui, pour
            // n'importe quelle raison, ne réagit pas au SIGINT — observé en
            // CI sur `run_client_lifecycle.sh` (`exit` n'aboutissant jamais,
            // le script de test finissant par tuer les process au bout de
            // 60s). `exit` DOIT terminer le programme, jamais rester bloqué
            // à attendre un fils récalcitrant.
            //
            // Escalade INDIVIDUELLE, PAR FILS (child_idle_ms), pas un délai
            // unique appliqué à tout le lot depuis le SIGINT initial : un fils
            // encore en train de vider sa file d'acquittements en attente, ou
            // de renvoyer son stock local restant (feed_thread_aposs /
            // bt_flush_pending, après REQUEST_STOP — cf. server_io_active /
            // fork_last_activity) ne doit pas être interrompu au milieu de ce
            // vidage juste parce qu'un AUTRE fils, lui, est réellement bloqué.
            // Un fils qui ne rapporte JAMAIS
            // d'activité (ancien client sans cette instrumentation, ou mort
            // avant son premier rapport) reste soumis à l'escalade normale —
            // `child_idle_ms` compte alors son inactivité depuis
            // `escalation_start`, exactement le comportement d'avant ce
            // suivi par fils.
            time_t escalation_start = time(NULL);
            stop_escalation_action_t *last_escalation =
                (childrens_pid != NULL)
                    ? calloc((size_t)NB_THREADS, sizeof(stop_escalation_action_t))
                    : NULL;
            // On attend que tous les enfants soient réellement terminés.
            // kill(pid, 0) renvoie 0 tant que le process existe, -1 (ESRCH)
            // une fois qu'il a été récolté par wait_child / sigchld_handler.
            do {
                remaining = 0;
                time_t now = time(NULL);
                if (childrens_pid != NULL) {
                    for (int c = 0; c < NB_THREADS; c++) {
                        pid_t childrenPid = childrens_pid[c];
                        if (childrenPid <= 0 || kill(childrenPid, 0) != 0) {
                            continue;
                        }
                        remaining++;

                        time_t last_seen = (fork_last_activity != NULL) ? fork_last_activity[c] : 0;
                        long idle_ms = child_idle_ms(last_seen, escalation_start, now);
                        stop_escalation_action_t action = stop_escalation_next(idle_ms);
                        if (last_escalation != NULL && action != last_escalation[c]
                            && action != STOP_ESCALATION_NONE) {
                            int sig = (action == STOP_ESCALATION_SIGKILL) ? SIGKILL : SIGTERM;
                            char state_buf[160];
                            fork_diagnostic_summary(
                                (fork_statistics != NULL) ? &fork_statistics[c] : NULL,
                                last_seen != 0, current_fork_role(c) == FORK_ROLE_PRUNE,
                                state_buf, sizeof(state_buf));
                            log_error("exit : fils %d encore vivant après %lds d'inactivité (%s) — escalade %s\n",
                                      (int)childrenPid, idle_ms / 1000, state_buf,
                                      action == STOP_ESCALATION_SIGKILL ? "SIGKILL" : "SIGTERM");
                            kill(childrenPid, sig);
                        }
                        if (last_escalation != NULL) {
                            last_escalation[c] = action;
                        }
                    }
                }
                if (cptloop == 10) {
                    log_console("\r            ");
                    log_console("\r");
                    cptloop = 0;
                }
                log_console("*");
                flush_console();
                cptloop++;
                usleep(MICRO_SLEEP);
            } while (remaining > 0);
            free(last_escalation);
            log_console("\n");
            flush_console();
            exit(EXIT_SUCCESS);
        }
    } else  {
        exit(EXIT_SUCCESS);
    }
    
    return 0;
}

/**
 * @brief Cœur réentrant de `restore [fichier [fichier_analyse]]`, extrait de
 *        `restore_interpreter` pour être appelable sans passer par le
 *        curseur global `strtok` (cf. `admin_apply_privileged_command`, qui
 *        tokenise déjà la ligne via `strtok_r` et ne doit jamais toucher à
 *        l'état de `strtok` — même raison d'être que `admin_apply_remote_command`).
 *
 * @param file         Chemin du fichier de stock à restaurer.
 * @param analyse_file Chemin du fichier de possibilités analysées à restaurer.
 * @return             0 si la restauration a réussi, une valeur négative sinon.
 */
static int restore_apply(char *file, char *analyse_file) {
    log_info("start restore\n");

    // Suspension de la recherche pendant le remplacement du stock : sans cela,
    // les threads de recherche consomment et délèguent des possibilités au
    // milieu du vidage/réimport et mélangent ancien et nouvel état.
    int previous_request = request;
    if (previous_request == REQUEST_CONTINUE) {
        request = REQUEST_PAUSE;
        usleep(THREAD_MICRO_SLEEP);
    }

    // Fenêtre `maintenance` posée pour TOUTE la séquence (débordement disque
    // PUIS RAM) : `stock_spill_step` ne consulte que ce drapeau, jamais les
    // verrous par file que `restore()` pose/lève lui-même — sans cette
    // fenêtre, une éviction/un rechargement concurrent pourrait migrer une
    // possibilité au beau milieu du remplacement.
    datamanager_begin_maintenance();

    // Remise en place des segments de débordement EN PREMIER (« snapshot »
    // — même sous-répertoire que `backup_interpreter`/l'arrêt sur solution,
    // les deux seuls chemins qui écrivent les fichiers par défaut que
    // `restore` restaure ici) : un import qui déborde ensuite (configuration
    // changée, plafond RAM plus bas) COMPLÈTE ces segments au lieu de les
    // écraser — c'est cet ordre qui le garantit. Un `restore` d'un fichier
    // personnalisé (chemin explicite, hors convention par défaut) n'a pas de
    // cliché de débordement correspondant : no-op tolérant, RAM restaurée
    // quand même — limitation documentée.
    unsigned long long spill_restored = stock_spill_restore_snapshot("snapshot");

    // Correctif : `stock_spill_restore_snapshot` était tolérante par
    // construction (cliché absent/mal configuré → simplement rien restauré,
    // aucune erreur remontée) — un `--stock-spill-dir` oublié, différent de
    // celui utilisé à la sauvegarde, ou un cliché supprimé/corrompu
    // produisait donc une restauration RAPPORTÉE COMME RÉUSSIE mais en
    // réalité amputée du débordement, en silence (perte de possibilités
    // contraire au principe du projet : aucune perte tolérée sans plan de
    // secours. `<file>.spillcount`
    // (`datamanager_read_spillcount_sidecar`), écrit par `consistent_backup`
    // au moment de CETTE sauvegarde précise et donc indépendant du
    // répertoire de débordement (qui peut, lui, être absent/mal configuré à
    // la restauration), permet de le détecter : sa présence dit combien de
    // possibilités AURAIENT dû revenir. Absence tolérée (sauvegarde
    // antérieure à ce correctif, ou sans débordement actif ce jour-là) —
    // dans ce cas, rien à vérifier, pas une anomalie.
    unsigned long long spill_expected = 0;
    int spill_mismatch = 0;
    if (datamanager_read_spillcount_sidecar(file, &spill_expected) && spill_expected != spill_restored) {
        spill_mismatch = 1;
        log_error("restore : débordement disque INCOMPLET — %llu possibilité(s) attendue(s) "
                  "(déportées au moment de la sauvegarde de %s), %llu récupérée(s) depuis le "
                  "cliché de débordement (--stock-spill-dir absent/différent de celui utilisé à "
                  "la sauvegarde, ou cliché supprimé/corrompu ?) — %llu possibilité(s) "
                  "potentiellement perdue(s). La restauration continue (le stock résident "
                  "reste utilisable) mais est INCOMPLÈTE.\n",
                  spill_expected, file, spill_restored,
                  spill_expected > spill_restored ? spill_expected - spill_restored : 0);
    }

    // `core_result` (volet RAM stock+analysed) reste distinct de `result`
    // (retour de la fonction) : un débordement incomplet (spill_mismatch)
    // ne doit RIEN changer au chargement de best_board/known_clients
    // ci-dessous, qui n'a aucun rapport avec le débordement — seul un échec
    // RÉEL du volet RAM (core_result != 0) doit les sauter, comme avant ce
    // correctif.
    int core_result = restore(file);
    if (core_result != 0) {
        log_error("restore impossible (%s) : stock conservé\n", file);
    } else if (restore_analysed(analyse_file) != 0) {
        log_error("restore analysed impossible (%s) : files analysées conservées\n", analyse_file);
        core_result = -1;
    }

    datamanager_end_maintenance();
    // Non bloquant : un backup plus ancien peut ne pas avoir ce fichier (feature
    // ajoutée après coup) — le stock/analysed restaurés ci-dessus restent valides
    // sans lui, seule la représentation du meilleur plateau reste vide.
    if (core_result == 0) {
        if (best_board_load(&g_server_best_board, DEF_BEST_BOARD_FILE) != 0) {
            log_error("restore best board impossible (%s) : aucun plateau record connu\n", DEF_BEST_BOARD_FILE);
        } else {
            // Le stock restauré ne reflète que la profondeur du curseur des
            // possibilités en attente, pas le meilleur plateau jamais atteint :
            // sans cette resynchronisation, max_result affiché reste sous-évalué
            // par rapport au vrai record connu du serveur.
            uint16_t recorded = best_board_result(&g_server_best_board);
            if (recorded > max_result) {
                max_result = recorded;
            }
        }
        // Même tolérance que best_board ci-dessus : un backup plus ancien peut
        // ne pas avoir ce fichier (feature ajoutée après coup) sans que ça
        // remette en cause la restauration du stock déjà faite. Fusion
        // additive dans le registre en mémoire (voir la doc de
        // known_clients_registry_load) : ne repart jamais de zéro si des
        // clients sont déjà reconnectés au moment du restore.
        if (known_clients_registry_load(DEF_KNOWN_CLIENTS_FILE) != 0) {
            log_error("restore known clients impossible (%s) : cumul par machine reparti de zéro\n",
                      DEF_KNOWN_CLIENTS_FILE);
        }
    }

    // On ne reprend que si aucun arrêt n'a été demandé entre-temps
    if (request == REQUEST_PAUSE) {
        request = previous_request;
    }

    // Le résultat RENVOYÉ reflète l'ISSUE COMPLÈTE (RAM + débordement) —
    // jamais un succès si le débordement est resté incomplet, même quand le
    // volet RAM a, lui, parfaitement réussi (cf. la doc de spill_mismatch
    // plus haut) : c'est ce qui fait remonter l'échec jusqu'à l'appelant
    // (console, API HTTP admin) plutôt que de le taire.
    int result = (core_result == 0 && !spill_mismatch) ? 0 : -1;
    if (result == 0) {
        log_info("backup restore\n");
    }
    return result;
}

/** @brief Interpréteur de `restore [fichier [fichier_analyse]]` : restaure les possibilités depuis les fichiers `.back`. */
int restore_interpreter(void) {
    char *def_file = DEF_FILE;
    char *def_analyse_file = DEF_ANALYSE_FILE;
    char *arguments = strtok(NULL, " ");
    if (arguments != NULL) {
        def_file = arguments;
        arguments = strtok(NULL, " ");
        if (arguments != NULL) {
            def_analyse_file = arguments;
        }
    }
    return restore_apply(def_file, def_analyse_file);
}

/** @brief Interpréteur de `import` : importe les possibilités depuis les fichiers `.back` sans effacer les files actuelles. */
int import_interpreter(void) {
    char *def_file = DEF_FILE;
    char *def_analyse_file = DEF_ANALYSE_FILE;
    log_info("start import\n");
    import(NULL, def_file);
    import_analysed(def_analyse_file);
    log_info("backup restore\n");
    
    return 0;
}

/** @brief Interpréteur de `loadJson` : importe une possibilité depuis une chaîne JSON (stdin/clipboard). */
int loadjson_interpreter(void) {
    log_info("load from json\n");
    import_json();
    log_info("backup json\n");
    
    return 0;
}

/**
 * @brief Ouvre @p path en écriture pour un export console ; journalise un
 *        message homogène et clair en cas d'échec (le fichier n'existe pas
 *        forcément, le répertoire parent peut être en lecture seule, etc.).
 */
static FILE *open_export_file(const char *cmd, const char *path)
{
    FILE *out = fopen(path, "w");
    if (out == NULL) {
        log_error("%s : impossible d'ouvrir « %s » en écriture\n", cmd, path);
    }
    return out;
}

/**
 * @brief Referme un export ouvert par open_export_file et journalise le
 *        résultat : décompte des possibilités écrites en cas de succès,
 *        message d'erreur homogène sinon (export alors incomplet).
 */
static void close_export_file(const char *cmd, const char *path, FILE *out, int rc, size_t count)
{
    fclose(out);
    if (rc == 0) {
        log_info("%s : export : %zu possibilité%s écrite%s dans %s\n",
                  cmd, count, count != 1 ? "s" : "", count != 1 ? "s" : "", path);
    } else {
        log_error("%s : échec de l'écriture dans « %s » (export incomplet)\n", cmd, path);
    }
}

/**
 * @brief Interpréteur de `print [fichier]` : affiche l'état du data manager
 *        (files, tailles), ou exporte le dump JSON complet dans [fichier]
 *        si l'argument est fourni.
 */
int print_interpreter(void) {
    char *path = strtok(NULL, " ");
    if (path == NULL) {
        return printdatamanager();
    }
    FILE *out = open_export_file("print", path);
    if (out == NULL) return -1;
    size_t count = 0;
    int rc = fprint_datamanager(out, &count);
    close_export_file("print", path, out, rc, count);
    return rc;
}

/** @brief Interpréteur de `sortDescMulti` (alias `sortdm`) : tri descendant multi-threadé des fichiers de possibilités. */
int sortdm_interpreter(void) {
    return sort_descending_mthread();
}

/** @brief Interpréteur de `split` : répartit les possibilités entre les différentes files. */
int split_interpreter(void) {
    return split_datas();
}

/** @brief Interpréteur de `regroup` : regroupe toutes les possibilités dans une seule file. */
int regroup_interpreter(void) {
    return regroup_datas();
}

/** @brief Interpréteur de `checkDatas` : vérifie la cohérence des possibilités stockées. */
int checkdatas_interpreter(void) {
    return check_datas();
}

/** @brief Interpréteur de `checkDuplicate` : recherche et supprime les doublons dans les files. */
int check_duplicate_interpreter(void) {
    return check_duplicate();
}
/** @brief Interpréteur de `statistic` : affiche les statistiques détaillées des données. */
int statistic_interpreter(void) {
    int rc = statistic_datas();

    // Métriques de besoin par rôle : core/ ne peut pas les afficher lui-même
    // (etii_server.h/control_registry.h sont app/), d'où leur ajout ICI
    // plutôt que dans statistic_datas().
    // Toujours à 0 sur un process client/pruner (aucune connexion de contrôle
    // entrante ni service INST_GET côté client) — comme `clients`/`knownClients`,
    // ces compteurs n'ont de sens que côté serveur, sans garde explicite.
    log_info("service a vide (depuis le demarrage) : recherche=%llu controle=%llu\n",
              server_search_starved, server_prune_starved);

    control_session_info_t sessions[MAX_CONTROL_SESSIONS];
    int n = control_registry_snapshot(sessions, MAX_CONTROL_SESSIONS);
    int nb_search = 0, nb_prune = 0;
    control_registry_count_roles(sessions, n, &nb_search, &nb_prune);
    log_info("parc connecte : %d recherche(s), %d controle(s)\n", nb_search, nb_prune);

    return rc;
}

/**
 * @brief Interpréteur de `checkOrigin [purge]` : vérifie qu'aucune possibilité
 *        en stock n'est la racine d'une autre.
 *
 * L'avertissement sur le stock débordé est émis ICI et non dans `check_origin` :
 * `core/datamanager.c` n'a pas le droit de dépendre de `core/stock_spill.c`
 * (l'inverse seul est permis).
 */
int check_origin_interpreter(void) {
    char *arguments = strtok(NULL, " ");
    int purge = 0;
    if (arguments != NULL) {
        // Seul « purge » est reconnu : une faute de frappe ne doit pas être
        // silencieusement dégradée en simple rapport.
        if (strcmp(arguments, "purge") != 0) {
            return CMD_ERR_USAGE;
        }
        purge = 1;
    }
    unsigned long long spilled = stock_spill_total_packets();
    if (spilled > 0) {
        log_info("checkOrigin : %llu possibilites debordees sur disque ne sont PAS balayees\n", spilled);
    }
    return check_origin(purge);
}

/** @brief Interpréteur de `checkFiles` : vérifie la cohérence de toutes les files de possibilités. */
int checkfiles_interpreter(void) {
    return check_files();
}

/**
 * @brief Interpréteur de `printFile <n> [fichier]` : affiche le contenu de la
 *        file de possibilités numéro <n>, ou l'exporte en JSON dans [fichier]
 *        si l'argument est fourni. <n> reste obligatoire.
 */
int printfile_interpreter(void) {
    char *arguments = strtok(NULL, " ");
    if (arguments == NULL) {
        return CMD_ERR_USAGE;
    }
    int n = atoi(arguments);
    char *path = strtok(NULL, " ");
    if (path == NULL) {
        return print_file(n);
    }
    FILE *out = open_export_file("printFile", path);
    if (out == NULL) return -1;
    size_t count = 0;
    int rc = fprint_file(out, n, &count);
    close_export_file("printFile", path, out, rc, count);
    return rc;
}

/** @brief Interpréteur de `checkFile <n>` : vérifie la cohérence du fichier de possibilités numéro n. */
int checkfile_interpreter(void) {
    char *arguments = strtok(NULL, " ");
    if (arguments != NULL) {
        return check_file(atoi(arguments));
    }

    return CMD_ERR_USAGE;
}

/** @brief Interpréteur de `checkDirections` : vérifie la cohérence du tableau de traversée `directions`. */
int checkdirections_interpreter(void) {
    if(test_directions() == 0)
    {
        log_info("directions : ok\n");
    } else
    {
        log_info("directions : NOK !\n");
    }
    
    return 0;
}

/** @brief Interpréteur de `removeNoNext` (alias `rmnonext`, `prune`) : supprime les possibilités sans continuation valide. */
int rmnonext_interpreter(void) {
    struct array_part *apart= read_parts(parts_files);

    struct array_part *rotateParts = rotate_all_parts(apart);
    map_big_array *map_parts = prepare_map_part(rotateParts);
    remove_possibilities_with_no_next(map_parts, rotateParts);
    free_bigarray(map_parts);
    free_array_part(rotateParts);
    free_array_part(apart);

    return 0;
}

/**
 * @brief Commande `expand <niveau>` : développe le stock du serveur jusqu'au
 *        niveau de curseur demandé (anti-famine, cf. expand_datas_to_level).
 *
 * Reconstruit la map depuis `parts_files` (comme `removeNoNext` : le serveur libère
 * la sienne après l'expansion de démarrage), la passe à `expand_datas_to_level`,
 * puis libère tout. Utile à chaud quand le stock distribuable s'est raréfié.
 */
int expand_interpreter(void) {
    char *arguments = strtok(NULL, " ");
    if (arguments == NULL) {
        return CMD_ERR_USAGE;
    }
    int level = atoi(arguments);
    if (level <= 0) {
        log_error("expand : niveau invalide (\"%s\") — attendu un entier > 0\n", arguments);
        return -1;
    }

    struct array_part *apart = read_parts(parts_files);
    struct array_part *rotateParts = rotate_all_parts(apart);
    map_big_array *map_parts = prepare_map_part(rotateParts);
    expand_datas_to_level(level, map_parts, rotateParts);
    free_bigarray(map_parts);
    free_array_part(rotateParts);
    free_array_part(apart);

    return 0;
}

/**
 * @brief Voir la doc dans command_lines.h.
 */
int admin_pause_transition(int current, int want_pause) {
    if (want_pause) {
        if (current == REQUEST_CONTINUE || current == REQUEST_PAUSE) {
            return REQUEST_ADMIN_PAUSE;
        }
        // Déjà en pause admin, ou en arrêt : inchangé.
        return current;
    }
    if (current == REQUEST_ADMIN_PAUSE) {
        return REQUEST_CONTINUE;
    }
    // Pas en pause admin (continue, pause de régulation, ou arrêt) : inchangé.
    return current;
}

/**
 * @brief Interpréteur de `pause` : pose une pause administrative (`REQUEST_ADMIN_PAUSE`)
 *        ET diffuse `CTRL_COMMAND "pause"` à toutes les sessions de contrôle actives.
 *
 * Contrairement à `REQUEST_PAUSE` (régulation de débit, levée automatiquement
 * par `control_step`), cette pause ne peut être levée que par la commande
 * `resume` — cf. la note dans core_static_variables.h. No-op côté local si déjà en
 * pause admin ou si le processus est en cours d'arrêt (`REQUEST_STOP`).
 *
 * Le volet diffusion (`control_registry_broadcast_command`) est ce qui rend
 * cette commande utile sur le SERVEUR : celui-ci ne lance jamais lui-même de
 * recherche (`request` n'y est jamais consulté par `autosearch`), donc une
 * pause purement locale y serait un no-op. En diffusant systématiquement —
 * sans distinguer serveur/client — la commande reste correcte des deux côtés :
 * sur un client, `control_registry` est toujours vide (rempli uniquement côté
 * serveur par `INST_CONTROL_HELLO`), donc la diffusion y est un no-op
 * silencieux (`n == 0`). Ancien comportement de `clientsPause`, désormais
 * fusionné ici (y compris la persistance de l'état désiré pour les futurs
 * clients, `control_registry_desired_pause_state`).
 */
int pause_interpreter(void) {
    int previous = request;
    int updated = admin_pause_transition(previous, 1);
    request = updated;
    if (updated == REQUEST_ADMIN_PAUSE && previous != REQUEST_ADMIN_PAUSE) {
        log_event("pause administrative demandée : recherche mise en pause\n");
    } else if (previous == REQUEST_ADMIN_PAUSE) {
        log_info("pause : déjà en pause administrative\n");
    } else {
        log_error("pause : impossible (arrêt en cours)\n");
    }
    int n = control_registry_broadcast_command(CTRL_COMMAND, "pause");
    if (n > 0) {
        log_info("pause : diffusée à %d session(s) client(s)\n", n);
    }
    return 0;
}

/**
 * @brief Interpréteur de `resume` : lève une pause administrative (`REQUEST_ADMIN_PAUSE`)
 *        ET diffuse `CTRL_COMMAND "resume"` à toutes les sessions de contrôle actives.
 *
 * No-op côté local si le processus n'est pas en pause administrative (par ex.
 * déjà en fonctionnement normal, ou en pause de régulation de débit — laissée
 * à `control_step`). Voir `pause_interpreter` pour le volet diffusion, fusionné
 * depuis l'ancien `clientsResume`.
 */
int resume_interpreter(void) {
    int previous = request;
    int updated = admin_pause_transition(previous, 0);
    request = updated;
    if (updated == REQUEST_CONTINUE && previous == REQUEST_ADMIN_PAUSE) {
        log_event("pause administrative levée : reprise de la recherche\n");
    } else {
        log_info("resume : pas de pause administrative en cours\n");
    }
    int n = control_registry_broadcast_command(CTRL_COMMAND, "resume");
    if (n > 0) {
        log_info("resume : diffusée à %d session(s) client(s)\n", n);
    }
    return 0;
}

/**
 * @brief Portion "clientsCommand"/"clientsCmd" [--to <cible>] <ligne...> de
 *        `admin_apply_remote_command`, réentrante (aucun strtok global).
 *
 * `rest` est le reliquat de ligne laissé par `strtok_r` juste après le mot
 * "clientsCommand"/"clientsCmd" -- toujours un pointeur dans le tampon
 * modifiable de l'appelant, jamais retokenisé au delà de la recherche de
 * "--to". Même format et mêmes règles que `clients_cmd_interpreter` (cf. sa
 * documentation) : sans "--to", diffusion à toutes les sessions actives ;
 * avec "--to <cible>", une seule session ciblée (`session_no`, `client_uid`
 * ou `label`) ; dans les deux cas, la commande poussée est vérifiée par
 * `control_command_allowed` avant tout envoi -- cibler une session n'élargit
 * jamais le jeu de commandes autorisées.
 */
static int admin_remote_clients_command(char *rest) {
    while (rest != NULL && *rest == ' ') {
        rest++;
    }
    if (rest == NULL || *rest == '\0') {
        return ADMIN_CMD_BAD_ARGS;
    }

    const char *target = NULL;
    if (strncmp(rest, "--to ", 5) == 0) {
        rest += 5;
        while (*rest == ' ') {
            rest++;
        }
        char *sep = strchr(rest, ' ');
        if (sep == NULL) {
            /* "--to <cible>" sans commande derrière : rien à envoyer. */
            return ADMIN_CMD_BAD_ARGS;
        }
        *sep = '\0';
        target = rest;
        rest = sep + 1;
        while (*rest == ' ') {
            rest++;
        }
        if (*rest == '\0') {
            return ADMIN_CMD_BAD_ARGS;
        }
    }

    if (!control_command_allowed(rest)) {
        return ADMIN_CMD_FORBIDDEN;
    }

    if (target != NULL) {
        int posted = control_registry_send_command_to(target, CTRL_COMMAND, rest);
        return (posted == 1) ? ADMIN_CMD_OK : ADMIN_CMD_BAD_ARGS;
    }

    control_registry_broadcast_command(CTRL_COMMAND, rest);
    return ADMIN_CMD_OK;
}

/**
 * @brief Portion "clientsRoles [--to <cible>] <nb_pruner>" de
 *        `admin_apply_remote_command`, réentrante (aucun strtok global) --
 *        pendant HTTP de `clients_roles_interpreter`, même syntaxe.
 *
 * La composition ("config pruner_forks <n>" + "configApply") et la
 * mémorisation du dosage désiré par machine sont entièrement déléguées à
 * `control_registry_apply_role_dosage` -- rien à revalider ici, les deux
 * commandes composées sont fixes et déjà dans la liste blanche.
 */
static int admin_remote_clients_roles(char *rest) {
    while (rest != NULL && *rest == ' ') {
        rest++;
    }
    if (rest == NULL || *rest == '\0') {
        return ADMIN_CMD_BAD_ARGS;
    }

    const char *target = NULL;
    if (strncmp(rest, "--to ", 5) == 0) {
        rest += 5;
        while (*rest == ' ') {
            rest++;
        }
        char *sep = strchr(rest, ' ');
        if (sep == NULL) {
            /* "--to <cible>" sans nombre derrière : rien à envoyer. */
            return ADMIN_CMD_BAD_ARGS;
        }
        *sep = '\0';
        target = rest;
        rest = sep + 1;
        while (*rest == ' ') {
            rest++;
        }
        if (*rest == '\0') {
            return ADMIN_CMD_BAD_ARGS;
        }
    }

    char *end = NULL;
    long n = strtol(rest, &end, 10);
    if (end == rest || *end != '\0' || n < 0 || n > INT_MAX) {
        return ADMIN_CMD_BAD_ARGS;
    }

    int touched = control_registry_apply_role_dosage(target, (int)n);
    if (target != NULL && touched != 1) {
        return ADMIN_CMD_BAD_ARGS;
    }
    return ADMIN_CMD_OK;
}

/**
 * @brief Formate en texte compact `fork_seq=rôle ...` le rôle déclaré de
 *        chaque fork de travail actuellement connecté d'un client
 *        (`client_work_fork_roles`) — jusqu'ici la seule
 *        façon de voir ce dosage était le tableau « Thread queues » local du
 *        client concerné (commande `check`), jamais depuis le serveur.
 *        Partagé entre `clients_work_interpreter` et
 *        `admin_remote_clients_work` (un seul point à toucher).
 *
 * `CLIENT_MODE_GPU_PRUNER` s'affiche `prune-gpu`, distinct de `prune` — ce
 * détail n'a pas sa place dans `fork_role_t` (recherche/contrôle, binaire)
 * mais reste visible ici puisque l'identité déclarée le porte déjà.
 */
static void format_client_work_fork_roles(const uint8_t client_uid[CLIENT_UID_BYTES],
                                          char *buf, size_t bufsize) {
    int cap = (NB_THREADS > 0) ? NB_THREADS : 1;
    client_work_fork_t *roles = calloc((size_t)cap, sizeof *roles);
    int n = client_work_fork_roles(client_uid, roles, cap);
    if (n == 0) {
        snprintf(buf, bufsize, "aucun fork de travail connecté");
        free(roles);
        return;
    }
    int off = 0;
    for (int i = 0; i < n && off < (int)bufsize; i++) {
        const char *role = (roles[i].mode == CLIENT_MODE_SEARCH) ? "search"
                          : (roles[i].mode == CLIENT_MODE_PRUNER) ? "prune"
                          : "prune-gpu";
        off += snprintf(buf + off, bufsize - off, "%s%d=%s", (i > 0) ? " " : "", roles[i].fork_seq, role);
    }
    free(roles);
}

/**
 * @brief Portion "clientsWork <cible>" de `admin_apply_remote_command`,
 *        réentrante (aucun strtok global).
 *
 * Lecture pure (rien n'est envoyé à un client) : résout `target` vers un
 * `client_uid` exactement comme `clients_work_interpreter`
 * (`control_registry_resolve_client_uid`), puis journalise l'attribution déjà
 * enregistrée côté serveur (`datamanager_analysed_owned_by`). `POST
 * /api/v1/command` répond uniquement `{"result":"ok"}` (pas de canal pour
 * renvoyer une donnée dans le corps de la réponse) : comme pour `clientsStats`
 * via cette même API, le résultat de la consultation est à lire dans les
 * journaux du serveur.
 */
static int admin_remote_clients_work(char *target) {
    if (target == NULL || *target == '\0') {
        return ADMIN_CMD_BAD_ARGS;
    }

    uint8_t client_uid[CLIENT_UID_BYTES];
    if (control_registry_resolve_client_uid(target, client_uid) != 1) {
        return ADMIN_CMD_BAD_ARGS;
    }

    unsigned long long count = 0;
    int max_alloc = -1;
    datamanager_analysed_owned_by(client_uid, &count, &max_alloc);

    char client_uid_hex[2 * CLIENT_UID_BYTES + 1];
    client_identity_hex_encode(client_uid, CLIENT_UID_BYTES, client_uid_hex, sizeof(client_uid_hex));
    char fork_roles[512];
    format_client_work_fork_roles(client_uid, fork_roles, sizeof fork_roles);
    log_info("clientsWork (API HTTP admin) : %s (client_uid=%s) : %llu possibilite(s) en cours d'analyse, alloc max=%d ; forks: %s\n",
              target, client_uid_hex, count, max_alloc, fork_roles);
    return ADMIN_CMD_OK;
}

/**
 * @brief Portion "config [<clé> <valeur>]" de `admin_apply_remote_command`,
 *        réentrante (aucun strtok global) — pendant HTTP/canal de contrôle de
 *        `config_interpreter`, qui lui tokenise via le curseur global `strtok`
 *        et ne doit donc JAMAIS être appelé depuis ce chemin.
 *
 * `rest` est le reliquat de ligne laissé par `strtok_r` juste après le mot
 * "config" -- toujours un pointeur dans le tampon modifiable de l'appelant.
 * En PRODUCTION, `POST /api/v1/command` n'est atteignable que côté serveur
 * (`server` y vaut toujours 1) ; cette fonction branche néanmoins sur `server`
 * comme `config_interpreter` (plutôt que de le supposer), pour rester
 * testable dans les deux contextes et cohérente si jamais réutilisée
 * ailleurs :
 *
 * - SERVEUR, sans argument : affiche la configuration SERVEUR effective
 *   (`server_config_capture_effective`). Avec "<clé> <valeur>" : refusée
 *   (`ADMIN_CMD_FORBIDDEN`) — le serveur n'a pas de configuration "en
 *   préparation" à appliquer à chaud.
 * - CLIENT/PRUNER, sans argument : journalise l'état de l'orchestrateur, la
 *   configuration effective et la configuration en préparation (même contenu
 *   que `config_interpreter`), n'annule PAS le décompte. Avec "<clé> <valeur>"
 *   (un seul token par valeur, même convention que `limit`/`prunerBatch`) :
 *   écrit dans la configuration en préparation via
 *   `fork_orchestrator_stage_config_line` et annule le décompte SEULEMENT si
 *   la ligne est acceptée -- une commande mal formée ne doit pas coûter
 *   l'auto-démarrage.
 */
static int admin_remote_config(char *rest) {
    while (rest != NULL && *rest == ' ') {
        rest++;
    }

    if (server) {
        if (rest == NULL || *rest == '\0') {
            server_config_t cfg;
            server_config_capture_effective(&cfg);
            char buf[1024];
            server_config_format(&cfg, buf, sizeof(buf));
            server_config_free(&cfg);

            log_info("config (API HTTP admin) : configuration effective (fichier : %s) :\n%s",
                      server_config_file_path,
                      buf[0] != '\0' ? buf : "  (aucune valeur)\n");
            return ADMIN_CMD_OK;
        }

        log_error("config (API HTTP admin) : \"config <clé> <valeur>\" n'est pas supporté en mode "
                  "serveur (pas de configuration \"en préparation\" à appliquer à chaud) -- éditez "
                  "le fichier %s puis redémarrez, ou utilisez configSave pour figer l'état courant\n",
                  server_config_file_path);
        return ADMIN_CMD_FORBIDDEN;
    }

    if (rest == NULL || *rest == '\0') {
        orch_state_t state;
        long countdown_remaining_ms;
        fork_orchestrator_snapshot(&state, &countdown_remaining_ms);

        client_config_t cfg;
        client_config_capture_effective(&cfg, g_client_server_host);
        char buf[1024];
        client_config_format(&cfg, buf, sizeof(buf));
        client_config_free(&cfg);

        char staged_buf[1024];
        fork_orchestrator_format_staged_config(staged_buf, sizeof(staged_buf));

        log_info("config (API HTTP admin) : état=%s\n"
                  "configuration effective (fichier : %s) :\n%s"
                  "configuration en préparation :\n%s",
                  orch_state_name(state), client_config_file_path,
                  buf[0] != '\0' ? buf : "  (aucune valeur)\n",
                  staged_buf[0] != '\0' ? staged_buf : "  (aucune valeur)\n");
        return ADMIN_CMD_OK;
    }

    char *sep = strchr(rest, ' ');
    if (sep == NULL) {
        /* Clé seule, sans valeur : rien à préparer. */
        return ADMIN_CMD_BAD_ARGS;
    }
    *sep = '\0';
    const char *key = rest;
    char *value = sep + 1;
    while (*value == ' ') {
        value++;
    }
    if (*value == '\0') {
        return ADMIN_CMD_BAD_ARGS;
    }
    char *value_end = strchr(value, ' ');
    if (value_end != NULL) {
        *value_end = '\0';
    }

    char line[600];
    snprintf(line, sizeof(line), "%s = %s", key, value);
    client_config_line_status_t status = fork_orchestrator_stage_config_line(line);
    switch (status) {
    case CLIENT_CONFIG_LINE_SET:
        log_info("config (API HTTP admin) : \"%s\" préparé (%s = %s) — décompte annulé s'il était en cours\n",
                  key, key, value);
        return ADMIN_CMD_OK;
    case CLIENT_CONFIG_LINE_IGNORED:
        return ADMIN_CMD_OK;
    case CLIENT_CONFIG_LINE_UNKNOWN_KEY:
    case CLIENT_CONFIG_LINE_INVALID_VALUE:
        return ADMIN_CMD_BAD_ARGS;
    }
    return ADMIN_CMD_BAD_ARGS;
}

/**
 * @brief "start"/"stopForks"/"configApply" agissent sur le cycle de vie de
 *        fils CLIENT (`fork_orchestrator`), qui ne veut rien dire côté
 *        SERVEUR -- même raisonnement que `command_is_client_only` pour la
 *        console (voir sa doc), mais appliqué ICI parce que
 *        `admin_apply_remote_command` (contrairement à `do_command_line`) ne
 *        consulte jamais `command_is_client_only`. `POST /api/v1/command`
 *        (`src/net/http_server.c`, seul appelant de cette fonction en dehors
 *        des tests) n'est atteignable QUE depuis `runserver`
 *        (`src/app/etii_server.c`) : `server` y vaut donc toujours 1.
 *
 * "config"/"configSave" n'en font PLUS partie : `admin_remote_config`
 * (ci-dessous) et `config_save_interpreter` branchent désormais eux-mêmes sur
 * `server` — seule la forme "config <clé> <valeur>" reste refusée
 * (`ADMIN_CMD_FORBIDDEN`, pas de configuration "en préparation" côté serveur).
 */
static int admin_remote_command_is_client_only(const char *word) {
    return strcmp(word, "start") == 0 ||
           strcmp(word, "stopForks") == 0 ||
           strcmp(word, "configApply") == 0;
}

/**
 * @brief Voir la doc dans command_lines.h.
 */
int admin_apply_remote_command(const char *line) {
    if (!control_command_allowed(line)) {
        return ADMIN_CMD_FORBIDDEN;
    }

    size_t length = strlen(line) + 1;
    char *copy = malloc(sizeof(char) * length);
    memcpy(copy, line, length);

    char *save = NULL;
    char *word = strtok_r(copy, " ", &save);
    int result = ADMIN_CMD_BAD_ARGS;
    if (word != NULL) {
        if (server && admin_remote_command_is_client_only(word)) {
            result = ADMIN_CMD_FORBIDDEN;
        } else if (strcmp(word, "clientsCommand") == 0 || strcmp(word, "clientsCmd") == 0) {
            result = admin_remote_clients_command(save);
        } else if (strcmp(word, "clientsRoles") == 0) {
            result = admin_remote_clients_roles(save);
        } else if (strcmp(word, "clientsWork") == 0) {
            char *target = strtok_r(NULL, " ", &save);
            result = admin_remote_clients_work(target);
        } else if (strcmp(word, "config") == 0) {
            result = admin_remote_config(save);
        } else if (strcmp(word, "start") == 0) {
            // start_interpreter ne touche jamais strtok : appelable
            // directement, comme backup_interpreter (admin_apply_privileged_command).
            result = (start_interpreter() == 0) ? ADMIN_CMD_OK : ADMIN_CMD_BAD_ARGS;
        } else if (strcmp(word, "stopForks") == 0) {
            result = (stop_forks_interpreter() == 0) ? ADMIN_CMD_OK : ADMIN_CMD_BAD_ARGS;
        } else if (strcmp(word, "configApply") == 0) {
            result = (config_apply_interpreter() == 0) ? ADMIN_CMD_OK : ADMIN_CMD_BAD_ARGS;
        } else if (strcmp(word, "configSave") == 0) {
            result = (config_save_interpreter() == 0) ? ADMIN_CMD_OK : ADMIN_CMD_BAD_ARGS;
        } else {
            char *arg = strtok_r(NULL, " ", &save);
            if (strcmp(word, "pause") == 0) {
                request = admin_pause_transition(request, 1);
                control_registry_broadcast_command(CTRL_COMMAND, "pause");
                result = ADMIN_CMD_OK;
            } else if (strcmp(word, "resume") == 0) {
                request = admin_pause_transition(request, 0);
                control_registry_broadcast_command(CTRL_COMMAND, "resume");
                result = ADMIN_CMD_OK;
            } else if (strcmp(word, "limit") == 0 && arg != NULL) {
                max_search_by_sec = atoi(arg);
                result = ADMIN_CMD_OK;
            } else if (strcmp(word, "maxStockByThread") == 0 && arg != NULL) {
                max_stock_by_thread = atoi(arg);
                result = ADMIN_CMD_OK;
            } else if (strcmp(word, "prunerBatch") == 0 && arg != NULL) {
                pruner_batch_size = pruner_batch_clamp(atoi(arg));
                result = ADMIN_CMD_OK;
            } else if (strcmp(word, "prunerDfsBudget") == 0 && arg != NULL) {
                pruner_dfs_budget = pruner_dfs_budget_clamp(atoi(arg));
                result = ADMIN_CMD_OK;
            }
        }
    }

    free(copy);
    return result;
}

/**
 * @brief Voir la doc dans command_lines.h.
 */
int admin_apply_privileged_command(const char *line) {
    if (control_command_allowed(line)) {
        // Commande "standard" (pause/resume/limit/...) : identique, authentifiée
        // ou non, au chemin déjà validé par admin_apply_remote_command. Pas de
        // duplication de logique ici.
        return admin_apply_remote_command(line);
    }
    if (!control_command_privileged(line)) {
        return ADMIN_CMD_FORBIDDEN;
    }

    // À ce stade, line est reconnue par control_command_privileged (restore ou
    // backup) : l'appelant (handle_command_route, src/net/http_server.c) a déjà
    // vérifié le jeton Bearer AVANT cet appel — cette fonction n'authentifie rien
    // elle-même, elle exécute une commande déjà autorisée.
    size_t length = strlen(line) + 1;
    char *copy = malloc(sizeof(char) * length);
    memcpy(copy, line, length);

    char *save = NULL;
    char *word = strtok_r(copy, " ", &save);
    int result = ADMIN_CMD_BAD_ARGS;
    if (word != NULL) {
        if (strcmp(word, "backup") == 0) {
            // backup_interpreter ne prend aucun argument et n'appelle jamais
            // strtok : réentrant tel quel (contrairement à restore_interpreter).
            backup_interpreter();
            result = ADMIN_CMD_OK;
        } else if (strcmp(word, "restore") == 0) {
            char *file = strtok_r(NULL, " ", &save);
            char *analyse_file = (file != NULL) ? strtok_r(NULL, " ", &save) : NULL;
            // Comme les autres commandes admin (pause/resume/limit/...), le
            // résultat HTTP reflète que la commande a été exécutée, pas que
            // restore() a trouvé un fichier valide : un échec est déjà journalisé
            // par restore_apply (log_error), au même niveau que backup_interpreter.
            restore_apply(file != NULL ? file : DEF_FILE,
                           analyse_file != NULL ? analyse_file : DEF_ANALYSE_FILE);
            result = ADMIN_CMD_OK;
        } else if (strcmp(word, "sortAsc") == 0) {
            // sort_ascending()/sort_descending()/sort_descending_mthread()/
            // split_datas()/regroup_datas() ne touchent jamais strtok : appelés
            // directement (pas via leur *_interpreter, qui pour sortDesc lit un
            // argument optionnel via le curseur global strtok) pour rester
            // réentrant, même raison que backup_interpreter/restore_apply ci-dessus.
            sort_ascending();
            result = ADMIN_CMD_OK;
        } else if (strcmp(word, "sortAscFiles") == 0) {
            sort_ascending_files();
            result = ADMIN_CMD_OK;
        } else if (strcmp(word, "sortDesc") == 0) {
            char *arg = strtok_r(NULL, " ", &save);
            if (arg != NULL) {
                int n_file = atoi(arg);
                sort_d_mono(&n_file);
            } else {
                sort_descending();
            }
            result = ADMIN_CMD_OK;
        } else if (strcmp(word, "sortDescFiles") == 0) {
            sort_descending_files();
            result = ADMIN_CMD_OK;
        } else if (strcmp(word, "sortDescMulti") == 0) {
            sort_descending_mthread();
            result = ADMIN_CMD_OK;
        } else if (strcmp(word, "split") == 0) {
            split_datas();
            result = ADMIN_CMD_OK;
        } else if (strcmp(word, "regroup") == 0) {
            regroup_datas();
            result = ADMIN_CMD_OK;
        } else if (strcmp(word, "rebalance") == 0) {
            char *arg = strtok_r(NULL, " ", &save);
            int budget = rebalance_budget;
            if (arg != NULL) {
                int n = atoi(arg);
                if (n > 0) { budget = n; }
            }
            datamanager_rebalance_step(budget);
            result = ADMIN_CMD_OK;
        } else if (strcmp(word, "stockMaxRam") == 0) {
            // stock_max_ram_interpreter lit aussi <mo> via le curseur global
            // strtok : appelé directement, pas via l'interpréteur, même
            // raison que sortDesc/rebalance ci-dessus. <mo> ABSENT reste un
            // usage invalide (ADMIN_CMD_BAD_ARGS) ; <mo> <= 0 fourni
            // explicitement désactive le plafond (illimité), même convention
            // que la commande console.
            char *arg = strtok_r(NULL, " ", &save);
            if (arg != NULL) {
                int mo = atoi(arg);
                stock_max_ram_mb = mo;
                datamanager_configure_ram_limit(mo);
                result = ADMIN_CMD_OK;
            }
        } else if (strcmp(word, "spill") == 0) {
            // spill_interpreter lit aussi <n> via le curseur global strtok :
            // appelé directement, même raison que stockMaxRam/rebalance
            // ci-dessus. <n> absent -> budget par défaut (comme la commande
            // console) ; <n> <= 0 fourni explicitement reste un usage
            // invalide -- result garde ADMIN_CMD_BAD_ARGS (sa valeur par
            // défaut), même convention que rebalance ci-dessus.
            char *arg = strtok_r(NULL, " ", &save);
            int budget = STOCK_SPILL_BLOCK_PACKETS;
            int valid = 1;
            if (arg != NULL) {
                int n = atoi(arg);
                if (n <= 0) {
                    valid = 0;
                } else {
                    budget = n;
                }
            }
            if (valid) {
                stock_spill_step(budget);
                result = ADMIN_CMD_OK;
            }
        }
    }

    free(copy);
    return result;
}

/**
 * @brief Interpréteur de `clients` : liste les sessions de contrôle actives
 *        (canal `INST_CONTROL_HELLO`, v9) via `control_registry_snapshot`.
 *
 * Commande SERVEUR pure (send_to_childs = 0) : les process forkés du client
 * ne connaissent pas ce registre.
 */
int clients_interpreter(void) {
    control_session_info_t infos[MAX_CONTROL_SESSIONS];
    int n = control_registry_snapshot(infos, MAX_CONTROL_SESSIONS);
    if (n == 0) {
        log_info("clients : aucune session de contrôle active\n");
        return 0;
    }
    log_info("clients : %d session(s) de contrôle active(s)\n", n);
    for (int i = 0; i < n; i++) {
        log_info("  #%llu  %s (%s)  pid=%d  mode=%u  forks=%d  machine_uid=%s  client_uid=%s  derniere activite=%lld\n",
                  (unsigned long long)infos[i].session_no,
                  infos[i].label[0] != '\0' ? infos[i].label : "?",
                  infos[i].peer_ip[0] != '\0' ? infos[i].peer_ip : "?",
                  infos[i].pid, (unsigned)infos[i].mode, infos[i].nb_forks,
                  infos[i].machine_uid_hex, infos[i].client_uid_hex,
                  (long long)infos[i].last_activity);
    }
    return 0;
}

/**
 * @brief Interpréteur de `clientsStats` : demande les statistiques agrégées
 *        (`CTRL_GET_STATS`) à toutes les sessions de contrôle actives.
 */
int clients_stats_interpreter(void) {
    int n = control_registry_broadcast_get_stats();
    log_info("clientsStats : demande envoyée à %d session(s)\n", n);
    return 0;
}

/**
 * @brief Interpréteur de `clientsCommand [--to <cible>] <ligne...>` (alias `clientsCmd`) :
 *        pousse une commande console à distance (`CTRL_COMMAND`), à une session de
 *        contrôle précise (`--to`) ou, par défaut, à toutes les sessions actives.
 *
 * `<ligne...>` est reprise TELLE QUELLE (pas retokenisée : elle peut contenir
 * plusieurs arguments, ex. "limit 500"). Avant tout envoi, son premier mot est
 * vérifié par `control_command_allowed` (liste blanche définie dans
 * control_protocol.h) : une commande non autorisée est refusée SANS être
 * envoyée, avec ou sans `--to` -- cibler une session n'élargit jamais le jeu
 * de commandes autorisées.
 *
 * `--to <cible>` doit précéder immédiatement la commande et ne doit pas
 * contenir d'espace (un `session_no` ou un `client_uid` hexadécimal n'en
 * contiennent jamais ; un `label` qui en contiendrait doit être ciblé
 * autrement). La résolution (`control_registry_send_command_to`) refuse une
 * cible inconnue/déconnectée ou ambiguë plutôt que de deviner un destinataire.
 */
int clients_cmd_interpreter(void) {
    char *rest = strtok(NULL, "");
    if (rest != NULL) {
        while (*rest == ' ') {
            rest++;
        }
    }
    if (rest == NULL || *rest == '\0') {
        return CMD_ERR_USAGE;
    }

    const char *target = NULL;
    if (strncmp(rest, "--to ", 5) == 0) {
        rest += 5;
        while (*rest == ' ') {
            rest++;
        }
        char *sep = strchr(rest, ' ');
        if (sep == NULL) {
            /* "--to <cible>" sans commande derrière : rien à envoyer. */
            return CMD_ERR_USAGE;
        }
        *sep = '\0';
        target = rest;
        rest = sep + 1;
        while (*rest == ' ') {
            rest++;
        }
        if (*rest == '\0') {
            return CMD_ERR_USAGE;
        }
    }

    if (!control_command_allowed(rest)) {
        log_error("clientsCommand : commande non autorisée à distance (liste blanche : pause, resume, limit, maxStockByThread, prunerBatch, prunerDfsBudget) : \"%s\"\n", rest);
        return -1;
    }

    if (target != NULL) {
        int posted = control_registry_send_command_to(target, CTRL_COMMAND, rest);
        if (posted != 1) {
            log_error("clientsCommand : cible \"%s\" introuvable, déconnectée ou ambiguë -- rien envoyé\n", target);
            return -1;
        }
        log_info("clientsCommand : \"%s\" envoyée à la cible \"%s\"\n", rest, target);
        return 0;
    }

    int n = control_registry_broadcast_command(CTRL_COMMAND, rest);
    log_info("clientsCommand : \"%s\" diffusée à %d session(s)\n", rest, n);
    return 0;
}

/**
 * @brief Interpréteur de `clientsRoles [--to <cible>] <nb_pruner>` : ergonomie
 *        composant `config pruner_forks <nb_pruner>` + `configApply`, déjà possible via
 *        deux `clientsCommand` séparés -- voir `control_registry_apply_role_dosage`
 *        pour la composition ET la mémorisation du dosage désiré par machine.
 *
 * Même syntaxe `--to` que `clientsCommand` (doit précéder immédiatement
 * `<nb_pruner>`, sans espace dans la cible). `<nb_pruner>` doit être un
 * entier décimal non négatif -- une valeur hors [0, nb_forks] de la cible est
 * clampée côté client (`resolve_pruner_forks`), jamais rejetée ici : le
 * serveur ne connaît pas le `nb_forks` de la cible.
 */
int clients_roles_interpreter(void) {
    char *rest = strtok(NULL, "");
    if (rest != NULL) {
        while (*rest == ' ') {
            rest++;
        }
    }
    if (rest == NULL || *rest == '\0') {
        return CMD_ERR_USAGE;
    }

    const char *target = NULL;
    if (strncmp(rest, "--to ", 5) == 0) {
        rest += 5;
        while (*rest == ' ') {
            rest++;
        }
        char *sep = strchr(rest, ' ');
        if (sep == NULL) {
            /* "--to <cible>" sans nombre derrière : rien à envoyer. */
            return CMD_ERR_USAGE;
        }
        *sep = '\0';
        target = rest;
        rest = sep + 1;
        while (*rest == ' ') {
            rest++;
        }
        if (*rest == '\0') {
            return CMD_ERR_USAGE;
        }
    }

    char *end = NULL;
    long n = strtol(rest, &end, 10);
    if (end == rest || *end != '\0' || n < 0 || n > INT_MAX) {
        return CMD_ERR_USAGE;
    }

    int touched = control_registry_apply_role_dosage(target, (int)n);
    if (target != NULL) {
        if (touched != 1) {
            log_error("clientsRoles : cible \"%s\" introuvable, déconnectée ou ambiguë -- rien envoyé\n", target);
            return -1;
        }
        log_info("clientsRoles : pruner_forks=%d envoyé à la cible \"%s\" (mémorisé pour les reconnexions futures)\n",
                  (int)n, target);
        return 0;
    }

    log_info("clientsRoles : pruner_forks=%d diffusé à %d session(s) (mémorisé pour les reconnexions futures)\n",
              (int)n, touched);
    return 0;
}

/**
 * @brief Interpréteur de `knownClients` : liste les machines connues du
 *        registre de cumul (`known_clients_registry.h`).
 *
 * Contrairement à `clients` (sessions ACTUELLEMENT actives), cette liste
 * inclut aussi les machines déconnectées depuis le démarrage du serveur,
 * jusqu'à ce que la borne du registre (MAX_KNOWN_CLIENTS) impose leur
 * éviction. Commande SERVEUR pure (send_to_childs = 0).
 */
int known_clients_interpreter(void) {
    known_client_info_t infos[MAX_KNOWN_CLIENTS];
    int n = known_clients_registry_snapshot(infos, MAX_KNOWN_CLIENTS);
    if (n == 0) {
        log_info("knownClients : aucune machine connue\n");
        return 0;
    }
    log_info("knownClients : %d machine(s) connue(s)\n", n);
    for (int i = 0; i < n; i++) {
        log_info("  %s  %s (%s)  %s  sessions actives=%d (recherche=%d controle=%d)  connexions=%d  "
                  "pruner checked=%llu removed=%llu  record=%llu  derniere activite=%lld\n",
                  infos[i].machine_uid_hex,
                  infos[i].label[0] != '\0' ? infos[i].label : "?",
                  infos[i].peer_ip[0] != '\0' ? infos[i].peer_ip : "?",
                  infos[i].connected ? "connecte" : "deconnecte",
                  infos[i].nb_active_sessions, infos[i].nb_active_search, infos[i].nb_active_prune,
                  infos[i].nb_connections_total,
                  (unsigned long long)infos[i].total_pruner_checked,
                  (unsigned long long)infos[i].total_pruner_removed,
                  (unsigned long long)infos[i].best_max_result,
                  (long long)infos[i].last_seen);
    }
    return 0;
}

/**
 * @brief Interpréteur de `clientsWork <cible>` : consultation « que travaille
 *        X ? » (attribution des analyses en cours).
 *
 * `<cible>` est résolue vers un `client_uid` exactement comme `clientsCommand
 * --to` (`session_no`, `client_uid` hexadécimal, ou `label` déclaré — cf.
 * `control_registry_resolve_client_uid`) : une cible inconnue, déconnectée ou
 * ambiguë (label partagé) est refusée plutôt que de deviner. Contrairement à
 * `clientsCommand`, cette commande ne pousse rien au client : elle lit
 * uniquement l'attribution que LE SERVEUR a lui-même enregistrée en servant
 * ce client (`INST_GET`/`INST_GET_TO_CHECK[_BATCH]`), donc le résultat reflète
 * le point de vue serveur, jamais un aller-retour réseau vers le client.
 * Commande SERVEUR pure (send_to_childs = 0) : côté client, `control_registry`
 * est toujours vide, la résolution échouerait systématiquement.
 */
int clients_work_interpreter(void) {
    char *target = strtok(NULL, " ");
    if (target == NULL || *target == '\0') {
        return CMD_ERR_USAGE;
    }

    uint8_t client_uid[CLIENT_UID_BYTES];
    int resolved = control_registry_resolve_client_uid(target, client_uid);
    if (resolved != 1) {
        log_error("clientsWork : cible \"%s\" introuvable, déconnectée ou ambiguë\n", target);
        return -1;
    }

    unsigned long long count = 0;
    int max_alloc = -1;
    datamanager_analysed_owned_by(client_uid, &count, &max_alloc);

    char client_uid_hex[2 * CLIENT_UID_BYTES + 1];
    client_identity_hex_encode(client_uid, CLIENT_UID_BYTES, client_uid_hex, sizeof(client_uid_hex));

    char fork_roles[512];
    format_client_work_fork_roles(client_uid, fork_roles, sizeof fork_roles);

    if (count == 0) {
        log_info("clientsWork : %s (client_uid=%s) : aucune possibilité en cours d'analyse ; forks: %s\n",
                  target, client_uid_hex, fork_roles);
    } else {
        log_info("clientsWork : %s (client_uid=%s) : %llu possibilité(s) en cours d'analyse, alloc max=%d ; forks: %s\n",
                  target, client_uid_hex, count, max_alloc, fork_roles);
    }
    return 0;
}

/**
 * @brief Interpréteur de `leaseDuration <n>` : fixe la durée (secondes) du
 *        bail à expiration des possibilités attribuées à un client.
 *
 * Purement serveur (le bail n'a de sens que côté serveur, qui seul enregistre
 * une attribution — `add_possibility_analysed_owned`) : commande SERVEUR pure
 * (send_to_childs = 0), jamais propagée aux forks de recherche. `n <= 0`
 * désactive le bail (même convention que `limit 0`) : les possibilités
 * attribuées ne sont alors plus jamais rendues automatiquement au stock.
 * N'affecte que les possibilités attribuées APRÈS ce changement — celles déjà
 * en cours d'analyse gardent l'échéance calculée à leur insertion.
 */
int lease_duration_interpreter(void) {
    char *arguments = strtok(NULL, " ");
    if (arguments != NULL) {
        analysed_lease_seconds = atoi(arguments);
        return 0;
    }
    return CMD_ERR_USAGE;
}

/**
 * @brief Interpréteur de `printAnalysed [fichier]` : affiche les possibilités
 *        en cours d'analyse, ou les exporte en JSON dans [fichier] si
 *        l'argument est fourni.
 *
 * `printAnalysed` est propagée aux process fils (send_to_childs = 1,
 * `send_command_to_childs`) : le TEXTE de la commande, [fichier] compris, est
 * rejoué tel quel par le parent ET par chaque fork de recherche — des
 * processus séparés. Sans précaution, tous écriraient dans LE MÊME fichier en
 * concurrence (écritures entrelacées / clobbering). On suffixe donc le chemin
 * par le pid en mode client — même convention que `backup_interpreter` pour
 * DEF_FILE/DEF_ANALYSE_FILE — pour que chaque process écrive dans son propre
 * fichier. Le serveur ne force aucun processus de recherche, donc pas de
 * collision possible côté serveur : chemin utilisé tel quel.
 */
int printanalysed_interpreter(void) {
    char *path = strtok(NULL, " ");
    if (path == NULL) {
        return print_all_file_analysed();
    }

    char *final_path = path;
    char *suffixed = NULL;
    if (server == 0) {
        suffixed = malloc(strlen(path) + 11);
        if (suffixed == NULL) {
            log_error("printAnalysed : allocation échouée pour le suffixe de pid\n");
            return -1;
        }
        sprintf(suffixed, "%s_%i", path, getpid());
        final_path = suffixed;
    }

    FILE *out = open_export_file("printAnalysed", final_path);
    if (out == NULL) {
        free(suffixed);
        return -1;
    }
    size_t count = 0;
    int rc = fprint_all_file_analysed(out, &count);
    close_export_file("printAnalysed", final_path, out, rc, count);
    free(suffixed);
    return rc;
}

/** @brief Interpréteur de `restockAnalysed` : remet les possibilités en cours d'analyse dans le stock. */
int restockanalysed_interpreter(void) {
    return restock_analysed();
}

/**
 * @brief Interpréteur de `rebalance [n]` : un seul pas incrémental de
 *        rééquilibrage du stock (file la plus
 *        pleine vers la plus vide), même appel que celui automatique de
 *        chaque tour serveur (`check_server_step`) mais déclenché
 *        immédiatement plutôt que d'attendre le prochain tour.
 *
 * `n` optionnel : budget de possibilités déplacées PAR POOL (défaut
 * `rebalance_budget`, la même variable globale que l'appel périodique).
 * `n <= 0` explicitement fourni est un usage invalide (contrairement à
 * l'absence d'argument, qui retombe sur le défaut) — même distinction que
 * `sortDesc [n]`.
 */
int rebalance_interpreter(void) {
    char *arguments = strtok(NULL, " ");
    int budget = rebalance_budget;
    if (arguments != NULL) {
        int n = atoi(arguments);
        if (n <= 0) {
            return CMD_ERR_USAGE;
        }
        budget = n;
    }
    int moved = datamanager_rebalance_step(budget);
    log_info("rebalance : %d possibilité(s) déplacée(s)\n", moved);
    return 0;
}

/**
 * @brief Interpréteur de `stockMemory` : affiche le plafond RAM du stock (Mo
 *        et possibilités), l'occupation actuelle des deux pools de stock, et
 *        le débordement disque (`--stock-spill-dir`) — toujours affiché,
 *        même à 0, pour lire le stock COMPLET (résident + déporté) d'un seul
 *        coup d'œil, y compris quand le débordement n'est pas actif (0
 *        possibilité/segment, plutôt qu'une ligne qui disparaît selon l'état)
 *        — jusqu'ici visible uniquement via GET /api/v1/stats
 *        (`stock_spilled_packets`/`stock_spill_segments`).
 *
 * Lecture pure, jamais d'effet de bord — même esprit que `statistic`/`check`.
 * `stock_spill_total_packets`/`_segments` renvoient 0 côté client (le
 * débordement n'y est jamais configuré) : no-op silencieux, même convention
 * que le reste de cette commande sur ce rôle.
 */
int stock_memory_interpreter(void) {
    unsigned long long limit_packets = datamanager_ram_limit_packets();
    unsigned long long resident_packets = datamanager_resident_packets();
    unsigned long long resident_mb = datamanager_packets_to_ram_mb(resident_packets);
    unsigned long long spilled_packets = stock_spill_total_packets();
    unsigned long long spilled_segments = stock_spill_total_segments();
    if (limit_packets == 0) {
        log_info("stockMemory : plafond illimité, occupation ~%llu Mo (%llu possibilité(s))\n",
                  resident_mb, resident_packets);
    } else {
        unsigned long long limit_mb = datamanager_packets_to_ram_mb(limit_packets);
        log_info("stockMemory : plafond %llu Mo (~%llu possibilité(s)), occupation ~%llu Mo (%llu possibilité(s))\n",
                  limit_mb, limit_packets, resident_mb, resident_packets);
    }
    log_info("stockMemory : déporté sur disque : %llu possibilité(s) (%llu segment(s)) — total (résident + déporté) : %llu\n",
              spilled_packets, spilled_segments, resident_packets + spilled_packets);
    return 0;
}

/**
 * @brief Interpréteur de `stockMaxRam <mo>` : fixe à chaud le plafond RAM des
 *        deux pools de stock (non vérifié + vérifié).
 *
 * `<mo> <= 0` désactive le plafond (illimité) -- pas une erreur d'usage,
 * même convention que `limit 0`/`leaseDuration 0` : seul un argument
 * ENTIÈREMENT ABSENT est un usage invalide. Converti une seule fois en
 * possibilités par `datamanager_configure_ram_limit` ; n'affecte jamais ce
 * qui est déjà résident, seulement les ADD futurs.
 */
int stock_max_ram_interpreter(void) {
    char *arguments = strtok(NULL, " ");
    if (arguments == NULL) {
        return CMD_ERR_USAGE;
    }
    int mo = atoi(arguments);
    stock_max_ram_mb = mo;
    datamanager_configure_ram_limit(mo);
    if (mo > 0) {
        log_info("stockMaxRam : plafond fixé à %d Mo (~%llu possibilité(s))\n",
                  mo, datamanager_ram_limit_packets());
    } else {
        log_info("stockMaxRam : plafond désactivé (illimité)\n");
    }
    return 0;
}

/**
 * @brief Interpréteur de `spill [n]` : déclenche immédiatement un pas de
 *        débordement/rechargement sur disque (`core/stock_spill.h`).
 *
 * `n` optionnel : budget de possibilités pour CE pas (défaut
 * `STOCK_SPILL_BLOCK_PACKETS`) -- même convention `<n> <= 0` = usage invalide
 * que `rebalance [n]`.
 */
int spill_interpreter(void) {
    char *arguments = strtok(NULL, " ");
    int budget = STOCK_SPILL_BLOCK_PACKETS;
    if (arguments != NULL) {
        int n = atoi(arguments);
        if (n <= 0) {
            return CMD_ERR_USAGE;
        }
        budget = n;
    }
    int moved = stock_spill_step(budget);
    log_info("spill : %d possibilité(s) déplacée(s) (RAM <-> disque)\n", moved);
    return 0;
}

/** @brief Interpréteur de `min` : affiche le nombre minimal de pièces placées parmi toutes les possibilités. */
int min_interpreter(void) {
    log_info("min : %i\n",search_min_datas());
    
    return 0;
}

/**
 * @brief Recherche une `command_description` par son nom dans le tableau `commands`.
 *
 * La comparaison ignore la casse (`maxstockbythread` trouve `maxStockByThread`)
 * et un alias est résolu vers son entrée canonique (un seul niveau : les alias
 * pointent toujours un nom canonique, jamais un autre alias).
 *
 * @param instruction Nom de la commande à chercher.
 * @return            Pointeur vers la commande canonique trouvée, ou NULL si inconnue.
 */
/**
 * @brief `start`/`stopForks`/`configApply` agissent sur le cycle de vie CLIENT
 *        (fork_orchestrator.h) : exécutées côté SERVEUR, elles posteraient un
 *        événement à un orchestrateur qu'aucune boucle ne consomme jamais là
 *        (`fork_orchestrator_run` n'est appelée que par `handle_client`) —
 *        trompeur plutôt qu'un no-op inoffensif comme les commandes
 *        `server_only` à l'inverse (`clients`, …, harmless sur un client
 *        puisque `control_registry` y est toujours vide). D'où un masquage
 *        explicite (ni listées, ni exécutables, ni suggérées) plutôt qu'une
 *        simple annotation "[serveur]"/"[client]".
 *
 * `config`/`configSave` n'ont PLUS besoin de ce masquage : leurs interpréteurs
 * (`config_interpreter`/`config_save_interpreter`) branchent désormais
 * eux-mêmes sur `server` — affichage/écriture de la configuration SERVEUR
 * (`server_config.h`) d'un côté, configuration CLIENT (`client_config.h`) de
 * l'autre. Seule la forme `config <clé> <valeur>` (préparation d'un
 * changement à appliquer via `configApply`) reste refusée côté serveur, qui
 * n'a pas de configuration "en préparation" à appliquer à chaud.
 *
 * Delibérément une liste de noms plutôt qu'un nouveau champ sur
 * `command_description` : la table `commands[]` compte ~50 entrées toutes
 * initialisées positionnellement (pas de désignateurs) — ajouter un champ
 * neuf y forcerait soit à toucher chaque entrée, soit à laisser
 * `-Wmissing-field-initializers` (actif sous `-Wextra -Werror`) se déclencher.
 */
static int command_is_client_only(const command_description *command) {
    return strcmp(command->command, "start") == 0 ||
           strcmp(command->command, "stopForks") == 0 ||
           strcmp(command->command, "configApply") == 0;
}

/**
 * @brief Copie dans @p out les noms de commandes VISIBLES dans le rôle
 *        courant (masquage server-side de `config`/`configSave`, voir
 *        `command_is_client_only`) — utilisé pour la suggestion Levenshtein
 *        (`closest_command`), afin qu'un serveur ne suggère jamais une
 *        commande qu'il refuserait ensuite d'exécuter.
 *
 * @param out Tableau de sortie, capacité NB_COMMANDS.
 * @return    Nombre de noms écrits dans @p out.
 */
static int visible_command_names(const char *out[NB_COMMANDS]) {
    int n = 0;
    for (int c = 0; c < NB_COMMANDS; c++) {
        if (server && command_is_client_only(&commands[c])) {
            continue;
        }
        out[n++] = commands[c].command;
    }
    return n;
}

command_description *find_command(const char *instruction) {
    for (int c =0; c < NB_COMMANDS; c++) {
        command_description *command = &commands[c];
        if (strcasecmp(command->command, instruction) == 0) {
            if (command->alias_of != NULL) {
                return find_command(command->alias_of);
            }
            return command;
        }
    }
    return NULL;
}

/**
 * @brief Voir la doc dans command_lines.h.
 */
const char *command_canonical_name(const char *name) {
    if (name == NULL) {
        return NULL;
    }
    command_description *command = find_command(name);
    return command != NULL ? command->command : NULL;
}

/**
 * @brief Ajoute du texte formaté à la fin de `out` (borné à `cap`, offset suivi par `*len`).
 */
static void help_append(char *out, size_t cap, size_t *len, const char *fmt, ...) {
    if (*len >= cap) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(out + *len, cap - *len, fmt, args);
    va_end(args);
    if (written > 0) {
        *len += (size_t)written > cap - *len ? cap - *len : (size_t)written;
    }
}

/**
 * @brief Ajoute la liste des alias d'une entrée canonique : ` (alias : a, b)`.
 */
static void help_append_aliases(const command_description *canonical,
                                char *out, size_t cap, size_t *len) {
    int first = 1;
    for (int c = 0; c < NB_COMMANDS; c++) {
        if (commands[c].alias_of != NULL &&
            strcmp(commands[c].alias_of, canonical->command) == 0) {
            help_append(out, cap, len, "%s%s", first ? " (alias : " : ", ", commands[c].command);
            first = 0;
        }
    }
    if (!first) {
        help_append(out, cap, len, ")");
    }
}

/**
 * @brief Nombre de caractères affichés d'une chaîne UTF-8 (points de code, pas octets).
 *
 * Nécessaire pour aligner la colonne des résumés : `%-36s` compte les octets,
 * donc chaque accent ("catégorie") décalerait la colonne d'un caractère.
 */
static size_t utf8_display_length(const char *s) {
    size_t n = 0;
    for (; *s != '\0'; s++) {
        if (((unsigned char)*s & 0xC0) != 0x80) {
            n++;
        }
    }
    return n;
}

/**
 * @brief Ajoute la section d'une catégorie : titre + une ligne `usage  résumé` par commande.
 */
static void help_append_category(int category, char *out, size_t cap, size_t *len) {
    help_append(out, cap, len, "%s\n", category_labels[category]);
    for (int c = 0; c < NB_COMMANDS; c++) {
        const command_description *command = &commands[c];
        if (command->alias_of != NULL || command->category != category) {
            continue;
        }
        if (server && command_is_client_only(command)) {
            /* Masquée côté serveur, cf. command_is_client_only. */
            continue;
        }
        const char *shown = command->usage != NULL ? command->usage : command->command;
        size_t shown_length = utf8_display_length(shown);
        int padding = shown_length < 36 ? (int)(36 - shown_length) : 1;
        help_append(out, cap, len, "  %s%*s%s", shown, padding, " ", command->summary);
        if (command->server_only) {
            help_append(out, cap, len, " [serveur]");
        }
        help_append_aliases(command, out, cap, len);
        help_append(out, cap, len, "\n");
    }
}

/**
 * @brief Voir la doc dans command_lines.h.
 */
int help_format_general(char *out, size_t out_size) {
    size_t len = 0;
    out[0] = '\0';
    help_append(out, out_size, &len,
                "Commandes — « help <commande> » pour le détail, « help <catégorie> » pour filtrer.\n");
    help_append(out, out_size, &len, "Catégories :");
    for (int cat = 0; cat < CMD_CAT_COUNT; cat++) {
        help_append(out, out_size, &len, "%s %s", cat == 0 ? "" : ",", category_keywords[cat]);
    }
    help_append(out, out_size, &len, "\n\n");
    for (int cat = 0; cat < CMD_CAT_COUNT; cat++) {
        help_append_category(cat, out, out_size, &len);
        if (cat < CMD_CAT_COUNT - 1) {
            help_append(out, out_size, &len, "\n");
        }
    }
    return 0;
}

/**
 * @brief Voir la doc dans command_lines.h.
 */
int help_format_topic(const char *topic, char *out, size_t out_size) {
    size_t len = 0;
    out[0] = '\0';
    if (topic == NULL) {
        return -1;
    }

    command_description *command = find_command(topic);
    if (command != NULL && server && command_is_client_only(command)) {
        /* Masquée côté serveur, cf. command_is_client_only : traitée comme un
           sujet inconnu plutôt que d'en détailler l'usage. */
        command = NULL;
    }
    if (command != NULL) {
        help_append(out, out_size, &len, "%s — %s",
                    command->usage != NULL ? command->usage : command->command,
                    command->summary);
        help_append_aliases(command, out, out_size, &len);
        help_append(out, out_size, &len, "\n  catégorie   : %s\n",
                    category_labels[command->category]);
        help_append(out, out_size, &len, "  portée      : %s\n",
                    command->server_only ? "serveur uniquement" : "serveur et client");
        help_append(out, out_size, &len, "  propagation : %s\n",
                    command->send_to_childs ? "transmise aux processus fils"
                                            : "processus courant uniquement");
        if (command->details != NULL) {
            help_append(out, out_size, &len, "\n%s\n", command->details);
        }
        return 0;
    }

    for (int cat = 0; cat < CMD_CAT_COUNT; cat++) {
        if (strcasecmp(category_keywords[cat], topic) == 0) {
            help_append_category(cat, out, out_size, &len);
            return 0;
        }
    }
    return -1;
}

/** @brief Interpréteur de `help [commande|catégorie]` : aide générale, d'une commande ou d'une catégorie. */
int help_interpreter(void) {
    char *arg = strtok(NULL, " ");
    char *help = calloc(HELP_BUFFER_SIZE, sizeof(char));
    int result = arg == NULL ? help_format_general(help, HELP_BUFFER_SIZE)
                             : help_format_topic(arg, help, HELP_BUFFER_SIZE);
    if (result == 0) {
        /* log_info() tronque silencieusement chaque appel à LOG_LINE_MAX
           (4096 octets, cf. logger.c) : le texte d'aide générale dépasse
           régulièrement cette taille (table `commands` en croissance), donc
           on l'émet ligne par ligne (via strchr, pas strtok, pour ne pas
           fusionner les lignes vides qui séparent les catégories) plutôt
           qu'en un seul appel qui tronquerait la fin sans avertissement. */
        char *line_start = help;
        char *nl;
        while ((nl = strchr(line_start, '\n')) != NULL) {
            *nl = '\0';
            log_info("%s\n", line_start);
            line_start = nl + 1;
        }
        if (*line_start != '\0') {
            log_info("%s\n", line_start);
        }
    } else {
        const char *command_names[NB_COMMANDS];
        int n_visible = visible_command_names(command_names);
        const char *suggestion = closest_command(arg, command_names, n_visible);
        if (suggestion != NULL) {
            log_error("help : commande ou catégorie inconnue : %s -- vouliez-vous dire \"help %s\" ?\n",
                      arg, suggestion);
        } else {
            log_error("help : commande ou catégorie inconnue : %s\n", arg);
        }
    }
    free(help);
    return result;
}

/**
 * @brief Parse et exécute une ligne de commande.
 *
 * Tokenize `command` sur les espaces, trouve la commande via `find_command`,
 * l'exécute, et la propage aux processus enfants si `send_to_childs == 1`.
 *
 * @param command Ligne de commande saisie par l'utilisateur (modifiée par `strtok`).
 * @return        0 en cas de succès, -1 si commande inconnue ou erreur d'interpréteur.
 */
int do_command_line(char *command) {
    int result = 0;
    if (command != NULL && strlen(command) > 0) {
        size_t command_length = strlen(command) + 1;
        char *toSplit = malloc(sizeof(char) * command_length);
        memcpy(toSplit, command, command_length);
        char *instruction = strtok(toSplit, " ");
        if (instruction == NULL) {
            /* Ligne ne contenant que des espaces : rien à exécuter. */
            free(toSplit);
            return 0;
        }
        command_description *command_desc = find_command(instruction);
        if (command_desc != NULL && server && command_is_client_only(command_desc)) {
            /* Masquée côté serveur, cf. command_is_client_only : traitée comme
               une commande inconnue plutôt que d'être exécutée (elle
               agirait sur les globales du serveur, sans rapport avec la
               configuration client qu'elle est censée afficher/écrire). */
            command_desc = NULL;
        }
        if (command_desc != NULL) {
            result = command_desc->interpreter();
            if (result == CMD_ERR_USAGE) {
                /* Argument manquant/invalide : rappel d'usage uniforme depuis la
                   table, au lieu d'un échec silencieux propre à chaque commande. */
                if (command_desc->usage != NULL) {
                    log_error("usage : %s — %s\n", command_desc->usage, command_desc->summary);
                }
                result = -1;
            }
            if(result == 0 && command_desc->send_to_childs == 1) {
                send_command_to_childs(command);
            }
        } else {
            /* Commande inconnue : on le signale (au lieu d'échouer en silence)
               et on propose la commande la plus proche en cas de faute de frappe.
               On projette les noms de la table `commands` pour les passer au
               module d'appariement (qui ignore le type command_description). */
            const char *command_names[NB_COMMANDS];
            int n_visible = visible_command_names(command_names);
            const char *suggestion = closest_command(instruction, command_names, n_visible);
            if (suggestion != NULL) {
                log_error("commande inconnue : %s -- vouliez-vous dire \"%s\" ? (tapez \"help\")\n",
                          instruction, suggestion);
            } else {
                log_error("commande inconnue : %s (tapez \"help\" pour la liste)\n", instruction);
            }
            result = -1;
        }
        free(toSplit);
    }
    return result;
}

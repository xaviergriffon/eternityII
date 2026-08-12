#include "ui/command_lines.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdarg.h>
#include <signal.h>
#include <time.h>

#include "ui/logger.h"
#include "core/datamanager.h"
#include "net/local_socket.h"
#include "core/readdata.h"
#include "ui/command_match.h"
#include "app/static_variables.h"
#include "app/control_registry.h"
#include "app/known_clients_registry.h"
#include "net/control_protocol.h"
#include "core/best_board.h"
#include "app/client_config.h"
#include "app/fork_orchestrator.h"

#define DEF_FILE "./eternityII.back"
#define DEF_ANALYSE_FILE "./eternityII-in_analyse.back"
#define DEF_BEST_BOARD_FILE "./eternityII-best_board.back"
#define DEF_KNOWN_CLIENTS_FILE "./eternityII-known_clients.back"
#define NB_COMMANDS 52
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
int sort_descending_interpreter(void);
int max_stock_by_thread_interpreter(void);
int pruner_batch_interpreter(void);
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
int known_clients_interpreter(void);
int clients_work_interpreter(void);
int lease_duration_interpreter(void);
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
     "affiche ou prépare la configuration (nb_forks, serveur, fichier de pièces, ...)",
     "Sans argument : affiche l'état de l'orchestrateur (WAITING_CONFIG/COUNTDOWN/\n"
     "CONFIGURING/RUNNING/...), la configuration EFFECTIVE (celle réellement en\n"
     "vigueur) et la configuration EN PRÉPARATION. N'annule pas le décompte.\n"
     "Avec <clé> <valeur> : écrit dans la configuration en préparation (clés :\n"
     "nb_forks, server_host, parts_file, max_stock_by_thread, limit, pruner_batch)\n"
     "et ANNULE DÉFINITIVEMENT le décompte d'auto-démarrage — `start` consomme\n"
     "toujours la configuration EFFECTIVE, pas celle en préparation.", NULL},
    {"configSave", config_save_interpreter, 0, CMD_CAT_GENERAL, 0, NULL,
     "écrit la configuration effective (+ en préparation) dans le fichier de configuration",
     "Écrit la configuration EFFECTIVE, avec toute valeur EN PRÉPARATION\n"
     "(`config <clé> <valeur>`) superposée par-dessus — c'est ainsi qu'une\n"
     "valeur préparée prend effet au prochain démarrage. Écriture atomique\n"
     "(.tmp puis rename, comme « backup »). Fichier par défaut\n"
     "./eternityii-client.conf (option --config-file <chemin>).", NULL},
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
     "(max_stock_by_thread/limit/pruner_batch) sont préparées : appliquées immédiatement\n"
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

    {"sortAsc", sort_ascending_interpreter, 0, CMD_CAT_STOCK, 0, NULL,
     "trie le stock par ordre croissant (moins avancées d'abord)", NULL, NULL},
    {"sortDesc", sort_descending_interpreter, 0, CMD_CAT_STOCK, 0, "sortDesc [n]",
     "trie par ordre décroissant, toutes les files ou la file <n>", NULL, NULL},
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
     "profondeur (EXPAND_MAX_LEVELS) et en volume (EXPAND_MAX_STOCK) ; niveau 3-4 recommandé.", NULL},
    {"restockAnalysed", restockanalysed_interpreter, 0, CMD_CAT_STOCK, 0, NULL,
     "remet les possibilités en cours d'analyse dans le stock", NULL, NULL},
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
     "Liste blanche : pause, resume, limit, maxStockByThread, prunerBatch.\n"
     "Toute autre commande est refusée sans être diffusée, avec ou sans --to.\n"
     "Sans --to : diffusion à toutes les sessions de contrôle actives (comportement\n"
     "historique). Avec --to <cible> : n'atteint QUE la session désignée, par son\n"
     "session_no (entier, cf. commande « clients »), son client_uid (hexadécimal\n"
     "complet) ou son label déclaré (--name) -- la cible ne doit pas contenir\n"
     "d'espace. Un session_no ou client_uid qui ne désigne plus aucune session\n"
     "active (client déconnecté ou remplacé) est refusé, jamais redirigé vers un\n"
     "autre client ; un label partagé par plusieurs sessions actives est refusé\n"
     "comme ambigu.", NULL},
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
 * Sans argument : affiche l'état de l'orchestrateur (`fork_orchestrator_snapshot`),
 * la configuration EFFECTIVE (celle réellement en vigueur — voir
 * `client_config_capture_effective`, reflète aussi un `limit`/`maxStockByThread`/
 * `prunerBatch` déjà exécuté depuis cette même console) et la configuration
 * EN PRÉPARATION (`fork_orchestrator_format_staged_config`). N'annule pas le
 * décompte.
 *
 * Avec deux arguments (`strtok` — même convention que `limit`/`prunerBatch` :
 * un token par espace, pas de valeur contenant un espace) : synthétise une
 * ligne `clé = valeur` et la délègue à `fork_orchestrator_stage_config_line`
 * (réutilise `client_config_parse_line`, jamais de logique de validation
 * dupliquée) — écrit dans la configuration en préparation et annule
 * DÉFINITIVEMENT le décompte, seulement si la ligne est acceptée (une faute
 * de frappe ne doit pas faire perdre l'auto-démarrage). Un seul argument
 * (clé sans valeur) : `CMD_ERR_USAGE`.
 */
int config_interpreter(void) {
    char *key = strtok(NULL, " ");
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
 * @brief Interpréteur de `configSave` : écrit la configuration client
 *        EFFECTIVE dans le fichier `--config-file` (écriture atomique).
 */
int config_save_interpreter(void) {
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
    int rb = backup(def_file);
    if (rb == BACKUP_SKIPPED_MAINTENANCE) {
        log_info("backup de %s sauté (maintenance en cours)\n", def_file);
    } else if (rb != BACKUP_OK) {
        log_info("backup de %s échoué\n", def_file);
    }
    int rba = backup_analysed(def_analyse_file);
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
    // Cumul par machine (PR5) : même cadence que
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
            time_t escalation_start = time(NULL);
            stop_escalation_action_t last_escalation = STOP_ESCALATION_NONE;
            // On attend que tous les enfants soient réellement terminés.
            // kill(pid, 0) renvoie 0 tant que le process existe, -1 (ESRCH)
            // une fois qu'il a été récolté par wait_child / sigchld_handler.
            do {
                remaining = 0;
                if (childrens_pid != NULL) {
                    for (int c = 0; c < NB_THREADS; c++) {
                        if (childrens_pid[c] > 0 && kill(childrens_pid[c], 0) == 0) {
                            remaining++;
                        }
                    }
                }
                if (remaining > 0 && childrens_pid != NULL) {
                    long elapsed_ms = (long)(time(NULL) - escalation_start) * 1000L;
                    stop_escalation_action_t action = stop_escalation_next(elapsed_ms);
                    if (action != last_escalation && action != STOP_ESCALATION_NONE) {
                        int sig = (action == STOP_ESCALATION_SIGKILL) ? SIGKILL : SIGTERM;
                        log_error("exit : %d fils encore vivant(s) après %lds — escalade %s\n",
                                  remaining, elapsed_ms / 1000,
                                  action == STOP_ESCALATION_SIGKILL ? "SIGKILL" : "SIGTERM");
                        for (int c = 0; c < NB_THREADS; c++) {
                            if (childrens_pid[c] > 0) {
                                kill(childrens_pid[c], sig);
                            }
                        }
                    }
                    last_escalation = action;
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

    int result = restore(file);
    if (result != 0) {
        log_error("restore impossible (%s) : stock conservé\n", file);
    } else if (restore_analysed(analyse_file) != 0) {
        log_error("restore analysed impossible (%s) : files analysées conservées\n", analyse_file);
        result = -1;
    }
    // Non bloquant : un backup plus ancien peut ne pas avoir ce fichier (feature
    // ajoutée après coup) — le stock/analysed restaurés ci-dessus restent valides
    // sans lui, seule la représentation du meilleur plateau reste vide.
    if (result == 0) {
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
    return statistic_datas();
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
 * `resume` — cf. la note dans static_variables.h. No-op côté local si déjà en
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
    log_info("clientsWork (API HTTP admin) : %s (client_uid=%s) : %llu possibilite(s) en cours d'analyse, alloc max=%d\n",
              target, client_uid_hex, count, max_alloc);
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
 * Sans argument : journalise l'état de l'orchestrateur, la configuration
 * effective et la configuration en préparation (même contenu que
 * `config_interpreter`), n'annule PAS le décompte. Avec "<clé> <valeur>"
 * (un seul token par valeur, même convention que `limit`/`prunerBatch`) :
 * écrit dans la configuration en préparation via
 * `fork_orchestrator_stage_config_line` et annule le décompte SEULEMENT si la
 * ligne est acceptée -- une commande mal formée ne doit pas coûter
 * l'auto-démarrage.
 */
static int admin_remote_config(char *rest) {
    while (rest != NULL && *rest == ' ') {
        rest++;
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
 * @brief Les cinq commandes de cycle de vie des fils ("start",
 *        "stopForks", "configApply", "config", "configSave") agissent sur
 *        `fork_orchestrator`/`client_config`, qui ne veulent rien dire côté
 *        SERVEUR -- même raisonnement que `command_is_client_only` pour la
 *        console (voir sa doc), mais appliqué ICI parce que
 *        `admin_apply_remote_command` (contrairement à `do_command_line`) ne
 *        consulte jamais `command_is_client_only`. `POST /api/v1/command`
 *        (`src/net/http_server.c`, seul appelant de cette fonction en dehors
 *        des tests) n'est atteignable QUE depuis `runserver`
 *        (`src/app/etii_server.c`) : `server` y vaut donc toujours 1, et sans
 *        ce garde-fou "config nb_forks <n>" + "configApply" reçues par
 *        POST /api/v1/command corromprait réellement `NB_THREADS`/
 *        `childrens_pid`/… du SERVEUR (taille du pool de connexions, pas un
 *        nombre de forks) au lieu du no-op silencieux voulu.
 */
static int admin_remote_command_is_client_only(const char *word) {
    return strcmp(word, "config") == 0 ||
           strcmp(word, "configSave") == 0 ||
           strcmp(word, "start") == 0 ||
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
        } else if (strcmp(word, "sortDesc") == 0) {
            char *arg = strtok_r(NULL, " ", &save);
            if (arg != NULL) {
                int n_file = atoi(arg);
                sort_d_mono(&n_file);
            } else {
                sort_descending();
            }
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
 *        contrôle précise (`--to`, PR3) ou, par défaut, à toutes les sessions actives.
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
        log_error("clientsCommand : commande non autorisée à distance (liste blanche : pause, resume, limit, maxStockByThread, prunerBatch) : \"%s\"\n", rest);
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
 * @brief Interpréteur de `knownClients` : liste les machines connues du
 *        registre de cumul (`known_clients_registry.h`, PR4).
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
        log_info("  %s  %s (%s)  %s  sessions actives=%d  connexions=%d  "
                  "pruner checked=%llu removed=%llu  record=%llu  derniere activite=%lld\n",
                  infos[i].machine_uid_hex,
                  infos[i].label[0] != '\0' ? infos[i].label : "?",
                  infos[i].peer_ip[0] != '\0' ? infos[i].peer_ip : "?",
                  infos[i].connected ? "connecte" : "deconnecte",
                  infos[i].nb_active_sessions, infos[i].nb_connections_total,
                  (unsigned long long)infos[i].total_pruner_checked,
                  (unsigned long long)infos[i].total_pruner_removed,
                  (unsigned long long)infos[i].best_max_result,
                  (long long)infos[i].last_seen);
    }
    return 0;
}

/**
 * @brief Interpréteur de `clientsWork <cible>` : consultation « que travaille
 *        X ? » (PR6, attribution des analyses en cours).
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

    if (count == 0) {
        log_info("clientsWork : %s (client_uid=%s) : aucune possibilité en cours d'analyse\n",
                  target, client_uid_hex);
    } else {
        log_info("clientsWork : %s (client_uid=%s) : %llu possibilité(s) en cours d'analyse, alloc max=%d\n",
                  target, client_uid_hex, count, max_alloc);
    }
    return 0;
}

/**
 * @brief Interpréteur de `leaseDuration <n>` : fixe la durée (secondes) du
 *        bail à expiration des possibilités attribuées à un client (PR7).
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
 * @brief `config`/`configSave`/`start` agissent sur la configuration/le cycle
 *        de vie CLIENT (client_config.h, fork_orchestrator.h) : exécutées
 *        côté SERVEUR, elles liraient/écriraient les globales du serveur
 *        (sans rapport avec cette configuration, ex. `NB_THREADS` y désigne
 *        la taille du pool de connexions, pas un nombre de forks) ou
 *        posteraient un événement à un orchestrateur qu'aucune boucle ne
 *        consomme jamais côté serveur (`fork_orchestrator_run` n'est appelée
 *        que par `handle_client`) — trompeur plutôt qu'un no-op inoffensif
 *        comme les commandes `server_only` à l'inverse (`clients`, …,
 *        harmless sur un client puisque `control_registry` y est toujours
 *        vide). D'où un masquage explicite (ni listées, ni exécutables, ni
 *        suggérées) plutôt qu'une simple annotation "[serveur]"/"[client]".
 *
 * Delibérément une liste de noms plutôt qu'un nouveau champ sur
 * `command_description` : la table `commands[]` compte ~50 entrées toutes
 * initialisées positionnellement (pas de désignateurs) — ajouter un champ
 * neuf y forcerait soit à toucher chaque entrée, soit à laisser
 * `-Wmissing-field-initializers` (actif sous `-Wextra -Werror`) se déclencher.
 */
static int command_is_client_only(const command_description *command) {
    return strcmp(command->command, "config") == 0 ||
           strcmp(command->command, "configSave") == 0 ||
           strcmp(command->command, "start") == 0 ||
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
        log_info("%s", help);
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

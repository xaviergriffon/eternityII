#ifndef app_runtime_h
#define app_runtime_h

#include <stddef.h>
#include <sys/types.h>
#include <sys/un.h>

#include "app/etii_statistic.h"

/*
 * Fonctions « plumbing » du processus, extraites de main.c pour être testables
 * unitairement : main.c définit main() et n'est donc pas linké dans le binaire
 * de test. Ces fonctions ne dépendent que des globales de static_variables.h.
 *
 * Deux familles :
 *   - gestion des signaux (handlers + installation des dispositions) ;
 *   - bootstrap runtime (allocation des compteurs et des contextes enfants,
 *     message d'usage).
 */

/* ---- Signaux ---- */

/** @brief Handler no-op (utilisé pour SIGPIPE). */
void signal_ignored(int sig);

/** @brief Handler d'arrêt : positionne request=REQUEST_STOP, propage aux enfants
 *         (si parent), et exit(0) en mode serveur. */
void signal_end_handler(int sig);

/** @brief Handler SIGCHLD : récolte les enfants terminés (waitpid WNOHANG). */
void sigchld_handler(int signal);

/** @brief Installe sigchld_handler sur SIGCHLD. */
void init_sigchld_sigaction(void);

/** @brief Installe signal_end_handler (SIGINT/HUP/QUIT/TERM) + ignore SIGPIPE. */
void init_signals(void);

/** @brief Débloque SIGINT et installe signal_end_handler (SA_RESTART) pour un thread enfant. */
void configure_child_signals(void);

/** @brief Attend la terminaison de tous les enfants (boucle wait(), tolère EINTR). */
void wait_child(void);

/* ---- Bootstrap runtime ---- */

/** @brief Alloue et remet à zéro les compteurs par thread. @return 0. */
int  init_counters(void);

/** @brief Alloue/initialise les contextes des processus enfants (pids, forkId, stats). */
void init_childs(void);

/**
 * @brief Agrandit si besoin `childrens_pid`/`forkId`/`fork_statistics` pour
 *        couvrir au moins @p needed slots, en préservant les slots existants.
 *
 * `init_childs()` dimensionne ces trois tableaux sur `NB_THREADS` AU MOMENT
 * de son appel — avant tout fork, dans `handle_client`. Depuis que
 * `config nb_forks <n>` (console) suivi de `start` peut modifier `NB_THREADS`
 * APRÈS cet appel (`fork_orchestrator_apply_staged_config`,
 * `src/app/fork_orchestrator.c`), un `nb_forks` augmenté fait que
 * `orchestrator_spawn_forks` écrit hors bornes dans ces tableaux — trouvé via
 * un crash réel (`segmentation fault`) reproduit par un opérateur : démarrer,
 * `config nb_forks 6` (au-delà du nombre initial), `configSave`, `start`. Les
 * fils forkés avant le débordement restent vivants (observé), seul le parent
 * segfault dans la boucle de fork elle-même.
 *
 * Sans effet si @p needed est déjà couvert (jamais de rétrécissement — un
 * `nb_forks` réduit laisse simplement des slots surnuméraires inutilisés,
 * inoffensif). Les nouveaux slots sont initialisés exactement comme
 * `init_childs()` : `childrens_pid[c] = -1`, `forkId[c]` alloué et vide,
 * `fork_statistics[c]` remis à zéro.
 *
 * @param needed Capacité minimale requise (typiquement `NB_THREADS`, relu
 *               APRÈS l'application d'une configuration en préparation).
 */
void ensure_childs_capacity(int needed);

/**
 * @brief Libère `childrens_pid`/`forkId`/`fork_statistics` et remet la
 *        capacité suivie (`ensure_childs_capacity`) à 0 — symétrique
 *        d'`init_childs()`.
 *
 * Réservée à la phase `ORCH_APPLYING` d'un redémarrage à chaud
 * (`src/app/fork_orchestrator.c`) quand `nb_forks` change : appelée seulement une fois `NB_THREADS`
 * fils vivants ont été récoltés (zéro fils restant), immédiatement suivie
 * d'un nouvel `init_childs()` (qui alloue sur le `NB_THREADS` désormais à
 * jour) et d'un `init_counters()`. Ne PAS appeler pendant que des fils sont
 * encore vivants : les tableaux libérés sont ceux que `send_command_to_childs`/
 * le checker/le canal de contrôle lisent.
 *
 * Tolère un état déjà libéré (NULL) : idempotente, comme `client_config_free`.
 */
void free_childs(void);

/**
 * @brief Prédicat de vivacité d'un pid par défaut (production) : `kill(pid, 0)`.
 *
 * `ESRCH` ⇒ mort ; tout le reste (succès, ou `EPERM` — un pid vivant possédé
 * par un autre utilisateur, situation qui ne se produit pas ici puisque ce
 * sont toujours nos propres enfants) ⇒ vivant. Même technique que la commande
 * console `exit` (`src/ui/command_lines.c`).
 */
int pid_is_alive(pid_t pid);

/** @brief Type du prédicat de vivacité injecté dans `reap_dead_child_slots`
 *         (testabilité : un test fournit un prédicat déterministe plutôt que
 *         de dépendre de vrais process). */
typedef int (*child_pid_alive_fn)(pid_t pid);

/**
 * @brief Nettoie les slots de `childrens_pid`/`forkId`/`fork_statistics` dont
 *        le process n'est plus vivant.
 *
 * Corrige un trou existant : `sigchld_handler` moissonne les zombies (`waitpid`) mais ne touche jamais
 * ces tableaux, si bien qu'un fils mort de façon inattendue laisse un slot
 * fantôme — `forkId[]` continue de cibler une socket Unix `etii_fork.<pid>`
 * disparue, vers laquelle `send_command_to_childs`
 * (`src/net/local_socket.c`) continue d'émettre en pure perte. Un slot
 * détecté mort est remis à l'état de `init_childs` : `childrens_pid[c] = -1`,
 * `forkId[c][0] = '\0'`, `fork_statistics[c]` remis à zéro.
 *
 * @param childrens_pid   Tableau des pids (NULL/`nb == 0` accepté : no-op).
 * @param forkId          Tableau des chemins de socket Unix par slot.
 * @param fork_statistics Tableau des dernières statistiques connues par slot.
 * @param nb              Nombre de slots (`NB_THREADS` en production).
 * @param alive           Prédicat de vivacité. NULL accepté : repli sur
 *                        `pid_is_alive` (comportement de production), pour
 *                        qu'un appelant qui ne veut pas injecter de prédicat
 *                        obtienne quand même le comportement par défaut.
 * @return Le nombre de slots nettoyés.
 */
int reap_dead_child_slots(pid_t *childrens_pid, char **forkId,
                           struct client_statistics *fork_statistics,
                           int nb, child_pid_alive_fn alive);

/* ---- Visibilité sur la mort des enfants (diagnostic) ---------------------- */

/**
 * @brief Capacité du ring signal-safe de `child_death_record`/`child_death_drain`.
 *
 * Diagnostic uniquement : un dépassement fait perdre les évènements les plus
 * anciens depuis le dernier drain (comptés par `child_death_drain`, jamais
 * silencieusement), pas une file de production à ne jamais perdre.
 */
#define CHILD_DEATH_RING_CAPACITY 64

/** @brief Un évènement « enfant terminé » capturé par `sigchld_handler`. */
typedef struct {
    pid_t pid;
    int status; /**< Statut brut renvoyé par waitpid() — décoder avec
                     `child_death_format_reason` (WIFEXITED/WIFSIGNALED). */
} child_death_record_t;

/**
 * @brief Enregistre un évènement de mort d'enfant — appelée UNIQUEMENT depuis
 *        `sigchld_handler` (contexte signal). Async-signal-safe : un simple
 *        `__atomic_fetch_add` suivi d'une écriture dans un tableau statique
 *        préalloué, aucun malloc, aucun appel à `log_*` (non signal-safe, cf.
 *        le commentaire de `sigchld_handler`).
 *
 * `sigchld_handler` moissonnait déjà les zombies (`waitpid`) mais ne
 * conservait leur statut de sortie nulle part hors `DEBUG_SIGNAL` (jamais
 * activé en production) : un fork mort de façon inattendue (crash, OOM
 * killer, SIGSEGV) laissait les compteurs de l'orchestrateur retomber
 * silencieusement à zéro sans AUCUNE trace de la cause. Ce ring est le pont
 * entre le contexte signal (où logger est interdit) et le thread de
 * l'orchestrateur (`fork_orchestrator_run`), qui le draine à chaque tour et
 * logue la cause décodée.
 */
void child_death_record(pid_t pid, int status);

/**
 * @brief Draine jusqu'à @p max_out évènements enregistrés par
 *        `child_death_record` depuis le dernier appel. Lecture réservée à un
 *        seul thread consommateur (l'orchestrateur) — pas de verrou, la
 *        synchronisation avec l'écrivain (signal) passe par les opérations
 *        atomiques sur l'index d'écriture.
 *
 * Un dépassement de `CHILD_DEATH_RING_CAPACITY` entre deux drains fait sauter
 * silencieusement aux entrées encore valides (les plus anciennes sont déjà
 * écrasées) — la perte est comptée, jamais un accès à une entrée
 * potentiellement en cours de réécriture. Voir `child_death_dropped_count`.
 *
 * @return Le nombre d'évènements copiés dans @p out (0..max_out).
 */
int child_death_drain(child_death_record_t *out, int max_out);

/**
 * @brief Nombre d'évènements perdus par débordement du ring depuis le
 *        dernier appel à cette fonction (remise à 0 à chaque appel, comme un
 *        compteur "delta" plutôt que cumulé).
 */
int child_death_dropped_count(void);

/**
 * @brief Décode un statut brut waitpid() en texte lisible (sortie normale +
 *        code, ou signal tueur + nom). Fonction pure, testable sans process
 *        réel (statuts synthétiques).
 *
 * @param status   Statut brut tel que renvoyé par waitpid()/child_death_record.
 * @param out      Tampon destination, toujours NUL-terminé.
 * @param out_size Taille de @p out.
 */
void child_death_format_reason(int status, char *out, size_t out_size);

/**
 * @brief Prédicat PUR : ce statut waitpid() correspond-il à une fin de
 *        process propre et volontaire (`WIFEXITED` + code de sortie 0) ?
 *
 * Un fork de recherche peut légitimement `exit(EXIT_SUCCESS)` de lui-même en
 * dehors de toute séquence `stopForks`/`configApply` — typiquement en
 * exhaustant tout l'espace de recherche local d'un tout petit puzzle
 * (`ETERN_PARTS=16`, cf. les scripts `tests/integration/`), qui peut se vider
 * entièrement en quelques dizaines de millisecondes. Sans ce prédicat,
 * `fork_orchestrator_run` classait CETTE mort — un succès, pas une anomalie —
 * comme « disparu de façon inattendue » (`log_error`) simplement parce
 * qu'elle survenait en `ORCH_RUNNING` plutôt que pendant un arrêt piloté
 * (`ORCH_STOPPING`/`ORCH_APPLYING`), un faux positif trouvé sur le test
 * d'intégration `run_client_lifecycle.sh` (16 pièces, plusieurs cycles
 * start/stopForks/configApply). Un code de sortie non nul, ou une
 * terminaison par signal (crash, OOM killer, `kill -9`), reste classé comme
 * anomalie potentielle quel que soit l'état — seul le "succès propre" est
 * inconditionnellement bénin.
 */
int child_death_is_clean_exit(int status);

/** @brief Affiche le message d'usage (arguments invalides) : aide générale sur
 *         stderr via `log_error` — même source de vérité que `--help`. */
void failed_arg(void);

/**
 * @brief Résout le libellé déclaré d'un client (fonction pure, testable sans
 *        appeler `gethostname`) : priorité au libellé CLI explicite
 *        (`--name`, cf. `client_label`), sinon repli sur `hostname_or_null`
 *        (résultat de `gethostname`, ou `NULL` en cas d'échec), sinon `"?"`.
 *        Toujours borné et NUL-terminé.
 *
 * @param cli_label       Valeur de `client_label` (peut être `NULL`).
 * @param hostname_or_null Nom d'hôte déjà lu par l'appelant (peut être `NULL`).
 * @param out             Tampon destination.
 * @param out_size        Taille de `out` (`CLIENT_LABEL_MAX` typique).
 */
void resolve_client_label(const char *cli_label, const char *hostname_or_null,
                           char *out, size_t out_size);

/**
 * @brief Résout l'identité déclarée de CE process client (`g_client_identity_template`,
 *        static_variables.h) AVANT tout fork : charge/crée le `machine_uid`
 *        persistant (`machine_uid_file_path`), tire un `client_uid` de session,
 *        résout le mode (recherche/pruner/pruner GPU) et le libellé
 *        (`resolve_client_label`). `fork_seq` est laissé à -1 dans le gabarit —
 *        chaque point d'envoi (hello de travail par fork, hello de contrôle du
 *        parent) l'ajuste sur une copie locale.
 *
 * Appelée une seule fois par `handle_client` (src/app/main.c), jamais en mode
 * serveur ou test (ces modes n'ouvrent aucune connexion de travail vers un
 * serveur, donc aucun hello n'est jamais émis).
 */
void init_client_identity(void);

/**
 * @brief Garantit `nb_file_possibility >= nb_threads` avant tout fork — voir la
 *        doc au corps de la fonction (`src/app/app_runtime.c`) pour le raisonnement complet.
 *
 * @param nb_threads Nombre de forks de recherche demandés (`NB_THREADS`).
 * @return           1 si un ajustement a eu lieu (log déjà émis), 0 sinon.
 */
int ensure_stock_files_cover_forks(int nb_threads);

/* ---- Aide CLI (`--help` / `-h`, mode `help [sujet]`) ---- */

/**
 * @brief Un sujet d'aide CLI : un mode d'exécution ou une option globale.
 *
 * Source unique de vérité de l'aide en ligne de commande, sur le modèle de la
 * table `commands[]` de la console (ui/command_lines.c) : l'aide générale
 * (`format_cli_help`), l'aide par sujet (`format_cli_help_topic`) et le message
 * d'erreur d'arguments (`failed_arg`) sont tous dérivés de cette table.
 */
typedef struct cli_help_topic {
	const char *name;    /**< Nom canonique : mode (`server`) ou option (`--http-port`). */
	const char *usage;   /**< Ligne d'usage complète (arguments entre crochets = optionnels). */
	const char *summary; /**< Résumé d'une ligne, affiché dans l'aide générale. */
	const char *details; /**< Complément affiché par `help <sujet>` (NULL accepté). */
} cli_help_topic_t;

/**
 * @brief Renvoie la table des sujets d'aide CLI.
 * @param out_count Sortie : nombre de sujets (NULL accepté).
 * @return Tableau statique, jamais NULL.
 */
const cli_help_topic_t *cli_help_topics(int *out_count);

/**
 * @brief Cherche un sujet d'aide par nom, insensible à la casse et aux tirets
 *        de tête (`help http-port` == `help --http-port`).
 * @return Le sujet trouvé, ou NULL si inconnu.
 */
const cli_help_topic_t *cli_help_find_topic(const char *name);

/**
 * @brief Formate l'aide générale (usage global, modes, options) dans `buf`.
 * @return La longueur écrite (tronquée à `bufsz - 1`).
 */
int format_cli_help(char *buf, size_t bufsz);

/**
 * @brief Formate l'aide détaillée d'un sujet (usage, résumé, complément).
 * @return La longueur écrite, ou -1 si le sujet est inconnu (buf non modifié).
 */
int format_cli_help_topic(const char *name, char *buf, size_t bufsz);

/** @brief Affiche l'aide générale sur la sortie standard (`log_console`). */
void print_cli_help(void);

/**
 * @brief Affiche l'aide d'un sujet sur la sortie standard.
 * @return 0 si le sujet est connu, -1 sinon (rien n'est affiché).
 */
int print_cli_help_topic(const char *name);

/* ---- Parsing d'arguments CLI (main.c) ---- */

/**
 * @brief Parse un entier positif optionnel avec repli explicite.
 *
 * Extrait de `handle_client` (parsing du nombre de threads, argv[3]) pour
 * être testable hors de main.c (non linké dans le binaire de test).
 *
 * @param arg             Chaîne à parser (ex. argv[i]), ou NULL si absente.
 * @param fallback         Valeur renvoyée si `arg` est NULL ou si l'entier parsé est <= 0.
 * @param out_was_invalid  Sortie (NULL accepté) : 1 si `arg` était fourni mais
 *                         non positif (repli appliqué à cause d'une erreur, pas
 *                         d'une absence d'argument), 0 sinon.
 * @return `atoi(arg)` s'il est strictement positif, sinon `fallback`.
 */
int parse_positive_int_or_default(const char *arg, int fallback, int *out_was_invalid);

/**
 * @brief Classifie et interprète l'argument optionnel « nb_threads » de `server`.
 *
 * Usage : `server [nb_threads] [pieces.csv]` — le premier argument DOIT être
 * un nombre de threads, pas un fichier ; une erreur fréquente ("server
 * data/pieces16.csv") donnerait sinon `atoi(chemin) == 0` → serveur démarré
 * avec 0 thread de communication (accepte les connexions mais ne les sert
 * jamais). Extrait de `handle_server` pour être testable hors de main.c.
 *
 * @param arg                 argv[2], ou NULL si absent.
 * @param default_nb_threads   Valeur affectée à `*out_nb_threads` si `arg` est
 *                              absent, ou numérique mais invalide (<= 0).
 * @param out_nb_threads       Sortie : nombre de threads retenu.
 * @return SERVER_ARG_AS_COUNT (0) si `arg` est absent ou interprété comme un
 *         compte de threads valide ; SERVER_ARG_INVALID_COUNT (1) si `arg`
 *         ressemble à un nombre mais est invalide (<= 0, repli appliqué) ;
 *         SERVER_ARG_AS_FILENAME (2) si `arg` ne ressemble pas à un nombre
 *         (traité comme le fichier de pièces).
 */
enum {
	SERVER_ARG_AS_COUNT = 0,
	SERVER_ARG_INVALID_COUNT = 1,
	SERVER_ARG_AS_FILENAME = 2,
};
int parse_server_thread_arg(const char *arg, int default_nb_threads, int *out_nb_threads);

/**
 * @brief Parse les arguments positionnels de `client` / `pruner`.
 *
 * Usage : `client [serveur] [nb_threads] [max_stock] [pieces.csv]`
 *         `pruner [serveur] [nb_threads] [pieces.csv] [batch]`
 * Le sens d'argv[4]/argv[5] dépend donc de `pruner_mode` (lu, jamais écrit ici) :
 * pour un pruner, argv[4] est le fichier de pièces et argv[5] la taille de lot
 * (bornée à [1, PRUNER_BATCH_MAX]) ; pour un client de recherche, argv[4] est le
 * stock max par thread et argv[5] le fichier de pièces. Positionne les globales
 * `NB_THREADS`, `max_stock_by_thread`, `pruner_batch_size`, `parts_files`.
 * Extrait de `handle_client` (main.c) pour être testable.
 *
 * @param argc Nombre d'arguments (après retrait des options par parse_cli_options).
 * @param argv Arguments (argv[1] = mode, déjà consommé par l'appelant).
 * @return     L'adresse du serveur (argv[2], ou "localhost" si absente).
 */
const char *parse_client_args(int argc, const char *argv[]);

/* ---- Fin de vie du client ---- */

/**
 * @brief Sauvegarde de secours à la sortie d'un client (extrait de `run_client`).
 *
 * En mode client, les files locales doivent être vides au retour de
 * `run_mono_client` : s'il reste du stock (`datas_size() > 0`), c'est une
 * anomalie — on le sauvegarde dans `./failed_exit_eternityII_<pid>.back` (+
 * variante `-in_analyse`) pour ne rien perdre. No-op si les files sont vides.
 */
void backup_failed_exit(void);

/* ---- Threads de statistiques ---- */

/**
 * @brief Démarre le thread de statistiques (détaché) : `check_server` en mode
 *        serveur (server == 1), `check_client_threads` sinon.
 *
 * Non fatal : sous forte pression de ressources, un échec de création laisse
 * l'application tourner sans thread de statistiques.
 *
 * @param server 1 pour le serveur, 0 pour un client.
 * @return 0 si le thread a démarré, -1 sinon.
 */
int run_checker(int server);

/* ---- IPC parent<->enfants (sockets Unix UDP locales) ---- */

/**
 * @brief Thread de statistiques du processus enfant (fork).
 *
 * Crée le socket Unix local de l'enfant (`etii_fork.<pid>`), démarre le thread
 * `fork_udp` pour la réception des commandes IPC, puis toutes les secondes :
 * calcule le débit moyen sur 5 s, compte les possibilités en stock et en
 * analyse, envoie une structure `client_statistics` au parent via `sendto`.
 *
 * @param param Pointeur vers la `sockaddr_un` du socket principal du parent.
 * @return      NULL.
 */
void *fork_checker(void *param);

/**
 * @brief Exécute `fork_checker` dans un thread détaché.
 * @param main_addr Adresse du socket principal du parent.
 * @return 0 en cas de succès.
 */
int run_fork_checker(struct sockaddr_un *main_addr);

/**
 * @brief Thread de réception des statistiques IPC en provenance des processus
 *        enfants (parent).
 *
 * Reçoit des structures `client_statistics` via `recvfrom` sur le socket Unix
 * principal, identifie l'enfant émetteur en comparant `sun_path` avec
 * `forkId[]`, et met à jour `fork_statistics[cpt]` par copie mémoire.
 *
 * @param param Pointeur vers l'entier `socket_id` du socket UDP Unix principal.
 * @return      NULL.
 */
void *server_tcp(void *param);

/**
 * @brief Exécute `server_tcp` dans un thread détaché.
 * @param socket_id Pointeur vers le descripteur du socket UDP Unix principal.
 */
void run_server_thread(int *socket_id);

/**
 * @brief Thread de réception des commandes IPC dans les processus enfants.
 *
 * Reçoit des chaînes de commande envoyées par le parent via UDP Unix et les
 * délègue à `do_command_line`. Configure les signaux enfant via
 * `configure_child_signals` au démarrage.
 *
 * @param param Pointeur vers l'entier `socket_id` du socket UDP Unix de l'enfant.
 * @return      NULL.
 */
void *fork_udp(void *param);

/**
 * @brief Démarre le thread `fork_udp` (réception des commandes IPC) en mode détaché.
 * @param socket_id Pointeur vers le descripteur du socket UDP Unix de l'enfant.
 */
void run_fork_thread(int *socket_id);

#endif /* app_runtime_h */

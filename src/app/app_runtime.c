/*
 * app_runtime.c — fonctions de plomberie du processus extraites de main.c
 * (gestion des signaux + bootstrap runtime), regroupées ici pour être testables
 * unitairement. Voir app_runtime.h. Le comportement est strictement identique à
 * l'original : les corps ont été déplacés verbatim depuis main.c.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stddef.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>

#include "app/app_runtime.h"
#include "app/static_variables.h"
#include "app/etii_client.h"
#include "app/etii_server.h"
#include "app/etii_statistic.h"
#include "app/fork_gate.h"
#include "core/datamanager.h"
#include "core/best_board.h"
#include "net/client_identity.h"
#include "net/local_socket.h"
#include "net/ipc_protocol.h"
#include "ui/command_lines.h"
#include "ui/logger.h"

/* ============================== Aide CLI ================================== */

/*
 * Source unique de vérité de l'aide en ligne de commande (cf. app_runtime.h) :
 * les modes d'abord, les options globales ensuite — l'aide générale préserve
 * cet ordre. `--gpu` est toujours listée (avec la mention CUDA=1) même sur un
 * build CPU : l'utilisateur doit pouvoir découvrir que l'exécution GPU existe
 * et comment l'obtenir (le mode pruner produit alors une erreur explicite).
 */
static const cli_help_topic_t cli_topics[] = {
	{ "server",
	  "server [nb_threads] [pieces.csv]",
	  "Serveur : distribue les possibilités aux clients connectés.",
	  "nb_threads (défaut 80) : connexions simultanées servies — dimensionner pour\n"
	  "les connexions de travail PLUS une connexion de contrôle par machine cliente.\n"
	  "Options utiles : --expand-level <n> (pré-expansion anti-famine du stock au\n"
	  "démarrage), --http-port <n> (API REST d'administration sur 127.0.0.1),\n"
	  "--http-token-file <chemin> (authentifie restore/backup sur cette API)." },
	{ "client",
	  "client [serveur] [nb_threads] [max_stock] [pieces.csv]",
	  "Client de recherche : explore l'arbre et renvoie ses résultats au serveur.",
	  "serveur (défaut localhost) : hôte du serveur. nb_threads (défaut 1) :\n"
	  "processus de recherche forkés. max_stock : possibilités conservées\n"
	  "localement par processus avant délégation au serveur.\n"
	  "Options utiles : --name <label> (identité affichée côté serveur, défaut le\n"
	  "nom d'hôte), --machine-uid-file <chemin> (identité machine persistante)." },
	{ "pruner",
	  "pruner [serveur] [nb_threads] [pieces.csv] [batch]",
	  "Client pruner : valide par lots les possibilités non vérifiées du serveur.",
	  "batch : taille du lot d'échange avec le serveur (borné à PRUNER_BATCH_MAX ;\n"
	  "modifiable en cours d'exécution via la commande console prunerBatch <n>).\n"
	  "Avec --gpu, le contrôle des lots est exécuté sur le GPU (build CUDA=1).\n"
	  "Options utiles : --name <label>, --machine-uid-file <chemin> (cf. help client)." },
	{ "test",
	  "test [pieces.csv]",
	  "Mode autonome : recherche locale mono-processus, sans serveur.",
	  "Génère lui-même ses premières possibilités depuis le fichier de pièces\n"
	  "(défaut ./data/pieces.csv) — aucun serveur nécessaire." },
	{ "help",
	  "help [sujet]",
	  "Affiche cette aide, ou le détail d'un mode/option (ex. help server).",
	  "Sans argument : aide générale (équivalent de --help). Avec un sujet : le\n"
	  "détail d'un mode ou d'une option (les tirets de tête sont facultatifs :\n"
	  "help http-port == help --http-port)." },
	{ "--stop-on-solution",
	  "--stop-on-solution",
	  "S'arrêter à la première solution trouvée (défaut : continuer la recherche).",
	  "Acceptée à n'importe quelle position, par tous les modes. Absente : le\n"
	  "processus de recherche backtrack après une solution pour en chercher\n"
	  "d'autres et le serveur reste en service. Chaque solution est sauvegardée\n"
	  "dans un fichier unique (jamais écrasée)." },
	{ "--expand-level",
	  "--expand-level <n>",
	  "Serveur : pré-expansion du stock au démarrage jusqu'au niveau de curseur n.",
	  "Transforme la possibilité genèse en milliers de possibilités distribuables\n"
	  "avant l'arrivée des clients (anti-famine). Niveau 3-4 conseillé ; borné par\n"
	  "EXPAND_MAX_LEVELS et EXPAND_MAX_STOCK. Ignorée par les autres modes." },
	{ "--http-port",
	  "--http-port <n>",
	  "Serveur : API REST d'administration sur 127.0.0.1:<n> (désactivée par défaut).",
	  "Boucle locale uniquement (jamais exposée sur le réseau). Endpoints sous\n"
	  "/api/v1 : stats, status, clients, best-board, command (commandes de la\n"
	  "liste blanche seulement : pause, resume, limit, ... ; restore/backup avec\n"
	  "--http-token-file, cf. help --http-token-file)." },
	{ "--http-token-file",
	  "--http-token-file <chemin>",
	  "Serveur : jeton Bearer débloquant restore/backup sur POST /api/v1/command.",
	  "Fichier lisible du seul propriétaire (chmod 600, comme une clé SSH), une\n"
	  "ligne = le jeton. Sans cette option (défaut) : restore/backup restent\n"
	  "inaccessibles via l'API HTTP, quel que soit --http-port. Avec elle :\n"
	  "Authorization: Bearer <jeton> requis pour ces deux commandes UNIQUEMENT —\n"
	  "les autres (pause, limit, ...) et les autres routes restent sans\n"
	  "authentification, comme avant. Sans --http-port : accepté, avertissement\n"
	  "au démarrage (jeton inutilisé)." },
	{ "--name",
	  "--name <label>",
	  "Client/pruner : libellé déclaré, affiché côté serveur (défaut : nom d'hôte).",
	  "Purement déclaratif (jamais vérifié par le serveur, à la différence de l'IP\n"
	  "du pair). Visible dans la commande console `clients` et\n"
	  "`GET /api/v1/clients`. Ignorée en mode serveur." },
	{ "--machine-uid-file",
	  "--machine-uid-file <chemin>",
	  "Client/pruner : chemin de l'identité machine persistante (défaut ./eternityii-machine_uid).",
	  "Nonce tiré au premier lancement puis relu aux suivants, pour que les\n"
	  "statistiques cumulées d'une même machine restent corrélables après un\n"
	  "redémarrage du client. Fichier absent/illisible : régénéré silencieusement.\n"
	  "Répertoire non inscriptible : identité volatile pour cette exécution\n"
	  "(avertissement), la recherche continue normalement. Ignorée en mode serveur." },
	{ "--config-file",
	  "--config-file <chemin>",
	  "Client/pruner : fichier de configuration clé=valeur (défaut ./eternityii-client.conf).",
	  "Lu au démarrage (avant tout fork), pré-remplit les valeurs par défaut des\n"
	  "positions non fournies en ligne de commande — priorité CLI > fichier >\n"
	  "défauts. Clés reconnues : nb_forks, server_host, parts_file,\n"
	  "max_stock_by_thread, limit, pruner_batch. Fichier absent ou illisible :\n"
	  "pas une erreur (valeurs par défaut/CLI utilisées). Écrit par la commande\n"
	  "console configSave (écriture atomique .tmp puis rename) ; affiché par\n"
	  "la commande config." },
	{ "--gpu",
	  "--gpu",
	  "Pruner : exécute le contrôle des lots sur le GPU (build CUDA=1 uniquement).",
	  "Interprétée par le mode pruner uniquement (ignorée par les autres modes).\n"
	  "Sur un binaire compilé sans CUDA : erreur explicite au lancement plutôt\n"
	  "qu'un repli CPU silencieux — recompiler avec make CUDA=1 (GPU NVIDIA)." },
	{ "--headless",
	  "--headless",
	  "N'exécute pas la console interactive (lecture de stdin) — pensé pour un service.",
	  "Acceptée à n'importe quelle position, par tous les modes (server, client,\n"
	  "pruner, test). Utile sous systemd (StandardInput=null) : sans ce flag, la\n"
	  "console se termine déjà proprement sur EOF immédiat, mais démarre et meurt\n"
	  "inutilement à chaque lancement. Les logs restent inchangés dans les deux\n"
	  "cas — aucun code ANSI n'est émis quand stdout n'est pas un TTY." },
	{ "--help",
	  "--help | -h",
	  "Affiche l'aide générale puis quitte.",
	  "Acceptée à n'importe quelle position, par tous les modes." },
};

const cli_help_topic_t *cli_help_topics(int *out_count)
{
	if (out_count != NULL) {
		*out_count = (int)(sizeof(cli_topics) / sizeof(cli_topics[0]));
	}
	return cli_topics;
}

/** @brief Saute les tirets de tête d'un nom de sujet (`--http-port` → `http-port`). */
static const char *cli_topic_strip_dashes(const char *name)
{
	while (*name == '-') {
		name++;
	}
	return name;
}

const cli_help_topic_t *cli_help_find_topic(const char *name)
{
	if (name == NULL) {
		return NULL;
	}
	const char *wanted = cli_topic_strip_dashes(name);
	int count = 0;
	const cli_help_topic_t *topics = cli_help_topics(&count);
	for (int i = 0; i < count; i++) {
		if (strcasecmp(cli_topic_strip_dashes(topics[i].name), wanted) == 0) {
			return &topics[i];
		}
	}
	return NULL;
}

/** @brief Ajoute du texte formaté à `buf` sans jamais déborder (accumulateur snprintf). */
static void cli_help_append(char *buf, size_t bufsz, size_t *len, const char *format, ...)
{
	if (*len >= bufsz) {
		return;
	}
	va_list args;
	va_start(args, format);
	int n = vsnprintf(buf + *len, bufsz - *len, format, args);
	va_end(args);
	if (n > 0) {
		*len += (size_t)n;
		if (*len > bufsz - 1) {
			*len = bufsz - 1;
		}
	}
}

int format_cli_help(char *buf, size_t bufsz)
{
	if (buf == NULL || bufsz == 0) {
		return 0;
	}
	buf[0] = '\0';
	size_t len = 0;
	cli_help_append(buf, bufsz, &len,
	                "eternityII — solveur distribué du puzzle Eternity II\n\n"
	                "Usage : ./eternityII <mode> [arguments] [options]\n\nModes :\n");
	int count = 0;
	const cli_help_topic_t *topics = cli_help_topics(&count);
	for (int i = 0; i < count; i++) {
		if (topics[i].name[0] == '-') {
			continue;
		}
		cli_help_append(buf, bufsz, &len, "  %-58s %s\n", topics[i].usage, topics[i].summary);
	}
	cli_help_append(buf, bufsz, &len,
	                "\nOptions (position-indépendantes, retirées avant le parsing des modes) :\n");
	for (int i = 0; i < count; i++) {
		if (topics[i].name[0] != '-') {
			continue;
		}
		cli_help_append(buf, bufsz, &len, "  %-58s %s\n", topics[i].usage, topics[i].summary);
	}
	cli_help_append(buf, bufsz, &len,
	                "\n./eternityII help <sujet> détaille un mode ou une option.\n");
	return (int)len;
}

int format_cli_help_topic(const char *name, char *buf, size_t bufsz)
{
	const cli_help_topic_t *topic = cli_help_find_topic(name);
	if (topic == NULL || buf == NULL || bufsz == 0) {
		return -1;
	}
	buf[0] = '\0';
	size_t len = 0;
	cli_help_append(buf, bufsz, &len, "usage : %s\n%s\n", topic->usage, topic->summary);
	if (topic->details != NULL) {
		cli_help_append(buf, bufsz, &len, "\n%s\n", topic->details);
	}
	return (int)len;
}

/* Taille suffisante pour l'aide générale complète (la plus longue des sorties). */
#define CLI_HELP_BUF_SIZE 4096

void print_cli_help(void)
{
	char buf[CLI_HELP_BUF_SIZE];
	format_cli_help(buf, sizeof buf);
	log_console("%s", buf);
	flush_console();
}

int print_cli_help_topic(const char *name)
{
	char buf[CLI_HELP_BUF_SIZE];
	if (format_cli_help_topic(name, buf, sizeof buf) < 0) {
		return -1;
	}
	log_console("%s", buf);
	flush_console();
	return 0;
}

/**
 * @brief Affiche un message d'erreur indiquant que les arguments sont incorrects.
 *
 * Réutilise l'aide générale (même table `cli_topics` que `--help`) sur stderr.
 */
void failed_arg(void)
{
	char buf[CLI_HELP_BUF_SIZE];
	format_cli_help(buf, sizeof buf);
	log_error("arguments invalides.\n%s", buf);
}
/**
 * @brief Initialise les compteurs utilisés dans le programme.
 *
 * Cette fonction configure et initialise tous les compteurs nécessaires
 * au bon fonctionnement du programme. Elle doit être appelée au début du
 * programme avant que les compteurs ne soient utilisés.
 *
 * @return int Retourne 0 si l'initialisation est réussie, ou un code
 * d'erreur non nul en cas d'échec.
 */
int init_counters(void)
{
	// free(NULL) est un no-op : sûr au premier appel (globales à zéro).
	// Nécessaire depuis qu'un redémarrage à chaud (ORCH_APPLYING,
	// src/app/fork_orchestrator.c) peut réappeler cette fonction après un
	// changement de nb_forks, sans quoi chaque redémarrage fuyait l'ancien
	// tampon.
	free(counters);
	free(lastfilesize);
	counters = malloc(sizeof(unsigned long long) * NB_THREADS);
	lastfilesize = malloc(sizeof(unsigned long long) * NB_THREADS);
	
	for(int c = 0; c < NB_THREADS;c++)
	{
		counters[c] = 0;
		lastfilesize[c] = 0;
	}

	return 0;
}

int parse_positive_int_or_default(const char *arg, int fallback, int *out_was_invalid)
{
	if (out_was_invalid != NULL) {
		*out_was_invalid = 0;
	}
	if (arg == NULL) {
		return fallback;
	}
	int n = atoi(arg);
	if (n > 0) {
		return n;
	}
	if (out_was_invalid != NULL) {
		*out_was_invalid = 1;
	}
	return fallback;
}

int parse_server_thread_arg(const char *arg, int default_nb_threads, int *out_nb_threads)
{
	if (arg == NULL) {
		*out_nb_threads = default_nb_threads;
		return SERVER_ARG_AS_COUNT;
	}
	int n = atoi(arg);
	char c0 = arg[0];
	int looks_numeric = (c0 == '+' || c0 == '-' || (c0 >= '0' && c0 <= '9'));
	if (n > 0) {
		*out_nb_threads = n;
		return SERVER_ARG_AS_COUNT;
	}
	*out_nb_threads = default_nb_threads;
	if (looks_numeric) {
		/* Nombre fourni mais non valide (0 ou négatif) : on garde le défaut. */
		return SERVER_ARG_INVALID_COUNT;
	}
	/* Pas un nombre : traité comme le fichier de pièces, nombre de threads inchangé. */
	return SERVER_ARG_AS_FILENAME;
}

const char *parse_client_args(int argc, const char *argv[])
{
    NB_THREADS = 1;
    const char *serverIp = "localhost";
    if (argc >= 3) {
        serverIp = argv[2];
    }
    if (argc >= 4) {
        int was_invalid = 0;
        NB_THREADS = parse_positive_int_or_default(argv[3], 1, &was_invalid);
        if (was_invalid) {
            // Un nombre de threads <= 0 (ou non numérique) donnerait 0 process de
            // travail : le client ne ferait rien. On retombe sur 1.
            log_error("nombre de threads invalide (\"%s\") — 1 thread par défaut\n", argv[3]);
        }
    }
    if (argc >= 5) {
        if (pruner_mode) {
            // pruner [serveur] [nb_threads] [pieces.csv] [batch] : pas de stock local
            parts_files = (char *)(argv[4]);
        } else {
            max_stock_by_thread = atoi(argv[4]);
        }
    }
    if (pruner_mode && argc >= 6) {
        // Taille du lot d'échange pruner (configurable au démarrage). Bornée pour
        // maîtriser la mémoire du pruner et les tampons GPU.
        pruner_batch_size = atoi(argv[5]);
        if (pruner_batch_size < 1) {
            pruner_batch_size = 1;
        }
        if (pruner_batch_size > PRUNER_BATCH_MAX) {
            pruner_batch_size = PRUNER_BATCH_MAX;
        }
    }
    // argv[5] = pieces.csv pour un client de recherche. Pour un pruner, argv[5]
    // est la taille de lot (cf. plus haut) et le fichier de pièces reste argv[4].
    if (!pruner_mode && argc >= 6) {
        parts_files = (char *)(argv[5]);
    }
#ifdef DEBUG_IN_MONO_PROCESS
    NB_THREADS = 1;
#endif
    return serverIp;
}

void backup_failed_exit(void)
{
	// Comme on est en mode client, on ne devrait plus rien avoir dans les files
	// si c'est le cas, il s'agit d'une erreur
	if (datas_size() > 0) {
		char *def_file = malloc(sizeof(char) * 50);
        sprintf(def_file, "./failed_exit_eternityII_%i.back", getpid());
        char *def_analyse_file = malloc(sizeof(char) * 60);
        sprintf(def_analyse_file, "./failed_exit_eternityII-in_analyse_%i.back", getpid());
		backup(def_file);
        backup_analysed(def_analyse_file);
        free(def_file);
        free(def_analyse_file);
	}
}

/**
 * @brief Initialise le thread chargé de faire les statistiques.
 *
 * @param server 1 si le thread est pour le serveur, 0 pour le client.
 */
int run_checker(int server)
{
	pthread_attr_t *thread_attributes = malloc(sizeof *thread_attributes);
	pthread_attr_init(thread_attributes);
	pthread_attr_setdetachstate(thread_attributes, PTHREAD_CREATE_DETACHED);
	pthread_t thread;
	/* Création du thread */

	void *method= NULL;
	if(server == 1)
	{
		method = check_server;
	} else
	{
		method = check_client_threads;
	}

	if(0 != pthread_create(&thread, NULL, method, NULL))
	{
		// Non fatal : sous forte pression de ressources (trop de threads/process
		// demandés), on poursuit sans thread de statistiques plutôt que de
		// planter l'application.
		log_error("run_checker : pthread_create a échoué — pas de thread de statistiques\n");
		free(thread_attributes);
		return -1;
	}
	pthread_attr_destroy(thread_attributes);
	free(thread_attributes);
	return 0;
}

/** @brief Gestionnaire de signal no-op (utilisé pour SIGPIPE). */
void signal_ignored(int sig) {
    (void)sig;
#ifdef DEBUG_SIGNAL
    log_debug("catch signal %s\n", strsignal(sig));
#endif
}

/**
 * @brief Gestionnaire de signal d'arrêt (SIGINT, SIGTERM, SIGHUP, SIGQUIT).
 *
 * Positionne `request = REQUEST_STOP` et propage le signal à tous les processus
 * enfants (si le processus courant est le parent). En mode serveur, appelle
 * `exit(0)` directement.
 *
 * @param sig Numéro du signal reçu.
 */
void signal_end_handler(int sig)
{
#ifdef DEBUG_SIGNAL
    log_console("receive signal : %i\n", sig);
    flush_console();
#endif // DEBUG_SIGNAL
	request = REQUEST_STOP;
    if (childrens_pid != NULL && parent_pid == getpid()) {
		for (int c = 0; c < NB_THREADS; c++) {
            if (childrens_pid[c] > 0) {
                kill(childrens_pid[c], sig);
            }
		}
#ifdef DEBUG_SIGNAL
    } else if (childrens_pid != NULL && parent_pid != getpid()) {
        log_info("child %d receive signal %s\n", getpid(), strsignal(sig));
#endif
    }

    if (server == 1) {
        exit(0);
    }
}


/**
 * @brief Gestionnaire de SIGCHLD : récolte les statuts des processus enfants terminés.
 *
 * Appelle `waitpid(-1, WNOHANG)` en boucle pour éviter les zombies. En mode
 * DEBUG_SIGNAL, journalise les codes de sortie et les signaux reçus.
 *
 * @param signal Numéro du signal (toujours SIGCHLD).
 */
/* ---- Visibilité sur la mort des enfants (diagnostic, cf. app_runtime.h) --- */

static child_death_record_t g_child_death_ring[CHILD_DEATH_RING_CAPACITY];
/* Index d'écriture : jamais remis à 0, toujours croissant. Un seul écrivain
   possible à la fois PAR CONSTRUCTION habituelle (SIGCHLD se bloque sur le
   thread qui exécute son propre handler pendant l'exécution de celui-ci),
   mais rien n'empêche en théorie deux threads différents de recevoir SIGCHLD
   et d'exécuter le handler en parallèle (le masquage est par-thread, pas
   process-wide) — __atomic_fetch_add reste correct dans ce cas, contrairement
   à un simple `g_child_death_write_index++`. */
static unsigned long g_child_death_write_index = 0;
/* Index de lecture : lu/écrit UNIQUEMENT par le thread consommateur
   (fork_orchestrator_run) — jamais concurrent avec lui-même, pas besoin
   d'atomique côté lecture pour cet index précis. */
static unsigned long g_child_death_read_index = 0;
static int g_child_death_dropped = 0;

void child_death_record(pid_t pid, int status)
{
    unsigned long idx = __atomic_fetch_add(&g_child_death_write_index, 1, __ATOMIC_SEQ_CST);
    child_death_record_t *slot = &g_child_death_ring[idx % CHILD_DEATH_RING_CAPACITY];
    slot->pid = pid;
    slot->status = status;
}

int child_death_drain(child_death_record_t *out, int max_out)
{
    if (out == NULL || max_out <= 0) {
        return 0;
    }
    unsigned long write_index = __atomic_load_n(&g_child_death_write_index, __ATOMIC_SEQ_CST);
    unsigned long available = write_index - g_child_death_read_index;
    if (available > CHILD_DEATH_RING_CAPACITY) {
        /* Débordement depuis le dernier drain : les entrées les plus
           anciennes ont déjà été écrasées par des morts plus récentes — on
           saute directement au plus vieux slot encore valide plutôt que de
           relire une entrée potentiellement déjà réécrite. */
        g_child_death_dropped += (int)(available - CHILD_DEATH_RING_CAPACITY);
        g_child_death_read_index = write_index - CHILD_DEATH_RING_CAPACITY;
        available = CHILD_DEATH_RING_CAPACITY;
    }
    int n = 0;
    while (n < max_out && (unsigned long)n < available) {
        out[n] = g_child_death_ring[g_child_death_read_index % CHILD_DEATH_RING_CAPACITY];
        g_child_death_read_index++;
        n++;
    }
    return n;
}

int child_death_dropped_count(void)
{
    int d = g_child_death_dropped;
    g_child_death_dropped = 0;
    return d;
}

void child_death_format_reason(int status, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }
    if (WIFEXITED(status)) {
        snprintf(out, out_size, "sortie normale, code %d", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        snprintf(out, out_size, "tué par le signal %s (%d)%s",
                 strsignal(WTERMSIG(status)), WTERMSIG(status),
#ifdef WCOREDUMP
                 WCOREDUMP(status) ? " [core dump]" : ""
#else
                 ""
#endif
                 );
    } else {
        snprintf(out, out_size, "statut brut 0x%x (ni sortie ni signal)", (unsigned)status);
    }
}

int child_death_is_clean_exit(int status)
{
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

void sigchld_handler(int signal) {
	(void)signal;
	// lecture du statut pour éviter les process zombie
	int status = 0;
	pid_t wpid;
#ifdef DEBUG_SIGNAL
    log_debug("sigchld_handler\n");
#endif // DEBUG_SIGNAL
    while(0 < (wpid = waitpid(-1, &status, WNOHANG))) {
        // child_death_record : async-signal-safe (cf. app_runtime.h), capture
        // pid+statut pour un drain/log différé hors contexte signal — voir
        // fork_orchestrator_run. Remplace l'ancien log_debug (DEBUG_SIGNAL
        // uniquement, jamais actif en production) comme seule trace d'une
        // mort d'enfant inattendue.
        child_death_record(wpid, status);
#ifdef DEBUG_SIGNAL
        log_debug("waitpid %d\n", (int)wpid);
        log_debug("Exit status of %d was %d\n", (int)wpid, status);
        if (WIFEXITED(status)) {
            log_debug("Exit value %d\n", WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            log_debug("Killed by %s\n", strsignal(WTERMSIG(status)));
        }
#endif // DEBUG_SIGNAL
    }
#ifdef DEBUG_SIGNAL
    flush_debug();
#endif // DEBUG_SIGNAL
}

/**
 * @brief Initialise les signaux pour les threads enfants.
 */
void init_sigchld_sigaction(void) {
 	struct sigaction sa;
     //memset(&sa, 0, sizeof *sa);
     sa.sa_handler = sigchld_handler;
     sa.sa_flags = SA_SIGINFO|SA_RESTART;
     sigemptyset(&(sa.sa_mask));
     if (sigaction(SIGCHLD, &sa, NULL) != 0) {
         log_error("Problème avec sigaction()\n");
         exit(EXIT_FAILURE);
     }
 }

/**
 * @brief Attend la terminaison de tous les processus enfants (mode client parent).
 *
 * Boucle sur `wait()` jusqu'à ce qu'il n'y ait plus d'enfants. En mode
 * DEBUG_SIGNAL, journalise les codes de sortie de chaque enfant.
 */
void wait_child(void) {
    log_info("start wait_child\n");
    int status = 0;
    pid_t wpid;
    /* Boucle tant que wait() réussit OU est interrompu par un signal.
       Avec ncurses (SIGWINCH au redimensionnement) ou tout autre signal
       sans SA_RESTART, wait() peut retourner -1 avec errno==EINTR : il ne
       faut PAS sortir, sinon le parent terminerait alors que des enfants
       sont encore vivants (qui deviendraient des orphelins). On ne quitte
       que sur ECHILD (plus d'enfants) ou une vraie erreur. */
    while (1) {
        wpid = wait(&status);
        if (wpid > 0) {
#ifdef DEBUG_SIGNAL
            log_debug("Exit status of %d was %d\n", (int)wpid, status);
            if (WIFEXITED(status)) {
                log_debug("Exit value %d\n", WEXITSTATUS(status));
            } else if (WIFSIGNALED(status)) {
                log_debug("Killed by %s\n", strsignal(WTERMSIG(status)));
            }
#else
            (void)wpid;
            (void)status;
#endif // DEBUG_SIGNAL
            continue;
        }
        if (wpid == -1 && errno == EINTR) {
            /* Interrompu par un signal (ex: SIGWINCH installé par ncurses
               sans SA_RESTART). On retente. */
            continue;
        }
        /* Plus d'enfants à attendre (ECHILD) ou erreur fatale : on sort. */
        break;
    }
    log_info("end wait_child\n");
 }
/** @brief Capacité actuellement allouée de `childrens_pid`/`forkId`/`fork_statistics`
 *         (dernier `NB_THREADS` connu de `init_childs`/`ensure_childs_capacity`) —
 *         voir `ensure_childs_capacity` : NB_THREADS peut changer après coup
 *         (`config nb_forks` + `start`), ce compteur permet de savoir de
 *         combien agrandir sans le redéduire de NB_THREADS lui-même (déjà
 *         muté au moment où l'agrandissement est décidé). */
static int g_childs_capacity = 0;

/**
 * @brief Initialise les attributs des threads enfants.
 */
void init_childs(void) {
    g_childs_capacity = NB_THREADS;
    childrens_pid = malloc(sizeof(pid_t) * NB_THREADS);
    forkId = malloc(sizeof(char *) * NB_THREADS);
    fork_statistics = malloc(sizeof(struct client_statistics) * NB_THREADS);
    memset(fork_statistics, 0, sizeof(struct client_statistics) * NB_THREADS);
    for (int c = 0; c < NB_THREADS; c++) {
        childrens_pid[c] = -1;
        forkId[c] = malloc(sizeof(char) * 300);
        forkId[c][0] = '\0';

        fork_statistics[c].analyses_in_stock = 0;
        fork_statistics[c].possibilities_in_stock = 0;
        fork_statistics[c].shots_per_second = 0;
    }
}

void ensure_childs_capacity(int needed) {
    if (needed <= g_childs_capacity) {
        return;
    }
    childrens_pid = realloc(childrens_pid, sizeof(pid_t) * (size_t)needed);
    forkId = realloc(forkId, sizeof(char *) * (size_t)needed);
    fork_statistics = realloc(fork_statistics, sizeof(struct client_statistics) * (size_t)needed);
    for (int c = g_childs_capacity; c < needed; c++) {
        childrens_pid[c] = -1;
        forkId[c] = malloc(sizeof(char) * 300);
        forkId[c][0] = '\0';

        fork_statistics[c].analyses_in_stock = 0;
        fork_statistics[c].possibilities_in_stock = 0;
        fork_statistics[c].shots_per_second = 0;
    }
    g_childs_capacity = needed;
}

void free_childs(void) {
    if (forkId != NULL) {
        for (int c = 0; c < g_childs_capacity; c++) {
            free(forkId[c]);
        }
    }
    free(childrens_pid);
    free(forkId);
    free(fork_statistics);
    childrens_pid = NULL;
    forkId = NULL;
    fork_statistics = NULL;
    g_childs_capacity = 0;
}

int pid_is_alive(pid_t pid)
{
    if (pid <= 0) {
        return 0;
    }
    if (kill(pid, 0) == 0) {
        return 1;
    }
    return errno != ESRCH;
}

int reap_dead_child_slots(pid_t *childrens_pid, char **forkId,
                          struct client_statistics *fork_statistics,
                          int nb, child_pid_alive_fn alive)
{
    if (childrens_pid == NULL || forkId == NULL || fork_statistics == NULL) {
        return 0;
    }
    if (alive == NULL) {
        alive = pid_is_alive;
    }
    int cleaned = 0;
    for (int c = 0; c < nb; c++) {
        if (childrens_pid[c] <= 0) {
            continue;
        }
        if (alive(childrens_pid[c])) {
            continue;
        }
        childrens_pid[c] = -1;
        if (forkId[c] != NULL) {
            forkId[c][0] = '\0';
        }
        memset(&fork_statistics[c], 0, sizeof(fork_statistics[c]));
        cleaned++;
    }
    return cleaned;
}

void resolve_client_label(const char *cli_label, const char *hostname_or_null,
                           char *out, size_t out_size)
{
	if (out == NULL || out_size == 0) {
		return;
	}
	const char *source = "?";
	if (cli_label != NULL && cli_label[0] != '\0') {
		source = cli_label;
	} else if (hostname_or_null != NULL && hostname_or_null[0] != '\0') {
		source = hostname_or_null;
	}
	size_t len = strlen(source);
	if (len >= out_size) {
		len = out_size - 1;
	}
	memcpy(out, source, len);
	out[len] = '\0';
}

void init_client_identity(void)
{
	memset(&g_client_identity_template, 0, sizeof(g_client_identity_template));

	machine_uid_status_t st = machine_uid_load_or_create(machine_uid_file_path,
	                                                      g_client_identity_template.machine_uid);
	if (st == MACHINE_UID_CREATED) {
		log_info("identité : nouveau machine_uid généré et enregistré dans \"%s\"\n",
		         machine_uid_file_path);
	} else if (st == MACHINE_UID_VOLATILE) {
		log_error("identité : impossible d'écrire le machine_uid dans \"%s\" — "
		          "identité volatile pour cette exécution\n", machine_uid_file_path);
	}

	if (client_identity_random_bytes(g_client_identity_template.client_uid, CLIENT_UID_BYTES) != 0) {
		log_error("identité : génération du client_uid échouée — champ laissé à zéro\n");
	}

	g_client_identity_template.fork_seq = -1;
#ifdef WITH_CUDA
	g_client_identity_template.mode = (uint8_t)(gpu_pruner_mode ? CLIENT_MODE_GPU_PRUNER
	                                             : (pruner_mode ? CLIENT_MODE_PRUNER : CLIENT_MODE_SEARCH));
#else
	g_client_identity_template.mode = (uint8_t)(pruner_mode ? CLIENT_MODE_PRUNER : CLIENT_MODE_SEARCH);
#endif

	char hostname[CLIENT_LABEL_MAX];
	hostname[0] = '\0';
	const char *hostname_ptr = NULL;
	if (gethostname(hostname, sizeof(hostname)) == 0) {
		hostname[CLIENT_LABEL_MAX - 1] = '\0';
		hostname_ptr = hostname;
	}
	resolve_client_label(client_label, hostname_ptr,
	                      g_client_identity_template.label, CLIENT_LABEL_MAX);
}

/**
 * @brief Initialise les gestionnaires de signaux pour l'application.
 *
 * Cette fonction configure les gestionnaires de signaux nécessaires pour s'assurer que
 * l'application peut gérer divers signaux de manière appropriée. Elle est
 * généralement appelée pendant la phase d'initialisation du programme.
 */
void init_signals(void) {
    struct sigaction sa;
    sa.sa_handler = signal_end_handler;
    /* Pas de SA_RESTART : on veut que les appels bloquants (accept, recvfrom,
       wait) renvoient EINTR sur réception d'un signal d'arrêt, afin que leurs
       boucles puissent constater request==REQUEST_STOP et sortir proprement. */
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);

    // Configure les signaux pour le processus principal et les threads enfants
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // Ignorer SIGPIPE
    signal(SIGPIPE, signal_ignored);
}


/**
 * @brief Configure les signaux pour les threads enfants.
 *
 * Cette fonction est appelée dans chaque thread enfant pour s'assurer qu'ils
 * écoutent les signaux comme SIGINT.
 */
void configure_child_signals(void) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);

    struct sigaction sa;
    sa.sa_handler = signal_end_handler;
    /* Pas de SA_RESTART — même rationale qu'init_signals() ci-dessus, dont
       cette fonction contredisait à tort le choix : sigaction() est un
       réglage PROCESS-WIDE (seul le masque bloqué est par-thread), donc
       l'appeler ici — depuis le thread fork_udp d'un fork de recherche,
       après init_signals() côté parent avant le fork() — REMPLACE le SA_RESTART=0
       hérité par SA_RESTART=1 pour tout le process enfant. Avec SA_RESTART, un
       SIGINT/SIGTERM reçu pendant un appel bloquant (le `recvfrom()` de
       fork_udp lui-même, qui n'a AUCUN timeout et bloque la quasi-totalité du
       temps d'un fork inactif ; ou le `connect()` bloquant de
       `create_tcp_client`, qui n'est pas borné par SO_RCVTIMEO/SO_SNDTIMEO)
       fait juste RELANCER silencieusement l'appel interrompu au lieu de
       renvoyer EINTR — la boucle appelante ne voit alors JAMAIS
       request==REQUEST_STOP, et le fork reste sourd à stopForks/configApply/
       exit jusqu'à l'escalade SIGTERM (elle aussi absorbée par le même
       mécanisme) puis SIGKILL. Bogue réel reproduit deux fois de suite en
       conditions réelles : un fork resté à 0 (aucun travail) qui a mis plus
       de 10 s à mourir sur `exit` (`orchestrateur : X fils encore vivant(s)
       après 10s — escalade SIGKILL`), quand ses deux frères — occupés par du
       calcul CPU, jamais bloqués dans un appel système au moment du SIGINT —
       mouraient proprement en une fraction de seconde. Voir
       docs/echanges_client_serveur.md. */
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);

    // Configure SIGINT pour les threads enfants
    sigaction(SIGINT, &sa, NULL);
}

/* ---- IPC parent<->enfants (sockets Unix UDP locales) ---- */

void *fork_checker(void *param) {
	struct sockaddr_un *main_addr = (struct sockaddr_un *)param;
	char socket_fork[50];
    int sp_len = sprintf(socket_fork, "etii_fork.%d", getpid());
    socket_fork[sp_len] = '\0';
    struct sockaddr_un *fork_addr = build_sockaddr(socket_fork);
#ifdef DEBUG_LOCAL_SOCKET
    log_debug("socket fork : %s\n", socket_fork);
#endif // DEBUG_LOCAL_SOCKET
    fork_checker_socket_id = build_udp_local_socket(fork_addr);
    free(fork_addr);

	log_info("fork_checker_socket_id: %i\n", fork_checker_socket_id);
	if (fork_checker_socket_id > 0) {
		int *so = &fork_checker_socket_id;
		run_fork_thread(so);
	}

    // TPS tests per second (5 secondes)
    unsigned long long oldSPS[5];
    // Débit des études de prunage : même fenêtre glissante, sur pruner_cells_studied
    unsigned long long oldPPS[5];
    for (int c = 0; c < 5; c++) {
        oldSPS[c] = 0;
        oldPPS[c] = 0;
    }
    int s = 0;
    int t;
    unsigned long long last_counter = 0;
    unsigned long long last_prune_cells = 0;
    struct client_statistics *statistic = calloc(1, sizeof(struct client_statistics));
	while(request != REQUEST_STOP && fork_checker_socket_id > 0) {
        unsigned long long counter = 0;
        unsigned long long possibilities_in_stock = 0;
        for (t = 0; t < NB_THREADS; t++) {
            counter += counters[t];
            possibilities_in_stock += lastfilesize[t];
        }
        unsigned long long sps = 0;
        if (counter >= last_counter) {
            sps = counter - last_counter;
        } else {
            // le compteur a fait un tour
            sps = ((sps - 1) - last_counter) + counter;
        }
        last_counter = counter;
        oldSPS[s] = sps;

        // Même mécanique pour le débit des études de prunage : cumul des cases
        // étudiées par les contrôles de possibilité (pruner, rmnonext) ET par
        // le forward-checking de la recherche — flux disjoint de `counters`.
        unsigned long long prune_cells = pruner_cells_studied
            + __atomic_load_n(&fc_cells_studied, __ATOMIC_RELAXED);
        unsigned long long pps = 0;
        if (prune_cells >= last_prune_cells) {
            pps = prune_cells - last_prune_cells;
        } else {
            // le compteur a fait un tour
            pps = ((pps - 1) - last_prune_cells) + prune_cells;
        }
        last_prune_cells = prune_cells;
        oldPPS[s] = pps;

        s++;
        if (s >= 5) {
            s = 0;
        }

        // on effectue une moyenne sur 5 secondes
        // les valeurs à 0 ne sont pas comptées
        int m = 0;
        for (int i = 0; i < 5; i++) {
            if (oldSPS[i] > 0) {
                m++;
                sps += oldSPS[i];
            }
        }
        if (m > 0) {
            sps = sps / m;
        } else {
            sps = 0;
        }
        statistic->shots_per_second = sps;

        int mp = 0;
        for (int i = 0; i < 5; i++) {
            if (oldPPS[i] > 0) {
                mp++;
                pps += oldPPS[i];
            }
        }
        if (mp > 0) {
            pps = pps / mp;
        } else {
            pps = 0;
        }
        statistic->pruner_cells_per_second = pps;

        int analyses_in_stock = 0;
        for (int f = 0; f < NB_FILE_POSSIBILITY; f++) {
            analyses_in_stock += file_analysed_size(f);
        }
        statistic->analyses_in_stock = analyses_in_stock;
        statistic->possibilities_in_stock = possibilities_in_stock;
        statistic->max_result = max_result;
#if FORWARD_CHECK_K > 0
        // Statistiques du forward-checking : cumuls du processus, agrégés et
        // affichés par le parent dans le rapport de la commande `check`.
        statistic->fc_attempts = __atomic_load_n(&fc_attempts, __ATOMIC_RELAXED);
        statistic->fc_pruned = __atomic_load_n(&fc_pruned, __ATOMIC_RELAXED);
        // Borné par FC_STAT_MAX_K (indépendant de FORWARD_CHECK_K depuis le
        // passage de bt_forward_check aux voisines) et non par FORWARD_CHECK_K :
        // sinon un FORWARD_CHECK_K < 4 tronquerait la copie des positions 1..4
        // que la boucle chaude peut effectivement produire — cf. le commentaire
        // de fc_pruned_at dans static_variables.h.
        for (int j = 1; j <= FC_STAT_MAX_K; j++) {
            statistic->fc_pruned_at[j] = __atomic_load_n(&fc_pruned_at[j], __ATOMIC_RELAXED);
        }
#endif // FORWARD_CHECK_K > 0
        // Statistiques du client pruner (restent à zéro en mode recherche)
        statistic->pruner_checked = pruner_checked;
        statistic->pruner_removed = pruner_removed;
        statistic->pruner_cells_studied = pruner_cells_studied;
        /* On préfixe le datagramme d'un octet de type pour permettre au
           parent de multiplexer stats / logs / événements sur le même
           socket. Voir ipc_protocol.h. */
        char ipcbuf[1 + sizeof(struct client_statistics)];
        ipcbuf[0] = IPC_MSG_STATS;
        memcpy(ipcbuf + 1, statistic, sizeof(struct client_statistics));
        if (sendto(fork_checker_socket_id, ipcbuf, sizeof ipcbuf, MSG_DONTWAIT,
                   (struct sockaddr *) main_addr, sizeof(struct sockaddr_un))
            != (ssize_t)sizeof ipcbuf) {
            /* Échec visible UNE fois par processus (pas à chaque seconde) :
               un envoi de stats qui échoue en continu (ex. EMSGSIZE quand le
               datagramme dépassait la limite AF_UNIX de macOS, avant que
               build_udp_local_socket ne relève SO_SNDBUF) rendait le parent
               aveugle — aucune stat, aucun record — sans aucune trace hors
               DEBUG_LOCAL_SOCKET. Un message court passe toujours (les
               limites en jeu dépassent largement une ligne de log). */
            static int stats_send_warned = 0;
            if (!stats_send_warned) {
                stats_send_warned = 1;
                log_error("stats IPC : sendto de %zu octets vers %s a échoué : %s "
                          "— le parent ne recevra pas les statistiques de ce fork\n",
                          sizeof ipcbuf, main_addr->sun_path, strerror(errno));
            }
#ifdef DEBUG_LOCAL_SOCKET
            log_debug("fork_checker cl %d error %i sendto : %s\n", getpid(), errno, strerror(errno));
#endif // DEBUG_LOCAL_SOCKET
        }

        // Représentation du meilleur plateau LOCAL à ce fork : envoyée en plus
        // des stats, mais UNIQUEMENT quand ce fork bat son propre record
        // (jamais à chaque tour, contrairement à IPC_MSG_STATS ci-dessus) —
        // cf. core/best_board.h. `last_sent_best_board` est statique au thread :
        // un fork ne renvoie donc le plateau qu'une seule fois par record.
        {
            static uint16_t last_sent_best_board = 0;
            struct possibility_packet local_board;
            uint16_t local_alloc = 0;
            if (best_board_get(&g_search_best_board, &local_board, &local_alloc)
                && local_alloc > last_sent_best_board) {
                char boardbuf[1 + sizeof(struct possibility_packet)];
                boardbuf[0] = IPC_MSG_BEST_BOARD;
                memcpy(boardbuf + 1, &local_board, sizeof(local_board));
                sendto(fork_checker_socket_id, boardbuf, sizeof boardbuf, MSG_DONTWAIT,
                       (struct sockaddr *) main_addr, sizeof(struct sockaddr_un));
                last_sent_best_board = local_alloc;
            }
        }
		sleep(1);
	}
    free(statistic);

	return NULL;
}

int run_fork_checker(struct sockaddr_un *main_addr)
{
	pthread_attr_t *thread_attributes = malloc(sizeof *thread_attributes);
	pthread_attr_init(thread_attributes);
	pthread_attr_setdetachstate(thread_attributes, PTHREAD_CREATE_DETACHED);
	pthread_t thread;

    // Start the fork checker.
	if(0 != pthread_create(&thread, thread_attributes, fork_checker, main_addr))
	{
		log_error("Problème avec pthread_create()\n");
		free(thread_attributes);
		exit(EXIT_FAILURE);
	}

    // Clean up the thread attributes.
	pthread_attr_destroy(thread_attributes);
	free(thread_attributes);

	return 0;
}

/* Borne l'attente de server_tcp sur recvfrom() (cf. fork_gate_checkpoint
   ci-dessous) : sans timeout, un process parent sans aucun fork vivant (ou
   entre deux tours de fork_checker, 1/s) bloquerait indéfiniment dans le
   noyau sans jamais revoir la tête de boucle — le checkpoint de quiescence
   coopérative ci-dessous ne serait alors observé qu'au prochain datagramme
   reçu, pas dans un délai borné. Valeur courte :
   ce thread ne fait rien d'autre qu'attendre, un réveil par seconde est sans
   coût mesurable. */
#define SERVER_TCP_RECV_TIMEOUT_SEC 1

void *server_tcp(void *param) {
    int socket_id = *(int*)param;

    struct timeval tv;
    tv.tv_sec = SERVER_TCP_RECV_TIMEOUT_SEC;
    tv.tv_usec = 0;
    setsockopt(socket_id, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_un *claddr = malloc(sizeof(struct sockaddr_un));
    ssize_t numBytes;
    socklen_t len = sizeof(struct sockaddr_un);

    /* Tampon dimensionné pour le plus gros message attendu (stats, plateau
       record, ligne de log) — même source de vérité que les tampons socket
       de build_udp_local_socket. */
    size_t bufsz = ipc_max_datagram();
    char *buf = malloc(bufsz);

    int gate_slot = fork_gate_register("server_tcp");

    while (request != REQUEST_STOP) {
        fork_gate_checkpoint(gate_slot);
        if (request == REQUEST_STOP) {
            break;
        }
        len = sizeof(struct sockaddr_un);
        numBytes = recvfrom(socket_id, buf, bufsz, 0,
                            (struct sockaddr *) claddr, &len);
        if (numBytes == -1) {
            if (request != REQUEST_STOP) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    /* Timeout attendu (SO_RCVTIMEO ci-dessus) : simple
                       retour à la tête de boucle pour revoir le checkpoint. */
                    continue;
                }
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EBADF) {
#ifdef DEBUG_LOCAL_SOCKET
                    log_debug("srv error invalid descriptor on recvfrom\n");
                    flush_debug();
#endif // DEBUG_LOCAL_SOCKET
                    break;
                }
                log_errno("srv error on recvfrom => ");
                flush_error();
            }
            continue;
        }
        if (numBytes < 1) {
            continue;
        }

        /* recvfrom() n'est PAS garanti terminer sun_path par un octet nul :
         * le noyau n'écrit que `len` octets (convention SUN_LEN, qui exclut
         * le terminateur). Sans ce nul explicite, find_fork_index() (qui
         * compare via strcmp) peut lire au-delà de l'adresse reçue, dans la
         * mémoire non initialisée de `claddr` (malloc, jamais remis à zéro). */
        {
            size_t addr_path_len = (size_t)len - offsetof(struct sockaddr_un, sun_path);
            if (addr_path_len >= sizeof(claddr->sun_path)) {
                addr_path_len = sizeof(claddr->sun_path) - 1;
            }
            claddr->sun_path[addr_path_len] = '\0';
        }

        int8_t type = (int8_t)buf[0];
        switch (type) {
            case IPC_MSG_STATS:
                if (numBytes >= (ssize_t)(1 + sizeof(struct client_statistics))) {
                    int cpt = find_fork_index(claddr->sun_path, forkId, NB_THREADS);
                    if (cpt >= 0) {
                        memcpy(&fork_statistics[cpt], buf + 1,
                               sizeof(struct client_statistics));
                    } else {
                        // Un datagramme de stats dont l'expéditeur ne correspond
                        // à AUCUN slot connu (forkId[]) était jusqu'ici jeté en
                        // silence — un fork bien vivant, envoyant correctement
                        // ses stats, pouvait donc rester invisible côté parent
                        // sans la moindre trace si son socket source ne
                        // correspondait plus à l'entrée attendue (ex. tableau
                        // reconstruit entre-temps par un `configApply`). Un seul
                        // avertissement par chemin de socket inconnu (pas un par
                        // seconde, ce message reviendrait sinon en boucle tant
                        // que ce fork continue d'émettre) pour signaler la
                        // désynchronisation sans noyer les logs.
                        static char last_unknown_sender[sizeof(claddr->sun_path)] = {0};
                        if (strncmp(last_unknown_sender, claddr->sun_path,
                                    sizeof(last_unknown_sender)) != 0) {
                            // memcpy plutôt que strncpy : claddr->sun_path est
                            // déjà explicitement NUL-terminé DANS ses bornes
                            // quelques lignes plus haut dans cette même
                            // fonction (recvfrom() ne le garantit pas), donc
                            // les deux tampons ont la même taille et copier
                            // le tampon entier reste toujours borné et
                            // termine correctement — gcc/ARM (-Wstringop-truncation)
                            // ne peut pas le prouver pour strncpy, qui ne
                            // garantit d'ailleurs pas la terminaison NUL en
                            // cas de troncature (même piège déjà rencontré
                            // sur http_known_clients_collect/http_clients_collect,
                            // cf. AGENTS.md).
                            memcpy(last_unknown_sender, claddr->sun_path,
                                   sizeof(last_unknown_sender));
                            log_error("stats IPC : datagramme reçu de \"%s\", "
                                      "qui ne correspond à aucun fork connu — "
                                      "statistiques ignorées\n", claddr->sun_path);
                        }
                    }
                }
                break;

            case IPC_MSG_BEST_BOARD:
                if (numBytes >= (ssize_t)(1 + sizeof(struct possibility_packet))) {
                    struct possibility_packet board;
                    memcpy(&board, buf + 1, sizeof(board));
                    // Agrégat du process PARENT sur ses forks : même règle
                    // « premier à dépasser gagne » que g_search_best_board
                    // côté fork (cf. core/best_board.h). C'est cette instance
                    // que le canal de contrôle sert en réponse à
                    // CTRL_GET_BEST_BOARD (etii_control.c).
                    best_board_try_record(&g_client_aggregate_best_board, &board, board.alloc);
                }
                break;

            case IPC_MSG_LOG_INFO:
                buf[numBytes] = '\0';
                log_info("%s", buf + 1);
                break;
            case IPC_MSG_LOG_ERROR:
                buf[numBytes] = '\0';
                log_error("%s", buf + 1);
                break;
            case IPC_MSG_LOG_DEBUG:
                buf[numBytes] = '\0';
                log_debug("%s", buf + 1);
                break;
            case IPC_MSG_LOG_CONSOLE:
                buf[numBytes] = '\0';
                log_console("%s", buf + 1);
                break;
            case IPC_MSG_EVENT:
                buf[numBytes] = '\0';
                log_event("%s", buf + 1);
                break;

            default:
                /* Type inconnu : on ignore silencieusement (compat avenir). */
                break;
        }
    }
    fork_gate_unregister(gate_slot);
    free(claddr);
    free(buf);

    return NULL;
}

void run_server_thread(int *socket_id) {
    log_info("srv  socket_id %i\n", *socket_id);
    pthread_attr_t *thread_attributes = malloc(sizeof *thread_attributes);
    pthread_attr_init(thread_attributes);
    pthread_attr_setdetachstate(thread_attributes, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    if(0 != pthread_create(&thread, thread_attributes, server_tcp, socket_id))
        {
            // Non fatal : on poursuit sans thread de réception des statistiques
            // plutôt que de planter (et d'orphaniser les process enfants).
            log_error("run_server_thread : pthread_create a échoué — pas de réception de statistiques\n");
            free(thread_attributes);
            return;
        }
        pthread_attr_destroy(thread_attributes);
        free(thread_attributes);
}

void *fork_udp(void *param) {
    // Configure les signaux pour ce thread
    configure_child_signals();

	int socket_id = *(int*)param;
    struct sockaddr_un *srv_addr = malloc(sizeof(struct sockaddr_un));
    ssize_t numBytes;
    socklen_t len = sizeof(struct sockaddr_un);
    char *value = malloc(sizeof(char) * 100);
    while (request != REQUEST_STOP) {
        numBytes = recvfrom(socket_id, value, sizeof(char) * 100, 0,
                            (struct sockaddr *) srv_addr, &len);
        if (numBytes == -1) {
            if (request != REQUEST_STOP) {
                log_errno("cl error on recvfrom => ");
                flush_error();
            }
            continue;
        }
		value[numBytes] = '\0';
        do_command_line(value);
    }
    free(srv_addr);
    free(value);
    return NULL;
}

void run_fork_thread(int *socket_id) {
	log_info("cl socket_id %i\n", *socket_id);
	pthread_attr_t *thread_attributes = malloc(sizeof *thread_attributes);
    pthread_attr_init(thread_attributes);
    pthread_attr_setdetachstate(thread_attributes, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    if(0 != pthread_create(&thread, thread_attributes, fork_udp, socket_id))
	{
		// Non fatal : ce process enfant tourne sans thread de commandes IPC
		// plutôt que de mourir sous la pression des ressources.
		log_error("run_fork_thread : pthread_create a échoué — pas de commandes IPC pour ce process\n");
		free(thread_attributes);
		return;
	}
	pthread_attr_destroy(thread_attributes);
	free(thread_attributes);
}

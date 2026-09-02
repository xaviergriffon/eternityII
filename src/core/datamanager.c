#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <netdb.h>
#include <unistd.h>

#include "ui/logger.h"
#include "core/core_static_variables.h"
// core/ ne doit normalement dépendre que de core_static_variables.h — cette
// inclusion de app/ reste une exception documentée (cf. la note en tête de
// core/core_static_variables.h) : ce fichier lit directement `version`,
// `server`, `SERVER_PORT`, `pruner_mode`, `g_client_identity_template` et
// les options d'expansion/bail (protocole réseau et configuration serveur).
#include "app/app_static_variables.h"
#include "core/lifo.h"
#include "core/datamanager.h"
#include "core/stock_rate.h"
#include "net/tcpclient.h"
#include "net/etii_protocol.h"
#include "core/readdata.h"

// Ces trois pools et analysed_index plus bas sont des tableaux de POINTEURS, (ré)alloués par
// datamanager_configure_stock_files — le coût mémoire suit nb_file_possibility,
// jamais un plafond pré-alloué (cf. le commentaire de NB_FILE_POSSIBILITY_MAX,
// datamanager.h). Valent NULL/0 tant que datamanager_configure_stock_files n'a
// pas été appelée — appel OBLIGATOIRE avant tout autre usage de ce fichier,
// cf. la doc de nb_file_possibility (datamanager.h) pour la liste des points
// d'entrée qui l'appellent.
int nb_file_possibility = 0;
static int nb_file_possibility_capacity = 0; // NOMBRE DE FILES RÉELLEMENT ALLOUÉES (>= nb_file_possibility ; ne décroît jamais, cf. datamanager_configure_stock_files)

// Compteurs round-robin de démarrage pour put_to_pool/scroll_from_pool — un par
// pool réellement distinct (non vérifié / vérifié), jamais remis à 0. Partagés
// entre TOUS les appelants d'un même pool (ex. rr_scroll_unchecked sert aussi
// bien scroll_from_local que scroll_from_local_tocheck) : c'est le trafic
// combiné qui doit tourner, pas seulement celui d'un point d'appel isolé. Cf.
// datamanager_rr_next_start.
static unsigned int rr_put_unchecked = 0;
static unsigned int rr_put_checked = 0;
static unsigned int rr_scroll_unchecked = 0;
static unsigned int rr_scroll_checked = 0;

// Débit d'ajouts/consommations du stock (core/stock_rate.h), tous pools
// confondus (non vérifié + vérifié) — instrumenté depuis put_to_pool/
// scroll_from_pool ci-dessous. Lu par statistic_datas() (console) et
// datamanager_stock_rate_stats() (exposé à l'API HTTP via http_stats_collect).
static stock_rate_counter_t stock_adds_rate;
static stock_rate_counter_t stock_removes_rate;

// Même débit, VENTILÉ par pool : l'agrégat ci-dessus ne dit pas si les GET
// consommés viennent du pool non
// vérifié (pruners) ou vérifié (chercheurs), ni si les ADD alimentent l'un ou
// l'autre. Enregistrés EN PLUS de l'agrégat (jamais à sa place — aucun
// consommateur existant de stock_adds_rate/stock_removes_rate ne doit changer
// de comportement), depuis les mêmes sites, avec le pool déjà connu de
// l'appelant (paramètre `want_checked`/`pool` de put_to_pool/scroll_from_pool).
static stock_rate_counter_t stock_adds_unchecked_rate;
static stock_rate_counter_t stock_adds_checked_rate;
static stock_rate_counter_t stock_removes_unchecked_rate;
static stock_rate_counter_t stock_removes_checked_rate;

int datamanager_rr_next_start(unsigned int *counter, int n)
{
	if (n <= 0)
	{
		return 0;
	}
	unsigned int prev = __atomic_fetch_add(counter, 1u, __ATOMIC_RELAXED);
	return (int)(prev % (unsigned int)n);
}

// Réservée aux tests (jamais appelée en production, non déclarée dans
// datamanager.h — même convention que les autres « helpers internes » que
// tests/core/test_datamanager.c déclare lui-même en tête de fichier) : remet
// à zéro l'état round-robin ADD/GET pour que les assertions « tout atterrit
// dans la file 0 juste après un drain » restent déterministes quel que soit
// l'ordre d'exécution des tests.
void datamanager_reset_rr_state_for_tests(void)
{
	rr_put_unchecked = 0;
	rr_put_checked = 0;
	rr_scroll_unchecked = 0;
	rr_scroll_checked = 0;
}

// Réservée aux tests (même convention que datamanager_reset_rr_state_for_tests
// ci-dessus) : isole les compteurs de débit ADD/GET entre deux cas de test.
void datamanager_reset_stock_rate_counters_for_tests(void)
{
	stock_rate_reset_for_tests(&stock_adds_rate);
	stock_rate_reset_for_tests(&stock_removes_rate);
	stock_rate_reset_for_tests(&stock_adds_unchecked_rate);
	stock_rate_reset_for_tests(&stock_adds_checked_rate);
	stock_rate_reset_for_tests(&stock_removes_unchecked_rate);
	stock_rate_reset_for_tests(&stock_removes_checked_rate);
}

static file_possibility_t **file_possibility = NULL;
static file_possibility_t **file_possibility_analysed = NULL;

/**
 * @brief Files des possibilités vérifiées par un client pruner (`checked == 1`).
 *
 * Servies en priorité aux clients de recherche (INST_GET). Le pool historique
 * `file_possibility` reçoit les possibilités non vérifiées et alimente les
 * clients pruners (INST_GET_TO_CHECK) ainsi que, en repli, les clients de
 * recherche quand ce pool-ci est vide (fonctionnement sans pruner inchangé).
 */
static file_possibility_t **file_possibility_checked = NULL;

/* ==========================================================================
 * Index de recherche du pool « analysed » (accélère remove_possibility_analysed)
 * ==========================================================================
 *
 * remove_possibility_analysed() balayait linéairement toute la file avec
 * compare_possibility() : O(n) par retrait. Cet index (table de hachage par
 * file, chaînage séparé) ramène le cas courant à O(1) amorti.
 *
 * Le hash ne porte que sur les champs comparés par compare_possibility()
 * (alloc, x, y, les ETERN_PARTS premiers bits de b_faceused, grid), jamais
 * sur l'image mémoire brute : b_faceused porte un padding d'alignement non
 * garanti initialisé, qui ferait diverger le hash de deux paquets pourtant
 * égaux.
 *
 * L'index n'est jamais une source de vérité : sur un miss, l'appelant
 * retombe sur le balayage linéaire seulement si
 * `analysed_index_may_be_incomplete` l'exige — sinon un miss signifie une
 * absence réelle, et le repli O(n) régresserait le chemin le plus fréquent.
 */
#define ANALYSED_INDEX_BUCKETS 8191

/**
 * @brief 0 tant qu'AUCUN `analysed_index_add` n'a jamais échoué (OOM) sur ce
 *        process, 1 dès le premier échec — jamais remis à 0.
 *
 * Tant qu'il vaut 0, l'index est garanti EXHAUSTIF (chaque `put()` réussi
 * dans `file_possibility_analysed` est indexé dans la foulée) : un miss de
 * `analysed_index_find_and_remove` signifie alors une absence RÉELLE, jamais
 * un défaut d'indexation, et `remove_possibility_analysed` peut sauter son
 * repli par balayage linéaire sans risque. Bascule à 1 dès le premier échec
 * d'allocation d'un nœud d'index et le reste pour tout le process : quelques
 * balayages de repli superflus après un unique OOM coûtent moins cher qu'un
 * second OOM silencieusement mal couvert.
 */
static int analysed_index_may_be_incomplete = 0;

/*
 * Attribution des analyses en cours: `owner_uid` est le `client_uid` du
 * client à qui LE SERVEUR a servi cette possibilité (INST_GET /
 * INST_GET_TO_CHECK[_BATCH]). C'est une table latérale adossée à
 * `analysed_index`, jamais un champ ajouté à `possibility_packet` (fil +
 * backups + padding caché, cf. possibility-packet-struct-padding), et jamais
 * une surcharge du paramètre `thread` (déjà -1 côté serveur / index de thread
 * côté client, deux sens distincts qu'il ne faut pas empiler d'un troisième).
 * `has_owner == 0` côté client (thread >= 0, cf. add_possibility_analysed) et
 * pour toute possibilité rechargée par `import_analysed`/`restore_analysed` :
 * les baux ne sont pas persistés, l'attribution ne l'est donc pas davantage —
 * au redémarrage, une possibilité restaurée est réputée sans propriétaire,
 * comme avant cette PR.
 */
typedef struct AnalysedIndexNode {
	uint64_t hash;
	Element *element;
	uint8_t owner_uid[CLIENT_UID_BYTES];
	int has_owner;
	/*
	 * Échéance du bail (section 4.3): instant (epoch) au-delà duquel
	 * cette possibilité, si toujours non acquittée, est réputée abandonnée
	 * et rendue au stock par datamanager_reclaim_expired_leases(). Valide
	 * seulement si has_owner (une possibilité sans propriétaire connu n'a pas
	 * de bail — rien à rendre à personne). 0 = bail désactivé
	 * (analysed_lease_seconds <= 0 au moment de l'insertion) : jamais expiré.
	 */
	time_t lease_deadline;
	struct AnalysedIndexNode *next;
} AnalysedIndexNode;

// Tableau de POINTEURS (un par file) vers un tableau de
// ANALYSED_INDEX_BUCKETS pointeurs chacun — alloué par
// datamanager_configure_stock_files, comme les trois pools ci-dessus.
// analysed_index[fileidx][bucket] reste une expression valide identique à
// avant (déréférencement à deux niveaux), seule l'ALLOCATION change.
static AnalysedIndexNode ***analysed_index = NULL;

int datamanager_configure_stock_files(int n)
{
	if (n <= 0) {
		return -1;
	}
	if (n > NB_FILE_POSSIBILITY_MAX) {
		n = NB_FILE_POSSIBILITY_MAX;
	}
	if (n > nb_file_possibility_capacity) {
		file_possibility_t **grown_stock = realloc(file_possibility, (size_t)n * sizeof(file_possibility_t *));
		file_possibility_t **grown_checked = realloc(file_possibility_checked, (size_t)n * sizeof(file_possibility_t *));
		file_possibility_t **grown_analysed = realloc(file_possibility_analysed, (size_t)n * sizeof(file_possibility_t *));
		AnalysedIndexNode ***grown_index = realloc(analysed_index, (size_t)n * sizeof(AnalysedIndexNode **));
		// realloc NE LIBÈRE JAMAIS l'original en cas d'échec : chaque pointeur
		// "grown_*" réussi remplace le global correspondant même si un AUTRE a
		// échoué — pas de fuite, pas de pointeur pendant. nb_file_possibility_capacity
		// n'avance pas dans ce cas : le prochain appel retente depuis la même base.
		if (grown_stock != NULL) { file_possibility = grown_stock; }
		if (grown_checked != NULL) { file_possibility_checked = grown_checked; }
		if (grown_analysed != NULL) { file_possibility_analysed = grown_analysed; }
		if (grown_index != NULL) { analysed_index = grown_index; }
		if (grown_stock == NULL || grown_checked == NULL || grown_analysed == NULL || grown_index == NULL) {
			log_error("datamanager_configure_stock_files : realloc échoué pour %d files\n", n);
			return -1;
		}

		for (int fp = nb_file_possibility_capacity; fp < n; fp++) {
			file_possibility[fp] = malloc(sizeof(file_possibility_t));
			file_possibility_checked[fp] = malloc(sizeof(file_possibility_t));
			file_possibility_analysed[fp] = malloc(sizeof(file_possibility_t));
			analysed_index[fp] = calloc(ANALYSED_INDEX_BUCKETS, sizeof(AnalysedIndexNode *));
			if (file_possibility[fp] == NULL || file_possibility_checked[fp] == NULL ||
			    file_possibility_analysed[fp] == NULL || analysed_index[fp] == NULL) {
				// nb_file_possibility_capacity n'avance pas au-delà de ce qui a
				// RÉUSSI (mise à jour seulement après la boucle complète) : les
				// quelques slots [nb_file_possibility_capacity, fp] déjà alloués
				// ici restent orphelins mais jamais indexés (nb_file_possibility
				// non plus avancé) — fuite mineure, jamais un déréférencement.
				log_error("datamanager_configure_stock_files : allocation échouée pour la file %d\n", fp);
				return -1;
			}
			init_file(&file_possibility[fp]->file, sizeof(struct possibility_packet));
			pthread_mutex_init(&file_possibility[fp]->lock, NULL);
			init_file(&file_possibility_checked[fp]->file, sizeof(struct possibility_packet));
			pthread_mutex_init(&file_possibility_checked[fp]->lock, NULL);
			init_file(&file_possibility_analysed[fp]->file, sizeof(struct possibility_packet));
			pthread_mutex_init(&file_possibility_analysed[fp]->lock, NULL);
		}
		nb_file_possibility_capacity = n;
	}
	nb_file_possibility = n;
	return 0;
}

/**
 * @brief Surcoût d'allocateur estimé, en octets, par appel `malloc()` réussi
 *        (en-tête d'allocateur + arrondi interne). `put()` (`core/lifo.c`) en
 *        paie DEUX par possibilité stockée (l'`Element` et sa valeur) — cf.
 *        `datamanager_bytes_per_possibility`.
 */
#define DATAMANAGER_MALLOC_OVERHEAD 16

/**
 * @brief Plafond RAM actif du stock, en NOMBRE de possibilités — publié par
 *        `datamanager_configure_ram_limit`, lu par `put_to_pool`. 0 = illimité
 *        (comportement historique, avant l'introduction de ce plafond).
 */
static unsigned long long stock_max_ram_packets = 0;

/**
 * @brief Throttling du log « plafond RAM atteint » (`put_to_pool`) : un refus
 *        peut se produire à chaque ADD sous charge soutenue, pas question
 *        d'inonder les logs à ce rythme (contrairement à un refus de
 *        contention passager, celui-ci signale une action opérateur réelle —
 *        resserrer le débit, relever le plafond ou activer le débordement
 *        — donc il DOIT rester visible, juste pas à chaque appel).
 */
static time_t last_ram_cap_warning = 0;
#define STOCK_RAM_CAP_WARN_COOLDOWN_SEC 10

unsigned long long datamanager_bytes_per_possibility(void)
{
	return (unsigned long long)sizeof(Element) + (unsigned long long)sizeof(struct possibility_packet)
	       + 2ULL * DATAMANAGER_MALLOC_OVERHEAD;
}

unsigned long long datamanager_ram_limit_to_packets(int megabytes)
{
	if (megabytes <= 0) {
		return 0;
	}
	unsigned long long bytes_per_packet = datamanager_bytes_per_possibility();
	if (bytes_per_packet == 0) {
		return 0;
	}
	return ((unsigned long long)megabytes * 1024ULL * 1024ULL) / bytes_per_packet;
}

unsigned long long datamanager_packets_to_ram_mb(unsigned long long packets)
{
	unsigned long long bytes_per_packet = datamanager_bytes_per_possibility();
	unsigned long long total_bytes = packets * bytes_per_packet;
	// Arrondi au Mo SUPÉRIEUR : un affichage à 0 Mo pour un stock non vide
	// serait trompeur (« illimité » se lit précisément 0 ailleurs dans ce
	// module — cf. la convention `limit 0`/`leaseDuration 0`).
	return (total_bytes + (1024ULL * 1024ULL) - 1ULL) / (1024ULL * 1024ULL);
}

void datamanager_configure_ram_limit(int megabytes)
{
	stock_max_ram_packets = datamanager_ram_limit_to_packets(megabytes);
}

unsigned long long datamanager_ram_limit_packets(void)
{
	return stock_max_ram_packets;
}

unsigned long long datamanager_resident_packets(void)
{
	return datas_size();
}

/**
 * @brief Draine jusqu'à `max_packets` possibilités depuis la TÊTE (mode FIFO,
 *        `scroll_fifo`) de la file `file_index` du pool désigné.
 *
 * Interface étroite réservée à `core/stock_spill.c` (débordement sur disque) :
 * ce module ne connaît ni `file_possibility_t` ni les tableaux privés, juste
 * cette fonction et `datamanager_pool_refill`. La tête de file contient les
 * possibilités les plus anciennes (`scroll()` dépile la queue) — la donnée
 * froide à évincer.
 *
 * Un seul essai de verrouillage (`trylock`), jamais d'attente : appelée
 * depuis le tick périodique du thread de débordement, un échec se rattrape
 * au tick suivant.
 *
 * @param is_checked   0 = pool non vérifié, 1 = pool vérifié (même
 *                     convention que `want_checked` dans `put_to_pool`).
 * @param file_index   Indice de file, `[0, nb_file_possibility[`.
 * @param out          Tampon de sortie, au moins `max_packets` éléments.
 * @param max_packets  Nombre maximum de possibilités à extraire.
 * @return             Nombre réellement extrait (0 : file vide, index hors
 *                      bornes, ou verrou momentanément indisponible).
 */
int datamanager_pool_drain_head(int is_checked, int file_index, struct possibility_packet *out, int max_packets)
{
	if (file_index < 0 || file_index >= nb_file_possibility || max_packets <= 0) {
		return 0;
	}
	file_possibility_t **pool = is_checked ? file_possibility_checked : file_possibility;
	if (pthread_mutex_trylock(&pool[file_index]->lock) != 0) {
		return 0;
	}
	int n = 0;
	while (n < max_packets && scroll_fifo(&pool[file_index]->file, &out[n])) {
		n++;
	}
	pthread_mutex_unlock(&pool[file_index]->lock);
	return n;
}

/**
 * @brief Réinsère `count` possibilités (déjà extraites d'ailleurs — segment
 *        de débordement rechargé, ou drainées de la RAM par une éviction qui
 *        a ensuite échoué à les écrire sur disque) dans la file `file_index`
 *        du pool désigné, au bout chaud (`put`, comme tout ADD normal).
 *
 * Contrairement à `datamanager_pool_drain_head`, DOIT réussir : ces
 * possibilités n'ont nulle part ailleurs où aller. Même discipline que la
 * réinsertion de `rebalance_pool_step` (`trylock` + rotation vers la file
 * suivante + micro-sommeil, sans budget borné — le stock déplacé a déjà
 * quitté sa file d'origine, abandonner reviendrait à le perdre) : jamais
 * utilisée sur le chemin chaud d'un client, seulement par le thread de
 * débordement, où un blocage occasionnel de quelques dizaines de ms
 * est sans conséquence.
 *
 * @param is_checked 0 = pool non vérifié, 1 = pool vérifié.
 * @param file_index Indice de file, `[0, nb_file_possibility[`.
 * @param in         Possibilités à réinsérer.
 * @param count      Nombre de possibilités dans `in`.
 * @return           `count` en fonctionnement normal ; peut être inférieur
 *                    seulement sur OOM de `put()` (même angle mort accepté
 *                    que `rebalance_pool_step`, dont la réinsertion ignore
 *                    déjà ce cas).
 */
int datamanager_pool_refill(int is_checked, int file_index, const struct possibility_packet *in, int count)
{
	if (file_index < 0 || file_index >= nb_file_possibility || count <= 0) {
		return 0;
	}
	file_possibility_t **pool = is_checked ? file_possibility_checked : file_possibility;
	int dest = file_index;
	for (int i = 0; i < count; i++) {
		int added = 0;
		while (!added) {
			if (pthread_mutex_trylock(&pool[dest]->lock) == 0) {
				put(&pool[dest]->file, (void *)&in[i]);
				pthread_mutex_unlock(&pool[dest]->lock);
				added = 1;
			} else {
				dest = (dest + 1) % nb_file_possibility;
				usleep(MICRO_SLEEP);
			}
		}
	}
	return count;
}

// Réservée aux tests (jamais appelée en production, non déclarée dans
// datamanager.h — même convention que datamanager_reset_rr_state_for_tests
// ci-dessus) : fixe le plafond DIRECTEMENT en possibilités, sans passer par
// la conversion Mo -> possibilités (qui arrondit, cf.
// datamanager_ram_limit_to_packets) -- un test de put_to_pool veut une
// frontière EXACTE, pas une valeur approchée par un Mo.
void datamanager_set_ram_limit_packets_for_tests(unsigned long long packets)
{
	stock_max_ram_packets = packets;
}

char*server_ip = NULL;

int put_to_local(array_possibility_packet *possibilities);

int maintenance = 0;

/**
 * @brief 1 si une opération de maintenance (sauvegarde, restauration, tri…)
 *        tient actuellement toutes les files verrouillées, 0 sinon.
 *
 * Accesseur plutôt qu'un `extern int maintenance` brut — même convention que
 * `datas_size()`/`file_size()` pour l'état interne de ce module. Réservé à
 * `core/stock_spill.c` pour suspendre son propre travail (éviction/
 * rechargement) pendant qu'un cliché RAM est en train d'être pris : sans
 * cette pause, une possibilité pourrait migrer entre RAM et disque au
 * mauvais instant et se retrouver comptée deux fois — ou aucune — dans une
 * sauvegarde en cours.
 */
int datamanager_is_maintenance_active(void)
{
	return maintenance != 0;
}

/**
 * @brief Pose/lève `maintenance` pour un appelant EXTERNE à ce module —
 *        `restore_apply` (`ui/command_lines.c`) encadre `stock_spill_restore_snapshot`
 *        (`core/stock_spill.c`) PUIS `restore`/`restore_analysed` dans une
 *        seule fenêtre : sans elle, `stock_spill_step` (qui ne consulte QUE
 *        `maintenance`, jamais les verrous par file que `restore()` pose et
 *        lève lui-même) pourrait démarrer une éviction/un rechargement au
 *        beau milieu du remplacement des segments ou du drainage/réimport
 *        RAM, migrant des possibilités au mauvais instant.
 *
 * Non ré-entrant à dessein (pas de compteur) : ni `restore` ni
 * `restore_analysed` ne touchent eux-mêmes `maintenance` aujourd'hui — ne
 * jamais appeler depuis l'intérieur d'une fenêtre déjà posée par
 * `consistent_backup`/`sort_*`, qui gèrent la leur en interne.
 */
void datamanager_begin_maintenance(void)
{
	maintenance = 1;
}

void datamanager_end_maintenance(void)
{
	maintenance = 0;
}

void set_server_ip(const char *server)
{
	if(server_ip != NULL)
	{
		free(server_ip);
	}
	server_ip = NULL;
	if(server != NULL && strlen(server)>0)
	{
		server_ip = calloc(strlen(server) + 1, sizeof(char));
		strcpy(server_ip, server);
	}
}
char *get_server_ip(void)
{
	if(server_ip != NULL)
	{
		char *value =calloc(strlen(server_ip) + 1, sizeof(char));
		strcpy(value, server_ip);
		return value;
	} else {
		return NULL;
	}
}

/**
 * @brief Vérifie la connexion TCP au serveur et la (ré)établit si nécessaire.
 *
 * Si le socket du client est invalide ou déconnecté, ouvre une nouvelle connexion,
 * effectue le handshake de version et met à jour `client_possibility->socket_id`.
 * Déclenche `REQUEST_STOP` si la version n'est pas supportée.
 *
 * @param client_possibility Contexte du thread client.
 * @return                   Identifiant du socket valide, ou -1 en cas d'échec.
 */
int check_and_connect_to_server(client_possibility_t *client_possibility) {
	int socket_id = client_possibility->socket_id;
	// Mémorisé AVANT réassignation de socket_id ci-dessous : distingue la toute
	// première connexion d'un thread (routine, pas de trace utile) d'une VRAIE
	// reconnexion après coupure (pendant utile pour reconstituer une chronologie
	// de connectivité, cf. le "socket deconnected" de is_connected/poll_server_hunger).
	int had_connection = (socket_id != -1);
    // Création de connexion si "non connecté" ou "si erreur lors du test"
	if (socket_id == -1 || is_connected(socket_id) <= 0) {
		if(-1 == (socket_id = create_tcp_client(server_ip, SERVER_PORT)))
		{
			log_errno("Erreur sur accept() => ");
			return -1;
		}

		// Controle de version
		send_instruction(socket_id, INST_CHECK_VERSION);
		send(socket_id, &version, sizeof(int), 0);
		int8_t result = recv_instruction(socket_id);
		switch (handshake_verdict(result)) {
		case HANDSHAKE_OK:
			break;
		case HANDSHAKE_VERSION_REJECTED:
			// Refus EXPLICITE du serveur : incompatibilité de version réelle.
			// C'est le seul cas qui justifie d'arrêter le client — réessayer ne
			// servirait à rien tant que les binaires ne sont pas alignés.
			log_error("Version %i refusée par le serveur (incompatible)\n", version);
			close_socket(socket_id);
			request = REQUEST_STOP;
			return -1;
		case HANDSHAKE_RETRY:
		default:
			// Pas de réponse exploitable au handshake : timeout (serveur saturé,
			// « all threads busy »), connexion coupée ou pair occupé. Ce N'EST PAS
			// une erreur de version : on ne doit donc PAS arrêter le client. On
			// ferme cette tentative et on rend la main — l'appelant réessaiera
			// plus tard (avec back-off), le temps qu'un thread serveur se libère.
			log_event("handshake serveur sans réponse (result=%i, serveur occupé ?) — nouvelle tentative ultérieure\n", result);
			close_socket(socket_id);
			return -1;
		}

		// Hello d'identité (v12, INST_CLIENT_HELLO) : une fois par connexion
		// FRAÎCHEMENT établie, juste après le handshake — jamais rejoué sur une
		// connexion déjà en place (elle est déjà identifiée côté serveur). Best
		// effort côté serveur (cf. etii_server.c), mais un échec d'ENVOI ici
		// signale une connexion déjà cassée : on la referme et on laisse
		// l'appelant réessayer avec une connexion fraîche, plutôt que de
		// poursuivre sur un flux potentiellement désynchronisé.
		client_identity_t identity = g_client_identity_template;
		identity.fork_seq = client_possibility->fork_seq;
		uint8_t identity_buf[CLIENT_IDENTITY_WIRE_MAX_SIZE];
		int32_t identity_len = client_identity_encode(&identity, identity_buf, sizeof(identity_buf));
		int hello_ok = identity_len >= 0
			&& send_instruction(socket_id, INST_CLIENT_HELLO) > 0
			&& send_all(socket_id, &identity_len, sizeof(identity_len)) == (long)sizeof(identity_len)
			&& send_all(socket_id, identity_buf, (size_t)identity_len) == (long)identity_len;
		if (!hello_ok) {
			log_error("échec de l'envoi du hello d'identité — connexion abandonnée\n");
			close_socket(socket_id);
			return -1;
		}

		client_possibility->socket_id = socket_id;
		times(&client_possibility->start_socket);
		if (had_connection) {
			log_event("connexion serveur retablie (socket=%d)\n", socket_id);
		}
	}

	// Tout échange réseau passe ici : on rafraîchit l'horodatage d'activité
	// pour que le keepalive ne pingue que pendant les vraies périodes d'inactivité.
	client_possibility->last_socket_activity = time(NULL);
	return socket_id;
}

/**
 * @brief Envoie un tableau de possibilités au serveur TCP.
 *
 * Pour chaque possibilité, envoie INST_ADD suivi du paquet et attend INST_CONSIDERED.
 * En cas d'erreur d'acquittement, replie la possibilité dans les files locales.
 *
 * @param client_possibility Contexte du thread client (contient le socket).
 * @param possibilities      Tableau de possibilités à envoyer.
 * @return                   0 en cas de succès, -1 si la connexion échoue.
 */
void server_socket_io_lock(client_possibility_t *client_possibility)
{
	pthread_mutex_lock(&client_possibility->socket_mutex);
	server_io_active = 1;
}

void server_socket_io_unlock(client_possibility_t *client_possibility)
{
	// Effacer AVANT de déverrouiller : sinon un autre thread peut verrouiller
	// et démarrer son propre échange (server_io_active repassant à 1) avant
	// que CET appel n'ait fini de le remettre à 0 juste après — la remise à
	// zéro écraserait alors, à tort, l'échange du nouveau détenteur.
	server_io_active = 0;
	pthread_mutex_unlock(&client_possibility->socket_mutex);
}

int put_to_server(client_possibility_t *client_possibility, array_possibility_packet *possibilities)
{
	// Échange réseau atomique : empêche l'entrelacement avec le thread d'alimentation.
	server_socket_io_lock(client_possibility);
	int socket_id = check_and_connect_to_server(client_possibility);
	if (socket_id == -1) {
		server_socket_io_unlock(client_possibility);
		return -1;
	}

	int t;
	int connection_lost = 0;
	int last_routed = -1; /* indice du dernier élément déjà remis en local */
	for(t=0; t < possibilities->size && !connection_lost; t++)
	{
		if(possibilities->possibilities[t].alloc > max_result)
		{
			max_result = possibilities->possibilities[t].alloc;
		}
        /*
		if(possibilities->possibilities[t].x < 0 || possibilities->possibilities[t].y < 0 || possibilities->possibilities[t].x > 16 || possibilities->possibilities[t].y > 16)
		{
			printf("alert\n");
		}
         */
		send_instruction(socket_id, INST_ADD);
		struct possibility_packet *possibility = &possibilities->possibilities[t];
		// send_all : un send() brut pouvait n'écrire qu'une partie du paquet
		// et désynchroniser le flux (le serveur relisait la fin du paquet
		// comme des instructions).
		long result = send_all(socket_id, possibility, sizeof(struct possibility_packet));
		if (result != (long)sizeof(struct possibility_packet)) {
			log_errno("problème put_to_server send => ");
			/* L'envoi a échoué : on sort et on remet t..fin en local */
			connection_lost = 1;
			break;
		}
		int8_t ack = recv_instruction(socket_id);
		if(ack != INST_CONSIDERED) {
			// INST_ERROR n'est PAS une anomalie : c'est la dégradation gracieuse
			// prévue quand le stock du serveur est momentanément
			// intégralement verrouillé — typiquement la phase 1 de
			// consistent_backup, qui gèle toutes les files à l'instant T
			// puis les libère une à une. `put_to_pool` y épuise son budget borné
			// (DATAMANAGER_TRYLOCK_MAX_SWEEPS × MICRO_SLEEP ≈ 500 ms) et refuse
			// l'insertion plutôt que de bloquer le thread serveur — et donc la
			// connexion TCP — jusqu'au timeout du client. Sur un gros stock la
			// fenêtre « tout verrouillé » vaut l'écriture d'UN fichier (≈ 1,8 s
			// pour 14 M de possibilités réparties sur 20 files), soit plus que
			// ce budget : quelques refus par sauvegarde sont donc NORMAUX.
			// Rien n'est perdu : la possibilité part en stock local juste
			// en dessous et sera renvoyée plus tard (et, depuis le correctif
			// `from_server` de get_last_possibility, sans être acquittée à tort
			// au passage). Journalisé en info, sans vidage du plateau, pour ne
			// pas faire passer un fonctionnement nominal pour un incident.
			// Tout AUTRE valeur reste une vraie anomalie protocolaire.
			if (ack == INST_ERROR) {
				log_info("stock serveur momentanément indisponible (maintenance) : possibilité conservée en local, renvoi ultérieur\n");
			} else {
				log_error("problème de prise en compte du serveur (ack=%d)\n", ack);
			}
			array_possibility_packet *single_array = build_single_array_possibility_packet(possibility);
			put_to_local(single_array);
			free_array_possibility_packet(single_array);
			if (ack != INST_ERROR) {
				log_error_possibility_packet(possibility);
			}
			if (ack == INST_END) {
				/* Connexion perdue (timeout ou fermeture) : on sort et on remet t+1..fin en local */
				last_routed = t;
				connection_lost = 1;
				break;
			}
		}
	}

	if (connection_lost) {
		int first_remaining = (last_routed >= 0) ? last_routed + 1 : t;
		if (first_remaining < possibilities->size) {
			array_possibility_packet remaining;
			remaining.possibilities = &possibilities->possibilities[first_remaining];
			remaining.size = possibilities->size - first_remaining;
			put_to_local(&remaining);
		}
		server_socket_io_unlock(client_possibility);
		return -1;
	}

	server_socket_io_unlock(client_possibility);
	return 0;

}

int send_solution(client_possibility_t *client_possibility, struct possibility_packet *possibility)
{
	// Mode local (test/auto) : pas de serveur à prévenir. La solution reste
	// sauvegardée localement par log_solution ; on ne signale rien.
	if (client_possibility == NULL || server_ip == NULL) {
		return -1;
	}

	// Échange réseau atomique : empêche l'entrelacement avec le thread d'alimentation.
	server_socket_io_lock(client_possibility);
	int socket_id = check_and_connect_to_server(client_possibility);
	if (socket_id == -1) {
		server_socket_io_unlock(client_possibility);
		log_error("solution trouvée mais serveur injoignable pour la signaler\n");
		return -1;
	}

	int rc = -1;
	if (send_instruction(socket_id, INST_SOLUTION) > 0
	    && send_all(socket_id, possibility, sizeof(*possibility)) == (long)sizeof(*possibility)) {
		if (recv_instruction(socket_id) == INST_CONSIDERED) {
			rc = 0;
		} else {
			log_error("le serveur n'a pas acquitté la solution\n");
		}
	} else {
		log_errno("envoi de la solution au serveur a échoué => ");
	}

	server_socket_io_unlock(client_possibility);
	return rc;
}

/**
 * @brief Insère dans `pool` les possibilités du tableau retenues par le filtre `want_checked`.
 *
 * Trylock pour trouver une file libre ; toutes les possibilités retenues
 * vont dans la même file.
 *
 * @param pool          Pool de files cible.
 * @param possibilities Tableau de possibilités à filtrer/insérer.
 * @param want_checked  1 pour le pool vérifié (checked == 1), 0 pour le reste.
 * @param rr_counter    État round-robin du pool — fait démarrer chaque appel
 *                      sur une file différente plutôt que toujours la file 0.
 * @param pool_rate     Compteur de débit ventilé par pool, enregistré en plus
 *                      de l'agrégat `stock_adds_rate`, jamais à sa place.
 * @return 0 si inséré (ou rien à insérer), 1 si `pool` est resté
 *         intégralement verrouillé au-delà de
 *         `DATAMANAGER_TRYLOCK_MAX_SWEEPS` tours (maintenance en cours) —
 *         rien n'a été inséré, sûr à réessayer.
 */
static int put_to_pool(file_possibility_t **pool, array_possibility_packet *possibilities, int want_checked,
                        unsigned int *rr_counter, stock_rate_counter_t *pool_rate)
{
	int count = 0;
	int t;
	for(t=0; t < possibilities->size; t++)
	{
		// Routage robuste : seul checked == 1 est « vérifié », toute autre valeur
		// (0, ou résidu de padding d'anciens fichiers v4) va au pool standard
		if((possibilities->possibilities[t].checked == 1) == want_checked)
		{
			count++;
		}
	}
	if(count == 0)
	{
		return 0;
	}

	// Plafond RAM (--stock-max-ram / commande stockMaxRam): refuse AVANT
	// toute insertion si l'ajout ferait dépasser le budget publié par
	// datamanager_configure_ram_limit (0 = illimité, chemin inchangé). Compte
	// les DEUX pools de stock ensemble (datamanager_resident_packets, alias de
	// datas_size) puisque le budget couvre non-vérifié + vérifié, jamais un
	// seul des deux isolément. Même contrat de retour que l'épuisement du
	// budget de trylock plus bas (1 = rien inséré, sûr à réessayer) :
	// l'appelant (put_to_server côté serveur) sait déjà dégrader gracieusement
	// ce refus en INST_ERROR / repli local (cf. l'épilogue documenté dans
	// pour ce chemin).
	if (stock_max_ram_packets > 0 &&
	    datamanager_resident_packets() + (unsigned long long)count > stock_max_ram_packets)
	{
		time_t now = time(NULL);
		if (now - last_ram_cap_warning >= STOCK_RAM_CAP_WARN_COOLDOWN_SEC)
		{
			last_ram_cap_warning = now;
			log_error("stock : plafond RAM atteint (%llu possibilité(s) résidente(s), plafond %llu) — ADD refusé\n",
			          datamanager_resident_packets(), stock_max_ram_packets);
		}
		return 1;
	}

	int addpossibility = 0;
	int currfile = datamanager_rr_next_start(rr_counter, nb_file_possibility);
	int tried = 0;
	int failed_sweeps = 0;
	while(addpossibility == 0)
	{
		if(pthread_mutex_trylock(&pool[currfile]->lock) == 0)
		{
            for(t=0; t< possibilities->size; t++)
            {
                if((possibilities->possibilities[t].checked == 1) != want_checked)
                {
                    continue;
                }
                if(possibilities->possibilities[t].alloc > max_result)
                {
                    max_result = possibilities->possibilities[t].alloc;
                    //printf("max result:%i\n",max_result);
                }

                put(&pool[currfile]->file, &possibilities->possibilities[t]);
            }
			addpossibility = 1;
			pthread_mutex_unlock(&pool[currfile]->lock);
			time_t now_rate = time(NULL);
			stock_rate_record(&stock_adds_rate, (unsigned int)count, now_rate);
			stock_rate_record(pool_rate, (unsigned int)count, now_rate);
		}
		currfile = (currfile + 1) % nb_file_possibility;
		tried++;
		if(tried >= nb_file_possibility)
		{
			tried = 0;
			// On ne cède le CPU que quand un tour complet des nb_file_possibility
			// files n'a permis de verrouiller aucune d'entre elles : le cas
			// nominal (trylock réussi dès le premier essai) sort de la boucle
			// via addpossibility avant même d'atteindre ce tour complet. Même
			// motif que add_possibility_analysed. `tried` (et non plus
			// `currfile == 0`) compte le tour, puisque `currfile` démarre
			// désormais sur une file arbitraire (round-robin) plutôt que sur 0.
			usleep(MICRO_SLEEP);
			// Sortie bornée : au-delà de DATAMANAGER_TRYLOCK_MAX_SWEEPS tours consécutifs sans
			// verrouiller la moindre file, abandonner plutôt que de bloquer
			// indéfiniment le thread serveur qui sert ce client (et par
			// ricochet la connexion TCP jusqu'à son timeout). Rien n'a été
			// inséré : sûr de rendre la main ici.
			failed_sweeps++;
			if (failed_sweeps >= DATAMANAGER_TRYLOCK_MAX_SWEEPS)
			{
				return 1;
			}
		}
	}
	return 0;
}

int put_to_local(array_possibility_packet *possibilities)
{
	if(possibilities == NULL)
	{
		return 0;
	}
	// Routage par le flag `checked` : les possibilités vérifiées par un pruner
	// vont dans leur pool dédié, les autres dans le pool historique. Chaque
	// sous-tableau (checked / unchecked) est indépendant : un échec borné
	// sur l'un des deux pools n'empêche pas l'insertion dans l'autre.
	int err_unchecked = put_to_pool(file_possibility, possibilities, 0, &rr_put_unchecked, &stock_adds_unchecked_rate);
	int err_checked = put_to_pool(file_possibility_checked, possibilities, 1, &rr_put_checked, &stock_adds_checked_rate);
	return (err_unchecked || err_checked) ? 1 : 0;
}

int add_possibility(client_possibility_t *client_possibility, array_possibility_packet *possibilities)
{
	int error = 0;
    if(server_ip != NULL && client_possibility != NULL)
	{
		error = put_to_server(client_possibility, possibilities);
	} else
	{
		error = put_to_local(possibilities);
	}
	
	return error;
}

/**
 * @brief Calcule le hash d'une possibilité, cohérent avec `compare_possibility`.
 * @param p Paquet à hacher.
 * @return  Hash 64 bits (FNV-1a) des seuls champs comparés par `compare_possibility`.
 */
static uint64_t hash_possibility_key(const struct possibility_packet *p)
{
	uint64_t h = 1469598103934665603ULL; /* FNV-1a offset basis */
	const uint64_t prime = 1099511628211ULL;

#define ANALYSED_HASH_BYTE(b) do { h ^= (uint8_t)(b); h *= prime; } while (0)

	ANALYSED_HASH_BYTE(p->x);
	ANALYSED_HASH_BYTE(p->y);
	ANALYSED_HASH_BYTE(p->alloc & 0xff);
	ANALYSED_HASH_BYTE((p->alloc >> 8) & 0xff);

	/* compare_possibility() ne teste que les ETERN_PARTS premiers bits de
	 * b_faceused via is_face_used() ; le(s) mot(s) restant(s) (capacité au-delà
	 * de ETERN_PARTS) est hors contrat d'égalité et ne doit pas être haché. */
	for (int g = 0; g < ETERN_PARTS / 16; g++) {
		uint16_t v = p->b_faceused[g];
		ANALYSED_HASH_BYTE(v & 0xff);
		ANALYSED_HASH_BYTE((v >> 8) & 0xff);
	}

	for (int x = 0; x < ETERN_SIZE; x++) {
		for (int y = 0; y < ETERN_SIZE; y++) {
			uint16_t v = (uint16_t)p->grid[x][y];
			ANALYSED_HASH_BYTE(v & 0xff);
			ANALYSED_HASH_BYTE((v >> 8) & 0xff);
		}
	}
#undef ANALYSED_HASH_BYTE

	return h;
}

/**
 * @brief Indexe le dernier élément ajouté à `file_possibility_analysed[fileidx]`.
 *
 * À appeler juste après un `put()` réussi, avec `e == file->end`. En cas
 * d'échec d'allocation du nœud d'index (OOM), l'élément reste dans la file
 * mais non indexé : bascule `analysed_index_may_be_incomplete` à 1, pour que
 * `remove_possibility_analysed` réactive son repli par balayage linéaire (le
 * retrouvant ainsi, jamais silencieusement perdu) — et
 * `datamanager_analysed_owned_by` ne le verra simplement pas, comme
 * documenté sur `AnalysedIndexNode` ci-dessus.
 *
 * @param fileidx   Indice de la file analysée (0..nb_file_possibility-1).
 * @param e         Élément (déjà présent dans la file) à indexer.
 * @param owner_uid `client_uid` du client servi côté serveur, ou `NULL`
 *                  si aucune attribution n'est connue/pertinente ici.
 */
static void analysed_index_add(int fileidx, Element *e, const uint8_t owner_uid[CLIENT_UID_BYTES])
{
	AnalysedIndexNode *node = malloc(sizeof(AnalysedIndexNode));
	if (node == NULL) {
		analysed_index_may_be_incomplete = 1;
		return;
	}
	node->hash = hash_possibility_key((struct possibility_packet *)e->value);
	node->element = e;
	if (owner_uid != NULL) {
		memcpy(node->owner_uid, owner_uid, CLIENT_UID_BYTES);
		node->has_owner = 1;
		// Bail: échéance calculée MAINTENANT, à l'insertion — jamais
		// recalculée ensuite. analysed_lease_seconds <= 0 désactive le bail
		// (sentinelle 0, cf. analysed_lease_is_expired) : comportement
		// d'avant cette PR, une possibilité attribuée n'expire jamais.
		node->lease_deadline = (analysed_lease_seconds > 0)
		                            ? (time(NULL) + analysed_lease_seconds)
		                            : 0;
	} else {
		node->has_owner = 0;
		node->lease_deadline = 0;
	}
	size_t bucket = node->hash % ANALYSED_INDEX_BUCKETS;
	node->next = analysed_index[fileidx][bucket];
	analysed_index[fileidx][bucket] = node;
}

/**
 * @brief Retrouve puis retire de l'INDEX (pas de la file) l'élément dont la
 *        valeur correspond à `key` au sens de `compare_possibility`.
 *
 * @param fileidx Indice de la file analysée.
 * @param key     Paquet à retrouver.
 * @return        L'`Element` de la file correspondant (à l'appelant de le
 *                retirer avec `file_remove_element`), ou NULL si l'index n'a
 *                pas (ou plus) de candidat pour cette clé.
 */
static Element *analysed_index_find_and_remove(int fileidx, const struct possibility_packet *key)
{
	uint64_t h = hash_possibility_key(key);
	size_t bucket = h % ANALYSED_INDEX_BUCKETS;
	AnalysedIndexNode *node = analysed_index[fileidx][bucket];
	AnalysedIndexNode *prev = NULL;
	while (node != NULL) {
		if (node->hash == h && compare_possibility((struct possibility_packet *)node->element->value, (struct possibility_packet *)key) == 0) {
			Element *found = node->element;
			if (prev == NULL) {
				analysed_index[fileidx][bucket] = node->next;
			} else {
				prev->next = node->next;
			}
			free(node);
			return found;
		}
		prev = node;
		node = node->next;
	}
	return NULL;
}

/**
 * @brief Retire de l'index le nœud référençant précisément `victim`.
 *
 * À appeler AVANT tout `scroll()` qui libérerait `victim->value` (le hash est
 * recalculé depuis cette valeur, encore valide à cet instant). Recherche par
 * identité de pointeur (pas par contenu) : correct même en présence de
 * doublons de contenu dans le même seau.
 *
 * @param fileidx Indice de la file analysée.
 * @param victim  Élément sur le point d'être retiré de la file.
 */
static void analysed_index_remove_element(int fileidx, Element *victim)
{
	if (victim == NULL) {
		return;
	}
	uint64_t h = hash_possibility_key((struct possibility_packet *)victim->value);
	size_t bucket = h % ANALYSED_INDEX_BUCKETS;
	AnalysedIndexNode *node = analysed_index[fileidx][bucket];
	AnalysedIndexNode *prev = NULL;
	while (node != NULL) {
		if (node->element == victim) {
			if (prev == NULL) {
				analysed_index[fileidx][bucket] = node->next;
			} else {
				prev->next = node->next;
			}
			free(node);
			return;
		}
		prev = node;
		node = node->next;
	}
	/* Non trouvé (ex. jamais indexé suite à un malloc échoué) : rien à faire. */
}

/**
 * @brief Vide entièrement l'index d'une file analysée.
 *
 * À appeler après tout drainage complet de `file_possibility_analysed[fileidx]->file`
 * (restock_analysed, restore_analysed, send_possibility_analysed en mode
 * local) : les `Element*` référencés par l'index seraient sinon des
 * pointeurs pendants (use-after-free au prochain hit).
 *
 * @param fileidx Indice de la file analysée.
 */
static void analysed_index_clear(int fileidx)
{
	for (size_t b = 0; b < ANALYSED_INDEX_BUCKETS; b++) {
		AnalysedIndexNode *node = analysed_index[fileidx][b];
		while (node != NULL) {
			AnalysedIndexNode *next = node->next;
			free(node);
			node = next;
		}
		analysed_index[fileidx][b] = NULL;
	}
}

int remove_possibility_analysed(struct possibility_packet *possibility, int thread, int preferred_file) {
#ifdef DEBUG_CHECK_POSSIBILITY
    int analyse = check_possibility(possibility, NULL);
    if (analyse < 0)
    {
        log_debug("possibility error : %i\n",analyse);
        log_debug(" ---");
        print_possibility_packet(possibility);
    }
#endif // DEBUG_CHECK_POSSIBILITY
	int removed_possibility = 0;
	int currfile = 0;
	int waits = 0;
	// `scan_start` fixe le point de départ du balayage exhaustif (thread < 0
	// uniquement): `preferred_file` (indice « probable » dérivé côté
	// serveur de la connexion — server_analysed_file_hint) s'il est valide,
	// sinon 0 comme avant ce correctif. `step` compte les files déjà
	// essayées CE tour-ci pour détecter qu'on a fait le tour complet, quel
	// que soit le point de départ — currfile lui-même ne peut plus servir à
	// ça une fois qu'il ne part plus systématiquement de 0.
	int scan_start = 0;
	int step = 0;
	if (thread >=0) {
		currfile = thread;
	} else if (preferred_file >= 0 && preferred_file < nb_file_possibility) {
		scan_start = preferred_file;
		currfile = scan_start;
	}
#ifdef DEBUG_CHECK_POSSIBILITY
	log_debug("a supprimer : \n");
	print_possibility_packet(possibility);
    log_debug("en cours d'analyse:\n");
	print_all_file_analysed();
#endif // DEBUG_CHECK_POSSIBILITY

	while(removed_possibility == 0)
	{
		int checked = 0;
		if(pthread_mutex_trylock(&file_possibility_analysed[currfile]->lock) == 0)
		{
			File *file = &file_possibility_analysed[currfile]->file;
			// Chemin rapide : l'index de hachage retrouve la possibilité en
			// O(1) amorti (cas courant : elle est bien présente). Le repli par
			// balayage linéaire (O(taille de la file), sous verrou) n'est
			// tenté QUE si l'index a pu manquer une entrée (échec d'allocation
			// d'un nœud, cf. analysed_index_may_be_incomplete) — sinon un miss
			// de l'index signifie une absence RÉELLE, jamais un défaut
			// d'indexation, et payer ce balayage serait une régression de
			// performance pure sur le cas le plus fréquent.
			Element *element = analysed_index_find_and_remove(currfile, possibility);
			if (element == NULL && analysed_index_may_be_incomplete) {
				element = file->start;
				while (element != NULL && compare_possibility((struct possibility_packet *)element->value, possibility) != 0) {
					element = element->next;
				}
			}
			if (element != NULL) {
				file_remove_element(file, element);
				removed_possibility = 1;
			}

			checked = 1;
			pthread_mutex_unlock(&file_possibility_analysed[currfile]->lock);
		}
		if (checked == 1) {
			if (removed_possibility == 1) {
				break;
			}
			if (thread < 0) {
				step++;
				if(step >= nb_file_possibility)
				{
					// Toutes les files ont été verrouillées et parcourues (en
					// partant de scan_start, quel qu'il soit) : absence
					// CONFIRMÉE.
#ifdef DEBUG_CHECK_POSSIBILITY
					log_debug("non supprimée \n");
#endif // DEBUG_CHECK_POSSIBILITY
					return 1;
				}
				currfile = (scan_start + step) % nb_file_possibility;
			} else {
				// Une seule file ciblée, déjà verrouillée et parcourue :
				// absence CONFIRMÉE.
#ifdef DEBUG_CHECK_POSSIBILITY
                log_debug("non supprimée \n");
#endif // DEBUG_CHECK_POSSIBILITY
				return 1;
			}
		} else {
			usleep(MICRO_SLEEP);
			// Sortie bornée (même motif que add_possibility_analysed_impl) :
			// au-delà de DATAMANAGER_TRYLOCK_MAX_SWEEPS attentes
			// consécutives sans jamais réussir à verrouiller la moindre file,
			// abandonner plutôt que de bloquer indéfiniment l'appelant (et par
			// ricochet la connexion TCP du client jusqu'à son timeout) —
			// AVANT ce correctif, cette boucle était la SEULE des quatre
			// boucles trylock de ce fichier à ne pas être bornée. Retour -1,
			// DISTINCT du "absence confirmée" (retour 1) : voir le contrat
			// détaillé dans datamanager.h — un appelant qui confondrait les
			// deux perdrait silencieusement une possibilité qui existe peut-
			// être toujours mais n'a simplement pas pu être vérifiée à temps.
			waits++;
			if (waits >= DATAMANAGER_TRYLOCK_MAX_SWEEPS)
			{
				return -1;
			}
		}
	}
#ifdef DEBUG_CHECK_POSSIBILITY
    log_debug("après suppression (supprimer :%i) : \n", removed_possibility);
	print_all_file_analysed();
#endif // DEBUG_CHECK_POSSIBILITY
	return 0;
}
/**
 * @brief Envoie au serveur les possibilités de la file « analysées » du thread.
 *
 * En mode local (sans serveur), vide simplement la file analysée. En mode
 * client-serveur, envoie chaque `possibility_packet` via `INST_POSSIBILITY_ANALYSED`
 * et attend l'accusé `INST_CONSIDERED`. En cas d'erreur, remet la possibilité
 * dans la file et interrompt l'envoi.
 *
 * @param client_possibility Contexte du thread (id, socket, mutex, etc.).
 */
void send_possibility_analysed(client_possibility_t *client_possibility) {
	int thread = client_possibility->id;
	if (server_ip == NULL) {
		if(pthread_mutex_trylock(&file_possibility_analysed[thread]->lock) == 0)
		{
			File *file = &file_possibility_analysed[thread]->file;
			Element *element = file->start;
			if (element != NULL) {
                struct possibility_packet *possibility = malloc(sizeof(struct possibility_packet));
				while (scroll(file, possibility)) {
                    ;
				}
                free(possibility);
                // La file est désormais entièrement vide : purge en bloc (pas de
                // recherche possibilité par possibilité, juste des libérations).
                analysed_index_clear(thread);
			}

			pthread_mutex_unlock(&file_possibility_analysed[thread]->lock);
		}

		return;
	}

	// Échange réseau atomique : empêche l'entrelacement avec le thread d'alimentation.
	server_socket_io_lock(client_possibility);
	int socket_id = check_and_connect_to_server(client_possibility);
	if (socket_id == -1) {
		server_socket_io_unlock(client_possibility);
		return;
	}
	if(pthread_mutex_trylock(&file_possibility_analysed[thread]->lock) == 0)
	{
		File *file = &file_possibility_analysed[thread]->file;
		if (file->start != NULL) {
			// Acquittement par lot : on draine la file par tranches de
			// pruner_batch_size et on envoie chaque tranche en un seul
			// INST_POSSIBILITY_ANALYSED_BATCH (un INST_CONSIDERED par tranche).
			// Borne la mémoire (cap paquets) et lève le plafond « 1 aller-retour
			// par possibilité » de l'ancien acquittement individuel.
			int cap = (pruner_batch_size > 0) ? pruner_batch_size : 1;
			struct possibility_packet *buf = malloc((size_t)cap * sizeof(struct possibility_packet));
			if (buf != NULL) {
				int drained;
				do {
					drained = 0;
					// scroll() dépile file->end : on retire d'abord son entrée
					// d'index (par identité de pointeur, tant que value est
					// encore valide) puis on la sort réellement de la file.
					while (drained < cap && file->end != NULL) {
						analysed_index_remove_element(thread, file->end);
						scroll(file, &buf[drained]);
						drained++;
					}
					if (drained == 0) {
						break;
					}
					int32_t m = drained;
					size_t bytes = (size_t)drained * sizeof(struct possibility_packet);
					int sent_ok = (send_instruction(socket_id, INST_POSSIBILITY_ANALYSED_BATCH) > 0)
					           && (send_all(socket_id, &m, sizeof(m)) == (long)sizeof(m))
					           && (send_all(socket_id, buf, bytes) == (long)bytes);
					if (!sent_ok) {
						log_errno("Error when send_possibility_analysed (batch) => ");
						for (int i = 0; i < drained; i++) {
							if (put(file, &buf[i])) analysed_index_add(thread, file->end, NULL);
						}
						break;
					}
					if (recv_instruction(socket_id) != INST_CONSIDERED) {
						log_error("batch analysed non pris en compte (%d possibilités), remise en file\n", drained);
						for (int i = 0; i < drained; i++) {
							if (put(file, &buf[i])) analysed_index_add(thread, file->end, NULL);
						}
						break;
					}
				} while (drained == cap);
				free(buf);
			}
		}

		pthread_mutex_unlock(&file_possibility_analysed[thread]->lock);
	}

	server_socket_io_unlock(client_possibility);
}

/**
 * @brief Corps commun de `add_possibility_analysed`/`add_possibility_analysed_owned` :
 *        seule la présence d'un `owner_uid` diffère entre les deux.
 *
 * Tente un trylock sur `file_possibility_analysed[thread]`. Si `thread < 0`,
 * tourne sur toutes les files jusqu'à en trouver une disponible. Met à jour
 * `max_result` si `alloc` du paquet est supérieur.
 *
 * @param possiblity Paquet à enregistrer comme « en cours d'analyse ».
 * @param thread     Index du thread cible (≥ 0), ou -1 pour la première file libre.
 * @param owner_uid  `client_uid` du client servi côté serveur, ou `NULL` (cf.
 *                   `AnalysedIndexNode` pour les cas où l'attribution est
 *                   sciemment absente : côté client, ou possibilité restaurée).
 * @return           0 si ajouté, -1 si toutes les files sont restées
 *                    verrouillées au-delà de DATAMANAGER_TRYLOCK_MAX_SWEEPS
 *                    tours (typiquement une maintenance en cours sur le pool
 *                    analysé : sauvegarde, restore, tri...). Rien n'est
 *                    inséré dans ce cas.
 */
static int add_possibility_analysed_impl(struct possibility_packet *possiblity, int thread,
                                          const uint8_t owner_uid[CLIENT_UID_BYTES]) {
	int addpossibility = 0;
	int currfile = 0;
	int waits = 0;
	if (thread >=0) {
		currfile = thread;
	}
	while(possiblity != NULL && addpossibility == 0)
	{
		if(pthread_mutex_trylock(&file_possibility_analysed[currfile]->lock) == 0)
		{
			if(possiblity->alloc > max_result)
			{
				max_result = possiblity->alloc;
				//printf("max result:%i\n",max_result);
			}
            /*
			if(possiblity->x < 0 || possiblity->y < 0 || possiblity->x > ETERN_SIZE || possiblity->y > ETERN_SIZE)
			{
				printf("alert\n");
			}
             */

			File *target = &file_possibility_analysed[currfile]->file;
			if (put(target, possiblity)) {
				analysed_index_add(currfile, target->end, owner_uid);
			}
			addpossibility = 1;
			pthread_mutex_unlock(&file_possibility_analysed[currfile]->lock);
			break;
		}
		if (thread < 0) {
			currfile++;
			if(currfile >= nb_file_possibility)
			{
				currfile = 0;
				usleep(MICRO_SLEEP);
				waits++;
			}
		} else {
			usleep(MICRO_SLEEP);
			waits++;
		}
		// Sortie bornée: cf. le commentaire de la fonction. `waits`
		// compte les usleep() effectivement exécutés dans les deux modes
		// (rotation sur thread < 0, une file fixe sur thread >= 0) pour borner
		// le même temps d'horloge quel que soit le mode, plutôt qu'un nombre
		// de tentatives dont le coût réel diffère d'un mode à l'autre.
		if (waits >= DATAMANAGER_TRYLOCK_MAX_SWEEPS)
		{
			return -1;
		}
	}
	return 0;
}

int add_possibility_analysed(struct possibility_packet *possiblity, int thread) {
	return add_possibility_analysed_impl(possiblity, thread, NULL);
}

int add_possibility_analysed_owned(struct possibility_packet *possiblity, int thread,
                                    const uint8_t owner_uid[CLIENT_UID_BYTES]) {
	return add_possibility_analysed_impl(possiblity, thread, owner_uid);
}

/**
 * @brief Balaye `analysed_index` pour résumer ce qu'un `client_uid` donné
 *        détient actuellement (consultation « que travaille X ? »).
 *
 * Verrouille chaque file `file_possibility_analysed[f]` le temps de son
 * propre balayage — jamais toutes à la fois. Pas un chemin chaud : un
 * simple `pthread_mutex_lock` bloquant est préférable au `trylock` utilisé
 * ailleurs, on veut une réponse exacte.
 *
 * Ne voit que les possibilités effectivement indexées : une entrée non
 * indexée (OOM à l'insertion) est simplement absente du compte, jamais une
 * source d'erreur.
 *
 * @param out_max_alloc Le plus grand `alloc` parmi elles, ou -1 si `*out_count == 0`.
 * @return 0 si OK, -1 si `owner_uid`/`out_count`/`out_max_alloc` est NULL.
 */
int datamanager_analysed_owned_by(const uint8_t owner_uid[CLIENT_UID_BYTES],
                                   unsigned long long *out_count, int *out_max_alloc) {
	if (owner_uid == NULL || out_count == NULL || out_max_alloc == NULL) {
		return -1;
	}
	*out_count = 0;
	*out_max_alloc = -1;
	for (int f = 0; f < nb_file_possibility; f++) {
		pthread_mutex_lock(&file_possibility_analysed[f]->lock);
		for (size_t b = 0; b < ANALYSED_INDEX_BUCKETS; b++) {
			for (AnalysedIndexNode *node = analysed_index[f][b]; node != NULL; node = node->next) {
				if (!node->has_owner || memcmp(node->owner_uid, owner_uid, CLIENT_UID_BYTES) != 0) {
					continue;
				}
				(*out_count)++;
				int alloc = ((struct possibility_packet *)node->element->value)->alloc;
				if (alloc > *out_max_alloc) {
					*out_max_alloc = alloc;
				}
			}
		}
		pthread_mutex_unlock(&file_possibility_analysed[f]->lock);
	}
	return 0;
}

int analysed_lease_is_expired(time_t lease_deadline, time_t now) {
	if (lease_deadline <= 0) {
		// Sentinelle « bail désactivé/non applicable » (cf. AnalysedIndexNode) :
		// jamais expiré.
		return 0;
	}
	return now >= lease_deadline;
}

/**
 * @brief Voir la doc dans datamanager.h. Structure calquée sur
 *        `restock_analysed` : une passe par file, tout le travail sur l'index
 *        et la `File` sous le verrou de CETTE file, puis remise en stock hors
 *        verrou (même ordre de verrouillage que `restock_analysed` : jamais
 *        `file_possibility[dest]->lock` pris pendant que
 *        `file_possibility_analysed[f]->lock` est tenu).
 */
// Verrous globaux définis plus bas dans ce fichier (et absents de
// datamanager.h : ils sont internes au module).
void lock_all_file(void);
void unlock_all_file(void);
void lock_all_file_analysed(void);
void unlock_all_file_analysed(void);

/**
 * @brief Vrai si l'une des `n` origines est la racine de `candidate`.
 */
static int is_descendant_of_any(const struct possibility_packet *origins, unsigned long long n,
                                struct possibility_packet *candidate)
{
	for (unsigned long long k = 0; k < n; k++) {
		if (is_origin_of((struct possibility_packet *)&origins[k], candidate) == 1) {
			return 1;
		}
	}
	return 0;
}

/**
 * @brief Retire de l'index du pool analysé le nœud pointant sur `e` (s'il existe).
 *
 * Le compartiment est celui du hash de la VALEUR de `e` — c'est là que
 * `add_possibility_analysed_impl` l'a rangé. On compare `node->element` et non
 * le hash : deux paquets distincts peuvent partager un compartiment. Un nœud
 * absent est normal et sans conséquence (`analysed_index_may_be_incomplete`
 * quand une allocation de nœud a échoué), d'où le parcours de la File comme
 * source de vérité plutôt que de l'index.
 */
static void analysed_index_forget_element(int fileidx, Element *e)
{
	size_t bucket = hash_possibility_key((struct possibility_packet *)e->value) % ANALYSED_INDEX_BUCKETS;
	AnalysedIndexNode *node = analysed_index[fileidx][bucket];
	AnalysedIndexNode *prev = NULL;
	while (node != NULL) {
		if (node->element == e) {
			if (prev == NULL) {
				analysed_index[fileidx][bucket] = node->next;
			} else {
				prev->next = node->next;
			}
			free(node);
			return;
		}
		prev = node;
		node = node->next;
	}
}

unsigned long long datamanager_purge_descendants_of(const struct possibility_packet *origins,
                                                    unsigned long long n)
{
	if (origins == NULL || n == 0) {
		return 0;
	}
	unsigned long long removed = 0;

	// Temps 1 : pool analysé. D'abord, parce qu'un descendant qui en sort
	// (réinjection) retombe dans le stock, que le temps 2 balaie ensuite.
	lock_all_file_analysed();
	for (int f = 0; f < nb_file_possibility; f++) {
		File *file = &file_possibility_analysed[f]->file;
		Element *e = file->start;
		while (e != NULL) {
			Element *next = e->next;
			if (e->value != NULL
			    && is_descendant_of_any(origins, n, (struct possibility_packet *)e->value)) {
				analysed_index_forget_element(f, e);
				file_remove_element(file, e);
				removed++;
			}
			e = next;
		}
	}
	unlock_all_file_analysed();

	// Temps 2 : les deux pools de stock.
	lock_all_file();
	for (int f = 0; f < nb_file_possibility; f++) {
		File *pools[2] = { &file_possibility[f]->file, &file_possibility_checked[f]->file };
		for (int p = 0; p < 2; p++) {
			Element *e = pools[p]->start;
			while (e != NULL) {
				Element *next = e->next;
				if (e->value != NULL
				    && is_descendant_of_any(origins, n, (struct possibility_packet *)e->value)) {
					file_remove_element(pools[p], e);
					removed++;
				}
				e = next;
			}
		}
	}
	unlock_all_file();

	if (removed > 0) {
		log_info("bail rendu au stock : %llu possibilite(s) redondante(s) supprimee(s) (descendants de %llu racine(s))\n",
		         removed, n);
	}
	return removed;
}

/**
 * @brief Remet un paquet dans le stock, dans le pool correspondant à son
 *        drapeau `checked`.
 *
 * Chemin commun aux deux réinjections « en bloc » (`restock_analysed` et
 * `datamanager_reclaim_expired_leases`) ; le troisième chemin,
 * `requeue_last_sent_possibility`, route déjà selon le drapeau de la même
 * façon.
 *
 * Router selon `checked` et non forcer le pool non vérifié : la réinjection
 * ne modifie pas le plateau, donc la vérification du pruner vaut toujours.
 * Renvoyer un paquet vérifié dans le pool non vérifié le ferait re-vérifier
 * pour rien et contredirait son propre drapeau.
 *
 * Délibérément sans contrôle de plafond RAM (à la différence de
 * `put_to_pool`) : une réinjection ne doit jamais pouvoir échouer, sous
 * peine de perdre une possibilité. La boucle `trylock` en tourniquet
 * garantit qu'elle finit toujours par aboutir sur une file.
 */
static void put_back_to_stock(struct possibility_packet *pk)
{
	file_possibility_t **pool = pk->checked ? file_possibility_checked : file_possibility;
	int dest = 0;
	int added = 0;
	while (!added) {
		if (pthread_mutex_trylock(&pool[dest]->lock) == 0) {
			if (pk->alloc > max_result) max_result = pk->alloc;
			put(&pool[dest]->file, pk);
			pthread_mutex_unlock(&pool[dest]->lock);
			added = 1;
		} else {
			dest = (dest + 1) % nb_file_possibility;
			usleep(MICRO_SLEEP);
		}
	}
}

unsigned long long datamanager_reclaim_expired_leases(time_t now, analysed_owner_alive_fn owner_alive) {
	unsigned long long reclaimed_total = 0;
	// Accumulateur des paquets réellement rendus au stock : ils servent, une
	// fois TOUS les verrous relâchés, à purger les descendants qu'ils rendent
	// redondants (datamanager_purge_descendants_of). Une seule passe pour
	// toutes les origines, et non une passe par origine.
	struct possibility_packet *reclaimed_all = NULL;
	unsigned long long reclaimed_all_n = 0;
	int accumulation_ok = 1;
	for (int f = 0; f < nb_file_possibility; f++) {
		pthread_mutex_lock(&file_possibility_analysed[f]->lock);
		File *file = &file_possibility_analysed[f]->file;
		unsigned long long capacity = file->size;
		struct possibility_packet *buf = (capacity > 0)
		                                      ? malloc(capacity * sizeof(struct possibility_packet))
		                                      : NULL;
		unsigned long long n = 0;
		if (buf != NULL) {
			for (size_t b = 0; b < ANALYSED_INDEX_BUCKETS; b++) {
				AnalysedIndexNode *node = analysed_index[f][b];
				AnalysedIndexNode *prev = NULL;
				while (node != NULL) {
					AnalysedIndexNode *next = node->next;
					// Deux conditions requises pour réclamer :
					// l'échéance ET l'absence de preuve de vivacité. Un client
					// toujours vivant (owner_alive vrai) n'est JAMAIS réclamé,
					// aussi longtemps l'analyse ait-elle mis.
					if (node->has_owner && analysed_lease_is_expired(node->lease_deadline, now)
					    && (owner_alive == NULL || !owner_alive(node->owner_uid))) {
						Element *victim = node->element;
						memcpy(&buf[n], victim->value, sizeof(struct possibility_packet));
						n++;

						if (prev == NULL) {
							analysed_index[f][b] = next;
						} else {
							prev->next = next;
						}
						free(node);

						// Retrait effectif de la File : après la copie ci-dessus
						// (file_remove_element libère victim->value).
						file_remove_element(file, victim);
					} else {
						prev = node;
					}
					node = next;
				}
			}
		} else if (capacity > 0) {
			log_error("datamanager_reclaim_expired_leases : allocation échouée (file %d, %llu possibilités) — passe sautée\n",
			          f, capacity);
		}
		pthread_mutex_unlock(&file_possibility_analysed[f]->lock);

		for (unsigned long long i = 0; i < n; i++) {
			put_back_to_stock(&buf[i]);
		}
		if (n > 0 && accumulation_ok) {
			struct possibility_packet *grown = realloc(reclaimed_all,
			                                           (size_t)(reclaimed_all_n + n) * sizeof(struct possibility_packet));
			if (grown == NULL) {
				// La réinjection, elle, a déjà eu lieu : rien n'est perdu. On
				// renonce seulement au nettoyage ciblé, que `checkOrigin`
				// rattrapera.
				log_error("datamanager_reclaim_expired_leases : allocation du nettoyage échouée — descendants non purgés\n");
				accumulation_ok = 0;
			} else {
				reclaimed_all = grown;
				memcpy(&reclaimed_all[reclaimed_all_n], buf, (size_t)n * sizeof(struct possibility_packet));
				reclaimed_all_n += n;
			}
		}
		free(buf);
		reclaimed_total += n;
	}

	// Hors de toute section critique : la purge prend elle-même ses verrous.
	if (accumulation_ok && reclaimed_all_n > 0) {
		datamanager_purge_descendants_of(reclaimed_all, reclaimed_all_n);
	}
	free(reclaimed_all);
	return reclaimed_total;
}

/**
 * @brief Remet dans le stock toutes les possibilités en cours d'analyse.
 *
 * Vide chaque file `file_possibility_analysed` et réinjecte les paquets dans
 * le stock, chacun dans le pool correspondant à son drapeau `checked`
 * (`put_back_to_stock`). Utile quand des clients sont morts sans avoir terminé
 * leur travail.
 *
 * @return 0.
 */
int restock_analysed(void) {
    unsigned long long moved = 0;
    for (int f = 0; f < nb_file_possibility; f++) {
        pthread_mutex_lock(&file_possibility_analysed[f]->lock);
        File *src = &file_possibility_analysed[f]->file;
        unsigned long long count = src->size;
        if (count == 0) {
            pthread_mutex_unlock(&file_possibility_analysed[f]->lock);
            continue;
        }
        struct possibility_packet *buf = malloc(count * sizeof(struct possibility_packet));
        unsigned long long n = 0;
        while (n < count && scroll(src, &buf[n])) {
            n++;
        }
        // La file est désormais entièrement vide : purge en bloc de l'index
        // (pas de recherche possibilité par possibilité).
        analysed_index_clear(f);
        pthread_mutex_unlock(&file_possibility_analysed[f]->lock);

        for (unsigned long long i = 0; i < n; i++) {
            put_back_to_stock(&buf[i]);
            moved++;
        }
        free(buf);
    }
    log_info("restock_analysed : %llu possibilité(s) remise(s) dans le stock\n", moved);
    return 0;
}

/**
 * @brief Rééquilibre UN pool (non vérifié ou vérifié) d'un pas incrémental :
 *        déplace jusqu'à `max_packets` possibilités de la file la plus
 *        pleine vers la plus vide.
 *
 * Lecture des tailles en O(1) (`.file.size`, sans verrou) pour choisir la
 * file source/destination, puis extraction bloquante de la seule file
 * source, relâchée avant l'insertion — jamais deux verrous de pool tenus
 * ensemble. L'insertion vise la file la plus vide en priorité, avec repli
 * en balayage rotatif si elle est momentanément prise par un autre thread.
 *
 * Ne déplace rien si `pool[fullest] <= total/nb_file_possibility` (déjà
 * équilibré) : évite un va-et-vient perpétuel pour de petites variations
 * dues au trafic concurrent normal.
 *
 * @return Nombre de possibilités effectivement déplacées.
 */
static int rebalance_pool_step(file_possibility_t **pool, int max_packets)
{
	if (max_packets <= 0) {
		return 0;
	}

	unsigned long long sizes[nb_file_possibility];
	unsigned long long total = 0;
	int fullest = 0;
	int emptiest = 0;
	for (int fp = 0; fp < nb_file_possibility; fp++) {
		sizes[fp] = pool[fp]->file.size;
		total += sizes[fp];
		if (sizes[fp] > sizes[fullest]) { fullest = fp; }
		if (sizes[fp] < sizes[emptiest]) { emptiest = fp; }
	}
	if (fullest == emptiest || total == 0) {
		return 0;
	}

	unsigned long long target = total / nb_file_possibility;
	if (sizes[fullest] <= target) {
		return 0;
	}

	unsigned long long surplus = sizes[fullest] - target;
	unsigned long long deficit = (sizes[emptiest] < target) ? (target - sizes[emptiest]) : 1;
	unsigned long long to_move = (surplus < deficit) ? surplus : deficit;
	if (to_move == 0) { to_move = 1; }
	if (to_move > (unsigned long long)max_packets) { to_move = (unsigned long long)max_packets; }

	struct possibility_packet *buf = malloc((size_t)to_move * sizeof(struct possibility_packet));
	if (buf == NULL) {
		return 0;
	}

	pthread_mutex_lock(&pool[fullest]->lock);
	unsigned long long n = 0;
	while (n < to_move && scroll(&pool[fullest]->file, &buf[n])) {
		n++;
	}
	pthread_mutex_unlock(&pool[fullest]->lock);

	for (unsigned long long i = 0; i < n; i++) {
		int dest = emptiest;
		int added = 0;
		while (!added) {
			if (pthread_mutex_trylock(&pool[dest]->lock) == 0) {
				put(&pool[dest]->file, &buf[i]);
				pthread_mutex_unlock(&pool[dest]->lock);
				added = 1;
			} else {
				dest = (dest + 1) % nb_file_possibility;
				usleep(MICRO_SLEEP);
			}
		}
	}
	free(buf);
	return (int)n;
}

/**
 * @brief Répète `rebalance_pool_step` sur un pool jusqu'à épuisement du
 *        budget ou équilibre complet.
 *
 * `rebalance_pool_step` ne fixe qu'une paire (la plus pleine vers la plus
 * vide) par appel : son propre plafond de mouvement est souvent plus petit
 * que le budget disponible, laissant une grande partie du budget d'un tour
 * inutilisée. Cette boucle enchaîne les paires jusqu'à consommer tout
 * `max_packets` — chaque pas individuel reste court (un seul verrou de pool
 * à la fois), ce n'est que le nombre de pas par appel qui change.
 *
 * Termine en au plus `nb_file_possibility` pas structurellement (chaque pas
 * fixe définitivement au moins une file à sa cible) — garde-fou de boucle
 * par prudence.
 *
 * @param max_packets Budget total pour ce pool, réparti sur autant de pas
 *                     que nécessaire.
 * @return             Nombre total de possibilités déplacées.
 */
static int rebalance_pool_until_budget(file_possibility_t **pool, int max_packets)
{
	int moved_total = 0;
	int rounds = 0;
	while (moved_total < max_packets && rounds < nb_file_possibility * 2) {
		int moved = rebalance_pool_step(pool, max_packets - moved_total);
		if (moved <= 0) {
			break;
		}
		moved_total += moved;
		rounds++;
	}
	return moved_total;
}

int datamanager_rebalance_step(int max_packets)
{
	int moved = rebalance_pool_until_budget(file_possibility, max_packets);
	moved += rebalance_pool_until_budget(file_possibility_checked, max_packets);
	return moved;
}

/**
 * @brief Récupère des possibilités depuis le serveur TCP.
 *
 * Envoie `max_result` fois INST_GET et collecte les paquets reçus dans `result`.
 * Un INST_NULL en réponse indique qu'il n'y a plus de possibilité disponible.
 *
 * @param client_possibility Contexte du thread client.
 * @param result             Tableau de résultats à remplir.
 * @param max_result         Nombre maximum de possibilités à demander.
 */
void scroll_from_server(client_possibility_t *client_possibility, array_possibility_packet *result, int max_result)
{
	// Échange réseau atomique : empêche l'entrelacement avec le thread de recherche.
	server_socket_io_lock(client_possibility);
	int socket_id = check_and_connect_to_server(client_possibility);
	if (socket_id == -1) {
		server_socket_io_unlock(client_possibility);
		return;
	}

	// Client pruner : échange par lot (un seul aller-retour pour jusqu'à
	// max_result possibilités). max_result = pruner_batch_size borne la mémoire
	// détenue : le pruner ne reçoit jamais plus que ce lot.
	if (pruner_mode)
	{
		int32_t requested = max_result;
		int ok = (send_instruction(socket_id, INST_GET_TO_CHECK_BATCH) > 0)
		      && (send_all(socket_id, &requested, sizeof(requested)) == (long)sizeof(requested));
		int32_t k = 0;
		if (ok && recv_all(socket_id, &k, sizeof(k)) == (long)sizeof(k) && k > 0)
		{
			if (k > requested) k = requested; // garde-fou défensif
			size_t bytes = (size_t)k * sizeof(struct possibility_packet);
			result->possibilities = malloc(bytes);
			if (recv_all(socket_id, result->possibilities, bytes) == (long)bytes)
			{
				result->size = k;
			}
			else
			{
				log_error("batch tocheck : bloc de %d possibilités incomplet\n", k);
				free(result->possibilities);
				result->possibilities = NULL;
				result->size = 0;
			}
		}
		// k == 0 → rien de disponible : result reste vide.
		server_socket_io_unlock(client_possibility);
		return;
	}

	File file;
	init_file(&file, sizeof(struct possibility_packet));

	struct possibility_packet buffer;
	int r;
	int connection_ok = 1;
	for(r=0; r < max_result && connection_ok; r++){
		send_instruction(socket_id, INST_GET);
		// Réponse cadrée (VERSION 7) : compte K puis, si K > 0, K paquets.
		// recv_all réassemble les lectures partielles — l'ancien recv() brut
		// discriminait la réponse par sa longueur (1 octet = INST_NULL,
		// sizeof(paquet) = possibilité) et prenait une lecture TCP partielle
		// pour un paquet valide, en désynchronisant tout le flux.
		int32_t k = 0;
		if (recv_all(socket_id, &k, sizeof(k)) != (long)sizeof(k)) {
			log_errno("Error when receive possibility count => ");
			connection_ok = 0;
			break;
		}
		if (k < 0 || k > PRUNER_BATCH_MAX) {
			log_error("GET : compte de possibilités aberrant (%d)\n", k);
			break;
		}
		if (k == 0)
		{
#ifdef DEBUG_SOCKET
			log_info("No possibility recept\n");
#endif // DEBUG_SOCKET
			continue;
		}
		for (int32_t i = 0; i < k && connection_ok; i++) {
			if (recv_all(socket_id, &buffer, sizeof(buffer)) != (long)sizeof(buffer)) {
				log_error("GET : possibilité incomplète (paquet %d/%d)\n", i + 1, k);
				connection_ok = 0;
				break;
			}
#ifdef DEBUG_CHECK_POSSIBILITY
            int analyse = check_possibility(&buffer, client_possibility->all_rotate_part);
            if (analyse < 0)
            {
                log_debug("possibility error : %i\n",analyse);
                log_debug(" ---");
                print_possibility_packet(&buffer);
            }
#endif // DEBUG_CHECK_POSSIBILITY
			put(&file, &buffer);
		}
	}

	if(file.size > 0)
	{
		result->possibilities = malloc(file.size * sizeof(struct possibility_packet));
		int p = 0;
		while(file.size > 0)
		{
			scroll(&file, &result->possibilities[p]);
			result->size++;
			p++;
		}
	}
	server_socket_io_unlock(client_possibility);
}

/**
 * @brief Extrait des possibilités des files locales.
 *
 * Parcourt les 10 files en mode trylock pour en trouver une disponible.
 * Extrait jusqu'à `max_result` possibilités depuis la première file non vide trouvée.
 * Réessaie sur les autres files si la première est vide.
 *
 * Sortie bornée : si `pool` reste intégralement verrouillé au-delà de
 * DATAMANAGER_TRYLOCK_MAX_SWEEPS tours (maintenance en cours — sauvegarde,
 * restore, tri...), abandonne avec `result->size == 0` plutôt que de tourner
 * indéfiniment. Indiscernable, côté appelant, d'un pool réellement vide —
 * réponse déjà normale et supportée du protocole (K = 0) depuis la v7.
 *
 * @param result     Tableau de résultats à remplir.
 * @param max_result Nombre maximum de possibilités à extraire.
 * @param rr_counter État round-robin du pool (`rr_scroll_unchecked`/`rr_scroll_checked`) —
 *                   fait démarrer chaque appel sur une file différente plutôt que
 *                   toujours la file 0 (cf. `datamanager_rr_next_start`).
 * @param pool_rate Compteur de débit VENTILÉ par pool
 *                   (`stock_removes_unchecked_rate`/`stock_removes_checked_rate`) —
 *                   enregistré EN PLUS de l'agrégat `stock_removes_rate`
 *                   (inchangé), jamais à sa place.
 */
static void scroll_from_pool(file_possibility_t **pool, array_possibility_packet *result, int max_result,
                              unsigned int *rr_counter, stock_rate_counter_t *pool_rate)
{
	int getpossibility = 0;
	int currfile = 0;
	int filetested[nb_file_possibility];
	int failed_sweeps = 0;
	int i;
	int rr_start = datamanager_rr_next_start(rr_counter, nb_file_possibility);
	for(i = 0; i < nb_file_possibility; i++)
	{
		filetested[i] = 0;
	}
	while (getpossibility == 0) {
		int k;
		for (k=0; k < nb_file_possibility && getpossibility == 0; k++)
		{
			int f = (rr_start + k) % nb_file_possibility;
			if(filetested[f] == 0)
			{
				currfile = f;
				if(pthread_mutex_trylock(&pool[currfile]->lock) == 0)
				{
					int p;
					int nothing = 0;
					File file;
					struct possibility_packet packet;
					init_file(&file, sizeof(struct possibility_packet));
					for(p=0; p < max_result && nothing == 0;p++)
					{
						if(scroll(&pool[currfile]->file, &packet))
						{
							put(&file, &packet);
						} else
						{
							nothing = 1;
						}
					}

					if(file.size > 0)
					{
						unsigned int moved = (unsigned int)file.size;
						result->possibilities = malloc(file.size * sizeof(struct possibility_packet));
						p = 0;
						while(file.size > 0)
						{
							scroll(&file, &result->possibilities[p]);
							result->size++;
							p++;
						}
						time_t now_rate = time(NULL);
						stock_rate_record(&stock_removes_rate, moved, now_rate);
						stock_rate_record(pool_rate, moved, now_rate);
					}

					filetested[f] = 1;
					getpossibility = 1;
					pthread_mutex_unlock(&pool[currfile]->lock);
				}
			}
		}

		if(getpossibility == 0)
		{
			// Le for ci-dessus n'a réussi aucun trylock sur les files restant à
			// tester (sinon getpossibility serait passé à 1 et aurait stoppé le
			// for) : un tour complet s'est fait sans le moindre verrou. On cède
			// le CPU uniquement dans ce cas ; le cas nominal (verrou pris dès le
			// premier essai) ne passe jamais par ici. Même motif que
			// add_possibility_analysed.
			usleep(MICRO_SLEEP);
			// Sortie bornée: cf. le commentaire de la fonction. `result`
			// est déjà à taille 0 (initialisé par l'appelant) : rien à défaire.
			failed_sweeps++;
			if (failed_sweeps >= DATAMANAGER_TRYLOCK_MAX_SWEEPS)
			{
				return;
			}
		}

		if(getpossibility == 1 && result->size == 0)
		{
			int all_tested = 1;
			for(int f=0; f < nb_file_possibility; f++)
			{
				if(filetested[f] == 0)
				{
					all_tested = 0;
					break;
				}
			}

			if(all_tested == 0)
			{
				getpossibility = 0;
				result->size = 0;
			}
		}
	}
}

/**
 * @brief Extrait des possibilités pour un client de recherche.
 *
 * Sert en priorité le pool vérifié par les pruners, puis, s'il est vide, le
 * pool historique (comportement inchangé quand aucun pruner ne tourne).
 *
 * @param result     Tableau de résultats à remplir.
 * @param max_result Nombre maximum de possibilités à extraire.
 */
void scroll_from_local(array_possibility_packet *result, int max_result)
{
	scroll_from_pool(file_possibility_checked, result, max_result, &rr_scroll_checked, &stock_removes_checked_rate);
	if(result->size == 0)
	{
		scroll_from_pool(file_possibility, result, max_result, &rr_scroll_unchecked, &stock_removes_unchecked_rate);
	}
}

/**
 * @brief Extrait des possibilités non vérifiées pour un client pruner.
 *
 * Pool historique uniquement : pas de repli sur le pool vérifié (une
 * possibilité déjà vérifiée n'a pas besoin de repasser par un pruner).
 *
 * @param result     Tableau de résultats à remplir.
 * @param max_result Nombre maximum de possibilités à extraire.
 */
void scroll_from_local_tocheck(array_possibility_packet *result, int max_result)
{
	scroll_from_pool(file_possibility, result, max_result, &rr_scroll_unchecked, &stock_removes_unchecked_rate);
}

array_possibility_packet *get_last_possibility(client_possibility_t *client_possibility, int max_result, int *from_server)
{
	array_possibility_packet *result = malloc(sizeof(array_possibility_packet));
	result->size = 0;
	result->possibilities = NULL;
	if (from_server != NULL) {
		*from_server = 0;
	}

	scroll_from_local(result, max_result);

	if(result->size == 0 && server_ip != NULL)
	{
		scroll_from_server(client_possibility, result, max_result);
		if (from_server != NULL && result->size > 0) {
			*from_server = 1;
		}
	}

	if(result->size == 0)
	{
#ifdef DEBUG_SOCKET
		log_info("result 0 \n");
#endif // DEBUG_SOCKET
	}
	return result;
}

/**
 * @brief Extrait des possibilités non vérifiées du datamanager local (côté serveur).
 *
 * Utilisé par le handler INST_GET_TO_CHECK : aucune bascule réseau, le serveur
 * sert son propre stock.
 *
 * @param max_result Nombre maximum de possibilités à extraire.
 * @return           Tableau alloué (à libérer avec `free_array_possibility_packet`).
 */
array_possibility_packet *get_last_possibility_tocheck(int max_result)
{
	array_possibility_packet *result = malloc(sizeof(array_possibility_packet));
	result->size = 0;
	result->possibilities = NULL;

	scroll_from_local_tocheck(result, max_result);

	return result;
}

unsigned long long file_size(int nfile)
{
	if(nfile >= 0 && nfile < nb_file_possibility)
	{
		return file_possibility[nfile]->file.size;
	}
    return 0;
}

unsigned long long file_checked_size(int nfile)
{
	if(nfile >= 0 && nfile < nb_file_possibility)
	{
		return file_possibility_checked[nfile]->file.size;
	}
	return 0;
}

unsigned long long file_analysed_size(int nfile)
{
	if(nfile >= 0 && nfile < nb_file_possibility)
	{
		return file_possibility_analysed[nfile]->file.size;
	}
	return 0;
}

unsigned long long datas_size(void)
{
    unsigned long long result = 0;
	int f;
	for(f=0; f < nb_file_possibility; f++)
	{
        result += file_size(f);
        result += file_checked_size(f);
	}
	return result;
}

/**
 * @brief Verrouille toutes les files de possibilités et active le mode maintenance.
 *
 * Appel bloquant (pthread_mutex_lock) sur chacune des `nb_file_possibility` files.
 * Doit être suivi d'un appel à `unlock_all_file`.
 */
void lock_all_file(void)
{
	maintenance = 1;
	int fp;
	// Bloquage des files (les deux pools : non vérifié et vérifié)
	for (fp=0; fp < nb_file_possibility; fp++)
	{
		pthread_mutex_lock(&file_possibility[fp]->lock);
		pthread_mutex_lock(&file_possibility_checked[fp]->lock);
	}
}

/**
 * @brief Déverrouille toutes les files de possibilités et désactive le mode maintenance.
 */
void unlock_all_file(void)
{
	int fp;
	//libération des files (les deux pools)
	for (fp=0; fp < nb_file_possibility; fp++)
	{
		pthread_mutex_unlock(&file_possibility[fp]->lock);
		pthread_mutex_unlock(&file_possibility_checked[fp]->lock);
	}
	maintenance = 0;
}

/**
 * @brief Construit le nom du fichier temporaire utilisé pour une sauvegarde atomique.
 *
 * Alloue `strlen(filename) + 5` octets (suffixe ".tmp" + '\0') ; l'appelant doit
 * `free()` le résultat.
 */
static char *backup_tmp_path(const char *filename)
{
	size_t len = strlen(filename);
	char *tmp = malloc(len + 5); // ".tmp" + '\0'
	if (tmp != NULL)
	{
		memcpy(tmp, filename, len);
		memcpy(tmp + len, ".tmp", 5);
	}
	return tmp;
}

int backup(char *filename)
{
	if(maintenance)
	{
		return BACKUP_SKIPPED_MAINTENANCE;
	}

	char *tmp_filename = backup_tmp_path(filename);
	if(!tmp_filename)
	{
		log_error("backup file :%s (malloc échoué)\n", filename);
		return BACKUP_ERROR;
	}

	FILE *f = fopen(tmp_filename, "w");
	if(!f)
	{
		log_errno("backup file :%s ",tmp_filename);
		free(tmp_filename);
		return BACKUP_ERROR;
	}
	// Tampon explicite (1 Mio) : le tampon stdio par défaut ramène un stock de
	// plusieurs millions de possibilités à quelques milliers d'appels write()
	// au lieu d'un par possibilité — significatif car ces write() ont lieu
	// sous lock_all_file().
	setvbuf(f, NULL, _IOFBF, 1 << 20);

	int write_error = 0;
	lock_all_file();
	int fp;
	for (fp=0; fp < nb_file_possibility; fp++)
	{
		Element *currElement = file_possibility[fp]->file.start;
		while(currElement != NULL)
		{
			if(currElement->value != NULL)
			{
				struct possibility_packet *possibility = (struct possibility_packet *)currElement->value;
				if(fwrite(possibility, sizeof(struct possibility_packet), 1, f) != 1)
				{
					write_error = 1;
				}
			}
			currElement = currElement->next;
		}
		// Pool vérifié : le flag `checked` est dans le paquet, la restauration
		// re-routera automatiquement chaque possibilité dans le bon pool.
		currElement = file_possibility_checked[fp]->file.start;
		while(currElement != NULL)
		{
			if(currElement->value != NULL)
			{
				struct possibility_packet *possibility = (struct possibility_packet *)currElement->value;
				if(fwrite(possibility, sizeof(struct possibility_packet), 1, f) != 1)
				{
					write_error = 1;
				}
			}
			currElement = currElement->next;
		}
	}
	unlock_all_file();

	if(fclose(f) != 0)
	{
		write_error = 1;
	}

	if(write_error)
	{
		log_error("backup file :%s (écriture incomplète)\n", tmp_filename);
		unlink(tmp_filename);
		free(tmp_filename);
		return BACKUP_ERROR;
	}

	if(rename(tmp_filename, filename) != 0)
	{
		log_errno("backup file :%s -> %s (rename) ",tmp_filename, filename);
		unlink(tmp_filename);
		free(tmp_filename);
		return BACKUP_ERROR;
	}

	free(tmp_filename);
	return BACKUP_OK;
}

/**
 * @brief Verrouille toutes les files des possibilités en cours d'analyse.
 */
void lock_all_file_analysed(void)
{
	maintenance = 1;
	int fp;
	// Bloquage des files
	for (fp=0; fp < nb_file_possibility; fp++)
	{
		pthread_mutex_lock(&file_possibility_analysed[fp]->lock);
	}
}

/**
 * @brief Déverrouille toutes les files des possibilités en cours d'analyse.
 */
void unlock_all_file_analysed(void)
{
	int fp;
	//libération des files
	for (fp=0; fp < nb_file_possibility; fp++)
	{
		pthread_mutex_unlock(&file_possibility_analysed[fp]->lock);
	}
	maintenance = 0;
}

int backup_analysed(char *filename)
{
	if(maintenance)
	{
		return BACKUP_SKIPPED_MAINTENANCE;
	}

	char *tmp_filename = backup_tmp_path(filename);
	if(!tmp_filename)
	{
		log_error("backup_analysed file :%s (malloc échoué)\n", filename);
		return BACKUP_ERROR;
	}

	FILE *f = fopen(tmp_filename, "w");
	if(!f)
	{
		log_errno("backup_analysed file :%s ",tmp_filename);
		free(tmp_filename);
		return BACKUP_ERROR;
	}
	setvbuf(f, NULL, _IOFBF, 1 << 20);

	int write_error = 0;
	lock_all_file_analysed();
	int fp;
	for (fp=0; fp < nb_file_possibility; fp++)
	{
		Element *currElement = file_possibility_analysed[fp]->file.start;
		while(currElement != NULL)
		{
			if(currElement->value != NULL)
			{
				struct possibility_packet *possibility = (struct possibility_packet *)currElement->value;
				if(fwrite(possibility, sizeof(struct possibility_packet), 1, f) != 1)
				{
					write_error = 1;
				}
			}
			currElement = currElement->next;
		}
	}
	unlock_all_file_analysed();

	if(fclose(f) != 0)
	{
		write_error = 1;
	}

	if(write_error)
	{
		log_error("backup_analysed file :%s (écriture incomplète)\n", tmp_filename);
		unlink(tmp_filename);
		free(tmp_filename);
		return BACKUP_ERROR;
	}

	if(rename(tmp_filename, filename) != 0)
	{
		log_errno("backup_analysed file :%s -> %s (rename) ",tmp_filename, filename);
		unlink(tmp_filename);
		free(tmp_filename);
		return BACKUP_ERROR;
	}

	free(tmp_filename);
	return BACKUP_OK;
}

/**
 * @brief Sauvegarde le pool analysé et le stock à un instant T unique —
 *        corrige un trou de `backup()`/`backup_analysed()` appelées l'une
 *        après l'autre : une possibilité acquittée entre les deux instants
 *        disparaissait des deux sauvegardes.
 *
 * Phase 1 : verrouille toutes les files des trois pools avant d'écrire quoi
 * que ce soit — cette fenêtre de gel simultané rend l'image cohérente à T,
 * pas un verrouillage progressif qui laisserait une possibilité migrer d'une
 * file pas encore gelée vers une file déjà écrite. `maintenance` est posé
 * une seule fois explicitement ici, pas via `lock_all_file()`/
 * `lock_all_file_analysed()` : leurs `unlock_*` remettraient le drapeau à 0
 * dès la première famille libérée.
 *
 * Phase 2 : écrit puis libère progressivement, une file à la fois — pool
 * analysé d'abord (un `INST_GET` exige les deux verrous, donc libérer le
 * stock en premier ne raccourcirait rien), puis chaque file de stock. La
 * fenêtre de blocage total pour un client vaut ainsi le temps d'écriture
 * d'une file, pas de la sauvegarde entière.
 *
 * Ne modifie jamais les pools eux-mêmes : une erreur d'écriture à
 * mi-parcours ne perd ni ne duplique aucune possibilité en mémoire — seul
 * le fichier `.tmp` correspondant est invalidé.
 *
 * @return Code du volet stock (même convention que `backup`).
 */
int consistent_backup(char *stock_filename, char *analysed_filename, int *out_analysed_status,
                       const char *spill_snapshot_dir, consistent_backup_spill_snapshot_fn spill_snapshot_fn)
{
	if (out_analysed_status != NULL)
	{
		*out_analysed_status = BACKUP_ERROR;
	}

	if (maintenance)
	{
		if (out_analysed_status != NULL) { *out_analysed_status = BACKUP_SKIPPED_MAINTENANCE; }
		return BACKUP_SKIPPED_MAINTENANCE;
	}

	char *stock_tmp = backup_tmp_path(stock_filename);
	if (!stock_tmp)
	{
		log_error("backup (cohérent) file :%s (malloc échoué)\n", stock_filename);
		return BACKUP_ERROR;
	}
	char *analysed_tmp = backup_tmp_path(analysed_filename);
	if (!analysed_tmp)
	{
		log_error("backup (cohérent) file :%s (malloc échoué)\n", analysed_filename);
		free(stock_tmp);
		return BACKUP_ERROR;
	}

	FILE *fstock = fopen(stock_tmp, "w");
	if (!fstock)
	{
		log_errno("backup (cohérent) file :%s ", stock_tmp);
		free(stock_tmp);
		free(analysed_tmp);
		return BACKUP_ERROR;
	}
	setvbuf(fstock, NULL, _IOFBF, 1 << 20);

	FILE *fanalysed = fopen(analysed_tmp, "w");
	if (!fanalysed)
	{
		log_errno("backup (cohérent) file :%s ", analysed_tmp);
		fclose(fstock);
		unlink(stock_tmp);
		free(stock_tmp);
		free(analysed_tmp);
		return BACKUP_ERROR;
	}
	setvbuf(fanalysed, NULL, _IOFBF, 1 << 20);

	// Phase 1 : gel global à l'instant T (cf. docstring ci-dessus).
	maintenance = 1;
	int fp;
	for (fp = 0; fp < nb_file_possibility; fp++)
	{
		pthread_mutex_lock(&file_possibility_analysed[fp]->lock);
	}
	for (fp = 0; fp < nb_file_possibility; fp++)
	{
		pthread_mutex_lock(&file_possibility[fp]->lock);
		pthread_mutex_lock(&file_possibility_checked[fp]->lock);
	}

	// Cliché du débordement disque, pendant que `maintenance` interdit
	// toute éviction/rechargement concurrent (cf. `stock_spill_step`) — sans
	// cette fenêtre, une possibilité pourrait migrer entre RAM et disque
	// pendant la capture et finir dupliquée ou absente des deux côtés. Le
	// compte renvoyé est écrit plus bas dans `<stock_filename>.spillcount`,
	// une fois le volet stock confirmé écrit — c'est ce compte qui permettra
	// à `restore` de détecter une restauration partielle du débordement
	// (correctif : avant, un cliché absent/mal configuré à la restauration
	// était toléré en silence, perdant des possibilités sans le signaler).
	unsigned long long spill_packets_snapshotted = 0;
	int spill_snapshot_taken = 0;
	if (spill_snapshot_dir != NULL && spill_snapshot_fn != NULL)
	{
		spill_packets_snapshotted = spill_snapshot_fn(spill_snapshot_dir);
		spill_snapshot_taken = 1;
	}

	// Phase 2a : pool analysé, libéré au fil de l'écriture.
	int write_error_analysed = 0;
	for (fp = 0; fp < nb_file_possibility; fp++)
	{
		Element *currElement = file_possibility_analysed[fp]->file.start;
		while (currElement != NULL)
		{
			if (currElement->value != NULL)
			{
				struct possibility_packet *possibility = (struct possibility_packet *)currElement->value;
				if (fwrite(possibility, sizeof(struct possibility_packet), 1, fanalysed) != 1)
				{
					write_error_analysed = 1;
				}
			}
			currElement = currElement->next;
		}
		pthread_mutex_unlock(&file_possibility_analysed[fp]->lock);
	}

	// Phase 2b : stock (non vérifié + vérifié), une file à la fois, libérée
	// dès son écriture terminée.
	int write_error_stock = 0;
	for (fp = 0; fp < nb_file_possibility; fp++)
	{
		Element *currElement = file_possibility[fp]->file.start;
		while (currElement != NULL)
		{
			if (currElement->value != NULL)
			{
				struct possibility_packet *possibility = (struct possibility_packet *)currElement->value;
				if (fwrite(possibility, sizeof(struct possibility_packet), 1, fstock) != 1)
				{
					write_error_stock = 1;
				}
			}
			currElement = currElement->next;
		}
		currElement = file_possibility_checked[fp]->file.start;
		while (currElement != NULL)
		{
			if (currElement->value != NULL)
			{
				struct possibility_packet *possibility = (struct possibility_packet *)currElement->value;
				if (fwrite(possibility, sizeof(struct possibility_packet), 1, fstock) != 1)
				{
					write_error_stock = 1;
				}
			}
			currElement = currElement->next;
		}
		pthread_mutex_unlock(&file_possibility[fp]->lock);
		pthread_mutex_unlock(&file_possibility_checked[fp]->lock);
	}
	maintenance = 0;

	int rc_analysed = BACKUP_OK;
	if (fclose(fanalysed) != 0)
	{
		write_error_analysed = 1;
	}
	if (write_error_analysed)
	{
		log_error("backup (cohérent) file :%s (écriture incomplète)\n", analysed_tmp);
		unlink(analysed_tmp);
		rc_analysed = BACKUP_ERROR;
	}
	else if (rename(analysed_tmp, analysed_filename) != 0)
	{
		log_errno("backup (cohérent) file :%s -> %s (rename) ", analysed_tmp, analysed_filename);
		unlink(analysed_tmp);
		rc_analysed = BACKUP_ERROR;
	}

	int rc_stock = BACKUP_OK;
	if (fclose(fstock) != 0)
	{
		write_error_stock = 1;
	}
	if (write_error_stock)
	{
		log_error("backup (cohérent) file :%s (écriture incomplète)\n", stock_tmp);
		unlink(stock_tmp);
		rc_stock = BACKUP_ERROR;
	}
	else if (rename(stock_tmp, stock_filename) != 0)
	{
		log_errno("backup (cohérent) file :%s -> %s (rename) ", stock_tmp, stock_filename);
		unlink(stock_tmp);
		rc_stock = BACKUP_ERROR;
	}
	else if (spill_snapshot_taken)
	{
		// Accessoire écrit UNIQUEMENT si un cliché de débordement a
		// réellement été demandé (spill_snapshot_fn != NULL) : son ABSENCE
		// doit rester un signal fiable de « rien à vérifier » (sauvegarde
		// antérieure à ce correctif, ou appelant sans cliché — ex. rôle
		// client, ou l'unique site interne à datamanager.c), jamais un faux
		// « 0 possibilité déportée ».
		size_t path_len = strlen(stock_filename) + strlen(".spillcount") + 1;
		char *sidecar_path = malloc(path_len);
		if (sidecar_path != NULL)
		{
			snprintf(sidecar_path, path_len, "%s.spillcount", stock_filename);
			char *sidecar_tmp = backup_tmp_path(sidecar_path);
			if (sidecar_tmp != NULL)
			{
				FILE *fsidecar = fopen(sidecar_tmp, "w");
				if (fsidecar != NULL)
				{
					int written_ok = (fprintf(fsidecar, "%llu\n", spill_packets_snapshotted) > 0);
					if (fclose(fsidecar) != 0) { written_ok = 0; }
					if (!written_ok || rename(sidecar_tmp, sidecar_path) != 0)
					{
						log_error("backup (cohérent) : échec d'écriture de %s (compte de débordement non "
						          "vérifiable à la prochaine restauration)\n", sidecar_path);
						unlink(sidecar_tmp);
					}
				}
				else
				{
					log_errno("backup (cohérent) file :%s ", sidecar_tmp);
				}
				free(sidecar_tmp);
			}
			free(sidecar_path);
		}
	}

	free(stock_tmp);
	free(analysed_tmp);
	if (out_analysed_status != NULL) { *out_analysed_status = rc_analysed; }
	return rc_stock;
}

int datamanager_read_spillcount_sidecar(const char *stock_filename, unsigned long long *out_count)
{
	size_t path_len = strlen(stock_filename) + strlen(".spillcount") + 1;
	char *sidecar_path = malloc(path_len);
	if (sidecar_path == NULL)
	{
		return 0;
	}
	snprintf(sidecar_path, path_len, "%s.spillcount", stock_filename);

	FILE *f = fopen(sidecar_path, "r");
	free(sidecar_path);
	if (f == NULL)
	{
		return 0;
	}

	unsigned long long count = 0;
	int ok = (fscanf(f, "%llu", &count) == 1);
	fclose(f);
	if (!ok)
	{
		return 0;
	}
	if (out_count != NULL) { *out_count = count; }
	return 1;
}

int import(client_possibility_t *client_possibility, char *filename)
{
    FILE *f = fopen(filename, "r");
    if(!f)
    {
        log_errno("import file :%s ",filename);
        return -1;
    }
    
    // NOTE VERSION 13 (cf. docs/autosearch_step.md) : un fichier .back écrit
    // avant ce bump porte `alloc` au sens curseur (position dans
    // directions[]), pas au sens nombre de pièces posées — et même en ordre
    // fixe ce curseur divergeait déjà du compte réel dans l'immense majorité
    // des cas. Recomptage systématique et INCONDITIONNEL à chaque lecture,
    // sans détection de version de fichier : `possibility_placed_count` est
    // idempotente sur un paquet déjà correct (écrit par du code v13, `alloc`
    // y vaut déjà ce compte), donc appliquer le recomptage à tous les
    // paquets — v12 ou v13 — donne le même résultat qu'une détection
    // explicite, sans marqueur de format à maintenir ni logique de version à
    // faire évoluer au prochain bump. Aucun paquet n'est jamais rejeté,
    // seulement réétiqueté si besoin.
    //
    // `min_candidats` (score MRV) suit une règle différente : il ne se
    // recalcule pas depuis la grille (il dépend de l'historique de
    // recherche, pas de l'état). Un fichier écrit avant son introduction
    // porte donc une valeur non fiable dans cet octet (ex-bourrage
    // d'alignement) : on l'écrase inconditionnellement par la sentinelle
    // « inconnu » plutôt que de la faire confiance.
    struct possibility_packet *possibility = malloc(sizeof(struct possibility_packet));
    while(fread(possibility, sizeof(struct possibility_packet),1,f))
    {
        // Anciens fichiers .back (v4) : l'octet `checked` correspond à du padding
        // (taille de structure inchangée) et peut contenir n'importe quoi.
        // On assainit : tout ce qui n'est pas exactement 1 redevient « à vérifier ».
        if (possibility->checked != 1) {
            possibility->checked = 0;
        }
        possibility->alloc = (uint16_t)possibility_placed_count(possibility);
        possibility->min_candidats = POSSIBILITY_MIN_CANDIDATS_UNKNOWN;
        array_possibility_packet *possibilities = malloc(sizeof(array_possibility_packet));
        possibilities->size = 1;
        possibilities->possibilities = malloc(sizeof(struct possibility_packet));
        memcpy(&possibilities->possibilities[0], possibility, sizeof(struct possibility_packet));
        add_possibility(client_possibility, possibilities);

        free_array_possibility_packet(possibilities);
    }

    free(possibility);
    
    
    fclose(f);
    return 0;
}

int restore(char *filename)
{
	// Contrôle avant vidage : un fichier illisible ne doit pas faire perdre le stock courant
	FILE *f = fopen(filename, "r");
	if(!f)
	{
		log_errno("restore file :%s ",filename);
		return -1;
	}
	fclose(f);

	lock_all_file();
	int fp;
	//vidage des files (les deux pools)
	for (fp=0; fp < nb_file_possibility; fp++)
	{
		File *suite = &file_possibility[fp]->file;
		struct possibility_packet value;
		while(suite->size >0)
		{
			scroll(suite, &value);
		}
		suite = &file_possibility_checked[fp]->file;
		while(suite->size >0)
		{
			scroll(suite, &value);
		}
	}

	unlock_all_file();

	return import(NULL, filename);
}

int import_json(void) {
	lock_all_file();
	
	int fp;
	//vidage des files
	for (fp=0; fp < nb_file_possibility; fp++)
	{
		File *suite = &file_possibility[fp]->file;
		while(suite->size >0)
		{
			struct possibility_packet *value = malloc(sizeof(struct possibility_packet));
			scroll(suite, value);
			free(value);
		}
	}
	unlock_all_file();

	//const char *json = "{\"alloc\": 98, \"x\": 4, \"y\": 1, \"grid\": [[259, 571, 567, 525, 554, 524, 549, 522, 536, 543, 541, 539, 528, 563, 551, 514], [291, 201, 763, 213, -2, -2, -2, -2, -2, -2, -2, -2, -2, 629, 481, 825], [309, 699, 976, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, 1023, 842, 817], [301, 1010, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, 776], [263, 435, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, 790], [270, 1008, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, 794], [297, 495, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, 777], [289, 888, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, 773], [273, 844, -2, -2, -2, -2, -2, 651, -2, -2, -2, -2, -2, -2, -2, 783], [312, 200, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, 788], [290, 698, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, 787], [296, 996, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, 779], [274, 861, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, 818], [314, 998, 949, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, 249, 700, 798], [316, 1013, 1009, 849, 856, 345, 890, 389, 452, 735, 851, 319, 383, 110, 900, 796], [4, 21, 47, 44, 32, 36, 46, 48, 43, 23, 54, 38, 52, 6, 25, 769]]}";
	const char *json = "{\"alloc\": 120, \"x\" :9, \"y\": 13, \"grid\": [[259, 563, 567, 525, 554, 522, 536, 543, 544, 541, 540, 528, 518, 562, 551, 514], [283, 319, 377, 456, 845, 334, 113, 979, 982, 146, 622, 660, 641, 629, 481, 825], [286, 189, 976, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, 1023, 842, 817], [308, 422, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, 950, 776], [263, 434, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, 459, 790], [268, 253, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, 865, 794], [301, 508, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, 390, 777], [270, 132, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, 882, 783], [297, 624, -2, -2, -2, -2, -2, 651, -2, -2, -2, -2, -2, -2, 168, 788], [292, 98, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, 684, 787], [300, 588, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, 1018, 779], [289, 713, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, 855, 773], [273, 460, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, 908, 805], [314, 998, 949, -2, -2, -2, -2, -2, -2, -2, 853, 323, 706, 249, 131, 814], [316, 1013, 1009, 615, 379, 446, 1002, 496, 744, 725, 986, 647, 222, 759, 638, 802], [4, 21, 47, 48, 40, 18, 53, 43, 23, 56, 35, 54, 38, 59, 25, 769]]}";
	struct possibility_packet *possibility = read_from_json(json);
	if (possibility != NULL) {
		array_possibility_packet *possibilities = malloc(sizeof(array_possibility_packet));
		possibilities->size = 1;
		possibilities->possibilities = malloc(sizeof(struct possibility_packet));
		memcpy(&possibilities->possibilities[0], possibility, sizeof(struct possibility_packet));
		add_possibility(NULL, possibilities);
		
		free_array_possibility_packet(possibilities);
		free(possibility);
		return 1;
	}
	return 0;
}

int import_analysed(char *filename)
{
	FILE *f = fopen(filename, "r");
	if(!f)
	{
		log_errno("import_analysed file :%s ",filename);
		return -1;
	}
	
	// Même recomptage systématique et inconditionnel qu'`import` (cf. le
	// commentaire détaillé là-bas) : le pool analysé n'est pas un simple
	// journal, `alloc` y pilote `max_result` (add_possibility_analysed) ET
	// le hash de déduplication `hash_possibility_key`/`compare_possibility`
	// (datamanager.c), qui compare `alloc` champ à champ pour décider si
	// deux paquets désignent le même plateau. Un paquet analysé restauré
	// avec un `alloc` pré-v13 ne se déduperait plus jamais correctement
	// contre un paquet produit en direct par un client v13 sur le même
	// plateau — recompter ici est donc requis pour la même raison de fond
	// que pour le pool stock, pas seulement par cohérence cosmétique.
	struct possibility_packet *possibility = malloc(sizeof(struct possibility_packet));
	while(fread(possibility, sizeof(struct possibility_packet),1,f))
	{
		possibility->alloc = (uint16_t)possibility_placed_count(possibility);
		// min_candidats ne se recompte pas (dépend de l'historique de
		// recherche, pas de la grille) : écrasé par la sentinelle, comme à
		// l'import du pool stock.
		possibility->min_candidats = POSSIBILITY_MIN_CANDIDATS_UNKNOWN;
		add_possibility_analysed(possibility, -1);
	}

	free(possibility);


	fclose(f);
	return 0;
}

int restore_analysed(char *filename)
{
	// Contrôle avant vidage : un fichier illisible ne doit pas faire perdre le stock courant
	FILE *f = fopen(filename, "r");
	if(!f)
	{
		log_errno("restore_analysed file :%s ",filename);
		return -1;
	}
	fclose(f);

	lock_all_file_analysed();
	int fp;
	//vidage des files
	for (fp=0; fp < nb_file_possibility; fp++)
	{
		File *suite = &file_possibility_analysed[fp]->file;
		while(suite->size >0)
		{
			struct possibility_packet *value = malloc(sizeof(struct possibility_packet));
			scroll(suite, value);
			free(value);
		}
		// File désormais vide : purge en bloc de son index.
		analysed_index_clear(fp);
	}

	unlock_all_file_analysed();

	return import_analysed(filename);
}

int print_file(int fp)
{
    // Les deux pools sont affichés : non vérifié et vérifié (checked == 1)
    File *pools[2] = { &file_possibility[fp]->file, &file_possibility_checked[fp]->file };
    for (int p = 0; p < 2; p++)
    {
        Element *currElement = pools[p]->start;
        while(currElement != NULL)
        {
            if(currElement->value != NULL)
            {
                struct possibility_packet *possibility = (struct possibility_packet *)currElement->value;
                print_possibility_packet(possibility);
            } else {
                log_info("null value\n");
            }
            currElement = currElement->next;
        }
    }
    return 0;
}

int printdatamanager(void)
{
	lock_all_file();
	int fp;
	for (fp=0; fp < nb_file_possibility; fp++)
	{
		print_file(fp);
	}

	unlock_all_file();

	return 0;
}

/**
 * @brief Variante fichier de print_file (voir datamanager.h) : écrit dans
 *        @p out plutôt que dans les logs, en comptant les possibilités écrites.
 *
 * Même absence de verrouillage que print_file (le verrouillage, quand il a
 * lieu, est à la charge de l'appelant — voir fprint_datamanager).
 *
 * @param out   Flux ouvert en écriture.
 * @param fp    Numéro de la file à exporter.
 * @param count Accumulateur du nombre de possibilités écrites (non remis à
 *              zéro : permet à l'appelant de cumuler sur plusieurs files).
 *              NULL si l'appelant ne veut pas ce décompte.
 * @return      0 en cas de succès, -1 dès la première écriture en échec
 *              (le fichier peut alors être incomplet).
 */
int fprint_file(FILE *out, int fp, size_t *count)
{
    File *pools[2] = { &file_possibility[fp]->file, &file_possibility_checked[fp]->file };
    for (int p = 0; p < 2; p++)
    {
        Element *currElement = pools[p]->start;
        while(currElement != NULL)
        {
            if(currElement->value != NULL)
            {
                struct possibility_packet *possibility = (struct possibility_packet *)currElement->value;
                if (fprint_possibility_packet(out, possibility) != 0) {
                    return -1;
                }
                if (count != NULL) (*count)++;
            }
            currElement = currElement->next;
        }
    }
    return 0;
}

/**
 * @brief Variante fichier de printdatamanager (voir datamanager.h) : exporte
 *        toutes les files vers @p out au lieu des logs.
 * @param out   Flux ouvert en écriture.
 * @param count Décompte cumulé des possibilités écrites (NULL si inutile).
 * @return      0 en cas de succès, -1 si une file a échoué à s'écrire (export
 *              alors interrompu, le fichier peut être incomplet).
 */
int fprint_datamanager(FILE *out, size_t *count)
{
	lock_all_file();
	int rc = 0;
	for (int fp = 0; fp < nb_file_possibility; fp++)
	{
		if (fprint_file(out, fp, count) != 0) {
			rc = -1;
			break;
		}
	}
	unlock_all_file();
	return rc;
}

int print_file_analysed(int fp)
{
	log_info("file_analysed %i, size:%llu\n", fp, file_possibility_analysed[fp]->file.size);
    Element *currElement = file_possibility_analysed[fp]->file.start;
    while(currElement != NULL)
    {
        if(currElement->value != NULL)
        {
            struct possibility_packet *possibility = (struct possibility_packet *)currElement->value;
            print_possibility_packet(possibility);
        } else {
            log_info("null value\n");
        }
        currElement = currElement->next;
    }
    return 0;
}

/**
 * @brief Variante fichier de print_file_analysed (voir datamanager.h).
 * @param out   Flux ouvert en écriture.
 * @param fp    Numéro de la file d'analyse à exporter.
 * @param count Accumulateur du nombre de possibilités écrites (NULL si inutile).
 * @return      0 en cas de succès, -1 dès la première écriture en échec.
 */
int fprint_file_analysed(FILE *out, int fp, size_t *count)
{
    Element *currElement = file_possibility_analysed[fp]->file.start;
    while(currElement != NULL)
    {
        if(currElement->value != NULL)
        {
            struct possibility_packet *possibility = (struct possibility_packet *)currElement->value;
            if (fprint_possibility_packet(out, possibility) != 0) {
                return -1;
            }
            if (count != NULL) (*count)++;
        }
        currElement = currElement->next;
    }
    return 0;
}

int print_all_file_analysed(void)
{
	lock_all_file_analysed();
	int fp;
	for (fp=0; fp < nb_file_possibility; fp++)
	{
		print_file_analysed(fp);
	}

	unlock_all_file_analysed();

	return 0;
}

/**
 * @brief Variante fichier de print_all_file_analysed (voir datamanager.h).
 * @param out   Flux ouvert en écriture.
 * @param count Décompte cumulé des possibilités écrites (NULL si inutile).
 * @return      0 en cas de succès, -1 si une file a échoué à s'écrire.
 */
int fprint_all_file_analysed(FILE *out, size_t *count)
{
	lock_all_file_analysed();
	int rc = 0;
	for (int fp = 0; fp < nb_file_possibility; fp++)
	{
		if (fprint_file_analysed(out, fp, count) != 0) {
			rc = -1;
			break;
		}
	}
	unlock_all_file_analysed();
	return rc;
}

/**
 * @brief Regroupe toutes les files d'un pool dans sa file 0, sans verrouillage.
 *
 * Doit être appelée avec les files déjà verrouillées.
 *
 * @param pool Tableau de files (pool non vérifié ou vérifié).
 * @return     Taille totale de la file 0 après regroupement.
 */
static unsigned long long regroup_pool_nolock(file_possibility_t **pool)
{
	int fp;
	unsigned long long size = pool[0]->file.size;
	struct possibility_packet *packet = malloc(sizeof(struct possibility_packet));
	for (fp=1; fp < nb_file_possibility; fp++)
	{

		while (pool[fp]->file.size > 0) {

			scroll(&pool[fp]->file,packet);
			put(&pool[0]->file, packet);
			size++;

		}

	}
	free(packet);
	packet = NULL;
	return size;
}

/**
 * @brief Regroupe le pool non vérifié dans sa file 0, sans verrouillage.
 *
 * Doit être appelée avec les files déjà verrouillées. Utilisée en interne par
 * les routines de tri/répartition, qui ne travaillent que sur le pool non
 * vérifié.
 *
 * @return 0.
 */
unsigned long long regroup_datas_nolock(void)
{
	unsigned long long size = regroup_pool_nolock(file_possibility);
	log_info("regroup size :%llu\n",size);
	return 0;
}

int regroup_datas(void)
{
	lock_all_file();
	// Les deux pools sont regroupés indépendamment : non vérifié et vérifié
	regroup_pool_nolock(file_possibility);
	regroup_pool_nolock(file_possibility_checked);
	unlock_all_file();
	return 0;
}

/// Intervalle de re-essai (µs) pendant l'attente d'une place en RAM — plus
/// long que `MICRO_SLEEP` (contention de verrou) : on attend le tick du
/// thread de débordement (100 ms) ou un GET client, pas un verrou pris
/// momentanément. 20 ms = 5 essais par tick, réactif sans boucle chaude.
#define EXPAND_RAM_WAIT_POLL_US 20000
/// Fréquence (s) du rappel journalisé pendant une attente prolongée.
#define EXPAND_RAM_WAIT_LOG_INTERVAL_SEC 5

/**
 * @brief Insère `single` via `add_possibility`, en attendant patiemment
 *        qu'une place se libère si le plafond RAM est atteint — jamais un
 *        abandon silencieux. Réservée à `expand_datas_to_level`.
 *
 * Contexte mono-thread pré-fork : pas de « prochaine tentative naturelle »
 * comme côté client, donc la pause et le retry se font ici. Ne dépend
 * d'aucun symbole `stock_spill` (éviterait une inversion de dépendance) : se
 * contente de retenter, le débordement libère la RAM indépendamment de qui
 * la demande.
 *
 * Retente indéfiniment tant que le refus persiste et que le process n'est
 * pas en arrêt : un plafond RAM mal configuré se traduit par un démarrage
 * qui ne progresse plus (journalisé), jamais par une perte silencieuse.
 *
 * @param had_to_wait Mis à 1 si au moins un refus a été essuyé — l'appelant
 *                    cesse d'approfondir pour la passe courante seulement.
 * @return 1 si insérée, 0 si arrêt demandé pendant l'attente (`single` non
 *         inséré, à l'appelant de drainer proprement).
 */
static int add_possibility_with_retry_or_abort(array_possibility_packet *single, int *had_to_wait)
{
	int waited = 0;
	time_t first_refusal = 0;
	time_t last_log = 0;
	while (add_possibility(NULL, single) != 0) {
		if (request == REQUEST_STOP) {
			return 0;
		}
		time_t now = time(NULL);
		if (!waited) {
			first_refusal = now;
			last_log = now;
			log_error("expansion : plafond RAM atteint, possibilité mise en attente "
			          "(le débordement --stock-spill-dir devrait libérer de la place "
			          "sous peu ; si ce message persiste, relever --stock-max-ram ou "
			          "vérifier --stock-spill-dir)\n");
			waited = 1;
			*had_to_wait = 1;
		} else if (now - last_log >= EXPAND_RAM_WAIT_LOG_INTERVAL_SEC) {
			log_error("expansion : toujours en attente de place (plafond RAM atteint depuis %ld s)\n",
			          (long)(now - first_refusal));
			last_log = now;
		}
		usleep(EXPAND_RAM_WAIT_POLL_US);
	}
	if (waited) {
		log_info("expansion : place libérée, reprise après %ld s d'attente\n",
		         (long)(time(NULL) - first_refusal));
	}
	return 1;
}

/**
 * @brief Attend, entre deux passes de `expand_datas_to_level`, que le stock
 *        résident redescende sous le plafond RAM avant de tenter
 *        d'approfondir davantage.
 *
 * Une pression RAM rencontrée pendant une passe est transitoire — le thread
 * de débordement a vocation à la faire retomber en quelques dizaines/
 * centaines de ms — contrairement au garde-fou de volume
 * (`--expand-max-stock`), qui lui reste définitif. Sans cette attente, une
 * seule insertion ayant dû patienter arrêtait toute l'expansion pour de
 * bon, le niveau visé restant sous-atteint alors que les passes restantes
 * auraient pu progresser une fois la pression retombée.
 *
 * Non bornée sauf par `REQUEST_STOP` (même philosophie que
 * `add_possibility_with_retry_or_abort`). Journalise le début et la fin de
 * l'attente, avec un rappel périodique tant qu'elle se prolonge.
 *
 * @return 1 si de la place a été retrouvée (ou si aucun plafond n'est
 *         configuré), 0 si `REQUEST_STOP` a été demandé pendant l'attente.
 */
static int expand_wait_for_ram_headroom_between_passes(void)
{
	if (datamanager_ram_limit_packets() == 0
	    || datamanager_resident_packets() < datamanager_ram_limit_packets()) {
		return 1;
	}

	time_t first_refusal = time(NULL);
	time_t last_log = first_refusal;
	log_error("expansion : plafond RAM toujours atteint entre deux passes — attente que le "
	          "débordement (--stock-spill-dir) libère de la place avant de poursuivre "
	          "l'approfondissement (jusqu'à %d passe(s) au total)\n", expand_max_levels);
	while (datamanager_resident_packets() >= datamanager_ram_limit_packets()) {
		if (request == REQUEST_STOP) {
			return 0;
		}
		time_t now = time(NULL);
		if (now - last_log >= EXPAND_RAM_WAIT_LOG_INTERVAL_SEC) {
			log_error("expansion : toujours en attente entre deux passes (plafond RAM atteint depuis %ld s)\n",
			          (long)(now - first_refusal));
			last_log = now;
		}
		usleep(EXPAND_RAM_WAIT_POLL_US);
	}
	log_info("expansion : place retrouvée entre deux passes, reprise de l'approfondissement après %ld s d'attente\n",
	         (long)(time(NULL) - first_refusal));
	return 1;
}

int expand_datas_to_level(int target_level, map_big_array *mapParts, struct array_part *all_rotate_part)
{
    if (target_level <= 0) {
        return 0;
    }

    // Table d'indices de rotation attendue par search_possiblity_light. Dimension
    // ETERN_PARTS+1 : les identifiants de pièces vont de 1 à ETERN_PARTS et
    // servent d'indice (idParts[id]) — un tableau [ETERN_PARTS][4] déborderait à
    // id == ETERN_PARTS (même dimensionnement que first_possibility).
    int16_t idParts[ETERN_PARTS + 1][4];
    for (int p = 0; p <= ETERN_PARTS; p++) {
        for (int r = 0; r < 4; r++) {
            idParts[p][r] = (int16_t)(p + ETERN_PARTS * r);
        }
    }

    int rounds = 0;
    int cap_reached = 0;
    // 1 si le process a demandé l'arrêt (Ctrl-C) PENDANT une attente de place
    // RAM (add_possibility_with_retry_or_abort) : plus aucune insertion
    // n'est retentée au-delà de ce point, seulement un drainage propre du
    // travail encore en cours (voir plus bas) — le process s'arrête de
    // toute façon.
    int aborted = 0;
    while (rounds < expand_max_levels && !cap_reached && !aborted) {
        // 1. Draine tout le pool non vérifié dans une file de travail. L'expansion
        //    tourne au démarrage du serveur, mono-thread (aucun thread TCP ni
        //    rmnonext lancé) : le verrou est pris par cohérence, sans contention.
        //    Le pool est vide après ce drainage ; on le reconstruit ci-dessous.
        File work;
        init_file(&work, sizeof(struct possibility_packet));
        lock_all_file();
        for (int fp = 0; fp < nb_file_possibility; fp++) {
            struct possibility_packet drained;
            while (scroll(&file_possibility[fp]->file, &drained)) {
                put(&work, &drained);
            }
        }
        unlock_all_file();

        // 2. Développe d'un niveau chaque possibilité sous le niveau cible ;
        //    réinjecte inchangées celles qui l'ont déjà atteint. Une possibilité
        //    sans successeur (branche morte) disparaît — élagage gratuit.
        //
        //    Plafond en NOMBRE (garde-fou principal) : le facteur de branchement
        //    est inconnu et une seule passe peut exploser (des dizaines de
        //    milliers × le branchement). On compte donc le stock reconstruit et,
        //    dès `expand_max_stock` franchi, on cesse d'approfondir : le reste du
        //    travail est réinjecté tel quel (possibilités valides, niveau moindre).
        //    Le stock est déjà largement suffisant pour nourrir les clients.
        //
        //    Plafond RAM (--stock-max-ram) : un ADD qui y bute n'est JAMAIS
        //    abandonné — add_possibility_with_retry_or_abort attend patiemment
        //    (le thread de débordement, core/stock_spill.h, ou un GET client
        //    libère de la place pendant ce temps) plutôt que de perdre la
        //    possibilité. Dès le premier refus essuyé (had_to_wait), on cesse
        //    d'approfondir davantage POUR CETTE PASSE (ram_wait_this_round) —
        //    inutile de produire encore plus de travail au moment précis où
        //    la RAM est sous tension ; le reste de `work` est réinjecté tel
        //    quel, chaque insertion pouvant elle aussi attendre son tour.
        //    Contrairement au garde-fou de volume ci-dessus, cet arrêt n'est
        //    PAS définitif : une pause a lieu ENTRE cette passe et la
        //    suivante (voir plus bas, expand_wait_for_ram_headroom_between_passes)
        //    pour laisser la pression RAM retomber avant de retenter
        //    d'approfondir, jusqu'à expand_max_levels passes.
        unsigned long long produced = 0;
        int expanded_any = 0;
        // Local à CETTE passe (contrairement à cap_reached, qui reste vrai
        // une fois franchi et pilote AUSSI la boucle externe) : une pression
        // RAM rencontrée ici cesse d'approfondir pour le reste de la passe
        // courante uniquement — la pause entre deux passes ci-dessous laisse
        // sa chance à un approfondissement ultérieur une fois la pression
        // retombée, plutôt que d'arrêter l'expansion pour de bon (seul
        // cap_reached, le garde-fou de VOLUME, doit avoir cet effet définitif).
        int ram_wait_this_round = 0;
        // Vrai dès qu'un paquet PAS ENCORE au niveau cible est réinjecté tel
        // quel à cause de ram_wait_this_round (jamais à cause de deep_enough
        // ni du garde-fou de volume) : signale qu'il reste du vrai travail
        // d'approfondissement en attente, même si `expanded_any` est resté à
        // 0 (la pression a pu frapper dès le premier paquet de la passe,
        // avant tout approfondissement réel) — sans ce signal, le `break`
        // sur `!expanded_any` plus bas conclurait à tort « plus rien à
        // approfondir » et arrêterait l'expansion en silence.
        int shallow_deferred_by_ram_wait = 0;
        struct possibility_packet pkt;
        while (!aborted && scroll(&work, &pkt)) {
            int deep_enough = (pkt.alloc >= (uint16_t)target_level);
            if (deep_enough || cap_reached || ram_wait_this_round) {
                if (!deep_enough && !cap_reached) {
                    shallow_deferred_by_ram_wait = 1;
                }
                array_possibility_packet *single = build_single_array_possibility_packet(&pkt);
                if (!add_possibility_with_retry_or_abort(single, &ram_wait_this_round)) {
                    aborted = 1;
                }
                free_array_possibility_packet(single);
                produced++;
                continue;
            }
            expanded_any = 1;
            File children;
            init_file(&children, sizeof(struct possibility_packet));
            // search_possiblity_light choisit elle-même la case la plus
            // contrainte (MRV) et calcule sa clé : plus de clé pré-calculée
            // ici sur la case du curseur, cf. sa doc (possibility.h).
            search_possiblity_light(&children, &pkt, mapParts, all_rotate_part, idParts);
            // `children` est sur la PILE : la vidange par scroll libère chaque
            // Element ; pas de free_file (qui ferait free() de la structure pile).
            struct possibility_packet child;
            while (!aborted && scroll(&children, &child)) {
                array_possibility_packet *single = build_single_array_possibility_packet(&child);
                if (!add_possibility_with_retry_or_abort(single, &ram_wait_this_round)) {
                    aborted = 1;
                }
                free_array_possibility_packet(single);
                produced++;
            }
            if (aborted) {
                // Arrêt demandé pendant l'attente : draine le reste de
                // `children` SANS l'insérer (le process s'arrête de toute
                // façon) — juste libérer la mémoire, jamais free_file (pile).
                struct possibility_packet discard;
                while (scroll(&children, &discard)) { }
            }
            if (produced >= (unsigned long long)expand_max_stock) {
                cap_reached = 1; // le reste de `work` sera réinjecté tel quel
            }
        }
        if (aborted) {
            // Même raisonnement : draine `work` sans insérer, jamais free_file.
            struct possibility_packet discard;
            while (scroll(&work, &discard)) { }
        }
        // Sinon, `work` est entièrement vidée par la boucle scroll ci-dessus
        // (Elements libérés au fil de l'eau).

        if (produced >= (unsigned long long)expand_max_stock) {
            log_event("expansion : plafond de stock atteint (%llu ≥ %d) — arrêt de l'approfondissement",
                      produced, expand_max_stock);
        } else if (ram_wait_this_round) {
            log_event("expansion : plafond RAM atteint pendant cette passe — approfondissement suspendu "
                      "pour cette passe (%llu possibilité(s) produites, réinjectées telles quelles)", produced);
        }

        // Pause ENTRE deux passes (pas d'arrêt définitif) si cette passe a
        // été freinée par la RAM plutôt que par le garde-fou de volume, et
        // qu'une passe suivante sera effectivement tentée — inutile
        // d'attendre si c'était de toute façon la dernière autorisée
        // (`expand_max_levels`) ou si l'arrêt du process a déjà été demandé.
        if (ram_wait_this_round && !cap_reached && !aborted && rounds + 1 < expand_max_levels) {
            if (!expand_wait_for_ram_headroom_between_passes()) {
                aborted = 1;
            }
        }

        if (!expanded_any && !shallow_deferred_by_ram_wait) {
            break; // tout le stock a atteint le niveau cible : rien de plus à faire
        }
        rounds++;
    }

    if (aborted) {
        log_event("expansion interrompue par l'arrêt du serveur : %llu possibilité(s) en stock (%d passe(s))",
                  datas_size(), rounds);
    }
    log_event("expansion terminée : %llu possibilités en stock (%d passe(s), niveau visé %d)",
              datas_size(), rounds, target_level);
    log_info("expansion : %llu possibilités en stock après %d passe(s) (niveau visé %d)\n",
             datas_size(), rounds, target_level);
    return rounds;
}

// Test qu'une seule fois de placer. On peut donc trouver des possibilités avec suite mais en ayant placé les cases ayant
// qu'une seule possibilité, au tir suivant la possibilité peut avoir aucune suite car des pieces placées (1 seule poss) ont
// pu éléminer une case à plusieurs possiblités.
int remove_possibilities_with_no_next(map_big_array *mapParts, struct array_part *all_rotate_part)
{
    lock_all_file();
    int fp;
    // Statistique : chaque case examinée par l'élagage compte comme un coup
    // (même unité que la recherche), avec un minimum d'un coup par possibilité
    // (plateau déjà complet : le balayage ne fait rien). Cumulé localement puis
    // crédité en une fois après la boucle.
    unsigned long long total_cells = 0;
	// Seul le pool non vérifié est élagué : une possibilité vérifiée (checked == 1)
	// a déjà été confirmée vivante par un pruner, elle a donc forcément une suite.
	for (fp=0; fp < nb_file_possibility; fp++)
	{
		Element *currElement = file_possibility[fp]->file.start;

		while (currElement != NULL)
		{
            Element *nextElement = NULL;
			struct possibility_packet *possibility = (struct possibility_packet *)currElement->value;
            unsigned int cells_studied = 0;
            int has_next = possibility_all_has_a_next_counted(possibility, mapParts, all_rotate_part, &cells_studied);
            total_cells += (cells_studied > 0) ? cells_studied : 1;
            int is_solution = (possibility->alloc >= ETERN_PARTS);

            if (is_solution) {
                /* Solution complète détectée par rmnonext (packet déjà complet ou
                 * complété via placements forcés). On sauvegarde sans appeler exit()
                 * afin de ne pas tuer le processus serveur. */
                static unsigned rmnonext_sol_seq = 0;
                unsigned seq = __atomic_fetch_add(&rmnonext_sol_seq, 1, __ATOMIC_RELAXED);
                char fileName[64];
                snprintf(fileName, sizeof fileName, "./solution_server_%i_%u.csv",
                         (int)getpid(), seq);
                log_event("SOLUTION trouvée par rmnonext (%i pièces)", possibility->alloc);
                log_info("*** SOLUTION trouvée par rmnonext (%i pièces) ***\n", possibility->alloc);
                save_solution_csv(fileName, possibility, all_rotate_part);
                log_info("solution sauvegardée dans %s\n", fileName);
                if (stop_on_solution) {
                    unlock_all_file();
                    // Sans cliché de débordement ici (spill_snapshot_dir/fn
                    // à NULL) : ce module (core/) ne peut pas dépendre de
                    // core/stock_spill.h (règle de dépendance à sens unique).
                    // Lacune documentée — plus étroite que celle
                    // qu'elle remplace : seul CE chemin (solution trouvée par
                    // le passe de fond rmnonext, pas celui, plus courant, où
                    // un client la rapporte via etii_server.c) manque de
                    // couverture du débordement.
                    consistent_backup("./eternityII.back", "./eternityII-in_analyse.back", NULL, NULL, NULL);
                    log_event("serveur arrêté suite à la solution (stock sauvegardé)");
                    log_info("serveur arrêté suite à la solution — stock sauvegardé\n");
                    flush_info();
                    exit(EXIT_SUCCESS);
                }
                /* Le packet complet est retiré ci-dessous (déjà traité). */
            }

			if (is_solution || !has_next)
			{
                // On place le suivant du précédent au suivant du courrant
				if(currElement->previous != NULL)
                {
                    currElement->previous->next = currElement->next;
                } else {
                    // On est au début alors la pile commence au suivant
                    file_possibility[fp]->file.start = currElement->next;
                }

                // On a une suite alors le précédent du suivant devient le précédent du courrant
                if(currElement->next != NULL)
                {
                    currElement->next->previous = currElement->previous;
                } else {
                    // Pas de suite alors la fin de la pile  devient le précédent (ou null)
                    file_possibility[fp]->file.end = currElement->previous;
                }
                nextElement = currElement->next;
                free (currElement->value);
                free (currElement);
                currElement = NULL;
                file_possibility[fp]->file.size--;

			}
            
            // Aucune suite n'a été determiné et on a un courrant alors on prend sa suite.
            if(nextElement == NULL && currElement != NULL)
            {
                currElement = currElement->next;
            } else {
                // On a déterminé une suite alors on l'étudie à la prochaine boucle
                currElement = nextElement;
            }
		}
	}
    unlock_all_file();
    // Crédit des études dans le flux de prunage (« dont prunage/s » et
    // « études/s » des rapports check) — flux disjoint des compteurs de coups.
    pruner_cells_studied += total_cells;
    return 0;
}

/**
 * @brief Répartit équitablement les possibilités entre `nbsplit` files sans verrouillage.
 *
 * Regroupe d'abord tout dans la file 0, puis distribue par quotient égal.
 * Doit être appelée avec les files déjà verrouillées.
 *
 * @param nbsplit Nombre de files cibles (≤ nb_file_possibility).
 * @return        0.
 */
/**
 * @brief Répartit un pool sur `nbsplit` files, sans verrouillage.
 * @param pool    Tableau de files (pool non vérifié ou vérifié).
 * @param nbsplit Nombre de files cibles (≤ nb_file_possibility).
 * @return        0.
 */
static int split_pool_nolock(file_possibility_t **pool, int nbsplit)
{
	regroup_pool_nolock(pool);

	File *file = malloc(sizeof(File));
	init_file(file, sizeof(struct possibility_packet));
	struct possibility_packet *possibility = malloc(sizeof(struct possibility_packet));
	while (pool[0]->file.size > 0)
	{

		if(scroll(&pool[0]->file, possibility))
		{
			put(file, possibility);
		}
	}

	lldiv_t d = lldiv(file->size, nbsplit);
	long long quotient = d.quot;
	if(d.rem != 0)
	{
		quotient++;
	}

	int f;
	for (f=0; f < nbsplit; f++){
		while(pool[f]->file.size < (unsigned long long)quotient && file->size > 0){
			if(scroll(file, possibility))
			{
				put(&pool[f]->file, possibility);
			}
		}
	}

	// si le quotient n'était pas bon on vide dans la premiere liste pour éviter la perte
	while(file->size > 0){
		if(scroll(file, possibility))
		{
			put(&pool[0]->file, possibility);
		}
	}

	free(possibility);
	free_file(file);

	return 0;
}

int split_datas_nolock(int nbsplit)
{
	// Pool non vérifié uniquement (utilisé en interne par le tri multi-thread)
	return split_pool_nolock(file_possibility, nbsplit);
}

int split_datas(void)
{
	// Rééquilibrage incrémental (datamanager_rebalance_step) au lieu de regroup_pool_nolock
	// + 3 copies par paquet sous verrou global (split_pool_nolock ci-dessus)
	// — inexploitable à l'échelle de plusieurs millions de possibilités.
	// Budget INT_MAX : datamanager_rebalance_step boucle désormais en interne
	// jusqu'à épuisement du budget ou équilibre complet (rebalance_pool_until_budget),
	// donc un seul appel suffit à converger entièrement — split_datas() est un
	// appel EXPLICITE (commande console), pas un tick périodique, on veut
	// converger en un coup plutôt qu'étaler sur plusieurs tours comme le fait
	// l'appel périodique de check_server_step avec un budget modeste.
	datamanager_rebalance_step(INT_MAX);
	return 0;
}

int check_datas(void)
{
    struct array_part *apart= read_parts(parts_files);
    
    struct array_part *rotateParts = rotate_all_parts(apart);
	lock_all_file();
	int count = 0;
    int errors = 0;
	int fp;
	for (fp=0; fp < nb_file_possibility; fp++)
	{
		// Les deux pools sont vérifiés : non vérifié et vérifié (checked == 1)
		File *pools[2] = { &file_possibility[fp]->file, &file_possibility_checked[fp]->file };
		for (int p = 0; p < 2; p++)
		{
			Element *currElement = pools[p]->start;
			while (currElement != NULL)
			{
				count++;
				int analyse = check_possibility((struct possibility_packet *)currElement->value, rotateParts);
				if (analyse < 0)
				{
					log_error("possibility error : %i\n",analyse);
					log_error(" ---");
					log_error_possibility_packet((struct possibility_packet *)currElement->value);
					errors++;
				}
				currElement = currElement->next;
			}
		}
	}

	unlock_all_file();

	log_event("check_datas errors %i on %i\n", errors, count);
    return errors > 0 ? -1 : 0;
}

#define nbDuplicateThread 8
unsigned long long duplicateCount[nbDuplicateThread];
unsigned long long duplicateErrors[nbDuplicateThread];
unsigned long long duplicateFinish[nbDuplicateThread];
unsigned long long duplicateAnalyzed[nbDuplicateThread];

struct arg_duplicate_thread {
    Element *currElement;
    int filePossibility;
    unsigned long long position;
    unsigned long long nbCombinations;
    int threadPosition;
};

/**
 * @brief Thread de détection des doublons dans les files de possibilités.
 *
 * Chaque thread traite un sous-ensemble des paires (nbCombinations / nbDuplicateThread).
 * Utilise `compare_possibility` et `is_origin_of` pour détecter les doublons exacts
 * et les relations ancêtre-descendant entre possibilités.
 *
 * @param arguments Pointeur vers un `arg_duplicate_thread` décrivant la partition à analyser.
 * @return          NULL.
 */
void *check_duplicate_thread(void *arguments) {
    struct arg_duplicate_thread *args = (struct arg_duplicate_thread *)arguments;
    Element *currElement = args->currElement;
    int fp= args->filePossibility;
    unsigned long long position = args->position;
    int cfp;
    while (currElement != NULL && duplicateAnalyzed[args->threadPosition] <= args->nbCombinations)
    {
        duplicateCount[args->threadPosition]++;
        for (cfp=fp; cfp < nb_file_possibility; cfp++)
        {
            unsigned long long comparePosition = 0;
            Element *elementToCompare = NULL;
            if (fp == cfp) {
                elementToCompare = currElement->next;
                comparePosition = position + 1;
                //printf("%i position %llu start with next for %llu\n", args->threadPosition, position, duplicateCount[args->threadPosition]);
            } else {
                elementToCompare = file_possibility[cfp]->file.start;
                //printf("%i position %llu start with start %i for %llu\n", args->threadPosition, position, cfp, duplicateCount[args->threadPosition]);
            }
            while (elementToCompare != NULL)
            {
                if (currElement != elementToCompare) {
                    int analyse = compare_possibility((struct possibility_packet *)currElement->value, (struct possibility_packet *)elementToCompare->value);
                    if (analyse == 0)
                    {
                        log_info("possibility error : %i F%i:%llu to F%i:%llu\n",analyse, fp, position, cfp, comparePosition);
                        // print_possibility_packet((struct possibility_packet *)currElement->value);
                        duplicateErrors[args->threadPosition]++;
                    } else {
                        analyse = is_origin_of(currElement->value, elementToCompare->value);
                        if (analyse == 1) {
                            log_info("possibility origin error : F%i:%llu to F%i:%llu\n", fp, position, cfp, comparePosition);
                            duplicateErrors[args->threadPosition]++;
                        }
                    }
                } else {
                    log_info("possibility error : equals F%i:%llu to F%i:%llu\n", fp, position, cfp, comparePosition);
                    duplicateErrors[args->threadPosition]++;
                }
                elementToCompare = elementToCompare->next;
                comparePosition++;
                duplicateAnalyzed[args->threadPosition]++;
            }
            
        }
        if (currElement != NULL) {
            currElement = currElement->next;
            position++;
        }
        if (position >= file_possibility[fp]->file.size) {
            fp++;
            position = 0;
            currElement = NULL;
            if (fp < nb_file_possibility) {
                currElement = file_possibility[fp]->file.start;
            }
        }

    }
    
    duplicateFinish[args->threadPosition] = 1;
    return NULL;
}

/**
 * @brief Affiche dans les logs la configuration d'un thread de détection de doublons.
 * @param args Paramètres du thread à afficher.
 */
void print_duplicate_args(struct arg_duplicate_thread *args) {
    log_info("thread:%i fileP:%i position:%llu nbCombinations:%llu\n", args->threadPosition, args->filePossibility, args->position, args->nbCombinations);
}

/**
 * @brief Lance un thread de détection de doublons pour la partition spécifiée.
 *
 * @param currElement    Premier élément de la partition à analyser.
 * @param filePossibility Indice de la file de départ.
 * @param position       Position de départ dans la file.
 * @param nbCombinations Nombre de paires à analyser par ce thread.
 * @param threadPosition Indice du thread dans les tableaux de statistiques.
 */
void run_check_duplicate_thread(Element *currElement, int filePossibility, unsigned long long position, unsigned long long nbCombinations, int threadPosition) {
    struct arg_duplicate_thread *args = malloc(sizeof(struct arg_duplicate_thread));
    args->currElement = currElement;
    args->filePossibility = filePossibility;
    args->threadPosition = threadPosition;
    args->position = position;
    args->nbCombinations = nbCombinations;
    print_duplicate_args(args);
    pthread_attr_t *thread_attributes = malloc(sizeof *thread_attributes);
    pthread_attr_init(thread_attributes);
    pthread_attr_setdetachstate(thread_attributes, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    if(0 != pthread_create(&thread, thread_attributes, check_duplicate_thread, args))
        {
            log_error("Problème avec pthread_create()\n");
            duplicateFinish[args->threadPosition] = 1;
            free(thread_attributes);
            free(args);
            return;
        }
        pthread_attr_destroy(thread_attributes);
        free(thread_attributes);
}

/**
 * @brief Affiche la progression et les erreurs des threads de détection de doublons.
 *
 * @param dataSize       Nombre total de possibilités analysées.
 * @param nbCombinations Nombre total de paires à comparer.
 */
void print_duplicate_activity(unsigned long long dataSize, unsigned long long nbCombinations) {
    unsigned long long current = 0;
    unsigned long long errors = 0;
    unsigned long long analyzed = 0;
    int activeThreads = 0;
    for (int t = 0; t < nbDuplicateThread; t++) {
        current += duplicateCount[t];
        errors += duplicateErrors[t];
        analyzed += duplicateAnalyzed[t];
        if (duplicateFinish[t] == 0) {
            activeThreads++;
        }
    }
    
    double percentCombination = (analyzed/(nbCombinations*1.0))*100.0;
    log_info("analyzed: %llu/%llu %f/100 | %llu / %llu | errors: %llu | active threads: %i\n", analyzed, nbCombinations, percentCombination, current, dataSize, errors, activeThreads);
}

/**
 * @brief Calcule le nombre de combinaisons de paires C(x, 2) = x*(x-1)/2.
 *
 * Utilisé par `check_duplicate` pour estimer la charge de travail de comparaison.
 *
 * @param x Nombre d'éléments.
 * @return  Nombre de paires uniques.
 */
unsigned long long count_combinations(unsigned long long x) {
    unsigned long long i;
    unsigned long long z = x - 1;
    unsigned long long result = 0;
    unsigned long long lastResult = 0;
    for (i=1; i < x; i++) {
        lastResult = result;
        result += z;
        if (result < lastResult || result <= 0) {
            log_error("bug on count_combinations\n");
        }
        z--;
    }
    return result;
}

int check_duplicate(void)
{
    lock_all_file();
    unsigned long long count = 0;
    unsigned long long errors = 0;

    // NOTE : la détection de doublons ne balaie que le pool non vérifié.
    // Le walker threadé (check_duplicate_thread) référence directement
    // file_possibility[] pour partitionner et comparer ; étendre la recherche
    // au pool vérifié (checked == 1) imposerait de chaîner 2*nb_file_possibility
    // files dans une même séquence — hors périmètre ici. On dimensionne donc le
    // travail sur la taille du seul pool non vérifié (et non datas_size(), qui
    // inclut le pool vérifié) pour que le nombre de combinaisons corresponde
    // exactement aux éléments réellement parcourus.
    unsigned long long dataSize = 0;
    for (int f = 0; f < nb_file_possibility; f++) {
        dataSize += file_size(f);
    }
    unsigned long long nbCombinations = count_combinations(dataSize);
    unsigned long long nbByThread = nbCombinations / nbDuplicateThread;
    log_info("qt: %llu nb combinations %llu | nb/threads: %llu\n", dataSize, nbCombinations, nbByThread);
    
    int fp = 0;
    Element *currElement = file_possibility[fp]->file.start;
    unsigned long long position = 0;
    unsigned long long allocated = 0;
    unsigned long long remains = dataSize;
    // Nombre de threads réellement lancés : selon la taille du stock (et donc
    // nbCombinations) la boucle ci-dessous peut en lancer moins de
    // nbDuplicateThread, voire aucun (stock vide/à 1 élément -> nbCombinations==0).
    // La boucle de jointure ne doit attendre QUE ces threads-là : un index non
    // lancé garde un duplicateFinish résiduel qui ferait boucler l'attente sans
    // fin sur un thread jamais créé.
    int spawned = 0;
    for (int t = 0; t < nbDuplicateThread && fp < nb_file_possibility && allocated < nbCombinations; t++) {
        duplicateCount[t] = 0;
        duplicateErrors[t] = 0;
        duplicateFinish[t] = 0;
        duplicateAnalyzed[t] = 0;
        unsigned long long last = nbByThread;
        if (t == nbDuplicateThread - 1) {
            last = nbCombinations - allocated;
        }
        run_check_duplicate_thread(currElement, fp, position, last, t);
        spawned++;
        // Pour le dernier -> pas besoin
        if (t < nbDuplicateThread - 1) {
            unsigned long long allocatedToThread = remains - 1;
            while (allocatedToThread < nbByThread && currElement != NULL) {
                remains--;
                allocatedToThread += remains;
                position++;
                if (position > file_possibility[fp]->file.size) {
                    fp++;
                    position = 0;
                    if (fp >= nb_file_possibility) {
                        currElement = NULL;
                        break;
                    }
                    currElement = file_possibility[fp]->file.start;
                } else {
                    currElement = currElement->next;
                }
            }
            allocated += allocatedToThread;
        }
    }
        
    // On ne joint que les threads effectivement lancés (indices 0..spawned-1).
    for (int t = 0; t < spawned; t++) {
        int loop = 0;
        while (duplicateFinish[t] == 0) {
            sleep(1);
            loop++;
            if (loop == 30) {
                loop = 0;
                print_duplicate_activity(dataSize, nbCombinations);
            }
        }
        count += duplicateCount[t];
        errors += duplicateErrors[t];
    }
    
    unlock_all_file();
    
    log_event("check_duplicate errors %llu on %llu\n", errors, count);
    return errors > 0 ? -1 : 0;
}

/* Nombre de threads du balayage check_origin (même ordre de grandeur que
 * nbDuplicateThread : le travail est purement CPU et sans contention). */
#define NB_ORIGIN_THREAD 8
/* Plafond de lignes de détail loguées : au-delà, seul le total est rapporté.
 * Un stock incohérent peut produire des millions de relations ; noyer la
 * console (et, en ncurses, le pad de sortie) ne rendrait service à personne. */
#define ORIGIN_REPORT_MAX_LINES 100

/**
 * @brief Une possibilité du stock, aplatie pour le balayage de `check_origin`.
 *
 * `file`/`element` sont conservés pour la purge (`file_remove_element`),
 * `pool`/`file_index`/`position` uniquement pour le rapport. `alloc` est
 * recopié ici pour que le tri et le filtre de profondeur ne déréférencent pas
 * le paquet (une indirection par comparaison sur des millions d'entrées).
 */
typedef struct
{
	Element *element;
	File *file;
	unsigned long long position;
	uint16_t alloc;
	uint16_t file_index;
	uint8_t pool;          /* 0 = pool non vérifié, 1 = pool vérifié */
	uint8_t is_descendant; /* posé par le balayage : a une racine en stock */
} origin_entry_t;

struct arg_origin_thread {
	origin_entry_t *entries;
	unsigned long long count;
	unsigned long long first;   /* premier indice traité */
	unsigned long long stride;  /* pas entre deux indices traités */
	int thread_position;
};

/* Partagés par les threads de check_origin : compteur de descendants trouvés,
 * budget de lignes de rapport, progression. Tous manipulés en atomique. */
static unsigned long long origin_found;
static unsigned long long origin_reported;
static unsigned long long origin_progress[NB_ORIGIN_THREAD];
static unsigned long long origin_finish[NB_ORIGIN_THREAD];

/**
 * @brief Compare deux entrées par `alloc` croissant (comparateur `qsort`).
 *
 * Le tri est ce qui remplace tout pré-filtre : une racine a strictement moins
 * de pièces posées que son descendant, donc après tri elle le PRÉCÈDE — il
 * suffit de comparer chaque entrée à celles qui la suivent, une seule fois et
 * dans le bon sens.
 */
static int origin_entry_cmp(const void *a, const void *b)
{
	uint16_t alloc_a = ((const origin_entry_t *)a)->alloc;
	uint16_t alloc_b = ((const origin_entry_t *)b)->alloc;
	if (alloc_a < alloc_b) { return -1; }
	if (alloc_a > alloc_b) { return 1; }
	return 0;
}

/**
 * @brief Premier indice de `entries` (trié par `alloc` croissant) dont l'`alloc`
 *        dépasse strictement `alloc`.
 *
 * Sans cette dichotomie, la boucle interne balaierait tout le préfixe des
 * entrées de profondeur inférieure ou ÉGALE pour n'y rien faire. Sur un stock
 * réel, où l'immense majorité des plateaux partage le même `alloc`, ce préfixe
 * est presque tout le tableau : le coût deviendrait quadratique même quand il
 * n'y a strictement aucune paire à comparer.
 *
 * @param entries Tableau trié par `alloc` croissant.
 * @param count   Nombre d'entrées.
 * @param alloc   Profondeur de référence.
 * @return        Indice de la première entrée strictement plus profonde (`count` si aucune).
 */
static unsigned long long origin_upper_bound(const origin_entry_t *entries, unsigned long long count, uint16_t alloc)
{
	unsigned long long low = 0;
	unsigned long long high = count;
	while (low < high) {
		unsigned long long mid = low + (high - low) / 2;
		if (entries[mid].alloc > alloc) {
			high = mid;
		} else {
			low = mid + 1;
		}
	}
	return low;
}

/**
 * @brief Thread de balayage de `check_origin`.
 *
 * Traite les indices `first`, `first + stride`, … (entrelacement plutôt que
 * blocs contigus : le travail d'une entrée décroît avec son indice, un
 * découpage par blocs donnerait au dernier thread une part dérisoire).
 *
 * Une entrée déjà marquée descendante est sautée : elle sera supprimée de
 * toute façon, lui trouver une seconde racine n'apprendrait rien. La course
 * sur `is_descendant` est bénigne (seule la valeur 1 est écrite) et le
 * comptage reste exact grâce à l'échange atomique, qui n'attribue la
 * découverte qu'à un seul thread.
 *
 * @param arguments `struct arg_origin_thread` décrivant la part à traiter.
 * @return          NULL.
 */
void *check_origin_thread(void *arguments)
{
	struct arg_origin_thread *args = (struct arg_origin_thread *)arguments;
	origin_entry_t *entries = args->entries;

	for (unsigned long long i = args->first; i < args->count; i += args->stride) {
		const struct possibility_packet *root =
			(const struct possibility_packet *)entries[i].element->value;
		/* Tri croissant : à alloc égal aucune des deux n'est la racine de
		 * l'autre. On saute donc directement à la première entrée
		 * strictement plus profonde — elle vient forcément après i. */
		unsigned long long start = origin_upper_bound(entries, args->count, entries[i].alloc);
		for (unsigned long long j = start; j < args->count; j++) {
			if (__atomic_load_n(&entries[j].is_descendant, __ATOMIC_RELAXED)) {
				continue;
			}
			if (is_origin_of((struct possibility_packet *)root,
			                 (struct possibility_packet *)entries[j].element->value) != 1) {
				continue;
			}
			if (__atomic_exchange_n(&entries[j].is_descendant, (uint8_t)1, __ATOMIC_RELAXED)) {
				continue; /* un autre thread l'a marquée en même temps */
			}
			__atomic_fetch_add(&origin_found, 1ULL, __ATOMIC_RELAXED);
			if (__atomic_fetch_add(&origin_reported, 1ULL, __ATOMIC_RELAXED)
			    < ORIGIN_REPORT_MAX_LINES) {
				log_info("possibility origin : %s F%u:%llu (alloc %u) racine de %s F%u:%llu (alloc %u)\n",
				         entries[i].pool ? "checked" : "unchecked",
				         (unsigned)entries[i].file_index, entries[i].position,
				         (unsigned)entries[i].alloc,
				         entries[j].pool ? "checked" : "unchecked",
				         (unsigned)entries[j].file_index, entries[j].position,
				         (unsigned)entries[j].alloc);
			}
		}
		__atomic_fetch_add(&origin_progress[args->thread_position], 1ULL, __ATOMIC_RELAXED);
	}

	__atomic_store_n(&origin_finish[args->thread_position], 1ULL, __ATOMIC_RELAXED);
	return NULL;
}

/**
 * @brief Aplatit les deux pools de stock dans un tableau d'entrées.
 *
 * Verrou déjà tenu par l'appelant (`lock_all_file`), donc les `Element *`
 * collectés restent valides pendant tout le balayage ET la purge.
 *
 * @param entries Tableau à remplir, dimensionné pour `datas_size()` entrées.
 * @param capacity Capacité de `entries`.
 * @return         Nombre d'entrées réellement écrites.
 */
static unsigned long long collect_origin_entries(origin_entry_t *entries, unsigned long long capacity)
{
	unsigned long long n = 0;
	for (int fp = 0; fp < nb_file_possibility; fp++) {
		File *pools[2] = { &file_possibility[fp]->file, &file_possibility_checked[fp]->file };
		for (int p = 0; p < 2; p++) {
			unsigned long long position = 0;
			for (Element *e = pools[p]->start; e != NULL; e = e->next) {
				if (n >= capacity) {
					return n;
				}
				entries[n].element = e;
				entries[n].file = pools[p];
				entries[n].position = position;
				entries[n].alloc = ((struct possibility_packet *)e->value)->alloc;
				entries[n].file_index = (uint16_t)fp;
				entries[n].pool = (uint8_t)p;
				entries[n].is_descendant = 0;
				n++;
				position++;
			}
		}
	}
	return n;
}

int check_origin(int purge)
{
	lock_all_file();

	unsigned long long capacity = datas_size();
	origin_entry_t *entries = NULL;
	if (capacity > 0) {
		entries = malloc((size_t)capacity * sizeof(origin_entry_t));
		if (entries == NULL) {
			unlock_all_file();
			// Un contrôle qui n'a pas pu tourner ne dit pas « tout va bien ».
			log_error("check_origin : allocation de %llu entrees impossible\n", capacity);
			return -1;
		}
	}
	unsigned long long count = collect_origin_entries(entries, capacity);
	if (count > 1) {
		// Jamais qsort(NULL, 0, ...) : passer un pointeur nul est indéfini
		// même avec un compte de 0 (et le stock vide est un cas courant).
		qsort(entries, (size_t)count, sizeof(origin_entry_t), origin_entry_cmp);
	}

	origin_found = 0;
	origin_reported = 0;
	log_info("check_origin : %llu possibilites, %llu paires a comparer\n",
	         count, count > 1 ? count_combinations(count) : 0ULL);

	pthread_t threads[NB_ORIGIN_THREAD];
	struct arg_origin_thread args[NB_ORIGIN_THREAD];
	// Deux compteurs distincts, et non un seul : `configured` indexe
	// origin_progress/origin_finish (une part préparée, qu'elle ait été
	// confiée à un thread ou traitée sur place), `joinable` compte les
	// pthread_t RÉELLEMENT créés. Les confondre décalerait threads[] dès le
	// premier pthread_create en échec et ferait joindre un pthread_t jamais
	// initialisé.
	int configured = 0;
	int joinable = 0;
	for (int t = 0; t < NB_ORIGIN_THREAD && (unsigned long long)t < count; t++) {
		origin_progress[t] = 0;
		origin_finish[t] = 0;
		args[t].entries = entries;
		args[t].count = count;
		args[t].first = (unsigned long long)t;
		args[t].stride = NB_ORIGIN_THREAD;
		args[t].thread_position = t;
		configured++;
		pthread_t thread;
		if (pthread_create(&thread, NULL, check_origin_thread, &args[t]) != 0) {
			// Repli sur place : le balayage doit aboutir, pas être abandonné.
			log_error("check_origin : pthread_create a echoue, part %i traitee ici\n", t);
			check_origin_thread(&args[t]);
			continue;
		}
		threads[joinable++] = thread;
	}

	/* Progression toutes les 30s tant que des threads tournent, puis jointure
	 * (immédiate : ils ont déjà fini quand la boucle sort).
	 *
	 * Sondage à 50 ms et non à la seconde : sur un stock minuscule le balayage
	 * dure moins d'une milliseconde, et un sleep(1) ferait payer une seconde
	 * pleine à chaque appel (7 s pour la seule suite de tests). */
	int loop = 0;
	int running = joinable;
	while (running > 0) {
		usleep(50000);
		loop++;
		running = 0;
		unsigned long long done = 0;
		for (int t = 0; t < configured; t++) {
			if (__atomic_load_n(&origin_finish[t], __ATOMIC_RELAXED) == 0) { running++; }
			done += __atomic_load_n(&origin_progress[t], __ATOMIC_RELAXED);
		}
		if (loop == 600 && running > 0) { /* 600 * 50 ms = 30 s */
			loop = 0;
			log_info("check_origin : %llu/%llu possibilites balayees | racines trouvees: %llu | threads actifs: %i\n",
			         done, count, __atomic_load_n(&origin_found, __ATOMIC_RELAXED), running);
		}
	}
	for (int t = 0; t < joinable; t++) {
		pthread_join(threads[t], NULL);
	}

	unsigned long long found = origin_found;
	unsigned long long removed = 0;
	if (purge) {
		for (unsigned long long i = 0; i < count; i++) {
			if (entries[i].is_descendant) {
				file_remove_element(entries[i].file, entries[i].element);
				removed++;
			}
		}
	}

	free(entries);
	unlock_all_file();

	if (found > ORIGIN_REPORT_MAX_LINES) {
		log_info("check_origin : %llu relations non detaillees (plafond de %i lignes)\n",
		         found - ORIGIN_REPORT_MAX_LINES, ORIGIN_REPORT_MAX_LINES);
	}
	if (purge) {
		log_event("check_origin : %llu possibilites supprimees sur %llu -- lancer « backup » pour graver l'etat purge\n",
		         removed, count);
	}
	log_event("check_origin errors %llu on %llu\n", found, count);
	return found > 0 ? -1 : 0;
}

/**
 * @brief Renvoie l'INTÉGRALITÉ du pool vérifié dans le pool non vérifié
 *        (`checked` remis à 0), pour forcer tout le passif à repasser
 *        devant les pruners.
 *
 * `autoprune_step` (src/core/etii_search.c) ne retente JAMAIS la preuve de
 * fermeture bornée (search_packet_backtracking_budgeted) sur une
 * possibilité déjà `checked == 1` -- même si `prunerDfsBudget` est augmenté
 * ou la logique de prunage améliorée après coup, le passif déjà vérifié
 * n'en bénéficie jamais sans cette commande. Maintenance ponctuelle,
 * déclenchée à la main (console `resetChecked`), pas un pas incrémental
 * façon `rebalance` : coût O(n) trivial par entrée, contrairement aux
 * O(n²)/O(n log n) de `checkOrigin`/`checkDuplicate`.
 *
 * Déplace chaque entrée EN PLACE, du fichier `fp` du pool vérifié vers le
 * MÊME index `fp` du pool non vérifié (jamais round-robinée ailleurs) --
 * simple concaténation de listes chaînées, sans copie (contrairement à
 * `put_back_to_stock`, qui prend en plus ses propres verrous et
 * s'auto-bloquerait ici : l'appelant tient déjà tous les verrous via
 * `lock_all_file`). Le pool « en cours d'analyse » (batches en vol chez les
 * pruners) est hors périmètre, comme pour `checkOrigin`.
 *
 * @return Nombre de possibilités déplacées (cette commande ne peut pas
 *         « échouer » au sens diagnostic de `checkOrigin`/`checkDuplicate`).
 */
unsigned long long reset_checked_pool(void)
{
	lock_all_file();

	unsigned long long moved = 0;
	for (int fp = 0; fp < nb_file_possibility; fp++) {
		File *checked_file = &file_possibility_checked[fp]->file;
		File *unchecked_file = &file_possibility[fp]->file;

		if (checked_file->size == 0) {
			continue;
		}

		for (Element *e = checked_file->start; e != NULL; e = e->next) {
			if (e->value != NULL) {
				((struct possibility_packet *)e->value)->checked = 0;
			}
		}

		if (unchecked_file->end != NULL) {
			unchecked_file->end->next = checked_file->start;
			checked_file->start->previous = unchecked_file->end;
			unchecked_file->end = checked_file->end;
		} else {
			unchecked_file->start = checked_file->start;
			unchecked_file->end = checked_file->end;
		}
		unchecked_file->size += checked_file->size;
		moved += checked_file->size;

		checked_file->start = NULL;
		checked_file->end = NULL;
		checked_file->size = 0;
	}

	unlock_all_file();

	log_event("resetChecked : %llu possibilite(s) repassees du pool verifie vers le pool non verifie\n",
	          moved);
	return moved;
}

/**
 * @brief Cumule dans `levels` la répartition par `alloc` des paquets de `file`,
 *        et dans `min_candidats_sum`/`min_candidats_known` la seconde
 *        coordonnée (score MRV de la dernière pièce posée).
 *
 * Un `alloc` hors bornes (paquet corrompu) est ignoré plutôt qu'écrit hors du
 * tableau — ce parcours est de l'observation, il ne doit jamais pouvoir
 * déborder sur une donnée douteuse. `min_candidats == POSSIBILITY_MIN_CANDIDATS_UNKNOWN`
 * (score non mesuré : paquet non issu du moteur MRV, ou stock antérieur à son
 * introduction) est exclu de la somme/du compte plutôt que traité comme 0 —
 * une moyenne sur un score absent serait une donnée inventée.
 *
 * @param file               File à parcourir (verrou déjà tenu par l'appelant).
 * @param levels             Histogramme de `STOCK_DISTRIBUTION_LEVELS` entrées à incrémenter.
 * @param min_candidats_sum  Somme des `min_candidats` connus, par niveau, à incrémenter.
 * @param min_candidats_known Nombre de `min_candidats` connus, par niveau, à incrémenter.
 * @return       Nombre de paquets comptés (y compris ceux d'`alloc` hors bornes).
 */
static unsigned long long accumulate_alloc_levels(File *file, unsigned long long *levels,
                                                    unsigned long long *min_candidats_sum,
                                                    unsigned long long *min_candidats_known)
{
    unsigned long long count = 0;
    Element *currElement = file->start;
    while (currElement != NULL)
    {
        struct possibility_packet *possibility = (struct possibility_packet *)currElement->value;
        if (possibility != NULL)
        {
            count++;
            if (possibility->alloc < STOCK_DISTRIBUTION_LEVELS)
            {
                levels[possibility->alloc]++;
                if (possibility->min_candidats != POSSIBILITY_MIN_CANDIDATS_UNKNOWN) {
                    min_candidats_sum[possibility->alloc] += (unsigned long long)possibility->min_candidats;
                    min_candidats_known[possibility->alloc]++;
                }
            }
        }
        currElement = currElement->next;
    }
    return count;
}

void datamanager_stock_distribution(stock_distribution_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));

    // Passe 1 : les deux pools de stock (non vérifié et vérifié).
    lock_all_file();
    for (int fp = 0; fp < nb_file_possibility; fp++)
    {
        out->total_unchecked += accumulate_alloc_levels(&file_possibility[fp]->file, out->unchecked,
                                                          out->unchecked_min_candidats_sum,
                                                          out->unchecked_min_candidats_known);
        out->total_checked += accumulate_alloc_levels(&file_possibility_checked[fp]->file, out->checked,
                                                        out->checked_min_candidats_sum,
                                                        out->checked_min_candidats_known);
    }
    unlock_all_file();

    // Passe 2 : le pool « en cours d'analyse », sous sa propre famille de
    // verrous — jamais tenue en même temps que la précédente.
    lock_all_file_analysed();
    for (int fp = 0; fp < nb_file_possibility; fp++)
    {
        out->total_analysed += accumulate_alloc_levels(&file_possibility_analysed[fp]->file, out->analysed,
                                                         out->analysed_min_candidats_sum,
                                                         out->analysed_min_candidats_known);
    }
    unlock_all_file_analysed();
}

void datamanager_stock_rate_stats(stock_rate_stats_t *out)
{
    if (out == NULL)
    {
        return;
    }
    time_t now = time(NULL);
    stock_rate_windows(&stock_adds_rate, now,
                        &out->adds_last_1m, &out->adds_last_1h, &out->adds_last_1d);
    stock_rate_windows(&stock_removes_rate, now,
                        &out->removes_last_1m, &out->removes_last_1h, &out->removes_last_1d);
    stock_rate_windows(&stock_adds_unchecked_rate, now,
                        &out->adds_unchecked_last_1m, &out->adds_unchecked_last_1h, &out->adds_unchecked_last_1d);
    stock_rate_windows(&stock_adds_checked_rate, now,
                        &out->adds_checked_last_1m, &out->adds_checked_last_1h, &out->adds_checked_last_1d);
    stock_rate_windows(&stock_removes_unchecked_rate, now,
                        &out->removes_unchecked_last_1m, &out->removes_unchecked_last_1h, &out->removes_unchecked_last_1d);
    stock_rate_windows(&stock_removes_checked_rate, now,
                        &out->removes_checked_last_1m, &out->removes_checked_last_1h, &out->removes_checked_last_1d);
}

int statistic_datas(void)
{
    stock_distribution_t distribution;
    datamanager_stock_distribution(&distribution);

    // Historiquement, cette commande ne rapporte que les deux pools de stock
    // (le pool analysé, distribué aux clients, n'en fait pas partie).
    log_info("check_datas analyses:%llu\n", distribution.total_unchecked + distribution.total_checked);
    for (int i = 0; i < STOCK_DISTRIBUTION_LEVELS; i++) {
        unsigned long long count = distribution.unchecked[i] + distribution.checked[i];
        unsigned long long known = distribution.unchecked_min_candidats_known[i] + distribution.checked_min_candidats_known[i];
        if (known > 0) {
            double avg = (double)(distribution.unchecked_min_candidats_sum[i] + distribution.checked_min_candidats_sum[i]) / (double)known;
            // Seconde coordonnée : difficulté moyenne (score MRV) des paquets
            // de ce niveau — deux niveaux à autant de pièces posées peuvent
            // avoir une difficulté très différente (cf. docs/autosearch_step.md).
            log_info("%i : %llu (min_candidats moyen : %.2f sur %llu mesurés)\n", i, count, avg, known);
        } else {
            log_info("%i : %llu\n", i, count);
        }
    }

    stock_rate_stats_t rate;
    datamanager_stock_rate_stats(&rate);
    log_info("stock ADD (1min/1h/1j) : %llu / %llu / %llu\n",
             rate.adds_last_1m, rate.adds_last_1h, rate.adds_last_1d);
    log_info("stock GET (1min/1h/1j) : %llu / %llu / %llu\n",
             rate.removes_last_1m, rate.removes_last_1h, rate.removes_last_1d);
    // Ventilation par pool : distingue ce qui alimente/consomme le pool
    // vérifié (chercheurs) du pool non vérifié (pruners) — l'agrégat ci-dessus
    // ne le permettait pas.
    log_info("  dont pool verifie     ADD %llu/%llu/%llu  GET %llu/%llu/%llu\n",
             rate.adds_checked_last_1m, rate.adds_checked_last_1h, rate.adds_checked_last_1d,
             rate.removes_checked_last_1m, rate.removes_checked_last_1h, rate.removes_checked_last_1d);
    log_info("  dont pool non verifie ADD %llu/%llu/%llu  GET %llu/%llu/%llu\n",
             rate.adds_unchecked_last_1m, rate.adds_unchecked_last_1h, rate.adds_unchecked_last_1d,
             rate.removes_unchecked_last_1m, rate.removes_unchecked_last_1h, rate.removes_unchecked_last_1d);
    return 0;
}

/**
 * @brief Met à jour `current` avec le plus petit `alloc` trouvé dans `file`.
 * @param file    File à parcourir.
 * @param current Minimum courant.
 * @return        Le minimum entre `current` et tous les `alloc` de `file`.
 */
static int min_alloc_in_file(File *file, int current)
{
	Element *currElement = file->start;
	while (currElement != NULL)
	{
		struct possibility_packet *possibility = (struct possibility_packet *)currElement->value;
		if(possibility != NULL && possibility->alloc < current)
		{
			current = possibility->alloc;
		}
		currElement = currElement->next;
	}
	return current;
}

int search_min_datas(void)
{
	int result = ETERN_PARTS + 1;
	lock_all_file();
	int fp;
	for (fp=0; fp < nb_file_possibility; fp++)
	{
		// Les deux pools comptent : non vérifié et vérifié (checked == 1)
		result = min_alloc_in_file(&file_possibility[fp]->file, result);
		result = min_alloc_in_file(&file_possibility_checked[fp]->file, result);
	}

	unlock_all_file();
    
    // Il n'y a donc aucun élément
    if (result > ETERN_PARTS) {
        result = 0;
    }
	return result;
}

// TODO : revoir le trie pour prendre en compte le cache
/**
 * @brief Trie une `File` en place, par nombre de pièces placées croissant.
 *
 * Doit être appelée avec les files déjà verrouillées.
 *
 * @param file File à trier (déjà regroupée).
 */
static void sort_one_file_ascending(File *file)
{
	if (file->start == NULL) {
		return;
	}

	// Tableau d'éléments permettants d'avoir des repaires d'éléments classés
	// On mémorise par nb possibilités allouées.
	Element **orderedLair = malloc(sizeof(Element*) * (ETERN_PARTS +1));
	for (int l = 0; l < ETERN_PARTS +1; l++) {
		orderedLair[l] = NULL;
	}

	unsigned long long position = 0;
	int percent = 0;
	unsigned long long fivePercent = file->size * 0.05;
	unsigned long long nextShow = fivePercent;

	log_console("0");
    flush_console();

	Element *currElement = file->start;
	while (currElement != NULL)
	{
		position++;
		if (position >= nextShow) {
			nextShow += fivePercent;
			percent += 5;
            log_console("--%i", percent);
            flush_console();
		}

		Element *nextElement = currElement->next;
		if (currElement->value != NULL) {
			struct possibility_packet *curr = currElement->value;
			int currAlloc = curr->alloc;
			if (orderedLair[currAlloc] == NULL) {
				orderedLair[currAlloc] = currElement;
			}

			if(nextElement != NULL && nextElement->value != NULL)
			{
				struct possibility_packet *next = nextElement->value;
				int nextAlloc = next->alloc;
				if (orderedLair[nextAlloc] == NULL) {
					orderedLair[nextAlloc] = nextElement;
				}

				// Si l'élément n'est pas trié, on le place par rapport aux repaires
				if(curr->alloc > next->alloc)
				{
					// On essaye de voir si on peut le placer avant un "suivant"
					Element *target = NULL;
					for (int b = currAlloc +1; b < ETERN_PARTS+1 && target == NULL; b++) {
						target = orderedLair[b];
					}
					if (target != NULL) {
						move_before(file, currElement, target);
					} else {
						// Pas de suivant, on place donc à la fin de la suite
						move_after(file, currElement, file->end);
					}

					if(file->start != nextElement)
					{
						nextElement = nextElement->previous;
						position -= 2;
					} else {
						position = 0;
					}

				}
			}
		}

		currElement = nextElement;
	}
	log_console("--100\n");
	free(orderedLair);
}

int sort_ascending(void)
{
	// on bloque les files le temps du trie
	lock_all_file();
	// regroupement pour ne parcourir qu'une seule file, pour chaque pool
	regroup_pool_nolock(file_possibility);
	sort_one_file_ascending(&file_possibility[0]->file);
	regroup_pool_nolock(file_possibility_checked);
	sort_one_file_ascending(&file_possibility_checked[0]->file);
	unlock_all_file();
	return 0;
}

/**
 * @brief Trie chaque file par ordre croissant, individuellement, sans regroupement.
 *
 * Contrairement à `sort_ascending()` (qui fusionne toutes les files de chaque
 * pool en file 0 avant de trier), cette variante préserve la répartition
 * round-robin existante entre les `nb_file_possibility` files et trie chacune
 * en place. `scroll_from_pool` consomme depuis la fin (`scroll()` est LIFO) :
 * un tri croissant place donc les possibilités les plus avancées (`alloc` le
 * plus grand) en fin de chaque file, pour qu'elles soient consommées en
 * priorité — sur toutes les files, pas seulement la file 0.
 *
 * @return 0.
 */
int sort_ascending_files(void)
{
	// on bloque les files le temps du trie
	lock_all_file();
	int fp;
	for (fp = 0; fp < nb_file_possibility; fp++)
	{
		sort_one_file_ascending(&file_possibility[fp]->file);
		sort_one_file_ascending(&file_possibility_checked[fp]->file);
	}
	unlock_all_file();
	return 0;
}

/**
 * @brief Trie une `File` en place, par nombre de pièces placées décroissant.
 *
 * Doit être appelée avec les files déjà verrouillées.
 *
 * @param file File à trier (déjà regroupée).
 */
static void sort_one_file_descending(File *file)
{
	if (file->start == NULL) {
		return;
	}

	// Tableau d'éléments permettants d'avoir des repaires d'éléments classés
	// On mémorise par nb possibilités allouées.
	Element **orderedLair = malloc(sizeof(Element*) * (ETERN_PARTS +1));
	for (int l = 0; l < ETERN_PARTS +1; l++) {
		orderedLair[l] = NULL;
	}

	unsigned long long position = 0;
	int percent = 0;
	unsigned long long fivePercent = file->size * 0.05;
	unsigned long long nextShow = fivePercent;

	log_console("0");
    flush_console();

	Element *currElement = file->start;
	while (currElement != NULL)
	{
		position++;
		if (position >= nextShow) {
			nextShow += fivePercent;
			percent += 5;
			log_console("--%i", percent);
            flush_console();
		}

		Element *nextElement = currElement->next;
		if (currElement->value != NULL) {
			struct possibility_packet *curr = currElement->value;
			int currAlloc = curr->alloc;
			if (orderedLair[currAlloc] == NULL) {
				orderedLair[currAlloc] = currElement;
			}

			if(nextElement != NULL && nextElement->value != NULL)
			{
				struct possibility_packet *next = nextElement->value;
				int nextAlloc = next->alloc;
				if (orderedLair[nextAlloc] == NULL) {
					orderedLair[nextAlloc] = nextElement;
				}

				// Si l'élément n'est pas trié, on le place par rapport aux repaires
				if(curr->alloc < next->alloc)
				{
					// On essaye de voir si on peut le placer avant un "précédent" repaire
					Element *target = NULL;
					for (int b = currAlloc -1; b > 0 && target == NULL; b--) {
						target = orderedLair[b];
					}
					if (target != NULL) {
						move_before(file, currElement, target);
					} else {
						// Pas de précédent, on place donc à la fin de la suite car est le plus petit
						move_after(file, currElement, file->end);
					}

					if(file->start != nextElement)
					{
						nextElement = nextElement->previous;
						position -= 2;
					} else {
						position = 0;
					}

				}
			}
		}

		currElement = nextElement;
	}
	log_console("--100\n");
	free(orderedLair);
}

void *sort_d_mono(void *f)
{
    int intf = *(int *)f;
	log_info("sort d file:%i\n",intf);
	sort_one_file_descending(&file_possibility[intf]->file);
	log_info("end sort d file:%i\n",intf);
	return NULL;
}

/**
 * @brief Vérifie la cohérence structurelle d'une `File` (taille, chaînage, fin).
 * @param file  File à contrôler.
 * @param f     Indice de la file (pour les messages).
 * @param label Nom du pool (« unchecked » / « checked ») pour les messages.
 * @return      0 si cohérent, -1 sinon.
 */
int check_one_file(File *file, int f, const char *label)
{
	int result = 0;

	if(file->size == 0)
	{
		if(file->start != NULL)
		{
			log_info("File:%i (%s) size=0 and start not null\n",f,label);
			result = -1;
		}

		if(file->end != NULL)
		{
			log_info("File:%i (%s) size=0 and end not null\n",f,label);
			result = -1;
		}
	}

	// test que la fin correspond à la taille
	unsigned long long t;
	Element *currElement = file->start;
	Element *lastElement = currElement;
	for(t=0; t < file->size && currElement != NULL;t++)
	{
		if(currElement->value == NULL){
			log_info("File:%i (%s) value NULL\n",f,label);
			result = -1;
		}
		lastElement = currElement;
		currElement = currElement->next;
	}

	if(currElement != NULL)
	{
		log_info("File:%i (%s) last analysed element is not null | file.size:%llu analysed:%llu",f,label,file->size, t);
		result = -1;
	}
	if (t != file->size || lastElement != file->end) {
		log_info("File:%i (%s) end not correspond to the size:%llu analysed:%llu\n",f,label,file->size,t);
		result=-1;
	}
	return result;
}

int check_file(int f)
{
	// Les deux pools sont contrôlés : non vérifié et vérifié (checked == 1)
	int result = check_one_file(&file_possibility[f]->file, f, "unchecked");
	if(check_one_file(&file_possibility_checked[f]->file, f, "checked") != 0)
	{
		result = -1;
	}
	return result;
}

int check_files(void)
{
	int f;
	for(f = 0; f < nb_file_possibility;f++)
	{
		if(check_file(f))
		{
			log_event("checkFiles : incoherence detectee sur le fichier %d\n", f);
			return 1;
		}
	}
	log_event("checkFiles : %d fichier(s) verifie(s), aucune incoherence\n", nb_file_possibility);
	return 0;
}

/**
 * @brief Regroupe et trie le pool non vérifié en ordre décroissant, sans verrouillage.
 *
 * Doit être appelée avec les files déjà verrouillées. Ne traite que le pool non
 * vérifié (utilisée en interne par le tri multi-thread).
 *
 * @return 0.
 */
int sort_descending_nolock(void)
{
    log_info("regroup datas \n");
	regroup_pool_nolock(file_possibility);
    log_info("sort file 0\n");
	sort_one_file_descending(&file_possibility[0]->file);
	return 0;
}

/**
 * @brief Trie toutes les files en parallèle (un thread par file).
 *
 * Lance `nb_file_possibility` threads de tri descendant en parallèle
 * et attend leur fin avec `pthread_join`.
 */
void sortdmthread(void)
{
	pthread_t *tid = malloc( nb_file_possibility * sizeof(pthread_t) );
	int *f = malloc(nb_file_possibility * sizeof(int));
	int i;
	for( i=0; i<nb_file_possibility; i++ )
	{
		f[i] = i;
		pthread_create( &tid[i], NULL, sort_d_mono, &f[i] );
	}
	
	
	// Attente que les threads on terminés
	for( i=0; i<nb_file_possibility; i++ )
	{
		pthread_join( tid[i], NULL );
	}

	free(tid);
	free(f);
}

int sort_descending_mthread(void)
{
	lock_all_file();
	
	int nbfile=nb_file_possibility;
	int n;
	for(n = 1; n < nbfile; n++)
	{
		div_t d = div(nbfile,n);
		nbfile = d.quot;
		if(d.rem != 0)
		{
			nbfile++;
		}
        log_info("split to:%i\n",nbfile);
		split_datas_nolock(nbfile);
		sortdmthread();
	}
	
    log_info("sort d one thread\n");
	sort_descending_nolock();

	// Pool vérifié (généralement petit) : un seul passage de tri
	log_info("sort d checked pool\n");
	regroup_pool_nolock(file_possibility_checked);
	sort_one_file_descending(&file_possibility_checked[0]->file);

	unlock_all_file();
	return 0;
}

// TODO : revoir le trie pour prendre en compte le cache
int sort_descending(void)
{
	lock_all_file();

	// Les deux pools sont triés indépendamment : non vérifié et vérifié
	sort_descending_nolock();
	regroup_pool_nolock(file_possibility_checked);
	sort_one_file_descending(&file_possibility_checked[0]->file);

	unlock_all_file();
	return 0;
}

/**
 * @brief Trie chaque file par ordre décroissant, individuellement, sans regroupement.
 *
 * Symétrique de `sort_ascending_files()` pour l'ordre décroissant : contrairement
 * à `sort_descending()`/`sort_descending_mthread()` (qui fusionnent toutes les
 * files de chaque pool en file 0 avant de trier), cette variante préserve la
 * répartition round-robin existante entre les `nb_file_possibility` files et
 * trie chacune en place. `scroll_from_pool` consomme depuis la fin
 * (`scroll()` est LIFO) : un tri décroissant place donc les possibilités les
 * MOINS avancées (`alloc` le plus petit) en fin de chaque file, pour qu'elles
 * soient consommées en priorité — sur toutes les files, pas seulement la file 0.
 *
 * @return 0.
 */
int sort_descending_files(void)
{
	// on bloque les files le temps du trie
	lock_all_file();
	int fp;
	for (fp = 0; fp < nb_file_possibility; fp++)
	{
		sort_one_file_descending(&file_possibility[fp]->file);
		sort_one_file_descending(&file_possibility_checked[fp]->file);
	}
	unlock_all_file();
	return 0;
}

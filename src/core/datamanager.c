#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <netdb.h>
#include <unistd.h>

#include "ui/logger.h"
#include "app/static_variables.h"
#include "core/lifo.h"
#include "core/datamanager.h"
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
 * remove_possibility_analysed() est appelée pour CHAQUE possibilité acquittée
 * (INST_POSSIBILITY_ANALYSED / _BATCH, requeue côté serveur) et balayait
 * linéairement toute la file avec compare_possibility() : O(n) par retrait,
 * O(n·M) pour un acquittement de M paquets. Cet index (une table de hachage
 * par file, à chaînage séparé) ramène le cas courant — la possibilité
 * recherchée est bien présente — à O(1) amorti : compare_possibility() n'est
 * plus appelée que sur les quelques candidats du seau concerné, jamais sur
 * toute la file.
 *
 * Le hash ne porte QUE sur les champs réellement comparés par
 * compare_possibility() : alloc, x, y, les ETERN_PARTS premiers bits de
 * b_faceused (soit ETERN_PARTS/16 mots), et grid. Il exclut `checked`
 * (jamais comparé) ainsi que tout octet de bourrage de struct — b_faceused
 * porte un `__attribute__((aligned(16)))` qui, combiné au `packed` global,
 * introduit du padding avant lui ; ce padding n'est pas garanti initialisé et
 * hacher l'image mémoire brute du paquet ferait diverger le hash de deux
 * paquets pourtant égaux au sens de compare_possibility().
 *
 * L'index n'est jamais une source de vérité : sur un « miss » (rien dans le
 * seau), remove_possibility_analysed() retombe sur le balayage linéaire
 * historique — mais SEULEMENT si `analysed_index_may_be_incomplete` (ci-dessous)
 * l'exige : dans l'écrasante majorité du temps, un miss signifie une absence
 * RÉELLE (voir sa doc), et le repli — O(taille de la file), sous verrou —
 * serait une régression de performance pure sur le chemin le plus fréquent
 * (acquittement d'une possibilité déjà retirée par un autre acquittement
 * concurrent, ou par requeue_last_sent_possibility).
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
 * Attribution des analyses en cours (PR6) : `owner_uid` est le `client_uid` du
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
	 * Échéance du bail (PR7, section 4.3) : instant (epoch) au-delà duquel
	 * cette possibilité, si toujours non acquittée, est réputée abandonnée
	 * et rendue au stock par datamanager_reclaim_expired_leases(). Valide
	 * seulement si has_owner (une possibilité sans propriétaire connu n'a pas
	 * de bail — rien à rendre à personne). 0 = bail désactivé
	 * (analysed_lease_seconds <= 0 au moment de l'insertion) : jamais expiré.
	 */
	time_t lease_deadline;
	struct AnalysedIndexNode *next;
} AnalysedIndexNode;

// PR4 : tableau de POINTEURS (un par file) vers un tableau de
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
 *        (PR2) — donc il DOIT rester visible, juste pas à chaque appel).
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
			log_info("handshake serveur sans réponse (result=%i, serveur occupé ?) — nouvelle tentative ultérieure\n", result);
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
int put_to_server(client_possibility_t *client_possibility, array_possibility_packet *possibilities)
{
	// Échange réseau atomique : empêche l'entrelacement avec le thread d'alimentation.
	pthread_mutex_lock(&client_possibility->socket_mutex);
	int socket_id = check_and_connect_to_server(client_possibility);
	if (socket_id == -1) {
		pthread_mutex_unlock(&client_possibility->socket_mutex);
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
			// prévue (PR1) quand le stock du serveur est momentanément
			// intégralement verrouillé — typiquement la phase 1 de
			// consistent_backup (PR2), qui gèle toutes les files à l'instant T
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
		pthread_mutex_unlock(&client_possibility->socket_mutex);
		return -1;
	}

	pthread_mutex_unlock(&client_possibility->socket_mutex);
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
	pthread_mutex_lock(&client_possibility->socket_mutex);
	int socket_id = check_and_connect_to_server(client_possibility);
	if (socket_id == -1) {
		pthread_mutex_unlock(&client_possibility->socket_mutex);
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

	pthread_mutex_unlock(&client_possibility->socket_mutex);
	return rc;
}

/**
 * @brief Insère un tableau de possibilités dans les files locales (sans serveur).
 *
 * Utilise un trylock pour trouver une file non verrouillée parmi les 10 disponibles.
 * Toutes les possibilités sont insérées dans la même file (première libre trouvée).
 *
 * @param possibilities Tableau de possibilités à insérer.
 * @return              0.
 */
/**
 * @brief Insère dans `pool` les possibilités du tableau retenues par le filtre `want_checked`.
 *
 * Même mécanique que l'historique `put_to_local` : trylock pour trouver une
 * file libre, toutes les possibilités retenues vont dans la même file.
 *
 * @param pool          Pool de files cible.
 * @param possibilities Tableau de possibilités à filtrer/insérer.
 * @param want_checked   1 pour le pool vérifié (checked == 1), 0 pour le reste.
 * @param rr_counter    État round-robin du pool (`rr_put_unchecked`/`rr_put_checked`) —
 *                      fait démarrer chaque appel sur une file différente plutôt que
 *                      toujours la file 0 (cf. `datamanager_rr_next_start`).
 * @return              0 si inséré (ou rien à insérer), 1 si `pool` est resté
 *                       intégralement verrouillé au-delà de
 *                       DATAMANAGER_TRYLOCK_MAX_SWEEPS tours (maintenance en
 *                       cours : sauvegarde, restore, tri...) — rien n'a été
 *                       inséré dans ce cas, sûr à réessayer par l'appelant.
 */
static int put_to_pool(file_possibility_t **pool, array_possibility_packet *possibilities, int want_checked, unsigned int *rr_counter)
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

	// Plafond RAM (PR1, --stock-max-ram / commande stockMaxRam) : refuse AVANT
	// toute insertion si l'ajout ferait dépasser le budget publié par
	// datamanager_configure_ram_limit (0 = illimité, chemin inchangé). Compte
	// les DEUX pools de stock ensemble (datamanager_resident_packets, alias de
	// datas_size) puisque le budget couvre non-vérifié + vérifié, jamais un
	// seul des deux isolément. Même contrat de retour que l'épuisement du
	// budget de trylock plus bas (1 = rien inséré, sûr à réessayer) :
	// l'appelant (put_to_server côté serveur) sait déjà dégrader gracieusement
	// ce refus en INST_ERROR / repli local (cf. l'épilogue documenté dans
	// AGENTS.md pour ce chemin).
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
	// (PR1) sur l'un des deux pools n'empêche pas l'insertion dans l'autre.
	int err_unchecked = put_to_pool(file_possibility, possibilities, 0, &rr_put_unchecked);
	int err_checked = put_to_pool(file_possibility_checked, possibilities, 1, &rr_put_checked);
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
 * `datamanager_analysed_owned_by` (PR6) ne le verra simplement pas, comme
 * documenté sur `AnalysedIndexNode` ci-dessus.
 *
 * @param fileidx   Indice de la file analysée (0..nb_file_possibility-1).
 * @param e         Élément (déjà présent dans la file) à indexer.
 * @param owner_uid `client_uid` du client servi côté serveur (PR6), ou `NULL`
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
		// Bail (PR7) : échéance calculée MAINTENANT, à l'insertion — jamais
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
	// uniquement) : `preferred_file` (PR8, indice « probable » dérivé côté
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
			// Sortie bornée (même motif que add_possibility_analysed_impl,
			// PR1) : au-delà de DATAMANAGER_TRYLOCK_MAX_SWEEPS attentes
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
	pthread_mutex_lock(&client_possibility->socket_mutex);
	int socket_id = check_and_connect_to_server(client_possibility);
	if (socket_id == -1) {
		pthread_mutex_unlock(&client_possibility->socket_mutex);
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

	pthread_mutex_unlock(&client_possibility->socket_mutex);
}

/**
 * @brief Corps commun de `add_possibility_analysed`/`add_possibility_analysed_owned`
 *        (PR6) : seule la présence d'un `owner_uid` diffère entre les deux.
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
		// Sortie bornée (PR1) : cf. le commentaire de la fonction. `waits`
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
 *        détient actuellement (consultation « que travaille X ? », PR6).
 *
 * Verrouille chaque file `file_possibility_analysed[f]` (comme tout autre
 * accès à ces files) le temps de son propre balayage — jamais toutes les
 * files à la fois — puis la relâche avant de passer à la suivante. Ce n'est
 * pas un chemin chaud (commande console/diagnostic), un simple
 * `pthread_mutex_lock` bloquant est donc préférable au `trylock` utilisé
 * ailleurs dans ce fichier : on veut une réponse exacte, pas céder la main.
 *
 * Ne voit que les possibilités effectivement indexées (cf. le commentaire
 * sur `AnalysedIndexNode`) : une entrée non indexée (OOM à l'insertion, cas
 * limite déjà toléré par `remove_possibility_analysed`) est simplement
 * absente du compte, jamais une source d'erreur.
 *
 * @param owner_uid `client_uid` recherché (16 octets, jamais NULL).
 * @param out_count Nombre de possibilités actuellement attribuées à ce client.
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
unsigned long long datamanager_reclaim_expired_leases(time_t now, analysed_owner_alive_fn owner_alive) {
	unsigned long long reclaimed_total = 0;
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
					// Deux conditions requises pour réclamer (correctif PR7) :
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
			int dest = 0;
			int added = 0;
			while (!added) {
				if (pthread_mutex_trylock(&file_possibility[dest]->lock) == 0) {
					if (buf[i].alloc > max_result) max_result = buf[i].alloc;
					put(&file_possibility[dest]->file, &buf[i]);
					pthread_mutex_unlock(&file_possibility[dest]->lock);
					added = 1;
				} else {
					dest = (dest + 1) % nb_file_possibility;
					usleep(MICRO_SLEEP);
				}
			}
		}
		free(buf);
		reclaimed_total += n;
	}
	return reclaimed_total;
}

/**
 * @brief Remet dans le stock toutes les possibilités en cours d'analyse.
 *
 * Vide chaque file `file_possibility_analysed` et réinjecte les paquets dans
 * `file_possibility` (stock non vérifié). Utile quand des clients sont morts
 * sans avoir terminé leur travail.
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
            int dest = 0;
            int added = 0;
            while (!added) {
                if (pthread_mutex_trylock(&file_possibility[dest]->lock) == 0) {
                    if (buf[i].alloc > max_result) max_result = buf[i].alloc;
                    put(&file_possibility[dest]->file, &buf[i]);
                    pthread_mutex_unlock(&file_possibility[dest]->lock);
                    added = 1;
                } else {
                    dest = (dest + 1) % nb_file_possibility;
                    usleep(MICRO_SLEEP);
                }
            }
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
 * Lecture des tailles en O(1) (`.file.size`, sans verrou — même convention
 * que `file_size`/`datas_size`) pour choisir la file source/destination,
 * puis extraction bloquante de la SEULE file source (`pthread_mutex_lock`,
 * jamais `lock_all_file`), relâchée avant l'insertion — jamais deux verrous
 * de pool tenus ensemble, même discipline que `restock_analysed`/
 * `datamanager_reclaim_expired_leases`. L'insertion vise la file la plus
 * vide en priorité, avec repli en balayage rotatif (`trylock` + tour suivant)
 * si elle est momentanément prise par un autre thread — jamais de perte,
 * même motif que `restock_analysed`.
 *
 * Ne déplace rien si `pool[fullest] <= total/nb_file_possibility` (déjà
 * équilibré) : évite un va-et-vient perpétuel entre deux tours pour de
 * petites variations dues au trafic concurrent normal.
 *
 * @param pool        Pool cible (`file_possibility` ou `file_possibility_checked`).
 * @param max_packets Borne du nombre de possibilités déplacées par cet appel.
 * @return            Nombre de possibilités effectivement déplacées.
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
 * @brief Répète `rebalance_pool_step` sur UN pool jusqu'à épuisement du
 *        budget ou équilibre complet (PR3).
 *
 * `rebalance_pool_step` ne fixe qu'UNE paire (la plus pleine vers la plus
 * vide) par appel : son propre plafond de mouvement (`min(surplus, deficit)`)
 * est souvent plus petit que le budget disponible, laissant une grande
 * partie du budget d'un tour inutilisée alors que d'autres files restent
 * déséquilibrées. Cette boucle enchaîne les paires jusqu'à consommer tout
 * `max_packets` (converge plus vite, même budget total par tour) — chaque
 * pas individuel reste court (un seul verrou de pool à la fois, comme avant)
 * : ce n'est que le NOMBRE de pas par appel qui change, pas leur coût
 * unitaire.
 *
 * Termine en au plus `nb_file_possibility` pas structurellement (chaque pas
 * fixe définitivement au moins une file à sa cible, cf. `rebalance_pool_step`)
 * — garde-fou de boucle par prudence, même discipline que `split_datas`.
 *
 * @param pool        Pool cible.
 * @param max_packets Budget total pour CE pool, réparti sur autant de pas
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
	pthread_mutex_lock(&client_possibility->socket_mutex);
	int socket_id = check_and_connect_to_server(client_possibility);
	if (socket_id == -1) {
		pthread_mutex_unlock(&client_possibility->socket_mutex);
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
		pthread_mutex_unlock(&client_possibility->socket_mutex);
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
	pthread_mutex_unlock(&client_possibility->socket_mutex);
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
 */
static void scroll_from_pool(file_possibility_t **pool, array_possibility_packet *result, int max_result, unsigned int *rr_counter)
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
						result->possibilities = malloc(file.size * sizeof(struct possibility_packet));
						p = 0;
						while(file.size > 0)
						{
							scroll(&file, &result->possibilities[p]);
							result->size++;
							p++;
						}
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
			// Sortie bornée (PR1) : cf. le commentaire de la fonction. `result`
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
	scroll_from_pool(file_possibility_checked, result, max_result, &rr_scroll_checked);
	if(result->size == 0)
	{
		scroll_from_pool(file_possibility, result, max_result, &rr_scroll_unchecked);
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
	scroll_from_pool(file_possibility, result, max_result, &rr_scroll_unchecked);
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
 * @brief Sauvegarde le pool analysé et le stock à un instant T UNIQUE — corrige
 *        un trou préexistant de `backup()`/`backup_analysed()` appelées l'une après
 *        l'autre : une possibilité acquittée entre les deux instants
 *        disparaissait des deux sauvegardes (le parent déjà retiré du pool
 *        analysé, ses enfants pas encore présents dans le stock capturé plus
 *        tôt).
 *
 * Phase 1 : verrouille TOUTES les files des trois pools (analysé, stock non
 * vérifié, stock vérifié) avant d'écrire quoi que ce soit — c'est cette
 * fenêtre de gel simultané qui rend l'image cohérente à T, pas un
 * verrouillage progressif (qui laisserait une possibilité migrer d'une file
 * pas encore gelée vers une file déjà écrite). `maintenance` est posé une
 * seule fois explicitement ici, PAS via `lock_all_file()`/
 * `lock_all_file_analysed()` : leurs `unlock_*` respectifs remettraient le
 * drapeau à 0 dès la première famille libérée (non-réentrance déjà en place
 * pour ces deux verrous), alors qu'ici les deux familles doivent rester sous
 * le même état "maintenance" jusqu'à la fin de la phase 2.
 *
 * Phase 2 : écrit puis libère progressivement, une file à la fois — pool
 * analysé D'ABORD (un `INST_GET` exige à la fois un verrou de stock et un
 * verrou analysé ; libérer le stock en premier ne raccourcirait donc en rien
 * la dégradation), puis chaque file de stock (non vérifié + vérifié
 * ensemble, comme `backup()`). La fenêtre de blocage total pour un client
 * vaut ainsi le temps d'écriture d'UNE file, pas de la sauvegarde entière —
 * la capacité de service remonte par paliers au fil de la libération.
 *
 * Ne modifie jamais les pools eux-mêmes (lecture seule des `Element`
 * existants, comme `backup()`/`backup_analysed()`) : une erreur d'écriture à
 * mi-parcours ne perd ni ne duplique aucune possibilité en mémoire — seul le
 * fichier `.tmp` correspondant est invalidé.
 *
 * @param stock_filename    Fichier cible du stock (même convention que `backup`).
 * @param analysed_filename Fichier cible du pool analysé (même convention que `backup_analysed`).
 * @param out_analysed_status Sur retour, code du volet analysé (BACKUP_OK/
 *                             BACKUP_ERROR/BACKUP_SKIPPED_MAINTENANCE) — NULL accepté.
 * @return Code du volet stock (même convention que `backup`).
 */
int consistent_backup(char *stock_filename, char *analysed_filename, int *out_analysed_status)
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

	free(stock_tmp);
	free(analysed_tmp);
	if (out_analysed_status != NULL) { *out_analysed_status = rc_analysed; }
	return rc_stock;
}

int import(client_possibility_t *client_possibility, char *filename)
{
    FILE *f = fopen(filename, "r");
    if(!f)
    {
        log_errno("import file :%s ",filename);
        return -1;
    }
    
    struct possibility_packet *possibility = malloc(sizeof(struct possibility_packet));
    int repaired = 0;
    while(fread(possibility, sizeof(struct possibility_packet),1,f))
    {
        // Anciens fichiers .back (v4) : l'octet `checked` correspond à du padding
        // (taille de structure inchangée) et peut contenir n'importe quoi.
        // On assainit : tout ce qui n'est pas exactement 1 redevient « à vérifier ».
        if (possibility->checked != 1) {
            possibility->checked = 0;
        }
        // Paquets d'anciens fichiers .back : un trou peut subsister derrière la
        // position de reprise (case (0,0) jamais traitée par l'ancien moteur)
        repaired += normalize_possibility_packet(possibility);
        array_possibility_packet *possibilities = malloc(sizeof(array_possibility_packet));
        possibilities->size = 1;
        possibilities->possibilities = malloc(sizeof(struct possibility_packet));
        memcpy(&possibilities->possibilities[0], possibility, sizeof(struct possibility_packet));
        add_possibility(client_possibility, possibilities);

        free_array_possibility_packet(possibilities);
    }
    if (repaired > 0) {
        log_info("import : %i paquets ancien format normalisés (invariant alloc/directions)\n", repaired);
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
	
	struct possibility_packet *possibility = malloc(sizeof(struct possibility_packet));
	while(fread(possibility, sizeof(struct possibility_packet),1,f))
	{
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
    // Possibilités que add_possibility a refusées (plafond RAM --stock-max-ram
    // atteint) et qui n'ont donc pu être réinsérées nulle part — cf. la boucle
    // ci-dessous. Comptées et journalisées EXPLICITEMENT en fin de fonction
    // plutôt que silencieusement perdues (avant ce correctif, le code de
    // retour de add_possibility était totalement ignoré aux deux points
    // d'appel : une fois un plafond introduit, un refus devenait une perte
    // silencieuse de possibilité — un sous-arbre jamais exploré, sans aucune
    // trace dans les logs).
    unsigned long long dropped = 0;
    while (rounds < expand_max_levels && !cap_reached) {
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
        unsigned long long produced = 0;
        int expanded_any = 0;
        struct possibility_packet pkt;
        while (scroll(&work, &pkt)) {
            int deep_enough = (pkt.alloc >= (uint16_t)target_level);
            if (deep_enough || cap_reached) {
                array_possibility_packet *single = build_single_array_possibility_packet(&pkt);
                if (add_possibility(NULL, single) != 0) {
                    // Refusé (plafond RAM --stock-max-ram atteint, cf.
                    // put_to_pool) : mono-thread, pré-fork — rien ne libère de
                    // place tant que cette fonction tourne, réessayer
                    // immédiatement ne changerait rien. Compté honnêtement
                    // (jamais perdu en silence) plutôt que retenté en boucle.
                    dropped++;
                    cap_reached = 1;
                }
                free_array_possibility_packet(single);
                produced++;
                continue;
            }
            expanded_any = 1;
            key_part key;
            what_search_to_key(all_rotate_part, &pkt, &key, mapParts->sizearrayM);
            File children;
            init_file(&children, sizeof(struct possibility_packet));
            search_possiblity_light(&children, &key, &pkt, mapParts, all_rotate_part, idParts);
            // `children` est sur la PILE : la vidange par scroll libère chaque
            // Element ; pas de free_file (qui ferait free() de la structure pile).
            struct possibility_packet child;
            while (scroll(&children, &child)) {
                array_possibility_packet *single = build_single_array_possibility_packet(&child);
                if (add_possibility(NULL, single) != 0) {
                    // Même raisonnement que ci-dessus : refus honnêtement
                    // compté, jamais silencieusement perdu.
                    dropped++;
                    cap_reached = 1;
                }
                free_array_possibility_packet(single);
                produced++;
            }
            if (produced >= (unsigned long long)expand_max_stock) {
                cap_reached = 1; // le reste de `work` sera réinjecté tel quel
            }
        }
        // `work` est entièrement vidé par la boucle scroll ci-dessus (Elements
        // libérés au fil de l'eau) ; comme `children`, structure pile, pas de
        // free_file.

        if (cap_reached) {
            log_event("expansion : plafond de stock atteint (%llu ≥ %d) — arrêt de l'approfondissement",
                      produced, expand_max_stock);
        }
        if (!expanded_any) {
            break; // tout le stock a atteint le niveau cible : rien de plus à faire
        }
        rounds++;
    }

    if (dropped > 0) {
        // Perte réelle, mais désormais JOURNALISÉE explicitement plutôt que
        // silencieuse (avant ce correctif, le code de retour de
        // add_possibility n'était vérifié nulle part dans cette fonction) :
        // le plafond RAM (--stock-max-ram) est plus bas que ce que
        // --expand-level/--expand-max-stock tentent de produire.
        log_error("expansion : plafond RAM atteint, %llu possibilité(s) n'ont pas pu être "
                  "réinsérées et ont été perdues (relever --stock-max-ram, ou réduire "
                  "--expand-level/--expand-max-stock)\n", dropped);
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
                    consistent_backup("./eternityII.back", "./eternityII-in_analyse.back", NULL);
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

	log_info("check_datas errors %i on %i\n", errors, count);
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
    
    log_info("check_duplicate errors %llu on %llu\n", errors, count);
    return errors > 0 ? -1 : 0;
}

/**
 * @brief Cumule dans `levels` la répartition par `alloc` des paquets de `file`.
 *
 * Un `alloc` hors bornes (paquet corrompu) est ignoré plutôt qu'écrit hors du
 * tableau — ce parcours est de l'observation, il ne doit jamais pouvoir
 * déborder sur une donnée douteuse.
 *
 * @param file   File à parcourir (verrou déjà tenu par l'appelant).
 * @param levels Histogramme de `STOCK_DISTRIBUTION_LEVELS` entrées à incrémenter.
 * @return       Nombre de paquets comptés (y compris ceux d'`alloc` hors bornes).
 */
static unsigned long long accumulate_alloc_levels(File *file, unsigned long long *levels)
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
        out->total_unchecked += accumulate_alloc_levels(&file_possibility[fp]->file, out->unchecked);
        out->total_checked += accumulate_alloc_levels(&file_possibility_checked[fp]->file, out->checked);
    }
    unlock_all_file();

    // Passe 2 : le pool « en cours d'analyse », sous sa propre famille de
    // verrous — jamais tenue en même temps que la précédente.
    lock_all_file_analysed();
    for (int fp = 0; fp < nb_file_possibility; fp++)
    {
        out->total_analysed += accumulate_alloc_levels(&file_possibility_analysed[fp]->file, out->analysed);
    }
    unlock_all_file_analysed();
}

int statistic_datas(void)
{
    stock_distribution_t distribution;
    datamanager_stock_distribution(&distribution);

    // Historiquement, cette commande ne rapporte que les deux pools de stock
    // (le pool analysé, distribué aux clients, n'en fait pas partie).
    log_info("check_datas analyses:%llu\n", distribution.total_unchecked + distribution.total_checked);
    for (int i = 0; i < STOCK_DISTRIBUTION_LEVELS; i++) {
        log_info("%i : %llu\n", i, distribution.unchecked[i] + distribution.checked[i]);
    }
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
			return 1;
		}
	}
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>

#include "ui/logger.h"
#include "app/static_variables.h"
#include "core/lifo.h"
#include "core/datamanager.h"
#include "net/tcpclient.h"
#include "net/etii_protocol.h"
#include "core/readdata.h"

static file_possibility_t file_possibility[NB_FILE_POSSIBILITY] =
{
	{{NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
	{{NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER}
};

static file_possibility_t file_possibility_analysed[NB_FILE_POSSIBILITY] =
{
	{{NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
	{{NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER}
};

/**
 * @brief Files des possibilités vérifiées par un client pruner (`checked == 1`).
 *
 * Servies en priorité aux clients de recherche (INST_GET). Le pool historique
 * `file_possibility` reçoit les possibilités non vérifiées et alimente les
 * clients pruners (INST_GET_TO_CHECK) ainsi que, en repli, les clients de
 * recherche quand ce pool-ci est vide (fonctionnement sans pruner inchangé).
 */
static file_possibility_t file_possibility_checked[NB_FILE_POSSIBILITY] =
{
	{{NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
	{{NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER}
};


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
		long result = send(socket_id, (struct possibility_packet *)possibility, sizeof(struct possibility_packet),0);
		if (result <= 0) {
			log_errno("problème put_to_server send => ");
			/* L'envoi a échoué : on sort et on remet t..fin en local */
			connection_lost = 1;
			break;
		}
		int8_t ack = recv_instruction(socket_id);
		if(ack != INST_CONSIDERED) {
			log_error("problème de prise en compte du serveur (ack=%d)\n", ack);
			array_possibility_packet *single_array = build_single_array_possibility_packet(possibility);
			put_to_local(single_array);
			free_array_possibility_packet(single_array);
			print_possibility_packet(possibility);
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
 * @return              0.
 */
static int put_to_pool(file_possibility_t *pool, array_possibility_packet *possibilities, int want_checked)
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

	int addpossibility = 0;
	int currfile = 0;
	while(addpossibility == 0)
	{
		if(pthread_mutex_trylock(&pool[currfile].lock) == 0)
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
				
                put(&pool[currfile].file, &possibilities->possibilities[t]);
            }
			addpossibility = 1;
			pthread_mutex_unlock(&pool[currfile].lock);
		}
		currfile++;
		if(currfile >= NB_FILE_POSSIBILITY)
		{
			currfile = 0;
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
	// vont dans leur pool dédié, les autres dans le pool historique.
	put_to_pool(file_possibility, possibilities, 0);
	put_to_pool(file_possibility_checked, possibilities, 1);
	return 0;
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

int remove_possibility_analysed(struct possibility_packet *possibility, int thread) {
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
	if (thread >=0) {
		currfile = thread;
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
		if(pthread_mutex_trylock(&file_possibility_analysed[currfile].lock) == 0)
		{
			File *file = &file_possibility_analysed[currfile].file;
			// On défile la suite pour retrouver la possibilité
			Element *element = file->start;
			while (removed_possibility == 0 && element != NULL) {
				struct possibility_packet *possibilityInFile = element->value;
				// comparaison

				if (compare_possibility(possibilityInFile, possibility) == 0) {
					file_remove_element(file, element);
					removed_possibility = 1;
				} else {
					// On passe au suivant
					element = element->next;
				}
			}
			
			checked = 1;
			pthread_mutex_unlock(&file_possibility_analysed[currfile].lock);
		}
		if (checked == 1) {
			if (thread < 0) {
				currfile++;
				if(currfile >= NB_FILE_POSSIBILITY)
				{
					// On n'a pas retrouvé la possibilité
#ifdef DEBUG_CHECK_POSSIBILITY
					log_debug("non supprimée \n");
#endif // DEBUG_CHECK_POSSIBILITY
					return 1;
				}
			} else if (removed_possibility == 0) {
#ifdef DEBUG_CHECK_POSSIBILITY
                log_debug("non supprimée \n");
#endif // DEBUG_CHECK_POSSIBILITY
				// On n'a pas retrouvé la possibilité
				return 1;
			}
		} else {
			usleep(MICRO_SLEEP);
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
		if(pthread_mutex_trylock(&file_possibility_analysed[thread].lock) == 0)
		{
			File *file = &file_possibility_analysed[thread].file;
			Element *element = file->start;
			if (element != NULL) {
                struct possibility_packet *possibility = malloc(sizeof(struct possibility_packet));
				while (scroll(file, possibility)) {
                    ;
				}
                free(possibility);
			}

			pthread_mutex_unlock(&file_possibility_analysed[thread].lock);
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
	if(pthread_mutex_trylock(&file_possibility_analysed[thread].lock) == 0)
	{
		File *file = &file_possibility_analysed[thread].file;
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
					while (drained < cap && scroll(file, &buf[drained])) {
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
						for (int i = 0; i < drained; i++) put(file, &buf[i]);
						break;
					}
					if (recv_instruction(socket_id) != INST_CONSIDERED) {
						log_error("batch analysed non pris en compte (%d possibilités), remise en file\n", drained);
						for (int i = 0; i < drained; i++) put(file, &buf[i]);
						break;
					}
				} while (drained == cap);
				free(buf);
			}
		}

		pthread_mutex_unlock(&file_possibility_analysed[thread].lock);
	}

	pthread_mutex_unlock(&client_possibility->socket_mutex);
}

/**
 * @brief Ajoute une possibilité dans la file « analysées » du thread indiqué.
 *
 * Tente un trylock sur `file_possibility_analysed[thread]`. Si `thread < 0`,
 * tourne sur toutes les files jusqu'à en trouver une disponible. Met à jour
 * `max_result` si `alloc` du paquet est supérieur.
 *
 * @param possiblity Paquet à enregistrer comme « en cours d'analyse ».
 * @param thread     Index du thread cible (≥ 0), ou -1 pour la première file libre.
 * @return           0 si ajouté, -1 si toutes les files sont verrouillées.
 */
int add_possibility_analysed(struct possibility_packet *possiblity, int thread) {
	int addpossibility = 0;
	int currfile = 0;
	if (thread >=0) {
		currfile = thread;
	}
	while(possiblity != NULL && addpossibility == 0)
	{
		if(pthread_mutex_trylock(&file_possibility_analysed[currfile].lock) == 0)
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
			
			put(&file_possibility_analysed[currfile].file, possiblity);
			addpossibility = 1;
			pthread_mutex_unlock(&file_possibility_analysed[currfile].lock);
		}
		if (thread < 0) {
			currfile++;
			if(currfile >= NB_FILE_POSSIBILITY)
			{
				currfile = 0;
				usleep(MICRO_SLEEP);
			}
		} else if (addpossibility == 0) {
			usleep(MICRO_SLEEP);
		}
	}
	return 0;
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
    for (int f = 0; f < NB_FILE_POSSIBILITY; f++) {
        pthread_mutex_lock(&file_possibility_analysed[f].lock);
        File *src = &file_possibility_analysed[f].file;
        unsigned long long count = src->size;
        if (count == 0) {
            pthread_mutex_unlock(&file_possibility_analysed[f].lock);
            continue;
        }
        struct possibility_packet *buf = malloc(count * sizeof(struct possibility_packet));
        unsigned long long n = 0;
        while (n < count && scroll(src, &buf[n])) {
            n++;
        }
        pthread_mutex_unlock(&file_possibility_analysed[f].lock);

        for (unsigned long long i = 0; i < n; i++) {
            int dest = 0;
            int added = 0;
            while (!added) {
                if (pthread_mutex_trylock(&file_possibility[dest].lock) == 0) {
                    if (buf[i].alloc > max_result) max_result = buf[i].alloc;
                    put(&file_possibility[dest].file, &buf[i]);
                    pthread_mutex_unlock(&file_possibility[dest].lock);
                    added = 1;
                } else {
                    dest = (dest + 1) % NB_FILE_POSSIBILITY;
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
	// Un client pruner demande des possibilités non vérifiées
	int8_t get_instruction = pruner_mode ? INST_GET_TO_CHECK : INST_GET;
	int r;
	for(r=0; r < max_result;r++){
		send_instruction(socket_id, get_instruction);
		long recv_r = recv(socket_id, &buffer, sizeof(buffer), 0);
		if(recv_r == 0 || (recv_r == sizeof(int8_t) && (*(int8_t *)&buffer) == INST_NULL))
		{
#ifdef DEBUG_SOCKET
			log_info("No possibility recept\n");
#endif // DEBUG_SOCKET
		} else if (recv_r < 0) {
			log_errno("Error when receive possibility => ");
		}else
		{
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
 * @param result     Tableau de résultats à remplir.
 * @param max_result Nombre maximum de possibilités à extraire.
 */
static void scroll_from_pool(file_possibility_t *pool, array_possibility_packet *result, int max_result)
{
	int getpossibility = 0;
	int currfile = 0;
	int filetested[NB_FILE_POSSIBILITY];
	int i;
	for(i = 0; i < NB_FILE_POSSIBILITY; i++)
	{
		filetested[i] = 0;
	}
	while (getpossibility == 0) {
		int f;
		for (f=0; f < NB_FILE_POSSIBILITY && getpossibility == 0; f++)
		{
			if(filetested[f] == 0)
			{
				currfile = f;
				if(pthread_mutex_trylock(&pool[currfile].lock) == 0)
				{
					int p;
					int nothing = 0;
					File file;
					struct possibility_packet packet;
					init_file(&file, sizeof(struct possibility_packet));
					for(p=0; p < max_result && nothing == 0;p++)
					{
						if(scroll(&pool[currfile].file, &packet))
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
					pthread_mutex_unlock(&pool[currfile].lock);
				}
			}
		}
		
		if(getpossibility == 1 && result->size == 0)
		{
			int all_tested = 1;
			for(f=0; f < NB_FILE_POSSIBILITY; f++)
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
	scroll_from_pool(file_possibility_checked, result, max_result);
	if(result->size == 0)
	{
		scroll_from_pool(file_possibility, result, max_result);
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
	scroll_from_pool(file_possibility, result, max_result);
}

array_possibility_packet *get_last_possibility(client_possibility_t *client_possibility, int max_result)
{
	array_possibility_packet *result = malloc(sizeof(array_possibility_packet));
	result->size = 0;
	result->possibilities = NULL;
	
	scroll_from_local(result, max_result);
    
	if(result->size == 0 && server_ip != NULL)
	{
		scroll_from_server(client_possibility, result, max_result);
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
	if(nfile >= 0 && nfile < NB_FILE_POSSIBILITY)
	{
		return file_possibility[nfile].file.size;
	}
    return 0;
}

unsigned long long file_checked_size(int nfile)
{
	if(nfile >= 0 && nfile < NB_FILE_POSSIBILITY)
	{
		return file_possibility_checked[nfile].file.size;
	}
	return 0;
}

unsigned long long file_analysed_size(int nfile)
{
	if(nfile >= 0 && nfile < NB_FILE_POSSIBILITY)
	{
		return file_possibility_analysed[nfile].file.size;
	}
	return 0;
}

unsigned long long datas_size(void)
{
    unsigned long long result = 0;
	int f;
	for(f=0; f < NB_FILE_POSSIBILITY; f++)
	{
        result += file_size(f);
        result += file_checked_size(f);
	}
	return result;
}

/**
 * @brief Verrouille toutes les files de possibilités et active le mode maintenance.
 *
 * Appel bloquant (pthread_mutex_lock) sur chacune des `NB_FILE_POSSIBILITY` files.
 * Doit être suivi d'un appel à `unlock_all_file`.
 */
void lock_all_file(void)
{
	maintenance = 1;
	int fp;
	// Bloquage des files (les deux pools : non vérifié et vérifié)
	for (fp=0; fp < NB_FILE_POSSIBILITY; fp++)
	{
		pthread_mutex_lock(&file_possibility[fp].lock);
		pthread_mutex_lock(&file_possibility_checked[fp].lock);
	}
}

/**
 * @brief Déverrouille toutes les files de possibilités et désactive le mode maintenance.
 */
void unlock_all_file(void)
{
	int fp;
	//libération des files (les deux pools)
	for (fp=0; fp < NB_FILE_POSSIBILITY; fp++)
	{
		pthread_mutex_unlock(&file_possibility[fp].lock);
		pthread_mutex_unlock(&file_possibility_checked[fp].lock);
	}
	maintenance = 0;
}

int backup(char *filename)
{
	if(!maintenance)
	{
		FILE *f = fopen(filename, "w");
		if(!f)
		{
			log_error("backup file :%s",filename);
			perror("fopen()");
			return -1;
		}
		
		lock_all_file();
		int fp;
		for (fp=0; fp < NB_FILE_POSSIBILITY; fp++)
		{
			Element *currElement = file_possibility[fp].file.start;
			while(currElement != NULL)
			{
				if(currElement->value != NULL)
				{
					struct possibility_packet *possibility = (struct possibility_packet *)currElement->value;
					fwrite(possibility, sizeof(struct possibility_packet), 1, f);
				}
				currElement = currElement->next;
			}
			// Pool vérifié : le flag `checked` est dans le paquet, la restauration
			// re-routera automatiquement chaque possibilité dans le bon pool.
			currElement = file_possibility_checked[fp].file.start;
			while(currElement != NULL)
			{
				if(currElement->value != NULL)
				{
					struct possibility_packet *possibility = (struct possibility_packet *)currElement->value;
					fwrite(possibility, sizeof(struct possibility_packet), 1, f);
				}
				currElement = currElement->next;
			}
		}
		unlock_all_file();
		
		fclose(f);
	}
	return 0;
}

/**
 * @brief Verrouille toutes les files des possibilités en cours d'analyse.
 */
void lock_all_file_analysed(void)
{
	maintenance = 1;
	int fp;
	// Bloquage des files
	for (fp=0; fp < NB_FILE_POSSIBILITY; fp++)
	{
		pthread_mutex_lock(&file_possibility_analysed[fp].lock);
	}
}

/**
 * @brief Déverrouille toutes les files des possibilités en cours d'analyse.
 */
void unlock_all_file_analysed(void)
{
	int fp;
	//libération des files
	for (fp=0; fp < NB_FILE_POSSIBILITY; fp++)
	{
		pthread_mutex_unlock(&file_possibility_analysed[fp].lock);
	}
	maintenance = 0;
}

int backup_analysed(char *filename)
{
	if(!maintenance)
	{
		FILE *f = fopen(filename, "w");
		if(!f)
		{
			log_error("backup_analysed file :%s",filename);
			perror("fopen()");
			return -1;
		}
		
		lock_all_file_analysed();
		int fp;
		for (fp=0; fp < NB_FILE_POSSIBILITY; fp++)
		{
			Element *currElement = file_possibility_analysed[fp].file.start;
			while(currElement != NULL)
			{
				if(currElement->value != NULL)
				{
					struct possibility_packet *possibility = (struct possibility_packet *)currElement->value;
					fwrite(possibility, sizeof(struct possibility_packet), 1, f);
				}
				currElement = currElement->next;
			}
		}
		unlock_all_file_analysed();
		
		fclose(f);
	}
	return 0;
}

int import(client_possibility_t *client_possibility, char *filename)
{
    FILE *f = fopen(filename, "r");
    if(!f)
    {
        log_error("import file :%s",filename);
        perror("fopen()");
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
		log_error("restore file :%s",filename);
		perror("fopen()");
		return -1;
	}
	fclose(f);

	lock_all_file();
	int fp;
	//vidage des files (les deux pools)
	for (fp=0; fp < NB_FILE_POSSIBILITY; fp++)
	{
		File *suite = &file_possibility[fp].file;
		struct possibility_packet value;
		while(suite->size >0)
		{
			scroll(suite, &value);
		}
		suite = &file_possibility_checked[fp].file;
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
	for (fp=0; fp < NB_FILE_POSSIBILITY; fp++)
	{
		File *suite = &file_possibility[fp].file;
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
		log_error("import_analysed file :%s",filename);
		perror("fopen()");
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
		log_error("restore_analysed file :%s",filename);
		perror("fopen()");
		return -1;
	}
	fclose(f);

	lock_all_file_analysed();
	int fp;
	//vidage des files
	for (fp=0; fp < NB_FILE_POSSIBILITY; fp++)
	{
		File *suite = &file_possibility_analysed[fp].file;
		while(suite->size >0)
		{
			struct possibility_packet *value = malloc(sizeof(struct possibility_packet));
			scroll(suite, value);
			free(value);
		}
	}

	unlock_all_file_analysed();

	return import_analysed(filename);
}

int print_file(int fp)
{
    // Les deux pools sont affichés : non vérifié et vérifié (checked == 1)
    File *pools[2] = { &file_possibility[fp].file, &file_possibility_checked[fp].file };
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
	for (fp=0; fp < NB_FILE_POSSIBILITY; fp++)
	{
		print_file(fp);
	}
	
	unlock_all_file();
	
	return 0;
}

int print_file_analysed(int fp)
{
	log_info("file_analysed %i, size:%llu\n", fp, file_possibility_analysed[fp].file.size);
    Element *currElement = file_possibility_analysed[fp].file.start;
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

int print_all_file_analysed(void)
{
	lock_all_file_analysed();
	int fp;
	for (fp=0; fp < NB_FILE_POSSIBILITY; fp++)
	{
		print_file_analysed(fp);
	}
	
	unlock_all_file_analysed();
	
	return 0;
}

/**
 * @brief Regroupe toutes les files d'un pool dans sa file 0, sans verrouillage.
 *
 * Doit être appelée avec les files déjà verrouillées.
 *
 * @param pool Tableau de files (pool non vérifié ou vérifié).
 * @return     Taille totale de la file 0 après regroupement.
 */
static unsigned long long regroup_pool_nolock(file_possibility_t *pool)
{
	int fp;
	unsigned long long size = pool[0].file.size;
	struct possibility_packet *packet = malloc(sizeof(struct possibility_packet));
	for (fp=1; fp < NB_FILE_POSSIBILITY; fp++)
	{

		while (pool[fp].file.size > 0) {

			scroll(&pool[fp].file,packet);
			if(packet!=NULL)
			{
				put(&pool[0].file, packet);
				size++;
			}

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

// Test qu'une seule fois de placer. On peut donc trouver des possibilités avec suite mais en ayant placé les cases ayant
// qu'une seule possibilité, au tir suivant la possibilité peut avoir aucune suite car des pieces placées (1 seule poss) ont
// pu éléminer une case à plusieurs possiblités.
int remove_possibilities_with_no_next(map_big_array *mapParts, struct array_part *all_rotate_part)
{
    lock_all_file();
    int fp;
	// Seul le pool non vérifié est élagué : une possibilité vérifiée (checked == 1)
	// a déjà été confirmée vivante par un pruner, elle a donc forcément une suite.
	for (fp=0; fp < NB_FILE_POSSIBILITY; fp++)
	{
		Element *currElement = file_possibility[fp].file.start;
        
		while (currElement != NULL)
		{
            Element *nextElement = NULL;
			struct possibility_packet *possibility = (struct possibility_packet *)currElement->value;
            int has_next = possibility_all_has_a_next(possibility, mapParts, all_rotate_part);
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
                    backup("./eternityII.back");
                    backup_analysed("./eternityII-in_analyse.back");
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
                    file_possibility[fp].file.start = currElement->next;
                }

                // On a une suite alors le précédent du suivant devient le précédent du courrant
                if(currElement->next != NULL)
                {
                    currElement->next->previous = currElement->previous;
                } else {
                    // Pas de suite alors la fin de la pile  devient le précédent (ou null)
                    file_possibility[fp].file.end = currElement->previous;
                }
                nextElement = currElement->next;
                free (currElement->value);
                free (currElement);
                currElement = NULL;
                file_possibility[fp].file.size--;

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
    return 0;
}

/**
 * @brief Répartit équitablement les possibilités entre `nbsplit` files sans verrouillage.
 *
 * Regroupe d'abord tout dans la file 0, puis distribue par quotient égal.
 * Doit être appelée avec les files déjà verrouillées.
 *
 * @param nbsplit Nombre de files cibles (≤ NB_FILE_POSSIBILITY).
 * @return        0.
 */
/**
 * @brief Répartit un pool sur `nbsplit` files, sans verrouillage.
 * @param pool    Tableau de files (pool non vérifié ou vérifié).
 * @param nbsplit Nombre de files cibles (≤ NB_FILE_POSSIBILITY).
 * @return        0.
 */
static int split_pool_nolock(file_possibility_t *pool, int nbsplit)
{
	regroup_pool_nolock(pool);

	File *file = malloc(sizeof(File));
	init_file(file, sizeof(struct possibility_packet));
	struct possibility_packet *possibility = malloc(sizeof(struct possibility_packet));
	while (pool[0].file.size > 0)
	{

		if(scroll(&pool[0].file, possibility))
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
		while(pool[f].file.size < (unsigned long long)quotient && file->size > 0){
			if(scroll(file, possibility))
			{
				put(&pool[f].file, possibility);
			}
		}
	}

	// si le quotient n'était pas bon on vide dans la premiere liste pour éviter la perte
	while(file->size > 0){
		if(scroll(file, possibility))
		{
			put(&pool[0].file, possibility);
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
	lock_all_file();
	// Les deux pools sont répartis indépendamment : non vérifié et vérifié
	split_pool_nolock(file_possibility, NB_FILE_POSSIBILITY);
	split_pool_nolock(file_possibility_checked, NB_FILE_POSSIBILITY);
	unlock_all_file();
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
	for (fp=0; fp < NB_FILE_POSSIBILITY; fp++)
	{
		// Les deux pools sont vérifiés : non vérifié et vérifié (checked == 1)
		File *pools[2] = { &file_possibility[fp].file, &file_possibility_checked[fp].file };
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
					print_possibility_packet((struct possibility_packet *)currElement->value);
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
        for (cfp=fp; cfp < NB_FILE_POSSIBILITY; cfp++)
        {
            unsigned long long comparePosition = 0;
            Element *elementToCompare = NULL;
            if (fp == cfp) {
                elementToCompare = currElement->next;
                comparePosition = position + 1;
                //printf("%i position %llu start with next for %llu\n", args->threadPosition, position, duplicateCount[args->threadPosition]);
            } else {
                elementToCompare = file_possibility[cfp].file.start;
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
        if (position >= file_possibility[fp].file.size) {
            fp++;
            position = 0;
            currElement = NULL;
            if (fp < NB_FILE_POSSIBILITY) {
                currElement = file_possibility[fp].file.start;
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
    // au pool vérifié (checked == 1) imposerait de chaîner 2*NB_FILE_POSSIBILITY
    // files dans une même séquence — hors périmètre ici. On dimensionne donc le
    // travail sur la taille du seul pool non vérifié (et non datas_size(), qui
    // inclut le pool vérifié) pour que le nombre de combinaisons corresponde
    // exactement aux éléments réellement parcourus.
    unsigned long long dataSize = 0;
    for (int f = 0; f < NB_FILE_POSSIBILITY; f++) {
        dataSize += file_size(f);
    }
    unsigned long long nbCombinations = count_combinations(dataSize);
    unsigned long long nbByThread = nbCombinations / nbDuplicateThread;
    log_info("qt: %llu nb combinations %llu | nb/threads: %llu\n", dataSize, nbCombinations, nbByThread);
    
    int fp = 0;
    Element *currElement = file_possibility[fp].file.start;
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
    for (int t = 0; t < nbDuplicateThread && fp < NB_FILE_POSSIBILITY && allocated < nbCombinations; t++) {
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
                if (position > file_possibility[fp].file.size) {
                    fp++;
                    position = 0;
                    if (fp >= NB_FILE_POSSIBILITY) {
                        currElement = NULL;
                        break;
                    }
                    currElement = file_possibility[fp].file.start;
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

int statistic_datas(void)
{
    lock_all_file();
    int count=0;
    int fp;
    // +1 : alloc peut valoir ETERN_PARTS (plateau complet, ex. import d'un .back
    // complet où normalize_possibility_packet ne réduit pas alloc faute de trou).
    int countSize[ETERN_PARTS + 1];
    for (int i = 0; i <= ETERN_PARTS; i++) {
        countSize[i] = 0;
    }
    for (fp=0; fp < NB_FILE_POSSIBILITY; fp++)
    {
        // Les deux pools comptent : non vérifié et vérifié (checked == 1)
        File *pools[2] = { &file_possibility[fp].file, &file_possibility_checked[fp].file };
        for (int p = 0; p < 2; p++)
        {
            Element *currElement = pools[p]->start;
            while (currElement != NULL)
            {
                count++;
                struct possibility_packet *possibility = (struct possibility_packet *)currElement->value;
                if (possibility != NULL)
                {
                    countSize[possibility->alloc]++;
                }
                currElement = currElement->next;
            }
        }
    }

    unlock_all_file();

    log_info("check_datas analyses:%i\n",count);
    for (int i = 0; i <= ETERN_PARTS; i++) {
        log_info("%i : %i\n", i, countSize[i]);

        countSize[i] = 0;
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
	for (fp=0; fp < NB_FILE_POSSIBILITY; fp++)
	{
		// Les deux pools comptent : non vérifié et vérifié (checked == 1)
		result = min_alloc_in_file(&file_possibility[fp].file, result);
		result = min_alloc_in_file(&file_possibility_checked[fp].file, result);
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
	sort_one_file_ascending(&file_possibility[0].file);
	regroup_pool_nolock(file_possibility_checked);
	sort_one_file_ascending(&file_possibility_checked[0].file);
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
	sort_one_file_descending(&file_possibility[intf].file);
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
	int result = check_one_file(&file_possibility[f].file, f, "unchecked");
	if(check_one_file(&file_possibility_checked[f].file, f, "checked") != 0)
	{
		result = -1;
	}
	return result;
}

int check_files(void)
{
	int f;
	for(f = 0; f < NB_FILE_POSSIBILITY;f++)
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
	sort_one_file_descending(&file_possibility[0].file);
	return 0;
}

/**
 * @brief Trie toutes les files en parallèle (un thread par file).
 *
 * Lance `NB_FILE_POSSIBILITY` threads de tri descendant en parallèle
 * et attend leur fin avec `pthread_join`.
 */
void sortdmthread(void)
{
	pthread_t *tid = malloc( NB_FILE_POSSIBILITY * sizeof(pthread_t) );
	int *f = malloc(NB_FILE_POSSIBILITY * sizeof(int));
	int i;
	for( i=0; i<NB_FILE_POSSIBILITY; i++ )
	{
		f[i] = i;
		pthread_create( &tid[i], NULL, sort_d_mono, &f[i] );
	}
	
	
	// Attente que les threads on terminés
	for( i=0; i<NB_FILE_POSSIBILITY; i++ )
	{
		pthread_join( tid[i], NULL );
	}

	free(tid);
	free(f);
}

int sort_descending_mthread(void)
{
	lock_all_file();
	
	int nbfile=NB_FILE_POSSIBILITY;
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
	sort_one_file_descending(&file_possibility_checked[0].file);

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
	sort_one_file_descending(&file_possibility_checked[0].file);

	unlock_all_file();
	return 0;
}

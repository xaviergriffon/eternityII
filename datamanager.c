#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h> /* gethostbyname */
#include <errno.h>
#include <time.h>

#include "logger.h"
#include "static_variables.h"
#include "lifo.h"
#include "datamanager.h"
#include "tcpclient.h"
#include "etii_protocol.h"
#include "readdata.h"

static file_possibility_t file_possibility[NB_FILE_POSSIBILITY] =
{
	{{NULL,NULL,0,0,NULL,NULL, 0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
	{{NULL,NULL,0,0,NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,0,NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,0,NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,0,NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,0,NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,0,NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,0,NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,0,NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,0,NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER}
};

static file_possibility_t file_possibility_analysed[NB_FILE_POSSIBILITY] =
{
	{{NULL,NULL,0,0,NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
	{{NULL,NULL,0,0,NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,0,NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,0,NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,0,NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,0,NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,0,NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,0,NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,0,NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,0,NULL,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER}
};

 struct old_possibility_packet
 {
 uint8_t x;
 uint8_t y;
 int16_t grid[ETERN_SIZE][ETERN_SIZE];
 uint16_t alloc;
 uint8_t faceused[ETERN_PARTS];
 } __attribute__((__packed__));


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
		if (result != INST_SUPPORTED_VERSION) {
			log_error("Version not supported by server\n");
			close_socket(socket_id);
			request = REQUEST_STOP;
			return -1;
			//exit(EXIT_FAILURE);
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

/**
 * @brief Insère un tableau de possibilités dans les files locales (sans serveur).
 *
 * Utilise un trylock pour trouver une file non verrouillée parmi les 10 disponibles.
 * Toutes les possibilités sont insérées dans la même file (première libre trouvée).
 *
 * @param possibilities Tableau de possibilités à insérer.
 * @return              0.
 */
int put_to_local(array_possibility_packet *possibilities)
{
	int addpossibility = 0;
	int currfile = 0;
	while(possibilities != NULL && addpossibility == 0)
	{
		if(pthread_mutex_trylock(&file_possibility[currfile].lock) == 0)
		{
            int t;
            for(t=0; t< possibilities->size; t++)
            {
                if(possibilities->possibilities[t].alloc > max_result)
                {
                    max_result = possibilities->possibilities[t].alloc;
                    //printf("max result:%i\n",max_result);
                }
                /*
                if(possibilities->possibilities[t].x < 0 || possibilities->possibilities[t].y < 0 || possibilities->possibilities[t].x > 16 || possibilities->possibilities[t].y > 16)
                {
                    printf("alert\n");
                }
                 */
				
                put(&file_possibility[currfile].file, &possibilities->possibilities[t]);
            }
			addpossibility = 1;
			pthread_mutex_unlock(&file_possibility[currfile].lock);
		}
		currfile++;
		if(currfile >= NB_FILE_POSSIBILITY)
		{
			currfile = 0;
		}
	}
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
					// TODO : factoriser
                    Element *currentElement = element;
					if (element->next != NULL) {
						element = element->next;
						if (currentElement->previous != NULL) {
							currentElement->previous->next = element;
							element->previous = currentElement->previous;
						} else {
							file->start = element;
							element->previous = NULL;
						}
					} else {
						if (file->start == element) {
							file->start = NULL;
							file->end = NULL;
						} else {
							file->end = element->previous;
							element->previous->next = NULL;
						}
					}
                    if (possibilityInFile != NULL) {
                        free(possibilityInFile);
                    }
                    free(currentElement);
					file->size--;

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
			struct possibility_packet *possibility = malloc(sizeof(struct possibility_packet));
			while (scroll(file, possibility)) {
				send_instruction(socket_id, INST_POSSIBILITY_ANALYSED);
				ssize_t result = send(socket_id, (struct possibility_packet *)possibility, sizeof(struct possibility_packet),0);
				if (result < 0 ) {
					log_errno("Error when send_possibility_analysed => ");
					put(file, possibility);
					break;
				}
				if(recv_instruction(socket_id) != INST_CONSIDERED){
					log_error("possibility analyzed not taken into account :\n");
					struct tms t2;
					times(&t2);
					time_t t;
					long tops = sysconf(_SC_CLK_TCK);
					t = ((t2.tms_utime + t2.tms_stime)
							- (client_possibility->start_socket.tms_utime + client_possibility->start_socket.tms_stime)) * 1000 / tops;
                    log_error ("socket time : %ld\n", t);
					print_possibility_packet(possibility);
                    put(file, possibility);
					break;
				}
/*
                Element *currentElement = element;
				if (element->next != NULL) {
					element = element->next;
					if (currentElement->previous != NULL) {
						currentElement->previous->next = element;
						element->previous = currentElement->previous;
					} else {
						file->start = element;
						element->previous = NULL;
					}
                    possibility = element->value;
				} else {
					if (file->start == element) {
						file->start = NULL;
						file->end = NULL;
					} else {
						file->end = element->previous;
						element->previous->next = NULL;
					}
					possibility = NULL;
				}
                // TODO : tester si liste avec cache
                free(currentElement);
				file->size--;
 */
			}
            free(possibility);
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

	File file;
	init_file_with_cache(&file, 0, sizeof(struct possibility_packet));
	
	struct possibility_packet buffer;
	int r;
	for(r=0; r < max_result;r++){
		send_instruction(socket_id, INST_GET);
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
void scroll_from_local(array_possibility_packet *result, int max_result)
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
				if(pthread_mutex_trylock(&file_possibility[currfile].lock) == 0)
				{
					int p;
					int nothing = 0;
					File file;
					struct possibility_packet packet;
					init_file_with_cache(&file, 0, sizeof(struct possibility_packet));
					for(p=0; p < max_result && nothing == 0;p++)
					{
						if(scroll(&file_possibility[currfile].file, &packet))
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
					pthread_mutex_unlock(&file_possibility[currfile].lock);
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

unsigned long long file_size(int nfile)
{
	if(nfile >= 0 && nfile < NB_FILE_POSSIBILITY)
	{
		return file_possibility[nfile].file.size;
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
	// Bloquage des files
	for (fp=0; fp < NB_FILE_POSSIBILITY; fp++)
	{
		pthread_mutex_lock(&file_possibility[fp].lock);
	}
}

/**
 * @brief Déverrouille toutes les files de possibilités et désactive le mode maintenance.
 */
void unlock_all_file(void)
{
	int fp;
	//libération des files
	for (fp=0; fp < NB_FILE_POSSIBILITY; fp++)
	{
		pthread_mutex_unlock(&file_possibility[fp].lock);
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
			exit(EXIT_FAILURE);
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

#ifdef FACES_USED_BITS
int import_old_file(client_possibility_t *client_possibility, char *filename)
{
	FILE *f = fopen(filename, "r");
	if(!f)
	{
		log_error("import file :%s",filename);
		perror("fopen()");
		exit(EXIT_FAILURE);
	}
	
	struct old_possibility_packet *old_possibility = malloc(sizeof(struct old_possibility_packet));
    struct possibility_packet *new_possibility = malloc(sizeof(struct possibility_packet));
	while(fread(old_possibility, sizeof(struct old_possibility_packet),1,f))
	{
        new_possibility->alloc = old_possibility->alloc;
        new_possibility->x = old_possibility->x;
        new_possibility->y = old_possibility->y;
        for(int y = 0; y < ETERN_SIZE; y++) {
            for(int x = 0; x < ETERN_SIZE; x++) {
                new_possibility->grid[y][x] = old_possibility->grid[y][x];
            }
        }
        
        for(int i = 0; i < ETERN_PARTS;i++) {
            set_face_used(new_possibility->b_faceused, i, old_possibility->faceused[i]);
        }

        // Fichiers ancien format : répare l'invariant alloc/directions si besoin
        normalize_possibility_packet(new_possibility);

		array_possibility_packet *possibilities = malloc(sizeof(array_possibility_packet));
		possibilities->size = 1;
		possibilities->possibilities = malloc(sizeof(struct possibility_packet));
		memcpy(&possibilities->possibilities[0], new_possibility, sizeof(struct possibility_packet));
		add_possibility(client_possibility, possibilities);
		
		free_array_possibility_packet(possibilities);
	}
	
	free(new_possibility);
    free(old_possibility);
	
	
	fclose(f);
	return 0;
}

int restore_old_file(char *filename)
{
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
    
    import_old_file(NULL, filename);
    return 0;
}
#endif // FACES_USED_BITS

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
	
	import(NULL, filename);
	return 0;
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
		exit(EXIT_FAILURE);
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
	
	import_analysed(filename);
	return 0;
}

int print_file(int fp)
{
    Element *currElement = file_possibility[fp].file.start;
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
 * @brief Regroupe toutes les files en une seule (file 0) sans verrouillage.
 *
 * Doit être appelée avec les files déjà verrouillées.
 *
 * @return Taille totale de la file 0 après regroupement.
 */
unsigned long long regroup_datas_nolock(void)
{
	int fp;
    unsigned long long size = file_possibility[0].file.size;
	struct possibility_packet *packet = malloc(sizeof(struct possibility_packet));
	for (fp=1; fp < NB_FILE_POSSIBILITY; fp++)
	{
		
		while (file_possibility[fp].file.size > 0) {
			
			scroll(&file_possibility[fp].file,packet);
			if(packet!=NULL)
			{
				put(&file_possibility[0].file, packet);
				size++;
			}
			
		}
        
	}
    log_info("regroup size :%llu\n",size);
	free(packet);
	packet = NULL;
	return 0;
}

int regroup_datas(void)
{
	lock_all_file();
	regroup_datas_nolock();
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
	for (fp=0; fp < NB_FILE_POSSIBILITY; fp++)
	{
		Element *currElement = file_possibility[fp].file.start;
        
		while (currElement != NULL)
		{
            Element *nextElement =NULL;
			struct possibility_packet *possibility = (struct possibility_packet *)currElement->value;
			if(!possibility_all_has_a_next(possibility, mapParts, all_rotate_part))
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
int split_datas_nolock(int nbsplit)
{
	regroup_datas_nolock();
	
	File *file = malloc(sizeof(File));
	init_file_with_cache(file, 0, sizeof(struct possibility_packet));
	struct possibility_packet *possibility = malloc(sizeof(struct possibility_packet));
	while (file_possibility[0].file.size > 0)
	{
		
		if(scroll(&file_possibility[0].file, possibility))
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
		while(file_possibility[f].file.size < quotient && file->size > 0){
			if(scroll(file, possibility))
			{
				put(&file_possibility[f].file, possibility);
			}
		}
	}
	
	// si le quotient n'était pas bon on vide dans la premiere liste pour éviter la perte
	while(file->size > 0){
		if(scroll(file, possibility))
		{
			put(&file_possibility[0].file, possibility);
		}
	}
	
	free(possibility);
	free_file(file);
	
	return 0;
}

int split_datas(void)
{
	lock_all_file();
	split_datas_nolock(NB_FILE_POSSIBILITY);
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
		Element *currElement = file_possibility[fp].file.start;
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
    
    unsigned long long dataSize = datas_size();
    unsigned long long nbCombinations = count_combinations(dataSize);
    unsigned long long nbByThread = nbCombinations / nbDuplicateThread;
    log_info("qt: %llu nb combinations %llu | nb/threads: %llu\n", dataSize, nbCombinations, nbByThread);
    
    int fp = 0;
    Element *currElement = file_possibility[fp].file.start;
    unsigned long long position = 0;
    unsigned long long allocated = 0;
    unsigned long long remains = dataSize;
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
        
    for (int t = 0; t < nbDuplicateThread; t++) {
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
    int countSize[ETERN_PARTS];
    for (int i = 0; i < ETERN_PARTS; i++) {
        countSize[i] = 0;
    }
    for (fp=0; fp < NB_FILE_POSSIBILITY; fp++)
    {
        Element *currElement = file_possibility[fp].file.start;
        while (currElement != NULL)
        {
            count++;
            struct possibility_packet *possibility = (struct possibility_packet *)currElement->value;
            countSize[possibility->alloc]++;
            currElement = currElement->next;
        }
    }
    
    unlock_all_file();
    
    log_info("check_datas analyses:%i\n",count);
    for (int i = 0; i < ETERN_PARTS; i++) {
        log_info("%i : %i\n", i, countSize[i]);
        
        countSize[i] = 0;
    }
    return 0;
}

int search_min_datas(void)
{
	int result = ETERN_PARTS + 1;
	lock_all_file();
	int fp;
	for (fp=0; fp < NB_FILE_POSSIBILITY; fp++)
	{
		Element *currElement = file_possibility[fp].file.start;
		while (currElement != NULL)
		{
			struct possibility_packet *possibility = (struct possibility_packet *)currElement->value;
			if(possibility->alloc < result)
			{
				result = possibility->alloc;
			}
			currElement = currElement->next;
		}
	}
	
	unlock_all_file();
    
    // Il n'y a donc aucun élément
    if (result > ETERN_PARTS) {
        result = 0;
    }
	return result;
}

// TODO : revoir le trie pour prendre en compte le cache
int sort_ascending(void)
{
	// on bloque les files le temps du trie
	lock_all_file();
	// regroupement pour ne parcourir qu'une seule file
	regroup_datas_nolock();

	// Tableau d'éléments permettants d'avoir des repaires d'éléments classés
	// On mémorise par nb possibilités allouées.
	Element **orderedLair = malloc(sizeof(Element*) * (ETERN_PARTS +1));
	for (int l = 0; l < ETERN_PARTS +1; l++) {
		orderedLair[l] = NULL;
	}
	
	File *file = &file_possibility[0].file;
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
	unlock_all_file();
	return 0;
}

void *sort_d_mono(void *f)
{
    int intf = *(int *)f;
	log_info("sort d file:%i\n",intf);

	// Tableau d'éléments permettants d'avoir des repaires d'éléments classés
	// On mémorise par nb possibilités allouées.
	Element **orderedLair = malloc(sizeof(Element*) * (ETERN_PARTS +1));
	for (int l = 0; l < ETERN_PARTS +1; l++) {
		orderedLair[l] = NULL;
	}
	
	file_possibility_t *file_poss =&file_possibility[intf];
	File *file = &file_poss->file;
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
	log_info("end sort d file:%i\n",*(int *)f);
	return NULL;
}

int check_file(int f)
{
	int result = 0;
	
	file_possibility_t file_poss =file_possibility[f];
	File *file = &file_poss.file;
	
	if(file->size == 0)
	{
		if(file->start != NULL)
		{
			log_info("File:%i size=0 and start not null\n",f);
			result = -1;
		}
		
		if(file->end != NULL)
		{
            log_info("File:%i size=0 and end not null\n",f);
			result = -1;
		}
	}
	
	// test que la fin correspond à la taille
	unsigned long long t;
	Element *currElement = file_poss.file.start;
	Element *lastElement = currElement;
	for(t=0; t < file->size && currElement != NULL;t++)
	{
		if(currElement->value == NULL){
            log_info("File:%i value NULL\n",f);
			result = -1;
		}
		lastElement = currElement;
		currElement = currElement->next;
	}
	
	if(currElement != NULL)
	{
        log_info("File:%i last analysed element is not null | file.size:%llu analysed:%llu",f,file->size, t);
		result = -1;
	}
	if (t != file->size || lastElement != file->end) {
        log_info("File:%i end not correspond to the size:%llu analysed:%llu\n",f,file->size,t);
		result=-1;
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
 * @brief Regroupe et trie les possibilités en ordre décroissant sans verrouillage.
 *
 * Doit être appelée avec les files déjà verrouillées.
 *
 * @return 0.
 */
int sort_descending_nolock(void)
{
    log_info("regroup datas \n");
	regroup_datas_nolock();
    log_info("sort file 0\n");
	int *i = malloc(sizeof(int));
	i[0] = 0;
	sort_d_mono(&i[0]);
	free(i);
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
    
	unlock_all_file();
	return 0;
}

// TODO : revoir le trie pour prendre en compte le cache
int sort_descending(void)
{
	lock_all_file();
	
	sort_descending_nolock();
	
	unlock_all_file();
	return 0;
}

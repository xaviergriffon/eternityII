#include "etii_search.h"

#include <unistd.h>
#include <stdlib.h>

#include "logger.h"
#include "static_variables.h"
#include "etii_client.h"
#include "datamanager.h"

/**
 * @brief Délègue au serveur les possibilités excédant `max_stock_by_thread` dans une `File`.
 *
 * Extrait (`scroll`) les éléments en surplus de `db` vers un `array_possibility_packet`,
 * les pousse via `add_possibility`, puis libère le tableau temporaire.
 *
 * @param client_possibility Contexte du thread client (socket, mutex, etc.).
 * @param db                 File locale dont on contrôle la taille.
 */
void checkAndDelegatePossibilitiesIfNeeded(client_possibility_t *client_possibility, File *db) {
    if(db->size > max_stock_by_thread)
    {
        array_possibility_packet *aposs = malloc(sizeof(array_possibility_packet));
        unsigned long long remains = db->size - max_stock_by_thread;
        aposs->possibilities = malloc(sizeof(struct possibility_packet) * (max_stock_by_thread));
        aposs->size = 0;
        while(db->size > remains)
        {
            scroll(db, &aposs->possibilities[aposs->size]);
            
            aposs->size++;
        }
        // En cas d'erreur les possibilités sont remises en locale
        if(add_possibility(client_possibility, aposs))
        {
            log_error("error on add_possibility\n");
        }
        free_array_possibility_packet(aposs);
    }
}

/**
 * @brief Délègue au serveur les possibilités excédant `max_stock_by_thread` dans un `big_table`.
 *
 * Variante de `checkAndDelegatePossibilitiesIfNeeded` utilisant un `big_table`
 * au lieu d'une `File`.
 *
 * @param client_possibility Contexte du thread client.
 * @param bt                 Table de grande capacité dont on contrôle la taille.
 */
void checkAndDelegatePossibilitiesIfNeeded_with_big_table(client_possibility_t *client_possibility, big_table *bt) {
    if(bt->size > max_stock_by_thread)
    {
        array_possibility_packet *aposs = malloc(sizeof(array_possibility_packet));
        unsigned long long remains = bt->size - max_stock_by_thread;
        aposs->possibilities = malloc(sizeof(struct possibility_packet) * (max_stock_by_thread));
        aposs->size = 0;
        while(bt->size > remains)
        {
            scroll_big_table(bt, &aposs->possibilities[aposs->size]);
            
            aposs->size++;
        }
        // En cas d'erreur les possibilités sont remises en locale
        if(add_possibility(client_possibility, aposs))
        {
            log_error("error on add_possibility\n");
        }
        free_array_possibility_packet(aposs);
    }
}

/**
 * @brief Thread de recherche principale (worker de résolution du puzzle).
 *
 * Attend que `client->works == 1` puis consomme toutes les `possibility_packet`
 * du tableau `client->aposs`. Pour chaque paquet, applique `what_search_to_key2`
 * puis `search_possiblity_light_with_big_table`, et délègue le surplus au serveur
 * via `checkAndDelegatePossibilitiesIfNeeded_with_big_table` toutes les 1 000 000
 * itérations. Met à jour `max_result`, gère les états PAUSE/STOP, et renvoie
 * les possibilités non traitées au serveur avant de s'arrêter.
 *
 * @param userdata Pointeur vers un `client_possibility_t` alloué par le parent.
 * @return         NULL.
 */
void *autosearch (void *userdata)
{
    client_possibility_t *client = userdata;
    int16_t idParts[ETERN_PARTS+1][PART_SIZES];
    for(int p=0; p <= ETERN_PARTS; p++) {
        int base = p;
        for(int r=0; r < PART_SIZES; r++) {
            idParts[p][r] = base;
            base += ETERN_PARTS;
        }
    }
#ifdef DEBUG_THREAD
    log_info("START search thread %i\n", client->pid);
#endif // DEBUG_THREAD
    // Boucle infinie pour maintenir le thread
    while(1)
    {
        // Attente d'un jeu de possibilité
        while ((client->works == 0 || client->aposs == NULL) && (request == REQUEST_CONTINUE || request == REQUEST_PAUSE))
        {
            usleep(MICRO_SLEEP);
        }
        // TODO : transformer en BIG Tableau qui serait checker lors de la recherche des possiblités
        // pour voir si suffisant et dans le cas contraire allouer une marge *2
        // de toute façon, on check sa quantité pour diminué donc ne sera jamais trop gros
        // ceci permettrai de diminuer les controles sur la suite lors des put et scroll
        // on ferai plus que des memcpy
        // allocation mémoire pour la suite
        //File *db = malloc(sizeof(File));
        big_table *bt = malloc(sizeof(big_table));
        init_big_table(bt, 350, sizeof(struct possibility_packet));
        // Initialisation de la suite
        // 350 ???
        //init_file_with_cache(db, 350, sizeof(struct possibility_packet));
        //struct possibility_packet *possibilityPacketCache = malloc(sizeof(struct possibility_packet));

        // Représentation de la piece que l'on recherche dans les pieces disponbiles.
        // certaines faces ne sont pas définies
        key_part *key = malloc(sizeof(key_part));
        int a;
        // Consommation des possibilités demandées
        for(a=0; client->aposs != NULL && a < client->aposs->size;a++)
        {
            //put(db, &client->aposs->possibilities[a]);
            put_big_table(bt, &client->aposs->possibilities[a]);
            // Boucle permettant d'effectuer un controle de la consommation sans "trop" imputer la boucle suivante effectuant un controle "miminum"
            int noCheckDelegate = 0;
            while (bt->size > 0 && (request == REQUEST_CONTINUE || request == REQUEST_PAUSE))
            {
                // On poursuit tant qu'il y a du stock et qu'on a toujours l'instruction de continuer
                //while(db->size > 0 && client->request == REQUEST_CONTINUE)
                while(bt->size > 0 && request == REQUEST_CONTINUE)
                {
                    noCheckDelegate++;
                    // TODO : voir pour calculer 1/2s (vitesse/s / 2)
                    if (noCheckDelegate == 1000000) {
                        // Si trop d'étude à faire pour 1 thread, alors on délègue le reste.
                        checkAndDelegatePossibilitiesIfNeeded_with_big_table(client, bt);
                        
                        // Statistique du nombre de possiblité en étude
                        lastfilesize[client->compteur] = bt->size;
                        
                        noCheckDelegate = 0;
                    }
                    
                    // Consomation d'un cache ??
                    struct possibility_packet *possibilityPacket = scroll_big_table_cache(bt);
#ifdef DEBUG_CHECK_POSSIBILITY
                    int analyse = check_possibility(possibilityPacket, client->all_rotate_part);
                    if (analyse < 0)
                    {
                        log_error("possibility error : %i\n",analyse);
                        log_error(" ---");
                        print_possibility_packet(possibilityPacket);
                    }
#endif // DEBUG_CHECK_POSSIBILITY
                    
                    // Statistique possibilité étudiées
                    counters[client->compteur]++;
                    
                    // alimente key pour indiquer quoi chercher
                    //what_search_to_key(client->all_rotate_part, possibilityPacket, key);
                    what_search_to_key2(client->all_rotate_part, possibilityPacket, key, client->map_part->sizearrayM);
                    int max =
                    search_possiblity_light_with_big_table(bt, key, possibilityPacket, client->map_part, client->all_rotate_part, idParts);
                    
                    // Si le résultat à dépasser le plus grand qu'on a trouvé, on trace
                    if(max > max_result)
                    {
                        max_result = max;
#ifdef DEBUG_CHECK_POSSIBILITY
                        if(max_result >= ETERN_PARTS)
                        {
                            log_error("Erreur alloc > ETERN_PARTS\n");
                        }
                        log_info("max result:%i\n",max_result);
#endif // DEBUG_CHECK_POSSIBILITY
                    }
                }

                if (request == REQUEST_PAUSE)
                {
                    usleep(MICRO_SHORT_SLEEP);
                }
            }
        }
        //free(possibilityPacketCache);
        free(key);
        //if(client->request == REQUEST_STOP && db->size > 0)
        if (request == REQUEST_STOP && bt->size > 0)
        {
#ifdef DEBUG_THREAD
            log_info("thread %i stop\n", client->pid);
#endif // DEBUG_THREAD
            array_possibility_packet *aposs = malloc(sizeof(array_possibility_packet));
            aposs->possibilities = malloc(sizeof(struct possibility_packet) * (bt->size));
            aposs->size = 0;
            while(bt->size > 0)
            {
                scroll_big_table(bt, &aposs->possibilities[aposs->size]);
                aposs->size++;
            }
            // En cas d'erreur, les possibilités sont remises en locale.
            if(add_possibility(client, aposs))
            {
                log_error("Error on add_possibility \n");
            }
            free_array_possibility_packet(aposs);

            send_possibility_analysed(client);
        }
        //free_file(db);
        free_big_table(bt);
        
        // A faire tout le temps ou juste si on arrete ?
        if (client->aposs != NULL) {
            free_array_possibility_packet(client->aposs);
            client->aposs = NULL;
        }
        pthread_mutex_lock(&client->works_mutex);
        client->works = 0;
        pthread_mutex_unlock(&client->works_mutex);
        lastfilesize[client->compteur] = 0;
        
        if (request == REQUEST_STOP) {
            break;
        } else {
            usleep(MICRO_SHORT_SLEEP);
        }
    }
#ifdef DEBUG_THREAD
    log_info("END search thread %i\n", client->pid);
#endif // DEBUG_THREAD
    
    return NULL;
}

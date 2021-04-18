#include <unistd.h>
#include <stdlib.h>
#include "etii_search.h"
#include "static_variables.h"
#include "etii_client.h"
#include "datamanager.h"

/**
 * Controle et délègue les possibilités dépassant le nombre authorisé par thread.
 */
void checkAndDelegatePossibilitiesIfNeeded(client_possibility_t *client_possibility, File *db) {
    if(db->size > max_stock_by_thread)
    {
        array_possibility_packet *aposs = malloc(sizeof(array_possibility_packet));
        int reste = db->size - max_stock_by_thread;
        aposs->possibilities = malloc(sizeof(struct possibility_packet) * (max_stock_by_thread));
        aposs->size = 0;
        while(db->size > reste)
        {
            scroll(db, &aposs->possibilities[aposs->size]);
            
            aposs->size++;
        }
        // En cas d'erreur les possibilités sont remises en locale
        if(add_possibility(client_possibility, aposs))
        {
            printf("error on add_possibility\n");
        }
        free_array_possibility_packet(aposs);
        
        
    }
}

/**
 * Controle et délègue les possibilités dépassant le nombre authorisé par thread.
 */
void checkAndDelegatePossibilitiesIfNeeded_with_big_table(client_possibility_t *client_possibility, big_table *bt) {
    if(bt->size > max_stock_by_thread)
    {
        array_possibility_packet *aposs = malloc(sizeof(array_possibility_packet));
        int reste = bt->size - max_stock_by_thread;
        aposs->possibilities = malloc(sizeof(struct possibility_packet) * (max_stock_by_thread));
        aposs->size = 0;
        while(bt->size > reste)
        {
            scroll_big_table(bt, &aposs->possibilities[aposs->size]);
            
            aposs->size++;
        }
        // En cas d'erreur les possibilités sont remises en locale
        if(add_possibility(client_possibility, aposs))
        {
            printf("error on add_possibility\n");
        }
        free_array_possibility_packet(aposs);
    }
}

void *autosearch (void *userdata)
{
    client_possibility_t *client = userdata;
    
    int16_t idParts[ETERN_PARTS+1][4];
    for(int p=0; p <= ETERN_PARTS;p++) {
        for(int r=0; r < 4;r++) {
            idParts[p][r] = p + ETERN_PARTS * r;
        }
    }
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
                    int analyse = check_possibility(possibilityPacket);
                    if (analyse < 0)
                    {
                        printf("possibility error : %i\n",analyse);
                        printf(" ---");
                        print_possibility_packet(possibilityPacket);
                    }
    #endif // DEBUG_CHECK_POSSIBILITY
                    
                    // Statistique possibilité étudiées
                    compteurs[client->compteur]++;
                    
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
                            printf("Erreur alloc > ETERN_PARTS\n");
                        }
                        printf("max result:%i\n",max_result);
#endif // DEBUG_CHECK_POSSIBILITY
                    }
                }

                if (request == REQUEST_PAUSE)
                {
                    usleep(MICRO_PAUSE);
                }
            }
        }
        //free(possibilityPacketCache);
        free(key);
        //if(client->request == REQUEST_STOP && db->size > 0)
        if (request == REQUEST_STOP && bt->size > 0)
        {
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
                printf("Error on add_possibility \n");
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
        
        client->works = 0;
        lastfilesize[client->compteur] = 0;
        
        if (request == REQUEST_STOP) {
            break;
        }
    }
    
    return NULL;
}

//
//  etii_search.c
//  eternityII
//
//  Created by Xavier GRIFFON on 04/10/2020.
//  Copyright © 2020 Xavier GRIFFON. All rights reserved.
//
#include <unistd.h>
#include <stdlib.h>
#include "etii_search.h"
#include "static_variables.h"
#include "etii_client.h"
#include "datamanager.h"

/**
 * Controle et délègue les possibilités dépassant le nombre authorisé par thread.
 */
void checkAndDelegatePossibilitiesIfNeeded(File *db) {
    if(db->size > MAX_STOCK_BY_THREAD)
    {
        array_possibility_packet *aposs = malloc(sizeof(array_possibility_packet));
        int reste = db->size - MAX_STOCK_BY_THREAD;
        aposs->possibilities = malloc(sizeof(struct possibility_packet) * (MAX_STOCK_BY_THREAD));
        aposs->size = 0;
        while(db->size > reste)
        {
            scroll(db, &aposs->possibilities[aposs->size]);
            
            aposs->size++;
        }
        // En cas d'erreur on remet les possibilitées dans la file
        if(add_possibility(aposs))
        {
            printf("error on add_possibility\n");
            int p;
            for(p=0; p < aposs->size;p++)
            {
                put(db,&aposs->possibilities[p]);
            }
        }
        free(aposs->possibilities);
        free(aposs);
        
        
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
        while (client->aposs == NULL)
        {
            usleep(MICRO_SLEEP);
        }
        // allocation mémoire pour la suite
        File *db = malloc(sizeof(File));
        // Initialisation de la suite
        // 350 ???
        init_file_with_cache(db, 350, sizeof(struct possibility_packet));
        //struct possibility_packet *possibilityPacketCache = malloc(sizeof(struct possibility_packet));

        // Représentation de la piece que l'on recherche dans les pieces disponbiles.
        // certaines faces ne sont pas définies
        key_part *key = malloc(sizeof(key_part));
        int a;
        // Consommation des possibilités demandées
        for(a=0; a < client->aposs->size;a++)
        {
            put(db,&client->aposs->possibilities[a]);
            // On poursuit tant qu'il y a du stock et qu'on a toujours l'instruction de continuer
            while(db->size > 0 && client->request == REQUEST_CONTINUE)
            {
                // Si trop d'étude à faire pour 1 thread, alors on délègue le reste.
                checkAndDelegatePossibilitiesIfNeeded(db);
                
                // Statistique du nombre de possiblité en étude
                lastfilesize[client->compteur] = db->size;
                
                // Consomation d'un cache ??
                struct possibility_packet *possibilityPacket = scroll_cache(db);
#ifdef CHECK_POSSIBILITY
                int analyse = check_possibility(possibilityPacket);
                if (analyse < 0)
                {
                    printf("possibility error : %i\n",analyse);
                    printf(" ---");
                    print_possibility_packet(possibilityPacket);
                }
#endif // CHECK_POSSIBILITY
                
                // Statistique possibilité étudiées
                compteurs[client->compteur]++;
                
                // alimente key pour indiquer quoi chercher
                what_search_to_key(client->all_rotate_part, possibilityPacket, key);
                
                int max = search_possiblity_light(db, key, possibilityPacket, client->map_part, client->all_rotate_part,idParts);
                
                // Si le résultat à dépasser le plus grand qu'on a trouvé, on trace
                if(max > max_result)
                {
                    max_result = max;
                    if(max_result >= ETERN_PARTS)
                    {
                        printf("Erreur alloc > ETERN_PARTS\n");
                    }
                    printf("max result:%i\n",max_result);
                }
            }
        }
        //free(possibilityPacketCache);
        free(key);
        if(client->request == REQUEST_STOP && db->size > 0)
        {
            array_possibility_packet *aposs = malloc(sizeof(array_possibility_packet));
            aposs->possibilities = malloc(sizeof(struct possibility_packet) * (db->size));
            aposs->size = 0;
            while(db->size > 0)
            {
                scroll(db, &aposs->possibilities[aposs->size]);
                
                aposs->size++;
            }
            if(add_possibility(aposs))
            {
                printf("Error with possibility : \n");
                int p;
                for (p=0;p < aposs->size;p++)
                {
                    struct possibility_packet *possibility = &aposs->possibilities[p];
                    print_possibility_packet(possibility);
                    save_possibility("./error_possibility",possibility);
                }
                
            }
            free(aposs->possibilities);
        }
        free_file(db);
        
        if(client->aposs->size > 0)
        {
            free(client->aposs->possibilities);
        }
        free(client->aposs);
        client->aposs = NULL;
        client->works = 0;
    }
    
    return NULL;
}

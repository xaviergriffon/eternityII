#include "etii_server.h"
#define closesocket(s) close(s)

#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <errno.h>
#include <time.h>

#include "static_variables.h"
#include "etii_protocol.h"
#include "datamanager.h"
#include "possibility.h"
#include "part.h"
#include "readdata.h"
#include "tcpserver.h"

typedef struct
{
    int exist;
    pthread_t *tid;
    int socket_id;
    map_big_array *map_part;
    int compteur;
    struct tms start_socket;
} client_t;

client_t *thread_params = NULL;

int get_active_threads(client_t *thread_params) {
    int activeThread = 0;
    if (thread_params != NULL) {
        for (int t = 0; t < NB_THREADS; t++) {
            if (thread_params[t].socket_id != -1) {
                activeThread++;
            }
        }
    }
    
    return activeThread;
}
void *check_server(void *param)
{
    unsigned long long lastactive = 0;
    int sleep_time = 10;
    int lastBack = 0;
    while(1)
    {
        free(lastcheck);
        lastcheck = calloc(2000, sizeof(char));
        unsigned long long currentactive = lastactive;
        int c;
        lastactive = 0;
        for(c=0; c < NB_THREADS;c++)
        {
            lastactive = lastactive + compteurs[c];
        }
        currentactive = lastactive - currentactive;
        getted_possibility_not_null = lastactive;
        
        unsigned long long file_possibility_stock = 0;
        int f;
        for(f=0; f < NB_FILE_POSSIBILITY; f++)
        {
            unsigned long long f_size = file_size(f);
            char *temp = calloc(1000, sizeof(char));
            sprintf(temp, "file:%i stock:%llu\n",f,f_size);
            strcat(lastcheck, temp);
            free(temp);
            file_possibility_stock = file_possibility_stock + f_size;
        }

        unsigned long long file_possibility_analysed_stock = 0;
        for(f=0; f < NB_FILE_POSSIBILITY; f++)
        {
            unsigned long long f_size = file_analysed_size(f);
            char *temp = calloc(1000, sizeof(char));
            sprintf(temp, "file_analysed:%i stock:%llu\n",f,f_size);
            strcat(lastcheck, temp);
            free(temp);
            file_possibility_analysed_stock = file_possibility_analysed_stock + f_size;
        }
        unsigned long long bys = currentactive / sleep_time;

        int activeThread = get_active_threads(thread_params);

        char *temp = calloc(1000, sizeof(char));
        sprintf(temp, "active thread last %isec :%lli\nactive thread/s :%lli\npossibility in stock :%lli\ngetted possibility not null :%lli\nmax result on server :%i\nactive Thread :%i\n",sleep_time,currentactive, bys,file_possibility_stock,getted_possibility_not_null, max_result, activeThread);
        strcat(lastcheck, temp);
        free(temp);
        
        if(lastBack == 6)
        {
            backup("./temp.back");
            backup_analysed("./temp_analysed.back");
            lastBack = 0;
        } else
        {
            lastBack++;
        }
        sleep(sleep_time);
    }
    
    return NULL;
}

void *communicate_with_client (void *userdata)
{
    client_t *client = userdata;
    while (client->socket_id == -1)
    {
        usleep(MICRO_SLEEP);
    }
    
    compteurs[client->compteur]++;
    
    int8_t instruction = recv_instruction(client->socket_id);
    
    array_possibility_packet *lastPossibilityPacketSend = NULL;
    int version_supported = 0;
    while(instruction != -1 && instruction != INST_END)
    {
        if (instruction == INST_CHECK_VERSION) {
            int client_version = -1;
            ssize_t ssize = recv(client->socket_id, &client_version, sizeof(int), 0);
            if (ssize != sizeof(int)) {
                printf("error on recept client version\n");
                break;
            }
            if (version == client_version) {
                send_instruction(client->socket_id, INST_SUPPORTED_VERSION);
                version_supported = 1;
            } else {
                printf("Version of client unsupported\n");
                send_instruction(client->socket_id, INST_UNSUPPORTED_VERSION);
            }
        } else if(instruction == INST_GET && version_supported == 1)
        {
            if(lastPossibilityPacketSend != NULL)
            {
                free_array_possibility_packet(lastPossibilityPacketSend);
                lastPossibilityPacketSend = NULL;
            }
            
            lastPossibilityPacketSend = get_last_possibility(NULL, 1);
            int p = 0;
            for (p = 0; p < lastPossibilityPacketSend->size; p++)
            {
                struct possibility_packet *possibility = &lastPossibilityPacketSend->possibilities[p];
                add_possibility_analysed(possibility, -1);
                //printf("send ");
                //print_possibility_packet(possibility);
                ssize_t ssize = send(client->socket_id, (struct possibility_packet *)possibility, sizeof(struct possibility_packet),0);
                if (ssize < 0) {
                    printf("erreur d'envoi %i\n", errno);
                }
            }
            if(p == 0)
            {
                send_instruction(client->socket_id,INST_NULL);
            }
            
        } else if(instruction == INST_ADD && version_supported == 1)
        {
            array_possibility_packet *aposs = malloc(sizeof(array_possibility_packet));
            struct possibility_packet *possibilityPacket = malloc(sizeof(struct possibility_packet));
            long receive = recv(client->socket_id, (struct possibility_packet *)possibilityPacket, sizeof(struct possibility_packet),0);
            if(sizeof(struct possibility_packet) == receive)
            {
                aposs->possibilities = possibilityPacket;
                aposs->size = 1;
                
                if(add_possibility(NULL, aposs) == 0)
                {
                    send_instruction(client->socket_id, INST_CONSIDERED);
                    
                } else{
                    send_instruction(client->socket_id, INST_ERROR);
                }
            } else{
                printf("bad possibility recept");
                if (receive < 0) {
                    printf(" : %i", errno);
                }
                printf("\n");
                send_instruction(client->socket_id, INST_ERROR);
            }
            free(possibilityPacket);
            free(aposs);
            
            
        } else if (instruction == INST_POSSIBILITY_ANALYSED && version_supported == 1) {
            struct possibility_packet *possibilityPacket = malloc(sizeof(struct possibility_packet));
            ssize_t ssize = recv(client->socket_id, (struct possibility_packet *)possibilityPacket, sizeof(struct possibility_packet), 0);
            if (sizeof(struct possibility_packet) == ssize) {
                if(remove_possibility_analysed(possibilityPacket, -1) == 0)
                {
                    send_instruction(client->socket_id,INST_CONSIDERED);
                    
                } else{
                    printf("possibility analysed not removed\n");
                    print_possibility_packet(possibilityPacket);
                    send_instruction(client->socket_id,INST_ERROR);
                }
            } else {
                printf("bad possibility recept");
                if (ssize < 0) {
                    printf(" : %i", errno);
                }
                printf("\n");
                send_instruction(client->socket_id, INST_ERROR);
            }
            free (possibilityPacket);
        } else if (instruction == INST_TEST_CONNECTED) {
            send_instruction(client->socket_id, INST_TEST_CONNECTED);
        } else if (version_supported == 0) {
            printf("Version of client unsupported\n");
            send_instruction(client->socket_id, INST_UNSUPPORTED_VERSION);
            break;
        } else
        {
            inst_unknow++;
            printf("server instruction inconnu: %i\n",instruction);
            printf("nb inst inconnu%li\n",inst_unknow);
            
            break;
        }
        
        instruction = recv_instruction(client->socket_id);
    }
    if(lastPossibilityPacketSend != NULL)
    {
        if (instruction == -1 || instruction != INST_END) {
            if(add_possibility(NULL, lastPossibilityPacketSend))
            {
                printf("Error with possibility : \n");
                int p;
                for (p=0;p < lastPossibilityPacketSend->size;p++)
                {
                    struct possibility_packet *possibility = &lastPossibilityPacketSend->possibilities[p];
                    print_possibility_packet(possibility);
                    save_possibility("./error_possibility",possibility);
                }
                
            }
        }
        
        free_array_possibility_packet(lastPossibilityPacketSend);
    }
    
    shutdown(client->socket_id, 2);
    int err = closesocket(client->socket_id);
#ifdef DEBUG_SOCKET
    opened_tcp--;
#endif // DEBUG_SOCKET
    if(0 != err)
    {
        printf("erreur close :%i\n",err);
    }
    
    usleep(THREAD_MICRO_SLEEP);
    
    client->socket_id = -1;

    client->exist =0;
    
    return NULL;
}

void create_server_thread(client_t *thread_params, int i) {
    client_t clientt = thread_params[i];
    /* création d'un nouveau thread */
    if(clientt.tid != NULL)
    {
        if (thread_params[i].tid != NULL) {
            free(thread_params[i].tid);
        }
        thread_params[i].tid = NULL;
    }
    thread_params[i].socket_id = -1;
    
    
    pthread_attr_t *thread_attributes = malloc(sizeof *thread_attributes);
    pthread_attr_init(thread_attributes);
    pthread_attr_setdetachstate(thread_attributes, PTHREAD_CREATE_DETACHED);
    /* Création du thread */
    thread_params[i].tid = malloc(sizeof(pthread_t));
    if(0 != pthread_create((thread_params[i].tid), thread_attributes, communicate_with_client, &(thread_params[i])))
    {
        fprintf(stderr, "Problème avec pthread_create()\n");
        free(thread_attributes);
        exit(EXIT_FAILURE);
    }
    pthread_attr_destroy(thread_attributes);
    free(thread_attributes);

    thread_params[i].exist = 1;
}

void *rmnonext_thread(void *param) {
    struct array_part *apart= read_parts(partsFiles);
    struct array_part *rotateParts = rotate_all_parts(apart);
    map_big_array *map_parts = prepare_map_part(rotateParts);
    while(request != REQUEST_STOP) {
        if (get_active_threads(thread_params) <= 0) {
#ifdef DEBUG_RM_NO_NEXT
            printf("Auto rmnonext\n");
#endif // DEBUG_RM_NO_NEXT
            remove_possibilities_with_no_next(map_parts, rotateParts);
#ifdef DEBUG_RM_NO_NEXT
        } else {
            
            printf("No auto rmnonext because thread active\n");
#endif // DEBUG_RM_NO_NEXT
        }
        sleep(server_rmnonext_timing);
    }
    free_bigarray(map_parts);
    free_array_part(rotateParts);
    free_array_part(apart);
    
    return NULL;
}

void create_rmnonext_thread(void) {
    pthread_attr_t *thread_attributes = malloc(sizeof *thread_attributes);
    pthread_attr_init(thread_attributes);
    pthread_attr_setdetachstate(thread_attributes, PTHREAD_CREATE_DETACHED);
    /* Création du thread */
    pthread_t thread;
    if(0 != pthread_create(&thread, thread_attributes, rmnonext_thread, NULL))
    {
        fprintf(stderr, "create_rmnonext_thread : Problème avec pthread_create()\n");
        free(thread_attributes);
        exit(EXIT_FAILURE);
    }
    pthread_attr_destroy(thread_attributes);
    free(thread_attributes);
}

void runserver(const char* file)
{
    struct array_part *apart= read_parts(file);
    struct array_part *rotateParts = rotate_all_parts(apart);
    map_big_array *map_parts = prepare_map_part(rotateParts);
    free_array_part(apart);
    first_possibility(map_parts, rotateParts);
    free_bigarray(map_parts);
    free_array_part(rotateParts);
    
    // Demarrage d'un thread de nettoyage des possibilités sans suite
    create_rmnonext_thread();
    
    /* création du tableau de structures client_t avec un élément par thread */
    if(NULL == (thread_params = malloc(sizeof(*thread_params) * NB_THREADS)))
    {
        fprintf(stderr, "Problème avec malloc()\n");
        exit(EXIT_FAILURE);
    }
    for(int i = 0; i < NB_THREADS; i++)
    {
        thread_params[i].exist = 0;
        thread_params[i].socket_id = -1;
        thread_params[i].tid = NULL;
        thread_params[i].compteur = i;
    }
    
    
    int socket_id = create_tcp_server(SERVER_PORT, NB_THREADS);
    long acceptclient = 0;
    while (request != REQUEST_STOP) {
        int client_id;
        int thread_id;

        if((client_id = accept(socket_id, NULL, 0)) < 0)
        {
            if (errno == EDEADLK || errno == EDEADLK || errno == EWOULDBLOCK) {
                printf("resource blocked, try again\n");
                continue;
            } else {
                fprintf(stderr, "Erreur sur accept() : %i\n", errno);
                exit(EXIT_FAILURE);
            }
        }
        acceptclient++;
        struct timeval tv;
        tv.tv_sec = tcp_timeout;
        tv.tv_usec = 0;
        // Timeout sur les sessions
        setsockopt(client_id, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(struct timeval));
        setsockopt(client_id, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(struct timeval));
        
        thread_id = -1;
        while (thread_id == -1) {
            /* recherche d'un thread libre */
            int t = 0;
            for(t = 0; t < NB_THREADS; t++)
            {
                client_t clientt = thread_params[t];
                if(clientt.exist != 0 && clientt.socket_id == -1)
                {
                    thread_id = t;
                    thread_params[t].socket_id = client_id;
                    times(&thread_params[t].start_socket);
                    break;
                }
            }
            
            int nbCreated = 0;
            // A chaque affectation, on vérifie les threads pour en regéréner 1 si besoin
            // et affecter directement le client si il ne l'a pas été.
            for(int i = 0; i < NB_THREADS; i++)
            {
                client_t clientt = thread_params[i];
                if(clientt.exist == 0)
                {
                    create_server_thread(thread_params, i);
                    if (thread_id == -1) {
                        thread_id = i;
                        thread_params[i].socket_id = client_id;
                        times(&thread_params[i].start_socket);
                    }
                    nbCreated++;
                    break;
                }
            }
            if (thread_id == -1 && nbCreated == 0) {
                printf("all thread used\n");
            }
        }
    }
}

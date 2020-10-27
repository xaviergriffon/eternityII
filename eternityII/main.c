#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#ifdef WIN32
#include <winsock2.h>
#define sleep(s) Sleep(s*1000)
#include <windows.h>

void usleep(int waitTime) {
	__int64 time1 = 0, time2 = 0, freq = 0;
	
	QueryPerformanceCounter((LARGE_INTEGER *)&time1);
	QueryPerformanceFrequency((LARGE_INTEGER *)&freq);
	
	do {
		QueryPerformanceCounter((LARGE_INTEGER *)&time2);
	} while ((time2 - time1) < waitTime);
}
#else
#include <sys/times.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h> /* close */
#include <netdb.h> /* gethostbyname */
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define closesocket(s) close(s)
typedef int SOCKET;
typedef struct sockaddr_in SOCKADDR_IN;
typedef struct sockaddr SOCKADDR;
typedef struct in_addr IN_ADDR;
#endif

#include "static_variables.h"
#include "console.h"
#include "possibility.h"

#include "datamanager.h"
#include "tcpserver.h"
#include "tcpclient.h"
#include "part.h"
#include "lifo.h"
#include "etii_protocol.h"
#include "readdata.h"
#include "etii_client.h"
#include "etii_search.h"

pthread_mutex_t max_lock;
pthread_mutex_t build_cl_instance;

static long inst_unknow = 0;


int NB_THREADS = 10;
int request = REQUEST_CONTINUE;

typedef struct
{
	int exist;
	pthread_t *tid;
	int socket_id;
	map_big_array *map_part;
	int compteur;
} client_t;


static void *check_server()
{
	unsigned long long lastactive = 0;
	int sleep_time = 10;
	int lastBack = 0;
	while(1)
	{
		free(lastcheck);
		lastcheck = calloc(1000, sizeof(char));
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
			int f_size = file_size(f);
			char *temp = calloc(1000, sizeof(char));
			sprintf(temp, "file:%i stock:%i\n",f,f_size);
			strcat(lastcheck, temp);
			free(temp);
			file_possibility_stock = file_possibility_stock + f_size;
		}
		unsigned long long bys = currentactive / sleep_time;
		char *temp = calloc(1000, sizeof(char));
		sprintf(temp, "active thread last %isec :%lli\nactive thread/s :%lli\npossibility in stock :%lli\ngetted possibility not null :%lli\nmax result on server :%i\n",sleep_time,currentactive, bys,file_possibility_stock,getted_possibility_not_null, max_result);
		strcat(lastcheck, temp);
		free(temp);
		
		if(lastBack == 6)
		{
			backup("./temp.back");
			lastBack = 0;
		} else
		{
			lastBack++;
		}
		sleep(sleep_time);
	}
	
	return NULL;
}

static void *check_client_threads()
{
	unsigned long long lastactive = 0;
	int sleep_time = 10;
	while(1)
	{
		free(lastcheck);
		lastcheck = calloc(1000, sizeof(char));
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
			int f_size = file_size(f);
			char *temp = calloc(1000, sizeof(char));
			sprintf(temp, "file:%i stock:%i\n",f,f_size);
			strcat(lastcheck, temp);
			free(temp);
			file_possibility_stock = file_possibility_stock + f_size;
		}
		for(f=0; f < NB_THREADS; f++)
		{
			char *temp = calloc(1000, sizeof(char));
			sprintf(temp, "Thread %i file size:%i\n",f,lastfilesize[f]);
			strcat(lastcheck, temp);
			free(temp);
		}
		unsigned long long bys = currentactive / sleep_time;
		char *temp = calloc(1000, sizeof(char));
		sprintf(temp, "active thread last %isec :%lli\nactive thread/s :%lli\npossibility in stock :%lli\ngetted possibility not null :%lli\nmax result :%i\n",sleep_time,currentactive, bys,file_possibility_stock,getted_possibility_not_null, max_result);
		strcat(lastcheck, temp);
		free(temp);
		
		sleep(sleep_time);
	}
	
	return NULL;
}

void *client (void *userdata)
{
	client_t *client = userdata;
	
	while (client->socket_id == -1)
	{
		usleep(MICRO_SLEEP);
	}
    
	compteurs[client->compteur]++;
	
	int8_t instruction = recv_instruction(client->socket_id);
	
	array_possibility_packet *lastPossibilityPacketSend = NULL;
	while(instruction != -1 && instruction != INST_END)
	{
		if(instruction == INST_GET)
		{
			if(lastPossibilityPacketSend != NULL)
			{
				free(lastPossibilityPacketSend->possibilities);
				free(lastPossibilityPacketSend);
				lastPossibilityPacketSend = NULL;
			}
			// TODO : a revoir pour demander au client
			int p=1;
			lastPossibilityPacketSend = get_last_possibility(p);
			for (p=0;p < lastPossibilityPacketSend->size;p++)
			{
				struct possibility_packet *possibility = &lastPossibilityPacketSend->possibilities[p];
				//printf("send ");
				//print_possibility_packet(possibility);
				send(client->socket_id, (struct possibility_packet *)possibility, sizeof(struct possibility_packet),0);
			}
			if(p == 0)
			{
				send_instruction(client->socket_id,INST_NULL);
			}
            
		} else if(instruction == INST_ADD)
		{
			array_possibility_packet *aposs = malloc(sizeof(array_possibility_packet));
			struct possibility_packet *possibilityPacket = malloc(sizeof(struct possibility_packet));
			if(sizeof(struct possibility_packet) == recv(client->socket_id, (struct possibility_packet *)possibilityPacket, sizeof(struct possibility_packet),0))
			{
				aposs->possibilities = possibilityPacket;
				aposs->size = 1;
                
				if(add_possibility(aposs) == 0)
				{
					send_instruction(client->socket_id,INST_CONSIDERED);
					
				} else{
					send_instruction(client->socket_id,INST_ERROR);
				}
			} else{
				printf("bad possibility recept\n");
			}
			free(possibilityPacket);
			free(aposs);
			
			
		} else
		{
			inst_unknow++;
			printf("server instruction inconnu: %i\n",instruction);
			printf("nb inst inconnu%li\n",inst_unknow);
			
			break;
		}
		
		instruction = recv_instruction(client->socket_id);
	}
	if((instruction == -1 || instruction != INST_END) && lastPossibilityPacketSend != NULL)
	{
		if(add_possibility(lastPossibilityPacketSend))
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
		free(lastPossibilityPacketSend->possibilities);
		free(lastPossibilityPacketSend);
	}
	
	shutdown(client->socket_id, 2);
	int err = closesocket(client->socket_id);
	if(0 != err)
	{
		printf("erreur close :%i\n",err);
	}
	
	usleep(THREAD_MICRO_SLEEP);
	client->exist =0;
	client->socket_id = -1;
	
	//printf("fin du thread\n");
	
	return NULL;
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
	
	int socket_id;
	client_t *thread_params;
	
	int i;
	
	/* création du tableau de structures client_t avec un élément par thread */
	if(NULL == (thread_params = malloc(sizeof(*thread_params) * NB_THREADS)))
	{
		fprintf(stderr, "Problème avec malloc()\n");
		exit(EXIT_FAILURE);
	}
	for(i = 0; i < NB_THREADS; i++)
	{
		thread_params[i].exist = 0;
		thread_params[i].socket_id = -1;
		thread_params[i].tid = NULL;
		thread_params[i].compteur = i;
	}
    
	
	socket_id = create_tcp_server(2000, NB_THREADS);
	long acceptclient = 0;
	while (1) {
		int client_id;
		int thread_id;

		if(-1 == (client_id = accept(socket_id, NULL, 0)))
		{
			fprintf(stderr, "Erreur sur accept() : %i\n",client_id);
			exit(EXIT_FAILURE);
		}
        acceptclient++;
        
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
					break;
				}
			}
			
			if(NB_THREADS == t)
			{
				for(i = 0; i < NB_THREADS; i++)
				{
					client_t clientt = thread_params[i];
					if(clientt.exist == 0)
					{
						/* création d'un nouveau thread */
						//thread_id = i;
						if(clientt.tid != NULL)
						{
							free(thread_params[i].tid);
							thread_params[i].tid = NULL;
						}
						thread_params[i].socket_id = -1;
						thread_params[i].exist = 1;
						//printf("Thread %d\n",i);
						
						pthread_attr_t *thread_attributes = malloc(sizeof *thread_attributes);
						pthread_attr_init(thread_attributes);
						pthread_attr_setdetachstate(thread_attributes, PTHREAD_CREATE_DETACHED);
						/* Création du thread */
						thread_params[i].tid = malloc(sizeof(pthread_t));
						if(0 != pthread_create((thread_params[i].tid), thread_attributes, client, &(thread_params[i])))
						{
							fprintf(stderr, "Problème avec pthread_create()\n");
							free(thread_attributes);
							exit(EXIT_FAILURE);
						}
						pthread_attr_destroy(thread_attributes);
						break;
					}
				}
			}
		}
	}
	//exit(EXIT_SUCCESS);
}

void runclient(const char *hostname, const char *file)
{
	// On indique au manager de passer par un serveur
	set_server_ip(hostname);
	
	runThreadClient(file);
	
	printf("sortie boucle while 1\n");
	exit(EXIT_SUCCESS);
}

void runauto(const char *file)
{
	struct array_part *apart= read_parts(file);
	struct array_part *rotateParts = rotate_all_parts(apart);
	// On prépare les premières possiblitées en local
	map_big_array *map_parts = prepare_map_part(rotateParts);
	first_possibility(map_parts, rotateParts);
	free_bigarray(map_parts);
	free_array_part(rotateParts);
	free_array_part(apart);
	
	runThreadClient(file);
	
	printf("sortie boucle while 1\n");
	exit(EXIT_SUCCESS);
}

/********************/




void failed_arg()
{
	printf("Indiquer parametre suivant :\ntcpserver [number of threads] [pieces.csv]\ntcpclient [serveur] [number of threads] [pieces.csv]\n");
}

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
		fprintf(stderr, "Problème avec pthread_create()\n");
		free(thread_attributes);
		exit(EXIT_FAILURE);
	}
	pthread_attr_destroy(thread_attributes);
	free(thread_attributes);
	return 0;
}

int init_compteurs()
{
    printf("allocation mémoire %i pour compteurs et lastfilesize\n", NB_THREADS);
	compteurs = malloc(sizeof(unsigned long long) * NB_THREADS);
	lastfilesize = malloc(sizeof(int) * NB_THREADS);
    printf("fin allocation compteurs et lastfilesize\n");
	
	int c;
	for(c=0; c < NB_THREADS;c++)
	{
		compteurs[c] = 0;
		lastfilesize[c] = 0;
	}
	return 0;
}



int main(int argc, const char * argv[])
{
	if (argc >= 2) {
		lastcheck = calloc(1000, sizeof(char));
		
		if(strcmp("tcpclient", argv[1]) == 0) {
			printf("client\n");
			char *serverIp = "localhost";
			if(argc >= 3){
				serverIp = (char *)argv[2];
			}
			if(argc >= 4) {
				printf("arg 3 : %s",argv[3]);
				NB_THREADS = atoi(argv[3]);
			}
			printf("Nb threads : %i\n",NB_THREADS);
			init_compteurs();
			run_checker(0);
			run_console(0);
            partsFiles = (char *)(argv[4]);
			runclient(serverIp, argv[4]);
		} else if (strcmp("tcpserver", argv[1]) == 0) {
			printf("server\n");
			if(argc >= 3) {
				printf("arg 2 : %s",argv[2]);
				NB_THREADS = atoi(argv[2]);
			}
			printf("Nb threads : %i\n",NB_THREADS);
			init_compteurs();
			run_checker(1);
			run_console(1);
            partsFiles = (char *)(argv[3]);
			runserver(argv[3]);
		} else if(strcmp("test", argv[1])==0) {
			if(argc >= 3) {
				printf("arg 2 : %s\n",argv[2]);
				NB_THREADS = atoi(argv[2]);
			}
			init_compteurs();
			run_checker(0);
			run_console(0);
            runauto(argv[3]);
		} else {
			failed_arg();
		}
        
	} else {
		failed_arg();
	}
}


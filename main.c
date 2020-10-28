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
#include "etii_server.h"

void runclient(const char *hostname, const char *file)
{
	// On indique au manager de passer par un serveur
	set_server_ip(hostname);
	
    runMonoClient(file);
	
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
	
	runMonoClient(file);
}


void failed_arg()
{
	printf("Indiquer parametre suivant :\ntcpserver [nombre de threads] [pieces.csv]\ntcpclient [serveur] [pieces.csv]\n");
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
            NB_THREADS = 1;
			char *serverIp = "localhost";
			if(argc >= 3){
				serverIp = (char *)argv[2];
			}

			init_compteurs();
			run_checker(0);
			run_console(0);
            partsFiles = (char *)(argv[3]);
			runclient(serverIp, partsFiles);
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
            NB_THREADS = 1;
			
			init_compteurs();
			run_checker(0);
			run_console(0);
            runauto(argv[3]);
		} else {
			failed_arg();
            exit(EXIT_FAILURE);
		}
        
	} else {
		failed_arg();
        exit(EXIT_FAILURE);
	}
    
    exit(EXIT_SUCCESS);
}


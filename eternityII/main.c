#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

//#include <readline/readline.h>
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
#include "possibility.h"

#include "datamanager.h"
#include "tcpserver.h"
#include "tcpclient.h"
#include "part.h"
#include "lifo.h"
#include "etii_protocol.h"
//#include "etii_opencl.h"
#include "readdata.h"
#include "etii_client.h"
#include "etii_search.h"


#define EXIT_CMD "exit"



pthread_mutex_t max_lock;
pthread_mutex_t build_cl_instance;

static long inst_unknow = 0;

static char *lastcheck = NULL;

// Partagées en extern dans static_variables
unsigned long long *compteurs = NULL;
int *lastfilesize = NULL;
int NB_THREADS = 10;
int request = REQUEST_CONTINUE;
uint16_t max_result = 0;

static char* partsFiles = NULL;

typedef struct
{
	int exist;
	pthread_t *tid;
	int socket_id;
	map_big_array *map_part;
	int compteur;
} client_t;



void first_possibility(map_big_array *mapParts, struct array_part *all_rotate_part);

static unsigned long long getted_possibility_not_null = 0;

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
	first_possibility(map_parts,rotateParts);
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
	exit(EXIT_SUCCESS);
}

/*
void *searchOpenCL (void *userdata)
{
	client_possibility_t *client = userdata;
	int nbPossMax = 1;
	pthread_mutex_lock(&build_cl_instance);
	etii_cl_instance *instance = create_etii_cl_instance(CL_DEVICE_TYPE_GPU, client->map_part,nbPossMax, client->all_rotate_part);
	pthread_mutex_unlock(&build_cl_instance);
	
	while(1)
	{
		while (client->aposs == NULL)
		{
			usleep(MICRO_SLEEP);
		}
		File *db = malloc(sizeof(File));
		init_file_with_cache(db, MAX_STOCK_BY_THREAD, sizeof(struct possibility_packet));
		struct possibility_packet *possibilityPacket = malloc(sizeof(struct possibility_packet));
		struct possibility_packet *posss = malloc(sizeof(struct possibility_packet) * nbPossMax);
		int a;
		for(a=0; a < client->aposs->size;a++)
		{
			put(db,&client->aposs->possibilities[a]);
			while(db->size > 0 && client->request == 0)
			{
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
				lastfilesize[client->compteur] = db->size;
				
                int nbSearch = 0;
                while(db->size > 0 && nbSearch < nbPossMax)
                {
					scroll(db,&posss[nbSearch]);
                    nbSearch++;
                    compteurs[client->compteur]++;
                    
                }
				
				//File *possibilities = search_possiblity_opencl(instance,posss,nbSearch,client->map_part,client->all_rotate_part);
				File *possibilities = search_possiblity_light_opencl(instance,posss,nbSearch);
				
				while(possibilities->size > 0)
				{
					struct possibility_packet *possibility = malloc(sizeof(struct possibility_packet));
					scroll(possibilities,possibility);
					
					//					pthread_mutex_lock(&max_lock);
					if(possibility->alloc > max_result)
					{
						
						max_result = possibility->alloc;
						printf("max result:%i\n",max_result);
					}
					//					pthread_mutex_unlock(&max_lock);
					put(db,possibility);
					
					free(possibility);
				}
				
				free_file(possibilities);
			}
		}
		free(posss);
		free(possibilityPacket);
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
		client->works =0;
	}
	
	free_etii_cl_instance(instance);
	return NULL;
}
 */

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

struct part* part_139_i8(map_big_array *mapParts)
{
	key_part key = {2,15,15,3};
	struct part *part = get_one_part(mapParts, key);
	if(part == NULL)
	{
		printf("part 139 not found\n");
		exit(EXIT_FAILURE);
	}
    return part;
}

void first_possibility(map_big_array *mapParts, struct array_part *all_rotate_part)
{
	struct part *etern[ETERN_SIZE][ETERN_SIZE];
    int x;
	int y;
	// initialisation
    for (x = 0; x < ETERN_SIZE; x++) {
        for(y=0; y < ETERN_SIZE; y++)
        {
            etern[x][y] = NULL;
        }
    }
	
	x = 7;
	y = 8;
	int cur_dir = DIR_UP;
    
    struct part *part = part_139_i8(mapParts);
	if(part != NULL) {
		etern[x][y] = part;
		
		// 208 C3 -- rotation 3
		// 1 13 12 3
		key_part k208 = {13,12,3,1};
		part = get_one_part(mapParts, k208);
		if(part == NULL)
		{
			printf("part 208 r3 not found\n");
			exit(EXIT_FAILURE);
		}
		etern[2][2] = part;
		
		// 255 C14 -- rotation 3
		// 7 13 11 13
		key_part k255 = {13,11,13,7};
		part = get_one_part(mapParts, k255);
		if(part == NULL)
		{
			printf("part 255 r3 not found\n");
			exit(EXIT_FAILURE);
		}
		etern[13][2] = part;
		
		// 181 N3-- rotation 3
		// 3 7 15 5
		key_part k181 = {7,15,5,3};
		part = get_one_part(mapParts, k181);
		if(part == NULL)
		{
			printf("part 181 r3 not found\n");
			exit(EXIT_FAILURE);
		}
		etern[2][13] = part;
		
		// 249 N14 -- rotation 0
		// 8 5 9 10
		key_part k249 = {8,5,9,10};
		part = get_one_part(mapParts, k249);
		if(part == NULL)
		{
			printf("part 249 r0 not found\n");
			exit(EXIT_FAILURE);
		}
		etern[13][13] = part;
		
		// on commence vers le haut
		// et sur l'angle en bas à droite
		x = ETERN_SIZE -1;
		y = ETERN_SIZE -1;
		
	} else
	{
		cur_dir = DIR_LEFT;
	}
	
	
	
	array_possibility_packet *aposs = malloc(sizeof(array_possibility_packet));
	aposs->size = 1;
	aposs->possibilities = generate_possibility_packet(x, y, etern, cur_dir);
	getted_possibility_not_null++;
	
	
	File *possibilities = malloc(sizeof(File));
	init_file_with_cache(possibilities, 0, sizeof(struct possibility_packet));
	search_possiblity(possibilities, &aposs->possibilities[0], mapParts, all_rotate_part);
	while (possibilities->size > 0) {
		struct possibility_packet *packet = malloc(sizeof(struct possibility_packet));
		scroll(possibilities,packet);
		printf("packet->alloc:%i",packet->alloc);
		if(packet->alloc > max_result)
		{
			max_result = packet->alloc;
			if(max_result >= ETERN_PARTS)
			{
				printf("Erreur alloc > ETERN_PARTS\n");
			}
			printf("max result:%i\n",max_result);
		}
		array_possibility_packet *aposs2 = malloc(sizeof(array_possibility_packet));
		aposs2->size = 1;
		aposs2->possibilities = packet;
		if(add_possibility(aposs2))
		{
			printf("error on add_possibility\n");
			exit(EXIT_FAILURE);
		}
		getted_possibility_not_null++;
		free(aposs2->possibilities);
		free(aposs2);
	}
	free_file(possibilities);
	
	free(aposs->possibilities);
	free(aposs);
    
}

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

static char * getcmdline() {
	char * line = malloc(100), *linep = line;
	size_t lenmax = 100, len = lenmax;
	int c;
	
	if (line == NULL)
		return NULL;
	
	for (;;) {
		c = fgetc(stdin);
		if (c == EOF || c == '\n')
			break;
		
		if (--len == 0) {
			len = lenmax;
			char * linen = realloc(linep, lenmax *= 2);
			
			if (linen == NULL) {
				free(linep);
				return NULL;
			}
			line = linen + (line - linep);
			linep = linen;
		}
		
		if ((*line++ = c) == '\n')
			break;
	}
	*line = '\0';
	return linep;
}
static void * console(void *param)
{
	int server = *(int *)param;
	char *def_file = "./eternityII.back";
	char *buffer = NULL;
	while(buffer == NULL)
	{
		printf("commande :");
		buffer = getcmdline();
		printf("\n");
		if(strcmp(buffer, EXIT_CMD) == 0)
		{
			if(server == 0)
			{
				request = REQUEST_STOP;
				free(buffer);
				buffer= NULL;
				while (1)
				{
					printf("*");
					usleep(MICRO_SLEEP);
				}
				
			} else
			{
				break;
			}
		}
		if(strcmp(buffer, "check") == 0)
		{
			printf("%s\n",lastcheck);
		}
		if(strcmp(buffer, "backup") == 0)
		{
			printf("start backup\n");
			backup(def_file);
			printf("backup ended\n");
		}
		if(strcmp(buffer, "restore") == 0)
		{
			printf("start restore\n");
			restore(def_file);
			printf("backup restore\n");
		}
		if(strcmp(buffer, "import") == 0)
		{
			printf("start import\n");
			import(def_file);
			printf("backup restore\n");
		}
		if(strcmp(buffer, "print") == 0)
		{
			printdatamanager();
		}
		if(strcmp(buffer, "sorta") == 0)
		{
			sort_ascending();
		}
		if(strcmp(buffer, "sortd") == 0)
		{
			sort_descending();
		}
		if(strcmp(buffer, "sortdm") == 0)
		{
			sort_descending_mthread();
		}
		if(strcmp(buffer, "split") == 0)
		{
			split_datas();
		}
		if(strcmp(buffer, "regroup") == 0)
		{
			regroup_datas();
		}
		if(strcmp(buffer, "checkdatas") == 0)
		{
			check_datas();
		}
		if(strcmp(buffer, "checkfiles") == 0)
		{
			check_files();
		}
		if(strncmp(buffer, "sortd ", 6) == 0)
		{
			int f = atoi(&buffer[6]);
			sort_d_mono(&f);
		}
        if(strncmp(buffer, "printfile ", 10) == 0)
		{
			int f = atoi(&buffer[10]);
			print_file(f);
		}
		if(strncmp(buffer, "checkfile ", 10) == 0)
		{
			int f = atoi(&buffer[10]);
			check_file(f);
		}
		if(strcmp(buffer, "checkdirections") == 0)
		{
			if(test_directions() == 0)
			{
				printf("directions : ok\n");
			} else
			{
				printf("directions : NOK !\n");
			}
		}
        if(strcmp(buffer, "rmnonext") == 0)
        {
			struct array_part *apart= read_parts(partsFiles);
			
			struct array_part *rotateParts = rotate_all_parts(apart);
            map_big_array *map_parts = prepare_map_part(rotateParts);
            remove_possibilities_with_no_next(map_parts, rotateParts);
            free_bigarray(map_parts);
			free_array_part(rotateParts);
			free_array_part(apart);
        }
		if(strcmp(buffer, "min") == 0)
		{
			printf("min :%i\n",search_min_datas());
		}
		if(strcmp(buffer, "help") == 0)
		{
			printf("commands:\n  help\n  check\n  backup\n  restore\n  import\n  print\n  regroup\n  sorta\n  sortd\n  split\n  checkdatas\n  checkfiles\n  checkdirections\n  min\n  rmnonext\n  exit\n");
		}
		free(buffer);
		buffer= NULL;
	}
	exit(EXIT_SUCCESS);
}

int run_console(int server)
{
	pthread_attr_t *thread_attributes = malloc(sizeof *thread_attributes);
	pthread_attr_init(thread_attributes);
	pthread_attr_setdetachstate(thread_attributes, PTHREAD_CREATE_DETACHED);
	pthread_t thread;
	/* Création du thread */
	
	int *param = malloc(sizeof(int));
	*param = server;
	if(0 != pthread_create(&thread, NULL, console, param))
	{
		fprintf(stderr, "Problème avec pthread_create()\n");
		free(thread_attributes);
		exit(EXIT_FAILURE);
	}
	pthread_attr_destroy(thread_attributes);
	free(thread_attributes);
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


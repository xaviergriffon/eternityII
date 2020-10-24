#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#ifdef WIN32
#include <winsock2.h>
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
#include "lifo.h"
#include "datamanager.h"
#include "tcpclient.h"
#include "etii_protocol.h"

static file_possibility_t file_possibility[NB_FILE_POSSIBILITY] =
{
	{{NULL,NULL,0,0,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
	{{NULL,NULL,0,0,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,0,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,0,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,0,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,0,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,0,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,0,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,0,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER},
    {{NULL,NULL,0,0,NULL,0,sizeof(struct possibility_packet)},PTHREAD_MUTEX_INITIALIZER}
};

char*server_ip = NULL;

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
char *get_server_ip()
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

int put_to_server(array_possibility_packet *possibilities)
{
	int socket_id = -1;
	if(-1 == (socket_id = create_tcp_client(server_ip, 2000)))
	{
		fprintf(stderr, "Erreur sur accept()\n");
		return -1;
	}
	
	int t;
	for(t=0; t< possibilities->size; t++)
	{
		if(possibilities->possibilities[t].alloc > max_result)
		{
			max_result = possibilities->possibilities[t].alloc;
			printf("max result:%i\n",max_result);
		}
		if(possibilities->possibilities[t].x < 0 || possibilities->possibilities[t].y < 0 || possibilities->possibilities[t].x > 16 || possibilities->possibilities[t].y > 16)
		{
			printf("alert\n");
		}
		send_instruction(socket_id, INST_ADD);
		struct possibility_packet *possibility = &possibilities->possibilities[t];
		long result = send(socket_id, (struct possibility_packet *)possibility, sizeof(struct possibility_packet),0);
		if (result <= 0) {
			printf("problème send : %li\n",result);
		}
		if(recv_instruction(socket_id) != INST_CONSIDERED){
            //TODO : reintegrer la possiblité
			printf("problème de prise en compte du serveur\n");
		}
	}
    
	send_instruction(socket_id, INST_END);
	shutdown(socket_id, 2);
	int err = closesocket(socket_id);
	if(0 != err)
	{
		printf("erreur close :%i\n",err);
	}
	
	return 0;
    
}

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
					//pthread_mutex_lock(&max_lock);
                    max_result = possibilities->possibilities[t].alloc;
					//pthread_mutex_unlock(&max_lock);
                    printf("max result:%i\n",max_result);
                }
                if(possibilities->possibilities[t].x < 0 || possibilities->possibilities[t].y < 0 || possibilities->possibilities[t].x > 16 || possibilities->possibilities[t].y > 16)
                {
                    printf("alert\n");
                }
				
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

int add_possibility(array_possibility_packet *possibilities)
{
	int error = 0;
    if(server_ip != NULL)
	{
		error = put_to_server(possibilities);
	} else
	{
		error = put_to_local(possibilities);
	}
	
	return error;
}

void scroll_from_server(array_possibility_packet *result,int max_result)
{
	
	int socket_id = -1;
	if(-1 == (socket_id = create_tcp_client(server_ip, 2000)))
	{
		fprintf(stderr, "Erreur sur accept()\n");
		return;
	}
	
	File file;
	init_file_with_cache(&file, 0, sizeof(struct possibility_packet));
	
	int r;
	for(r=0; r < max_result;r++){
		struct possibility_packet *possibilityPacket = malloc(sizeof(struct possibility_packet));
		send_instruction(socket_id, INST_GET);
		long r= recv(socket_id, (struct possibility_packet *)possibilityPacket, sizeof(struct possibility_packet),0);
		if(r != sizeof(struct possibility_packet))
		{
			printf("No possibility recept\n");
		}else
		{
#ifdef CHECK_POSSIBILITY
            int analyse = check_possibility(possibilityPacket);
            if (analyse < 0)
            {
                printf("possibility error : %i\n",analyse);
                printf(" ---");
                print_possibility_packet(possibilityPacket);
            }
#endif // CHECK_POSSIBILITY
			//printf("recev ");
			//print_possibility_packet(possibilityPacket);
			put(&file, possibilityPacket);
		}
		free(possibilityPacket);
	}
	
	if(file.size > 0)
	{
		result->possibilities = malloc(file.size * sizeof(struct possibility_packet));
		int p = 0;
		while(file.size > 0)
		{
			struct possibility_packet *packet = malloc(sizeof(struct possibility_packet));
			scroll(&file, packet);
			memcpy(&result->possibilities[p], packet, sizeof(*packet));
			result->size++;
			free(packet);
			p++;
		}
	}
	
	send_instruction(socket_id, INST_END);
	shutdown(socket_id, 2);
	int err = closesocket(socket_id);
	if(0 != err)
	{
		printf("erreur close :%i\n",err);
	}
}

void scroll_from_local(array_possibility_packet *result,int max_result)
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
					init_file_with_cache(&file, 0, sizeof(struct possibility_packet));
					for(p=0; p < max_result && nothing == 0;p++)
					{
						struct possibility_packet *packet = malloc(sizeof(struct possibility_packet));
						if(scroll(&file_possibility[currfile].file,packet))
						{
							put(&file, packet);
							free(packet);
							packet = NULL;
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
							struct possibility_packet *packet = malloc(sizeof(struct possibility_packet));
							scroll(&file, packet);
							memcpy(&result->possibilities[p], packet, sizeof(*packet));
							result->size++;
							free(packet);
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

array_possibility_packet *get_last_possibility(int max_result)
{
	array_possibility_packet *result = malloc(sizeof(array_possibility_packet));
	result->size = 0;
	result->possibilities = NULL;
	
    
	if(server_ip != NULL)
	{
		scroll_from_server(result, max_result);
	}else
	{
		scroll_from_local(result, max_result);
	}
	if(result->size == 0)
	{
		printf("result 0 \n");
	} else {
		
	}
	return result;
}

int file_size(int nfile)
{
	int result = -1;
	if(nfile >= 0 && nfile < NB_FILE_POSSIBILITY)
	{
		result = file_possibility[nfile].file.size;
	}
	return result;
}

int datas_size()
{
	int result = 0;
	int f;
	for(f=0; f < NB_FILE_POSSIBILITY; f++)
	{
		int fsize = file_size(f);
		if(fsize >0)
		{
			result = result + fsize;
		}
	}
	return result;
}

void lock_all_file()
{
	maintenance = 1;
	int fp;
	// Bloquage des files
	for (fp=0; fp < NB_FILE_POSSIBILITY; fp++)
	{
		pthread_mutex_lock(&file_possibility[fp].lock);
	}
}

void unlock_all_file()
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
			printf("file :%s",filename);
			perror("fopen()");
			exit(EXIT_FAILURE);
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

int import(char *filename)
{
	FILE *f = fopen(filename, "r");
	if(!f)
	{
		printf("file :%s",filename);
		perror("fopen()");
		exit(EXIT_FAILURE);
	}
	
	struct possibility_packet *possibility = malloc(sizeof(struct possibility_packet));
	while(fread(possibility, sizeof(struct possibility_packet),1,f))
	{
		array_possibility_packet *possibilities = malloc(sizeof(array_possibility_packet));
		possibilities->size = 1;
		possibilities->possibilities = malloc(sizeof(struct possibility_packet));
		memcpy(&possibilities->possibilities[0], possibility, sizeof(struct possibility_packet));
		add_possibility(possibilities);
		
		free(possibilities->possibilities);
		free(possibilities);
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
	
	import(filename);
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
            printf("x:%i y:%i alloc:%i\n",possibility->x,possibility->y,possibility->alloc);
        } else {
            printf("null value\n");
        }
        currElement = currElement->next;
    }
    return 0;
}

int printdatamanager()
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

int regroup_datas_nolock()
{
	int fp;
	int size = file_possibility[0].file.size;
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
    printf("regroup size :%i\n",size);
	free(packet);
	packet = NULL;
	return 0;
}

int regroup_datas()
{
	lock_all_file();
	regroup_datas_nolock();
	unlock_all_file();
	return 0;
}

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
			if(!possibility_all_has_a_next(possibility, mapParts,all_rotate_part))
			{
                
				if(currElement->previous != NULL)
                {
                    currElement->previous->next = currElement->next;
                } else {
                    // On est au début
                    file_possibility[fp].file.start = currElement->next;
                }
                if(currElement->next != NULL)
                {
                    currElement->next->previous = currElement->previous;
                } else {
                    file_possibility[fp].file.end = currElement->previous;
                }
                nextElement = currElement->next;
                free (currElement->value);
                free (currElement);
                currElement = NULL;
                file_possibility[fp].file.size--;
                
			}
            if(nextElement == NULL && currElement != NULL)
            {
                currElement = currElement->next;
            } else {
                currElement = nextElement;
            }
		}
	}
    unlock_all_file();
    return 0;
}

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
	
	div_t d = div(file->size, nbsplit);
	int quotient = d.quot;
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

int split_datas()
{
	lock_all_file();
	split_datas_nolock(NB_FILE_POSSIBILITY);
	unlock_all_file();
	return 0;
}

int check_datas()
{
	lock_all_file();
	int count=0;
	int fp;
	for (fp=0; fp < NB_FILE_POSSIBILITY; fp++)
	{
		Element *currElement = file_possibility[fp].file.start;
		while (currElement != NULL)
		{
			count++;
			int analyse = check_possibility((struct possibility_packet *)currElement->value);
			if (analyse < 0)
			{
				printf("possibility error : %i\n",analyse);
				printf(" ---");
				print_possibility_packet((struct possibility_packet *)currElement->value);
				unlock_all_file();
				return -1;
			}
			currElement = currElement->next;
		}
	}
	
	unlock_all_file();
	
	printf("check_datas analyses:%i\n",count);
	return 0;
}

int search_min_datas()
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
    
	return result;
}

// TODO : revoir le trie pour prendre en compte le cache
int sort_ascending()
{
	
	lock_all_file();
	regroup_datas_nolock();
	Element *currElement = file_possibility[0].file.start;
	while (currElement != NULL)
	{
		Element *nextElement = currElement->next;
		if(nextElement != NULL && currElement->value != NULL)
		{
			if(nextElement->value != NULL)
			{
				struct possibility_packet *curr = currElement->value;
				struct possibility_packet *next = nextElement->value;
				if(curr->alloc > next->alloc)
				{
					if(currElement->previous != NULL)
					{
						currElement->previous->next = nextElement;
					}
					if(nextElement->next != NULL)
					{
						nextElement->next->previous = currElement;
					}
					nextElement->previous = currElement->previous;
					currElement->next = nextElement->next;
					currElement->previous = nextElement;
					nextElement->next = currElement;
					if(file_possibility[0].file.start == currElement)
					{
						file_possibility[0].file.start = nextElement;
					}
					if(file_possibility[0].file.end == nextElement)
					{
						file_possibility[0].file.end = currElement;
					}
					
                    if(file_possibility[0].file.start != nextElement)
					{
                        nextElement = nextElement->previous;
                        
                    }
				}
			}
		}
		
		currElement = nextElement;
	}
	unlock_all_file();
	return 0;
}

void *sort_d_mono(void *f)
{
    int intf = *(int *)f;
	printf("sort d file:%i\n",intf);
	file_possibility_t *file_poss =&file_possibility[intf];
	Element *currElement = file_poss->file.start;
	while (currElement != NULL)
	{
		Element *nextElement = currElement->next;
		if(nextElement != NULL && currElement->value != NULL)
		{
			if(nextElement->value != NULL)
			{
				struct possibility_packet *curr = currElement->value;
				struct possibility_packet *next = nextElement->value;
				if(curr->alloc < next->alloc)
				{
					if(currElement->previous != NULL)
					{
						currElement->previous->next = nextElement;
					}
					if(nextElement->next != NULL)
					{
						nextElement->next->previous = currElement;
					}
					nextElement->previous = currElement->previous;
					currElement->next = nextElement->next;
					currElement->previous = nextElement;
					nextElement->next = currElement;
					if(file_poss->file.start == currElement)
					{
						file_poss->file.start = nextElement;
					}
					if(file_poss->file.end == nextElement)
					{
						file_poss->file.end = currElement;
					}
                    /*
                     if(check_file(intf)){
                     printf("--datas--\n");
                     print_file(intf);
                     printf("--\n");
                     printf("On file:%i curr:",intf);
                     print_possibility_packet(currElement->value);
                     printf("On file:%i next:",intf);
                     print_possibility_packet(nextElement->value);
                     printf("On file:%i start:",intf);
                     print_possibility_packet(file_poss->file.start->value);
                     printf("On file:%i end:",intf);
                     print_possibility_packet(file_poss->file.end->value);
                     exit(EXIT_FAILURE);
                     }
                     */
                    if(file_poss->file.start != nextElement)
					{
                        nextElement = nextElement->previous;
						
                    }
					
				}
			}
		}
		
		currElement = nextElement;
	}
	
	printf("end sort d file:%i\n",*(int *)f);
	return NULL;
}

int check_file(int f)
{
	int result = 0;
	
	file_possibility_t file_poss =file_possibility[f];
	File file = file_poss.file;
	
	if(file.size == 0)
	{
		if(file.start != NULL)
		{
			printf("File:%i size=0 and start not null\n",f);
			result = -1;
		}
		
		if(file.end != NULL)
		{
			printf("File:%i size=0 and end not null\n",f);
			result = -1;
		}
	}
	
	// test que la fin correspond à la taille
	int t;
	Element *currElement = file_poss.file.start;
	Element *lastElement = currElement;
	for(t=0; t < file.size && currElement != NULL;t++)
	{
		if(currElement->value == NULL){
			printf("File:%i value NULL\n",f);
			result = -1;
		}
		lastElement = currElement;
		currElement = currElement->next;
	}
	
	if(currElement != NULL)
	{
		printf("File:%i last analysed element is not null | file.size:%i analysed:%i",f,file.size, t);
		result = -1;
	}
	if (t != file.size || lastElement != file.end) {
		printf("File:%i end not correspond to the size:%i analysed:%i\n",f,file.size,t);
		result=-1;
	}
	return result;
}

int check_files()
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

int sort_descending_nolock()
{
	printf("regroup datas \n");
	regroup_datas_nolock();
	printf("sort file 0\n");
	int *i = malloc(sizeof(int));
	i[0] = 0;
	sort_d_mono(&i[0]);
	free(i);
	return 0;
}

void sortdmthread()
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
	
	free(f);
}

int sort_descending_mthread()
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
		printf("split to:%i\n",nbfile);
		split_datas_nolock(nbfile);
		sortdmthread();
	}
	
	printf("sort d one thread\n");
	sort_descending_nolock();
    
	unlock_all_file();
	return 0;
}

// TODO : revoir le trie pour prendre en compte le cache
int sort_descending()
{
	lock_all_file();
	
	sort_descending_nolock();
	
	unlock_all_file();
	return 0;
}

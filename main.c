#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/wait.h>
#include <signal.h>
#include <errno.h>

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
#include "local_socket.h"
#include "command_lines.h"
#include "etii_statistic.h"
#include "logger.h"

void runclient(const char *hostname, const char *file)
{
	// On indique au manager de passer par un serveur
	set_server_ip(hostname);
	
    runMonoClient(file);
	
	// Comme on est en mode client, on ne devrait plus rien avoir dans les files
	// si c'est le cas, il s'agit d'une erreur
	if (datas_size() > 0) {
		char *def_file = malloc(sizeof(char) * 50);
        sprintf(def_file, "./failed_exit_eternityII_%i.back", getpid());
        char *def_analyse_file = malloc(sizeof(char) * 60);
        sprintf(def_analyse_file, "./failed_exit_eternityII-in_analyse_%i.back", getpid());
		backup(def_file);
        backup_analysed(def_analyse_file);
        free(def_file);
        free(def_analyse_file);
	}
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


void failed_arg(void)
{
	log_error("Indiquer parametre suivant :\ntcpserver [nombre de threads] [pieces.csv]\ntcpclient [serveur] [pieces.csv]\n");
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
		log_error("Problème avec pthread_create()\n");
		free(thread_attributes);
		exit(EXIT_FAILURE);
	}
	pthread_attr_destroy(thread_attributes);
	free(thread_attributes);
	return 0;
}
void run_fork_thread(int *socket_id);

void *fork_checker(void *param) {
	struct sockaddr_un *main_addr = (struct sockaddr_un *)param;
	char socket_fork[50];
    int sp_len = sprintf(socket_fork, "etii_fork.%d", getpid());
    socket_fork[sp_len] = '\0';
    struct sockaddr_un *fork_addr = build_sockaddr(socket_fork);
#ifdef DEBUG_LOCAL_SOCKET
    log_debug("socket fork : %s\n", socket_fork);
#endif // DEBUG_LOCAL_SOCKET
    fork_checker_socket_id = build_udp_local_socket(fork_addr);
    free(fork_addr);

	log_info("fork_checker_socket_id: %i\n", fork_checker_socket_id);
	if (fork_checker_socket_id > 0) {
		int *so = &fork_checker_socket_id;
		run_fork_thread(so);
	}
    
    // TPS tests per second
    unsigned long long oldSPS[5];
    for (int c = 0; c < 5; c++) {
        oldSPS[c] = 0;
    }
    int t = 0;
    unsigned long long last_counter = 0;
    struct client_statistics *statistic = malloc(sizeof(struct client_statistics));
	while(request != REQUEST_STOP && fork_checker_socket_id > 0) {
        unsigned long long counter = compteurs[0];
        unsigned long long sps = 0;
        if (counter >= last_counter) {
            sps = counter - last_counter;
        } else {
            // le compteur a fait un tour
            sps = ((sps - 1) - last_counter) + counter;
        }
        last_counter = counter;
        oldSPS[t] = sps;
        t++;
        if (t >= 5) {
            t = 0;
        }
        
        // on effectue une moyenne sur 5 secondes
        // les valeurs à 0 ne sont pas comptées
        int m = 0;
        for (int i = 0; i < 5; i++) {
            if (oldSPS[i] > 0) {
                m++;
                sps += oldSPS[i];
            }
        }
        if (m > 0) {
            sps = sps / m;
        } else {
            sps = 0;
        }
        statistic->shots_per_second = sps;
        
        int analyses_in_stock = 0;
        for (int f = 0; f < NB_FILE_POSSIBILITY; f++) {
            analyses_in_stock += file_analysed_size(f);
        }
        statistic->analyses_in_stock = analyses_in_stock;
        statistic->possibilities_in_stock = lastfilesize[0];
        statistic->max_result = max_result;
#ifdef DEBUG_LOCAL_SOCKET
        //printf("send to %s on socket %i stat %lli\n", main_addr->sun_path, fork_checker_socket_id, statistic->shots_per_second);
        if(
#endif // DEBUG_LOCAL_SOCKET
        
		sendto(fork_checker_socket_id, statistic, sizeof(struct client_statistics), MSG_DONTWAIT, (struct sockaddr *) main_addr,
                               sizeof(struct sockaddr_un))
#ifdef DEBUG_LOCAL_SOCKET
           != sizeof(struct client_statistics) ) {
            log_debug("fork_checker cl %d error %i sendto : %s\n", getpid(), errno, strerror(errno));
        }
#else
        ;
#endif // DEBUG_LOCAL_SOCKET
		sleep(1);
	}
    free(statistic);
    
	return NULL;
}

int run_fork_checker(struct sockaddr_un *main_addr)
{
	pthread_attr_t *thread_attributes = malloc(sizeof *thread_attributes);
	pthread_attr_init(thread_attributes);
	pthread_attr_setdetachstate(thread_attributes, PTHREAD_CREATE_DETACHED);
	pthread_t thread;
	
	if(0 != pthread_create(&thread, thread_attributes, fork_checker, main_addr))
	{
		log_error("Problème avec pthread_create()\n");
		free(thread_attributes);
		exit(EXIT_FAILURE);
	}
	pthread_attr_destroy(thread_attributes);
	free(thread_attributes);
	return 0;
}

int init_compteurs(void)
{
	compteurs = malloc(sizeof(unsigned long long) * NB_THREADS);
	lastfilesize = malloc(sizeof(unsigned long long) * NB_THREADS);
	
	for(int c = 0; c < NB_THREADS;c++)
	{
		compteurs[c] = 0;
		lastfilesize[c] = 0;
	}

	return 0;
}

void signal_ignored(int sig) {
#ifdef DEBUG_SIGNAL
    log_debug("catch signal %s\n", strsignal(sig));
#endif
}

void signal_end_handler(int sig)
{
#ifdef DEBUG_SIGNAL
    log_console("receive signal : %i\n", sig);
    flush_console();
#endif // DEBUG_SIGNAL
	request = REQUEST_STOP;
    if (childrens_pid != NULL && parent_pid == getpid()) {
		for (int c = 0; c < NB_THREADS; c++) {
            if (childrens_pid[c] > 0) {
                kill(childrens_pid[c], sig);
            }
		}
	}
    if (server == 1) {
        exit(0);
    }
}

/*
 * Prise en charge du signal SIGCHLD
 */
void sigchld_handler(int signal) {
	// lecture du statut pour éviter les process zombie
	int status = 0;
#ifdef DEBUG_SIGNAL
    log_debug("sigchld_handler\n");
	pid_t wpid;
    while(0 < (wpid = waitpid(-1, &status, WNOHANG)));
	log_debug("Exit status of %d was %d\n", (int)wpid, status);
	if(WIFEXITED(status)) {
		/* The child process exited normally */
		log_debug("Exit value %d\n", WEXITSTATUS(status));
	} else if(WIFSIGNALED(status)) {
		/* The child process was killed by a signal. Note the use of strsignal
			to make the output human-readable. */
		log_debug("Killed by %s\n", strsignal(WTERMSIG(status)));
	}
    flush_debug();
#else
    while(0 < waitpid(-1, &status, WNOHANG));
#endif // DEBUG_SIGNAL
}

void init_sigchld_sigaction(void) {
 	struct sigaction sa;
     //memset(&sa, 0, sizeof *sa);
     sa.sa_handler = sigchld_handler;
     sa.sa_flags = SA_SIGINFO|SA_RESTART;
     sigemptyset(&(sa.sa_mask));
     if (sigaction(SIGCHLD, &sa, NULL) != 0) {
         log_error("Problème avec sigaction()\n");
         exit(EXIT_FAILURE);
     }
 }

void wait_child(void) {
 	
    int status = 0;
#ifdef DEBUG_SIGNAL
    log_debug("wait_child\n");
    pid_t wpid;
 	while ((wpid = wait(&status)) >= 0) {
        log_debug("Exit status of %d was %d\n", (int)wpid, status);
 		if(WIFEXITED(status)) {
 			/* The child process exited normally */
 			log_debug("Exit value %d\n", WEXITSTATUS(status));
 		} else if(WIFSIGNALED(status)) {
 			/* The child process was killed by a signal. Note the use of strsignal
 				to make the output human-readable. */
 			log_debug("Killed by %s\n", strsignal(WTERMSIG(status)));
 		}
    }
#else
    while (wait(&status) >= 0);
#endif // DEBUG_SIGNAL
 }
void *server_udp(void *param) {
    int socket_id = *(int*)param;
    
    struct sockaddr_un *claddr = malloc(sizeof(struct sockaddr_un));
    ssize_t numBytes;
    socklen_t len = sizeof(struct sockaddr_un);;
    
    struct client_statistics *statistics = malloc(sizeof(struct client_statistics));

    while (request != REQUEST_STOP) {
        numBytes = recvfrom(socket_id, statistics, sizeof(struct client_statistics), 0,
                            (struct sockaddr *) claddr, &len);
        if (numBytes == -1) {
            if (request != REQUEST_STOP) {
                if (errno == EBADF) {
#ifdef DEBUG_LOCAL_SOCKET
                    log_debug("srv error invalid descriptor on recvfrom\n");
                    flush_debug();
#endif // DEBUG_LOCAL_SOCKET
                    break;
                }
                log_errno("srv error on recvfrom => ");
                flush_error();
            }
            continue;
        }
        
        for (int cpt = 0; cpt < NB_THREADS; cpt++) {
            if (strcmp(claddr->sun_path, forkId[cpt]) == 0) {
                memcpy(&fork_statistics[cpt], statistics, sizeof(struct client_statistics));
                break;
            }
        }
    }
    free(claddr);
    
    free(statistics);
    
    return NULL;
}

void run_server_thread(int *socket_id) {
    log_info("srv  socket_id %i\n", *socket_id);
    pthread_attr_t *thread_attributes = malloc(sizeof *thread_attributes);
    pthread_attr_init(thread_attributes);
    pthread_attr_setdetachstate(thread_attributes, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    if(0 != pthread_create(&thread, thread_attributes, server_udp, socket_id))
        {
            log_error("Problème avec pthread_create()\n");
            free(thread_attributes);
            exit(EXIT_FAILURE);
        }
        pthread_attr_destroy(thread_attributes);
        free(thread_attributes);
}

void *fork_udp(void *param) {
	int socket_id = *(int*)param;
    struct sockaddr_un *srv_addr = malloc(sizeof(struct sockaddr_un));
    ssize_t numBytes;
    socklen_t len = sizeof(struct sockaddr_un);
    char *value = malloc(sizeof(char) * 100);
    while (request != REQUEST_STOP) {
        numBytes = recvfrom(socket_id, value, sizeof(char) * 100, 0,
                            (struct sockaddr *) srv_addr, &len);
        if (numBytes == -1) {
            if (request != REQUEST_STOP) {
                log_errno("cl error on recvfrom => ");
                flush_error();
            }
            continue;
        }
		value[numBytes] = '\0';
        do_command_line(value);
    }
    free(srv_addr);
    free(value);
    return NULL;
}

void run_fork_thread(int *socket_id) {
	log_info("cl socket_id %i\n", *socket_id);
	pthread_attr_t *thread_attributes = malloc(sizeof *thread_attributes);
    pthread_attr_init(thread_attributes);
    pthread_attr_setdetachstate(thread_attributes, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    if(0 != pthread_create(&thread, thread_attributes, fork_udp, socket_id))
	{
		log_error("run_fork_thread Problème avec pthread_create()\n");
		free(thread_attributes);
		exit(EXIT_FAILURE);
	}
	pthread_attr_destroy(thread_attributes);
	free(thread_attributes);
}

void init_childs(void) {
    childrens_pid = malloc(sizeof(pid_t) * NB_THREADS);
    forkId = malloc(sizeof(char *) * NB_THREADS);
    fork_statistics = malloc(sizeof(struct client_statistics) * NB_THREADS);
    for (int c = 0; c < NB_THREADS; c++) {
        childrens_pid[c] = -1;
        forkId[c] = malloc(sizeof(char) * 300);
        forkId[c][0] = '\0';
        
        fork_statistics[c].analyses_in_stock = 0;
        fork_statistics[c].possibilities_in_stock = 0;
        fork_statistics[c].shots_per_second = 0;
    }
}

void init_signals(void) {
    // TODO : voir si besoin de tous
    signal(SIGINT, signal_end_handler);
    signal(SIGHUP, signal_end_handler);
    signal(SIGQUIT, signal_end_handler);
    signal(SIGKILL, signal_end_handler);
    signal(SIGTERM, signal_end_handler);
    signal(SIGPIPE, signal_ignored);
}

int main(int argc, const char * argv[])
{
	parent_pid = getpid();
	log_info("Version %i", version);
	
	if (argc >= 2) {
		lastcheck = calloc(2000, sizeof(char));
		
		if(strcmp("tcpclient", argv[1]) == 0) {
			log_info("client\n");
            NB_THREADS = 1;
			char *serverIp = "localhost";
			if(argc >= 3){
				serverIp = (char *)argv[2];
			}
            if(argc >= 4) {
                NB_THREADS = atoi(argv[3]);
            }
#ifdef DEBUG_IN_MONO_PROCESS
            NB_THREADS = 1;
#endif
            init_childs();
            init_compteurs();
            init_signals();
			
			char socket_main[50];
			sprintf(socket_main, "etii_main.%d", getpid());
			main_addr = build_sockaddr(socket_main);
			log_info("socket main : %s\n", socket_main);

			int *socket_id = malloc(sizeof(int));
			*socket_id = build_udp_local_socket(main_addr);
			if (socket_id > 0) {
				run_server_thread(socket_id);
			}
			main_socket_id = socket_id;
					
			init_sigchld_sigaction();
			
			run_checker(0);
			run_console(0);
            if(argc >= 5) {
                partsFiles = (char *)(argv[4]);
            }

			pid_t child_pid = -1;
			for (int c = 0; c < NB_THREADS; c++) {
				if (parent_pid == getpid()) {
#ifdef DEBUG_IN_MONO_PROCESS
                    child_pid = getpid();
#else
                    child_pid = fork();
#endif // DEBUG_IN_MONO_PROCESS
                    if (child_pid != 0) {
                        // on enregistre les informations du process fils
                        int sp_len = sprintf(forkId[c], "etii_fork.%d", child_pid);
                        forkId[c][sp_len] = '\0';
                        childrens_pid[c] = child_pid;
#ifndef DEBUG_IN_MONO_PROCESS
                    } else {
#endif // DEBUG_IN_MONO_PROCESS
                        // un fils tourne sur 1 seul thread
                        NB_THREADS = 1;
                        // création d'un thread chargé de remonter l'information du compteur
                        run_fork_checker(main_addr);
                        
                        runclient(serverIp, partsFiles);
                        
                        if (fork_checker_socket_id > 0) {
                            close(fork_checker_socket_id);
                        }
                        char socket_fork[50];
                        int socket_fork_len = sprintf(socket_fork, "etii_fork.%d", getpid());
                        socket_fork[socket_fork_len] = '\0';
                        struct sockaddr_un *fork_addr = build_sockaddr(socket_fork);
#ifdef DEBUG_LOCAL_SOCKET
                        log_debug("remove : %s\n", fork_addr->sun_path);
                        flush_debug();
#endif // DEBUG_LOCAL_SOCKET
                        remove(fork_addr->sun_path);
                        free(fork_addr);
                    }
				}
			}

			if (parent_pid == getpid()) {
 				wait_child();
                close(*socket_id);
#ifdef DEBUG_LOCAL_SOCKET
                log_debug("remove : %s\n", main_addr->sun_path);
                flush_debug();
#endif // DEBUG_LOCAL_SOCKET
                remove(main_addr->sun_path);
 			}
            free(main_addr);
		} else if (strcmp("tcpserver", argv[1]) == 0) {
			log_info("server\n");
            server = 1;
            NB_THREADS = 80;
			if(argc >= 3) {
				log_info("arg 2 : %s",argv[2]);
				NB_THREADS = atoi(argv[2]);
			}
			log_info("Nb threads : %i\n", NB_THREADS);
            init_childs();
            init_signals();
			init_compteurs();
			run_checker(1);
			run_console(1);
            if(argc >= 4) {
                partsFiles = (char *)(argv[3]);
            }
			runserver(partsFiles);
		} else if(strcmp("test", argv[1])==0) {
            NB_THREADS = 1;
			max_search_by_sec = 100000;
			init_compteurs();
			run_checker(0);
			run_console(0);
            runauto(argv[2]);
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


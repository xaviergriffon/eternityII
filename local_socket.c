#include "local_socket.h"
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <sys/socket.h> 
#include <sys/un.h>
#include <sys/stat.h>
#include <errno.h>

#include "static_variables.h"

struct sockaddr_un build_sockaddr(const char *filename) {
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(struct sockaddr_un));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, filename, sizeof(addr.sun_path) - 1);
    
    return addr;
}

int create_udp_local_socket(struct sockaddr_un svaddr) {
    unlink(svaddr.sun_path);
    
    int socket_id = socket(AF_UNIX, SOCK_DGRAM, 0);       /* Create server socket */
    if (socket_id == -1) {
        printf("error %i on socket for %s\n", errno, svaddr.sun_path);
        return -1;
    }

    if (remove(svaddr.sun_path) == -1 && errno != ENOENT) {
        printf("remove-%s\n", svaddr.sun_path);
        return -1;
    }

    if (bind(socket_id, (struct sockaddr *) &svaddr, sizeof(struct sockaddr_un)) == -1) {
        printf("error %i on bind for %s\n", errno, svaddr.sun_path);
        return -1;
    }
    
    return socket_id;
}

int local_socket_new(const char *filename) {
    int s, sock;
    struct sockaddr_un server_address;
    size_t l;
    socklen_t server_len;

    // Suppression du fichier s'il existe déjà
    unlink(filename);
    
    // Création de la socket locale
    if ((s = socket(PF_UNIX, SOCK_STREAM, 0)) < 0) {
        printf("erreur %i lors de la création de la socket locale %s", s, filename);
        return -1;
    }

    /*
     * Attribution d'une adresse à la socket locale (le fichier)
     */
    l = strlen(filename);
    memcpy(server_address.sun_path, filename, l + 1);
    server_address.sun_family = AF_UNIX;
    server_len = (socklen_t)(sizeof(server_address.sun_family) + l + 1);
    int b;
    if ((b = bind(s, (struct sockaddr *) &server_address, server_len)) < 0) {
        printf("erreur %i lors du binding\n", b);
        return -1;
    }

    // Mise en écoute de la socket
    if (listen(s, 1000) < 0) {
        printf("erreur lors de la mise en écoute de la socket local\n");
        return -1;
    }

    sock = accept(s, NULL, NULL);

    // fermeture de la socket
    shutdown(s, 2);
    close(s);

    return sock;
}

int create_client_unix(const char *filename) {
    int s;
    struct sockaddr_un addr;
    struct stat buf;
    socklen_t server_len;
    size_t l;

    while (stat(filename, &buf)) {
        // on attend que le fichier existe
        // TODO : mettre un maximum de boucle
        sleep(1);
    }

    // Création de la socket locale
    if ((s = socket(PF_UNIX, SOCK_STREAM, 0)) < 0) {
        printf("erreur %i lors de la création de la socket locale cliente", s);
        return -1;
    }

    // Attribution de l'adresse
    l = strlen(filename);
    memcpy(addr.sun_path, filename, l + 1);
    addr.sun_family = AF_UNIX;
    server_len = (socklen_t)l + sizeof(addr.sun_family) + 1;

    // connexion
    int c;
    if ((c = connect(s, (struct sockaddr *) &addr, server_len)) < 0) {
        printf("erreur %i lors de la connexion à la socket locale %s\n", c, filename);
        return -1;
    }

    return s;
} 

void send_command_to_childs(char *command) {
    if (parent_pid == getpid()) {
        for (int f = 0; f < NB_THREADS; f++) {
            if (strcmp(forkId[f], "") != 0) {
                struct sockaddr_un cl_addr = build_sockaddr(forkId[f]);
                if (sendto(*main_socket_id, command, strlen(command), MSG_DONTWAIT, (struct sockaddr *) &cl_addr,
                            sizeof(struct sockaddr_un)) != strlen(command)) {
                    printf("cl %d error %i send : %s\n", getpid(), errno, strerror(errno));
                    
                }
            }
        }
    }
}

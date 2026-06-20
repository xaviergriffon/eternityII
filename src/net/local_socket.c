#include "net/local_socket.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h> 
#include <sys/un.h>
#include <sys/stat.h>
#include <errno.h>
#include <netinet/in.h>

#include "ui/logger.h"
#include "app/static_variables.h"

/**
 * @brief Alloue et initialise une structure `sockaddr_un` pour un socket Unix.
 * @param filename Chemin du socket Unix (tronqué à `sizeof(sun_path) - 1`).
 * @return         Pointeur alloué (à libérer par l'appelant).
 */
struct sockaddr_un *build_sockaddr(const char *filename) {
    struct sockaddr_un *addr = malloc(sizeof(struct sockaddr_un));
    memset(addr, 0, sizeof(struct sockaddr_un));
    addr->sun_family = AF_UNIX;
    strncpy(addr->sun_path, filename, sizeof(addr->sun_path) - 1);

    return addr;
}

/**
 * @brief Calcule la taille effective d'une `sockaddr_un`.
 *
 * Tient compte de la longueur réelle de `sun_path` plutôt que de `sizeof`.
 *
 * @param svaddr Adresse Unix dont on calcule la taille.
 * @return       Taille en octets utilisable pour `bind`/`sendto`.
 */
socklen_t size_of_sockaddr_un(struct sockaddr_un *svaddr) {
    return (socklen_t)(strlen(svaddr->sun_path) + sizeof(svaddr->sun_family) + 1);
}

/**
 * @brief Crée un socket UDP Unix lié à l'adresse donnée.
 *
 * Supprime le fichier socket existant (s'il y en a un), crée un socket
 * `PF_UNIX/SOCK_DGRAM` et le lie à `svaddr`. Utilisé pour la communication
 * IPC parent↔enfant.
 *
 * @param svaddr Adresse Unix cible (le fichier sera créé/recréé).
 * @return       Descripteur du socket, ou -1 en cas d'erreur.
 */
int build_udp_local_socket(struct sockaddr_un *svaddr) {
    unlink(svaddr->sun_path);
    
    // Création d'un socket serveur en PF_UNIX
    int socket_id = socket(PF_UNIX, SOCK_DGRAM, 0);
    if (socket_id == -1) {
        log_errno("Error on socket for %s => ", svaddr->sun_path);
        return -1;
    }

    if (remove(svaddr->sun_path) == -1 && errno != ENOENT) {
        log_errno("Error on remove-%s => ", svaddr->sun_path);
        return -1;
    }

    socklen_t addr_len = size_of_sockaddr_un(svaddr);
    if (bind(socket_id, (struct sockaddr *) svaddr, addr_len) == -1) {
        log_errno("Error on bind for %s => ", svaddr->sun_path);
        return -1;
    }
    
    return socket_id;
}

/**
 * @brief Envoie une commande texte à tous les processus enfants via UDP Unix.
 *
 * N'envoie que si le processus courant est le parent (`parent_pid == getpid()`).
 * Itère sur les `NB_THREADS` entrées de `forkId` et envoie `command` en mode
 * non-bloquant (`MSG_DONTWAIT`) à chacun.
 *
 * @param command Chaîne de commande à transmettre (ex. "backup", "exit").
 */
void send_command_to_childs(char *command) {
    if (parent_pid == getpid()) {
        for (int f = 0; f < NB_THREADS; f++) {
            if (strcmp(forkId[f], "") != 0) {
                struct sockaddr_un *cl_addr = build_sockaddr(forkId[f]);
                if (sendto(*main_socket_id, command, strlen(command), MSG_DONTWAIT, (struct sockaddr *) cl_addr,
                            sizeof(struct sockaddr_un)) != (ssize_t)strlen(command)) {
                    log_errno("Error on send_command_to_childs cl %d => ", getpid());
                    
                }
                free(cl_addr);
            }
        }
    }
}

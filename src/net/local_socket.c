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
#include "app/app_static_variables.h"
#include "net/ipc_protocol.h"
#include "core/possibility.h"

/**
 * @brief Taille du plus gros datagramme IPC parent<->fork (octet de type compris).
 *
 * Le maximum des trois familles de messages d'ipc_protocol.h :
 *   - IPC_MSG_STATS      : 1 + sizeof(struct client_statistics) — croît avec
 *                          FC_STAT_MAX_K (2130 octets à FC_STAT_MAX_K=256) ;
 *   - IPC_MSG_BEST_BOARD : 1 + sizeof(struct possibility_packet) ;
 *   - IPC_MSG_LOG_*      : 1 + IPC_LINE_MAX + 1 (cf. logger.c).
 *
 * @return Taille maximale en octets d'un datagramme IPC.
 */
size_t ipc_max_datagram(void)
{
    size_t sz = 1 + sizeof(struct client_statistics);
    if (sz < 1 + sizeof(struct possibility_packet)) {
        sz = 1 + sizeof(struct possibility_packet);
    }
    if (sz < (size_t)(1 + IPC_LINE_MAX + 1)) {
        sz = (size_t)(1 + IPC_LINE_MAX + 1);
    }
    return sz;
}

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

    /* macOS limite un datagramme AF_UNIX à net.local.dgram.maxdgram (2048
       octets par défaut) : tout message IPC plus gros échoue en EMSGSIZE —
       or IPC_MSG_STATS dépasse ce seuil dès que FC_STAT_MAX_K est relevé
       (ex. 256 pour accompagner un FORWARD_CHECK_K élevé), et les lignes de
       log proches d'IPC_LINE_MAX (4000) le dépassent déjà. Relever SO_SNDBUF
       lève cette limite par socket. Côté réception, le défaut (recvspace
       4096) ne met en file qu'un seul gros datagramme : SO_RCVBUF est
       dimensionné pour absorber une rafale (stats de tous les forks à la
       même seconde + logs). Sous Linux les défauts (~200 Ko) couvrent déjà
       ces tailles, l'appel est alors sans effet utile mais inoffensif.
       Échec non bloquant : on logue et on continue avec les défauts. */
    int sndbuf = (int)(2 * ipc_max_datagram());
    int rcvbuf = (int)(16 * ipc_max_datagram());
    if (setsockopt(socket_id, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) == -1) {
        log_errno("Error on setsockopt SO_SNDBUF for %s => ", svaddr->sun_path);
    }
    if (setsockopt(socket_id, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) == -1) {
        log_errno("Error on setsockopt SO_RCVBUF for %s => ", svaddr->sun_path);
    }

    if (remove(svaddr->sun_path) == -1 && errno != ENOENT) {
        log_errno("Error on remove-%s => ", svaddr->sun_path);
        close(socket_id);
        return -1;
    }

    socklen_t addr_len = size_of_sockaddr_un(svaddr);
    if (bind(socket_id, (struct sockaddr *) svaddr, addr_len) == -1) {
        log_errno("Error on bind for %s => ", svaddr->sun_path);
        close(socket_id);
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

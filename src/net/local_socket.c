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

/* ---- Nettoyage des fichiers socket à la terminaison ---------------------- */

/**
 * @brief Nombre de sockets locaux dont un même process peut retenir le chemin.
 *
 * En production un process n'en possède qu'UN (le parent son `etii_main.<pid>`,
 * un fork son `etii_fork.<pid>`) ; la marge sert aux tests unitaires, qui en
 * ouvrent plusieurs simultanément dans le même process.
 */
#define LOCAL_SOCKET_MAX_OWNED 16

/** @brief Chemin d'un socket lié, avec le pid du process qui l'a créé. */
typedef struct {
    pid_t owner;   /**< 0 = slot libre. Comparé à getpid() : un fils hérite de
                        la table de son parent et ne doit RIEN y supprimer. */
    char  path[sizeof(((struct sockaddr_un *)0)->sun_path)];
} owned_local_socket_t;

/* Pas de verrou, volontairement : `build_udp_local_socket` n'est appelée
   qu'une fois par process dans le code applicatif (thread principal du parent
   avant tout fork, thread `fork_checker` d'un fils juste après son fork), et
   un mutex ici deviendrait un piège de plus vis-à-vis de l'invariant « aucun
   thread du parent ne tourne pendant fork() » (voir fork_gate.c). */
static owned_local_socket_t g_owned_sockets[LOCAL_SOCKET_MAX_OWNED];
static int g_cleanup_registered = 0;

void local_socket_cleanup_owned(void)
{
    pid_t me = getpid();
    for (int i = 0; i < LOCAL_SOCKET_MAX_OWNED; i++) {
        if (g_owned_sockets[i].owner == me && g_owned_sockets[i].path[0] != '\0') {
            /* ENOENT attendu et sans conséquence : le chemin de sortie nominal
               (main.c / fork_orchestrator.c) fait déjà son propre remove(). */
            unlink(g_owned_sockets[i].path);
            g_owned_sockets[i].owner = 0;
            g_owned_sockets[i].path[0] = '\0';
        }
    }
}

/**
 * @brief Mémorise le chemin d'un socket lié pour le supprimer à la sortie.
 *
 * Réutilise un slot libre, un slot déjà occupé par LE MÊME chemin pour ce
 * process, ou un slot appartenant à un AUTRE pid (entrée héritée d'un parent
 * via fork : périmée dans ce process, jamais à supprimer par lui — la recycler
 * est donc à la fois correct et ce qui permet un nombre illimité de
 * générations de forks). Branche `local_socket_cleanup_owned` sur `atexit` au
 * premier enregistrement.
 *
 * Échec (table pleine) non bloquant : le socket reste fonctionnel, seul son
 * nettoyage automatique est perdu — on le signale plutôt que de le taire.
 *
 * @param path Chemin du fichier socket créé par `bind()`.
 */
static void register_owned_socket(const char *path)
{
    pid_t me = getpid();
    int slot = -1;
    int foreign = -1;
    for (int i = 0; i < LOCAL_SOCKET_MAX_OWNED; i++) {
        if (g_owned_sockets[i].owner == me && strcmp(g_owned_sockets[i].path, path) == 0) {
            slot = i; /* même chemin re-lié par ce process : on ne duplique pas */
            break;
        }
        if (g_owned_sockets[i].owner == 0) {
            if (slot == -1) slot = i;
        } else if (g_owned_sockets[i].owner != me && foreign == -1) {
            foreign = i; /* entrée héritée d'un parent : recyclable ici */
        }
    }
    if (slot == -1) slot = foreign;
    if (slot == -1) {
        log_error("local_socket : table de nettoyage pleine (%d entrées) — %s "
                  "ne sera pas supprimé automatiquement à la sortie\n",
                  LOCAL_SOCKET_MAX_OWNED, path);
        return;
    }

    /* `memcpy` + terminaison explicite plutôt que `strncpy` : source et
       destination ont la MÊME taille (`sun_path`), et gcc/aarch64 en -Ofast
       diagnostique alors un -Wstringop-truncation légitime sur le cas limite
       d'un chemin exactement plein. La borne est ici explicite. */
    size_t len = strlen(path);
    if (len >= sizeof(g_owned_sockets[slot].path)) {
        len = sizeof(g_owned_sockets[slot].path) - 1;
    }
    g_owned_sockets[slot].owner = me;
    memcpy(g_owned_sockets[slot].path, path, len);
    g_owned_sockets[slot].path[len] = '\0';

    /* Un fils hérite du drapeau ET de la chaîne atexit() du parent : ne pas
       ré-enregistrer chez lui est correct, le handler y est déjà. */
    if (!g_cleanup_registered) {
        if (atexit(local_socket_cleanup_owned) != 0) {
            log_error("local_socket : atexit(local_socket_cleanup_owned) a échoué — "
                      "les sockets locaux ne seront pas nettoyés automatiquement\n");
            return;
        }
        g_cleanup_registered = 1;
    }
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

    /* `bind` vient de créer le fichier spécial : on retient son chemin pour
       qu'il soit supprimé à la terminaison du process, quel que soit le chemin
       de sortie emprunté (cf. local_socket_cleanup_owned, local_socket.h). */
    register_owned_socket(svaddr->sun_path);

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

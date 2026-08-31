/**
 * @file        local_socket.h
 * @brief       Contient des méthodes permettants de communiquer via des "sockets" AF_UNIX et PF_UNIX.
 */
#ifndef local_socket_h
#define local_socket_h
#include <stddef.h>
#include <sys/un.h>

/**
 * @brief Taille du plus gros datagramme IPC parent<->fork (octet de type compris).
 *
 * Couvre les trois familles de messages d'ipc_protocol.h : IPC_MSG_STATS
 * (struct client_statistics — croît avec FC_STAT_MAX_K), IPC_MSG_BEST_BOARD
 * (struct possibility_packet) et IPC_MSG_LOG_* (IPC_LINE_MAX). Sert à
 * dimensionner les tampons SO_SNDBUF/SO_RCVBUF des sockets IPC (cf.
 * build_udp_local_socket).
 */
size_t ipc_max_datagram(void);

/**
 * @brief Construction d'une adresse de socket AF_UNIX vers le fichier
 * @return L'adresse du socket AF_UNIX
 */
struct sockaddr_un *build_sockaddr(const char *filename);
/**
 * @brief Construit un socket en PF_UNIX vers l'adresse
 * @return l'identifiant du socket
 */
int build_udp_local_socket(struct sockaddr_un *svaddr);
/**
 * @brief Supprime les fichiers socket créés par ce process, et eux seuls.
 *
 * `bind()` matérialise chaque socket AF_UNIX par un fichier spécial dans le
 * répertoire de travail. `build_udp_local_socket` enregistre ce chemin ;
 * cette fonction l'`unlink()`. Branchée sur `atexit()` au premier
 * enregistrement, pour que tous les chemins de sortie la jouent — pas
 * seulement le `remove()` explicite du retour nominal, mais aussi `exit`
 * console, les sorties d'erreur et `signal_end_handler`. Sans elle, chaque
 * exécution laissait une socket orpheline : invisible de `git status` et
 * fatale au `cp -R` de `make test-docker`.
 *
 * Invariant de fork : un fils hérite de la table d'enregistrement et de la
 * chaîne `atexit()` de son parent ; seul le pid propriétaire mémorisé à
 * l'enregistrement l'empêche de supprimer la socket de son parent — un fils
 * ne nettoie donc que la socket qu'il a lui-même créée.
 *
 * Idempotente, sûre depuis un gestionnaire de signal (`unlink` est
 * async-signal-safe) et sans effet si le fichier a déjà disparu.
 */
void local_socket_cleanup_owned(void);
/**
 * @brief Transmet une commande aux process fils
 * 
 * @param command commande à transmettre
 */
void send_command_to_childs(char *command);
#endif // local_socket_h

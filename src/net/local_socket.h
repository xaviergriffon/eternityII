/**
 * @file        local_socket.h
 * @brief       Contient des méthodes permettants de communiquer via des "sockets" AF_UNIX et PF_UNIX.
 */
#ifndef local_socket_h
#define local_socket_h
#include <stddef.h>
#include <sys/types.h>
#include <stdint.h>
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
 * @brief Nombre maximal de `possibility_packet` tenant dans UNE offre de travail.
 *
 * Dérivé d'`ipc_max_datagram()` : octet de type + `IPC_WORK_OFFER_HEADER_SIZE`
 * + N paquets. Un datagramme AF_UNIX n'est jamais réassemblé — au-delà de cette
 * borne il faut plusieurs offres, jamais un message plus grand.
 *
 * @return Nombre de paquets par offre (>= 1 sur toute configuration supportée).
 */
size_t ipc_work_offer_max_packets(void);

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
 * @brief Transmet un message typé à tous les process fils.
 *
 * Cadre le datagramme comme le sens enfant → parent : un octet de type
 * (cf. `net/ipc_protocol.h`) suivi de `len` octets bruts. La longueur est
 * passée explicitement et JAMAIS redérivée par `strlen()` — un
 * `possibility_packet` contient des octets nuls et serait tronqué au premier.
 *
 * Sans effet si le process courant n'est pas le parent
 * (`parent_pid != getpid()`), comme `send_command_to_childs`.
 *
 * @param type    Type de message (`IPC_MSG_*`).
 * @param payload Charge utile (peut être NULL si `len == 0`).
 * @param len     Longueur de la charge utile, hors octet de type. Bornée par
 *                `ipc_max_datagram() - 1` : au-delà, rien n'est envoyé et
 *                l'appel est journalisé (le datagramme ne passerait pas, et
 *                une troncature silencieuse serait pire qu'un refus).
 * @return        Nombre de fils auxquels le message a été remis, 0 si aucun
 *                (non-parent, aucun fils enregistré, ou payload trop grand).
 */
int send_typed_to_childs(int8_t type, const void *payload, size_t len);

/**
 * @brief Transmet une commande aux process fils (cadre `IPC_MSG_COMMAND`).
 *
 * Enveloppe de `send_typed_to_childs` : la commande voyage sans son octet
 * nul terminal, sa longueur étant celle du datagramme.
 *
 * @param command commande à transmettre
 */
void send_command_to_childs(char *command);

/**
 * @brief Décode en place un datagramme reçu par un fils (fonction pure).
 *
 * Sépare l'octet de type de la charge utile et TERMINE cette dernière par un
 * octet nul, écrit à l'indice `nbytes` de `buf` — d'où l'exigence
 * `bufcap > nbytes`, que cette fonction vérifie au lieu de la supposer :
 * l'ancien récepteur écrivait ce nul à l'indice 100 d'un tampon de 100
 * octets dès qu'un datagramme remplissait exactement le tampon.
 *
 * Ne connaît aucun type : elle ne fait que découper. C'est à l'appelant de
 * décider quoi faire de `*out_type`.
 *
 * @param buf         Tampon reçu, modifié en place (ajout du terminateur).
 * @param bufcap      Capacité totale de `buf`, terminateur compris.
 * @param nbytes      Nombre d'octets effectivement reçus (retour de `recvfrom`).
 * @param out_type    Reçoit le type du message. Non écrit si le retour est 0.
 * @param out_payload Reçoit un pointeur dans `buf` sur la charge utile
 *                    NUL-terminée. Non écrit si le retour est 0.
 * @return            1 si le datagramme est exploitable, 0 sinon (reçu vide,
 *                    erreur de `recvfrom`, ou tampon trop court d'un octet
 *                    pour le terminateur).
 */
int ipc_child_frame_decode(char *buf, size_t bufcap, ssize_t nbytes,
                           int8_t *out_type, char **out_payload);
#endif // local_socket_h

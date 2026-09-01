#ifndef ipc_protocol_h
#define ipc_protocol_h

#include <stdint.h>

/*
 * Protocole IPC parent <-> enfants (UDP sur socket Unix domain).
 *
 * Chaque datagramme commence par un octet de type, suivi d'un payload dont
 * la structure dépend du type. Les deux sens empruntent des sockets
 * distinctes (`etii_main.<pid>` pour enfant → parent, `etii_fork.<pid>` pour
 * parent → enfant) mais partagent cette table de types : un octet ne
 * désigne jamais deux choses selon la direction.
 *
 * Cette indirection permet aux processus forkés (en mode client) de :
 *   - continuer à envoyer leurs statistiques (comportement historique) ;
 *   - faire remonter leurs logs au parent pour qu'ils s'affichent dans
 *     la console unique du parent (utile en particulier sous ncurses,
 *     où une écriture directe sur le terminal depuis un enfant
 *     corromprait l'affichage).
 *
 * La LONGUEUR de la charge utile est celle du datagramme, jamais un
 * `strlen()` : un payload binaire (un `possibility_packet` contient des
 * octets nuls) doit pouvoir transiter tel quel dans les deux sens.
 */

/* Types de messages — premier octet du datagramme. La direction est portée par
 * chaque entrée ci-dessous, jamais déduite de la valeur : `server_tcp`
 * (app_runtime.c) reçoit les messages enfant → parent, `fork_udp` les
 * messages parent → enfant. */
#define IPC_MSG_STATS        ((int8_t)1)  /* suivi de struct client_statistics */
#define IPC_MSG_LOG_INFO     ((int8_t)2)  /* suivi d'une chaîne UTF-8           */
#define IPC_MSG_LOG_ERROR    ((int8_t)3)  /* idem (destinée à stderr en ANSI)   */
#define IPC_MSG_LOG_DEBUG    ((int8_t)4)  /* idem                               */
#define IPC_MSG_LOG_CONSOLE  ((int8_t)5)  /* idem (sortie interactive)          */
#define IPC_MSG_EVENT        ((int8_t)6)  /* idem, sans horodatage (parent l'ajoute) */
/* suivi d'un struct possibility_packet brut (représentation du plateau) :
 * émis par fork_checker UNIQUEMENT quand ce fork bat son propre record local
 * (best_board_try_record sur g_search_best_board renvoie 1), jamais à chaque
 * tour comme IPC_MSG_STATS — cf. core/best_board.h. */
#define IPC_MSG_BEST_BOARD   ((int8_t)7)

/* Parent → enfant. Suivi d'une ligne de commande console, NON terminée par un
 * octet nul : sa longueur est celle du datagramme. Le récepteur (`fork_udp`) la
 * termine lui-même avant de la passer à `do_command_line`. Émis par
 * `send_command_to_childs` (local_socket.c) pour les commandes marquées
 * « propagées aux enfants » et par la ré-application de configuration de
 * `fork_orchestrator.c`. */
#define IPC_MSG_COMMAND      ((int8_t)8)

/* Enfant → parent. Offre de travail : un fils cède au courtier du parent des
 * possibilités qu'il aurait sinon envoyées lui-même au serveur. Charge utile :
 *   int32 seq    — numéro d'offre, strictement croissant PAR FILS ;
 *   int32 count  — nombre de paquets qui suivent ;
 *   count × struct possibility_packet.
 * `count` est borné par `ipc_work_offer_max_packets()` (net/local_socket.h) :
 * un datagramme AF_UNIX n'est jamais réassemblé. */
#define IPC_MSG_WORK_OFFER   ((int8_t)9)

/* En-tête d'une offre : seq (int32) + count (int32), hors octet de type. */
#define IPC_WORK_OFFER_HEADER_SIZE 8

/* Parent → enfant. Suivi d'un `int32` : le plus grand `seq` d'offre de CE fils
 * que le courtier a rendu DURABLE, c'est-à-dire poussé au serveur. Le fils s'en
 * sert pour lever son blocage d'acquittement (cf. `work_broker_ack_allowed`,
 * app/work_broker.h). */
#define IPC_MSG_WORK_SETTLED ((int8_t)10)

/* Taille maximale du payload texte (hors octet de type). */
#define IPC_LINE_MAX 4000

#endif /* ipc_protocol_h */

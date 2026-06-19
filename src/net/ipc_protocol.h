#ifndef ipc_protocol_h
#define ipc_protocol_h

#include <stdint.h>

/*
 * Protocole IPC enfant → parent (UDP sur socket Unix domain).
 *
 * Chaque datagramme commence par un octet de type, suivi d'un payload dont
 * la structure dépend du type. Le receiver côté parent (`server_tcp` dans
 * main.c) discrimine sur ce premier octet.
 *
 * Cette indirection permet aux processus forkés (en mode client) de :
 *   - continuer à envoyer leurs statistiques (comportement historique) ;
 *   - faire remonter leurs logs au parent pour qu'ils s'affichent dans
 *     la console unique du parent (utile en particulier sous ncurses,
 *     où une écriture directe sur le terminal depuis un enfant
 *     corromprait l'affichage).
 */

/* Types de messages — premier octet du datagramme. */
#define IPC_MSG_STATS        ((int8_t)1)  /* suivi de struct client_statistics */
#define IPC_MSG_LOG_INFO     ((int8_t)2)  /* suivi d'une chaîne UTF-8           */
#define IPC_MSG_LOG_ERROR    ((int8_t)3)  /* idem (destinée à stderr en ANSI)   */
#define IPC_MSG_LOG_DEBUG    ((int8_t)4)  /* idem                               */
#define IPC_MSG_LOG_CONSOLE  ((int8_t)5)  /* idem (sortie interactive)          */
#define IPC_MSG_EVENT        ((int8_t)6)  /* idem, sans horodatage (parent l'ajoute) */

/* Taille maximale du payload texte (hors octet de type). */
#define IPC_LINE_MAX 4000

#endif /* ipc_protocol_h */

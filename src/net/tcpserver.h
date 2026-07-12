#ifndef eternityII_tcpserver_h
#define eternityII_tcpserver_h

#include <stdint.h>

/**
 * @brief Crée et met en écoute un socket TCP serveur sur une adresse donnée,
 *        sans jamais appeler `exit` en cas d'échec.
 *
 * Même comportement que `create_tcp_server`, mais l'adresse locale est
 * paramétrable (ex. `INADDR_LOOPBACK` pour un service admin qui ne doit pas
 * être exposé hors de la machine) et toute erreur (création, bind, listen)
 * est remontée par un retour -1 (socket fermée avant le retour) au lieu de
 * terminer le processus — laissé au jugement de l'appelant.
 *
 * @param s_addr_host_order Adresse locale à lier, en ordre hôte (ex. `INADDR_ANY`,
 *                           `INADDR_LOOPBACK`) — convertie en ordre réseau en interne.
 * @param port               Port TCP sur lequel écouter.
 * @param nb_max_clients     Longueur maximale de la file d'attente de connexions.
 * @return                   Descripteur de la socket en écoute, ou -1 en cas d'erreur.
 */
int create_tcp_server_bound(uint32_t s_addr_host_order, int port, int nb_max_clients);

/**
 * @brief Crée et met en écoute un socket TCP serveur.
 *
 * Crée une socket SOCK_STREAM, active SO_REUSEADDR, la lie à `INADDR_ANY`
 * sur `port`, puis la passe en écoute avec `nb_max_clients` connexions en
 * attente. Quitte le programme (`exit`) en cas d'erreur de création, bind ou listen.
 *
 * @param port           Port TCP sur lequel écouter.
 * @param nb_max_clients Longueur maximale de la file d'attente de connexions.
 * @return               Descripteur de la socket en écoute.
 */
int create_tcp_server(int port, int nb_max_clients);

#endif

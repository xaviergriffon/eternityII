#ifndef eternityII_tcpserver_h
#define eternityII_tcpserver_h

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

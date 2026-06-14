#ifndef eternityII_tcpclient_h
#define eternityII_tcpclient_h

/**
 * @brief Ouvre une connexion TCP vers un serveur distant.
 *
 * Résout le nom d'hôte, crée une socket SOCK_STREAM, configure les timeouts
 * d'émission/réception (`tcp_timeout`), puis tente de se connecter jusqu'à
 * 10 fois (pause 1 s entre chaque tentative).
 *
 * @param hostname Nom d'hôte ou adresse IP du serveur.
 * @param port     Port TCP du serveur.
 * @return         Descripteur de socket en cas de succès, -1 en cas d'échec.
 */
int create_tcp_client(const char *hostname, int port);

#endif

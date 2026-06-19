/*
 * Stubs de test : symboles externes réclamés au link par les modules de
 * production sous test, mais fournis ailleurs par du code non testable en
 * unitaire (sockets).
 *
 * datamanager.c est désormais lié pour de vrai (il fournit add_possibility et
 * les opérations sur les files). Son seul symbole externe non couvert par les
 * autres modules de test est create_tcp_client (ouverture de connexion TCP,
 * chemin réseau non exercé) : on le remplace par un no-op.
 */
#include "../tcpclient.h"

int create_tcp_client(const char *hostname, int port)
{
    (void)hostname;
    (void)port;
    return -1; /* « pas de connexion » : aucun chemin réseau n'est testé */
}

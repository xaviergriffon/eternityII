/*
 * Stubs de test : symboles externes réclamés au link par les modules de
 * production sous test, mais fournis ailleurs par du code non testable en
 * unitaire (sockets, files mutex-protégées).
 *
 * possibility.c n'appelle qu'un seul symbole de datamanager.c (`add_possibility`,
 * depuis first_possibility). On le remplace par un no-op pour pouvoir lier la
 * logique pure de possibility.c sans tirer toute la chaîne datamanager →
 * tcpclient → sockets.
 */
#include "../datamanager.h"

int add_possibility(client_possibility_t *client_possibility,
                    array_possibility_packet *possibilities)
{
    (void)client_possibility;
    (void)possibilities;
    return 0;
}

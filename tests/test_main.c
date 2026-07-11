/*
 * Point d'entrée unique de la suite de tests unitaires (framework greatest).
 *
 * Chaque module testé expose une SUITE dans son propre fichier test_<module>.c.
 * Ce fichier les enregistre et lance le runner. C'est le SEUL fichier qui
 * invoque GREATEST_MAIN_DEFS() (définitions de l'état global du framework).
 *
 * Compilation : voir la cible `make test` à la racine.
 */
#include "greatest.h"

/* Suites définies dans les autres fichiers de test. */
SUITE_EXTERN(lifo_suite);
SUITE_EXTERN(part_suite);
SUITE_EXTERN(readdata_suite);
SUITE_EXTERN(command_history_suite);
SUITE_EXTERN(possibility_suite);
SUITE_EXTERN(etii_protocol_suite);
SUITE_EXTERN(control_protocol_suite);
SUITE_EXTERN(command_match_suite);
SUITE_EXTERN(datamanager_suite);
SUITE_EXTERN(local_socket_suite);
SUITE_EXTERN(tcp_suite);
SUITE_EXTERN(logger_suite);
SUITE_EXTERN(command_lines_suite);
SUITE_EXTERN(console_suite);
SUITE_EXTERN(etii_search_suite);
SUITE_EXTERN(static_variables_suite);
SUITE_EXTERN(etii_client_suite);
SUITE_EXTERN(etii_server_suite);
SUITE_EXTERN(control_registry_suite);
SUITE_EXTERN(app_runtime_suite);
#if ETERN_PARTS == 16
/* Suite « solution réelle » : n'existe que dans le build 4×4 (cf. test-16). */
SUITE_EXTERN(solution16_suite);
#endif

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN(); /* parse les arguments, init le runner */
    RUN_SUITE(lifo_suite);
    RUN_SUITE(part_suite);
    RUN_SUITE(readdata_suite);
    RUN_SUITE(command_history_suite);
    RUN_SUITE(possibility_suite);
    RUN_SUITE(etii_protocol_suite);
    RUN_SUITE(control_protocol_suite);
    RUN_SUITE(command_match_suite);
    RUN_SUITE(datamanager_suite);
    RUN_SUITE(local_socket_suite);
    RUN_SUITE(tcp_suite);
    RUN_SUITE(logger_suite);
    RUN_SUITE(command_lines_suite);
    RUN_SUITE(console_suite);
    RUN_SUITE(etii_search_suite);
    RUN_SUITE(static_variables_suite);
    RUN_SUITE(etii_client_suite);
    RUN_SUITE(etii_server_suite);
    RUN_SUITE(control_registry_suite);
    RUN_SUITE(app_runtime_suite);
#if ETERN_PARTS == 16
    RUN_SUITE(solution16_suite);
#endif
    GREATEST_MAIN_END(); /* affiche le récap et retourne le code de sortie */
}

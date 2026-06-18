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

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN(); /* parse les arguments, init le runner */
    RUN_SUITE(lifo_suite);
    RUN_SUITE(part_suite);
    RUN_SUITE(readdata_suite);
    GREATEST_MAIN_END(); /* affiche le récap et retourne le code de sortie */
}

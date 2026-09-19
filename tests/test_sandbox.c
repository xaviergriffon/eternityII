/*
 * Garde-fou d'isolation du runner (cf. tests/sandbox.h pour le POURQUOI).
 *
 * Cette suite est jouée EN DERNIER par tests/test_main.c : elle constate, une
 * fois toutes les autres suites passées, que le répertoire de LANCEMENT (la
 * racine du dépôt sous `make test`) est resté strictement identique.
 *
 * C'est la partie qui « mord » : le bac à sable protège les chemins relatifs,
 * mais rien n'empêcherait un futur test d'écrire par chemin absolu, ou de faire
 * un chdir vers la racine sans revenir. Les deux cas font échouer cette suite en
 * nommant le fichier apparu. Vérifié par sabotage (un test écrivant dans
 * test_sandbox_origin() fait bien tomber le garde-fou).
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "greatest.h"
#include "sandbox.h"

/*
 * Le runner tourne bien dans le bac à sable, et y est TOUJOURS au moment du
 * bilan. Un test qui se déplace (plusieurs le font, pour écrire leurs
 * artefacts) doit revenir : sans ce contrôle, un oubli de restauration
 * déplacerait silencieusement toutes les écritures relatives des suites
 * suivantes — y compris, un jour, vers la racine du dépôt.
 */
TEST runner_stays_in_its_temporary_sandbox(void)
{
    char cwd[4096];
    ASSERT(getcwd(cwd, sizeof cwd) != NULL);
    ASSERT_STR_EQ(test_sandbox_dir(), cwd);
    ASSERT(strcmp(test_sandbox_origin(), cwd) != 0);
    PASS();
}

/* Le répertoire de lancement n'a ni gagné ni perdu la moindre entrée. */
TEST launch_directory_is_left_untouched(void)
{
    char details[2048];
    int diffs = test_sandbox_origin_diff(details, sizeof details);

    if (diffs != 0) {
        static char msg[2304];
        snprintf(msg, sizeof msg,
                 "le répertoire de lancement (%s) a changé pendant les tests "
                 "[%d écart(s) : %s] — un test écrit hors du bac à sable",
                 test_sandbox_origin(), diffs, details);
        FAILm(msg);
    }
    PASS();
}

SUITE(sandbox_suite)
{
    RUN_TEST(runner_stays_in_its_temporary_sandbox);
    RUN_TEST(launch_directory_is_left_untouched);
}

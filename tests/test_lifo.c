/*
 * Tests unitaires du module lifo.c (File chaînée + big_table dynamique).
 *
 * lifo.c est totalement autonome (aucune dépendance autre que la libc), c'est
 * donc le module idéal pour valider la chaîne de test de bout en bout.
 */
#include "greatest.h"
#include "../lifo.h"

/* --------------------------------------------------------------------------
 * File (liste chaînée doublement liée, exploitée en LIFO par put/scroll)
 * ------------------------------------------------------------------------ */

/* put empile en fin, scroll dépile en fin => ordre LIFO. */
TEST file_put_then_scroll_is_lifo(void)
{
    File f;
    init_file_with_cache(&f, 0, sizeof(int)); /* sans cache : malloc/free purs */

    int v;
    for (int i = 1; i <= 3; i++) {
        ASSERT_EQ_FMT(1, put(&f, &i), "%d");
    }
    ASSERT(f.size == 3);

    ASSERT_EQ_FMT(1, scroll(&f, &v), "%d");
    ASSERT_EQ_FMT(3, v, "%d");
    ASSERT_EQ_FMT(1, scroll(&f, &v), "%d");
    ASSERT_EQ_FMT(2, v, "%d");
    ASSERT_EQ_FMT(1, scroll(&f, &v), "%d");
    ASSERT_EQ_FMT(1, v, "%d");

    ASSERT(f.size == 0);
    PASS();
}

/* scroll sur une file vide retourne 0 et ne touche pas la destination. */
TEST file_scroll_on_empty_returns_zero(void)
{
    File f;
    init_file_with_cache(&f, 0, sizeof(int));

    int v = 42;
    ASSERT_EQ_FMT(0, scroll(&f, &v), "%d");
    ASSERT_EQ_FMT(42, v, "%d"); /* inchangé */
    PASS();
}

/* Avec un cache pré-alloué, les premiers éléments réutilisent le bloc, les
 * suivants sont alloués dynamiquement ; l'ordre LIFO doit rester correct
 * de part et d'autre de la frontière du cache. */
TEST file_cache_mixes_preallocated_and_heap(void)
{
    File f;
    init_file_with_cache(&f, 4, sizeof(int)); /* cache de 4, on en pousse 6 */

    int v;
    for (int i = 0; i < 6; i++) {
        ASSERT_EQ_FMT(1, put(&f, &i), "%d");
    }
    ASSERT(f.size == 6);

    for (int expected = 5; expected >= 0; expected--) {
        ASSERT_EQ_FMT(1, scroll(&f, &v), "%d");
        ASSERT_EQ_FMT(expected, v, "%d");
    }
    ASSERT_EQ_FMT(0, scroll(&f, &v), "%d");
    PASS();
}

/* --------------------------------------------------------------------------
 * big_table (tableau dynamique à plat qui double de capacité au besoin)
 * ------------------------------------------------------------------------ */

/* put_big_table déclenche au moins un redimensionnement et préserve les valeurs ;
 * scroll_big_table les ressort en LIFO. */
TEST big_table_grows_and_preserves_values(void)
{
    big_table t;
    init_big_table(&t, 2, sizeof(int)); /* capacité initiale = 2 */

    for (int i = 0; i < 5; i++) {
        put_big_table(&t, &i); /* force 2 -> 4 -> 8 */
    }
    ASSERT(t.size == 5);
    ASSERT(t.realsize >= 5); /* a bien grandi */

    int v;
    for (int expected = 4; expected >= 0; expected--) {
        ASSERT_EQ_FMT(1, scroll_big_table(&t, &v), "%d");
        ASSERT_EQ_FMT(expected, v, "%d");
    }
    ASSERT_EQ_FMT(0, scroll_big_table(&t, &v), "%d");

    clear_big_table(&t); /* table sur la pile : libère seulement le buffer interne */
    PASS();
}

SUITE(lifo_suite)
{
    RUN_TEST(file_put_then_scroll_is_lifo);
    RUN_TEST(file_scroll_on_empty_returns_zero);
    RUN_TEST(file_cache_mixes_preallocated_and_heap);
    RUN_TEST(big_table_grows_and_preserves_values);
}

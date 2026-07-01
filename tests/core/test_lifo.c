/*
 * Tests unitaires du module lifo.c (File chaînée + big_table dynamique).
 *
 * lifo.c est totalement autonome (aucune dépendance autre que la libc), c'est
 * donc le module idéal pour valider la chaîne de test de bout en bout.
 */
#include "greatest.h"
#include "core/lifo.h"

/* Fonctions internes non exposées dans lifo.h mais à visibilité de link :
   on les déclare ici pour les exercer directement. */
int  scroll_fifo(File *suite, void *dest);
void extract_element(File *suite, Element *element);

/* --------------------------------------------------------------------------
 * File (liste chaînée doublement liée, exploitée en LIFO par put/scroll)
 * ------------------------------------------------------------------------ */

/* put empile en fin, scroll dépile en fin => ordre LIFO. */
TEST file_put_then_scroll_is_lifo(void)
{
    File f;
    init_file(&f, sizeof(int));

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
    init_file(&f, sizeof(int));

    int v = 42;
    ASSERT_EQ_FMT(0, scroll(&f, &v), "%d");
    ASSERT_EQ_FMT(42, v, "%d"); /* inchangé */
    PASS();
}

/* scroll_fifo dépile en tête => ordre FIFO (1, 2, 3 dans l'ordre d'insertion). */
TEST file_scroll_fifo_is_fifo(void)
{
    File f;
    init_file(&f, sizeof(int));

    for (int i = 1; i <= 3; i++) {
        put(&f, &i);
    }

    int v;
    ASSERT_EQ_FMT(1, scroll_fifo(&f, &v), "%d");
    ASSERT_EQ_FMT(1, v, "%d");
    ASSERT_EQ_FMT(1, scroll_fifo(&f, &v), "%d");
    ASSERT_EQ_FMT(2, v, "%d");
    ASSERT_EQ_FMT(1, scroll_fifo(&f, &v), "%d");
    ASSERT_EQ_FMT(3, v, "%d");

    ASSERT_EQ_FMT(0, scroll_fifo(&f, &v), "%d"); /* file vide */
    PASS();
}

/* move_before déplace un élément juste avant un autre : [1,2,3] -> 3 avant 1 -> [3,1,2]. */
TEST file_move_before_reorders(void)
{
    File f;
    init_file(&f, sizeof(int));
    for (int i = 1; i <= 3; i++) put(&f, &i);

    Element *e1 = f.start;
    Element *e3 = f.end;
    move_before(&f, e3, e1); /* déplace 3 devant 1 */

    int v;
    scroll_fifo(&f, &v); ASSERT_EQ_FMT(3, v, "%d");
    scroll_fifo(&f, &v); ASSERT_EQ_FMT(1, v, "%d");
    scroll_fifo(&f, &v); ASSERT_EQ_FMT(2, v, "%d");
    PASS();
}

/* move_after déplace un élément juste après un autre : [1,2,3] -> 1 après 3 -> [2,3,1]. */
TEST file_move_after_reorders(void)
{
    File f;
    init_file(&f, sizeof(int));
    for (int i = 1; i <= 3; i++) put(&f, &i);

    Element *e1 = f.start;
    Element *e3 = f.end;
    move_after(&f, e1, e3); /* déplace 1 après 3 */

    int v;
    scroll_fifo(&f, &v); ASSERT_EQ_FMT(2, v, "%d");
    scroll_fifo(&f, &v); ASSERT_EQ_FMT(3, v, "%d");
    scroll_fifo(&f, &v); ASSERT_EQ_FMT(1, v, "%d");
    PASS();
}

/* move_before / move_after ignorent un argument NULL (element ou target) :
   la liste reste inchangée. Couvre la garde `element != NULL && target != NULL`. */
TEST file_move_ignores_null_args(void)
{
    File f;
    init_file(&f, sizeof(int));
    for (int i = 1; i <= 3; i++) put(&f, &i);
    Element *e1 = f.start;
    Element *e3 = f.end;

    /* Chaque appel avec un NULL doit être un no-op. */
    move_before(&f, NULL, e1);
    move_before(&f, e3, NULL);
    move_after(&f, NULL, e1);
    move_after(&f, e3, NULL);

    /* Ordre initial [1,2,3] préservé. */
    int v;
    scroll_fifo(&f, &v); ASSERT_EQ_FMT(1, v, "%d");
    scroll_fifo(&f, &v); ASSERT_EQ_FMT(2, v, "%d");
    scroll_fifo(&f, &v); ASSERT_EQ_FMT(3, v, "%d");
    PASS();
}

/* extract_element détache l'élément du milieu : les voisins se relient,
   start/end restent corrects. (extract ne décrémente pas size : nettoyage manuel.) */
TEST file_extract_element_detaches_middle(void)
{
    File f;
    init_file(&f, sizeof(int));
    for (int i = 1; i <= 3; i++) put(&f, &i);

    Element *e1 = f.start;
    Element *e2 = e1->next;
    Element *e3 = f.end;

    extract_element(&f, e2);

    /* e2 détaché, e1 et e3 désormais voisins directs. */
    ASSERT_EQ(NULL, e2->previous);
    ASSERT_EQ(NULL, e2->next);
    ASSERT_EQ(e3, e1->next);
    ASSERT_EQ(e1, e3->previous);
    ASSERT_EQ(e1, f.start);
    ASSERT_EQ(e3, f.end);

    /* Nettoyage manuel (size encore à 3, mais seuls 3 blocs alloués). */
    free(e1->value); free(e1);
    free(e2->value); free(e2);
    free(e3->value); free(e3);
    PASS();
}

/* free_file libère une File allouée sur le tas (structure + éléments).
   On vérifie surtout l'absence de crash/fuite (probant sous AddressSanitizer). */
TEST file_free_file_releases_heap_allocated_file(void)
{
    File *f = malloc(sizeof(File));
    init_file(f, sizeof(int));
    for (int i = 0; i < 5; i++) put(f, &i);

    free_file(f); /* doit tout libérer sans planter */
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

/* scroll_big_table_cache renvoie un pointeur direct vers le dernier élément
   (LIFO) sans copie, et décrémente la taille logique. */
TEST big_table_scroll_cache_returns_internal_pointer(void)
{
    big_table t;
    init_big_table(&t, 4, sizeof(int));
    for (int i = 0; i < 3; i++) put_big_table(&t, &i); /* 0,1,2 */

    int *p = (int *)scroll_big_table_cache(&t);
    ASSERT(p != NULL);
    ASSERT_EQ_FMT(2, *p, "%d");
    ASSERT_EQ_FMT(2ULL, (unsigned long long)t.size, "%llu");

    /* big_table vidée jusqu'au bout puis re-vide -> NULL. */
    scroll_big_table_cache(&t);
    scroll_big_table_cache(&t);
    ASSERT_EQ(NULL, scroll_big_table_cache(&t));

    clear_big_table(&t);
    PASS();
}

/* put refuse une valeur de taille nulle (sizeofvalue <= 0) -> 0. */
TEST file_put_rejects_zero_sizeofvalue(void)
{
    File f;
    init_file(&f, 0); /* sizeofvalue = 0 */
    int x = 1;
    ASSERT_EQ_FMT(0, put(&f, &x), "%d");
    ASSERT_EQ_FMT(0ULL, (unsigned long long)f.size, "%llu");
    PASS();
}

/* move_before/move_after sur une cible interne (voisin non NULL) : couvre les
   branches où targetPrevious / targetNext ne sont pas NULL. */
TEST file_move_targets_non_extremity(void)
{
    File f;
    init_file(&f, sizeof(int));
    for (int i = 1; i <= 3; i++) put(&f, &i); /* [1,2,3] */
    Element *e2 = f.start->next;
    Element *e3 = f.end;
    move_before(&f, e3, e2); /* 3 avant 2 (cible interne) -> [1,3,2] */

    int v;
    scroll_fifo(&f, &v); ASSERT_EQ_FMT(1, v, "%d");
    scroll_fifo(&f, &v); ASSERT_EQ_FMT(3, v, "%d");
    scroll_fifo(&f, &v); ASSERT_EQ_FMT(2, v, "%d");

    /* nouveau jeu pour move_after sur cible interne */
    init_file(&f, sizeof(int));
    for (int i = 1; i <= 3; i++) put(&f, &i); /* [1,2,3] */
    Element *e1 = f.start;
    Element *mid = f.start->next; /* e2, cible interne (next != NULL) */
    move_after(&f, e1, mid);      /* 1 après 2 -> [2,1,3] */
    scroll_fifo(&f, &v); ASSERT_EQ_FMT(2, v, "%d");
    scroll_fifo(&f, &v); ASSERT_EQ_FMT(1, v, "%d");
    scroll_fifo(&f, &v); ASSERT_EQ_FMT(3, v, "%d");
    PASS();
}

/* file_remove_element supprime l'élément du milieu d'une File de 3 :
   size passe à 2, les voisins se relient correctement. */
TEST file_remove_element_removes_middle(void)
{
    File f;
    init_file(&f, sizeof(int));
    for (int i = 1; i <= 3; i++) put(&f, &i); /* [1,2,3] */

    Element *e1 = f.start;
    Element *e2 = e1->next;
    Element *e3 = f.end;

    file_remove_element(&f, e2);

    ASSERT_EQ_FMT(2ULL, (unsigned long long)f.size, "%llu");
    ASSERT_EQ(e1, f.start);
    ASSERT_EQ(e3, f.end);
    ASSERT_EQ(e3, e1->next);
    ASSERT_EQ(e1, e3->previous);

    /* Vide la file proprement. */
    int v;
    scroll(&f, &v); ASSERT_EQ_FMT(3, v, "%d");
    scroll(&f, &v); ASSERT_EQ_FMT(1, v, "%d");
    PASS();
}

/* free_big_table libère un big_table alloué sur le tas (structure + buffer). */
TEST big_table_free_big_table_heap(void)
{
    big_table *t = malloc(sizeof(big_table));
    init_big_table(t, 2, sizeof(int));
    for (int i = 0; i < 5; i++) put_big_table(t, &i);
    free_big_table(t); /* doit tout libérer sans planter */
    PASS();
}

SUITE(lifo_suite)
{
    RUN_TEST(file_put_then_scroll_is_lifo);
    RUN_TEST(file_scroll_on_empty_returns_zero);
    RUN_TEST(file_scroll_fifo_is_fifo);
    RUN_TEST(file_move_before_reorders);
    RUN_TEST(file_move_after_reorders);
    RUN_TEST(file_move_ignores_null_args);
    RUN_TEST(file_extract_element_detaches_middle);
    RUN_TEST(file_free_file_releases_heap_allocated_file);
    RUN_TEST(big_table_grows_and_preserves_values);
    RUN_TEST(big_table_scroll_cache_returns_internal_pointer);
    RUN_TEST(file_put_rejects_zero_sizeofvalue);
    RUN_TEST(file_move_targets_non_extremity);
    RUN_TEST(big_table_free_big_table_heap);
    RUN_TEST(file_remove_element_removes_middle);
}

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
int  inside_cache(File *file, Element *element);
long position_cache(File *file, Element *element);

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

/* scroll_fifo dépile en tête => ordre FIFO (1, 2, 3 dans l'ordre d'insertion). */
TEST file_scroll_fifo_is_fifo(void)
{
    File f;
    init_file_with_cache(&f, 0, sizeof(int));

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
    init_file_with_cache(&f, 0, sizeof(int));
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
    init_file_with_cache(&f, 0, sizeof(int));
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

/* extract_element détache l'élément du milieu : les voisins se relient,
   start/end restent corrects. (extract ne décrémente pas size : nettoyage manuel.) */
TEST file_extract_element_detaches_middle(void)
{
    File f;
    init_file_with_cache(&f, 0, sizeof(int));
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

/* inside_cache / position_cache : un élément du bloc pré-alloué est "dans le
   cache" ; un élément alloué dynamiquement (au-delà du cache) ne l'est pas. */
TEST file_inside_cache_distinguishes_cached_and_heap(void)
{
    File *f = malloc(sizeof(File));
    init_file_with_cache(f, 4, sizeof(int)); /* cache de 4 */
    for (int i = 0; i < 6; i++) put(f, &i);  /* 4 cache + 2 tas */

    Element *cached = f->start;                      /* cacheElement[0] */
    Element *heap   = f->start->next->next->next->next; /* 5e élément, hors cache */

    ASSERT(inside_cache(f, cached));
    ASSERT_FALSE(inside_cache(f, heap));
    ASSERT_EQ_FMT(-1L, position_cache(f, heap), "%ld"); /* sentinelle hors cache */

    free_file(f);
    PASS();
}

/* scroll_cache renvoie un pointeur direct vers la valeur de l'élément dépilé
   (LIFO), sans copie. Utilisé avec cache pour éviter la fuite du chemin tas. */
TEST file_scroll_cache_returns_internal_pointer(void)
{
    File *f = malloc(sizeof(File));
    init_file_with_cache(f, 3, sizeof(int));
    for (int i = 10; i <= 30; i += 10) put(f, &i);

    int *p = (int *)scroll_cache(f);
    ASSERT(p != NULL);
    ASSERT_EQ_FMT(30, *p, "%d"); /* dernier inséré */
    ASSERT_EQ_FMT(2ULL, (unsigned long long)f->size, "%llu");

    free_file(f);
    PASS();
}

/* free_file libère une File allouée sur le tas (structure + cache + éléments).
   On vérifie surtout l'absence de crash/fuite (probant sous AddressSanitizer). */
TEST file_free_file_releases_heap_allocated_file(void)
{
    File *f = malloc(sizeof(File));
    init_file_with_cache(f, 2, sizeof(int)); /* cache + débordement tas */
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
    init_file_with_cache(&f, 0, 0); /* sizeofvalue = 0 */
    int x = 1;
    ASSERT_EQ_FMT(0, put(&f, &x), "%d");
    ASSERT_EQ_FMT(0ULL, (unsigned long long)f.size, "%llu");
    PASS();
}

/* scroll_cache : NULL sur file vide, et remise à zéro de start/end au dernier élément. */
TEST file_scroll_cache_empty_and_full_drain(void)
{
    File *f = malloc(sizeof(File));
    init_file_with_cache(f, 2, sizeof(int));
    ASSERT_EQ(NULL, scroll_cache(f)); /* vide d'emblée */

    int a = 1, b = 2;
    put(f, &a);
    put(f, &b);
    ASSERT(scroll_cache(f) != NULL);
    ASSERT(scroll_cache(f) != NULL); /* dernier -> start/end remis à NULL */
    ASSERT_EQ_FMT(0ULL, (unsigned long long)f->size, "%llu");
    ASSERT_EQ(NULL, scroll_cache(f));

    free_file(f);
    PASS();
}

/* position_cache renvoie l'offset (>= 0) d'un élément appartenant au cache. */
TEST file_position_cache_inside(void)
{
    File *f = malloc(sizeof(File));
    init_file_with_cache(f, 4, sizeof(int));
    int v = 7;
    put(f, &v);
    Element *cached = f->start; /* cacheElement[0] */
    ASSERT(inside_cache(f, cached));
    ASSERT(position_cache(f, cached) >= 0); /* dans le cache -> pas la sentinelle -1 */
    free_file(f);
    PASS();
}

/* scroll_fifo emprunte le chemin « cache » quand les éléments sont pré-alloués. */
TEST file_scroll_fifo_with_cache(void)
{
    File f;
    init_file_with_cache(&f, 4, sizeof(int));
    for (int i = 1; i <= 3; i++) put(&f, &i);

    int v;
    ASSERT_EQ_FMT(1, scroll_fifo(&f, &v), "%d"); ASSERT_EQ_FMT(1, v, "%d");
    ASSERT_EQ_FMT(1, scroll_fifo(&f, &v), "%d"); ASSERT_EQ_FMT(2, v, "%d");
    ASSERT_EQ_FMT(1, scroll_fifo(&f, &v), "%d"); ASSERT_EQ_FMT(3, v, "%d");
    PASS();
}

/* move_before/move_after sur une cible interne (voisin non NULL) : couvre les
   branches où targetPrevious / targetNext ne sont pas NULL. */
TEST file_move_targets_non_extremity(void)
{
    File f;
    init_file_with_cache(&f, 0, sizeof(int));
    for (int i = 1; i <= 3; i++) put(&f, &i); /* [1,2,3] */
    Element *e2 = f.start->next;
    Element *e3 = f.end;
    move_before(&f, e3, e2); /* 3 avant 2 (cible interne) -> [1,3,2] */

    int v;
    scroll_fifo(&f, &v); ASSERT_EQ_FMT(1, v, "%d");
    scroll_fifo(&f, &v); ASSERT_EQ_FMT(3, v, "%d");
    scroll_fifo(&f, &v); ASSERT_EQ_FMT(2, v, "%d");

    /* nouveau jeu pour move_after sur cible interne */
    init_file_with_cache(&f, 0, sizeof(int));
    for (int i = 1; i <= 3; i++) put(&f, &i); /* [1,2,3] */
    Element *e1 = f.start;
    Element *mid = f.start->next; /* e2, cible interne (next != NULL) */
    move_after(&f, e1, mid);      /* 1 après 2 -> [2,1,3] */
    scroll_fifo(&f, &v); ASSERT_EQ_FMT(2, v, "%d");
    scroll_fifo(&f, &v); ASSERT_EQ_FMT(1, v, "%d");
    scroll_fifo(&f, &v); ASSERT_EQ_FMT(3, v, "%d");
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
    RUN_TEST(file_cache_mixes_preallocated_and_heap);
    RUN_TEST(file_scroll_fifo_is_fifo);
    RUN_TEST(file_move_before_reorders);
    RUN_TEST(file_move_after_reorders);
    RUN_TEST(file_extract_element_detaches_middle);
    RUN_TEST(file_inside_cache_distinguishes_cached_and_heap);
    RUN_TEST(file_scroll_cache_returns_internal_pointer);
    RUN_TEST(file_free_file_releases_heap_allocated_file);
    RUN_TEST(big_table_grows_and_preserves_values);
    RUN_TEST(big_table_scroll_cache_returns_internal_pointer);
    RUN_TEST(file_put_rejects_zero_sizeofvalue);
    RUN_TEST(file_scroll_cache_empty_and_full_drain);
    RUN_TEST(file_position_cache_inside);
    RUN_TEST(file_scroll_fifo_with_cache);
    RUN_TEST(file_move_targets_non_extremity);
    RUN_TEST(big_table_free_big_table_heap);
}

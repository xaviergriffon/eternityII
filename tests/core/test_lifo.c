/*
 * Tests unitaires du module lifo.c (File chaînée).
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

/* Après avoir vidé une file par scroll_fifo, put() doit repartir d'une file
 * réellement vide (start == end == NULL) : sinon suite->end pointe vers un
 * Element déjà libéré (use-after-free) et le put() suivant plante ou
 * corrompt la mémoire au lieu de recréer un premier élément. */
TEST file_scroll_fifo_resets_end_after_emptying(void)
{
    File f;
    init_file(&f, sizeof(int));

    for (int i = 1; i <= 3; i++) {
        put(&f, &i);
    }

    int v;
    for (int i = 0; i < 3; i++) {
        ASSERT_EQ_FMT(1, scroll_fifo(&f, &v), "%d");
    }
    ASSERT(f.size == 0);
    ASSERT(f.start == NULL);
    ASSERT(f.end == NULL);

    int again = 42;
    ASSERT_EQ_FMT(1, put(&f, &again), "%d");
    ASSERT(f.size == 1);

    ASSERT_EQ_FMT(1, scroll_fifo(&f, &v), "%d");
    ASSERT_EQ_FMT(42, v, "%d");
    ASSERT(f.size == 0);
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

/* Contrat de retour de put() : 1 en cas de succès nominal, 0 si la file n'est
 * pas exploitable (sizeofvalue <= 0, cf. file_put_rejects_zero_sizeofvalue
 * ci-dessus). put() durcit aussi le cas malloc(sizeof(Element)) == NULL (OOM)
 * en renvoyant 0 au lieu de déréférencer un pointeur NULL ; ce chemin n'est
 * pas déclenchable de façon portable sans mocker malloc, donc non couvert ici
 * (cf. le twin put_possibility() dans core/possibility.c, testé pareillement
 * sans simulation d'OOM). */
TEST file_put_returns_one_on_success(void)
{
    File f;
    init_file(&f, sizeof(int));

    int v = 7;
    ASSERT_EQ_FMT(1, put(&f, &v), "%d");
    ASSERT_EQ_FMT(1ULL, (unsigned long long)f.size, "%llu");
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

/* scroll : branche size!=0 mais end==NULL (état incohérent, jamais produit par
 * l'API publique mais couvre le garde-fou `size == 0 || end == NULL`). */
TEST file_scroll_inconsistent_state_returns_zero(void)
{
    File f;
    init_file(&f, sizeof(int));
    f.size = 1; /* incohérent : end reste NULL */

    int v = 99;
    ASSERT_EQ_FMT(0, scroll(&f, &v), "%d");
    ASSERT_EQ_FMT(99, v, "%d"); /* inchangé */
    PASS();
}

/* extract_element(NULL, …) : aucune File à mettre à jour, mais le recâblage
 * des voisins doit avoir lieu quel que soit le NULL. Testé sur l'élément de
 * TÊTE (previous == NULL, ligne 148) puis de QUEUE (next == NULL, ligne 157). */
TEST file_extract_element_null_suite_detaches_head(void)
{
    Element e1, e2;
    int v1 = 1, v2 = 2;
    e1.value = &v1; e1.previous = NULL; e1.next = &e2;
    e2.value = &v2; e2.previous = &e1;  e2.next = NULL;

    extract_element(NULL, &e1); /* e1 est en tête */

    ASSERT_EQ(NULL, e1.previous);
    ASSERT_EQ(NULL, e1.next);
    ASSERT_EQ(NULL, e2.previous); /* e2 devient tête, plus de précédent */
    PASS();
}

TEST file_extract_element_null_suite_detaches_tail(void)
{
    Element e1, e2;
    int v1 = 1, v2 = 2;
    e1.value = &v1; e1.previous = NULL; e1.next = &e2;
    e2.value = &v2; e2.previous = &e1;  e2.next = NULL;

    extract_element(NULL, &e2); /* e2 est en queue */

    ASSERT_EQ(NULL, e2.previous);
    ASSERT_EQ(NULL, e2.next);
    ASSERT_EQ(NULL, e1.next); /* e1 devient queue, plus de suivant */
    PASS();
}

/* move_before(NULL, …) avec une cible en TÊTE de sa propre liste (ligne 187) :
 * aucune File à mettre à jour (start/end restent volontairement non
 * synchronisés par la fonction elle-même, comme documenté), mais le
 * recâblage element/target doit réussir. On vérifie directement les
 * pointeurs previous/next plutôt que de rejouer via scroll_fifo (qui
 * dépend, lui, de File.start/end à jour). */
TEST file_move_before_null_suite_target_is_head(void)
{
    File f;
    init_file(&f, sizeof(int));
    for (int i = 1; i <= 3; i++) put(&f, &i);
    Element *e1 = f.start; /* tête : target->previous == NULL */
    Element *e2 = e1->next;
    Element *e3 = f.end;

    move_before(NULL, e3, e1); /* déplace 3 devant 1 : ordre réel e3,e1,e2 */

    ASSERT_EQ(NULL, e3->previous);   /* e3 est maintenant en tête */
    ASSERT_EQ(e1, e3->next);
    ASSERT_EQ(e3, e1->previous);
    ASSERT_EQ(e2, e1->next);
    ASSERT_EQ(e1, e2->previous);
    ASSERT_EQ(NULL, e2->next);       /* e2 reste en queue */

    free(e1->value); free(e1);
    free(e2->value); free(e2);
    free(e3->value); free(e3);
    PASS();
}

/* move_after(NULL, …) avec une cible en QUEUE de sa propre liste (ligne 219). */
TEST file_move_after_null_suite_target_is_tail(void)
{
    File f;
    init_file(&f, sizeof(int));
    for (int i = 1; i <= 3; i++) put(&f, &i);
    Element *e1 = f.start;
    Element *e2 = e1->next;
    Element *e3 = f.end; /* queue : target->next == NULL */

    move_after(NULL, e1, e3); /* déplace 1 après 3 : ordre réel e2,e3,e1 */

    ASSERT_EQ(NULL, e2->previous);   /* e2 est maintenant en tête */
    ASSERT_EQ(e3, e2->next);
    ASSERT_EQ(e2, e3->previous);
    ASSERT_EQ(e1, e3->next);
    ASSERT_EQ(e3, e1->previous);
    ASSERT_EQ(NULL, e1->next);       /* e1 est maintenant en queue */

    free(e1->value); free(e1);
    free(e2->value); free(e2);
    free(e3->value); free(e3);
    PASS();
}

/* extract_element : element->previous == NULL mais suite->start != element
 * (orphelin hors liste) → branche FALSE de L148 ET L157 simultanément.
 * Vérifie que start/end de la suite restent inchangés. */
TEST file_extract_element_orphan_does_not_update_suite(void)
{
    File f;
    init_file(&f, sizeof(int));
    int a = 1, b = 2;
    put(&f, &a); put(&f, &b); /* f = [e1, e2] */
    Element *e1 = f.start;
    Element *e2 = f.end;

    /* Orphelin : previous==NULL et next==NULL → il n'appartient pas à f. */
    Element orphan;
    int ov = 99;
    orphan.value = &ov;
    orphan.previous = NULL;
    orphan.next = NULL;

    /* previous==NULL → L148 : suite->start == &orphan ? f->start=e1 ≠ &orphan → FALSE.
     * next==NULL     → L157 : suite->end == &orphan ?  f->end=e2  ≠ &orphan → FALSE. */
    extract_element(&f, &orphan);

    ASSERT_EQ(e1, f.start); /* suite inchangée */
    ASSERT_EQ(e2, f.end);
    ASSERT_EQ(NULL, orphan.previous);
    ASSERT_EQ(NULL, orphan.next);

    int v;
    scroll(&f, &v); ASSERT_EQ_FMT(2, v, "%d");
    scroll(&f, &v); ASSERT_EQ_FMT(1, v, "%d");
    PASS();
}

/* move_before : target->previous==NULL mais suite->start != target
 * → branche FALSE de L187.
 * On déplace e2 avant un orphelin hors liste. */
TEST file_move_before_target_not_head_of_suite(void)
{
    File f;
    init_file(&f, sizeof(int));
    int a = 1, b = 2;
    put(&f, &a); put(&f, &b); /* f = [e1, e2] */
    Element *e2 = f.end;

    Element orphan;
    int ov = 99;
    orphan.value = &ov;
    orphan.previous = NULL;
    orphan.next = NULL;

    /* Après extract de e2, f=[e1].  targetPrevious=orphan->previous=NULL.
     * f->start=e1 ≠ &orphan → FALSE au test L187. */
    move_before(&f, e2, &orphan);

    /* e2 câblé juste avant orphan */
    ASSERT_EQ(NULL, e2->previous);
    ASSERT_EQ(&orphan, e2->next);
    ASSERT_EQ(e2, orphan.previous);

    /* Nettoyage manuel : e1 reste dans f, e2 est décroché. */
    Element *e1 = f.start;
    free(e1->value); free(e1);
    free(e2->value); free(e2);
    PASS();
}

/* move_after : target->next==NULL mais suite->end != target
 * → branche FALSE de L219.
 * On déplace e1 après un orphelin hors liste. */
TEST file_move_after_target_not_tail_of_suite(void)
{
    File f;
    init_file(&f, sizeof(int));
    int a = 1, b = 2;
    put(&f, &a); put(&f, &b); /* f = [e1, e2] */
    Element *e1 = f.start;

    Element orphan;
    int ov = 99;
    orphan.value = &ov;
    orphan.previous = NULL;
    orphan.next = NULL;

    /* Après extract de e1, f=[e2].  targetNext=orphan->next=NULL.
     * f->end=e2 ≠ &orphan → FALSE au test L219. */
    move_after(&f, e1, &orphan);

    /* e1 câblé juste après orphan */
    ASSERT_EQ(&orphan, e1->previous);
    ASSERT_EQ(NULL, e1->next);
    ASSERT_EQ(e1, orphan.next);

    /* Nettoyage manuel : e2 reste dans f, e1 est décroché. */
    Element *e2 = f.start;
    free(e2->value); free(e2);
    free(e1->value); free(e1);
    PASS();
}

SUITE(lifo_suite)
{
    RUN_TEST(file_put_then_scroll_is_lifo);
    RUN_TEST(file_scroll_on_empty_returns_zero);
    RUN_TEST(file_scroll_fifo_is_fifo);
    RUN_TEST(file_scroll_fifo_resets_end_after_emptying);
    RUN_TEST(file_move_before_reorders);
    RUN_TEST(file_move_after_reorders);
    RUN_TEST(file_move_ignores_null_args);
    RUN_TEST(file_extract_element_detaches_middle);
    RUN_TEST(file_free_file_releases_heap_allocated_file);
    RUN_TEST(file_put_rejects_zero_sizeofvalue);
    RUN_TEST(file_put_returns_one_on_success);
    RUN_TEST(file_move_targets_non_extremity);
    RUN_TEST(file_remove_element_removes_middle);
    RUN_TEST(file_scroll_inconsistent_state_returns_zero);
    RUN_TEST(file_extract_element_null_suite_detaches_head);
    RUN_TEST(file_extract_element_null_suite_detaches_tail);
    RUN_TEST(file_move_before_null_suite_target_is_head);
    RUN_TEST(file_move_after_null_suite_target_is_tail);
    RUN_TEST(file_extract_element_orphan_does_not_update_suite);
    RUN_TEST(file_move_before_target_not_head_of_suite);
    RUN_TEST(file_move_after_target_not_tail_of_suite);
}

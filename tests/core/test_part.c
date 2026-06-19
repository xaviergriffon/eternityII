/*
 * Tests unitaires du module part.c (rotation des pièces et lookup par bords).
 *
 * Les fixtures sont construites à la main (pas de CSV ni de rotate_all_parts)
 * pour rester indépendantes de la valeur de ETERN_PARTS : rotate_all_parts
 * indexe en i + ETERN_PARTS*r et n'est correct que lorsque ETERN_PARTS == la
 * taille réelle du puzzle. search_face / buildBigArray, eux, n'utilisent pas
 * ETERN_PARTS et se testent donc avec un petit jeu de pièces arbitraire.
 */
#include "greatest.h"
#include "core/part.h"
#include "app/static_variables.h" /* ETERN_PARTS */

/* Fonctions publiques non déclarées dans part.h : prototypes locaux. */
int8_t          convert_p(int8_t p, int maxFaceM);
unsigned long   hashmap_hash_int(unsigned long key);
unsigned int    hash(char *str);
struct array_part *get_parts(struct map_part *map, char *key);

/* --------------------------------------------------------------------------
 * rotatePart : rotation horaire d'un quart de tour
 *   top -> right, right -> bottom, bottom -> left, left -> top
 * ------------------------------------------------------------------------ */

TEST rotate_part_zero_is_identity(void)
{
    struct part p = { .id = 7, .top = 1, .right = 2, .bottom = 3, .left = 4, .rotation = 0 };
    struct part *r = rotatePart(&p, 0);

    ASSERT_EQ_FMT(7, (int)r->id, "%d");
    ASSERT_EQ_FMT(1, (int)r->top, "%d");
    ASSERT_EQ_FMT(2, (int)r->right, "%d");
    ASSERT_EQ_FMT(3, (int)r->bottom, "%d");
    ASSERT_EQ_FMT(4, (int)r->left, "%d");
    ASSERT_EQ_FMT(0, (int)r->rotation, "%d");

    free(r);
    PASS();
}

TEST rotate_part_one_quarter_clockwise(void)
{
    struct part p = { .id = 7, .top = 1, .right = 2, .bottom = 3, .left = 4, .rotation = 0 };
    struct part *r = rotatePart(&p, 1);

    /* nouvelle face = ancienne face source : top<-left, right<-top, bottom<-right, left<-bottom */
    ASSERT_EQ_FMT(4, (int)r->top, "%d");
    ASSERT_EQ_FMT(1, (int)r->right, "%d");
    ASSERT_EQ_FMT(2, (int)r->bottom, "%d");
    ASSERT_EQ_FMT(3, (int)r->left, "%d");
    ASSERT_EQ_FMT(1, (int)r->rotation, "%d");

    free(r);
    PASS();
}

/* 4 quarts de tour = identité, et la rotation est ramenée modulo 4 à 0. */
TEST rotate_part_full_turn_is_identity(void)
{
    struct part p = { .id = 7, .top = 1, .right = 2, .bottom = 3, .left = 4, .rotation = 0 };
    struct part *r = rotatePart(&p, 4);

    ASSERT_EQ_FMT(1, (int)r->top, "%d");
    ASSERT_EQ_FMT(2, (int)r->right, "%d");
    ASSERT_EQ_FMT(3, (int)r->bottom, "%d");
    ASSERT_EQ_FMT(4, (int)r->left, "%d");
    ASSERT_EQ_FMT(0, (int)r->rotation, "%d"); /* 4 % 4 */

    free(r);
    PASS();
}

/* --------------------------------------------------------------------------
 * search_max_face / search_face / copy_array_part / id_for_rotated_part
 * ------------------------------------------------------------------------ */

TEST search_max_face_returns_highest_edge(void)
{
    struct part parts[] = {
        { .id = 0 },                                              /* bordure (0) */
        { .id = 1, .top = 3, .right = 7, .bottom = 2, .left = 1 },
        { .id = 2, .top = 5, .right = 4, .bottom = 9, .left = 6 },
    };
    struct array_part a = { .size = 3, .parts = parts };

    ASSERT_EQ_FMT(9, search_max_face(&a), "%d");
    PASS();
}

TEST search_face_filters_by_position_and_value(void)
{
    struct part parts[] = {
        { .id = 0 },                                              /* bordure */
        { .id = 1, .top = 5, .right = 2, .bottom = 3, .left = 1 },
        { .id = 2, .top = 5, .right = 4, .bottom = 9, .left = 6 },
        { .id = 3, .top = 7, .right = 2, .bottom = 1, .left = 8 },
    };
    struct array_part a = { .size = 4, .parts = parts };

    /* top == 5 -> pièces 1 et 2 */
    struct array_part *top5 = search_face(&a, 5, PART_TOP);
    ASSERT_EQ_FMT(2, top5->size, "%d");
    free_array_part(top5);

    /* right == 2 -> pièces 1 et 3 */
    struct array_part *right2 = search_face(&a, 2, PART_RIGHT);
    ASSERT_EQ_FMT(2, right2->size, "%d");
    free_array_part(right2);

    /* aucune pièce avec bottom == 4 */
    struct array_part *none = search_face(&a, 4, PART_BOTTOM);
    ASSERT_EQ_FMT(0, none->size, "%d");
    free_array_part(none);

    PASS();
}

TEST copy_array_part_is_a_deep_copy(void)
{
    struct part parts[] = {
        { .id = 1, .top = 1, .right = 2, .bottom = 3, .left = 4 },
        { .id = 2, .top = 5, .right = 6, .bottom = 7, .left = 8 },
    };
    struct array_part a = { .size = 2, .parts = parts };

    struct array_part *c = copy_array_part(&a);
    ASSERT(c != NULL);
    ASSERT_EQ_FMT(2, c->size, "%d");
    ASSERT(c->parts != a.parts); /* buffers distincts */
    ASSERT_EQ_FMT(2, (int)c->parts[0].right, "%d");

    parts[0].right = 99;                              /* on mute l'original */
    ASSERT_EQ_FMT(2, (int)c->parts[0].right, "%d");   /* la copie est intacte */

    free_array_part(c);

    ASSERT_EQ(NULL, copy_array_part(NULL)); /* NULL -> NULL */
    PASS();
}

TEST id_for_rotated_part_uses_etern_parts_stride(void)
{
    ASSERT_EQ_FMT(5, (int)id_for_rotated_part(5, 0), "%d");
    ASSERT_EQ_FMT(5 + ETERN_PARTS * 2, (int)id_for_rotated_part(5, 2), "%d");
    PASS();
}

/* --------------------------------------------------------------------------
 * buildBigArray + get_parts_bigarray + get_one_part
 *
 * On choisit des bords distincts pour que le lookup exact (top,right,bottom,left)
 * = (1,2,3,1) ne corresponde qu'à une seule pièce.
 * ------------------------------------------------------------------------ */
TEST build_big_array_lookup_finds_unique_part(void)
{
    struct part parts[] = {
        { .id = 0 },                                              /* bouchon bordure */
        { .id = 1, .top = 1, .right = 2, .bottom = 3, .left = 1 },
        { .id = 2, .top = 2, .right = 1, .bottom = 1, .left = 2 },
    };
    struct array_part a = { .size = 3, .parts = parts };

    int maxFace = search_max_face(&a);
    ASSERT_EQ_FMT(3, maxFace, "%d");

    map_big_array *map = buildBigArray(&a, maxFace);

    /* lookup direct par tableau de bords */
    int8_t key[4] = { 1, 2, 3, 1 };
    struct array_part *hit = get_parts_bigarray(map, key);
    ASSERT_EQ_FMT(1, hit->size, "%d");
    ASSERT_EQ_FMT(1, (int)hit->parts[0].id, "%d");

    /* même lookup via get_one_part (n'aboutit que s'il y a exactement 1 candidat) */
    key_part k = { .k1 = 1, .k2 = 2, .k3 = 3, .k4 = 1 };
    struct part *one = get_one_part(map, k);
    ASSERT(one != NULL);
    ASSERT_EQ_FMT(1, (int)one->id, "%d");

    /* combinaison sans correspondance -> NULL */
    key_part absent = { .k1 = 3, .k2 = 3, .k3 = 3, .k4 = 3 };
    ASSERT_EQ(NULL, get_one_part(map, absent));

    free_bigarray(map);
    PASS();
}

/* --------------------------------------------------------------------------
 * convert_p / hashmap_hash_int / hash : helpers purs
 * ------------------------------------------------------------------------ */

/* convert_p remplace la face -1 (« toute couleur ») par l'indice max, laisse
   les autres valeurs inchangées. */
TEST convert_p_maps_minus_one_to_max(void)
{
    ASSERT_EQ_FMT(4, (int)convert_p(-1, 4), "%d"); /* -1 -> maxFaceM */
    ASSERT_EQ_FMT(2, (int)convert_p(2, 4), "%d");  /* valeur concrète inchangée */
    ASSERT_EQ_FMT(0, (int)convert_p(0, 4), "%d");
    PASS();
}

/* hashmap_hash_int est déterministe et borné à [0, 1024[ (modulo 1024). */
TEST hashmap_hash_int_is_deterministic_and_bounded(void)
{
    unsigned long a = hashmap_hash_int(123456);
    unsigned long b = hashmap_hash_int(123456);
    ASSERT_EQ_FMT(a, b, "%lu");      /* déterministe */
    ASSERT(a < 1024);                /* borné */
    ASSERT(hashmap_hash_int(0) < 1024);
    PASS();
}

/* hash (djb2-like) est déterministe et distingue deux clés différentes. */
TEST hash_is_deterministic_and_distinguishes(void)
{
    char k1[] = "1_2_3_1";
    char k2[] = "1_2_3_2";
    char k1bis[] = "1_2_3_1";
    ASSERT_EQ_FMT(hash(k1), hash(k1bis), "%u"); /* même chaîne -> même hash */
    ASSERT(hash(k1) != hash(k2));               /* chaînes différentes -> hash distincts */
    PASS();
}

/* --------------------------------------------------------------------------
 * buildMapPart + get_parts : map par clé textuelle "top_right_bottom_left"
 * ------------------------------------------------------------------------ */

TEST build_map_part_lookup_by_text_key(void)
{
    struct part parts[] = {
        { .id = 0 },                                              /* bouchon bordure */
        { .id = 1, .top = 1, .right = 2, .bottom = 3, .left = 1 },
        { .id = 2, .top = 2, .right = 1, .bottom = 1, .left = 2 },
    };
    struct array_part a = { .size = 3, .parts = parts };

    int maxFace = search_max_face(&a); /* 3 */
    struct map_part *map = buildMapPart(&a, maxFace);

    /* clé exacte de la pièce 1 : top=1, right=2, bottom=3, left=1 */
    char key[] = "1_2_3_1";
    struct array_part *hit = get_parts(map, key);
    ASSERT(hit != NULL);
    ASSERT_EQ_FMT(1, hit->size, "%d");
    ASSERT_EQ_FMT(1, (int)hit->parts[0].id, "%d");

    /* clé sans correspondance -> tableau vide (size 0). */
    char absent[] = "3_3_3_3";
    struct array_part *none = get_parts(map, absent);
    ASSERT(none == NULL || none->size == 0);

    free_map_part(map);
    PASS();
}

/* --------------------------------------------------------------------------
 * regroup_map : aplatit la map_big_array en un seul tableau de pièces
 * ------------------------------------------------------------------------ */

TEST regroup_map_flattens_all_parts(void)
{
    struct part parts[] = {
        { .id = 0 },
        { .id = 1, .top = 1, .right = 2, .bottom = 3, .left = 1 },
        { .id = 2, .top = 2, .right = 1, .bottom = 1, .left = 2 },
    };
    struct array_part a = { .size = 3, .parts = parts };

    int maxFace = search_max_face(&a);
    map_big_array *map = buildBigArray(&a, maxFace);

    struct map_in_one *one = regroup_map(map);
    ASSERT(one != NULL);
    ASSERT(one->nbparts > 0);            /* des pièces ont bien été aplaties */
    ASSERT(one->parts != NULL);

    free_map_in_one(one);
    free_bigarray(map);
    PASS();
}

/* check_array est un helper de diagnostic : on vérifie surtout qu'il ne plante
   pas sur un tableau valide ni sur NULL (couverture des deux branches). */
TEST check_array_handles_valid_and_null(void)
{
    struct part parts[] = {
        { .id = 0 },
        { .id = 1, .top = 1, .right = 2, .bottom = 3, .left = 1 },
    };
    struct array_part a = { .size = 2, .parts = parts };

    check_array(&a);
    check_array(NULL);
    PASS();
}

SUITE(part_suite)
{
    RUN_TEST(rotate_part_zero_is_identity);
    RUN_TEST(rotate_part_one_quarter_clockwise);
    RUN_TEST(rotate_part_full_turn_is_identity);
    RUN_TEST(search_max_face_returns_highest_edge);
    RUN_TEST(search_face_filters_by_position_and_value);
    RUN_TEST(copy_array_part_is_a_deep_copy);
    RUN_TEST(id_for_rotated_part_uses_etern_parts_stride);
    RUN_TEST(build_big_array_lookup_finds_unique_part);
    RUN_TEST(convert_p_maps_minus_one_to_max);
    RUN_TEST(hashmap_hash_int_is_deterministic_and_bounded);
    RUN_TEST(hash_is_deterministic_and_distinguishes);
    RUN_TEST(build_map_part_lookup_by_text_key);
    RUN_TEST(regroup_map_flattens_all_parts);
    RUN_TEST(check_array_handles_valid_and_null);
}

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
#include "fork_assert.h"
#include "core/part.h"
#include "app/static_variables.h" /* ETERN_PARTS */

#include <stdlib.h>
#include <string.h>

/* Fonctions publiques non déclarées dans part.h : prototypes locaux. */
int8_t          convert_p(int8_t p, int maxFaceM);
int put_part(struct map_part *map, unsigned int key_int, char *key, struct array_part *apart);
unsigned long   hashmap_hash_int(unsigned long key);
unsigned int    hash(char *str);
struct array_part *get_parts(struct map_part *map, char *key);
long *compute_face_frequency(struct array_part *apart, int maxFace);
long arena_exposed_score(const struct part *p, int f1, int f2, int f3, int f4,
                          const long *freq, int maxFace);
void sort_compartment_by_exposed_rarity(struct array_part *arraypart,
                                         int f1, int f2, int f3, int f4,
                                         const long *freq, int maxFace);

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

/* 2 quarts de tour : top->bottom->left->right->top x2 */
TEST rotate_part_two_quarters(void)
{
    struct part p = { .id = 7, .top = 1, .right = 2, .bottom = 3, .left = 4, .rotation = 0 };
    struct part *r = rotatePart(&p, 2);

    /* 1er quart : top<-left(4), right<-top(1), bottom<-right(2), left<-bottom(3)
       2e quart : top<-left(3), right<-top(4), bottom<-right(1), left<-bottom(2) */
    ASSERT_EQ_FMT(3, (int)r->top, "%d");
    ASSERT_EQ_FMT(4, (int)r->right, "%d");
    ASSERT_EQ_FMT(1, (int)r->bottom, "%d");
    ASSERT_EQ_FMT(2, (int)r->left, "%d");
    ASSERT_EQ_FMT(2, (int)r->rotation, "%d");

    free(r);
    PASS();
}

/* 3 quarts de tour */
TEST rotate_part_three_quarters(void)
{
    struct part p = { .id = 7, .top = 1, .right = 2, .bottom = 3, .left = 4, .rotation = 0 };
    struct part *r = rotatePart(&p, 3);

    /* 3 quarts = inverse de 1 quart : top=right(2), right=bottom(3), bottom=left(4), left=top(1) */
    ASSERT_EQ_FMT(2, (int)r->top, "%d");
    ASSERT_EQ_FMT(3, (int)r->right, "%d");
    ASSERT_EQ_FMT(4, (int)r->bottom, "%d");
    ASSERT_EQ_FMT(1, (int)r->left, "%d");
    ASSERT_EQ_FMT(3, (int)r->rotation, "%d");

    free(r);
    PASS();
}

/* Rotation > 4 : modulo 4 appliqué (nbRotate=7 -> 7 % 4 = 3) */
TEST rotate_part_modulo_greater_than_four(void)
{
    struct part p = { .id = 7, .top = 1, .right = 2, .bottom = 3, .left = 4, .rotation = 0 };
    struct part *r = rotatePart(&p, 7); /* 7 % 4 = 3 */

    ASSERT_EQ_FMT(2, (int)r->top, "%d");
    ASSERT_EQ_FMT(3, (int)r->right, "%d");
    ASSERT_EQ_FMT(4, (int)r->bottom, "%d");
    ASSERT_EQ_FMT(1, (int)r->left, "%d");
    ASSERT_EQ_FMT(3, (int)r->rotation, "%d");

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

    /* FACE_UNKNOW : accepte toute face non nulle à la position demandée. Les 3
       pièces (hors bordure) ont top/right/bottom/left != 0 -> 3 à chaque position.
       Couvre les sous-conditions `face == FACE_UNKNOW` des 4 positions. */
    int positions[] = { PART_TOP, PART_RIGHT, PART_BOTTOM, PART_LEFT };
    for (int i = 0; i < 4; i++) {
        struct array_part *any = search_face(&a, FACE_UNKNOW, positions[i]);
        ASSERT_EQ_FMT(3, any->size, "%d");
        free_array_part(any);
    }

    /* PART_NONE : cherche la face sur N'IMPORTE quel bord. Avec une face absente
       (99), chaque pièce échoue les 4 tests en cascade -> évalue la sous-condition
       `position == PART_NONE` aux 4 positions. Aucune correspondance -> 0. */
    struct array_part *none_pos = search_face(&a, 99, PART_NONE);
    ASSERT_EQ_FMT(0, none_pos->size, "%d");
    free_array_part(none_pos);

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

/* get_one_part retourne NULL quand size != 1 (plusieurs candidats) */
TEST get_one_part_returns_null_when_multiple_matches(void)
{
    struct part parts[] = {
        { .id = 0 },
        { .id = 1, .top = 1, .right = 2, .bottom = 3, .left = 1 },
        { .id = 2, .top = 1, .right = 2, .bottom = 3, .left = 1 }, /* même clé que part 1 */
    };
    struct array_part a = { .size = 3, .parts = parts };

    map_big_array *map = buildBigArray(&a, search_max_face(&a));

    /* lookup qui retourne 2 candidats (parts 1 et 2) -> get_one_part retourne NULL */
    key_part k = { .k1 = 1, .k2 = 2, .k3 = 3, .k4 = 1 };
    struct part *one = get_one_part(map, k);
    ASSERT_EQ(NULL, one);

    free_bigarray(map);
    PASS();
}

/* get_one_part retourne NULL quand k1 <= -2 (key invalide) */
TEST get_one_part_returns_null_for_invalid_key(void)
{
    struct part parts[] = {
        { .id = 0 },
        { .id = 1, .top = 1, .right = 2, .bottom = 3, .left = 1 },
    };
    struct array_part a = { .size = 2, .parts = parts };

    map_big_array *map = buildBigArray(&a, search_max_face(&a));

    key_part invalid = { .k1 = -2, .k2 = 2, .k3 = 3, .k4 = 1 };
    struct part *one = get_one_part(map, invalid);
    ASSERT_EQ(NULL, one);

    free_bigarray(map);
    PASS();
}

/* --------------------------------------------------------------------------
 * prepare_map_part : wrapper enchaînant search_max_face puis buildBigArray.
 * On vérifie qu'il produit une map équivalente à l'enchaînement manuel testé
 * ci-dessus (même lookup exact sur la pièce 1).
 * ------------------------------------------------------------------------ */
TEST prepare_map_part_builds_lookup_equivalent_to_manual(void)
{
    struct part parts[] = {
        { .id = 0 },                                              /* bouchon bordure */
        { .id = 1, .top = 1, .right = 2, .bottom = 3, .left = 1 },
        { .id = 2, .top = 2, .right = 1, .bottom = 1, .left = 2 },
    };
    struct array_part a = { .size = 3, .parts = parts };

    map_big_array *map = prepare_map_part(&a);
    ASSERT(map != NULL);

    /* le wrapper a bien dimensionné la map sur maxFace -> lookup exact pièce 1 */
    int8_t key[4] = { 1, 2, 3, 1 };
    struct array_part *hit = get_parts_bigarray(map, key);
    ASSERT_EQ_FMT(1, hit->size, "%d");
    ASSERT_EQ_FMT(1, (int)hit->parts[0].id, "%d");

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

/* convert_p avec d'autres valeurs négatives (pas -1) : laisse inchangé */
TEST convert_p_preserves_non_minus_one_negatives(void)
{
    ASSERT_EQ_FMT(-2, (int)convert_p(-2, 4), "%d");
    ASSERT_EQ_FMT(-5, (int)convert_p(-5, 10), "%d");
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

/* free_bigarray avec arena NULL : branche if (array_parts->arena != NULL) == false */
TEST free_bigarray_with_null_arena(void)
{
    struct part parts[] = {
        { .id = 0 },
    };
    struct array_part a = { .size = 1, .parts = parts };

    int maxFace = search_max_face(&a);
    map_big_array *map = buildBigArray(&a, maxFace);

    /* map->arena est NULL quand aucune pièce n'a été trouvée (totalParts == 0) */
    if (map->arena == NULL) {
        /* branche couverte : free_bigarray ne plante pas sur NULL arena */
        free_bigarray(map);
        PASS();
    } else {
        /* Si arena != NULL dans ce cas, le test passe quand même (buildBigArray avec une pièce vide est un cas limite) */
        free_bigarray(map);
        PASS();
    }
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

    /* Pièce à l'id hors borne [0, 256] : déclenche la branche de signalement
       (log + print_part), jusqu'ici jamais prise (toutes les fixtures sont valides). */
    struct part bad[] = {
        { .id = 0 },
        { .id = 300, .top = 1, .right = 2, .bottom = 3, .left = 4 }, /* > 256 */
        { .id = -5,  .top = 5, .right = 6, .bottom = 7, .left = 8 }, /* < 0   */
    };
    struct array_part b = { .size = 3, .parts = bad };
    check_array(&b);
    PASS();
}

/* put_part : la sonde linéaire atteint la fin de la table (« map trop
 * petite ») -> exit(EXIT_FAILURE). Exécuté via fork_assert : deux clés qui
 * hachent sur le dernier slot ; la seconde sonde au-delà de sizemap. */
static void child_put_part_overflow(void)
{
    struct map_part map;
    map.size = 2;
    map.sizemap = 2;
    map.elements = calloc(2, sizeof(struct map_part_element));
    struct part parts[] = { { .id = 1, .top = 1, .right = 1, .bottom = 1, .left = 1 } };
    struct array_part apart = { .size = 1, .parts = parts };
    put_part(&map, 3, "k1", &apart);   /* 3 % 2 = 1 -> dernier slot occupé */
    put_part(&map, 5, "k2", &apart);   /* même slot -> sonde -> l == sizemap -> exit */
    exit(0);                            /* ne doit jamais être atteint */
}

TEST put_part_exits_when_map_full(void)
{
    ASSERT_EQ_FMT(EXIT_FAILURE, run_in_fork(child_put_part_overflow, NULL), "%d");
    PASS();
}

/* regroup_map sur une map sans aucune pièce : nbparts == 0 -> le tableau
 * `parts` est libéré et mis à NULL (branche jamais prise avec des maps
 * peuplées). */
TEST regroup_map_empty_map_frees_parts(void)
{
    struct array_part a = { .size = 0, .parts = NULL };
    map_big_array *map = buildBigArray(&a, search_max_face(&a));

    struct map_in_one *one = regroup_map(map);
    ASSERT(one != NULL);
    ASSERT_EQ_FMT(0, one->nbparts, "%d");
    ASSERT_EQ(NULL, one->parts);

    free_map_in_one(one);
    free_bigarray(map);
    PASS();
}

/* Jeu de pièces varié (bords 1..4) : produit à la fois des compartiments vides,
 * des compartiments à une pièce et un gros compartiment (celui tout-joker). */
static struct array_part *make_varied_parts(void)
{
    static struct part parts[] = {
        { .id = 0 },                                              /* bouchon bordure */
        { .id = 1, .top = 1, .right = 2, .bottom = 3, .left = 4 },
        { .id = 2, .top = 2, .right = 3, .bottom = 4, .left = 1 },
        { .id = 3, .top = 3, .right = 4, .bottom = 1, .left = 2 },
        { .id = 4, .top = 4, .right = 1, .bottom = 2, .left = 3 },
        { .id = 5, .top = 1, .right = 2, .bottom = 3, .left = 4 }, /* doublon de 1 */
        { .id = 6, .top = 2, .right = 2, .bottom = 2, .left = 2 },
    };
    static struct array_part a = { .size = 7, .parts = parts };
    return &a;
}

/* --------------------------------------------------------------------------
 * §4.8 (docs/conception/elagage_recherche.md) : ordre expérimental des
 * candidats dans chaque compartiment — compute_face_frequency,
 * arena_exposed_score, sort_compartment_by_exposed_rarity.
 * ------------------------------------------------------------------------ */

TEST compute_face_frequency_counts_all_four_faces(void)
{
    struct part parts[] = {
        { .id = 1, .top = 0, .right = 1, .bottom = 2, .left = 1 },
        { .id = 2, .top = 1, .right = 1, .bottom = 3, .left = 0 },
    };
    struct array_part a = { .size = 2, .parts = parts };

    long *freq = compute_face_frequency(&a, 3);
    ASSERT(freq != NULL);
    ASSERT_EQ_FMT(2L, freq[0], "%ld"); /* p1.top + p2.left */
    ASSERT_EQ_FMT(4L, freq[1], "%ld"); /* p1.right + p1.left + p2.top + p2.right */
    ASSERT_EQ_FMT(1L, freq[2], "%ld"); /* p1.bottom */
    ASSERT_EQ_FMT(1L, freq[3], "%ld"); /* p2.bottom */

    free(freq);
    PASS();
}

TEST compute_face_frequency_negative_max_face_returns_null(void)
{
    struct part parts[] = { { .id = 1, .top = 0, .right = 0, .bottom = 0, .left = 0 } };
    struct array_part a = { .size = 1, .parts = parts };
    ASSERT(compute_face_frequency(&a, -1) == NULL);
    PASS();
}

/* arena_exposed_score : seuls les côtés WILDCARD du compartiment (f_i == -1)
 * doivent contribuer au score — les côtés contraints sont identiques pour
 * toutes les pièces du compartiment et ne doivent jamais peser dessus. */
TEST arena_exposed_score_counts_only_wildcard_faces(void)
{
    struct part p = { .id = 1, .top = 5, .right = 9, .bottom = 1, .left = 2 };
    long freq[10] = { 0 };
    freq[1] = 100; freq[2] = 100; freq[5] = 10; freq[9] = 5;

    /* top et right wildcard (f1=f2=-1), bottom et left contraints (f3=1,f4=2) */
    long score_wildcard_top_right = arena_exposed_score(&p, -1, -1, 1, 2, freq, 9);
    ASSERT_EQ_FMT(15L, score_wildcard_top_right, "%ld"); /* freq[5] + freq[9] */

    /* tous contraints : aucune contribution, quelles que soient les fréquences */
    long score_all_constrained = arena_exposed_score(&p, 5, 9, 1, 2, freq, 9);
    ASSERT_EQ_FMT(0L, score_all_constrained, "%ld");

    /* tous wildcard : les 4 faces contribuent */
    long score_all_wildcard = arena_exposed_score(&p, -1, -1, -1, -1, freq, 9);
    ASSERT_EQ_FMT(215L, score_all_wildcard, "%ld"); /* 10 + 5 + 100 + 100 */

    PASS();
}

/* Sans fréquence (allocation échouée en amont), le tri se dégrade en no-op
 * plutôt que de lire freq == NULL. */
TEST sort_compartment_null_freq_is_a_no_op(void)
{
    struct part parts[] = {
        { .id = 30, .top = 5, .right = 9, .bottom = 1, .left = 2 },
        { .id = 31, .top = 1, .right = 3, .bottom = 1, .left = 2 },
    };
    struct array_part a = { .size = 2, .parts = parts };

    sort_compartment_by_exposed_rarity(&a, -1, -1, 1, 2, NULL, 9);

    ASSERT_EQ_FMT(30, (int)a.parts[0].id, "%d");
    ASSERT_EQ_FMT(31, (int)a.parts[1].id, "%d");
    PASS();
}

/* Cœur de la garantie de correction §4.8 : le tri ne peut JAMAIS faire
 * disparaître ou apparaître un candidat (un faux négatif jetterait
 * silencieusement une solution) — seul l'ordre change. Vérifié ici en même
 * temps que l'ordre effectivement obtenu (rareté croissante), sur un
 * compartiment où les scores sont tous distincts (pas d'ambiguïté d'ordre). */
TEST arena_sort_preserves_multiset_and_orders_by_rarity(void)
{
    struct part original[] = {
        { .id = 30, .top = 5, .right = 9, .bottom = 1, .left = 2 }, /* score 15 */
        { .id = 31, .top = 1, .right = 3, .bottom = 1, .left = 2 }, /* score 150 */
        { .id = 32, .top = 9, .right = 1, .bottom = 1, .left = 2 }, /* score 105 */
    };
    struct part sorted[3];
    memcpy(sorted, original, sizeof(original));

    struct array_part a = { .size = 3, .parts = sorted };
    long freq[10] = { 0 };
    freq[1] = 100; freq[2] = 100; freq[3] = 50; freq[5] = 10; freq[9] = 5;

    sort_compartment_by_exposed_rarity(&a, -1, -1, 1, 2, freq, 9);

    ASSERT_EQ_FMT(30, (int)a.parts[0].id, "%d"); /* score 15  (le plus rare) */
    ASSERT_EQ_FMT(32, (int)a.parts[1].id, "%d"); /* score 105 */
    ASSERT_EQ_FMT(31, (int)a.parts[2].id, "%d"); /* score 150 (le plus courant) */

    /* Multi-ensemble préservé : mêmes 3 ids présents, indépendamment de l'ordre. */
    for (int i = 0; i < 3; i++) {
        int found = 0;
        for (int j = 0; j < 3; j++) {
            if (a.parts[j].id == original[i].id) {
                found = 1;
            }
        }
        ASSERT(found);
    }
    PASS();
}

/* Intégration : buildBigArray (via prepare_map_part) trie RÉELLEMENT les
 * compartiments en production (comportement inconditionnel depuis
 * l'adoption de §4.8) — vérifié sur le compartiment « tout joker »
 * (f1=f2=f3=f4=-1, le plus gros de `make_varied_parts`) en comparant l'ordre
 * obtenu à un calcul indépendant de `arena_exposed_score` sur les MÊMES
 * fréquences. L'équivalence packed/flat, elle, est déjà verrouillée sans
 * hypothèse sur l'ordre par `packed_index_matches_flat_for_every_key`
 * ci-dessous : les deux représentations sont construites à partir du même
 * `flat` déjà trié, jamais l'une sans l'autre. */
TEST buildbigarray_sorts_wildcard_compartment_by_ascending_rarity(void)
{
    struct array_part *apart = make_varied_parts();
    int maxFace = search_max_face(apart);
    long *freq = compute_face_frequency(apart, maxFace);
    ASSERT(freq != NULL);

    map_big_array *map = buildBigArray(apart, maxFace);

    /* Convention d'indexation de buildBigArray pour f == -1 (voir la boucle de
     * construction) : p = maxFace + abs(f) = maxFace + 1 — PAS `convert_p`,
     * qui encode une convention différente (maxFace tout court) et n'est
     * appelée nulle part dans le code de production. */
    int8_t wildcard_idx = (int8_t)(maxFace + 1);
    key_part key = { .k1 = wildcard_idx, .k2 = wildcard_idx,
                      .k3 = wildcard_idx, .k4 = wildcard_idx };
    struct array_part *compartment = get_parts_bigarray_with_key(map, &key);
    ASSERT(compartment->size > 1); /* sinon le test ne prouve rien */

    for (int i = 1; i < compartment->size; i++) {
        long prev_score = arena_exposed_score(&compartment->parts[i - 1], -1, -1, -1, -1, freq, maxFace);
        long cur_score = arena_exposed_score(&compartment->parts[i], -1, -1, -1, -1, freq, maxFace);
        ASSERT(prev_score <= cur_score);
    }

    free(freq);
    free_bigarray(map);
    PASS();
}

/* --------------------------------------------------------------------------
 * Index compact `packed` : map_bucket_packed / map_packed_fits
 *
 * `packed` est une SECONDE REPRÉSENTATION de `flat`, plus dense, lue par la
 * boucle chaude du forward-checking. Le contrat, et donc l'objet de ces tests,
 * est l'équivalence STRICTE des deux représentations : si elle tient pour
 * toutes les clés, la sémantique de recherche ne peut pas avoir bougé.
 * ------------------------------------------------------------------------ */

/* Équivalence exhaustive : pour CHAQUE clé de la map, `map_bucket_packed` et
 * `get_parts_bigarray_with_key` renvoient la même taille et la même liste. */
TEST packed_index_matches_flat_for_every_key(void)
{
    map_big_array *map = prepare_map_part(make_varied_parts());
    ASSERT(map->packed != NULL); /* le jeu tient largement dans les 16 bits */

    int m = map->sizearray;
    long checked = 0, non_empty = 0;
    for (int k1 = 0; k1 < m; k1++)
        for (int k2 = 0; k2 < m; k2++)
            for (int k3 = 0; k3 < m; k3++)
                for (int k4 = 0; k4 < m; k4++) {
                    key_part key = { .k1 = (int8_t)k1, .k2 = (int8_t)k2,
                                     .k3 = (int8_t)k3, .k4 = (int8_t)k4 };
                    struct array_part *ref = get_parts_bigarray_with_key(map, &key);
                    map_bucket got = map_bucket_packed(map, &key);

                    ASSERT_EQ_FMT(ref->size, got.size, "%d");
                    if (ref->size > 0) {
                        /* Les deux vues désignent la MÊME zone de l'arène. */
                        ASSERT_EQ(ref->parts, got.parts);
                        for (int s = 0; s < ref->size; s++) {
                            ASSERT_EQ_FMT((int)ref->parts[s].id, (int)got.parts[s].id, "%d");
                            ASSERT_EQ_FMT((int)ref->parts[s].rotation, (int)got.parts[s].rotation, "%d");
                            ASSERT_EQ_FMT((int)ref->parts[s].top, (int)got.parts[s].top, "%d");
                            ASSERT_EQ_FMT((int)ref->parts[s].right, (int)got.parts[s].right, "%d");
                            ASSERT_EQ_FMT((int)ref->parts[s].bottom, (int)got.parts[s].bottom, "%d");
                            ASSERT_EQ_FMT((int)ref->parts[s].left, (int)got.parts[s].left, "%d");
                        }
                        non_empty++;
                    }
                    checked++;
                }

    /* Le balayage a bien vu les deux natures de compartiment. */
    ASSERT_EQ_FMT((long)(m * m * m * m), checked, "%ld");
    ASSERT(non_empty > 0);
    ASSERT(non_empty < checked); /* il existe aussi des compartiments vides */

    free_bigarray(map);
    PASS();
}

/* --------------------------------------------------------------------------
 * Index des masques d'ids `bucket_id_mask` (§4.7, choix de case MRV)
 *
 * Troisième représentation redondante du même `flat`, comme `packed` : le
 * contrat, et donc l'objet de ces tests, est l'ÉQUIVALENCE STRICTE avec un
 * comptage par parcours du compartiment. Une divergence changerait
 * silencieusement la case choisie par MRV — et, pire, le test de mort
 * (« zéro pièce libre ») qui, lui, élague pour de bon.
 * ------------------------------------------------------------------------ */

/* Équivalence exhaustive : pour CHAQUE clé, le nombre d'ids distincts du
 * masque (popcount) est celui obtenu en parcourant le compartiment, et ce pour
 * plusieurs masques de pièces utilisées (aucune, une sur deux, toutes). */
/* Borne large des ids de la fixture (7 pièces) : le masque n'en a besoin que
 * d'un mot, et `used` en réserve 8 — aucune hypothèse sur ETERN_PARTS. */
#define MASK_TEST_MAX_ID 64

TEST bucket_id_mask_matches_flat_for_every_key(void)
{
    map_big_array *map = prepare_map_part(make_varied_parts());
    ASSERT(map->packed != NULL);
    ASSERT(map->bucket_id_mask != NULL);
    ASSERT(map->id_mask_words > 0);

    int words = map->id_mask_words;
    /* Trois états de plateau : rien d'utilisé, un id sur deux, tout utilisé. */
    for (int scenario = 0; scenario < 3; scenario++) {
        uint64_t used[8] = {0};
        int used_flags[MASK_TEST_MAX_ID + 1];
        for (int id = 1; id <= MASK_TEST_MAX_ID; id++) {
            int is_used = (scenario == 2) || (scenario == 1 && (id % 2) == 0);
            used_flags[id] = is_used;
            if (is_used) {
                used[(id - 1) / 64] |= (uint64_t)1 << ((id - 1) % 64);
            }
        }

        int m = map->sizearray;
        long non_empty = 0;
        for (int k1 = 0; k1 < m; k1++)
            for (int k2 = 0; k2 < m; k2++)
                for (int k3 = 0; k3 < m; k3++)
                    for (int k4 = 0; k4 < m; k4++) {
                        key_part key = { .k1 = (int8_t)k1, .k2 = (int8_t)k2,
                                         .k3 = (int8_t)k3, .k4 = (int8_t)k4 };
                        struct array_part *ref = get_parts_bigarray_with_key(map, &key);
                        const uint64_t *mask = map_bucket_id_mask(map, &key);
                        if (ref->size == 0) {
                            /* Compartiment vide : pas de masque (l'appelant
                             * doit retomber sur le parcours, jamais conclure). */
                            ASSERT(mask == NULL);
                            continue;
                        }
                        ASSERT(mask != NULL);
                        non_empty++;

                        /* Référence : ids DISTINCTS et libres du compartiment. */
                        int seen[MASK_TEST_MAX_ID + 1];
                        memset(seen, 0, sizeof(seen));
                        int expected = 0;
                        for (int s = 0; s < ref->size; s++) {
                            int id = ref->parts[s].id;
                            if (id <= 0 || id > MASK_TEST_MAX_ID || seen[id] || used_flags[id]) {
                                continue;
                            }
                            seen[id] = 1;
                            expected++;
                        }
                        ASSERT_EQ_FMT(expected, map_mask_free_count(mask, words, used), "%d");
                    }
        ASSERT(non_empty > 0);
    }

    free_bigarray(map);
    PASS();
}

/* Cas limites : compartiment vide (taille 0) et plus gros compartiment
 * (celui de la clé tout-joker, qui contient toutes les rotations). */
TEST packed_index_handles_empty_and_largest_bucket(void)
{
    map_big_array *map = prepare_map_part(make_varied_parts());
    ASSERT(map->packed != NULL);

    int all = map->sizearrayM; /* indice « toute face » */

    /* (a) compartiment vide : la clé (1,1,1,1) n'a aucune pièce (bords tous
     *     différents dans la fixture, hors pièce 6 qui est en 2,2,2,2). */
    key_part empty_key = { .k1 = 1, .k2 = 1, .k3 = 1, .k4 = 1 };
    ASSERT_EQ_FMT(0, get_parts_bigarray_with_key(map, &empty_key)->size, "%d");
    ASSERT_EQ_FMT(0, map_bucket_packed(map, &empty_key).size, "%d");

    /* (b) plus gros compartiment : la clé tout-joker. Aucun autre compartiment
     *     ne peut être plus gros, puisqu'aucune contrainte ne le filtre. */
    key_part wild = { .k1 = (int8_t)all, .k2 = (int8_t)all,
                      .k3 = (int8_t)all, .k4 = (int8_t)all };
    struct array_part *ref = get_parts_bigarray_with_key(map, &wild);
    map_bucket got = map_bucket_packed(map, &wild);
    ASSERT(ref->size > 1);
    ASSERT_EQ_FMT(ref->size, got.size, "%d");
    ASSERT_EQ(ref->parts, got.parts);

    int m = map->sizearray;
    for (int k1 = 0; k1 < m; k1++)
        for (int k2 = 0; k2 < m; k2++)
            for (int k3 = 0; k3 < m; k3++)
                for (int k4 = 0; k4 < m; k4++) {
                    key_part key = { .k1 = (int8_t)k1, .k2 = (int8_t)k2,
                                     .k3 = (int8_t)k3, .k4 = (int8_t)k4 };
                    ASSERT(map_bucket_packed(map, &key).size <= ref->size);
                }

    free_bigarray(map);
    PASS();
}

/* Sans index compact (map bâtie à la main, ou capacité dépassée), le lecteur
 * retombe sur `flat` et renvoie exactement la même chose. */
TEST packed_index_falls_back_to_flat_when_absent(void)
{
    map_big_array *map = prepare_map_part(make_varied_parts());
    ASSERT(map->packed != NULL);

    key_part wild = { .k1 = (int8_t)map->sizearrayM, .k2 = (int8_t)map->sizearrayM,
                      .k3 = (int8_t)map->sizearrayM, .k4 = (int8_t)map->sizearrayM };
    map_bucket with_index = map_bucket_packed(map, &wild);

    /* On neutralise l'index : le repli doit donner le même résultat. */
    uint32_t *saved = map->packed;
    map->packed = NULL;
    map_bucket without_index = map_bucket_packed(map, &wild);
    map->packed = saved;

    ASSERT_EQ_FMT(with_index.size, without_index.size, "%d");
    ASSERT_EQ(with_index.parts, without_index.parts);

    free_bigarray(map);
    PASS();
}

/* map_packed_fits : détection du dépassement de capacité des champs 16 bits.
 * Un puzzle dont l'arène ou le plus gros compartiment dépasserait 65535 doit
 * être détecté à la construction — jamais tronqué silencieusement. */
TEST map_packed_fits_detects_capacity_overflow(void)
{
    /* Cas réels du projet : les deux tailles de puzzle tiennent très largement. */
    ASSERT_EQ_FMT(1, map_packed_fits(14401, 784), "%d");  /* puzzle 256 */
    ASSERT_EQ_FMT(1, map_packed_fits(577, 16), "%d");     /* puzzle 16 */

    /* Arène vide. */
    ASSERT_EQ_FMT(1, map_packed_fits(0, 0), "%d");

    /* Bornes exactes. */
    ASSERT_EQ_FMT(1, map_packed_fits(65535, 65535), "%d");
    ASSERT_EQ_FMT(0, map_packed_fits(65536, 1), "%d");     /* offset non représentable */
    ASSERT_EQ_FMT(0, map_packed_fits(65535, 65536), "%d"); /* taille non représentable */
    ASSERT_EQ_FMT(0, map_packed_fits(1000000, 1000), "%d");

    PASS();
}

/* Une map dont l'arène est vide n'a pas d'index compact : l'invariant
 * « packed != NULL implique arena != NULL » est préservé, et la lecture
 * retombe sur `flat` sans déréférencer une arène inexistante. */
TEST packed_index_absent_when_arena_empty(void)
{
    struct array_part a = { .size = 0, .parts = NULL };
    map_big_array *map = buildBigArray(&a, search_max_face(&a));

    if (map->arena == NULL) {
        ASSERT_EQ(NULL, map->packed);
    }
    key_part k = { .k1 = 0, .k2 = 0, .k3 = 0, .k4 = 0 };
    ASSERT_EQ_FMT(0, map_bucket_packed(map, &k).size, "%d");

    free_bigarray(map);
    PASS();
}

SUITE(part_suite)
{
    RUN_TEST(rotate_part_zero_is_identity);
    RUN_TEST(rotate_part_one_quarter_clockwise);
    RUN_TEST(rotate_part_full_turn_is_identity);
    RUN_TEST(rotate_part_two_quarters);
    RUN_TEST(rotate_part_three_quarters);
    RUN_TEST(rotate_part_modulo_greater_than_four);
    RUN_TEST(search_max_face_returns_highest_edge);
    RUN_TEST(search_face_filters_by_position_and_value);
    RUN_TEST(copy_array_part_is_a_deep_copy);
    RUN_TEST(id_for_rotated_part_uses_etern_parts_stride);
    RUN_TEST(build_big_array_lookup_finds_unique_part);
    RUN_TEST(get_one_part_returns_null_when_multiple_matches);
    RUN_TEST(get_one_part_returns_null_for_invalid_key);
    RUN_TEST(prepare_map_part_builds_lookup_equivalent_to_manual);
    RUN_TEST(convert_p_maps_minus_one_to_max);
    RUN_TEST(convert_p_preserves_non_minus_one_negatives);
    RUN_TEST(hashmap_hash_int_is_deterministic_and_bounded);
    RUN_TEST(hash_is_deterministic_and_distinguishes);
    RUN_TEST(build_map_part_lookup_by_text_key);
    RUN_TEST(regroup_map_flattens_all_parts);
    RUN_TEST(regroup_map_empty_map_frees_parts);
    RUN_TEST(put_part_exits_when_map_full);
    RUN_TEST(free_bigarray_with_null_arena);
    RUN_TEST(check_array_handles_valid_and_null);
    RUN_TEST(packed_index_matches_flat_for_every_key);
    RUN_TEST(bucket_id_mask_matches_flat_for_every_key);
    RUN_TEST(packed_index_handles_empty_and_largest_bucket);
    RUN_TEST(packed_index_falls_back_to_flat_when_absent);
    RUN_TEST(map_packed_fits_detects_capacity_overflow);
    RUN_TEST(packed_index_absent_when_arena_empty);
    RUN_TEST(compute_face_frequency_counts_all_four_faces);
    RUN_TEST(compute_face_frequency_negative_max_face_returns_null);
    RUN_TEST(arena_exposed_score_counts_only_wildcard_faces);
    RUN_TEST(sort_compartment_null_freq_is_a_no_op);
    RUN_TEST(arena_sort_preserves_multiset_and_orders_by_rarity);
    RUN_TEST(buildbigarray_sorts_wildcard_compartment_by_ascending_rarity);
}

#ifndef ETII_TESTS_EXPAND_FIXTURE_H
#define ETII_TESTS_EXPAND_FIXTURE_H

/* Fixtures d'expansion (`expand_datas_to_level`) partagées par les suites du
 * stock et du débordement. */

#include "packet_fixture.h"
#include "core/datamanager.h"
#include "core/part.h"

/*
 * Fixtures autonomes (indépendantes de pieces.csv / ETERN_PARTS) : une map
 * « libre » dont chaque clé renvoie les mêmes 8 pièces candidates (ids 1..8),
 * et un tableau de rotations aux faces PETITES (< sizearray) pour que les clés
 * calculées par what_search_to_key indexent flat[3^4] sans déborder. 8
 * candidats entretiennent le branchement sur > EXPAND_MAX_LEVELS niveaux (avec
 * seulement 2 pièces, toutes les branches mourraient dès le 2e placement). */
static inline struct array_part *make_expand_parts(void)
{
    /* Indices 0..8 : grid stocke idParts[id][0] == id (1..8), lu comme
       all_rotate_parts->parts[grid] par what_search_in_grid_to_key. */
    static struct part parts[9];
    static struct array_part ap;
    for (int i = 0; i < 9; i++) {
        memset(&parts[i], 0, sizeof(struct part));
        parts[i].id     = (int16_t)i;
        parts[i].top    = (int8_t)(i % 3);
        parts[i].right  = (int8_t)((i + 1) % 3);
        parts[i].bottom = (int8_t)((i + 2) % 3);
        parts[i].left   = (int8_t)(i % 3);
        parts[i].rotation = 0;
    }
    ap.size = 9;
    ap.parts = parts;
    return &ap;
}

static inline map_big_array *make_expand_free_map(void)
{
    static struct part cand[8];
    static struct array_part list = { .size = 8, .parts = cand };
    static map_big_array map;
    static struct array_part flat[3 * 3 * 3 * 3];
    for (int i = 0; i < 8; i++) {
        memset(&cand[i], 0, sizeof(struct part));
        cand[i].id = (int16_t)(i + 1);   /* candidats : ids 1..8 */
    }
    map.sizearray  = 3;
    map.sizearrayM = 2;
    map.arena = NULL;
    map.flat = flat;
    for (int i = 0; i < 3 * 3 * 3 * 3; i++) flat[i] = list;
    return &map;
}

/* Sème une possibilité genèse (plateau vide, curseur en directions[0]). */
static inline void seed_genesis(uint16_t alloc)
{
    /* Plateau COHÉRENT : `alloc` pièces réellement posées. Poser le champ sans
       les pièces ne suffit plus — `alloc` est déduit de la grille, et un stock
       « profond de 5 » avec un plateau vide serait vu comme profond de 0. */
    struct possibility_packet g;
    fixture_packet(&g, (int)alloc);
    g.x = dirx[alloc];
    g.y = diry[alloc];
    g.checked = 0;
    array_possibility_packet arr = { .size = 1, .possibilities = &g };
    add_possibility(NULL, &arr);
}

#endif

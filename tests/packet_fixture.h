/*
 * Fabriques de `possibility_packet` pour les tests.
 *
 * ## D'où viennent les fixtures qui posent un `alloc` arbitraire
 *
 * Elles sont d'AVANT VERSION 13, et elles avaient raison à l'époque : `alloc`
 * était alors la POSITION dans le parcours (`directions[]`), pas un nombre de
 * pièces posées — c'est `b_faceused` qui portait la vérité sur ce qui était
 * placé. Poser `alloc = 42` sur une grille quelconque était donc parfaitement
 * légitime : les deux ne parlaient pas de la même chose. S'y ajoutait un décalage
 * réel — les indices connus étaient comptés dans les pièces utilisées sans être
 * forcément positionnés, donc sans compter dans la position ; ils sont désormais
 * posés d'emblée.
 *
 * VERSION 13 a fait de `alloc` le nombre de cases non vides. `import()` recompte
 * d'ailleurs ce champ sans condition sur toute possibilité restaurée, justement
 * pour rattraper les fichiers d'avant la bascule. Ces fixtures sont le dernier
 * endroit où l'ancienne sémantique a survécu.
 *
 * ## Pourquoi `memset(&pk, 0, sizeof pk)` ne convient pas
 *
 * La case VIDE d'un plateau vaut `-2`, pas `0`. Un paquet mis à zéro n'a donc
 * aucune case vide : il porte `ETERN_PARTS` pièces, toutes d'identifiant 0.
 * Tant que le stock gardait des `possibility_packet` entiers, personne ne
 * regardait — `alloc` restait à la valeur qu'on lui posait, et la grille ne
 * servait à rien dans ces tests.
 *
 * Ce n'est plus vrai. `alloc` est DÉDUIT de la grille (c'est sa définition,
 * cf. `possibility_placed_count`, et ce que `import()` impose déjà sans
 * condition à toute possibilité restaurée). Un paquet mis à zéro annonce donc
 * un plateau COMPLET — autrement dit une SOLUTION. Les chemins qui en
 * rencontrent une la sauvegardent et, sous `--stop-on-solution`, appellent
 * `exit()` : une telle fixture fait sortir le runner de tests en plein milieu,
 * avec un code 0 et sans ligne de résumé. Un faux succès, le pire des échecs.
 *
 * ## Les trois fabriques
 *
 * - `fixture_blank` : plateau VIDE (0 pièce posée) ;
 * - `fixture_packet` : `placed` pièces distinctes, masque des pièces utilisées
 *   en accord, jamais le plateau plein ;
 * - `fixture_packet_distinct` : un plateau DIFFÉRENT par `idx`, la distinction
 *   venant du jeu de cases occupées plutôt que d'un identifiant de pièce qui
 *   grandirait — une valeur de case doit rester dans `[0, 4 x ETERN_PARTS]`,
 *   soit 64 seulement en build 4x4, alors que le nombre de plateaux distincts
 *   disponibles est de 2^ETERN_PARTS.
 */
#ifndef ETII_TEST_PACKET_FIXTURE_H
#define ETII_TEST_PACKET_FIXTURE_H

#include <string.h>

#include "core/possibility.h"

/// Plateau entièrement vide : toutes les cases à -2, aucune pièce posée.
static inline void fixture_blank(struct possibility_packet *pk)
{
    memset(pk, 0, sizeof *pk);
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            pk->grid[x][y] = -2;
        }
    }
    pk->min_candidats = POSSIBILITY_MIN_CANDIDATS_UNKNOWN;
}

/**
 * @brief Plateau COHÉRENT à `placed` pièces distinctes.
 *
 * `placed` est ramené à `ETERN_PARTS - 1` au maximum : jamais le plateau
 * plein, qui serait une solution (cf. l'en-tête de ce fichier). Les tests qui
 * comparent `alloc` à une valeur littérale doivent donc la borner de la même
 * façon — `FIXTURE_DEPTH`.
 */
static inline void fixture_packet(struct possibility_packet *pk, int placed)
{
    fixture_blank(pk);
    if (placed < 0) {
        placed = 0;
    }
    if (placed >= ETERN_PARTS) {
        placed = ETERN_PARTS - 1;
    }
    for (int i = 0; i < placed; i++) {
        pk->grid[i / ETERN_SIZE][i % ETERN_SIZE] = (int16_t)(i + 1);
        set_face_used(pk->b_faceused, (uint16_t)i, 1);
    }
    pk->alloc = (uint16_t)placed;
}

/**
 * @brief Plateau COMPLET — donc une SOLUTION.
 *
 * `fixture_packet` refuse d'en produire (cf. son commentaire) : c'est un garde-fou
 * contre les fixtures qui en fabriquaient un par accident. Les rares tests qui
 * veulent VRAIMENT une solution — parce que c'est leur sujet — passent par ici,
 * explicitement, et savent ce qu'ils font.
 */
static inline void fixture_full_board(struct possibility_packet *pk)
{
    fixture_blank(pk);
    for (int i = 0; i < ETERN_PARTS; i++) {
        /* La MÊME pièce partout, pas des pièces distinctes : ce qui fait de ce
         * plateau une solution aux yeux du code, c'est l'absence de case vide,
         * pas la légalité de l'assemblage. Et des identifiants montant jusqu'à
         * ETERN_PARTS déborderaient les maps minimales que ces tests montent à
         * la main — `save_solution_csv` indexe `all_rotate_part->parts[valeur]`. */
        pk->grid[i / ETERN_SIZE][i % ETERN_SIZE] = 1;
    }
    set_face_used(pk->b_faceused, 0, 1);
    pk->alloc = (uint16_t)ETERN_PARTS;
}

/// Profondeur effective d'une fixture — à utiliser dans les assertions qui
/// comparent `alloc`, pour qu'elles restent vraies dans les DEUX builds.
#define FIXTURE_DEPTH(n) (((n) < ETERN_PARTS) ? (n) : (ETERN_PARTS - 1))

/**
 * @brief Plateau PRESQUE plein : toutes les cases portent `value`, sauf
 *        `(hole_x, hole_y)` laissée vide.
 *
 * Reproduit l'ancienne fixture « `memset(0)` avec un trou » — un plateau dont
 * chaque case voisine du trou impose la couleur 0 — sans que le plateau soit
 * COMPLET. C'est cette nuance qui compte désormais : un plateau plein est une
 * solution, et les chemins qui en rencontrent une appellent `exit()`.
 *
 * `b_faceused` reste nul quand `value` vaut 0 : 0 n'est l'identifiant d'aucune
 * pièce, donc aucune n'est marquée utilisée — exactement ce que produisait le
 * `memset` d'origine.
 */
static inline void fixture_board_with_hole(struct possibility_packet *pk, int16_t value,
                                           int hole_x, int hole_y)
{
    fixture_blank(pk);
    int placed = 0;
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            if (x == hole_x && y == hole_y) {
                continue;
            }
            pk->grid[x][y] = value;
            if (value >= 1) {
                set_face_used(pk->b_faceused, (uint16_t)((value - 1) % ETERN_PARTS), 1);
            }
            placed++;
        }
    }
    pk->alloc = (uint16_t)placed;
}

/// Plateau cohérent et DIFFÉRENT pour chaque `idx` (cf. l'en-tête).
static inline void fixture_packet_distinct(struct possibility_packet *pk, unsigned idx)
{
    fixture_blank(pk);
    int placed = 0;
    for (int k = 0; k < ETERN_PARTS && k < 31; k++) {
        if (((idx >> k) & 1u) == 0) {
            continue;
        }
        pk->grid[k / ETERN_SIZE][k % ETERN_SIZE] = (int16_t)(k + 1);
        set_face_used(pk->b_faceused, (uint16_t)k, 1);
        placed++;
    }
    pk->alloc = (uint16_t)placed;
}

#endif /* ETII_TEST_PACKET_FIXTURE_H */

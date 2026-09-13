/**
 * @file ring_codec.h
 * @brief Format COMPACT de stockage d'anneaux de bordure (« packed6 »).
 *
 * Un `possibility_packet` pèse 576 octets alors qu'un anneau de bordure ne
 * porte que `BORDER_RING_LEN` pièces. Deux observations, mesurées sur 3 M
 * d'anneaux réels de `data/pieces.csv`, ramènent ça à 45 octets :
 *
 *  1. **La rotation ne porte AUCUNE information.** La face nulle d'une pièce
 *     de bord doit regarder vers l'extérieur du plateau, et la position dit
 *     où est l'extérieur : une seule rotation convient par case. Vérifié
 *     empiriquement — zéro position où la rotation varie d'un anneau à
 *     l'autre, sur 3 M x 60 cases. Seul l'id de pièce est donc stocké.
 *  2. **Il n'y a que 60 pièces de bordure**, donc un index local tient sur
 *     6 bits. 60 cases x 6 bits = 360 bits = 45 octets, pile.
 *
 * Le gain est de 12,8x : 10^9 anneaux passent de 576 Go à 45 Go.
 *
 * **L'accès aléatoire est conservé**, et c'est ce qui a fait préférer ce
 * format à un encodage différentiel (6,4 o/anneau, 90x, mais séquentiel) :
 * les enregistrements sont de taille FIXE, donc le n-ième est en
 * `RING_CODEC_HEADER_BYTES + n * RING_CODEC_PACKED_BYTES`.
 *
 * Ce format n'est PAS un `.back` : il ne s'importe pas tel quel dans un
 * stock. C'était un arbitrage explicite — compacité d'abord, compatibilité
 * plus tard si le besoin se confirme.
 */
#ifndef eternityII_ring_codec_h
#define eternityII_ring_codec_h

#include <stdint.h>
#include <stddef.h>

#include "core/part.h"
#include "core/possibility.h"
#include "tools/border_walk.h"

/** Octets par anneau : BORDER_RING_LEN index de 6 bits, arrondi au supérieur. */
#define RING_CODEC_PACKED_BYTES ((BORDER_RING_LEN * 6 + 7) / 8)

/** Nombre maximal de pièces de bordure adressables par un index de 6 bits. */
#define RING_CODEC_MAX_BORDER_PIECES 64

/** Taille de l'en-tête, fixe — l'offset d'un enregistrement en dépend. */
#define RING_CODEC_HEADER_BYTES 160

/** Magie + version, pour qu'un fichier d'un autre format soit refusé et non
 *  mal interprété (un `.back` relu comme du packed6 donnerait des anneaux
 *  absurdes sans que rien ne le signale). */
#define RING_CODEC_MAGIC "ETIIRING"
#define RING_CODEC_VERSION 1

/**
 * @brief Table des pièces de bordure : l'index 6 bits → l'id de pièce réel.
 *
 * Construite par `ring_codec_build_table` en parcourant les pièces dans
 * l'ordre croissant des ids — donc DÉTERMINISTE pour un jeu de pièces donné.
 * Elle est malgré tout écrite dans l'en-tête : un fichier doit pouvoir se
 * relire sans dépendre de l'ordre qu'une version future donnerait.
 */
struct ring_codec_table {
    uint16_t id[RING_CODEC_MAX_BORDER_PIECES]; /**< index → id de pièce (1-based) */
    int16_t index_of[ETERN_PARTS + 1];         /**< id de pièce → index, -1 si non bordure */
    int count;                                 /**< nombre de pièces de bordure */
};

/**
 * @brief Recense les pièces de bordure (1 ou 2 faces nulles) de `all_rotate_parts`.
 * @return 0 si la table tient sur 6 bits, -1 si le jeu en compte plus de 64.
 */
int ring_codec_build_table(struct array_part *all_rotate_parts, struct ring_codec_table *table);

/**
 * @brief Sérialise l'en-tête dans `buf` (au moins RING_CODEC_HEADER_BYTES).
 *
 * Écrit champ par champ, en petit-boutiste explicite — jamais un `fwrite` de
 * struct, qui embarquerait du bourrage (cf. `possibility_packet`, AGENTS.md).
 */
void ring_codec_write_header(uint8_t *buf, const struct ring_codec_table *table);

/**
 * @brief Relit un en-tête et reconstruit la table.
 * @return 0 si l'en-tête est valide, -1 sinon (magie, version, géométrie ou
 *         taille d'enregistrement incompatibles).
 */
int ring_codec_read_header(const uint8_t *buf, struct ring_codec_table *table);

/**
 * @brief Encode un anneau en RING_CODEC_PACKED_BYTES octets.
 *
 * Les cases sont prises dans l'ordre `border_ring_order` — l'ordre de
 * l'ANNEAU, pas celui du parcours DFS, pour que le format ne dépende pas de
 * la stratégie d'énumération qui l'a produit.
 *
 * @return 0, ou -1 si une case du pourtour est vide ou porte une pièce
 *         absente de la table.
 */
int ring_codec_pack(const struct possibility_packet *ring, const struct ring_codec_table *table,
                    uint8_t *out);

/**
 * @brief Décode un enregistrement en `possibility_packet` complet.
 *
 * Retrouve la rotation de chaque pièce en cherchant, parmi ses 4 rotations,
 * celle dont les faces tournées vers l'EXTÉRIEUR du plateau sont nulles. Une
 * telle rotation est unique (cf. l'observation 1 en tête de fichier) ; s'il
 * n'y en a aucune, l'enregistrement est incohérent et la fonction échoue —
 * c'est ce qui évite de fabriquer un plateau plausible à partir d'octets faux.
 *
 * @return 0, ou -1 si un index est hors table ou sans rotation valide.
 */
int ring_codec_unpack(const uint8_t *in, const struct ring_codec_table *table,
                      struct array_part *all_rotate_parts, struct possibility_packet *ring);

#endif /* eternityII_ring_codec_h */

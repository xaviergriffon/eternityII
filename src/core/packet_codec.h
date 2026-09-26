/**
 * @file packet_codec.h
 * @brief Forme COMPACTE d'un `possibility_packet` pour le STOCKAGE : `.back`,
 *        segments de débordement, et les deux pools de stock en mémoire.
 *
 * Format d'un enregistrement (`c = x * ETERN_SIZE + y`, `a = popcount(bitmap)`) :
 * ```
 *   0 : u8  x, y, checked, réservé (toujours 0)
 *   4 : i16 min_candidats                  petit-boutiste
 *   6 : bitmap[PACKET_CODEC_BITMAP_BYTES]  bit `c` levé <=> case `c` NON VIDE
 *       valeurs[a]                         PACKET_CODEC_VALUE_BITS bits par case
 *                                          non vide, `c` croissant, poids faible
 *                                          d'abord
 * ```
 * Taille : `HEADER + BITMAP + ceil(a x VALUE_BITS / 8)`, soit 65 octets pour une
 * racine à 19 pièces. Sérialisation CHAMP PAR CHAMP : un `fwrite` du struct
 * embarquerait ses 13 octets de bourrage (cf. AGENTS.md).
 *
 * Invariants — les rompre régresse ou casse :
 *  - `alloc` et `b_faceused` sont RECONSTRUITS au décodage, jamais stockés. Un
 *    paquet dont ils contredisent la grille ne revient donc pas identique : le
 *    stock porte une forme CANONIQUE.
 *  - Un enregistrement ne dépasse JAMAIS la forme brute (390 o contre 576),
 *    vérifié à la compilation (`packet_codec_never_larger_than_raw`) et par un
 *    test. Deux formes plus compactes EN MOYENNE ont été écartées là-dessus.
 *  - La valeur d'une case est stockée TELLE QUELLE — domaine `[0, 4 x
 *    ETERN_PARTS]`, plus `-2` pour une case vide. Un sérialiseur ne juge pas la
 *    légalité de ce qu'on lui confie ; seul l'irreprésentable est refusé.
 *  - La détection de format se fait sur la MAGIE de l'en-tête de fichier, jamais
 *    sur une taille : un `.back` hérité reste lu, un en-tête d'une autre version
 *    ou géométrie est refusé bruyamment.
 *  - Ce n'est PAS un format de fil : le protocole échange des paquets bruts.
 *
 * Mesures, formes écartées et pistes à ne pas rejouer :
 * docs/format_stock_compact.md
 */
#ifndef eternityII_packet_codec_h
#define eternityII_packet_codec_h

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "core/possibility.h"

/** Octets d'en-tête d'un enregistrement (x, y, checked, réservé, min_candidats). */
#define PACKET_CODEC_HEADER_BYTES 6

/** Octets du bitmap d'occupation : un bit par case du plateau. */
#define PACKET_CODEC_BITMAP_BYTES ((ETERN_PARTS + 7) / 8)

/** Plus grande valeur qu'une case non vide peut porter : `id_for_rotated_part`
 *  vaut au plus `ETERN_PARTS + ETERN_PARTS * 3`. */
#define PACKET_CODEC_VALUE_MAX (4 * ETERN_PARTS)

/** Bits par case non vide : de quoi coder `[0, PACKET_CODEC_VALUE_MAX]`.
 *  7 bits sur le puzzle 16, 11 sur le puzzle 256. */
#define PACKET_CODEC_VALUE_BITS ( \
    PACKET_CODEC_VALUE_MAX < (1 << 7)  ? 7  : \
    PACKET_CODEC_VALUE_MAX < (1 << 8)  ? 8  : \
    PACKET_CODEC_VALUE_MAX < (1 << 9)  ? 9  : \
    PACKET_CODEC_VALUE_MAX < (1 << 10) ? 10 : \
    PACKET_CODEC_VALUE_MAX < (1 << 11) ? 11 : 12)

/** Octets du plan des valeurs pour `a` cases non vides. */
#define PACKET_CODEC_VALUE_BYTES(a) (((a) * PACKET_CODEC_VALUE_BITS + 7) / 8)

/**
 * @brief Taille maximale d'un enregistrement, plateau plein — 390 octets sur
 *        le puzzle 256, contre 576 pour la forme brute.
 *
 * Borne CONSTANTE : un tampon de cette taille accueille n'importe quel paquet.
 * C'était aussi le pas fixe des segments de débordement jusqu'aux trames
 * (`core/stock_spill.c`) ; seuls les clichés hérités (manifeste v2) le portent
 * encore.
 */
#define PACKET_CODEC_MAX_BYTES (PACKET_CODEC_HEADER_BYTES + PACKET_CODEC_BITMAP_BYTES \
                                + PACKET_CODEC_VALUE_BYTES(ETERN_PARTS))

/** Taille de l'en-tête de FICHIER (`.back` compacté). Fixe : un lecteur le
 *  consomme avant le premier enregistrement. */
#define PACKET_CODEC_FILE_HEADER_BYTES 32

/** Magie d'un fichier de stock compacté — 8 octets, terminateur compris. */
#define PACKET_CODEC_FILE_MAGIC "ETIISTK"

/** Version du format de fichier. Un fichier d'une autre version est REFUSÉ,
 *  jamais réinterprété (cf. `packet_codec_read_file_header`). */
#define PACKET_CODEC_FILE_VERSION 1

/** Octet de DRAPEAUX de l'en-tête de fichier — réservé (zéro) jusqu'ici, donc
 *  un fichier antérieur se relit « sans drapeau », et un binaire antérieur, qui
 *  ne regarde pas cet octet, relit un fichier drapeauté sans broncher (d'où
 *  l'absence de bump de `PACKET_CODEC_FILE_VERSION`). */
#define PACKET_CODEC_FILE_FLAGS_OFFSET 18

/** Drapeau : le fichier porte le stock COMPLET, débordement disque compris
 *  (sauvegarde autonome, `consistent_backup_self_contained`). Un `restore` ne
 *  cherche alors AUCUN cliché de débordement à côté — il n'y en a pas besoin,
 *  et en remettre un en place dupliquerait ce que le fichier contient déjà. */
#define PACKET_CODEC_FILE_FLAG_COMPLETE 0x01

/**
 * @brief Taille qu'occupera `packet` une fois encodé.
 *
 * @param packet Paquet à mesurer (non NULL).
 * @return       Nombre d'octets, toujours <= `PACKET_CODEC_MAX_BYTES`.
 */
size_t packet_codec_encoded_size(const struct possibility_packet *packet);

/**
 * @brief Encode `packet` dans `out`.
 *
 * Échoue — plutôt que de produire un enregistrement plausible — si `grid`
 * porte une valeur que le format ne peut PAS représenter : ni la case vide
 * `-2`, ni une valeur de `[0, PACKET_CODEC_VALUE_MAX]`. Un appelant doit
 * traiter cet échec comme une erreur bruyante : un paquet corrompu en mémoire
 * ne doit pas se sauvegarder sous une forme que le décodeur relirait ensuite
 * sans broncher.
 *
 * @param packet    Paquet source (non NULL).
 * @param out       Tampon destination (non NULL).
 * @param outsize   Octets disponibles dans `out`.
 * @param out_written Reçoit le nombre d'octets écrits (peut être NULL).
 * @return          0 si encodé, -1 si `outsize` est insuffisant ou si `grid`
 *                  porte une valeur hors domaine.
 */
int packet_codec_encode(const struct possibility_packet *packet, uint8_t *out, size_t outsize,
                        size_t *out_written);

/**
 * @brief Décode un enregistrement vers un `possibility_packet` complet.
 *
 * Reconstruit `alloc` (popcount du bitmap) et `b_faceused` (depuis le plan des
 * valeurs) : aucun des deux n'est stocké, tous deux sont déductibles (cf. les
 * mesures en tête de fichier). Les cases vides reçoivent `-2`.
 *
 * @param in        Enregistrement source (non NULL).
 * @param insize    Octets lisibles depuis `in`.
 * @param out       Paquet destination (non NULL) — entièrement réécrit,
 *                  bourrage compris (`memset` initial), pour qu'un paquet
 *                  décodé ne porte jamais d'octet indéterminé.
 * @param out_consumed Reçoit la taille de l'enregistrement lu (peut être NULL).
 * @return          0 si décodé, -1 si `insize` est trop court ou si
 *                  l'enregistrement est incohérent (identifiant hors table).
 */
int packet_codec_decode(const uint8_t *in, size_t insize, struct possibility_packet *out,
                        size_t *out_consumed);

/**
 * @brief Nombre de cases non vides d'un enregistrement, SANS le décoder.
 *
 * C'est le `popcount` du bitmap d'occupation, donc `alloc`. Les balayages qui
 * ne veulent que la profondeur (histogramme du stock, tris, choix de la file
 * la plus chargée) passent sur tout le stock : leur faire reconstituer un
 * plateau de 576 octets par possibilité serait absurde.
 *
 * @return Le nombre de cases occupées, 0 si l'enregistrement est trop court.
 */
uint16_t packet_codec_peek_placed(const uint8_t *in, size_t insize);

/**
 * @brief Score MRV porté par l'en-tête de l'enregistrement, sans décoder.
 * @return Le score, ou `POSSIBILITY_MIN_CANDIDATS_UNKNOWN` si l'enregistrement
 *         est trop court.
 */
int16_t packet_codec_peek_min_candidats(const uint8_t *in, size_t insize);

/**
 * @brief Écrit le drapeau `checked` DANS l'enregistrement, sans le décoder ni
 *        le réencoder — c'est un octet d'en-tête à décalage fixe.
 * @return 0 si écrit, -1 si l'enregistrement est trop court.
 */
int packet_codec_poke_checked(uint8_t *out, size_t outsize, uint8_t checked);

/**
 * @brief Sérialise l'en-tête de fichier dans `buf` (>= PACKET_CODEC_FILE_HEADER_BYTES).
 *
 * Porte la géométrie compilée (`ETERN_SIZE`, `ETERN_PARTS`) en plus de la
 * magie et de la version : un `.back` de puzzle 4x4 relu par un binaire 16x16
 * est ainsi refusé net, là où l'ancien format brut se contentait de produire
 * des plateaux absurdes.
 */
void packet_codec_write_file_header(uint8_t *buf);

/// Variante de `packet_codec_write_file_header` portant des drapeaux
/// (`PACKET_CODEC_FILE_FLAG_*`).
void packet_codec_write_file_header_flags(uint8_t *buf, uint8_t flags);

/// Drapeaux d'un en-tête (`PACKET_CODEC_FILE_FLAG_*`) — 0 pour un en-tête
/// antérieur à leur introduction. Ne valide rien : appeler
/// `packet_codec_read_file_header` d'abord.
uint8_t packet_codec_file_header_flags(const uint8_t *buf);

/**
 * @brief Relit un en-tête de fichier.
 * @return 0 s'il est valide et compatible avec la géométrie compilée,
 *         -1 sinon (magie, version, géométrie ou taille d'enregistrement).
 */
int packet_codec_read_file_header(const uint8_t *buf);

/**
 * @brief Écrit `packet` dans `f` sous forme compactée.
 *
 * @return 0 en cas de succès, -1 sur échec d'encodage ou d'écriture.
 */
int packet_codec_fwrite(FILE *f, const struct possibility_packet *packet);

/**
 * @brief Lit l'enregistrement compacté suivant de `f`.
 *
 * Deux `fread` : l'en-tête + le bitmap (taille fixe), qui donne le nombre de
 * cases non vides, puis le plan des valeurs. Contre un tampon stdio d'un mégaoctet
 * ce sont deux `memcpy`, pas deux appels système.
 *
 * @return 1 si un enregistrement a été lu, 0 sur fin de fichier PROPRE
 *         (aucun octet disponible), -1 sur fin de fichier au milieu d'un
 *         enregistrement ou sur enregistrement incohérent.
 */
int packet_codec_fread(FILE *f, struct possibility_packet *packet);

#endif /* eternityII_packet_codec_h */

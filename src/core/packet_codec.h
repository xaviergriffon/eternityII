/**
 * @file packet_codec.h
 * @brief Forme COMPACTE d'un `possibility_packet` pour le STOCKAGE sur
 *        disque (sauvegardes `.back`, segments de débordement).
 *
 * `struct possibility_packet` pèse 576 octets quel que soit le remplissage du
 * plateau, alors qu'une possibilité du stock n'en a presque aucune case
 * occupée. Mesuré sur un stock de production réel (`eternityII.back`,
 * 3 407 891 possibilités, 1 963 Mo) : **19,2 pièces posées en moyenne sur
 * 256**, soit un plateau vide à 92,5 % payé au prix fort. Trois redondances
 * s'ajoutent à ça, vérifiées sur ces 3,4 M paquets sans une seule exception :
 *
 *  1. **`b_faceused` (34 o) est intégralement déductible de `grid`** — 0
 *     divergence sur 3 407 891. Il n'est donc pas stocké, mais reconstruit au
 *     décodage : bit `(v - 1) % ETERN_PARTS` levé pour chaque case portant une
 *     valeur `v >= 1`. Un paquet dont le masque CONTREDIRAIT sa grille ne
 *     revient donc pas identique d'un aller-retour — état qui n'existe pas en
 *     production, par construction de `generate_possibility_packet`.
 *  2. **`alloc` vaut exactement le nombre de cases non vides** — 0 divergence.
 *     Il n'est pas stocké non plus : c'est le `popcount` du bitmap.
 *  3. **13 octets de bourrage par paquet** (trou d'alignement 518-527 et queue
 *     563-575) partaient tels quels dans le `.back`, valeurs indéterminées
 *     comprises, parce que la sauvegarde faisait un `fwrite` du struct. Ce
 *     codec sérialise CHAMP PAR CHAMP, en petit-boutiste explicite — même
 *     règle que `tests/tools/ring_codec.c`, et pour la même raison : un
 *     `fwrite` de struct embarque du bourrage (cf. AGENTS.md, « never
 *     memcmp/hash the raw struct »).
 *
 * ## Le format, et pourquoi celui-là
 *
 * ```
 *   0 : u8  x                       (conservé : hashé/comparé par le pool analysé)
 *   1 : u8  y                              idem (hash_possibility_key, compare_possibility)
 *   2 : u8  checked
 *   3 : u8  réservé (toujours 0)
 *   4 : i16 min_candidats           petit-boutiste
 *   6 : bitmap[PACKET_CODEC_BITMAP_BYTES]  bit `c` levé <=> case `c` NON VIDE
 *       valeurs[a]                  PACKET_CODEC_VALUE_BITS bits par case non
 *                                   vide, ordre croissant de `c`, poids faible
 *                                   d'abord
 * ```
 * avec `c = x * ETERN_SIZE + y` et `a = popcount(bitmap)`. Taille :
 * `PACKET_CODEC_HEADER_BYTES + PACKET_CODEC_BITMAP_BYTES + ceil(a x BITS / 8)`,
 * soit **65 octets pour une racine à 19 pièces** et jamais plus de
 * `PACKET_CODEC_MAX_BYTES` (390 sur le puzzle 256, contre 576).
 *
 * **La valeur de la case est stockée TELLE QUELLE**, pas décomposée en
 * (identifiant, rotation). Une décomposition serait plus compacte d'un bit par
 * case (`id_for_rotated_part` couvre exactement `[1, 4 x ETERN_PARTS]`, soit
 * 10 bits sur le puzzle 256) mais elle ne saurait représenter que les valeurs
 * LÉGALES — et un sérialiseur n'a pas à juger de la légalité de ce qu'on lui
 * confie. Une première version le faisait, refusait toute autre valeur, et
 * échouait sur une demi-douzaine de fixtures de test qui construisent un
 * paquet par `memset(0)` : le coût de la rigueur retombait sur les appelants
 * plutôt que sur la corruption qu'elle prétendait attraper. Le domaine est
 * donc `[0, 4 x ETERN_PARTS]` pour une case non vide, plus `-2` pour une case
 * vide ; tout le reste (une valeur négative autre que `-2`, une valeur
 * au-delà de la dernière rotation) reste refusé, parce que réellement pas
 * représentable.
 *
 * Trois formes ont été prototypées et mesurées sur ces mêmes 3,4 M paquets
 * réels, deux ont été ÉCARTÉES — elles gagnent sur ce stock-ci et **perdent
 * sur un plateau profond**, ce qui en fait un piège :
 *
 * | Forme | Moyenne | Ratio | Pire cas (plateau plein) |
 * |---|---|---|---|
 * | Liste `(case, id, rot)` sur 18 bits | 47,5 o | x12,1 | **580 o** (pire que 576) |
 * | Liste alignée, 3 o par pièce | 61,6 o | x9,35 | **772 o** |
 * | **Bitmap + plan de valeurs (retenue)** | 65,2 o | **x8,83** | **390 o** |
 *
 * La forme retenue est la seule **bornée sous la taille actuelle** : elle ne
 * peut pas régresser, quel que soit le profil de profondeur du stock — et
 * c'est vérifié à la compilation (`packet_codec_never_larger_than_raw`) ainsi
 * que par un test. Sur le `.back` de production mesuré, **1 963 Mo tombent à
 * 222 Mo**. Débit : 2,84 M paquets/s à l'encodage, 2,00 M/s au décodage
 * (aller-retour exact sur les 3 407 891 paquets, zéro divergence) — soit
 * environ 1,2 s de processeur pour sauvegarder ce stock entier, négligeable
 * devant ses E/S.
 *
 * ## Ce que ce codec n'est PAS
 *
 * Ce n'est pas un format de FIL : le protocole client/serveur continue
 * d'échanger des `possibility_packet` bruts, et le bump de `VERSION` qu'un
 * changement de fil imposerait n'a pas lieu d'être ici. C'est le même
 * arbitrage explicite que `ring_codec.h` (« compacité d'abord, compatibilité
 * plus tard si le besoin se confirme »).
 *
 * C'est en revanche, depuis la mesure qui l'a justifié, la forme EN MÉMOIRE des
 * deux pools de STOCK (`init_file_variable`, `core/datamanager.c`) : 632 ->
 * 121,2 octets par possibilité, 2154 Mo -> 413 Mo (x5,21) sur un stock réel. Le
 * pool ANALYSÉ, lui, reste en paquets bruts — son chemin chaud est une
 * déduplication à chaque acquittement, qu'un décodage par candidat comparé
 * taxerait pour une économie sans objet (il est borné par les possibilités en
 * vol chez les clients).
 *
 * ## Pistes ÉCARTÉES — ne pas les rejouer sans lire la raison
 *
 *  - **Encodage différentiel** (un paquet décrit par rapport à son
 *    prédécesseur). Tentant, `.back` et segments étant purement séquentiels,
 *    mais son gain marginal au-dessus de la forme bitmap est petit et il
 *    couple chaque paquet à son voisin dans un fichier qui doit survivre à une
 *    restauration PARTIELLE.
 *  - **Décomposer la valeur d'une case en (identifiant, rotation)** : un bit de
 *    moins par case, mais ne sait représenter que les valeurs LÉGALES. Essayé,
 *    puis retiré — cf. « un sérialiseur ne juge pas la légalité », AGENTS.md.
 *  - **Compacter le format de FIL** (INST_ADD/INST_GET) : diviserait la bande
 *    passante par ~9, mais impose un bump de `VERSION` (poignée de main en
 *    correspondance exacte). Voir ci-dessus.
 *  - **Compacter le stock local du CLIENT** (« étage 3 » du plan d'origine) :
 *    ABANDONNÉ, décision prise après mesure. Le stock local est borné par
 *    `max_stock_per_thread` et la vraie empreinte d'un client est la map
 *    partagée en copie sur écriture ; le seul autre morceau qui pèse est le
 *    tampon de SORTIE `aposs->possibilities` (`max_stock_by_thread` x 576
 *    octets par fork, non partagés), qu'encoder coûterait sur un chemin
 *    semi-chaud. Le client mono (mode `test`, petits formats) est le seul à
 *    exécuter le codec côté client, et il n'a pas de problème de mémoire.
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
 * Borne CONSTANTE, et c'est ce qui permet à `core/stock_spill.c` de garder
 * son arithmétique d'octets à pas fixe (cf. sa doc) : un enregistrement de
 * cette taille accueille n'importe quel paquet.
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

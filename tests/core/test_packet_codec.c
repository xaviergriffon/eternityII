/*
 * Tests unitaires de packet_codec.c — forme compacte d'un `possibility_packet`
 * pour le stockage disque (sauvegardes `.back`, segments de débordement).
 *
 * Ce que ces tests verrouillent, dans l'ordre d'importance :
 *
 *  1. L'ALLER-RETOUR est exact sur tous les champs qui comptent — grille, x,
 *     y, checked, min_candidats — ET sur les deux champs DÉDUITS au décodage
 *     plutôt que stockés (`alloc`, `b_faceused`). C'est la seule chose qui
 *     rende le format acceptable : une possibilité sauvegardée puis restaurée
 *     doit être la même possibilité.
 *  2. Un enregistrement n'est JAMAIS plus gros que la forme brute — c'est la
 *     propriété qui garantit qu'aucun profil de stock ne peut faire régresser
 *     la taille d'une sauvegarde (cf. le tableau des formes écartées dans
 *     core/packet_codec.h, où deux variantes plus petites en moyenne
 *     dépassent 576 octets sur un plateau plein).
 *  3. Une grille hors domaine est REFUSÉE, jamais encodée en quelque chose de
 *     plausible.
 *  4. L'en-tête de fichier refuse une géométrie ou une version étrangère.
 *
 * Ces tests n'écrivent rien sur disque : le volet « fichier .back compacté /
 * hérité » est couvert côté datamanager (tests/core/test_datamanager.c).
 */
#include "greatest.h"
#include "core/packet_codec.h"
#include "core/possibility.h"
#include "core/part.h"

#include <string.h>

/* Générateur déterministe (xorshift) : un test qui échoue doit être rejouable
 * à l'identique, jamais « parfois ». */
static uint32_t rng_state = 0x1234567u;
static uint32_t rng_next(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* Profondeur EFFECTIVE d'une fixture : le suite tourne aussi en build 4x4
 * (ETERN_PARTS=16, cf. `make test-16`), où « 40 pièces posées » n'existe pas
 * — un tableau de cases indexé au-delà déborderait silencieusement sur macOS
 * et ne serait attrapé que par ASan sous Linux. */
static int depth(int wanted)
{
    return (wanted < ETERN_PARTS) ? wanted : ETERN_PARTS;
}

/* Construit un plateau à `placed` pièces distinctes, posées sur des cases
 * tirées au sort, chacune avec une rotation quelconque — exactement la forme
 * qu'a une possibilité du stock (plateau partiellement rempli). */
static void make_packet(struct possibility_packet *p, int placed)
{
    memset(p, 0, sizeof *p);
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            p->grid[x][y] = -2;
        }
    }
    p->x = (uint8_t)(rng_next() % ETERN_SIZE);
    p->y = (uint8_t)(rng_next() % ETERN_SIZE);
    p->checked = (uint8_t)(rng_next() & 1);
    p->min_candidats = (int16_t)((rng_next() % 300) - 1);

    int cells[ETERN_PARTS];
    for (int i = 0; i < ETERN_PARTS; i++) {
        cells[i] = i;
    }
    for (int i = ETERN_PARTS - 1; i > 0; i--) { /* Fisher-Yates */
        int j = (int)(rng_next() % (uint32_t)(i + 1));
        int t = cells[i]; cells[i] = cells[j]; cells[j] = t;
    }

    for (int k = 0; k < placed; k++) {
        int cell = cells[k];
        uint16_t id = (uint16_t)(k + 1);              /* pièces distinctes */
        uint8_t rot = (uint8_t)(rng_next() & 3);
        p->grid[cell / ETERN_SIZE][cell % ETERN_SIZE] = (int16_t)id_for_rotated_part(id, rot);
        set_face_used(p->b_faceused, (uint16_t)(id - 1), 1);
    }
    p->alloc = (uint16_t)placed;
}

static int packets_equal(const struct possibility_packet *a, const struct possibility_packet *b)
{
    return a->x == b->x && a->y == b->y && a->checked == b->checked
           && a->alloc == b->alloc && a->min_candidats == b->min_candidats
           && memcmp(a->grid, b->grid, sizeof a->grid) == 0
           && memcmp(a->b_faceused, b->b_faceused, sizeof a->b_faceused) == 0;
}

/* ---------------------------------------------------------------------- */

TEST round_trip_is_exact_at_every_fill_level(void)
{
    /* Toutes les profondeurs, pas un échantillon : le plateau vide et le
     * plateau plein sont précisément les deux bords où un calcul de taille
     * faux passerait inaperçu sur un tirage aléatoire. */
    for (int placed = 0; placed <= ETERN_PARTS; placed++) {
        struct possibility_packet src, dst;
        make_packet(&src, placed);

        uint8_t record[PACKET_CODEC_MAX_BYTES];
        size_t written = 0;
        ASSERT_EQ_FMT(0, packet_codec_encode(&src, record, sizeof record, &written), "%d");
        ASSERT_EQ_FMT(packet_codec_encoded_size(&src), written, "%zu");

        size_t consumed = 0;
        ASSERT_EQ_FMT(0, packet_codec_decode(record, written, &dst, &consumed), "%d");
        ASSERT_EQ_FMT(written, consumed, "%zu");
        ASSERT(packets_equal(&src, &dst));
    }
    PASS();
}

TEST alloc_and_faceused_are_rebuilt_not_stored(void)
{
    /* Les deux champs déduits au décodage : un paquet dont `alloc` et
     * `b_faceused` MENTENT doit ressortir avec les valeurs VRAIES, celles que
     * la grille impose. C'est ce qui rend légitime de ne pas les stocker —
     * et ce test tomberait si quelqu'un les ajoutait au format en les
     * recopiant tels quels. */
    struct possibility_packet src, dst;
    make_packet(&src, depth(37));

    struct possibility_packet truth = src;
    src.alloc = 999;
    memset(src.b_faceused, 0xFF, sizeof src.b_faceused);

    uint8_t record[PACKET_CODEC_MAX_BYTES];
    size_t written = 0;
    ASSERT_EQ_FMT(0, packet_codec_encode(&src, record, sizeof record, &written), "%d");
    ASSERT_EQ_FMT(0, packet_codec_decode(record, written, &dst, NULL), "%d");

    ASSERT_EQ_FMT(depth(37), (int)dst.alloc, "%d");
    ASSERT_MEM_EQ(truth.b_faceused, dst.b_faceused, sizeof truth.b_faceused);
    PASS();
}

TEST a_record_is_never_larger_than_the_raw_struct(void)
{
    /* Propriété qui interdit toute régression de taille, quel que soit le
     * profil de profondeur du stock — deux formes plus petites EN MOYENNE ont
     * été écartées pour avoir échoué exactement ici (cf. packet_codec.h). */
    struct possibility_packet full;
    make_packet(&full, ETERN_PARTS);
    ASSERT(packet_codec_encoded_size(&full) <= sizeof(struct possibility_packet));
    ASSERT_EQ_FMT((size_t)PACKET_CODEC_MAX_BYTES, packet_codec_encoded_size(&full), "%zu");

    /* Et le gain réel sur une racine peu profonde, l'usage courant : un stock
     * de production réel tient à 19 pièces posées sur 256 en moyenne (cf.
     * packet_codec.h). Exprimé en FRACTION du plateau pour rester un plateau
     * « peu rempli » dans les deux builds, y compris le 4x4 où 19 pièces sont
     * déjà le plateau plein. */
    struct possibility_packet shallow;
    make_packet(&shallow, ETERN_PARTS / 8);
    ASSERT(packet_codec_encoded_size(&shallow) < packet_codec_encoded_size(&full));
    ASSERT(packet_codec_encoded_size(&shallow) * 2 < sizeof(struct possibility_packet));
    PASS();
}

TEST decoded_padding_is_deterministic(void)
{
    /* Le pool analysé hache le paquet OCTET PAR OCTET (hash_possibility_key,
     * core/datamanager.c) : deux décodages du même enregistrement doivent
     * donner deux images mémoire identiques jusqu'au bourrage d'alignement,
     * sinon un paquet restauré ne se dédupliquerait jamais contre son jumeau
     * produit en direct. */
    struct possibility_packet src, a, b;
    make_packet(&src, depth(44));
    uint8_t record[PACKET_CODEC_MAX_BYTES];
    size_t written = 0;
    ASSERT_EQ_FMT(0, packet_codec_encode(&src, record, sizeof record, &written), "%d");

    memset(&a, 0xAA, sizeof a);
    memset(&b, 0x55, sizeof b);
    ASSERT_EQ_FMT(0, packet_codec_decode(record, written, &a, NULL), "%d");
    ASSERT_EQ_FMT(0, packet_codec_decode(record, written, &b, NULL), "%d");
    ASSERT_MEM_EQ(&a, &b, sizeof a);
    PASS();
}

TEST encode_refuses_only_what_it_cannot_represent(void)
{
    struct possibility_packet p, dst;
    uint8_t record[PACKET_CODEC_MAX_BYTES];

    /* Refusé : une valeur négative qui n'est pas la case vide. */
    make_packet(&p, depth(12));
    p.grid[1][2] = -1;
    ASSERT_EQ_FMT(-1, packet_codec_encode(&p, record, sizeof record, NULL), "%d");

    /* Refusé : au-delà de la dernière rotation de la dernière pièce. */
    make_packet(&p, depth(12));
    p.grid[1][2] = (int16_t)(PACKET_CODEC_VALUE_MAX + 1);
    ASSERT_EQ_FMT(-1, packet_codec_encode(&p, record, sizeof record, NULL), "%d");

    /* ACCEPTÉ, et restitué tel quel : 0 n'est pas un identifiant de pièce, mais
     * un sérialiseur n'a pas à juger de la légalité d'un plateau — il doit
     * rendre ce qu'on lui a confié. C'est ce qui rend écrivable un paquet
     * construit par `memset(0)`, idiome répandu dans cette base de test. */
    make_packet(&p, depth(12));
    p.grid[1][2] = 0;
    size_t written = 0;
    ASSERT_EQ_FMT(0, packet_codec_encode(&p, record, sizeof record, &written), "%d");
    ASSERT_EQ_FMT(0, packet_codec_decode(record, written, &dst, NULL), "%d");
    ASSERT_EQ_FMT(0, (int)dst.grid[1][2], "%d");

    /* La borne haute exacte, elle, passe. */
    make_packet(&p, depth(12));
    p.grid[1][2] = (int16_t)PACKET_CODEC_VALUE_MAX;
    ASSERT_EQ_FMT(0, packet_codec_encode(&p, record, sizeof record, &written), "%d");
    ASSERT_EQ_FMT(0, packet_codec_decode(record, written, &dst, NULL), "%d");
    ASSERT_EQ_FMT(PACKET_CODEC_VALUE_MAX, (int)dst.grid[1][2], "%d");
    PASS();
}

TEST a_zeroed_packet_round_trips(void)
{
    /* Le paquet entièrement nul (`memset(0)`) : toutes les cases « non vides »
     * à 0, `b_faceused` nul. Aucune pièce n'étant désignée, le masque
     * reconstruit reste nul lui aussi — donc l'aller-retour est exact. */
    struct possibility_packet src, dst;
    memset(&src, 0, sizeof src);
    uint8_t record[PACKET_CODEC_MAX_BYTES];
    size_t written = 0;
    ASSERT_EQ_FMT(0, packet_codec_encode(&src, record, sizeof record, &written), "%d");
    ASSERT_EQ_FMT(0, packet_codec_decode(record, written, &dst, NULL), "%d");
    ASSERT_MEM_EQ(src.grid, dst.grid, sizeof src.grid);
    ASSERT_MEM_EQ(src.b_faceused, dst.b_faceused, sizeof src.b_faceused);
    ASSERT_EQ_FMT(ETERN_PARTS, (int)dst.alloc, "%d"); /* toutes les cases sont « non vides » */
    PASS();
}

TEST encode_refuses_a_buffer_too_small(void)
{
    struct possibility_packet p;
    make_packet(&p, depth(60));
    size_t needed = packet_codec_encoded_size(&p);
    uint8_t record[PACKET_CODEC_MAX_BYTES];
    ASSERT_EQ_FMT(-1, packet_codec_encode(&p, record, needed - 1, NULL), "%d");
    ASSERT_EQ_FMT(0, packet_codec_encode(&p, record, needed, NULL), "%d");
    PASS();
}

TEST decode_refuses_a_truncated_record(void)
{
    struct possibility_packet src, dst;
    make_packet(&src, depth(40));
    uint8_t record[PACKET_CODEC_MAX_BYTES];
    size_t written = 0;
    ASSERT_EQ_FMT(0, packet_codec_encode(&src, record, sizeof record, &written), "%d");

    /* Coupé n'importe où avant la fin : refusé, jamais un plateau partiel
     * silencieusement plausible. */
    for (size_t cut = 0; cut < written; cut++) {
        ASSERT_EQ_FMT(-1, packet_codec_decode(record, cut, &dst, NULL), "%d");
    }
    ASSERT_EQ_FMT(0, packet_codec_decode(record, written, &dst, NULL), "%d");
    PASS();
}

TEST file_header_round_trips_and_refuses_a_foreign_one(void)
{
    uint8_t header[PACKET_CODEC_FILE_HEADER_BYTES];
    packet_codec_write_file_header(header);
    ASSERT_EQ_FMT(0, packet_codec_read_file_header(header), "%d");

    /* Magie fausse. */
    uint8_t bad[PACKET_CODEC_FILE_HEADER_BYTES];
    memcpy(bad, header, sizeof bad);
    bad[0] = 'X';
    ASSERT_EQ_FMT(-1, packet_codec_read_file_header(bad), "%d");

    /* Version inconnue : refusée, jamais « lue quand même ». */
    memcpy(bad, header, sizeof bad);
    bad[8] = (uint8_t)(PACKET_CODEC_FILE_VERSION + 1);
    ASSERT_EQ_FMT(-1, packet_codec_read_file_header(bad), "%d");

    /* Autre géométrie : c'est précisément ce que l'ancien format brut, sans
     * en-tête, ne pouvait pas distinguer — il produisait des plateaux
     * absurdes en silence. */
    memcpy(bad, header, sizeof bad);
    bad[12] = (uint8_t)(bad[12] ^ 0x01); /* ETERN_PARTS */
    ASSERT_EQ_FMT(-1, packet_codec_read_file_header(bad), "%d");
    PASS();
}

/* L'octet de drapeaux était réservé (zéro) : un en-tête sans drapeau se relit
 * « 0 », un en-tête drapeauté reste un en-tête VALIDE — c'est ce qui dispense
 * de bumper PACKET_CODEC_FILE_VERSION pour marquer une sauvegarde autonome. */
TEST file_header_flags_round_trip_without_breaking_validity(void)
{
    uint8_t header[PACKET_CODEC_FILE_HEADER_BYTES];
    packet_codec_write_file_header(header);
    ASSERT_EQ_FMT(0, (int)packet_codec_file_header_flags(header), "%d");

    packet_codec_write_file_header_flags(header, PACKET_CODEC_FILE_FLAG_COMPLETE);
    ASSERT_EQ_FMT(0, packet_codec_read_file_header(header), "%d");
    ASSERT_EQ_FMT(PACKET_CODEC_FILE_FLAG_COMPLETE, (int)packet_codec_file_header_flags(header), "%d");

    /* Seul l'octet de drapeaux diffère d'un en-tête ordinaire. */
    uint8_t plain[PACKET_CODEC_FILE_HEADER_BYTES];
    packet_codec_write_file_header(plain);
    for (int i = 0; i < PACKET_CODEC_FILE_HEADER_BYTES; i++) {
        if (i != PACKET_CODEC_FILE_FLAGS_OFFSET) {
            ASSERT_EQ_FMT((int)plain[i], (int)header[i], "%d");
        }
    }
    PASS();
}

TEST fwrite_fread_round_trip_through_a_stream(void)
{
    /* Plusieurs enregistrements de TAILLES DIFFÉRENTES à la suite : c'est le
     * cas que le format auto-descriptif doit tenir — le lecteur n'a aucun
     * champ de longueur, il déduit la taille du bitmap qu'il vient de lire. */
    const int depths[] = { 0, 1, depth(19), depth(60), ETERN_PARTS, 7 };
    const int nb = (int)(sizeof depths / sizeof depths[0]);
    struct possibility_packet src[6];

    FILE *f = tmpfile();
    ASSERT(f != NULL);
    for (int i = 0; i < nb; i++) {
        make_packet(&src[i], depths[i]);
        ASSERT_EQ_FMT(0, packet_codec_fwrite(f, &src[i]), "%d");
    }
    rewind(f);

    for (int i = 0; i < nb; i++) {
        struct possibility_packet dst;
        ASSERT_EQ_FMT(1, packet_codec_fread(f, &dst), "%d");
        ASSERT(packets_equal(&src[i], &dst));
    }
    /* Fin de fichier PROPRE : 0, pas -1 — un lecteur doit pouvoir distinguer
     * « plus rien à lire » de « enregistrement tronqué ». */
    struct possibility_packet extra;
    ASSERT_EQ_FMT(0, packet_codec_fread(f, &extra), "%d");
    fclose(f);
    PASS();
}

TEST fread_reports_a_truncated_stream(void)
{
    struct possibility_packet src;
    make_packet(&src, depth(50));
    uint8_t record[PACKET_CODEC_MAX_BYTES];
    size_t written = 0;
    ASSERT_EQ_FMT(0, packet_codec_encode(&src, record, sizeof record, &written), "%d");

    FILE *f = tmpfile();
    ASSERT(f != NULL);
    ASSERT_EQ_FMT(written - 1, fwrite(record, 1, written - 1, f), "%zu");
    rewind(f);

    struct possibility_packet dst;
    ASSERT_EQ_FMT(-1, packet_codec_fread(f, &dst), "%d");
    fclose(f);
    PASS();
}

SUITE(packet_codec_suite)
{
    rng_state = 0x1234567u; /* même graine à chaque exécution */
    RUN_TEST(round_trip_is_exact_at_every_fill_level);
    RUN_TEST(alloc_and_faceused_are_rebuilt_not_stored);
    RUN_TEST(a_record_is_never_larger_than_the_raw_struct);
    RUN_TEST(decoded_padding_is_deterministic);
    RUN_TEST(encode_refuses_only_what_it_cannot_represent);
    RUN_TEST(a_zeroed_packet_round_trips);
    RUN_TEST(encode_refuses_a_buffer_too_small);
    RUN_TEST(decode_refuses_a_truncated_record);
    RUN_TEST(file_header_round_trips_and_refuses_a_foreign_one);
    RUN_TEST(file_header_flags_round_trip_without_breaking_validity);
    RUN_TEST(fwrite_fread_round_trip_through_a_stream);
    RUN_TEST(fread_reports_a_truncated_stream);
}

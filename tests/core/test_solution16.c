/*
 * Tests unitaires « solution réelle » du puzzle 4×4 (ETERN_PARTS=16).
 *
 * Contrairement aux autres suites — volontairement indépendantes de la taille du
 * puzzle et construites sur de petits fixtures à la main — celle-ci EXIGE le build
 * 16 cases : elle s'appuie sur un plateau RÉELLEMENT résolu, capturé une fois via
 * le moteur (save_solution_csv) puis figé dans tests/fixtures/solution16.json et
 * transformé au build en tableau C (tests/fixtures/solution16.h, généré par
 * tests/fixtures/gen_solution16.py). Cette « golden fixture » sert trois
 * périmètres :
 *   - RÉSOLUTION   : la solution est reconnue (check_possibility == 0,
 *                    checkIfResultFound sort EXIT_SUCCESS, log_solution écrit un
 *                    fichier CSV lisible avec les bonnes colonnes et le bon nombre de lignes).
 *   - NON-RÉSOLUTION : des copies dégradées de la fixture sont rejetées avec le
 *                    bon code de check_possibility, et checkIfResultFound ne sort pas.
 *   - COMMUNICATION : le paquet solution circule sur un socketpair (protocole
 *                    INST_SOLUTION + send_all / recv_all + INST_CONSIDERED) et
 *                    reste identique après transfert.
 *
 * Les pièces tournées sont reconstruites à partir du contenu de data/pieces16.csv
 * (embarqué ci-dessous, écrit dans un fichier temporaire — aucune dépendance au
 * CWD), ce qui garantit que les indices stockés dans la grille « golden » pointent
 * sur les mêmes pièces que lorsque le moteur a produit la solution.
 */
#include "greatest.h"
#include "core/possibility.h"
#include "core/part.h"
#include "core/readdata.h"
#include "net/etii_protocol.h"
#include "app/etii_server.h"        /* communicate_with_client_step, client_t */
#include "app/static_variables.h"   /* stop_on_solution */
#include "fork_assert.h"
#include "fixtures/solution16.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <dirent.h>

#if ETERN_PARTS != 16
#error "test_solution16.c doit être compilé avec -DETERN_PARTS=16"
#endif

/* Contenu de data/pieces16.csv, embarqué pour rester indépendant du CWD. */
static const char *PIECES16_CSV =
    "ntiles: 16\n"
    "1 3 0 1 5\n"
    "2 2 4 0 0\n"
    "3 0 0 1 2\n"
    "4 1 7 2 0\n"
    "5 8 6 6 8\n"
    "6 7 3 0 4\n"
    "7 5 7 6 6\n"
    "8 8 3 0 3\n"
    "9 1 0 3 7\n"
    "10 0 4 2 0\n"
    "11 6 5 7 7\n"
    "12 1 0 0 3\n"
    "13 6 5 8 5\n"
    "14 0 4 8 4\n"
    "15 0 2 5 4\n"
    "16 2 8 1 0\n";

/* Écrit PIECES16_CSV dans un fichier temporaire et renvoie son chemin (statique). */
static const char *write_pieces16_csv(void)
{
    static char path[] = "/tmp/etii_pieces16_XXXXXX";
    strcpy(path, "/tmp/etii_pieces16_XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0) return NULL;
    FILE *fp = fdopen(fd, "w");
    if (fp == NULL) { close(fd); return NULL; }
    fputs(PIECES16_CSV, fp);
    fclose(fp);
    return path;
}

/* Reconstruit le tableau de toutes les rotations depuis pieces16.csv.
   apart (pièces brutes) est libéré ici ; l'appelant libère le tableau renvoyé. */
static struct array_part *make_rotate_parts(void)
{
    const char *path = write_pieces16_csv();
    if (path == NULL) return NULL;
    struct array_part *apart = read_parts(path);
    unlink(path);
    struct array_part *rot = rotate_all_parts(apart);
    free_array_part(apart);
    return rot;
}

/* Construit le paquet « golden » depuis la fixture : grille pleine, (x,y) et
   alloc capturés, b_faceused recalculé (pièce id -> bit id-1, comme le moteur). */
static struct possibility_packet *build_golden(struct array_part *rot)
{
    struct possibility_packet *p = calloc(1, sizeof(*p));
    p->alloc = SOLUTION16_ALLOC;
    p->x = SOLUTION16_X;
    p->y = SOLUTION16_Y;
    for (int x = 0; x < SOLUTION16_SIZE; x++) {
        for (int y = 0; y < SOLUTION16_SIZE; y++) {
            int16_t v = SOLUTION16_GRID[x][y];
            p->grid[x][y] = v;
            uint16_t id = rot->parts[v].id;   /* pièce posée (rotation 0 ici) */
            set_face_used(p->b_faceused, id - 1, 1);
        }
    }
    return p;
}

/* Contexte partagé avec les fonctions-fils (copié par fork). */
static struct possibility_packet *g_poss;
static struct array_part *g_rot;
static char g_solution_dir[256];

/* Fonction-fils : grille complète -> checkIfResultFound exit(EXIT_SUCCESS).
   On se place dans un répertoire temporaire pour confiner le ./solution_* écrit. */
static void child_check_result_found(void)
{
    if (chdir(g_solution_dir) != 0) _exit(99);
    checkIfResultFound(g_poss, g_rot);
}

/* Fonction-fils : sauvegarde la solution puis _exit(0) (log_solution ne quitte pas). */
static void child_log_solution(void)
{
    if (chdir(g_solution_dir) != 0) _exit(99);
    log_solution(g_poss, g_rot);
}

/* --------------------------------------------------------------------------
 * RÉSOLUTION
 * ------------------------------------------------------------------------ */

/* La solution réelle passe la validation complète (bords cohérents, alloc plein). */
TEST golden_solution_is_valid(void)
{
    struct array_part *rot = make_rotate_parts();
    ASSERT(rot != NULL);
    struct possibility_packet *p = build_golden(rot);

    ASSERT_EQ_FMT(SOLUTION16_SIZE * SOLUTION16_SIZE, (int)p->alloc, "%d"); /* 16 pièces */
    ASSERT_EQ_FMT(0, check_possibility(p, rot), "%d");

    free(p);
    free_array_part(rot);
    PASS();
}

/* Grille complète (alloc == ETERN_PARTS) : checkIfResultFound sort EXIT_SUCCESS. */
TEST golden_check_if_result_found_exits_success(void)
{
    strcpy(g_solution_dir, "/tmp/etii_sol16_XXXXXX");
    ASSERT(mkdtemp(g_solution_dir) != NULL);

    g_rot = make_rotate_parts();
    ASSERT(g_rot != NULL);
    g_poss = build_golden(g_rot);

    pid_t pid = 0;
    int code = run_in_fork(child_check_result_found, &pid);

    char sol[320];
    snprintf(sol, sizeof(sol), "%s/solution_%d_0.csv", g_solution_dir, (int)pid);
    unlink(sol);
    rmdir(g_solution_dir);

    free(g_poss);
    free_array_part(g_rot);
    ASSERT_EQ_FMT(EXIT_SUCCESS, code, "%d");
    PASS();
}

/* log_solution écrit un CSV lisible avec en-tête et une ligne par pièce. */
TEST golden_log_solution_roundtrips(void)
{
    strcpy(g_solution_dir, "/tmp/etii_sol16r_XXXXXX");
    ASSERT(mkdtemp(g_solution_dir) != NULL);

    g_rot = make_rotate_parts();
    ASSERT(g_rot != NULL);
    g_poss = build_golden(g_rot);

    pid_t pid = 0;
    int code = run_in_fork(child_log_solution, &pid);
    ASSERT_EQ_FMT(0, code, "%d"); /* log_solution ne quitte pas */

    char sol[320];
    snprintf(sol, sizeof(sol), "%s/solution_%d_0.csv", g_solution_dir, (int)pid);
    FILE *f = fopen(sol, "r");
    ASSERT(f != NULL);

    char line[256];
    /* en-tête */
    ASSERT(fgets(line, sizeof(line), f) != NULL);
    ASSERT_STR_EQ("row,col,piece_id,rotation,top,right,bottom,left\n", line);

    /* une ligne par pièce placée */
    int count = 0;
    while (fgets(line, sizeof(line), f)) count++;
    fclose(f);
    unlink(sol);
    rmdir(g_solution_dir);

    ASSERT_EQ_FMT(SOLUTION16_SIZE * SOLUTION16_SIZE, count, "%d");

    free(g_poss);
    free_array_part(g_rot);
    PASS();
}

/* --------------------------------------------------------------------------
 * NON-RÉSOLUTION (copies dégradées de la fixture)
 * ------------------------------------------------------------------------ */

/* Une pièce retirée (alloc reculé) : ce n'est plus une solution complète. */
TEST degraded_missing_piece_is_not_complete(void)
{
    struct array_part *rot = make_rotate_parts();
    ASSERT(rot != NULL);
    struct possibility_packet *p = build_golden(rot);

    p->alloc = SOLUTION16_ALLOC - 1; /* 15 < ETERN_PARTS */
    /* checkIfResultFound ne doit PAS sortir (grille incomplète) : retour normal. */
    checkIfResultFound(p, rot);
    ASSERT(p->alloc < ETERN_PARTS);

    free(p);
    free_array_part(rot);
    PASS();
}

/* Deux pièces permutées : un bord ne concorde plus -> -9. */
TEST degraded_swapped_pieces_breaks_edge(void)
{
    struct array_part *rot = make_rotate_parts();
    ASSERT(rot != NULL);
    struct possibility_packet *p = build_golden(rot);

    int16_t tmp = p->grid[0][0];
    p->grid[0][0] = p->grid[SOLUTION16_SIZE - 1][SOLUTION16_SIZE - 1];
    p->grid[SOLUTION16_SIZE - 1][SOLUTION16_SIZE - 1] = tmp;

    ASSERT_EQ_FMT(-9, check_possibility(p, rot), "%d");

    free(p);
    free_array_part(rot);
    PASS();
}

/* Une case posée contenant un indice de grille hors plage -> -7. */
TEST degraded_invalid_grid_value_is_minus_seven(void)
{
    struct array_part *rot = make_rotate_parts();
    ASSERT(rot != NULL);
    struct possibility_packet *p = build_golden(rot);

    p->grid[0][0] = 30000; /* >= rot->size */
    ASSERT_EQ_FMT(-7, check_possibility(p, rot), "%d");

    free(p);
    free_array_part(rot);
    PASS();
}

/* Masque de pièces utilisées vidé alors qu'alloc=16 -> faceused < alloc -> -5. */
TEST degraded_cleared_faceused_is_minus_five(void)
{
    struct array_part *rot = make_rotate_parts();
    ASSERT(rot != NULL);
    struct possibility_packet *p = build_golden(rot);

    memset(p->b_faceused, 0, sizeof(p->b_faceused));
    ASSERT_EQ_FMT(-5, check_possibility(p, rot), "%d");

    free(p);
    free_array_part(rot);
    PASS();
}

/* alloc > ETERN_PARTS -> -4 ; coordonnées hors plateau -> -2 ; paquet NULL -> -1. */
TEST degraded_out_of_range_fields(void)
{
    struct array_part *rot = make_rotate_parts();
    ASSERT(rot != NULL);

    struct possibility_packet *p = build_golden(rot);
    p->alloc = ETERN_PARTS + 1;
    ASSERT_EQ_FMT(-4, check_possibility(p, rot), "%d");
    free(p);

    p = build_golden(rot);
    p->x = ETERN_SIZE; /* hors plateau */
    ASSERT_EQ_FMT(-2, check_possibility(p, rot), "%d");
    free(p);

    ASSERT_EQ_FMT(-1, check_possibility(NULL, rot), "%d");

    free_array_part(rot);
    PASS();
}

/* --------------------------------------------------------------------------
 * COMMUNICATION (la solution circule sur le fil)
 * ------------------------------------------------------------------------ */

/* Échange complet calqué sur send_solution : INST_SOLUTION + send_all(paquet),
   le pair acquitte par INST_CONSIDERED. Le paquet reçu doit être identique. */
TEST solution_round_trips_over_socket(void)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) FAILm("socketpair");

    struct array_part *rot = make_rotate_parts();
    ASSERT(rot != NULL);
    struct possibility_packet *golden = build_golden(rot);

    /* Côté client : annonce la solution. */
    ASSERT(send_instruction(sv[0], INST_SOLUTION) > 0);
    ASSERT_EQ_FMT((long)sizeof(*golden),
                  send_all(sv[0], golden, sizeof(*golden)), "%ld");

    /* Côté serveur : reçoit l'instruction, le paquet, puis acquitte. */
    ASSERT_EQ_FMT((int)INST_SOLUTION, (int)recv_instruction(sv[1]), "%d");
    struct possibility_packet received;
    ASSERT_EQ_FMT((long)sizeof(received),
                  recv_all(sv[1], &received, sizeof(received)), "%ld");
    ASSERT_EQ_FMT(0, compare_possibility(golden, &received), "%d"); /* transfert fidèle */
    ASSERT(send_instruction(sv[1], INST_CONSIDERED) > 0);

    /* Côté client : l'acquittement attendu. */
    ASSERT_EQ_FMT((int)INST_CONSIDERED, (int)recv_instruction(sv[0]), "%d");

    close(sv[0]);
    close(sv[1]);
    free(golden);
    free_array_part(rot);
    PASS();
}

/* --------------------------------------------------------------------------
 * COMMUNICATION — branche INST_SOLUTION de communicate_with_client_step
 *
 * On exerce le vrai code serveur (réception du paquet + save_solution_csv +
 * INST_CONSIDERED, puis, avec --stop-on-solution, backup + exit) en lui passant
 * un bout d'un socketpair et la VRAIE solution 4×4. Les fichiers écrits
 * (solution_server_*, *.back) sont confinés dans un répertoire temporaire.
 * ------------------------------------------------------------------------ */

/* Vide puis supprime un répertoire (fichiers de premier niveau uniquement). */
static void empty_and_remove_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (d != NULL) {
        struct dirent *e;
        char path[512];
        while ((e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
            unlink(path);
        }
        closedir(d);
    }
    rmdir(dir);
}

/* Contexte du fils pour la branche --stop-on-solution (copié par fork). */
static client_t g_sol_client;

/* Fonction-fils : INST_SOLUTION avec stop_on_solution -> backup + exit(EXIT_SUCCESS). */
static void child_solution_stop(void)
{
    if (chdir(g_solution_dir) != 0) _exit(99);
    stop_on_solution = 1;
    array_possibility_packet *last = NULL;
    int vsupp = 1;
    communicate_with_client_step(&g_sol_client, INST_SOLUTION, &last, &vsupp);
    /* La branche gagnante quitte par exit() : un retour ici est une anomalie. */
    _exit(42);
}

/* Sans --stop-on-solution : la solution est sauvegardée, acquittée, et on
   continue à servir le client (retour 1). */
TEST solution_step_no_stop_acks_and_continues(void)
{
    char cwd[512];
    ASSERT(getcwd(cwd, sizeof cwd) != NULL);
    strcpy(g_solution_dir, "/tmp/etii_solstep_XXXXXX");
    ASSERT(mkdtemp(g_solution_dir) != NULL);

    struct array_part *rot = make_rotate_parts();
    ASSERT(rot != NULL);
    struct possibility_packet *golden = build_golden(rot);

    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    ASSERT_EQ((long)sizeof(*golden), send_all(sv[0], golden, sizeof(*golden)));

    client_t client;
    memset(&client, 0, sizeof client);
    client.socket_id = sv[1];
    client.compteur = 0;
    client.rotate_parts = rot;

    int saved_stop = stop_on_solution;
    stop_on_solution = 0;

    array_possibility_packet *last = NULL;
    int vsupp = 1;

    /* Confine ./solution_server_*.csv dans le répertoire temporaire. */
    ASSERT_EQ(0, chdir(g_solution_dir));
    int cont = communicate_with_client_step(&client, INST_SOLUTION, &last, &vsupp);
    int8_t ack = recv_instruction(sv[0]);
    ASSERT_EQ(0, chdir(cwd));   /* restaure le CWD AVANT assertions/cleanup */

    stop_on_solution = saved_stop;
    close(sv[0]);
    close(sv[1]);
    empty_and_remove_dir(g_solution_dir);
    free(golden);
    free_array_part(rot);

    ASSERT_EQ_FMT(1, cont, "%d");                        /* on continue à servir */
    ASSERT_EQ_FMT((int)INST_CONSIDERED, (int)ack, "%d"); /* solution acquittée */
    PASS();
}

/* Avec --stop-on-solution : le premier gagnant sauvegarde le stock et
   exit(EXIT_SUCCESS). Exécuté en fils car la branche appelle exit(). */
TEST solution_step_stop_backs_up_and_exits(void)
{
    strcpy(g_solution_dir, "/tmp/etii_solstop_XXXXXX");
    ASSERT(mkdtemp(g_solution_dir) != NULL);

    g_rot = make_rotate_parts();
    ASSERT(g_rot != NULL);
    struct possibility_packet *golden = build_golden(g_rot);

    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    ASSERT_EQ((long)sizeof(*golden), send_all(sv[0], golden, sizeof(*golden)));

    memset(&g_sol_client, 0, sizeof g_sol_client);
    g_sol_client.socket_id = sv[1];
    g_sol_client.compteur = 0;
    g_sol_client.rotate_parts = g_rot;

    pid_t pid = 0;
    int code = run_in_fork(child_solution_stop, &pid);

    close(sv[0]);
    close(sv[1]);
    empty_and_remove_dir(g_solution_dir);
    free(golden);
    free_array_part(g_rot);

    ASSERT_EQ_FMT(EXIT_SUCCESS, code, "%d");   /* branche gagnante -> exit(EXIT_SUCCESS) */
    PASS();
}

SUITE(solution16_suite)
{
    /* Un send vers un pair fermé ne doit jamais tuer le runner. */
    signal(SIGPIPE, SIG_IGN);

    /* Résolution */
    RUN_TEST(golden_solution_is_valid);
    RUN_TEST(golden_check_if_result_found_exits_success);
    RUN_TEST(golden_log_solution_roundtrips);
    /* Non-résolution */
    RUN_TEST(degraded_missing_piece_is_not_complete);
    RUN_TEST(degraded_swapped_pieces_breaks_edge);
    RUN_TEST(degraded_invalid_grid_value_is_minus_seven);
    RUN_TEST(degraded_cleared_faceused_is_minus_five);
    RUN_TEST(degraded_out_of_range_fields);
    /* Communication */
    RUN_TEST(solution_round_trips_over_socket);
    RUN_TEST(solution_step_no_stop_acks_and_continues);
    RUN_TEST(solution_step_stop_backs_up_and_exits);
}

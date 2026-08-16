/*
 * Tests unitaires de stock_spill.c — débordement sur disque du stock serveur
 * (PR2 de la série plafond RAM, --stock-max-ram/--stock-spill-dir).
 *
 * Contrairement à test_datamanager.c (état global en mémoire, mono-thread,
 * pas d'E/S réelle), ces tests font de VRAIES E/S disque dans un répertoire
 * temporaire dédié par test (mkdtemp) — nettoyé en fin de chaque test, y
 * compris sur un chemin d'échec (best-effort). `nb_file_possibility` et les
 * pools de stock RAM sont l'état global déjà mis en place par
 * tests/test_main.c (datamanager_configure_stock_files) ; chaque test
 * commence par vider les pools (mêmes helpers que test_datamanager.c,
 * dupliqués ici — chaque fichier de test est indépendant par convention).
 */
#include "greatest.h"
#include "core/datamanager.h"
#include "core/possibility.h"
#include "core/stock_spill.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>

/* Réservés aux tests, non déclarés dans les en-têtes de production — même
 * convention que tests/core/test_datamanager.c. */
void datamanager_reset_rr_state_for_tests(void);
void datamanager_set_ram_limit_packets_for_tests(unsigned long long packets);
void stock_spill_set_segment_bytes_for_tests(long bytes);

/* ---------------------------------------------------------------------- */
/* Capture stderr (même technique que test_datamanager.c : mesure la TAILLE,
 * jamais le contenu — un log reformulé ne doit pas casser ces tests). */
static int g_cap_fd1 = -1, g_cap_fd2 = -1;
static char g_cap_path[128];
static void capture_stderr(void)
{
    fflush(stdout); fflush(stderr);
    snprintf(g_cap_path, sizeof g_cap_path, "/tmp/etii_spill_stderr_%d", (int)getpid());
    g_cap_fd1 = dup(1);
    g_cap_fd2 = dup(2);
    int dn = open("/dev/null", O_WRONLY);
    dup2(dn, 1); close(dn);
    int f = open(g_cap_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    dup2(f, 2); close(f);
}
static long restore_stderr_size(void)
{
    fflush(stdout); fflush(stderr);
    dup2(g_cap_fd1, 1); close(g_cap_fd1);
    dup2(g_cap_fd2, 2); close(g_cap_fd2);
    struct stat st;
    long sz = (stat(g_cap_path, &st) == 0) ? (long)st.st_size : -1;
    unlink(g_cap_path);
    return sz;
}

/* Même macro que tests/ui/test_command_lines.c : root outrepasse chmod. */
#define SKIP_IF_ROOT() \
    do { \
        if (geteuid() == 0) \
            SKIPm("root outrepasse les permissions : chmod 0444 sans effet"); \
    } while (0)

/* ---------------------------------------------------------------------- */
/* Helpers de pool (dupliqués de test_datamanager.c — chaque fichier de test
 * reste indépendant par convention de ce projet). */
static void drain_datamanager(void)
{
    while (datas_size() > 0) {
        array_possibility_packet *r = get_last_possibility(NULL, 1000, NULL);
        free_array_possibility_packet(r);
    }
    datamanager_reset_rr_state_for_tests();
}

static void add_packets(const int *allocs, int n)
{
    array_possibility_packet arr;
    arr.size = n;
    arr.possibilities = calloc((size_t)n, sizeof(struct possibility_packet));
    for (int i = 0; i < n; i++) {
        arr.possibilities[i].alloc = (uint16_t)allocs[i];
        arr.possibilities[i].checked = 0;
    }
    add_possibility(NULL, &arr); /* server_ip == NULL -> put_to_local */
    free(arr.possibilities);
}

static char *make_tmp_spill_dir(char *tmpl_buf)
{
    strcpy(tmpl_buf, "/tmp/etii_spill_XXXXXX");
    return mkdtemp(tmpl_buf);
}

/* Nettoyage best-effort d'un répertoire de test (fichiers directs, pas de
 * sous-répertoires — jamais nécessaire ici, stock_spill.c ne crée que des
 * fichiers plats dans le répertoire configuré). */
static void rmdir_recursive(const char *dir)
{
    DIR *d = opendir(dir);
    if (d == NULL) {
        rmdir(dir);
        return;
    }
    struct dirent *entry;
    char path[PATH_MAX];
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
        unlink(path);
    }
    closedir(d);
    rmdir(dir);
}

/* ---------------------------------------------------------------------- */

/* stock_spill_configure crée le répertoire s'il n'existe pas encore, et le
 * module démarre bien vide (aucun débordement résiduel). */
TEST configure_creates_directory_and_starts_empty(void)
{
    char tmpl[64];
    char *base = make_tmp_spill_dir(tmpl);
    ASSERT(base != NULL);
    char subdir[PATH_MAX];
    snprintf(subdir, sizeof(subdir), "%s/spilldir", base);

    stock_spill_configure(subdir, nb_file_possibility);

    struct stat st;
    ASSERT_EQ_FMT(0, stat(subdir, &st), "%d");
    ASSERT(S_ISDIR(st.st_mode));
    ASSERT_EQ_FMT(0ULL, stock_spill_total_packets(), "%llu");
    ASSERT_EQ_FMT(0ULL, stock_spill_total_segments(), "%llu");

    rmdir_recursive(subdir);
    rmdir_recursive(base);
    PASS();
}

/* Répertoire parent non inscriptible : le module se désactive proprement
 * (erreur journalisée une fois), jamais de crash ni de blocage — et reste un
 * no-op silencieux même face à un plafond RAM dépassé. */
TEST configure_degrades_gracefully_when_directory_unwritable(void)
{
    SKIP_IF_ROOT();

    char tmpl[64];
    char *base = make_tmp_spill_dir(tmpl);
    ASSERT(base != NULL);
    if (chmod(base, 0444) != 0) {
        rmdir(base);
        SKIPm("chmod non supporté sur cet environnement");
    }

    char subdir[PATH_MAX];
    snprintf(subdir, sizeof(subdir), "%s/spilldir", base);

    capture_stderr();
    stock_spill_configure(subdir, nb_file_possibility);
    long err_bytes = restore_stderr_size();

    ASSERT(err_bytes > 0); /* échec de création journalisé */
    ASSERT_EQ_FMT(0ULL, stock_spill_total_packets(), "%llu");

    drain_datamanager();
    datamanager_set_ram_limit_packets_for_tests(0); /* efface tout plafond résiduel avant l'ajout */
    int allocs[] = { 1, 2, 3 };
    add_packets(allocs, 3);
    datamanager_set_ram_limit_packets_for_tests(1); /* plafond largement dépassé */

    int moved = stock_spill_step(1000);
    ASSERT_EQ_FMT(0, moved, "%d"); /* désactivé pour tout le process : no-op */
    ASSERT_EQ_FMT(3ULL, file_size(0), "%llu"); /* rien n'a bougé */

    datamanager_set_ram_limit_packets_for_tests(0);
    drain_datamanager();
    chmod(base, 0755);
    rmdir(base);
    PASS();
}

/* Purge au démarrage : ne supprime QUE les fichiers correspondant EXACTEMENT
 * au motif spill_[uc]_<n>_<n>.dat, jamais un effacement générique du
 * répertoire — et journalise la perte (aucun segment résiduel n'est
 * silencieusement avalé). */
TEST configure_purges_matching_segments_and_spares_others(void)
{
    char tmpl[64];
    char *dir = make_tmp_spill_dir(tmpl);
    ASSERT(dir != NULL);

    char seg_path[PATH_MAX];
    snprintf(seg_path, sizeof(seg_path), "%s/spill_u_0_1.dat", dir);
    struct possibility_packet fake[3];
    memset(fake, 0, sizeof(fake));
    FILE *f = fopen(seg_path, "wb");
    ASSERT(f != NULL);
    ASSERT_EQ_FMT((size_t)3, fwrite(fake, sizeof(fake[0]), 3, f), "%zu");
    fclose(f);

    char other_path[PATH_MAX];
    snprintf(other_path, sizeof(other_path), "%s/notaspillfile.txt", dir);
    f = fopen(other_path, "wb");
    ASSERT(f != NULL);
    fputs("garder", f);
    fclose(f);

    capture_stderr();
    stock_spill_configure(dir, nb_file_possibility);
    long err_bytes = restore_stderr_size();

    ASSERT(err_bytes > 0); /* perte de données réelle, journalisée */
    struct stat st;
    ASSERT(stat(seg_path, &st) != 0);              /* segment supprimé */
    ASSERT_EQ_FMT(0, stat(other_path, &st), "%d"); /* fichier étranger épargné */

    unlink(other_path);
    rmdir_recursive(dir);
    PASS();
}

/* Sans plafond RAM (illimité, cas par défaut) : stock_spill_step est un
 * no-op silencieux, quel que soit l'état du stock. */
TEST step_is_noop_without_ram_cap(void)
{
    char tmpl[64];
    char *dir = make_tmp_spill_dir(tmpl);
    ASSERT(dir != NULL);
    stock_spill_configure(dir, nb_file_possibility);

    drain_datamanager();
    datamanager_set_ram_limit_packets_for_tests(0); /* efface tout plafond résiduel avant l'ajout */
    int allocs[100];
    for (int i = 0; i < 100; i++) {
        allocs[i] = i + 1;
    }
    add_packets(allocs, 100);

    /* illimité (répété : c'est le cas testé, pas seulement une précaution) */
    datamanager_set_ram_limit_packets_for_tests(0);
    int moved = stock_spill_step(4096);
    ASSERT_EQ_FMT(0, moved, "%d");
    ASSERT_EQ_FMT(100ULL, file_size(0), "%llu");
    ASSERT_EQ_FMT(0ULL, stock_spill_total_packets(), "%llu");

    drain_datamanager();
    rmdir_recursive(dir);
    PASS();
}

/* L'éviction retire la TÊTE (les possibilités les plus anciennes, jamais
 * servies) et jamais la queue : après éviction, les possibilités RESTÉES en
 * RAM sont exactement les plus récemment ajoutées, sans trou. Le total
 * (résident + déporté) est conservé à tout instant. */
TEST evict_removes_oldest_first_and_conserves_total(void)
{
    char tmpl[64];
    char *dir = make_tmp_spill_dir(tmpl);
    ASSERT(dir != NULL);
    stock_spill_configure(dir, nb_file_possibility);

    drain_datamanager();
    datamanager_set_ram_limit_packets_for_tests(0); /* efface tout plafond résiduel avant l'ajout */
    int allocs[2000];
    for (int i = 0; i < 2000; i++) {
        allocs[i] = i + 1; /* ordre d'ajout croissant : alloc 1 = le plus ancien */
    }
    add_packets(allocs, 2000);
    ASSERT_EQ_FMT(2000ULL, file_size(0), "%llu");

    /* Plafond à 1000 : haut=900, bas=750. Budget volontairement PETIT (100,
     * très inférieur à l'excédent à évacuer) pour observer une convergence
     * incrémentale plutôt qu'une évacuation en un seul appel. */
    datamanager_set_ram_limit_packets_for_tests(1000);
    int rounds = 0;
    while (file_size(0) > 900 && rounds < 30) {
        stock_spill_step(100);
        rounds++;
    }

    unsigned long long resident = file_size(0);
    unsigned long long spilled = stock_spill_total_packets();
    ASSERT_EQ_FMT(2000ULL, resident + spilled, "%llu"); /* rien perdu */
    ASSERT(resident <= 900ULL);
    ASSERT(spilled > 0ULL);

    array_possibility_packet *r = get_last_possibility(NULL, (int)resident, NULL);
    ASSERT_EQ_FMT((int)resident, r->size, "%d");
    int min_alloc = 100000;
    int max_alloc = 0;
    for (int i = 0; i < r->size; i++) {
        int a = r->possibilities[i].alloc;
        if (a < min_alloc) { min_alloc = a; }
        if (a > max_alloc) { max_alloc = a; }
    }
    free_array_possibility_packet(r);

    /* Les survivants sont exactement les `resident` DERNIERS ajoutés :
     * alloc [2000-resident+1 .. 2000], sans trou -- preuve que l'éviction a
     * pris la tête (les plus anciens), jamais la queue. */
    ASSERT_EQ_FMT(2000, max_alloc, "%d");
    ASSERT_EQ_FMT((int)(2000ULL - resident + 1ULL), min_alloc, "%d");

    datamanager_set_ram_limit_packets_for_tests(0);
    drain_datamanager();
    rmdir_recursive(dir);
    PASS();
}

/* Rechargement : quand l'occupation résidente tombe sous 25 % du plafond et
 * qu'un débordement existe, stock_spill_step le récupère automatiquement.
 * Vérifie la conservation du total à chaque étape ET la préservation exacte
 * des champs (jamais un memcmp du struct brut -- padding caché, cf.
 * possibility-packet-struct-padding). */
TEST reload_restores_evicted_data_when_ram_drops_and_preserves_fields(void)
{
    char tmpl[64];
    char *dir = make_tmp_spill_dir(tmpl);
    ASSERT(dir != NULL);
    stock_spill_configure(dir, nb_file_possibility);

    drain_datamanager();
    datamanager_set_ram_limit_packets_for_tests(0); /* efface tout plafond résiduel avant l'ajout */
    array_possibility_packet arr;
    arr.size = 20;
    arr.possibilities = calloc(20, sizeof(struct possibility_packet));
    for (int i = 0; i < 20; i++) {
        arr.possibilities[i].alloc = (uint16_t)(i + 1);
        arr.possibilities[i].checked = 0;
        arr.possibilities[i].grid[0][0] = (int16_t)(1000 + i); /* marqueur distinctif */
    }
    add_possibility(NULL, &arr);
    free(arr.possibilities);
    ASSERT_EQ_FMT(20ULL, file_size(0), "%llu");

    /* Plafond à 5 (haut=4, bas=3, rechargement=1). Budget PETIT (3) pour une
     * convergence incrémentale observable. */
    datamanager_set_ram_limit_packets_for_tests(5);
    int rounds = 0;
    while (file_size(0) > 4 && rounds < 20) {
        stock_spill_step(3);
        rounds++;
    }
    unsigned long long spilled = stock_spill_total_packets();
    ASSERT(spilled > 0ULL);
    ASSERT_EQ_FMT(20ULL, file_size(0) + spilled, "%llu");

    /* Vide tout le résident restant (simule des GET qui consomment le
     * stock) : force le rechargement à s'activer sur le prochain appel,
     * quel que soit le plafond (0 <= 25 % de n'importe quelle valeur > 0). */
    array_possibility_packet *drained = get_last_possibility(NULL, 1000, NULL);
    for (int i = 0; i < drained->size; i++) {
        int a = drained->possibilities[i].alloc;
        ASSERT_EQ_FMT(1000 + (a - 1), (int)drained->possibilities[i].grid[0][0], "%d");
    }
    unsigned long long drained_count = (unsigned long long)drained->size;
    free_array_possibility_packet(drained);
    ASSERT_EQ_FMT(20ULL, drained_count + spilled, "%llu");
    ASSERT_EQ_FMT(0ULL, file_size(0), "%llu");

    /* Relève le plafond bien au-dessus du total (20) : sous le plafond
     * précédent (5), le rechargement vise 75 % de 5 = 3 et s'arrête là par
     * conception (ne remplit jamais au-delà du seuil BAS -- cf. la doc de
     * stock_spill_step) ; une partie du débordement resterait alors sur
     * disque INDÉFINIMENT, ce qui est le comportement voulu, pas un bug.
     * Ici on simule un opérateur qui relève --stock-max-ram : tout doit
     * alors pouvoir revenir. */
    datamanager_set_ram_limit_packets_for_tests(1000);
    rounds = 0;
    while (stock_spill_total_packets() > 0 && rounds < 30) {
        stock_spill_step(3);
        rounds++;
    }
    ASSERT_EQ_FMT(0ULL, stock_spill_total_packets(), "%llu");
    /* Seules les `spilled` possibilités DÉPORTÉES reviennent -- les
     * `drained_count` extraites juste au-dessus pour simuler des GET ont
     * été consommées (libérées), exactement comme un client qui a reçu une
     * possibilité et ne l'a jamais rendue : elles ne réapparaissent jamais. */
    ASSERT_EQ_FMT(spilled, file_size(0), "%llu");

    array_possibility_packet *reloaded = get_last_possibility(NULL, (int)spilled, NULL);
    ASSERT_EQ_FMT((int)spilled, reloaded->size, "%d");
    for (int i = 0; i < reloaded->size; i++) {
        int a = reloaded->possibilities[i].alloc;
        ASSERT(a >= 1 && a <= 20);
        ASSERT_EQ_FMT(1000 + (a - 1), (int)reloaded->possibilities[i].grid[0][0], "%d");
    }
    free_array_possibility_packet(reloaded);

    datamanager_set_ram_limit_packets_for_tests(0);
    drain_datamanager();
    rmdir_recursive(dir);
    PASS();
}

/* Franchissement d'une frontière de segment : avec une taille de segment
 * minuscule (test-only), 50 possibilités s'étalent sur plusieurs segments.
 * Vérifie que l'éviction écrit correctement à travers plusieurs segments en
 * un seul appel ET que le rechargement les dépile correctement un par un
 * (jusqu'à vider et supprimer chacun), sans jamais perdre le total. */
TEST evict_and_reload_span_multiple_segments(void)
{
    char tmpl[64];
    char *dir = make_tmp_spill_dir(tmpl);
    ASSERT(dir != NULL);
    stock_spill_configure(dir, nb_file_possibility);
    long packet_size = (long)sizeof(struct possibility_packet);
    stock_spill_set_segment_bytes_for_tests(5 * packet_size); /* 5 possibilités/segment */

    drain_datamanager();
    datamanager_set_ram_limit_packets_for_tests(0); /* efface tout plafond résiduel avant l'ajout */
    int allocs[50];
    for (int i = 0; i < 50; i++) {
        allocs[i] = i + 1;
    }
    add_packets(allocs, 50);
    ASSERT_EQ_FMT(50ULL, file_size(0), "%llu");

    datamanager_set_ram_limit_packets_for_tests(10);
    int rounds = 0;
    while (file_size(0) > 9 && rounds < 30) {
        stock_spill_step(4096); /* budget large : exerce le rollover multi-segment en un appel */
        rounds++;
    }
    unsigned long long resident_after_evict = file_size(0);
    unsigned long long spilled_after_evict = stock_spill_total_packets();
    ASSERT_EQ_FMT(50ULL, resident_after_evict + spilled_after_evict, "%llu");
    ASSERT(spilled_after_evict >= 10ULL); /* étalé sur au moins 2 segments de 5 */
    ASSERT(stock_spill_total_segments() >= 2ULL);

    array_possibility_packet *r = get_last_possibility(NULL, 1000, NULL);
    free_array_possibility_packet(r);
    ASSERT_EQ_FMT(0ULL, file_size(0), "%llu");

    /* Relève le plafond bien au-dessus du total (50) : sous cap=10, le
     * rechargement viserait 75 % de 10 = 7 et s'arrêterait là par
     * conception (cf. reload_restores_evicted_data_when_ram_drops_...) --
     * ici on veut vérifier le dépilement complet, multi-segment, jusqu'à
     * suppression de tous les segments. */
    datamanager_set_ram_limit_packets_for_tests(1000);
    rounds = 0;
    while (stock_spill_total_packets() > 0 && rounds < 50) {
        stock_spill_step(4096);
        rounds++;
    }
    ASSERT_EQ_FMT(0ULL, stock_spill_total_packets(), "%llu");
    ASSERT_EQ_FMT(0ULL, stock_spill_total_segments(), "%llu"); /* tous les segments supprimés */
    ASSERT_EQ_FMT(50ULL, file_size(0), "%llu");

    datamanager_set_ram_limit_packets_for_tests(0);
    drain_datamanager();
    rmdir_recursive(dir);
    PASS();
}

SUITE(stock_spill_suite)
{
    RUN_TEST(configure_creates_directory_and_starts_empty);
    RUN_TEST(configure_degrades_gracefully_when_directory_unwritable);
    RUN_TEST(configure_purges_matching_segments_and_spares_others);
    RUN_TEST(step_is_noop_without_ram_cap);
    RUN_TEST(evict_removes_oldest_first_and_conserves_total);
    RUN_TEST(reload_restores_evicted_data_when_ram_drops_and_preserves_fields);
    RUN_TEST(evict_and_reload_span_multiple_segments);
}

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

/* Grille "vide partout sauf une case" (-2 = case vide, cf.
 * possibility_placed_count) : depuis VERSION 13, stock_spill_reload()
 * recompte `alloc` au sens nombre de cases pleines dès qu'un paquet
 * retraverse le disque (docs/autosearch_step.md) — une
 * grille calloc'ée (tout à 0, jamais -2) serait donc vue comme entièrement
 * pleine (256) après un aller-retour d'éviction/rechargement. `grid[0][0]`
 * porte le marqueur distinctif ET la seule case pleine : la valeur de
 * `alloc` posée ici survit tant que le paquet reste résident (jamais
 * recomptée), mais un paquet qui a fait un aller-retour disque revient
 * TOUJOURS avec `alloc == 1` (une case pleine) -- c'est `grid[0][0]`, pas
 * `alloc`, qui reste l'identifiant fiable après un rechargement. */
static void init_empty_grid(struct possibility_packet *p)
{
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            p->grid[x][y] = -2;
        }
    }
}

static void add_packets(const int *allocs, int n)
{
    array_possibility_packet arr;
    arr.size = n;
    arr.possibilities = calloc((size_t)n, sizeof(struct possibility_packet));
    for (int i = 0; i < n; i++) {
        init_empty_grid(&arr.possibilities[i]);
        arr.possibilities[i].grid[0][0] = (int16_t)allocs[i];
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

/* Nettoyage best-effort d'un répertoire de test — récursif d'un niveau
 * (fichiers directs + sous-répertoires) : depuis PR3, un cliché vit dans un
 * sous-répertoire ("snap"/"snap-inexistant") du répertoire configuré. */
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
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            DIR *sub = opendir(path);
            if (sub != NULL) {
                struct dirent *subentry;
                char subpath[PATH_MAX];
                while ((subentry = readdir(sub)) != NULL) {
                    if (strcmp(subentry->d_name, ".") == 0 || strcmp(subentry->d_name, "..") == 0) {
                        continue;
                    }
                    snprintf(subpath, sizeof(subpath), "%s/%s", path, subentry->d_name);
                    unlink(subpath);
                }
                closedir(sub);
            }
            rmdir(path);
        } else {
            unlink(path);
        }
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
        init_empty_grid(&arr.possibilities[i]);
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

    /* VERSION 13 (docs/autosearch_step.md) : un paquet
     * qui a fait un aller-retour par un segment de débordement traverse
     * stock_spill_reload(), qui recompte `alloc` au sens nombre de cases
     * pleines -- ici toujours 1, la seule case posée par construction. La
     * formule `1000 + (alloc - 1)` d'avant cette bascule ne tient donc plus
     * (alloc ne code plus l'ordre d'ajout) : c'est `grid[0][0]`, jamais
     * recompté, qui reste l'identifiant fiable pour vérifier que chaque
     * possibilité déportée est revenue sans perte ni duplication. */
    array_possibility_packet *reloaded = get_last_possibility(NULL, (int)spilled, NULL);
    ASSERT_EQ_FMT((int)spilled, reloaded->size, "%d");
    int seen[20] = {0};
    for (int i = 0; i < reloaded->size; i++) {
        ASSERT_EQ_FMT(1, (int)reloaded->possibilities[i].alloc, "%d"); /* recompté : une seule case pleine */
        int marker = reloaded->possibilities[i].grid[0][0];
        ASSERT(marker >= 1000 && marker < 1020);
        int idx = marker - 1000;
        ASSERT(!seen[idx]); /* jamais deux fois le même marqueur */
        seen[idx] = 1;
    }
    free_array_possibility_packet(reloaded);

    datamanager_set_ram_limit_packets_for_tests(0);
    drain_datamanager();
    rmdir_recursive(dir);
    PASS();
}

/* Avant ce comportement, stock_spill_step (appelé toutes les 100 ms par le
 * thread de débordement) ne journalisait RIEN sur ses bascules de mode --
 * seules les erreurs d'E/S l'étaient. Une pression RAM prolongée (bascules
 * répétées éviction/rechargement) n'était donc reconstituable après coup
 * qu'en croisant des métriques indirectes, jamais directement dans
 * events.log. Un log_event par TRANSITION (pas par tick, cf. le commentaire
 * dans stock_spill_step) doit maintenant y apparaître aux 4 franchissements :
 * début/fin d'éviction, début/fin de rechargement. events.log est écrit dans
 * le CWD du process de test (indépendant du répertoire de débordement dédié
 * ci-dessus), d'où le nettoyage séparé. */
TEST step_logs_eviction_and_reload_transitions_to_events_log(void)
{
    char tmpl[64];
    char *dir = make_tmp_spill_dir(tmpl);
    ASSERT(dir != NULL);
    stock_spill_configure(dir, nb_file_possibility);
    unlink("events.log");

    drain_datamanager();
    datamanager_set_ram_limit_packets_for_tests(0);
    int allocs[2000];
    for (int i = 0; i < 2000; i++) {
        allocs[i] = i + 1;
    }
    add_packets(allocs, 2000);

    /* Plafond à 1000 : haut=900, bas=750 -- déclenche EVICTING puis, une fois
     * sous 750, la sortie vers IDLE. */
    datamanager_set_ram_limit_packets_for_tests(1000);
    int rounds = 0;
    while (file_size(0) > 750 && rounds < 60) {
        stock_spill_step(100);
        rounds++;
    }
    ASSERT(stock_spill_total_packets() > 0ULL);

    /* Vide le résident restant PUIS relève largement le plafond (bien
     * au-dessus du total déporté) avant de recharger : sous le plafond
     * précédent (1000), le rechargement viserait seulement le seuil BAS
     * (75 % = 750) et s'arrêterait là par conception (cf. la doc de
     * stock_spill_step) sans jamais atteindre IDLE -- même repli que
     * reload_restores_evicted_data_when_ram_drops_and_preserves_fields. */
    array_possibility_packet *drained = get_last_possibility(NULL, 1000, NULL);
    free_array_possibility_packet(drained);
    ASSERT_EQ_FMT(0ULL, file_size(0), "%llu");
    datamanager_set_ram_limit_packets_for_tests(1000000);
    rounds = 0;
    while (stock_spill_total_packets() > 0ULL && rounds < 60) {
        stock_spill_step(100);
        rounds++;
    }
    ASSERT_EQ_FMT(0ULL, stock_spill_total_packets(), "%llu");
    // La bascule RELOADING -> IDLE (et son log_event "termine") est détectée
    // en TÊTE de stock_spill_step, sur le prochain appel APRÈS que le
    // débordement soit tombé à 0 -- un appel de plus est donc nécessaire ici
    // pour l'observer (même raison que la bascule EVICTING -> IDLE ci-dessus,
    // déclenchée par le premier stock_spill_step de CETTE boucle).
    stock_spill_step(100);

    FILE *f = fopen("events.log", "r");
    ASSERT(f != NULL);
    char buf[8192] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    (void)n;
    ASSERT(strstr(buf, "stock_spill : eviction disque demarree") != NULL);
    ASSERT(strstr(buf, "stock_spill : eviction disque terminee") != NULL);
    ASSERT(strstr(buf, "stock_spill : rechargement disque demarre") != NULL);
    ASSERT(strstr(buf, "stock_spill : rechargement disque termine") != NULL);

    unlink("events.log");
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

/* ---------------------------------------------------------------------- */
/* PR3 : cohérence sauvegarde/restauration (stock_spill_snapshot /
 * stock_spill_restore_snapshot). Helpers additionnels. */

static long g_ss_packet_size_cached = 0;
static long ss_packet_size(void)
{
    if (g_ss_packet_size_cached == 0) {
        g_ss_packet_size_cached = (long)sizeof(struct possibility_packet);
    }
    return g_ss_packet_size_cached;
}

static int same_inode(const char *a, const char *b)
{
    struct stat sa, sb;
    if (stat(a, &sa) != 0 || stat(b, &sb) != 0) {
        return 0;
    }
    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

/* Écrit `n` paquets bruts (alloc croissant à partir de `first_alloc`, marqueur
 * distinctif dans grid[0][0]) directement dans `path` — contourne toute la
 * mécanique d'éviction pour construire un cliché de test à la main (même
 * esprit que configure_purges_matching_segments_and_spares_others, qui
 * écrit déjà un segment brut directement). */
static void write_raw_segment(const char *path, int first_marker, int n)
{
    struct possibility_packet *buf = calloc((size_t)n, sizeof(struct possibility_packet));
    for (int i = 0; i < n; i++) {
        init_empty_grid(&buf[i]);
        buf[i].alloc = (uint16_t)(first_marker + i);
        buf[i].grid[0][0] = (int16_t)(first_marker + i);
    }
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "write_raw_segment: fopen(%s) a échoué\n", path);
        free(buf);
        return;
    }
    fwrite(buf, sizeof(struct possibility_packet), (size_t)n, f);
    fclose(f);
    free(buf);
}

static void write_manifest_line(FILE *f, char pool, int file_index, int last_seq,
                                 unsigned long long packets, long tail_bytes)
{
    fprintf(f, "%c %d %d %llu %ld\n", pool, file_index, last_seq, packets, tail_bytes);
}

/* Récupère tous les paquets résidents (jusqu'à `max`) et renvoie le nombre
 * lu, en remplissant `markers_out[i]` avec chaque marqueur `grid[0][0]`
 * rencontré — que `add_packets`/`write_raw_segment` posent tous les deux —
 * pour vérifier qu'un ensemble EXACT de possibilités (ni perte, ni
 * duplication, ni contamination croisée) est revenu en RAM.
 *
 * VERSION 13 (docs/autosearch_step.md) :
 * délibérément PAS `.alloc` ici — un paquet qui a fait un aller-retour par
 * un segment de débordement traverse `stock_spill_reload()`, qui recompte
 * `alloc` au sens nombre de cases pleines (idempotent, mais `add_packets`/
 * `write_raw_segment` ne posent qu'UNE case pleine par construction : tout
 * paquet rechargé revient donc avec `alloc == 1`, quel que soit son
 * marqueur d'origine). `grid[0][0]`, lui, n'est jamais touché par ce
 * recomptage et reste l'identifiant fiable après rechargement. */
static int drain_and_collect_markers(int *markers_out, int max)
{
    array_possibility_packet *r = get_last_possibility(NULL, max, NULL);
    int n = r->size;
    for (int i = 0; i < n && i < max; i++) {
        markers_out[i] = r->possibilities[i].grid[0][0];
    }
    free_array_possibility_packet(r);
    return n;
}

static int int_cmp(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

/* stock_spill_snapshot : les segments PLEINS sont dupliqués par lien (même
 * inode que le vivant), le segment de QUEUE (partiel) est toujours une copie
 * fraîche (inode distinct) — sinon une éviction ultérieure muterait aussi le
 * cliché déjà publié. Le manifeste liste exactement (last_seq, packets). */
TEST snapshot_links_full_segments_and_copies_tail(void)
{
    char tmpl[64];
    char *dir = make_tmp_spill_dir(tmpl);
    ASSERT(dir != NULL);
    stock_spill_configure(dir, nb_file_possibility);
    stock_spill_set_segment_bytes_for_tests(5 * ss_packet_size()); /* 5 possibilités/segment */

    drain_datamanager();
    datamanager_set_ram_limit_packets_for_tests(0);
    int allocs[12];
    for (int i = 0; i < 12; i++) { allocs[i] = i + 1; }
    add_packets(allocs, 12);

    datamanager_set_ram_limit_packets_for_tests(1);
    int rounds = 0;
    while (stock_spill_total_packets() < 12ULL && rounds < 30) {
        stock_spill_step(4096);
        rounds++;
    }
    ASSERT_EQ_FMT(12ULL, stock_spill_total_packets(), "%llu");
    ASSERT_EQ_FMT(3ULL, stock_spill_total_segments(), "%llu"); /* 5 + 5 + 2 */

    stock_spill_snapshot("snap");

    char live1[PATH_MAX], live2[PATH_MAX], live3[PATH_MAX];
    char snap1[PATH_MAX], snap2[PATH_MAX], snap3[PATH_MAX];
    snprintf(live1, sizeof live1, "%s/spill_u_0_1.dat", dir);
    snprintf(live2, sizeof live2, "%s/spill_u_0_2.dat", dir);
    snprintf(live3, sizeof live3, "%s/spill_u_0_3.dat", dir);
    snprintf(snap1, sizeof snap1, "%s/snap/spill_u_0_1.dat", dir);
    snprintf(snap2, sizeof snap2, "%s/snap/spill_u_0_2.dat", dir);
    snprintf(snap3, sizeof snap3, "%s/snap/spill_u_0_3.dat", dir);

    ASSERT(same_inode(live1, snap1)); /* plein -> lien */
    ASSERT(same_inode(live2, snap2)); /* plein -> lien */
    ASSERT(!same_inode(live3, snap3)); /* queue partielle -> copie */
    struct stat st3;
    ASSERT_EQ_FMT(0, stat(snap3, &st3), "%d");
    ASSERT_EQ_FMT((long)(2 * ss_packet_size()), (long)st3.st_size, "%ld"); /* 2 possibilités restantes */

    char manifest_path[PATH_MAX];
    snprintf(manifest_path, sizeof manifest_path, "%s/snap/manifest.txt", dir);
    FILE *mf = fopen(manifest_path, "r");
    ASSERT(mf != NULL);
    char line[256];
    ASSERT(fgets(line, sizeof line, mf) != NULL);
    line[strcspn(line, "\r\n")] = '\0';
    ASSERT_STR_EQ("eternityii-spill-manifest-v1", line);
    int found = 0;
    while (fgets(line, sizeof line, mf) != NULL) {
        char pc; int fidx, last_seq; unsigned long long packets; long tail_bytes;
        if (sscanf(line, "%c %d %d %llu %ld", &pc, &fidx, &last_seq, &packets, &tail_bytes) == 5) {
            ASSERT_EQ_FMT('u', pc, "%c");
            ASSERT_EQ_FMT(0, fidx, "%d");
            ASSERT_EQ_FMT(3, last_seq, "%d");
            ASSERT_EQ_FMT(12ULL, packets, "%llu");
            found = 1;
        }
    }
    fclose(mf);
    ASSERT(found);

    datamanager_set_ram_limit_packets_for_tests(0);
    drain_datamanager();
    rmdir_recursive(dir); /* purge best-effort du sous-répertoire "snap" inclus par readdir successif */
    PASS();
}

/* Cas piège documenté dans stock_spill.h : un segment rechargé PUIS réévincé
 * peut réutiliser le MÊME numéro de séquence avec un contenu DIFFÉRENT — la
 * comparaison par inode (pas seulement par nom) doit détecter ce
 * renumérotage et rafraîchir le cliché, et purger toute entrée devenue
 * obsolète (au-delà du nouveau last_seq). */
TEST snapshot_refreshes_stale_reused_segment_number(void)
{
    char tmpl[64];
    char *dir = make_tmp_spill_dir(tmpl);
    ASSERT(dir != NULL);
    stock_spill_configure(dir, nb_file_possibility);
    stock_spill_set_segment_bytes_for_tests(5 * ss_packet_size());

    drain_datamanager();
    datamanager_set_ram_limit_packets_for_tests(0);
    int allocs_old[12];
    for (int i = 0; i < 12; i++) { allocs_old[i] = 1000 + i; } /* marqueurs "ancien" jeu */
    add_packets(allocs_old, 12);
    datamanager_set_ram_limit_packets_for_tests(1);
    int rounds = 0;
    while (stock_spill_total_packets() < 12ULL && rounds < 30) { stock_spill_step(4096); rounds++; }
    ASSERT_EQ_FMT(12ULL, stock_spill_total_packets(), "%llu");
    ASSERT_EQ_FMT(3ULL, stock_spill_total_segments(), "%llu");

    stock_spill_snapshot("snap"); /* premier cliché : 3 segments, marqueurs 1000..1011 */

    /* Tout recharger en RAM : les 3 segments vivants disparaissent (le
     * cliché, lui, les garde vivants via ses liens). */
    datamanager_set_ram_limit_packets_for_tests(1000);
    rounds = 0;
    while (stock_spill_total_packets() > 0ULL && rounds < 30) { stock_spill_step(4096); rounds++; }
    ASSERT_EQ_FMT(0ULL, stock_spill_total_packets(), "%llu");
    ASSERT_EQ_FMT(0ULL, stock_spill_total_segments(), "%llu");

    array_possibility_packet *drained = get_last_possibility(NULL, 1000, NULL);
    free_array_possibility_packet(drained);
    datamanager_set_ram_limit_packets_for_tests(0);

    /* Réévince un NOUVEAU jeu, plus petit (8 possibilités -> segments 5+3),
     * réutilisant les numéros de séquence 1 et 2, avec un contenu DIFFÉRENT. */
    int allocs_new[8];
    for (int i = 0; i < 8; i++) { allocs_new[i] = i + 1; } /* marqueurs "nouveau" jeu : 1..8 */
    add_packets(allocs_new, 8);
    datamanager_set_ram_limit_packets_for_tests(1);
    rounds = 0;
    while (stock_spill_total_packets() < 8ULL && rounds < 30) { stock_spill_step(4096); rounds++; }
    ASSERT_EQ_FMT(8ULL, stock_spill_total_packets(), "%llu");
    ASSERT_EQ_FMT(2ULL, stock_spill_total_segments(), "%llu"); /* 5 + 3, plus de seq 3 */

    stock_spill_snapshot("snap"); /* rafraîchissement : doit détecter le renumérotage */

    char snap3[PATH_MAX];
    snprintf(snap3, sizeof snap3, "%s/snap/spill_u_0_3.dat", dir);
    struct stat st3;
    ASSERT(stat(snap3, &st3) != 0); /* seq 3 n'existe plus vivant -> purgé du cliché */

    /* Round-trip via restore_snapshot : reconfigure (efface tout état vivant
     * ET le désactive), remet le module en service, restaure -- seul le
     * NOUVEAU jeu (marqueurs 1..8) doit revenir, jamais l'ancien (1000..1011). */
    array_possibility_packet *cur = get_last_possibility(NULL, 1000, NULL);
    free_array_possibility_packet(cur); /* vide le résident restant avant reconfigure */
    stock_spill_configure(dir, nb_file_possibility);
    stock_spill_set_segment_bytes_for_tests(5 * ss_packet_size()); /* configure() a réinitialisé la surcharge */
    stock_spill_restore_snapshot("snap");
    ASSERT_EQ_FMT(8ULL, stock_spill_total_packets(), "%llu");

    datamanager_set_ram_limit_packets_for_tests(1000);
    rounds = 0;
    while (stock_spill_total_packets() > 0ULL && rounds < 30) { stock_spill_step(4096); rounds++; }
    int markers[16];
    int n = drain_and_collect_markers(markers, 16);
    ASSERT_EQ_FMT(8, n, "%d");
    qsort(markers, (size_t)n, sizeof(int), int_cmp);
    for (int i = 0; i < n; i++) {
        ASSERT_EQ_FMT(i + 1, markers[i], "%d"); /* exactement 1..8, jamais 1000+ */
    }

    datamanager_set_ram_limit_packets_for_tests(0);
    drain_datamanager();
    rmdir_recursive(dir);
    PASS();
}

/* Restauration SANS collision de re-séquencement (--stock-files inchangé) :
 * aller-retour cliché -> reconfigure (simule un redémarrage) -> restauration
 * -> rechargement intégral, ensemble de marqueurs préservé EXACTEMENT. */
TEST restore_snapshot_no_collision_round_trip_preserves_data(void)
{
    char tmpl[64];
    char *dir = make_tmp_spill_dir(tmpl);
    ASSERT(dir != NULL);
    stock_spill_configure(dir, nb_file_possibility);
    stock_spill_set_segment_bytes_for_tests(4 * ss_packet_size());

    drain_datamanager();
    datamanager_set_ram_limit_packets_for_tests(0);
    int allocs[10];
    for (int i = 0; i < 10; i++) { allocs[i] = i + 1; }
    add_packets(allocs, 10);
    datamanager_set_ram_limit_packets_for_tests(1);
    int rounds = 0;
    while (stock_spill_total_packets() < 10ULL && rounds < 30) { stock_spill_step(4096); rounds++; }
    ASSERT_EQ_FMT(10ULL, stock_spill_total_packets(), "%llu");

    stock_spill_snapshot("snap");

    array_possibility_packet *cur = get_last_possibility(NULL, 1000, NULL);
    free_array_possibility_packet(cur);
    datamanager_set_ram_limit_packets_for_tests(0);

    /* "Redémarrage" : même nombre de files -> aucune collision de
     * re-séquencement, chemin pur lien. */
    stock_spill_configure(dir, nb_file_possibility);
    stock_spill_set_segment_bytes_for_tests(4 * ss_packet_size()); /* configure() a réinitialisé la surcharge */
    stock_spill_restore_snapshot("snap");
    ASSERT_EQ_FMT(10ULL, stock_spill_total_packets(), "%llu");

    datamanager_set_ram_limit_packets_for_tests(1000);
    rounds = 0;
    while (stock_spill_total_packets() > 0ULL && rounds < 30) { stock_spill_step(4096); rounds++; }
    ASSERT_EQ_FMT(0ULL, stock_spill_total_segments(), "%llu");
    int markers[16];
    int n = drain_and_collect_markers(markers, 16);
    ASSERT_EQ_FMT(10, n, "%d");
    qsort(markers, (size_t)n, sizeof(int), int_cmp);
    for (int i = 0; i < n; i++) {
        ASSERT_EQ_FMT(i + 1, markers[i], "%d");
    }

    datamanager_set_ram_limit_packets_for_tests(0);
    drain_datamanager();
    rmdir_recursive(dir);
    PASS();
}

/* Restauration AVEC collision de re-séquencement (--stock-files réduit
 * depuis la sauvegarde) : deux anciennes files convergent vers la même file
 * vivante -> réempaquetage via stock_spill_write_block. Cliché construit à
 * la main (contourne l'éviction réelle) pour contrôler exactement quels
 * old_file_index entrent en collision. Vérifie à la fois la conservation
 * des données ET l'invariant « seul le sommet peut être partiel » sur les
 * segments réempaquetés (jamais un segment partiel d'une source enterré au
 * milieu de la pile fusionnée). */
TEST restore_snapshot_collision_repacks_when_stock_files_shrinks(void)
{
    char tmpl[64];
    char *dir = make_tmp_spill_dir(tmpl);
    ASSERT(dir != NULL);

    /* Cliché "sauvegardé" avec 6 files (0..5) : construit à la main.
     * `dir` existe déjà (créé par mkdtemp dans make_tmp_spill_dir). */
    char snap_dir[PATH_MAX];
    snprintf(snap_dir, sizeof snap_dir, "%s/snap", dir);
    ASSERT_EQ_FMT(0, mkdir(snap_dir, 0755), "%d");

    char seg_a[PATH_MAX], seg_b[PATH_MAX];
    snprintf(seg_a, sizeof seg_a, "%s/spill_u_0_1.dat", snap_dir);
    snprintf(seg_b, sizeof seg_b, "%s/spill_u_3_1.dat", snap_dir);
    write_raw_segment(seg_a, 100, 3); /* old_file_index=0 : marqueurs 100,101,102 */
    write_raw_segment(seg_b, 200, 2); /* old_file_index=3 : marqueurs 200,201 */

    char manifest_path[PATH_MAX];
    snprintf(manifest_path, sizeof manifest_path, "%s/manifest.txt", snap_dir);
    FILE *mf = fopen(manifest_path, "w");
    ASSERT(mf != NULL);
    fprintf(mf, "eternityii-spill-manifest-v1\n");
    write_manifest_line(mf, 'u', 0, 1, 3, 3 * ss_packet_size());
    write_manifest_line(mf, 'u', 3, 1, 2, 2 * ss_packet_size());
    fclose(mf);

    /* nb_files=3 : old {0,3} convergent tous deux vers la file vivante 0
     * (0%3=0, 3%3=0) -- collision garantie. */
    drain_datamanager();
    datamanager_set_ram_limit_packets_for_tests(0);
    stock_spill_configure(dir, 3);
    stock_spill_set_segment_bytes_for_tests(2 * ss_packet_size()); /* force le réempaquetage à travers plusieurs segments */

    stock_spill_restore_snapshot("snap");
    ASSERT_EQ_FMT(5ULL, stock_spill_total_packets(), "%llu");

    /* Invariant "seul le sommet est partiel" sur le résultat réempaqueté :
     * 5 possibilités / 2 par segment -> segments pleins (2,2) puis un
     * sommet partiel (1) -- jamais un segment du MILIEU plus petit qu'un
     * segment plein. */
    ASSERT_EQ_FMT(3ULL, stock_spill_total_segments(), "%llu");
    char rebuilt1[PATH_MAX], rebuilt2[PATH_MAX], rebuilt3[PATH_MAX];
    snprintf(rebuilt1, sizeof rebuilt1, "%s/spill_u_0_1.dat", dir);
    snprintf(rebuilt2, sizeof rebuilt2, "%s/spill_u_0_2.dat", dir);
    snprintf(rebuilt3, sizeof rebuilt3, "%s/spill_u_0_3.dat", dir);
    struct stat st1, st2, st3;
    ASSERT_EQ_FMT(0, stat(rebuilt1, &st1), "%d");
    ASSERT_EQ_FMT(0, stat(rebuilt2, &st2), "%d");
    ASSERT_EQ_FMT(0, stat(rebuilt3, &st3), "%d");
    ASSERT_EQ_FMT((long)(2 * ss_packet_size()), (long)st1.st_size, "%ld"); /* plein */
    ASSERT_EQ_FMT((long)(2 * ss_packet_size()), (long)st2.st_size, "%ld"); /* plein */
    ASSERT_EQ_FMT((long)(1 * ss_packet_size()), (long)st3.st_size, "%ld"); /* sommet, partiel */

    /* Conservation exacte des données : les 5 marqueurs des DEUX sources
     * reviennent, sans perte ni duplication. */
    datamanager_set_ram_limit_packets_for_tests(1000);
    int rounds = 0;
    while (stock_spill_total_packets() > 0ULL && rounds < 30) { stock_spill_step(4096); rounds++; }
    int markers[16];
    int n = drain_and_collect_markers(markers, 16);
    ASSERT_EQ_FMT(5, n, "%d");
    qsort(markers, (size_t)n, sizeof(int), int_cmp);
    int expected[5] = { 100, 101, 102, 200, 201 };
    for (int i = 0; i < 5; i++) {
        ASSERT_EQ_FMT(expected[i], markers[i], "%d");
    }

    datamanager_set_ram_limit_packets_for_tests(0);
    drain_datamanager();
    rmdir_recursive(dir);
    PASS();
}

/* Correctif : le manifeste peut lister un segment que le disque n'a plus
 * (fichier .dat supprimé/corrompu, manifest.txt lui-même intact) -- avant ce
 * correctif, le groupe entier était compté dans total_linked et le
 * descripteur posé tel quel malgré l'absence réelle des données sur disque,
 * si bien qu'un restore semblait réussir alors qu'il importait un stock ne
 * correspondant plus à la sauvegarde. Chemin SANS collision (une seule
 * source par file vivante, le cas courant). */
TEST restore_snapshot_no_collision_missing_segment_reports_partial(void)
{
    char tmpl[64];
    char *dir = make_tmp_spill_dir(tmpl);
    ASSERT(dir != NULL);

    char snap_dir[PATH_MAX];
    snprintf(snap_dir, sizeof snap_dir, "%s/snap", dir);
    ASSERT_EQ_FMT(0, mkdir(snap_dir, 0755), "%d");

    /* Manifeste annonce 2 segments (1 plein + 1 partiel, 5 possibilités au
     * total) pour (pool=u, ancienne file=0), mais SEUL le premier segment
     * est réellement écrit sur disque -- le second (le sommet) manque, comme
     * s'il avait été supprimé/corrompu après la sauvegarde. */
    char seg1[PATH_MAX];
    snprintf(seg1, sizeof seg1, "%s/spill_u_0_1.dat", snap_dir);
    write_raw_segment(seg1, 100, 3); /* segment 1, plein : marqueurs 100,101,102 */
    /* spill_u_0_2.dat (le sommet, 2 possibilités) volontairement absent. */

    char manifest_path[PATH_MAX];
    snprintf(manifest_path, sizeof manifest_path, "%s/manifest.txt", snap_dir);
    FILE *mf = fopen(manifest_path, "w");
    ASSERT(mf != NULL);
    fprintf(mf, "eternityii-spill-manifest-v1\n");
    write_manifest_line(mf, 'u', 0, 2, 5, 2 * ss_packet_size());
    fclose(mf);

    drain_datamanager();
    datamanager_set_ram_limit_packets_for_tests(0);
    stock_spill_configure(dir, nb_file_possibility); /* même nb_files -> pas de collision (old 0 -> new 0) */
    stock_spill_set_segment_bytes_for_tests(3 * ss_packet_size());

    unsigned long long restored = stock_spill_restore_snapshot("snap");

    /* Le groupe entier est invalidé -- jamais restauré à moitié en silence :
     * ni compté dans le total renvoyé, ni reflété dans le descripteur vivant
     * (stock_spill_total_packets), qui doit rester à 0 pour cette file
     * plutôt que de prétendre que les 5 possibilités sont là. */
    ASSERT_EQ_FMT(0ULL, restored, "%llu");
    ASSERT_EQ_FMT(0ULL, stock_spill_total_packets(), "%llu");
    ASSERT_EQ_FMT(0ULL, stock_spill_total_segments(), "%llu");

    /* Le segment 1, pourtant placé AVANT l'échec (constaté seulement au
     * segment 2), est nettoyé plutôt que laissé orphelin sur le disque
     * vivant, invisible du descripteur. */
    char live_seg1[PATH_MAX];
    snprintf(live_seg1, sizeof live_seg1, "%s/spill_u_0_1.dat", dir);
    struct stat st;
    ASSERT(stat(live_seg1, &st) != 0);

    drain_datamanager();
    rmdir_recursive(dir);
    PASS();
}

/* Même correctif, chemin AVEC collision (--stock-files réduit) : sur deux
 * sources fusionnées dans la même file vivante, une seule a un segment
 * manquant -- seule CETTE source doit être amputée du total, l'autre
 * (intacte) doit revenir intégralement. */
TEST restore_snapshot_collision_missing_segment_reports_partial(void)
{
    char tmpl[64];
    char *dir = make_tmp_spill_dir(tmpl);
    ASSERT(dir != NULL);

    char snap_dir[PATH_MAX];
    snprintf(snap_dir, sizeof snap_dir, "%s/snap", dir);
    ASSERT_EQ_FMT(0, mkdir(snap_dir, 0755), "%d");

    /* old_file_index=0 : intact, 1 segment, 2 possibilités. */
    char seg_a[PATH_MAX];
    snprintf(seg_a, sizeof seg_a, "%s/spill_u_0_1.dat", snap_dir);
    write_raw_segment(seg_a, 100, 2); /* marqueurs 100,101 */
    /* old_file_index=3 : manifeste annonce 2 possibilités, mais le fichier
     * .dat correspondant est absent (supprimé/corrompu). */

    char manifest_path[PATH_MAX];
    snprintf(manifest_path, sizeof manifest_path, "%s/manifest.txt", snap_dir);
    FILE *mf = fopen(manifest_path, "w");
    ASSERT(mf != NULL);
    fprintf(mf, "eternityii-spill-manifest-v1\n");
    write_manifest_line(mf, 'u', 0, 1, 2, 2 * ss_packet_size());
    write_manifest_line(mf, 'u', 3, 1, 2, 2 * ss_packet_size());
    fclose(mf);

    /* nb_files=3 : old {0,3} convergent tous deux vers la file vivante 0
     * (0%3=0, 3%3=0) -- collision garantie, comme
     * restore_snapshot_collision_repacks_when_stock_files_shrinks. */
    drain_datamanager();
    datamanager_set_ram_limit_packets_for_tests(0);
    stock_spill_configure(dir, 3);
    stock_spill_set_segment_bytes_for_tests(2 * ss_packet_size());

    unsigned long long restored = stock_spill_restore_snapshot("snap");

    /* Seules les 2 possibilités de la source intacte (old_file_index=0)
     * reviennent -- jamais les 4 promises par le manifeste. */
    ASSERT_EQ_FMT(2ULL, restored, "%llu");
    ASSERT_EQ_FMT(2ULL, stock_spill_total_packets(), "%llu");

    datamanager_set_ram_limit_packets_for_tests(1000);
    int rounds = 0;
    while (stock_spill_total_packets() > 0ULL && rounds < 30) { stock_spill_step(4096); rounds++; }
    int markers[8];
    int n = drain_and_collect_markers(markers, 8);
    ASSERT_EQ_FMT(2, n, "%d");
    qsort(markers, (size_t)n, sizeof(int), int_cmp);
    ASSERT_EQ_FMT(100, markers[0], "%d");
    ASSERT_EQ_FMT(101, markers[1], "%d");

    datamanager_set_ram_limit_packets_for_tests(0);
    drain_datamanager();
    rmdir_recursive(dir);
    PASS();
}

/* Manifeste absent (aucun cliché sauvegardé pour ce répertoire, ou backup
 * antérieur à PR3) : tolérant, aucun crash, aucune action -- pas une
 * erreur. */
TEST restore_snapshot_tolerates_missing_manifest(void)
{
    char tmpl[64];
    char *dir = make_tmp_spill_dir(tmpl);
    ASSERT(dir != NULL);
    stock_spill_configure(dir, nb_file_possibility);

    stock_spill_restore_snapshot("snap-inexistant");
    ASSERT_EQ_FMT(0ULL, stock_spill_total_packets(), "%llu");
    ASSERT_EQ_FMT(0ULL, stock_spill_total_segments(), "%llu");

    rmdir_recursive(dir);
    PASS();
}

/* Le débordement VIVANT courant (non sauvegardé) est intégralement remplacé
 * par le cliché restauré -- jamais fusionné avec lui, même comportement que
 * le drainage RAM que `restore()` fait déjà pour les deux pools résidents. */
TEST restore_snapshot_replaces_current_live_segments(void)
{
    char tmpl[64];
    char *dir = make_tmp_spill_dir(tmpl);
    ASSERT(dir != NULL);
    stock_spill_configure(dir, nb_file_possibility);
    stock_spill_set_segment_bytes_for_tests(3 * ss_packet_size());

    /* État vivant courant, non sauvegardé (jamais snapshotté). */
    drain_datamanager();
    datamanager_set_ram_limit_packets_for_tests(0);
    int allocs_live[6];
    for (int i = 0; i < 6; i++) { allocs_live[i] = 500 + i; }
    add_packets(allocs_live, 6);
    datamanager_set_ram_limit_packets_for_tests(1);
    int rounds = 0;
    while (stock_spill_total_packets() < 6ULL && rounds < 30) { stock_spill_step(4096); rounds++; }
    ASSERT_EQ_FMT(6ULL, stock_spill_total_packets(), "%llu");

    /* Cliché construit à la main, complètement disjoint. */
    char snap_dir[PATH_MAX];
    snprintf(snap_dir, sizeof snap_dir, "%s/snap", dir);
    ASSERT_EQ_FMT(0, mkdir(snap_dir, 0755), "%d");
    char seg[PATH_MAX];
    snprintf(seg, sizeof seg, "%s/spill_u_0_1.dat", snap_dir);
    write_raw_segment(seg, 900, 2);
    char manifest_path[PATH_MAX];
    snprintf(manifest_path, sizeof manifest_path, "%s/manifest.txt", snap_dir);
    FILE *mf = fopen(manifest_path, "w");
    ASSERT(mf != NULL);
    fprintf(mf, "eternityii-spill-manifest-v1\n");
    write_manifest_line(mf, 'u', 0, 1, 2, 2 * ss_packet_size());
    fclose(mf);

    stock_spill_restore_snapshot("snap");
    ASSERT_EQ_FMT(2ULL, stock_spill_total_packets(), "%llu"); /* pas 6+2 */

    datamanager_set_ram_limit_packets_for_tests(1000);
    rounds = 0;
    while (stock_spill_total_packets() > 0ULL && rounds < 30) { stock_spill_step(4096); rounds++; }
    int markers[8];
    int n = drain_and_collect_markers(markers, 8);
    ASSERT_EQ_FMT(2, n, "%d");
    qsort(markers, (size_t)n, sizeof(int), int_cmp);
    ASSERT_EQ_FMT(900, markers[0], "%d");
    ASSERT_EQ_FMT(901, markers[1], "%d");

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
    RUN_TEST(step_logs_eviction_and_reload_transitions_to_events_log);
    RUN_TEST(evict_and_reload_span_multiple_segments);
    RUN_TEST(snapshot_links_full_segments_and_copies_tail);
    RUN_TEST(snapshot_refreshes_stale_reused_segment_number);
    RUN_TEST(restore_snapshot_no_collision_round_trip_preserves_data);
    RUN_TEST(restore_snapshot_collision_repacks_when_stock_files_shrinks);
    RUN_TEST(restore_snapshot_no_collision_missing_segment_reports_partial);
    RUN_TEST(restore_snapshot_collision_missing_segment_reports_partial);
    RUN_TEST(restore_snapshot_tolerates_missing_manifest);
    RUN_TEST(restore_snapshot_replaces_current_live_segments);
}

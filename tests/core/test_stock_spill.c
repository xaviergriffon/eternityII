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
#include "expand_fixture.h"
#include "app/app_static_variables.h"
#include "fork_assert.h"
#include "core/datamanager.h"
#include "core/possibility.h"
#include "core/stock_spill.h"
#include "core/packet_codec.h"
#include "core/stock_tier.h"

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
void datamanager_set_ram_limit_bytes_for_tests(unsigned long long bytes);

/* Plafond calé sur l'occupation RÉELLE des possibilités DÉJÀ résidentes,
 * ramenée à `keep` d'entre elles. Exprimer un plafond en « nombre » suppose une
 * taille par possibilité constante — faux depuis que le stock range des
 * enregistrements compacts, dont la taille dépend du remplissage du plateau.
 * Les fixtures d'ici posent des plateaux QUASI VIDES (une seule case), donc des
 * enregistrements minuscules : un plafond exprimé au tarif d'un plateau plein y
 * serait dix fois trop large. */
static void set_ram_limit_for_resident(unsigned long long keep, unsigned long long total)
{
    unsigned long long resident = datamanager_resident_bytes();
    datamanager_set_ram_limit_bytes_for_tests(total == 0 ? 0 : (resident * keep) / total);
}
void stock_spill_set_segment_records_for_tests(long records);

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
/* Base des marqueurs distinctifs posés dans `grid[0][0]`.
 *
 * Un marqueur DOIT rester un identifiant de pièce pivotée valide
 * ([1, 4 x ETERN_PARTS]) : les segments sont sérialisés champ par champ
 * (core/packet_codec.c), qui refuse toute autre valeur. L'ancienne base 1000
 * tenait dans le build 16x16 (4 x 256 = 1024) mais pas dans le build 4x4
 * (4 x 16 = 64) -- même piège que les identifiants de fixture bornés par
 * ETERN_PARTS ailleurs dans cette suite. 20 marqueurs consécutifs à partir
 * de 30 tiennent dans les deux. */
#define MARK_BASE 30

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
        /* Le marqueur est REPLIÉ dans le domaine des identifiants de pièce
         * pivotée ([1, 4 x ETERN_PARTS]) : les segments de débordement sont
         * sérialisés champ par champ (core/packet_codec.c) et refusent toute
         * autre valeur. Le repli est l'IDENTITÉ pour tout marqueur <=
         * 4 x ETERN_PARTS -- donc sans effet sur les tests qui comparent
         * `grid[0][0]` (marqueurs MARK_BASE..MARK_BASE+19) -- et ne joue que
         * pour les grosses fixtures (100, 2000 possibilités) dont seul
         * `alloc` est observé. */
        arr.possibilities[i].grid[0][0] = (int16_t)(((allocs[i] - 1) % (4 * ETERN_PARTS)) + 1);
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
    /* Population bornée par le nombre de MARQUEURS distincts que le puzzle
       permet : `add_packets` replie son marqueur dans [1, 4 x ETERN_PARTS], soit
       64 valeurs seulement en build 4x4. Au-delà, deux possibilités deviennent
       indiscernables et « l'éviction a pris les plus anciennes » ne se vérifie
       plus. Le sujet du test ne dépend pas de la valeur exacte de N. */
    enum { N = (2000 < 4 * ETERN_PARTS) ? 2000 : (4 * ETERN_PARTS) };
    enum { KEEP = N / 2, FLOOR = (N * 45) / 100 };
    int allocs[N];
    for (int i = 0; i < N; i++) {
        allocs[i] = i + 1; /* ordre d'ajout croissant : le marqueur 1 est le plus ancien */
    }
    add_packets(allocs, N);
    ASSERT_EQ_FMT((unsigned long long)N, file_size(0), "%llu");

    /* Plafond à la moitié de la population. Budget volontairement PETIT par
       rapport à l'excédent, pour observer une convergence incrémentale plutôt
       qu'une évacuation en un seul appel. */
    set_ram_limit_for_resident(KEEP, N);
    int rounds = 0;
    while (file_size(0) > (unsigned long long)FLOOR && rounds < 60) {
        stock_spill_step(N / 20 + 1);
        rounds++;
    }

    unsigned long long resident = file_size(0);
    unsigned long long spilled = stock_spill_total_packets();
    ASSERT_EQ_FMT((unsigned long long)N, resident + spilled, "%llu"); /* rien perdu */
    ASSERT(resident <= (unsigned long long)FLOOR);
    ASSERT(spilled > 0ULL);

    array_possibility_packet *r = get_last_possibility(NULL, (int)resident, NULL);
    ASSERT_EQ_FMT((int)resident, r->size, "%d");
    /* Identité par le MARQUEUR de grille, plus par `alloc`.
     *
     * `alloc` servait ici de numéro d'ordre (1..2000) : c'était licite quand il
     * n'était qu'un curseur, ça ne l'est plus depuis qu'il est déduit de la
     * grille — un plateau de 4x4 ne peut pas porter 2000 pièces. Le marqueur
     * `grid[0][0]`, lui, est posé exprès pour identifier chaque possibilité
     * (cf. `add_packets`), et il replie déjà l'ordre d'ajout dans le domaine
     * réalisable. */
    int min_mark = 100000;
    int max_mark = -100000;
    for (int i = 0; i < r->size; i++) {
        int m = r->possibilities[i].grid[0][0];
        if (m < min_mark) { min_mark = m; }
        if (m > max_mark) { max_mark = m; }
    }
    free_array_possibility_packet(r);

    /* Les survivants sont exactement les `resident` DERNIERS ajoutés : leurs
     * marqueurs forment une plage sans trou — preuve que l'éviction a pris la
     * tête (les plus anciens), jamais la queue. */
    ASSERT_EQ_FMT((int)resident, max_mark - min_mark + 1, "%d");

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
        arr.possibilities[i].grid[0][0] = (int16_t)(MARK_BASE + i); /* marqueur distinctif */
    }
    add_possibility(NULL, &arr);
    free(arr.possibilities);
    ASSERT_EQ_FMT(20ULL, file_size(0), "%llu");

    /* Plafond à 5 (haut=4, bas=3, rechargement=1). Budget PETIT (3) pour une
     * convergence incrémentale observable. */
    set_ram_limit_for_resident(5, 20);
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
        /* Le marqueur suffit à identifier la possibilité : le lier à `alloc`
           n'a plus de sens, ce champ étant désormais déduit de la grille (et
           donc identique pour toutes les fixtures de ce test). */
        int m = (int)drained->possibilities[i].grid[0][0];
        ASSERT(m >= MARK_BASE && m < MARK_BASE + 20);
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
        ASSERT(marker >= MARK_BASE && marker < MARK_BASE + 20);
        int idx = marker - MARK_BASE;
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

    /* Plafond à la MOITIÉ de l'occupation réellement mesurée (haut = 90 %,
     * bas = 75 % de ce plafond) : déclenche EVICTING puis, une fois sous le
     * seuil bas, la sortie vers IDLE. Un plafond exprimé en « nombre de
     * possibilités » ne mordrait plus — le stock range des enregistrements
     * compacts, bien plus petits qu'un plateau plein. */
    set_ram_limit_for_resident(1000, 2000);
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
    /* Plafond très large (mais non nul : à 0 le débordement est inerte), pour
     * que le rechargement vise IDLE et non le seul seuil bas. */
    datamanager_set_ram_limit_bytes_for_tests(1ULL << 30);
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

/* Une passe d'expansion draine tout le pool dans une file que
 * `datamanager_resident_bytes` ne compte pas : la RAM paraît vide, le
 * débordement rechargeait, et la passe renvoyait ces possibilités sur disque en
 * remplissant le pool — va-et-vient disque sans fin sur un gros stock. Pendant
 * une expansion : aucun rechargement, même déjà commencé ; l'état s'imbrique ;
 * le rechargement reprend dès la fin. */
TEST no_reload_while_an_expansion_is_running(void)
{
    char tmpl[64];
    char *dir = make_tmp_spill_dir(tmpl);
    ASSERT(dir != NULL);
    stock_spill_configure(dir, nb_file_possibility);

    drain_datamanager();
    datamanager_set_ram_limit_packets_for_tests(0);
    int allocs[200];
    for (int i = 0; i < 200; i++) {
        allocs[i] = i + 1;
    }
    add_packets(allocs, 200);
    set_ram_limit_for_resident(100, 200);
    int rounds = 0;
    while (file_size(0) > 75 && rounds < 60) {
        stock_spill_step(10);
        rounds++;
    }
    unsigned long long spilled = stock_spill_total_packets();
    ASSERT(spilled > 20ULL);

    /* RAM vidée (ce que voit le débordement après le drainage d'une passe). */
    array_possibility_packet *drained = get_last_possibility(NULL, 1000, NULL);
    free_array_possibility_packet(drained);
    ASSERT_EQ_FMT(0ULL, file_size(0), "%llu");

    /* Rechargement DÉJÀ en cours quand l'expansion commence. */
    stock_spill_step(10);
    ASSERT_EQ_FMT(spilled - 10ULL, stock_spill_total_packets(), "%llu");
    spilled = stock_spill_total_packets();

    datamanager_begin_expansion();
    datamanager_begin_expansion();
    ASSERT(datamanager_is_expansion_active());
    for (int i = 0; i < 10; i++) {
        ASSERT_EQ_FMT(0, stock_spill_step(10), "%d");
    }
    ASSERT_EQ_FMT(spilled, stock_spill_total_packets(), "%llu");

    /* Imbrication : la première fin ne lève pas l'état de l'autre. */
    datamanager_end_expansion();
    ASSERT(datamanager_is_expansion_active());
    ASSERT_EQ_FMT(0, stock_spill_step(10), "%d");
    ASSERT_EQ_FMT(spilled, stock_spill_total_packets(), "%llu");

    /* Fin de l'expansion : le rechargement reprend au tick suivant. Un
     * décrément de trop reste saturé à 0. */
    datamanager_end_expansion();
    datamanager_end_expansion();
    ASSERT_FALSE(datamanager_is_expansion_active());
    ASSERT_EQ_FMT(10, stock_spill_step(10), "%d");
    ASSERT_EQ_FMT(spilled - 10ULL, stock_spill_total_packets(), "%llu");

    datamanager_set_ram_limit_packets_for_tests(0);
    drain_datamanager();
    rmdir_recursive(dir);
    PASS();
}

/* L'éviction, elle, reste active pendant une expansion : c'est elle qui fait
 * la place que la passe réclame. */
TEST eviction_still_runs_during_an_expansion(void)
{
    char tmpl[64];
    char *dir = make_tmp_spill_dir(tmpl);
    ASSERT(dir != NULL);
    stock_spill_configure(dir, nb_file_possibility);

    drain_datamanager();
    datamanager_set_ram_limit_packets_for_tests(0);
    int allocs[100];
    for (int i = 0; i < 100; i++) {
        allocs[i] = i + 1;
    }
    add_packets(allocs, 100);
    set_ram_limit_for_resident(50, 100);

    datamanager_begin_expansion();
    ASSERT_EQ_FMT(10, stock_spill_step(10), "%d");
    datamanager_end_expansion();
    ASSERT_EQ_FMT(10ULL, stock_spill_total_packets(), "%llu");

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
    stock_spill_set_segment_records_for_tests(5); /* 5 possibilités/segment */

    drain_datamanager();
    datamanager_set_ram_limit_packets_for_tests(0); /* efface tout plafond résiduel avant l'ajout */
    int allocs[50];
    for (int i = 0; i < 50; i++) {
        allocs[i] = i + 1;
    }
    add_packets(allocs, 50);
    ASSERT_EQ_FMT(50ULL, file_size(0), "%llu");

    set_ram_limit_for_resident(10, 12);
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

static int same_inode(const char *a, const char *b)
{
    struct stat sa, sb;
    if (stat(a, &sa) != 0 || stat(b, &sb) != 0) {
        return 0;
    }
    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

/* Segment au format v2 HÉRITÉ (forme compacte à pas fixe
 * PACKET_CODEC_MAX_BYTES), tel qu'un cliché antérieur aux trames en contient :
 * `n` paquets, marqueur distinctif dans grid[0][0]. Contourne toute la
 * mécanique d'éviction pour construire un cliché de test à la main. */
static void write_stride_segment(const char *path, int first_marker, int n)
{
    struct possibility_packet *buf = calloc((size_t)n, sizeof(struct possibility_packet));
    for (int i = 0; i < n; i++) {
        init_empty_grid(&buf[i]);
        buf[i].alloc = 1;
        buf[i].grid[0][0] = (int16_t)(first_marker + i);
    }
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "write_stride_segment: fopen(%s) a échoué\n", path);
        free(buf);
        return;
    }
    /* Forme compacte, chaque enregistrement complété de zéros jusqu'au pas. */
    uint8_t *raw = calloc((size_t)n, (size_t)PACKET_CODEC_MAX_BYTES);
    for (int i = 0; i < n; i++) {
        if (packet_codec_encode(&buf[i], raw + (size_t)i * PACKET_CODEC_MAX_BYTES,
                                PACKET_CODEC_MAX_BYTES, NULL) != 0) {
            fprintf(stderr, "write_stride_segment: paquet %d non encodable\n", i);
        }
    }
    fwrite(raw, (size_t)PACKET_CODEC_MAX_BYTES, (size_t)n, f);
    free(raw);
    fclose(f);
    free(buf);
}

/* Variante HÉRITÉE du helper ci-dessus : écrit des `possibility_packet`
 * BRUTS, tels qu'un cliché antérieur à la compaction en contient. Sert au
 * seul test de restauration d'un cliché v1 (le manifeste doit alors porter
 * la magie v1, cf. STOCK_SPILL_MANIFEST_MAGIC_LEGACY côté module). */
static void write_legacy_segment(const char *path, int first_marker, int n)
{
    struct possibility_packet *buf = calloc((size_t)n, sizeof(struct possibility_packet));
    for (int i = 0; i < n; i++) {
        init_empty_grid(&buf[i]);
        buf[i].alloc = 1;
        buf[i].grid[0][0] = (int16_t)(first_marker + i);
    }
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "write_legacy_segment: fopen(%s) a échoué\n", path);
        free(buf);
        return;
    }
    fwrite(buf, sizeof(struct possibility_packet), (size_t)n, f);
    fclose(f);
    free(buf);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Segment au format ACTUEL (manifeste v3), écrit à la main : une seule trame
 * (en-tête « ETSB » + enregistrements compacts bruts, codec 0 + pied
 * « ETSE », cf. spill_frame_t dans core/stock_spill.c). Mêmes marqueurs que
 * write_stride_segment. @return octets écrits — le `tail_bytes` du manifeste. */
static long write_framed_segment(const char *path, int first_marker, int n)
{
    uint8_t *raw = calloc((size_t)n, (size_t)PACKET_CODEC_MAX_BYTES);
    size_t used = 0;
    for (int i = 0; i < n; i++) {
        struct possibility_packet p;
        memset(&p, 0, sizeof p);
        init_empty_grid(&p);
        p.grid[0][0] = (int16_t)(first_marker + i);
        size_t len = 0;
        if (packet_codec_encode(&p, raw + used, (size_t)n * PACKET_CODEC_MAX_BYTES - used, &len) != 0) {
            fprintf(stderr, "write_framed_segment: paquet %d non encodable\n", i);
        }
        used += len;
    }
    uint8_t h[20] = { 'E', 'T', 'S', 'B', 0, 0, 0, 0 };
    put_le32(h + 8, (uint32_t)n);
    put_le32(h + 12, (uint32_t)used);
    put_le32(h + 16, (uint32_t)used);
    uint8_t t[12] = { 0 };
    put_le32(t, (uint32_t)n);
    put_le32(t + 4, (uint32_t)used);
    memcpy(t + 8, "ETSE", 4);
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "write_framed_segment: fopen(%s) a échoué\n", path);
        free(raw);
        return -1;
    }
    fwrite(h, 1, sizeof h, f);
    fwrite(raw, 1, used, f);
    fwrite(t, 1, sizeof t, f);
    fclose(f);
    free(raw);
    return (long)(sizeof h + used + sizeof t);
}

/* Possibilités d'un segment en trames, en suivant ses en-têtes et pieds,
 * indépendamment du module. -1 si le fichier ne se découpe pas en trames
 * cohérentes. `codecs_out` (peut être NULL) reçoit l'OU des codecs vus
 * (bit 0 : brut, bit 1 : zstd). */
static long framed_segment_records(const char *path, int *codecs_out)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return -1;
    }
    long total = 0;
    int codecs = 0;
    uint8_t h[20], t[12];
    for (;;) {
        size_t got = fread(h, 1, sizeof h, f);
        if (got == 0) {
            break;
        }
        if (got != sizeof h || memcmp(h, "ETSB", 4) != 0) {
            total = -1;
            break;
        }
        uint32_t stored = get_le32(h + 16);
        if (fseek(f, (long)stored, SEEK_CUR) != 0 || fread(t, 1, sizeof t, f) != sizeof t
            || memcmp(t + 8, "ETSE", 4) != 0 || get_le32(t) != get_le32(h + 8) || get_le32(t + 4) != stored) {
            total = -1;
            break;
        }
        codecs |= 1 << h[4];
        total += (long)get_le32(h + 8);
    }
    fclose(f);
    if (codecs_out != NULL) {
        *codecs_out = codecs;
    }
    return total;
}

static void write_manifest_line(FILE *f, char pool, int file_index, int last_seq,
                                 unsigned long long packets, long tail_bytes)
{
    fprintf(f, "%c %d %d %llu %ld\n", pool, file_index, last_seq, packets, tail_bytes);
}

/* Récupère tous les paquets résidents (jusqu'à `max`) et renvoie le nombre
 * lu, en remplissant `markers_out[i]` avec chaque marqueur `grid[0][0]`
 * rencontré — que `add_packets`/`write_stride_segment` posent tous les deux —
 * pour vérifier qu'un ensemble EXACT de possibilités (ni perte, ni
 * duplication, ni contamination croisée) est revenu en RAM.
 *
 * VERSION 13 (docs/autosearch_step.md) :
 * délibérément PAS `.alloc` ici — un paquet qui a fait un aller-retour par
 * un segment de débordement traverse `stock_spill_reload()`, qui recompte
 * `alloc` au sens nombre de cases pleines (idempotent, mais `add_packets`/
 * `write_stride_segment` ne posent qu'UNE case pleine par construction : tout
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

/* Vide le stock RAM en relevant le marqueur `grid[0][0]` de chaque possibilité. */
static int collect_markers(int *seen, int max)
{
    int n = 0;
    while (datas_size() > 0) {
        array_possibility_packet *r = get_last_possibility(NULL, 1000, NULL);
        for (int i = 0; i < r->size && n < max; i++) {
            seen[n++] = r->possibilities[i].grid[0][0];
        }
        free_array_possibility_packet(r);
    }
    return n;
}

/* Le rechargement consomme le sommet d'un segment en reculant `tail_bytes`,
 * sans toucher au fichier. L'éviction suivante vers le MÊME (pool, file)
 * ajoutait en `fopen("ab")`, donc à la fin PHYSIQUE, au-delà du sommet
 * logique : à pas fixe, le rechargement d'après rendait une deuxième fois les
 * possibilités déjà servies (7..10) et ne voyait jamais les nouvelles
 * (43..46) — doublons ET pertes, sur un enchaînement ordinaire
 * (rechargement partiel quand la RAM se vide, puis éviction quand elle se
 * remplit). En trames, le sommet logique tombe au milieu d'une trame et le
 * rechargement échoue. Contre-épreuve : sans spill_trim_segment_to_tail, la
 * relecture finale ne rend pas 1..6 et 41..46. Le rechargement rendant des
 * trames ENTIÈRES, l'éviction se fait en deux pas (deux trames : 1..6, 7..10). */
TEST evict_after_a_partial_reload_neither_duplicates_nor_loses(void)
{
    char tmpl[64];
    char *dir = make_tmp_spill_dir(tmpl);
    ASSERT(dir != NULL);
    stock_spill_configure(dir, nb_file_possibility);
    drain_datamanager();
    datamanager_set_ram_limit_packets_for_tests(0);

    int allocs[20];
    for (int i = 0; i < 20; i++) allocs[i] = i + 1;
    add_packets(allocs, 20);
    set_ram_limit_for_resident(2, 20);
    ASSERT_EQ_FMT(6, stock_spill_step(6), "%d");     /* trame 1..6 */
    ASSERT_EQ_FMT(4, stock_spill_step(4), "%d");     /* trame 7..10 */
    int seen[64];
    ASSERT_EQ_FMT(10, collect_markers(seen, 64), "%d");

    /* Rechargement PARTIEL du sommet : 7..10 reviennent, 1..6 restent. */
    datamanager_set_ram_limit_bytes_for_tests(1ULL << 30);
    ASSERT_EQ_FMT(4, stock_spill_step(4), "%d");
    ASSERT_EQ_FMT(4, collect_markers(seen, 64), "%d");

    /* Nouvelles possibilités évincées vers la MÊME file. */
    datamanager_set_ram_limit_packets_for_tests(0);
    datamanager_reset_rr_state_for_tests();
    int fresh[6];
    for (int i = 0; i < 6; i++) fresh[i] = 41 + i;
    add_packets(fresh, 6);
    ASSERT_EQ_FMT(6ULL, file_size(0), "%llu");
    datamanager_set_ram_limit_bytes_for_tests(1);
    ASSERT_EQ_FMT(6, stock_spill_step(6), "%d");

    /* Tout recharger : exactement 1..6 et 41..46, chacun une fois. */
    datamanager_set_ram_limit_bytes_for_tests(1ULL << 30);
    for (int k = 0; k < 20 && stock_spill_total_packets() > 0; k++) {
        stock_spill_step(100);
    }
    int n = collect_markers(seen, 64);
    ASSERT_EQ_FMT(12, n, "%d");
    int count[64] = {0};
    for (int i = 0; i < n; i++) {
        ASSERT(seen[i] > 0 && seen[i] < 64);
        count[seen[i]]++;
    }
    for (int m = 1; m <= 6; m++) ASSERT_EQ_FMT(1, count[m], "%d");
    for (int m = 41; m <= 46; m++) ASSERT_EQ_FMT(1, count[m], "%d");

    datamanager_set_ram_limit_packets_for_tests(0);
    drain_datamanager();
    rmdir_recursive(dir);
    PASS();
}

/* Même enchaînement, mais le segment redevenu sommet est LIÉ physiquement à un
 * cliché (les segments pleins le sont) : le ramener à son sommet logique ne
 * doit pas modifier le cliché. Contre-épreuves : sans recalage, l'ajout
 * allonge le fichier du cliché ; avec un `truncate` en place qui ignore le
 * nombre de liens, l'ajout suivant réécrit sa fin. */
TEST evict_after_a_partial_reload_leaves_a_linked_snapshot_intact(void)
{
    char tmpl[64];
    char *dir = make_tmp_spill_dir(tmpl);
    ASSERT(dir != NULL);
    stock_spill_configure(dir, nb_file_possibility);
    stock_spill_set_segment_records_for_tests(4); /* 4 possibilités/segment */
    drain_datamanager();
    datamanager_set_ram_limit_packets_for_tests(0);

    int allocs[10];
    for (int i = 0; i < 10; i++) allocs[i] = i + 1;
    add_packets(allocs, 10);
    datamanager_set_ram_limit_bytes_for_tests(1);
    /* Segment 1 : [1..4] ; segment 2 : deux trames [5,6] [7,8] ; segment 3 : [9,10]. */
    ASSERT_EQ_FMT(4, stock_spill_step(4), "%d");
    ASSERT_EQ_FMT(2, stock_spill_step(2), "%d");
    ASSERT_EQ_FMT(2, stock_spill_step(2), "%d");
    ASSERT_EQ_FMT(2, stock_spill_step(2), "%d");
    ASSERT_EQ_FMT(3ULL, stock_spill_total_segments(), "%llu");
    stock_spill_snapshot("snap");

    char snap2[PATH_MAX];
    snprintf(snap2, sizeof snap2, "%s/snap/spill_u_0_2.dat", dir);
    /* Contenu, pas seulement taille : recalé puis complété, le segment
       retrouve ses 4 enregistrements — mais plus les mêmes. */
    struct stat st2;
    ASSERT_EQ_FMT(0, stat(snap2, &st2), "%d");
    size_t seg_len = (size_t)st2.st_size;
    unsigned char *before = malloc(seg_len + 1);
    unsigned char *after = malloc(seg_len + 1);
    FILE *sf = fopen(snap2, "rb");
    ASSERT(sf != NULL);
    ASSERT_EQ_FMT(seg_len, fread(before, 1, seg_len + 1, sf), "%zu");
    fclose(sf);

    /* Dépile le segment 3 puis la trame du haut du segment 2 : le 2, lié,
       redevient sommet partiel. */
    int seen[64];
    datamanager_set_ram_limit_bytes_for_tests(1ULL << 30);
    ASSERT_EQ_FMT(2, stock_spill_step(2), "%d");
    ASSERT_EQ_FMT(2, stock_spill_step(1), "%d"); /* une trame entière, jamais entamée */
    collect_markers(seen, 64);

    datamanager_set_ram_limit_packets_for_tests(0);
    datamanager_reset_rr_state_for_tests();
    int fresh[2] = { 41, 42 };
    add_packets(fresh, 2);
    datamanager_set_ram_limit_bytes_for_tests(1);
    ASSERT_EQ_FMT(2, stock_spill_step(2), "%d");

    sf = fopen(snap2, "rb");
    ASSERT(sf != NULL);
    size_t got = fread(after, 1, seg_len + 1, sf);
    fclose(sf);
    int intact = (got == seg_len) && memcmp(before, after, seg_len) == 0;
    free(before);
    free(after);
    ASSERT(intact);

    /* Et le vivant rend bien 1..6 puis 41, 42. */
    datamanager_set_ram_limit_bytes_for_tests(1ULL << 30);
    for (int k = 0; k < 20 && stock_spill_total_packets() > 0; k++) {
        stock_spill_step(100);
    }
    int n = collect_markers(seen, 64);
    ASSERT_EQ_FMT(8, n, "%d");
    int count[64] = {0};
    for (int i = 0; i < n; i++) count[seen[i]]++;
    for (int m = 1; m <= 6; m++) ASSERT_EQ_FMT(1, count[m], "%d");
    ASSERT_EQ_FMT(1, count[41], "%d");
    ASSERT_EQ_FMT(1, count[42], "%d");

    stock_spill_set_segment_records_for_tests(0);
    datamanager_set_ram_limit_packets_for_tests(0);
    drain_datamanager();
    rmdir_recursive(dir);
    PASS();
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
    stock_spill_set_segment_records_for_tests(5); /* 5 possibilités/segment */

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
    ASSERT_EQ_FMT(5L, framed_segment_records(snap1, NULL), "%ld");
    ASSERT_EQ_FMT(5L, framed_segment_records(snap2, NULL), "%ld");
    ASSERT_EQ_FMT(2L, framed_segment_records(snap3, NULL), "%ld"); /* 2 possibilités restantes */

    char manifest_path[PATH_MAX];
    snprintf(manifest_path, sizeof manifest_path, "%s/snap/manifest.txt", dir);
    FILE *mf = fopen(manifest_path, "r");
    ASSERT(mf != NULL);
    char line[256];
    ASSERT(fgets(line, sizeof line, mf) != NULL);
    line[strcspn(line, "\r\n")] = '\0';
    ASSERT_STR_EQ("eternityii-spill-manifest-v3", line);
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
    stock_spill_set_segment_records_for_tests(5);

    drain_datamanager();
    datamanager_set_ram_limit_packets_for_tests(0);
    int allocs_old[12];
    for (int i = 0; i < 12; i++) { allocs_old[i] = MARK_BASE + i; } /* marqueurs "ancien" jeu */
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
    stock_spill_set_segment_records_for_tests(5); /* configure() a réinitialisé la surcharge */
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
    stock_spill_set_segment_records_for_tests(4);

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
    stock_spill_set_segment_records_for_tests(4); /* configure() a réinitialisé la surcharge */
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
    long bytes_a = write_framed_segment(seg_a, 20, 3); /* old_file_index=0 : marqueurs 20,21,22 */
    long bytes_b = write_framed_segment(seg_b, 40, 2); /* old_file_index=3 : marqueurs 40,41 */

    char manifest_path[PATH_MAX];
    snprintf(manifest_path, sizeof manifest_path, "%s/manifest.txt", snap_dir);
    FILE *mf = fopen(manifest_path, "w");
    ASSERT(mf != NULL);
    fprintf(mf, "eternityii-spill-manifest-v3\n");
    write_manifest_line(mf, 'u', 0, 1, 3, bytes_a);
    write_manifest_line(mf, 'u', 3, 1, 2, bytes_b);
    fclose(mf);

    /* nb_files=3 : old {0,3} convergent tous deux vers la file vivante 0
     * (0%3=0, 3%3=0) -- collision garantie. */
    drain_datamanager();
    datamanager_set_ram_limit_packets_for_tests(0);
    stock_spill_configure(dir, 3);
    stock_spill_set_segment_records_for_tests(2); /* force le réempaquetage à travers plusieurs segments */

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
    ASSERT_EQ_FMT(2L, framed_segment_records(rebuilt1, NULL), "%ld"); /* plein */
    ASSERT_EQ_FMT(2L, framed_segment_records(rebuilt2, NULL), "%ld"); /* plein */
    ASSERT_EQ_FMT(1L, framed_segment_records(rebuilt3, NULL), "%ld"); /* sommet, partiel */

    /* Conservation exacte des données : les 5 marqueurs des DEUX sources
     * reviennent, sans perte ni duplication. */
    datamanager_set_ram_limit_packets_for_tests(1000);
    int rounds = 0;
    while (stock_spill_total_packets() > 0ULL && rounds < 30) { stock_spill_step(4096); rounds++; }
    int markers[16];
    int n = drain_and_collect_markers(markers, 16);
    ASSERT_EQ_FMT(5, n, "%d");
    qsort(markers, (size_t)n, sizeof(int), int_cmp);
    int expected[5] = { 20, 21, 22, 40, 41 };
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
    write_framed_segment(seg1, 20, 3); /* segment 1, plein : marqueurs 20,21,22 */
    /* spill_u_0_2.dat (le sommet, 2 possibilités) volontairement absent. */

    char manifest_path[PATH_MAX];
    snprintf(manifest_path, sizeof manifest_path, "%s/manifest.txt", snap_dir);
    FILE *mf = fopen(manifest_path, "w");
    ASSERT(mf != NULL);
    fprintf(mf, "eternityii-spill-manifest-v3\n");
    write_manifest_line(mf, 'u', 0, 2, 5, 100);
    fclose(mf);

    drain_datamanager();
    datamanager_set_ram_limit_packets_for_tests(0);
    stock_spill_configure(dir, nb_file_possibility); /* même nb_files -> pas de collision (old 0 -> new 0) */
    stock_spill_set_segment_records_for_tests(3);

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
    long bytes_a = write_framed_segment(seg_a, 20, 2); /* marqueurs 20,21 */
    /* old_file_index=3 : manifeste annonce 2 possibilités, mais le fichier
     * .dat correspondant est absent (supprimé/corrompu). */

    char manifest_path[PATH_MAX];
    snprintf(manifest_path, sizeof manifest_path, "%s/manifest.txt", snap_dir);
    FILE *mf = fopen(manifest_path, "w");
    ASSERT(mf != NULL);
    fprintf(mf, "eternityii-spill-manifest-v3\n");
    write_manifest_line(mf, 'u', 0, 1, 2, bytes_a);
    write_manifest_line(mf, 'u', 3, 1, 2, bytes_a);
    fclose(mf);

    /* nb_files=3 : old {0,3} convergent tous deux vers la file vivante 0
     * (0%3=0, 3%3=0) -- collision garantie, comme
     * restore_snapshot_collision_repacks_when_stock_files_shrinks. */
    drain_datamanager();
    datamanager_set_ram_limit_packets_for_tests(0);
    stock_spill_configure(dir, 3);
    stock_spill_set_segment_records_for_tests(2);

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
    ASSERT_EQ_FMT(20, markers[0], "%d");
    ASSERT_EQ_FMT(21, markers[1], "%d");

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
    stock_spill_set_segment_records_for_tests(3);

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
    long bytes = write_framed_segment(seg, 50, 2);
    char manifest_path[PATH_MAX];
    snprintf(manifest_path, sizeof manifest_path, "%s/manifest.txt", snap_dir);
    FILE *mf = fopen(manifest_path, "w");
    ASSERT(mf != NULL);
    fprintf(mf, "eternityii-spill-manifest-v3\n");
    write_manifest_line(mf, 'u', 0, 1, 2, bytes);
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
    ASSERT_EQ_FMT(50, markers[0], "%d");
    ASSERT_EQ_FMT(51, markers[1], "%d");

    datamanager_set_ram_limit_packets_for_tests(0);
    drain_datamanager();
    rmdir_recursive(dir);
    PASS();
}

/* Cliché au format HÉRITÉ (manifeste v1, segments en `possibility_packet`
 * bruts) : restaurable, par RÉÉCRITURE en trames — jamais par lien direct,
 * les formats n'ayant pas les mêmes octets. Avec le test v2 qui suit, c'est le
 * seul chemin qui fasse traverser la frontière de format à des données
 * réelles, donc le seul qui puisse attraper une confusion de pas. */
TEST restore_snapshot_converts_a_legacy_format_snapshot(void)
{
    char tmpl[64];
    char *dir = make_tmp_spill_dir(tmpl);
    ASSERT(dir != NULL);

    char snap_dir[PATH_MAX];
    snprintf(snap_dir, sizeof snap_dir, "%s/snap", dir);
    ASSERT_EQ_FMT(0, mkdir(snap_dir, 0755), "%d");

    /* Deux segments bruts : un PLEIN (2 paquets) et le sommet, partiel (1). */
    long legacy_record = (long)sizeof(struct possibility_packet);
    char seg1[PATH_MAX], seg2[PATH_MAX];
    snprintf(seg1, sizeof seg1, "%s/spill_u_0_1.dat", snap_dir);
    snprintf(seg2, sizeof seg2, "%s/spill_u_0_2.dat", snap_dir);
    write_legacy_segment(seg1, 20, 2);
    write_legacy_segment(seg2, 22, 1);

    char manifest_path[PATH_MAX];
    snprintf(manifest_path, sizeof manifest_path, "%s/manifest.txt", snap_dir);
    FILE *mf = fopen(manifest_path, "w");
    ASSERT(mf != NULL);
    fprintf(mf, "eternityii-spill-manifest-v1\n");
    write_manifest_line(mf, 'u', 0, 2, 3, legacy_record); /* sommet = 1 paquet brut */
    fclose(mf);

    drain_datamanager();
    datamanager_set_ram_limit_packets_for_tests(0);
    stock_spill_configure(dir, nb_file_possibility);
    /* Le segment plein du cliché se lit à sa taille de fichier, quelle que
     * soit la taille de segment courante. */
    stock_spill_set_segment_records_for_tests(2);

    capture_stderr();
    stock_spill_restore_snapshot("snap");
    (void)restore_stderr_size();
    ASSERT_EQ_FMT(3ULL, stock_spill_total_packets(), "%llu");

    /* Les segments vivants sont désormais en TRAMES. */
    char live[PATH_MAX];
    snprintf(live, sizeof live, "%s/spill_u_0_1.dat", dir);
    ASSERT_EQ_FMT(2L, framed_segment_records(live, NULL), "%ld");

    /* Et les trois possibilités reviennent intactes en RAM. */
    datamanager_set_ram_limit_packets_for_tests(1000);
    int rounds = 0;
    while (stock_spill_total_packets() > 0ULL && rounds < 30) { stock_spill_step(4096); rounds++; }
    int markers[8];
    int n = drain_and_collect_markers(markers, 8);
    ASSERT_EQ_FMT(3, n, "%d");
    qsort(markers, (size_t)n, sizeof(int), int_cmp);
    ASSERT_EQ_FMT(20, markers[0], "%d");
    ASSERT_EQ_FMT(21, markers[1], "%d");
    ASSERT_EQ_FMT(22, markers[2], "%d");

    datamanager_set_ram_limit_packets_for_tests(0);
    drain_datamanager();
    rmdir_recursive(dir);
    PASS();
}

/* Cliché v2 (forme compacte à pas FIXE, le format d'avant les trames) :
 * celui qu'un serveur mis à jour trouve à côté de son dernier .back. Relu au
 * pas compact, réécrit en trames. */
TEST restore_snapshot_converts_a_v2_stride_snapshot(void)
{
    char tmpl[64];
    char *dir = make_tmp_spill_dir(tmpl);
    ASSERT(dir != NULL);
    char snap_dir[PATH_MAX];
    snprintf(snap_dir, sizeof snap_dir, "%s/snap", dir);
    ASSERT_EQ_FMT(0, mkdir(snap_dir, 0755), "%d");
    char seg1[PATH_MAX], seg2[PATH_MAX];
    snprintf(seg1, sizeof seg1, "%s/spill_u_0_1.dat", snap_dir);
    snprintf(seg2, sizeof seg2, "%s/spill_u_0_2.dat", snap_dir);
    write_stride_segment(seg1, 20, 3);
    write_stride_segment(seg2, 23, 2);
    char manifest_path[PATH_MAX];
    snprintf(manifest_path, sizeof manifest_path, "%s/manifest.txt", snap_dir);
    FILE *mf = fopen(manifest_path, "w");
    ASSERT(mf != NULL);
    fprintf(mf, "eternityii-spill-manifest-v2\n");
    write_manifest_line(mf, 'u', 0, 2, 5, 2L * (long)PACKET_CODEC_MAX_BYTES);
    fclose(mf);

    drain_datamanager();
    datamanager_set_ram_limit_packets_for_tests(0);
    stock_spill_configure(dir, nb_file_possibility);
    capture_stderr();
    unsigned long long restored = stock_spill_restore_snapshot("snap");
    (void)restore_stderr_size();
    ASSERT_EQ_FMT(5ULL, restored, "%llu");
    char live[PATH_MAX];
    snprintf(live, sizeof live, "%s/spill_u_0_1.dat", dir);
    ASSERT_EQ_FMT(5L, framed_segment_records(live, NULL), "%ld");

    datamanager_set_ram_limit_packets_for_tests(1000);
    for (int k = 0; k < 30 && stock_spill_total_packets() > 0ULL; k++) stock_spill_step(4096);
    int markers[8];
    int n = drain_and_collect_markers(markers, 8);
    ASSERT_EQ_FMT(5, n, "%d");
    qsort(markers, (size_t)n, sizeof(int), int_cmp);
    for (int i = 0; i < 5; i++) ASSERT_EQ_FMT(20 + i, markers[i], "%d");

    datamanager_set_ram_limit_packets_for_tests(0);
    drain_datamanager();
    rmdir_recursive(dir);
    PASS();
}

/* Un segment de cliché présent mais ABÎMÉ (une trame dont le pied ne
 * correspond plus à l'en-tête) ne passe pas pour restauré : sans collision, un
 * simple lien ne lit rien, c'est la relecture des trames qui le refuse. */
TEST restore_snapshot_refuses_a_damaged_frame(void)
{
    char tmpl[64];
    char *dir = make_tmp_spill_dir(tmpl);
    ASSERT(dir != NULL);
    char snap_dir[PATH_MAX];
    snprintf(snap_dir, sizeof snap_dir, "%s/snap", dir);
    ASSERT_EQ_FMT(0, mkdir(snap_dir, 0755), "%d");
    char seg[PATH_MAX];
    snprintf(seg, sizeof seg, "%s/spill_u_0_1.dat", snap_dir);
    long bytes = write_framed_segment(seg, 20, 3);
    ASSERT(bytes > 12);
    FILE *f = fopen(seg, "r+b");
    ASSERT(f != NULL);
    ASSERT_EQ_FMT(0, fseek(f, bytes - 12, SEEK_SET), "%d"); /* nombre de possibilités du pied */
    fputc(7, f);
    fclose(f);
    char manifest_path[PATH_MAX];
    snprintf(manifest_path, sizeof manifest_path, "%s/manifest.txt", snap_dir);
    FILE *mf = fopen(manifest_path, "w");
    ASSERT(mf != NULL);
    fprintf(mf, "eternityii-spill-manifest-v3\n");
    write_manifest_line(mf, 'u', 0, 1, 3, bytes);
    fclose(mf);

    drain_datamanager();
    datamanager_set_ram_limit_packets_for_tests(0);
    stock_spill_configure(dir, nb_file_possibility);
    capture_stderr();
    unsigned long long restored = stock_spill_restore_snapshot("snap");
    long err = restore_stderr_size();
    ASSERT_EQ_FMT(0ULL, restored, "%llu");
    ASSERT_EQ_FMT(0ULL, stock_spill_total_packets(), "%llu");
    ASSERT(err > 0);

    drain_datamanager();
    rmdir_recursive(dir);
    PASS();
}

/* Un `restore` sous plafond RAM ne perd AUCUNE possibilité : ce qui ne tient
 * pas en RAM part sur disque, il n'en disparaît pas.
 *
 * Avant correctif : `import()` ignorait la valeur de retour d'
 * `add_possibility`, or `put_to_pool` REFUSE (sans rien insérer) dès que le
 * plafond est atteint. Chaque refus était donc une possibilité perdue en
 * silence. Sur un cas réel — 3 407 891 possibilités restaurées sous
 * `--stock-max-ram 1024` — il en restait 1 272 974 en RAM et 483 328 sur
 * disque : **1 651 589 évaporées**. Le thread de débordement n'y pouvait rien,
 * il évince 4096 possibilités par tick de 100 ms là où l'import en pousse des
 * centaines de milliers par seconde.
 *
 * Restaurer sans plafond PUIS appliquer le plafond ne perdait rien, lui — d'où
 * un bug longtemps invisible. */
TEST restore_under_a_ram_cap_loses_nothing(void)
{
    char tmpl[64];
    char *dir = make_tmp_spill_dir(tmpl);
    ASSERT(dir != NULL);

    /* 1. Un .back de 200 possibilités, produit sans plafond. */
    drain_datamanager();
    datamanager_set_ram_limit_packets_for_tests(0);
    stock_spill_configure(dir, nb_file_possibility);
    int allocs[200];
    for (int i = 0; i < 200; i++) { allocs[i] = i + 1; }
    add_packets(allocs, 200);
    ASSERT_EQ_FMT(200ULL, datas_size(), "%llu");

    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/stock.back", dir);
    ASSERT_EQ_FMT(BACKUP_OK, backup(path), "%d");
    /* Occupation RÉELLE des 200 possibilités de ce test, mesurée avant le
       vidage : le plafond en sera le quart. Un plafond exprimé en « nombre »
       supposerait une taille par possibilité constante, ce qui n'est plus
       vrai depuis que le stock range des enregistrements compacts. */
    unsigned long long cap_bytes = datamanager_resident_bytes() / 4;
    drain_datamanager();
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");

    /* 2. Restauration sous un plafond QUATRE FOIS trop petit. Le crochet de
     *    dégagement rend l'import capable de faire de la place lui-même : sans
     *    lui il dépendrait du tick du thread de débordement, qui n'existe pas
     *    dans un test — et en production ne suit pas la cadence d'un import. */
    stock_spill_configure(dir, nb_file_possibility);
    datamanager_set_ram_relief_hook(stock_spill_relieve);
    datamanager_set_ram_limit_bytes_for_tests(cap_bytes);

    capture_stderr();
    int rc = restore(path);
    (void)restore_stderr_size();
    ASSERT_EQ_FMT(0, rc, "%d");

    /* 3. Rien n'a disparu : tout est soit résident, soit déporté. */
    unsigned long long resident = datas_size();
    unsigned long long spilled = stock_spill_total_packets();
    ASSERT_EQ_FMT(200ULL, resident + spilled, "%llu");
    ASSERT(resident <= 60ULL);   /* le plafond est bien respecté */
    ASSERT(spilled > 0ULL);      /* et le surplus est bien parti sur disque */

    datamanager_set_ram_relief_hook(NULL);
    datamanager_set_ram_limit_packets_for_tests(0);
    drain_datamanager();
    rmdir_recursive(dir);
    PASS();
}

/* Chemin de l'appelant qui DÉTIENT la fenêtre de maintenance et importe
 * dedans : l'import doit pouvoir faire de la place LUI-MÊME, sinon il attend
 * une éviction que le drapeau `maintenance` interdit, indéfiniment.
 *
 * `stock_spill_step` refuse délibérément de travailler sous maintenance (une
 * éviction CONCURRENTE ferait migrer une possibilité au milieu d'une capture).
 * `stock_spill_relieve` est la porte réservée à l'appelant qui détient
 * lui-même la fenêtre : il n'y a alors aucune concurrence, l'éviction
 * s'intercale entre deux de ses propres insertions.
 *
 * Exécuté dans un FILS avec `alarm()` : sans le correctif ce test ne renvoie
 * jamais, et un test qui pend est un test qui bloque la CI au lieu d'échouer. */
static char g_maint_import_path[PATH_MAX];

static void maint_import_child(void)
{
    alarm(15); /* filet : un blocage tue le fils au lieu de figer le runner */
    datamanager_begin_maintenance();
    int rc = import(NULL, g_maint_import_path);
    datamanager_end_maintenance();
    if (rc != 0) {
        exit(3);
    }
    unsigned long long total = datas_size() + stock_spill_total_packets();
    exit(total == 200ULL ? 0 : 2);
}

TEST import_makes_room_itself_when_it_holds_the_maintenance_window(void)
{
    char tmpl[64];
    char *dir = make_tmp_spill_dir(tmpl);
    ASSERT(dir != NULL);

    drain_datamanager();
    datamanager_set_ram_limit_packets_for_tests(0);
    stock_spill_configure(dir, nb_file_possibility);
    int allocs[200];
    for (int i = 0; i < 200; i++) { allocs[i] = i + 1; }
    add_packets(allocs, 200);

    snprintf(g_maint_import_path, sizeof g_maint_import_path, "%s/stock.back", dir);
    ASSERT_EQ_FMT(BACKUP_OK, backup(g_maint_import_path), "%d");
    drain_datamanager();

    stock_spill_configure(dir, nb_file_possibility);
    datamanager_set_ram_relief_hook(stock_spill_relieve);
    datamanager_set_ram_limit_packets_for_tests(50);

    /* 0 = les 200 possibilités sont là ; 2 = il en manque ; -1 = le fils a été
     * tué par l'alarme, donc l'import ne rendait pas la main. */
    ASSERT_EQ_FMT(0, run_in_fork(maint_import_child, NULL), "%d");

    datamanager_set_ram_relief_hook(NULL);
    datamanager_set_ram_limit_packets_for_tests(0);
    drain_datamanager();
    rmdir_recursive(dir);
    PASS();
}

/* RÉGRESSION : la fenêtre de maintenance posée AUTOUR de `restore()` doit
 * tenir pendant TOUT l'import, y compris après les `lock_all_file()`/
 * `unlock_all_file()` que `restore` pose et lève lui-même pour vider le stock.
 *
 * Le défaut corrigé : `maintenance` était un DRAPEAU, et `unlock_all_file()`
 * le remettait inconditionnellement à 0. La fenêtre que `restore_apply`
 * (`ui/command_lines.c`) croyait tenir sur toute la séquence se refermait donc
 * au premier `unlock_*` imbriqué, AVANT l'import — et le thread de
 * débordement (`spill_thread`, `app/etii_server.c`) redevenait libre d'évincer
 * ou de recharger en plein remplacement du stock, exactement ce que la fenêtre
 * existait pour interdire. Devenu compteur de profondeur, cet `unlock_*`
 * ramène la profondeur de 2 à 1 et la fenêtre externe survit.
 *
 * Deux observations, l'une pendant, l'autre après :
 *   - le crochet de dégagement RAM sert de SONDE : appelé depuis
 *     `add_possibility_waiting_for_room` au cœur de l'import, il constate que
 *     la fenêtre est toujours tenue. Le plafond RAM (50 pour 200
 *     possibilités) garantit qu'il est bien atteint — la sonde se vérifie
 *     elle-même (code 5 si elle n'a jamais été appelée, sans quoi le test
 *     passerait à vide) ;
 *   - au retour de `restore()`, la fenêtre doit encore être ouverte.
 *
 * Exécuté dans un FILS avec `alarm()`, comme son voisin ci-dessus : la fenêtre
 * tenue rend `stock_spill_step` inerte, donc une régression du crochet de
 * dégagement se manifesterait par un blocage — qui doit échouer, pas figer la
 * CI. */
static char g_maint_restore_path[PATH_MAX];
static int g_maint_probe_calls = 0;
static int g_maint_probe_saw_window_closed = 0;

static int maintenance_probing_relief_hook(int max_packets)
{
    g_maint_probe_calls++;
    if (!datamanager_is_maintenance_active()) {
        g_maint_probe_saw_window_closed = 1;
    }
    return stock_spill_relieve(max_packets);
}

static void maint_restore_child(void)
{
    alarm(15); /* filet : un blocage tue le fils au lieu de figer le runner */
    g_maint_probe_calls = 0;
    g_maint_probe_saw_window_closed = 0;

    datamanager_begin_maintenance();
    int rc = restore(g_maint_restore_path);
    int window_still_open = datamanager_is_maintenance_active();
    datamanager_end_maintenance();

    if (rc != 0) {
        exit(2);
    }
    if (g_maint_probe_saw_window_closed) {
        exit(4); /* refermée PENDANT l'import */
    }
    if (!window_still_open) {
        exit(3); /* refermée par le unlock_all_file() interne à restore() */
    }
    if (g_maint_probe_calls == 0) {
        exit(5); /* sonde jamais atteinte : le test ne prouverait rien */
    }
    exit(0);
}

TEST restore_keeps_the_maintenance_window_open_through_the_import(void)
{
    char tmpl[64];
    char *dir = make_tmp_spill_dir(tmpl);
    ASSERT(dir != NULL);

    drain_datamanager();
    datamanager_set_ram_limit_packets_for_tests(0);
    stock_spill_configure(dir, nb_file_possibility);
    int allocs[200];
    for (int i = 0; i < 200; i++) { allocs[i] = i + 1; }
    add_packets(allocs, 200);

    snprintf(g_maint_restore_path, sizeof g_maint_restore_path, "%s/stock.back", dir);
    ASSERT_EQ_FMT(BACKUP_OK, backup(g_maint_restore_path), "%d");
    /* Occupation RÉELLE des 200 possibilités, mesurée avant le vidage : le
       plafond en sera le quart, pour que l'import le heurte et sollicite le
       dégagement (sans quoi la sonde ne serait jamais atteinte et le test ne
       prouverait rien — code 5). Un plafond exprimé en « nombre » ne mordrait
       plus, le stock rangeant des enregistrements compacts. */
    unsigned long long cap_bytes = datamanager_resident_bytes() / 4;
    drain_datamanager();

    stock_spill_configure(dir, nb_file_possibility);
    datamanager_set_ram_relief_hook(maintenance_probing_relief_hook);
    datamanager_set_ram_limit_bytes_for_tests(cap_bytes);

    /* 0 = fenêtre tenue de bout en bout ; 2..5 = cf. maint_restore_child ;
     * -1 = fils tué par l'alarme, donc `restore` ne rendait pas la main. */
    ASSERT_EQ_FMT(0, run_in_fork(maint_restore_child, NULL), "%d");

    datamanager_set_ram_relief_hook(NULL);
    datamanager_set_ram_limit_packets_for_tests(0);
    drain_datamanager();
    rmdir_recursive(dir);
    PASS();
}

/* --------------------------------------------------------------------------
 * Consommation du débordement par l'expansion
 * ------------------------------------------------------------------------ */

static const datamanager_expansion_disk_source_t g_spill_source = {
    stock_spill_expansion_begin, stock_spill_expansion_take, stock_spill_expansion_end,
    stock_spill_expansion_stats
};

/* Recharge TOUT le débordement puis vide la RAM en relevant `alloc`. */
static int collect_all_allocs(int *allocs, int max)
{
    datamanager_set_ram_limit_bytes_for_tests(1ULL << 30);
    for (int k = 0; k < 200 && stock_spill_total_packets() > 0; k++) {
        stock_spill_step(4096);
    }
    int n = 0;
    while (datas_size() > 0) {
        array_possibility_packet *r = get_last_possibility(NULL, 1000, NULL);
        for (int i = 0; i < r->size && n < max; i++) {
            allocs[n++] = r->possibilities[i].alloc;
        }
        free_array_possibility_packet(r);
    }
    return n;
}

/* Référence : même expansion, sans plafond ni débordement. */
static int expand_reference_count(int seeds, int level, int max_levels)
{
    drain_datamanager();
    datamanager_set_ram_limit_bytes_for_tests(0);
    for (int i = 0; i < seeds; i++) seed_genesis(1);
    int saved = expand_max_levels;
    expand_max_levels = max_levels;
    expand_datas_to_level(level, make_expand_free_map(), make_expand_parts());
    expand_max_levels = saved;
    int n = (int)datas_size();
    drain_datamanager();
    return n;
}

/* Sème `seeds` genèses et en déporte la plupart sur disque, en petits segments. */
static void seed_and_spill(int seeds, int keep)
{
    drain_datamanager();
    datamanager_set_ram_limit_bytes_for_tests(0);
    for (int i = 0; i < seeds; i++) seed_genesis(1);
    set_ram_limit_for_resident((unsigned long long)keep, (unsigned long long)seeds);
    for (int k = 0; k < 50 && datas_size() > (unsigned long long)keep; k++) {
        stock_spill_step(1);
    }
}

/* La part du stock sur disque est développée, pas seulement le pool résident.
 * Avant, une passe ne drainait que la RAM : les genèses déportées restaient au
 * niveau 1, au fond de la pile, sous les enfants que la passe y évinçait.
 * Contre-épreuve : sans source disque, elles reviennent au niveau 1 et le
 * compte n'est pas celui de la référence. */
TEST expansion_develops_the_spilled_stock_too(void)
{
    char tmpl[64];
    char *dir = make_tmp_spill_dir(tmpl);
    ASSERT(dir != NULL);
    stock_spill_configure(dir, nb_file_possibility);
    stock_spill_set_segment_records_for_tests(4);
    request = REQUEST_CONTINUE;

    int expected = expand_reference_count(12, 2, EXPAND_MAX_LEVELS);
    ASSERT(expected > 12);

    seed_and_spill(12, 3);
    ASSERT(stock_spill_total_packets() >= 8ULL);
    ASSERT(stock_spill_total_segments() >= 2ULL);

    datamanager_set_ram_limit_bytes_for_tests(datamanager_bytes_per_possibility() * 8);
    datamanager_set_ram_relief_hook(stock_spill_relieve);
    datamanager_set_expansion_disk_source(&g_spill_source);
    expand_datas_to_level(2, make_expand_free_map(), make_expand_parts());
    datamanager_set_expansion_disk_source(NULL);
    datamanager_set_ram_relief_hook(NULL);

    int allocs[512];
    int n = collect_all_allocs(allocs, 512);
    ASSERT_EQ_FMT(expected, n, "%d");
    for (int i = 0; i < n; i++) {
        ASSERT(allocs[i] >= 2);
    }

    stock_spill_set_segment_records_for_tests(0);
    datamanager_set_ram_limit_packets_for_tests(0);
    drain_datamanager();
    rmdir_recursive(dir);
    PASS();
}

/* Une passe ne lit que ce qui était sur disque AVANT elle : les enfants qu'elle
 * évince vont au-dessus de la frontière et ne sont pas repris. Une seule passe
 * vers le niveau 3 laisse donc tout au niveau 2 exactement. Contre-épreuve :
 * sans frontière, la lecture par le bas atteint ses propres enfants et en
 * développe au niveau 3. */
TEST expansion_pass_never_retakes_its_own_evicted_children(void)
{
    char tmpl[64];
    char *dir = make_tmp_spill_dir(tmpl);
    ASSERT(dir != NULL);
    stock_spill_configure(dir, nb_file_possibility);
    stock_spill_set_segment_records_for_tests(4);
    request = REQUEST_CONTINUE;

    int expected = expand_reference_count(12, 3, 1);

    seed_and_spill(12, 3);
    datamanager_set_ram_limit_bytes_for_tests(datamanager_bytes_per_possibility() * 8);
    datamanager_set_ram_relief_hook(stock_spill_relieve);
    datamanager_set_expansion_disk_source(&g_spill_source);
    int saved = expand_max_levels;
    expand_max_levels = 1;
    expand_datas_to_level(3, make_expand_free_map(), make_expand_parts());
    expand_max_levels = saved;
    datamanager_set_expansion_disk_source(NULL);
    datamanager_set_ram_relief_hook(NULL);

    int allocs[512];
    int n = collect_all_allocs(allocs, 512);
    ASSERT_EQ_FMT(expected, n, "%d");
    for (int i = 0; i < n; i++) {
        ASSERT_EQ_FMT(2, allocs[i], "%d");
    }

    stock_spill_set_segment_records_for_tests(0);
    datamanager_set_ram_limit_packets_for_tests(0);
    drain_datamanager();
    rmdir_recursive(dir);
    PASS();
}

/* Les deux contrats ci-dessus tiennent à plusieurs fils d'expansion : tout le
 * disque antérieur à la passe est développé, rien de ce qu'elle évince n'est
 * repris. Le fil qui relit le disque le fait pendant que les autres insèrent
 * encore : un « pas de place » pour le segment ne clôt la phase disque qu'une
 * fois leurs lots finis. Contre-épreuve : le conclure tout de suite laisse des
 * genèses au niveau 1 dans une partie des essais (mesuré sur le stock de
 * production : 1/3 seulement de la passe traitée à 4 fils sous 400 Mo). Le
 * plafond (32 possibilités) laisse la place d'un segment (4) au-dessus du seuil
 * bas où s'arrête le dégagement (75 %) : à 16, le segment pesait exactement les
 * 25 % libres et un ordre de fils défavorable le laissait illisible jusqu'à la
 * fin de la passe — la limite documentée (docs/utilisation.md), pas une perte. */
TEST expansion_threads_develop_the_spilled_stock_and_never_retake_their_children(void)
{
    char tmpl[64];
    char *dir = make_tmp_spill_dir(tmpl);
    ASSERT(dir != NULL);
    stock_spill_configure(dir, nb_file_possibility);
    stock_spill_set_segment_records_for_tests(4);
    request = REQUEST_CONTINUE;

    /* L'ordre des fils varie d'un essai à l'autre : le scénario est rejoué
     * pour que la contre-épreuve le rencontre (1 essai sur 7 environ seul). */
    for (int trial = 0; trial < 40; trial++)
    for (int level = 2; level <= 3; level++) {
        int max_levels = (level == 2) ? EXPAND_MAX_LEVELS : 1;
        int expected = expand_reference_count(12, level, max_levels);

        seed_and_spill(12, 3);
        datamanager_set_ram_limit_bytes_for_tests(datamanager_bytes_per_possibility() * 32);
        datamanager_set_ram_relief_hook(stock_spill_relieve);
        datamanager_set_expansion_disk_source(&g_spill_source);
        datamanager_set_expand_threads(4);
        int saved = expand_max_levels;
        expand_max_levels = max_levels;
        expand_datas_to_level(level, make_expand_free_map(), make_expand_parts());
        expand_max_levels = saved;
        datamanager_set_expand_threads(1);
        datamanager_set_expansion_disk_source(NULL);
        datamanager_set_ram_relief_hook(NULL);

        int allocs[512];
        int n = collect_all_allocs(allocs, 512);
        ASSERT_EQ_FMT(expected, n, "%d");
        for (int i = 0; i < n; i++) {
            ASSERT_EQ_FMT(2, allocs[i], "%d"); /* niveau 2 visé, ou une seule passe vers 3 */
        }
        datamanager_set_ram_limit_packets_for_tests(0);
        drain_datamanager();
    }

    stock_spill_set_segment_records_for_tests(0);
    rmdir_recursive(dir);
    PASS();
}

/* Collecteur de marqueurs pour appeler stock_spill_expansion_take directement. */
typedef struct { int markers[64]; int develop[64]; int n; int fail_at; } take_sink_t;
static int take_sink(const struct possibility_packet *p, int develop, void *ctx)
{
    take_sink_t *t = ctx;
    if (t->n == t->fail_at) {
        return 0;
    }
    t->develop[t->n] = develop;
    t->markers[t->n++] = p->grid[0][0];
    return 1;
}

/* Lecture par le bas : le segment le plus ancien d'abord, rien supprimé sur
 * échec (peek puis commit), le segment de frontière lu en entier — son ancien
 * contenu à développer, ce que la passe y a ajouté à rendre tel quel — et plus
 * rien ensuite, même quand la pile vidée repart à 1. */
TEST expansion_take_reads_bottom_first_and_commits_only_on_success(void)
{
    char tmpl[64];
    char *dir = make_tmp_spill_dir(tmpl);
    ASSERT(dir != NULL);
    stock_spill_configure(dir, nb_file_possibility);
    stock_spill_set_segment_records_for_tests(4);
    drain_datamanager();
    datamanager_set_ram_limit_packets_for_tests(0);
    int allocs[10];
    for (int i = 0; i < 10; i++) allocs[i] = i + 1;
    add_packets(allocs, 10);
    datamanager_set_ram_limit_bytes_for_tests(1);
    ASSERT_EQ_FMT(10, stock_spill_step(10), "%d");     /* 1..4 | 5..8 | 9,10 */

    stock_spill_expansion_begin();
    /* Place insuffisante : rien lu. Échec du collecteur : rien supprimé. */
    take_sink_t t = { {0}, {0}, 0, -1 };
    ASSERT_EQ_FMT(DATAMANAGER_DISK_TAKE_NO_ROOM, stock_spill_expansion_take(take_sink, &t, 3), "%d");
    t.fail_at = 2;
    ASSERT_EQ_FMT(-1, stock_spill_expansion_take(take_sink, &t, 100), "%d");
    ASSERT_EQ_FMT(10ULL, stock_spill_total_packets(), "%llu");

    /* Le bas d'abord : 1..4, puis 5..8. */
    take_sink_t a = { {0}, {0}, 0, -1 };
    ASSERT_EQ_FMT(4, stock_spill_expansion_take(take_sink, &a, 100), "%d");
    for (int i = 0; i < 4; i++) ASSERT_EQ_FMT(i + 1, a.markers[i], "%d");
    ASSERT_EQ_FMT(2ULL, stock_spill_total_segments(), "%llu");

    /* Une éviction pendant la passe complète le sommet (9, 10 + 41). */
    datamanager_set_ram_limit_packets_for_tests(0);
    datamanager_reset_rr_state_for_tests();
    int fresh[1] = { 41 };
    add_packets(fresh, 1);
    datamanager_set_ram_limit_bytes_for_tests(1);
    ASSERT_EQ_FMT(1, stock_spill_step(1), "%d");
    take_sink_t b = { {0}, {0}, 0, -1 };
    ASSERT_EQ_FMT(4, stock_spill_expansion_take(take_sink, &b, 100), "%d");
    ASSERT_EQ_FMT(5, b.markers[0], "%d");
    take_sink_t c = { {0}, {0}, 0, -1 };
    ASSERT_EQ_FMT(3, stock_spill_expansion_take(take_sink, &c, 100), "%d");
    ASSERT_EQ_FMT(9, c.markers[0], "%d");
    ASSERT_EQ_FMT(1, c.develop[0], "%d");
    ASSERT_EQ_FMT(1, c.develop[1], "%d");
    ASSERT_EQ_FMT(41, c.markers[2], "%d");
    ASSERT_EQ_FMT(0, c.develop[2], "%d");                  /* enfant de la passe */
    ASSERT_EQ_FMT(0ULL, stock_spill_total_packets(), "%llu");

    /* Pile vidée : une nouvelle éviction repart au segment 1, jamais relu. */
    datamanager_set_ram_limit_packets_for_tests(0);
    datamanager_reset_rr_state_for_tests();
    int more[1] = { 42 };
    add_packets(more, 1);
    datamanager_set_ram_limit_bytes_for_tests(1);
    ASSERT_EQ_FMT(1, stock_spill_step(1), "%d");
    take_sink_t d = { {0}, {0}, 0, -1 };
    ASSERT_EQ_FMT(0, stock_spill_expansion_take(take_sink, &d, 100), "%d");
    stock_spill_expansion_end();
    ASSERT_EQ_FMT(1ULL, stock_spill_total_packets(), "%llu");

    stock_spill_set_segment_records_for_tests(0);
    datamanager_set_ram_limit_packets_for_tests(0);
    drain_datamanager();
    rmdir_recursive(dir);
    PASS();
}

/* Une pile consommée par le bas commence au-dessus de 1 (`first_seq` > 1) : le
 * cliché la renumérote à partir de 1, et la restauration rend exactement ce qui
 * restait. Contre-épreuve : sans renumérotation, le cliché écrit 2..3 sous un
 * manifeste qui en annonce 2, et la restauration échoue. */
TEST snapshot_of_a_stack_consumed_from_the_bottom_restores_exactly(void)
{
    char tmpl[64];
    char *dir = make_tmp_spill_dir(tmpl);
    ASSERT(dir != NULL);
    stock_spill_configure(dir, nb_file_possibility);
    stock_spill_set_segment_records_for_tests(4);
    drain_datamanager();
    datamanager_set_ram_limit_packets_for_tests(0);
    int allocs[12];
    for (int i = 0; i < 12; i++) allocs[i] = i + 1;
    add_packets(allocs, 12);
    datamanager_set_ram_limit_bytes_for_tests(1);
    ASSERT_EQ_FMT(12, stock_spill_step(12), "%d");      /* 1..4 | 5..8 | 9..12 */

    stock_spill_expansion_begin();
    take_sink_t a = { {0}, {0}, 0, -1 };
    ASSERT_EQ_FMT(4, stock_spill_expansion_take(take_sink, &a, 100), "%d");
    stock_spill_expansion_end();

    ASSERT_EQ_FMT(8ULL, stock_spill_snapshot("snap"), "%llu");
    ASSERT_EQ_FMT(8ULL, stock_spill_restore_snapshot("snap"), "%llu");

    datamanager_set_ram_limit_bytes_for_tests(1ULL << 30);
    for (int k = 0; k < 20 && stock_spill_total_packets() > 0; k++) {
        stock_spill_step(100);
    }
    int seen[64];
    int n = drain_and_collect_markers(seen, 64);
    ASSERT_EQ_FMT(8, n, "%d");
    int count[64] = {0};
    for (int i = 0; i < n; i++) count[seen[i]]++;
    for (int m = 5; m <= 12; m++) ASSERT_EQ_FMT(1, count[m], "%d");

    stock_spill_set_segment_records_for_tests(0);
    datamanager_set_ram_limit_packets_for_tests(0);
    drain_datamanager();
    rmdir_recursive(dir);
    PASS();
}

/* Les cumuls d'éviction et de rechargement que lit le point d'avancement de
 * l'expansion suivent ce que le débordement déplace réellement. */
TEST expansion_stats_count_evictions_and_reloads(void)
{
    char tmpl[64];
    char *dir = make_tmp_spill_dir(tmpl);
    ASSERT(dir != NULL);
    stock_spill_configure(dir, nb_file_possibility);
    drain_datamanager();
    datamanager_set_ram_limit_packets_for_tests(0);
    int allocs[10];
    for (int i = 0; i < 10; i++) allocs[i] = i + 1;
    add_packets(allocs, 10);

    datamanager_spill_stats_t before, after;
    stock_spill_expansion_stats(&before);
    datamanager_set_ram_limit_bytes_for_tests(1);
    ASSERT_EQ_FMT(4, stock_spill_step(4), "%d");
    ASSERT_EQ_FMT(2, stock_spill_step(2), "%d"); /* trame du haut : celle que recharge le pas suivant */
    int seen[64];
    drain_and_collect_markers(seen, 64);
    datamanager_set_ram_limit_bytes_for_tests(1ULL << 30);
    ASSERT_EQ_FMT(2, stock_spill_step(2), "%d");
    stock_spill_expansion_stats(&after);

    ASSERT_EQ_FMT(6ULL, after.evicted_total - before.evicted_total, "%llu");
    ASSERT_EQ_FMT(2ULL, after.reloaded_total - before.reloaded_total, "%llu");
    ASSERT_EQ_FMT(4ULL, after.spilled, "%llu");

    datamanager_set_ram_limit_packets_for_tests(0);
    drain_datamanager();
    rmdir_recursive(dir);
    PASS();
}

/* ---------------------------------------------------------------------- */
/* Sauvegarde AUTONOME (consistent_backup_self_contained) : le débordement
 * disque est recopié DANS le .back, qui se restaure seul. */

/* Déporte TOUT le stock résident (plafond d'une possibilité, pas de 4096). */
static int spill_everything(unsigned long long expected)
{
    datamanager_set_ram_limit_packets_for_tests(1);
    int rounds = 0;
    while (stock_spill_total_packets() < expected && rounds < 60) {
        stock_spill_step(4096);
        rounds++;
    }
    datamanager_set_ram_limit_packets_for_tests(0);
    return stock_spill_total_packets() == expected;
}

/* Recharge tout le débordement en RAM (plafond large). */
static void reload_everything(void)
{
    datamanager_set_ram_limit_packets_for_tests(100000);
    int rounds = 0;
    while (stock_spill_total_packets() > 0ULL && rounds < 200) {
        stock_spill_step(4096);
        rounds++;
    }
    datamanager_set_ram_limit_packets_for_tests(0);
}

static int dir_has_entry_with_prefix(const char *dir, const char *prefix)
{
    DIR *d = opendir(dir);
    if (d == NULL) {
        return 0;
    }
    int found = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strncmp(entry->d_name, prefix, strlen(prefix)) == 0) {
            found = 1;
        }
    }
    closedir(d);
    return found;
}

/* Le cas qui motive la sauvegarde autonome : le .back est restauré sur une
 * AUTRE machine (autre --stock-spill-dir, sans le moindre cliché), sous un
 * plafond RAM PLUS BAS, et ce --stock-spill-dir porte déjà un débordement
 * vivant étranger. Tout revient — rien de moins (le débordement était dans le
 * fichier), rien de plus (le débordement vivant étranger a été vidé, pas
 * fusionné) — et le surplus repart sur disque sans rien perdre. */
TEST self_contained_backup_restores_alone_elsewhere_under_a_lower_cap(void)
{
    char tmpl[64];
    char *dir = make_tmp_spill_dir(tmpl);
    ASSERT(dir != NULL);
    char tmpl2[64];
    char *dir2 = make_tmp_spill_dir(tmpl2);
    ASSERT(dir2 != NULL);

    /* 1. 20 possibilités déportées (plusieurs segments), puis 5 résidentes. */
    drain_datamanager();
    datamanager_set_ram_limit_packets_for_tests(0);
    stock_spill_configure(dir, nb_file_possibility);
    stock_spill_set_segment_records_for_tests(4);
    int allocs[20];
    for (int i = 0; i < 20; i++) { allocs[i] = i + 1; }
    add_packets(allocs, 20);
    ASSERT(spill_everything(20ULL));
    int more[5] = { 21, 22, 23, 24, 25 };
    add_packets(more, 5);
    ASSERT_EQ_FMT(5ULL, datas_size(), "%llu");
    unsigned long long cap_bytes = datamanager_resident_bytes();

    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/stock.back", dir2);
    char path_an[PATH_MAX];
    snprintf(path_an, sizeof path_an, "%s/analysed.back", dir2);
    /* Un .spillcount d'une sauvegarde précédente de ce nom : il ne décrit plus
     * rien une fois le fichier autonome publié, et doit disparaître. */
    char sidecar[PATH_MAX + 16];
    snprintf(sidecar, sizeof sidecar, "%s.spillcount", path);
    FILE *stale = fopen(sidecar, "w");
    ASSERT(stale != NULL);
    fprintf(stale, "999\nsnapshot\n");
    fclose(stale);

    int rba = -99;
    int rb = consistent_backup_self_contained(path, path_an, &rba, stock_spill_snapshot,
                                              stock_spill_embed_snapshot);
    ASSERT_EQ_FMT(BACKUP_OK, rb, "%d");
    ASSERT_EQ_FMT(BACKUP_OK, rba, "%d");
    ASSERT_EQ_FMT(1, datamanager_backup_is_complete(path), "%d");
    ASSERT_EQ_FMT(-1, access(sidecar, F_OK), "%d");
    /* Le cliché temporaire a été recopié puis supprimé. */
    ASSERT_EQ_FMT(0, dir_has_entry_with_prefix(dir, CONSISTENT_BACKUP_EMBED_PREFIX), "%d");
    /* La sauvegarde n'a rien pris au stock vivant. */
    ASSERT_EQ_FMT(20ULL, stock_spill_total_packets(), "%llu");
    ASSERT_EQ_FMT(5ULL, datas_size(), "%llu");

    /* 2. « Autre machine » : autre répertoire de débordement, qui porte un
     *    débordement vivant étranger (marqueurs 40..46). */
    drain_datamanager();
    stock_spill_configure(dir2, nb_file_possibility);
    stock_spill_set_segment_records_for_tests(4);
    int foreign[7] = { 40, 41, 42, 43, 44, 45, 46 };
    add_packets(foreign, 7);
    ASSERT(spill_everything(7ULL));

    /* 3. Restauration sous un plafond de 5 possibilités, comme restore_apply. */
    datamanager_set_ram_relief_hook(stock_spill_relieve);
    datamanager_set_ram_limit_bytes_for_tests(cap_bytes);
    capture_stderr();
    datamanager_begin_maintenance();
    int prep = stock_spill_prepare_restore(path);
    int rc = restore(path);
    datamanager_end_maintenance();
    (void)restore_stderr_size();
    ASSERT_EQ_FMT(0, prep, "%d");
    ASSERT_EQ_FMT(0, rc, "%d");
    ASSERT_EQ_FMT(25ULL, datas_size() + stock_spill_total_packets(), "%llu");
    ASSERT(stock_spill_total_packets() >= 20ULL); /* le surplus est reparti sur disque */

    /* 4. Exactement les 25 d'origine, aucune étrangère. */
    datamanager_set_ram_relief_hook(NULL);
    reload_everything();
    ASSERT_EQ_FMT(0ULL, stock_spill_total_packets(), "%llu");
    int markers[32];
    int n = collect_markers(markers, 32);
    ASSERT_EQ_FMT(25, n, "%d");
    qsort(markers, (size_t)n, sizeof(int), int_cmp);
    for (int i = 0; i < n; i++) {
        ASSERT_EQ_FMT(i + 1, markers[i], "%d");
    }

    datamanager_set_ram_limit_packets_for_tests(0);
    drain_datamanager();
    stock_spill_configure(dir, nb_file_possibility);
    rmdir_recursive(dir);
    rmdir_recursive(dir2);
    PASS();
}

/* Un segment du cliché qui manque au moment de la recopie : la recopie
 * échoue (jamais un .back qui se dirait complet sans l'être), le compte
 * écrit dit ce qui a réellement été écrit, et le cliché est supprimé. */
TEST embed_snapshot_refuses_a_snapshot_with_a_missing_segment(void)
{
    char tmpl[64];
    char *dir = make_tmp_spill_dir(tmpl);
    ASSERT(dir != NULL);
    drain_datamanager();
    datamanager_set_ram_limit_packets_for_tests(0);
    stock_spill_configure(dir, nb_file_possibility);
    stock_spill_set_segment_records_for_tests(4);
    int allocs[10];
    for (int i = 0; i < 10; i++) { allocs[i] = i + 1; }
    add_packets(allocs, 10);
    ASSERT(spill_everything(10ULL));
    ASSERT_EQ_FMT(10ULL, stock_spill_snapshot("snap"), "%llu");

    /* Un segment PLEIN du cliché disparaît. */
    char snap_dir[PATH_MAX];
    snprintf(snap_dir, sizeof snap_dir, "%s/snap", dir);
    DIR *d = opendir(snap_dir);
    ASSERT(d != NULL);
    struct dirent *entry;
    char victim[PATH_MAX + 300] = "";
    while ((entry = readdir(d)) != NULL) {
        if (strstr(entry->d_name, "_1.dat") != NULL && victim[0] == '\0') {
            snprintf(victim, sizeof victim, "%s/%s", snap_dir, entry->d_name);
        }
    }
    closedir(d);
    ASSERT(victim[0] != '\0');
    ASSERT_EQ_FMT(0, unlink(victim), "%d");

    FILE *out = tmpfile();
    ASSERT(out != NULL);
    unsigned long long written = 0;
    capture_stderr();
    int rc = stock_spill_embed_snapshot("snap", out, &written);
    long err = restore_stderr_size();
    fclose(out);
    ASSERT_EQ_FMT(-1, rc, "%d");
    ASSERT(written < 10ULL);
    ASSERT(err > 0); /* signalé, jamais silencieux */
    ASSERT_EQ_FMT(-1, access(snap_dir, F_OK), "%d");

    drain_datamanager();
    stock_spill_configure(dir, nb_file_possibility);
    rmdir_recursive(dir);
    PASS();
}

/* Un .back NON autonome (autobackup) se restaure avec le cliché que nomme SON
 * .spillcount — « snapshot-temp » pour temp.back —, jamais avec le cliché
 * « snapshot » d'une autre sauvegarde, qui porte un autre compte. */
TEST prepare_restore_reads_the_snapshot_named_by_the_sidecar(void)
{
    char tmpl[64];
    char *dir = make_tmp_spill_dir(tmpl);
    ASSERT(dir != NULL);
    drain_datamanager();
    datamanager_set_ram_limit_packets_for_tests(0);
    stock_spill_configure(dir, nb_file_possibility);
    stock_spill_set_segment_records_for_tests(4);
    int allocs[10];
    for (int i = 0; i < 10; i++) { allocs[i] = i + 1; }
    add_packets(allocs, 10);
    ASSERT(spill_everything(10ULL));
    /* Cliché « snapshot » d'une autre sauvegarde : 10 possibilités. */
    ASSERT_EQ_FMT(10ULL, stock_spill_snapshot(CONSISTENT_BACKUP_DEFAULT_SNAPSHOT), "%llu");

    /* Rechargement partiel : le débordement n'en compte plus que 6. */
    datamanager_set_ram_limit_packets_for_tests(100000);
    stock_spill_step(4);
    datamanager_set_ram_limit_packets_for_tests(0);
    unsigned long long spilled = stock_spill_total_packets();
    ASSERT(spilled < 10ULL && spilled > 0ULL);

    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/temp.back", dir);
    char path_an[PATH_MAX];
    snprintf(path_an, sizeof path_an, "%s/temp_analysed.back", dir);
    int rba = -99;
    ASSERT_EQ_FMT(BACKUP_OK, consistent_backup(path, path_an, &rba, "snapshot-temp", stock_spill_snapshot), "%d");
    ASSERT_EQ_FMT(0, datamanager_backup_is_complete(path), "%d");

    /* Redémarrage puis restauration. */
    drain_datamanager();
    stock_spill_configure(dir, nb_file_possibility);
    stock_spill_set_segment_records_for_tests(4);
    capture_stderr();
    int prep = stock_spill_prepare_restore(path);
    long err = restore_stderr_size();
    ASSERT_EQ_FMT(0, prep, "%d");
    ASSERT_EQ_FMT(0L, err, "%ld");
    ASSERT_EQ_FMT(spilled, stock_spill_total_packets(), "%llu");

    drain_datamanager();
    stock_spill_configure(dir, nb_file_possibility);
    rmdir_recursive(dir);
    PASS();
}

/* Un cliché temporaire de sauvegarde autonome qu'un arrêt brutal a laissé
 * n'appartient à aucun fichier publié : purgé au démarrage. Un cliché
 * ordinaire (« snapshot »), lui, reste — un restore peut encore en avoir
 * besoin. */
TEST configure_purges_leftover_embed_snapshots(void)
{
    char tmpl[64];
    char *dir = make_tmp_spill_dir(tmpl);
    ASSERT(dir != NULL);
    char embed_dir[PATH_MAX];
    snprintf(embed_dir, sizeof embed_dir, "%s/%s123-4", dir, CONSISTENT_BACKUP_EMBED_PREFIX);
    char keep_dir[PATH_MAX];
    snprintf(keep_dir, sizeof keep_dir, "%s/%s", dir, CONSISTENT_BACKUP_DEFAULT_SNAPSHOT);
    ASSERT_EQ_FMT(0, mkdir(embed_dir, 0755), "%d");
    ASSERT_EQ_FMT(0, mkdir(keep_dir, 0755), "%d");
    char p[PATH_MAX + 32];
    snprintf(p, sizeof p, "%s/spill_u_0_1.dat", embed_dir);
    write_stride_segment(p, 1, 2);
    snprintf(p, sizeof p, "%s/manifest.txt", embed_dir);
    FILE *f = fopen(p, "w");
    ASSERT(f != NULL);
    fputs("eternityii-spill-manifest-v2\n", f);
    fclose(f);
    snprintf(p, sizeof p, "%s/manifest.txt", keep_dir);
    f = fopen(p, "w");
    ASSERT(f != NULL);
    fputs("eternityii-spill-manifest-v2\n", f);
    fclose(f);

    stock_spill_configure(dir, nb_file_possibility);
    ASSERT_EQ_FMT(-1, access(embed_dir, F_OK), "%d");
    ASSERT_EQ_FMT(0, access(p, F_OK), "%d");

    rmdir_recursive(dir);
    PASS();
}

/* ====================================================================== */
/* Étage RAM en blocs (docs/conception/etage_ram_compresse.md) : la chaîne
 * liste -> étage -> disque, pilotée par stock_spill_step. Les tests
 * historiques ci-dessus vérifient la mécanique disque avec l'étage coupé
 * (liste -> disque direct) ; ceux-ci l'activent. */

void lock_all_file(void);
void unlock_all_file(void);
void stock_spill_set_tier_enabled_for_tests(int enabled);
void stock_spill_set_hot_buffer_for_tests(int hot_max, int hot_min);
void stock_spill_set_tier_disk_factor_for_tests(int factor);

/* `n` possibilités d'une seule case, marquées MARK_BASE+first..+n-1 dans
 * l'ordre d'ajout (la première est la plus ancienne, donc la tête froide),
 * dans le pool vérifié si `checked`. */
static void add_marked_pool(int first, int n, int checked)
{
    array_possibility_packet arr;
    arr.size = n;
    arr.possibilities = calloc((size_t)n, sizeof(struct possibility_packet));
    for (int i = 0; i < n; i++) {
        init_empty_grid(&arr.possibilities[i]);
        arr.possibilities[i].grid[0][0] = (int16_t)(MARK_BASE + first + i);
        arr.possibilities[i].alloc = 1;
        arr.possibilities[i].checked = (uint8_t)(checked ? 1 : 0);
    }
    add_possibility(NULL, &arr);
    free(arr.possibilities);
}

static void add_marked(int first, int n)
{
    add_marked_pool(first, n, 0);
}

/* Possibilités de la liste d'un pool (toutes files confondues). */
static unsigned long long list_size_of_pool(int checked)
{
    unsigned long long n = 0;
    for (int f = 0; f < nb_file_possibility; f++) {
        n += checked ? file_checked_size(f) : file_size(f);
    }
    return n;
}

/* Vide la liste et range ses marqueurs (relatifs à MARK_BASE) triés. */
static int list_markers_sorted(int *out, int max)
{
    int n = collect_markers(out, max);
    for (int i = 0; i < n; i++) {
        out[i] -= MARK_BASE;
    }
    qsort(out, (size_t)n, sizeof(int), int_cmp);
    return n;
}

static void tier_test_begin(char *tmpl, const char **dir_out)
{
    stock_spill_set_tier_enabled_for_tests(1);
    /* Les tests de l'ordre de pile comptent un transfert disque au budget du
     * pas ; le lot de production est couvert par
     * tier_disk_transfer_moves_a_whole_batch_per_step. */
    stock_spill_set_tier_disk_factor_for_tests(1);
    *dir_out = make_tmp_spill_dir(tmpl);
    stock_spill_configure(*dir_out, nb_file_possibility);
    stock_spill_configure_tier(STOCK_TIER_HOT_MAX_DEFAULT, STOCK_TIER_HOT_MIN_DEFAULT);
    drain_datamanager();
    datamanager_set_ram_limit_packets_for_tests(0);
}

static void tier_test_end(const char *dir)
{
    datamanager_set_ram_limit_packets_for_tests(0);
    drain_datamanager();
    stock_spill_configure(dir, nb_file_possibility); /* vide l'étage */
    stock_spill_configure_tier(STOCK_TIER_HOT_MAX_DEFAULT, STOCK_TIER_HOT_MIN_DEFAULT);
    rmdir_recursive(dir);
    stock_spill_set_tier_disk_factor_for_tests(0);
}

/* Au-dessus du seuil haut, la tête froide de la liste part dans l'étage — pas
 * sur disque — et l'étage compte dans l'occupation, en moins cher. */
TEST tier_eviction_moves_the_cold_head_into_ram_blocks(void)
{
    char tmpl[64];
    const char *dir;
    tier_test_begin(tmpl, &dir);
    ASSERT(dir != NULL);
    add_marked(0, 30);
    unsigned long long before = datamanager_resident_bytes();

    datamanager_set_ram_limit_bytes_for_tests(1);
    ASSERT_EQ_FMT(10, stock_spill_step(10), "%d");
    ASSERT_EQ_FMT(10ULL, stock_spill_tier_packets(), "%llu");
    ASSERT_EQ_FMT(20ULL, datas_size(), "%llu");
    ASSERT_EQ_FMT(0ULL, stock_spill_total_packets(), "%llu");

    /* Comptée : occupation = liste + étage, et l'étage coûte moins que les
     * maillons qu'il remplace. */
    ASSERT(stock_spill_tier_bytes() > 0ULL);
    ASSERT_EQ_FMT(datamanager_pools_resident_bytes() + stock_spill_tier_bytes(),
                  datamanager_resident_bytes(), "%llu");
    ASSERT(datamanager_resident_bytes() < before);

    /* Ce sont les plus ANCIENNES qui sont parties : il reste 10..29. */
    datamanager_set_ram_limit_packets_for_tests(0);
    int m[64];
    ASSERT_EQ_FMT(20, list_markers_sorted(m, 64), "%d");
    for (int i = 0; i < 20; i++) {
        ASSERT_EQ_FMT(10 + i, m[i], "%d");
    }
    tier_test_end(dir);
    PASS();
}

/* Le rechargement suit la LISTE (sous --stock-hot-min), pas le total, et
 * remonte le bloc le plus RÉCENT de l'étage en premier. */
TEST tier_reload_returns_the_newest_block_first(void)
{
    char tmpl[64];
    const char *dir;
    tier_test_begin(tmpl, &dir);
    ASSERT(dir != NULL);
    add_marked(0, 30);
    datamanager_set_ram_limit_bytes_for_tests(1);
    ASSERT_EQ_FMT(10, stock_spill_step(10), "%d");   /* bloc 0..9 */
    ASSERT_EQ_FMT(10, stock_spill_step(10), "%d");   /* bloc 10..19 */
    ASSERT_EQ_FMT(20ULL, stock_spill_tier_packets(), "%llu");

    /* Plafond large, tampon de 15 à 24 : la liste (10 possibilités) est sous
     * son minimum — un bloc remonte, le plus récent, et le rechargement
     * s'arrête au milieu (19). */
    stock_spill_set_hot_buffer_for_tests(24, 15);
    datamanager_set_ram_limit_bytes_for_tests(1ULL << 30);
    ASSERT_EQ_FMT(10, stock_spill_step(10), "%d");
    ASSERT_EQ_FMT(10ULL, stock_spill_tier_packets(), "%llu");
    int m[64];
    ASSERT_EQ_FMT(20, list_markers_sorted(m, 64), "%d");
    for (int i = 0; i < 20; i++) {
        ASSERT_EQ_FMT(10 + i, m[i], "%d");
    }
    tier_test_end(dir);
    PASS();
}

/* L'étage à lui seul dépasse 25 % du plafond et la liste est vide : le
 * rechargement doit se déclencher quand même. Jugé sur l'occupation TOTALE
 * (comme le disque), il ne partirait jamais — les clients recevraient 0
 * possibilité avec un étage plein. */
TEST tier_reload_is_driven_by_the_list_not_the_total(void)
{
    char tmpl[64];
    const char *dir;
    tier_test_begin(tmpl, &dir);
    ASSERT(dir != NULL);
    add_marked(0, 30);
    datamanager_set_ram_limit_bytes_for_tests(1);
    ASSERT_EQ_FMT(20, stock_spill_step(20), "%d");
    datamanager_set_ram_limit_packets_for_tests(0);
    int m[64];
    ASSERT_EQ_FMT(10, collect_markers(m, 64), "%d");   /* des GET vident la liste */

    /* Étage = un tiers du plafond : au-dessus des 25 % du disque. */
    datamanager_set_ram_limit_bytes_for_tests(stock_spill_tier_bytes() * 3);
    ASSERT(stock_spill_step(10) > 0);
    ASSERT(datas_size() > 0ULL);
    ASSERT_EQ_FMT(20ULL, datas_size() + stock_spill_tier_packets(), "%llu");
    tier_test_end(dir);
    PASS();
}

/* La liste à son plancher, c'est le BAS de l'étage (le plus ancien) qui part
 * sur disque ; au rechargement, l'étage d'abord (du plus récent au plus
 * ancien), le disque seulement une fois l'étage vide : l'ordre de pile du
 * stock est conservé de bout en bout. */
TEST tier_overflow_goes_to_disk_from_the_bottom_and_comes_back_in_order(void)
{
    char tmpl[64];
    const char *dir;
    tier_test_begin(tmpl, &dir);
    ASSERT(dir != NULL);
    add_marked(0, 30);
    datamanager_set_ram_limit_bytes_for_tests(1);
    ASSERT_EQ_FMT(10, stock_spill_step(10), "%d");   /* étage : 0..9 */
    ASSERT_EQ_FMT(10, stock_spill_step(10), "%d");   /* étage : 0..9 | 10..19 */
    ASSERT_EQ_FMT(10, stock_spill_step(10), "%d");   /* étage : 0..9 | 10..19 | 20..29 */
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");
    ASSERT_EQ_FMT(10, stock_spill_step(10), "%d");   /* liste vide : 0..9 part sur disque */
    ASSERT_EQ_FMT(10ULL, stock_spill_total_packets(), "%llu");
    ASSERT_EQ_FMT(20ULL, stock_spill_tier_packets(), "%llu");

    /* Tampon de 5 à 12 : chaque rechargement s'arrête après un bloc. */
    stock_spill_set_hot_buffer_for_tests(12, 5);
    datamanager_set_ram_limit_bytes_for_tests(1ULL << 30);
    int m[64];
    for (int round = 0; round < 3; round++) {
        ASSERT_EQ_FMT(10, stock_spill_step(10), "%d");
        ASSERT_EQ_FMT(10, list_markers_sorted(m, 64), "%d");
        int first = 20 - 10 * round;
        for (int i = 0; i < 10; i++) {
            ASSERT_EQ_FMT(first + i, m[i], "%d");
        }
    }
    ASSERT_EQ_FMT(0ULL, stock_spill_tier_packets(), "%llu");
    ASSERT_EQ_FMT(0ULL, stock_spill_total_packets(), "%llu");
    tier_test_end(dir);
    PASS();
}

/* Un bloc de l'étage part sur disque TEL QUEL : la trame porte ses octets
 * stockés (compressés sous `make ZSTD=1`), sans décodage ni recompression. Le
 * disque n'est plus au pas fixe de PACKET_CODEC_MAX_BYTES par possibilité
 * (390 octets en 16x16) : il coûte au plus la forme compacte, et sous zstd
 * bien moins. */
TEST tier_blocks_reach_the_disk_in_their_stored_form(void)
{
    char tmpl[64];
    const char *dir;
    tier_test_begin(tmpl, &dir);
    ASSERT(dir != NULL);
    enum { N = 400 };
    array_possibility_packet arr;
    arr.size = N;
    arr.possibilities = calloc((size_t)N, sizeof(struct possibility_packet));
    for (int i = 0; i < N; i++) {
        init_empty_grid(&arr.possibilities[i]);
        arr.possibilities[i].grid[0][0] = (int16_t)(1 + i % 30);
    }
    add_possibility(NULL, &arr);
    free(arr.possibilities);

    datamanager_set_ram_limit_bytes_for_tests(1);
    for (int k = 0; k < 20 && datas_size() > 0; k++) stock_spill_step(4096);  /* liste -> étage */
    for (int k = 0; k < 20 && stock_spill_tier_packets() > 0; k++) stock_spill_step(4096); /* étage -> disque */
    ASSERT_EQ_FMT((unsigned long long)N, stock_spill_total_packets(), "%llu");

    long records = 0, bytes = 0;
    int codecs = 0;
    DIR *d = opendir(dir);
    ASSERT(d != NULL);
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "spill_", 6) != 0) continue;
        char path[PATH_MAX + 300];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        int c = 0;
        long r = framed_segment_records(path, &c);
        ASSERT(r > 0);
        struct stat st;
        ASSERT_EQ_FMT(0, stat(path, &st), "%d");
        records += r;
        bytes += (long)st.st_size;
        codecs |= c;
    }
    closedir(d);
    ASSERT_EQ_FMT((long)N, records, "%ld");
    /* Forme compacte d'un plateau à une case : en-tête + bitmap + une valeur. */
    long compact = (long)N * (PACKET_CODEC_HEADER_BYTES + PACKET_CODEC_BITMAP_BYTES + 2);
#ifdef ETII_ZSTD
    ASSERT(codecs & (1 << STOCK_TIER_CODEC_ZSTD));
    ASSERT(bytes * 4 < compact);
#else
    ASSERT_EQ_FMT(1 << STOCK_TIER_CODEC_RAW, codecs, "%d");
    ASSERT(bytes <= compact + 32L * N);
#endif
    ASSERT(bytes < (long)N * (long)PACKET_CODEC_MAX_BYTES);

    datamanager_set_ram_limit_bytes_for_tests(1ULL << 30);
    for (int k = 0; k < 50 && stock_spill_total_packets() > 0; k++) stock_spill_step(4096);
    ASSERT_EQ_FMT((unsigned long long)N, datas_size(), "%llu");
    tier_test_end(dir);
    PASS();
}

/* Un bloc de l'étage est une trame INSÉCABLE : quand il ne tient plus dans le
 * segment entamé, la pile roule. Le segment quitté devient immuable, et quand
 * il redevient sommet ses compteurs sont repris sur le disque (taille du
 * fichier, trames) — il doit donc avoir été ramené à son sommet logique AVANT
 * de rouler. Ici ce sommet a reculé (rechargement de la trame [2,3]) sans que
 * le fichier change. Contre-épreuve : sans ce recalage, 2 et 3, déjà servis,
 * reviennent une seconde fois. */
TEST tier_rollover_after_a_partial_reload_trims_the_left_segment(void)
{
    char tmpl[64];
    const char *dir;
    tier_test_begin(tmpl, &dir);
    ASSERT(dir != NULL);
    stock_spill_set_segment_records_for_tests(4);
    add_marked(0, 4);
    datamanager_set_ram_limit_bytes_for_tests(1);
    ASSERT_EQ_FMT(2, stock_spill_step(2), "%d");   /* étage : [0,1] */
    ASSERT_EQ_FMT(2, stock_spill_step(2), "%d");   /* étage : [0,1] [2,3] */
    ASSERT_EQ_FMT(2, stock_spill_step(2), "%d");   /* segment 1 : [0,1] */
    ASSERT_EQ_FMT(2, stock_spill_step(2), "%d");   /* segment 1 : [0,1] [2,3] */
    ASSERT_EQ_FMT(4ULL, stock_spill_total_packets(), "%llu");

    stock_spill_set_hot_buffer_for_tests(3, 1);    /* une trame par rechargement */
    datamanager_set_ram_limit_bytes_for_tests(1ULL << 30);
    ASSERT_EQ_FMT(2, stock_spill_step(2), "%d");   /* 2, 3 reviennent */
    int m[64];
    ASSERT_EQ_FMT(2, list_markers_sorted(m, 64), "%d");
    ASSERT_EQ_FMT(2, m[0], "%d");

    datamanager_reset_rr_state_for_tests(); /* même file 0 que le segment entamé */
    add_marked(10, 3);
    datamanager_set_ram_limit_bytes_for_tests(1);
    ASSERT_EQ_FMT(3, stock_spill_step(3), "%d");   /* étage : [10,11,12] */
    ASSERT_EQ_FMT(3, stock_spill_step(3), "%d");   /* 2 + 3 > 4 : segment 2 */
    ASSERT_EQ_FMT(2ULL, stock_spill_total_segments(), "%llu");

    stock_spill_configure_tier(STOCK_TIER_HOT_MAX_DEFAULT, STOCK_TIER_HOT_MIN_DEFAULT);
    datamanager_set_ram_limit_bytes_for_tests(1ULL << 30);
    for (int k = 0; k < 20 && stock_spill_total_packets() > 0; k++) {
        stock_spill_step(100);
    }
    int n = list_markers_sorted(m, 64);
    ASSERT_EQ_FMT(5, n, "%d");
    int expected[5] = { 0, 1, 10, 11, 12 };
    for (int i = 0; i < 5; i++) {
        ASSERT_EQ_FMT(expected[i], m[i], "%d");
    }
    stock_spill_set_segment_records_for_tests(0);
    tier_test_end(dir);
    PASS();
}

/* Au-dessus du seuil haut, la liste descend à son tampon — ici sa borne en
 * octets, un petit plafond — et pas plus bas tant que le disque peut prendre
 * le trop-plein : elle n'est pas vidée d'un coup. */
TEST tier_eviction_stops_the_list_at_its_buffer(void)
{
    char tmpl[64];
    const char *dir;
    tier_test_begin(tmpl, &dir);
    ASSERT(dir != NULL);
    add_marked(0, 30);
    unsigned long long list = datamanager_pools_resident_bytes();
    unsigned long long per = list / 30;
    unsigned long long cap = list * 100 / 95;
    unsigned long long guard = cap * STOCK_TIER_HOT_GUARD_PERMILLE / 1000;
    datamanager_set_ram_limit_bytes_for_tests(cap);

    ASSERT(stock_spill_step(1000) > 0);
    unsigned long long hot = datamanager_pools_resident_bytes();
    ASSERT(hot <= guard);
    ASSERT(hot + per > guard);             /* pas une possibilité de trop */
    ASSERT_EQ_FMT(30ULL, datas_size() + stock_spill_tier_packets() + stock_spill_total_packets(), "%llu");
    tier_test_end(dir);
    PASS();
}

/* L'étage ne dépend pas du répertoire de débordement : sans disque, la liste
 * continue de descendre vers l'étage, rien n'est perdu. */
TEST tier_relieves_the_cap_without_a_usable_spill_dir(void)
{
    stock_spill_set_tier_enabled_for_tests(1);
    stock_spill_configure("/proc/etii-inexistant/spill", nb_file_possibility);
    drain_datamanager();
    datamanager_set_ram_limit_packets_for_tests(0);
    add_marked(0, 30);
    datamanager_set_ram_limit_bytes_for_tests(1);
    capture_stderr();
    int moved = 0;
    for (int i = 0; i < 10; i++) {
        moved += stock_spill_step(10);
    }
    (void)restore_stderr_size();
    ASSERT_EQ_FMT(30, moved, "%d");
    ASSERT_EQ_FMT(30ULL, stock_spill_tier_packets(), "%llu");
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");
    ASSERT_EQ_FMT(0ULL, stock_spill_total_packets(), "%llu");

    datamanager_set_ram_limit_bytes_for_tests(1ULL << 30);
    stock_spill_step(100);
    ASSERT_EQ_FMT(30ULL, datas_size(), "%llu");
    datamanager_set_ram_limit_packets_for_tests(0);
    drain_datamanager();
    capture_stderr();
    stock_spill_configure("/proc/etii-inexistant/spill", nb_file_possibility);
    (void)restore_stderr_size();
    PASS();
}

/* Une sauvegarde porte l'étage (c'est de la RAM : aucun cliché à côté), et
 * une restauration le remplace au lieu de s'y ajouter. */
TEST tier_is_saved_by_backup_and_replaced_by_restore(void)
{
    char tmpl[64];
    const char *dir;
    tier_test_begin(tmpl, &dir);
    ASSERT(dir != NULL);
    datamanager_set_ram_tier_hooks(stock_spill_ram_tier_hooks());
    add_marked(0, 30);
    datamanager_set_ram_limit_bytes_for_tests(1);
    ASSERT_EQ_FMT(20, stock_spill_step(20), "%d");
    datamanager_set_ram_limit_packets_for_tests(0);

    char path[PATH_MAX], path_an[PATH_MAX];
    snprintf(path, sizeof path, "%s/tier.back", dir);
    snprintf(path_an, sizeof path_an, "%s/tier_an.back", dir);
    int rba = -99;
    ASSERT_EQ_FMT(BACKUP_OK, consistent_backup(path, path_an, &rba, NULL, NULL), "%d");
    /* La sauvegarde n'a rien déplacé. */
    ASSERT_EQ_FMT(20ULL, stock_spill_tier_packets(), "%llu");
    ASSERT_EQ_FMT(10ULL, datas_size(), "%llu");

    /* Le fichier contient les 30. */
    FILE *f = fopen(path, "rb");
    ASSERT(f != NULL);
    uint8_t header[PACKET_CODEC_FILE_HEADER_BYTES];
    ASSERT_EQ_FMT(sizeof header, fread(header, 1, sizeof header, f), "%zu");
    struct possibility_packet pk;
    int in_file = 0;
    while (packet_codec_fread(f, &pk) == 1) {
        in_file++;
    }
    fclose(f);
    ASSERT_EQ_FMT(30, in_file, "%d");

    /* Stock modifié entre-temps, puis restauration : exactement les 30. */
    add_marked(40, 5);
    datamanager_set_ram_limit_bytes_for_tests(1);
    ASSERT(stock_spill_step(5) > 0);
    datamanager_set_ram_limit_packets_for_tests(0);
    capture_stderr();
    datamanager_begin_maintenance();
    int rc = restore(path);
    datamanager_end_maintenance();
    (void)restore_stderr_size();
    ASSERT_EQ_FMT(0, rc, "%d");
    ASSERT_EQ_FMT(0ULL, stock_spill_tier_packets(), "%llu");
    ASSERT_EQ_FMT(0ULL, stock_spill_tier_bytes(), "%llu");
    int m[64];
    ASSERT_EQ_FMT(30, list_markers_sorted(m, 64), "%d");
    for (int i = 0; i < 30; i++) {
        ASSERT_EQ_FMT(i, m[i], "%d");
    }
    datamanager_set_ram_tier_hooks(NULL);
    tier_test_end(dir);
    PASS();
}

/* Une restauration sous un plafond plus bas que le stock sauvegardé : l'import
 * déverse son trop-plein dans l'étage par le crochet de dégagement, sous la
 * fenêtre de maintenance qu'il tient — sans interblocage avec le vidage de
 * l'étage que `restore()` fait juste avant, et sans rien perdre. */
TEST tier_takes_the_overflow_of_a_restore_under_a_lower_cap(void)
{
    char tmpl[64];
    const char *dir;
    tier_test_begin(tmpl, &dir);
    ASSERT(dir != NULL);
    datamanager_set_ram_tier_hooks(stock_spill_ram_tier_hooks());
    add_marked(0, 30);
    unsigned long long list30 = datamanager_pools_resident_bytes();
    char path[PATH_MAX], path_an[PATH_MAX];
    snprintf(path, sizeof path, "%s/cap.back", dir);
    snprintf(path_an, sizeof path_an, "%s/cap_an.back", dir);
    int rba = -99;
    ASSERT_EQ_FMT(BACKUP_OK, consistent_backup(path, path_an, &rba, NULL, NULL), "%d");
    drain_datamanager();

    /* Place pour ~la moitié du stock en liste : le reste doit aller dans
     * l'étage (le disque est disponible, mais la liste est au-dessus de son
     * tampon tant qu'elle n'est pas descendue à sa borne en octets). */
    datamanager_set_ram_relief_hook(stock_spill_relieve);
    datamanager_set_ram_limit_bytes_for_tests(list30 / 2);
    capture_stderr();
    datamanager_begin_maintenance();
    int rc = restore(path);
    datamanager_end_maintenance();
    (void)restore_stderr_size();
    datamanager_set_ram_relief_hook(NULL);
    ASSERT_EQ_FMT(0, rc, "%d");
    ASSERT(stock_spill_tier_packets() > 0ULL);
    ASSERT_EQ_FMT(30ULL, datas_size() + stock_spill_tier_packets() + stock_spill_total_packets(), "%llu");
    ASSERT(datamanager_resident_bytes() <= list30 / 2);

    /* Et ce sont bien les 30 d'origine. */
    datamanager_set_ram_limit_bytes_for_tests(1ULL << 30);
    for (int i = 0; i < 20 && (stock_spill_tier_packets() > 0 || stock_spill_total_packets() > 0); i++) {
        stock_spill_step(4096);
    }
    int m[64];
    ASSERT_EQ_FMT(30, list_markers_sorted(m, 64), "%d");
    for (int i = 0; i < 30; i++) {
        ASSERT_EQ_FMT(i, m[i], "%d");
    }
    datamanager_set_ram_tier_hooks(NULL);
    tier_test_end(dir);
    PASS();
}

/* Sous plafond, une restauration range le `.back` DIRECTEMENT dans l'étage, en
 * blocs, chaque possibilité dans le pool de son drapeau : aucune ne passe par
 * un maillon de liste. Par la liste, un stock de 1,3 milliard sous 36 Go
 * gardait 215 M maillons le temps de l'import et finissait en swap. Les
 * listes se remplissent ensuite au rechargement, depuis l'étage. Contre-
 * épreuve : sans `import_tier_add`, les listes tiennent les 30 au retour de
 * `restore()`. */
TEST restore_under_a_cap_goes_straight_into_the_tier(void)
{
    char tmpl[64];
    const char *dir;
    tier_test_begin(tmpl, &dir);
    ASSERT(dir != NULL);
    datamanager_set_ram_tier_hooks(stock_spill_ram_tier_hooks());
    add_marked_pool(0, 20, 0);
    add_marked_pool(20, 10, 1);
    char path[PATH_MAX], path_an[PATH_MAX];
    snprintf(path, sizeof path, "%s/direct.back", dir);
    snprintf(path_an, sizeof path_an, "%s/direct_an.back", dir);
    int rba = -99;
    ASSERT_EQ_FMT(BACKUP_OK, consistent_backup(path, path_an, &rba, NULL, NULL), "%d");
    drain_datamanager();

    datamanager_set_ram_relief_hook(stock_spill_relieve);
    datamanager_set_ram_limit_bytes_for_tests(1ULL << 30);
    capture_stderr();
    datamanager_begin_maintenance();
    int rc = restore(path);
    datamanager_end_maintenance();
    (void)restore_stderr_size();
    datamanager_set_ram_relief_hook(NULL);
    ASSERT_EQ_FMT(0, rc, "%d");
    ASSERT_EQ_FMT(0ULL, list_size_of_pool(0), "%llu");
    ASSERT_EQ_FMT(0ULL, list_size_of_pool(1), "%llu");
    ASSERT_EQ_FMT(20ULL, stock_spill_tier_pool_packets(0), "%llu");
    ASSERT_EQ_FMT(10ULL, stock_spill_tier_pool_packets(1), "%llu");
    ASSERT_EQ_FMT(0ULL, stock_spill_total_packets(), "%llu");
    ASSERT_EQ_FMT(stock_spill_tier_bytes(), datamanager_resident_bytes(), "%llu");

    /* Le rechargement remplit les listes, chaque pool depuis ses piles. */
    for (int i = 0; i < 20 && stock_spill_tier_packets() > 0; i++) {
        stock_spill_step(4096);
    }
    ASSERT_EQ_FMT(20ULL, list_size_of_pool(0), "%llu");
    ASSERT_EQ_FMT(10ULL, list_size_of_pool(1), "%llu");
    int m[64];
    ASSERT_EQ_FMT(30, list_markers_sorted(m, 64), "%d");
    for (int i = 0; i < 30; i++) {
        ASSERT_EQ_FMT(i, m[i], "%d");
    }
    datamanager_set_ram_tier_hooks(NULL);
    tier_test_end(dir);
    PASS();
}

/* Un stock plus gros que le plafond : l'import direct fait sa place en
 * envoyant le BAS de l'étage sur disque, reste sous sa cible (mi-chemin des
 * seuils 75 %/90 %, pour que le rechargement qui suit ne relance pas
 * l'éviction) et ne perd rien. */
TEST restore_into_the_tier_makes_room_on_disk(void)
{
    char tmpl[64];
    const char *dir;
    tier_test_begin(tmpl, &dir);
    ASSERT(dir != NULL);
    datamanager_set_ram_tier_hooks(stock_spill_ram_tier_hooks());
    add_marked(0, 30);
    unsigned long long list30 = datamanager_pools_resident_bytes();
    char path[PATH_MAX], path_an[PATH_MAX];
    snprintf(path, sizeof path, "%s/disk.back", dir);
    snprintf(path_an, sizeof path_an, "%s/disk_an.back", dir);
    int rba = -99;
    ASSERT_EQ_FMT(BACKUP_OK, consistent_backup(path, path_an, &rba, NULL, NULL), "%d");
    drain_datamanager();

    /* Un tiers de ce que les 30 pèsent en liste : les blocs, petits sous un
     * tel plafond (un seizième de la cible), paient chacun leur en-tête et ne
     * tiennent pas tous. Pas une fraction de leur poids COMPRIMÉ : sous zstd,
     * un bloc d'un enregistrement dépasserait à lui seul la cible. */
    unsigned long long cap = list30 / 3;
    datamanager_set_ram_relief_hook(stock_spill_relieve);
    datamanager_set_ram_limit_bytes_for_tests(cap);
    capture_stderr();
    datamanager_begin_maintenance();
    int rc = restore(path);
    datamanager_end_maintenance();
    (void)restore_stderr_size();
    datamanager_set_ram_relief_hook(NULL);
    ASSERT_EQ_FMT(0, rc, "%d");
    ASSERT_EQ_FMT(0ULL, list_size_of_pool(0), "%llu");
    ASSERT(stock_spill_total_packets() > 0ULL);
    ASSERT(stock_spill_tier_packets() > 0ULL);
    ASSERT_EQ_FMT(30ULL, stock_spill_tier_packets() + stock_spill_total_packets(), "%llu");
    ASSERT(datamanager_resident_bytes() <= cap / 200 * 165 + cap % 200 * 165 / 200);

    datamanager_set_ram_limit_bytes_for_tests(1ULL << 30);
    for (int i = 0; i < 50 && (stock_spill_tier_packets() > 0 || stock_spill_total_packets() > 0); i++) {
        stock_spill_step(4096);
    }
    int m[64];
    ASSERT_EQ_FMT(30, list_markers_sorted(m, 64), "%d");
    for (int i = 0; i < 30; i++) {
        ASSERT_EQ_FMT(i, m[i], "%d");
    }
    datamanager_set_ram_tier_hooks(NULL);
    tier_test_end(dir);
    PASS();
}

/* L'import direct lit le `.back` par morceaux : sous des morceaux à peine plus
 * grands qu'un enregistrement, presque chacun tombe à cheval sur deux lectures,
 * et tous arrivent entiers, chacun dans son pool. Un fichier tronqué garde ce
 * qui précède la coupure et le signale. */
TEST restore_direct_copy_survives_records_split_across_reads(void)
{
    char tmpl[64];
    const char *dir;
    tier_test_begin(tmpl, &dir);
    ASSERT(dir != NULL);
    datamanager_set_ram_tier_hooks(stock_spill_ram_tier_hooks());
    add_marked_pool(0, 20, 0);
    add_marked_pool(20, 10, 1);
    char path[PATH_MAX], path_an[PATH_MAX];
    snprintf(path, sizeof path, "%s/split.back", dir);
    snprintf(path_an, sizeof path_an, "%s/split_an.back", dir);
    int rba = -99;
    ASSERT_EQ_FMT(BACKUP_OK, consistent_backup(path, path_an, &rba, NULL, NULL), "%d");
    drain_datamanager();

    datamanager_set_import_chunk_for_tests(1); /* borné à un enregistrement maximal + 1 */
    datamanager_set_ram_limit_bytes_for_tests(1ULL << 30);
    for (int truncated = 0; truncated < 2; truncated++) {
        if (truncated) {
            struct stat st;
            ASSERT_EQ_FMT(0, stat(path, &st), "%d");
            ASSERT_EQ_FMT(0, truncate(path, st.st_size - 3), "%d");
        }
        capture_stderr();
        datamanager_begin_maintenance();
        int rc = restore(path);
        datamanager_end_maintenance();
        long err = restore_stderr_size();
        ASSERT_EQ_FMT(0, rc, "%d");
        ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");
        ASSERT_EQ_FMT(truncated ? 29ULL : 30ULL, stock_spill_tier_packets(), "%llu");
        if (truncated) {
            ASSERT(err > 0); /* « enregistrement tronqué ou incohérent » */
        } else {
            ASSERT_EQ_FMT(20ULL, stock_spill_tier_pool_packets(0), "%llu");
            ASSERT_EQ_FMT(10ULL, stock_spill_tier_pool_packets(1), "%llu");
        }
        for (int i = 0; i < 20 && stock_spill_tier_packets() > 0; i++) {
            stock_spill_step(4096);
        }
        int m[64];
        int n = list_markers_sorted(m, 64);
        ASSERT_EQ_FMT(truncated ? 29 : 30, n, "%d");
        for (int i = 0; i < 29; i++) {
            ASSERT_EQ_FMT(i, m[i], "%d");
        }
    }
    datamanager_set_import_chunk_for_tests(0);
    datamanager_set_ram_tier_hooks(NULL);
    tier_test_end(dir);
    PASS();
}

/* Lit un `.back` compacté et range ses marqueurs (relatifs à MARK_BASE) dans
 * l'ordre du fichier. */
static int read_back_markers(const char *path, int *out, int max)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return -1;
    }
    uint8_t header[PACKET_CODEC_FILE_HEADER_BYTES];
    if (fread(header, 1, sizeof header, f) != sizeof header) {
        fclose(f);
        return -1;
    }
    struct possibility_packet p;
    int n = 0;
    while (n < max && packet_codec_fread(f, &p) == 1) {
        out[n++] = p.grid[0][0] - MARK_BASE;
    }
    fclose(f);
    return n;
}

/* La compression des blocs d'un import est parallèle, mais chaque pile garde
 * l'ordre d'envoi : tous les blocs d'une file vont au même fil. Des blocs d'une
 * possibilité, envoyés en alternance à deux files sous quatre fils, ressortent
 * de la sauvegarde (étage écrit de bas en haut, file par file) exactement dans
 * leur ordre d'envoi. */
TEST import_workers_keep_the_push_order_of_each_stack(void)
{
    char tmpl[64];
    const char *dir;
    tier_test_begin(tmpl, &dir);
    ASSERT(dir != NULL);
    datamanager_set_ram_tier_hooks(stock_spill_ram_tier_hooks());
    stock_spill_set_import_workers_for_tests(4);
    datamanager_set_ram_limit_bytes_for_tests(1ULL << 30);
    const datamanager_ram_tier_hooks_t *hooks = stock_spill_ram_tier_hooks();
    ASSERT(hooks->import_block_bytes() > 0);
    for (int i = 0; i < 34; i++) {
        struct possibility_packet p;
        memset(&p, 0, sizeof p);
        init_empty_grid(&p);
        p.grid[0][0] = (int16_t)(MARK_BASE + i);
        uint8_t rec[PACKET_CODEC_MAX_BYTES];
        size_t len = 0;
        ASSERT_EQ_FMT(0, packet_codec_encode(&p, rec, sizeof rec, &len), "%d");
        ASSERT_EQ_FMT(1, hooks->import_push(0, i % 2, rec, len), "%d");
    }
    ASSERT_EQ_FMT(0, hooks->import_finish(), "%d");
    ASSERT_EQ_FMT(34ULL, stock_spill_tier_packets(), "%llu");

    char path[PATH_MAX], path_an[PATH_MAX];
    snprintf(path, sizeof path, "%s/order.back", dir);
    snprintf(path_an, sizeof path_an, "%s/order_an.back", dir);
    int rba = -99;
    ASSERT_EQ_FMT(BACKUP_OK, consistent_backup(path, path_an, &rba, NULL, NULL), "%d");
    int m[64];
    ASSERT_EQ_FMT(34, read_back_markers(path, m, 64), "%d");
    for (int k = 0; k < 17; k++) {
        ASSERT_EQ_FMT(2 * k, m[k], "%d");          /* file 0, de bas en haut */
        ASSERT_EQ_FMT(2 * k + 1, m[17 + k], "%d"); /* puis file 1 */
    }
    stock_spill_set_import_workers_for_tests(0);
    datamanager_set_ram_tier_hooks(NULL);
    tier_test_end(dir);
    PASS();
}

/* Le nombre de fils de compression ne change rien à ce qui est restauré : une
 * restauration à 1 fil et une à 4 donnent des sauvegardes identiques octet
 * pour octet (sans disque, où le choix de la pile à déporter dépend du
 * moment). */
TEST restore_is_the_same_whatever_the_number_of_compression_workers(void)
{
    char tmpl[64];
    const char *dir;
    tier_test_begin(tmpl, &dir);
    ASSERT(dir != NULL);
    datamanager_set_ram_tier_hooks(stock_spill_ram_tier_hooks());
    add_marked_pool(0, 20, 0);
    add_marked_pool(20, 10, 1);
    unsigned long long list30 = datamanager_pools_resident_bytes();
    char src[PATH_MAX], src_an[PATH_MAX];
    snprintf(src, sizeof src, "%s/src.back", dir);
    snprintf(src_an, sizeof src_an, "%s/src_an.back", dir);
    int rba = -99;
    ASSERT_EQ_FMT(BACKUP_OK, consistent_backup(src, src_an, &rba, NULL, NULL), "%d");
    drain_datamanager();

    /* Petits blocs (un seizième de la cible) : plusieurs par pool. */
    datamanager_set_ram_limit_bytes_for_tests(list30 * 2);
    const int workers[2] = { 1, 4 };
    char out[2][PATH_MAX];
    for (int k = 0; k < 2; k++) {
        stock_spill_set_import_workers_for_tests(workers[k]);
        capture_stderr();
        datamanager_begin_maintenance();
        int rc = restore(src);
        datamanager_end_maintenance();
        (void)restore_stderr_size();
        ASSERT_EQ_FMT(0, rc, "%d");
        ASSERT_EQ_FMT(0ULL, stock_spill_total_packets(), "%llu");
        ASSERT_EQ_FMT(30ULL, stock_spill_tier_packets(), "%llu");
        ASSERT(stock_spill_tier_packets() > 0ULL);
        char an[PATH_MAX];
        snprintf(out[k], sizeof out[k], "%s/out%d.back", dir, workers[k]);
        snprintf(an, sizeof an, "%s/out%d_an.back", dir, workers[k]);
        ASSERT_EQ_FMT(BACKUP_OK, consistent_backup(out[k], an, &rba, NULL, NULL), "%d");
    }
    int a[64], b[64];
    int na = read_back_markers(out[0], a, 64);
    int nb = read_back_markers(out[1], b, 64);
    ASSERT_EQ_FMT(30, na, "%d");
    ASSERT_EQ_FMT(na, nb, "%d");
    ASSERT_MEM_EQ(a, b, sizeof(int) * (size_t)na);
    stock_spill_set_import_workers_for_tests(0);
    datamanager_set_ram_tier_hooks(NULL);
    tier_test_end(dir);
    PASS();
}

/* Pendant que le fil de l'import décide de la place, les fils de travail
 * chaînent leurs blocs et rendent leur réservation : lue en deux fois,
 * l'occupation comptait par moments un bloc deux fois (chaîné ET réservé), et
 * l'étage refusait à tort un bloc — tout le reste de l'import repassait par
 * les listes (mesuré : 1,7 M possibilités sur 49 M, une restauration sur
 * trois). 2 000 possibilités en petits blocs, du disque, quatre fils, vingt
 * fois : pas une ne doit finir en liste ni manquer. */
static char g_stress_dir[PATH_MAX];
static char g_stress_src[PATH_MAX];

/* Exécuté dans un FILS avec `alarm()` : un refus à tort fait passer la suite de
 * l'import par les listes, qui sous un plafond aussi bas attendent de la place
 * sans fin (par conception : caler visiblement plutôt que perdre) — un test
 * qui pendrait au lieu d'échouer. 0 = tout va bien, 2 = une possibilité en
 * liste ou perdue, 3 = une restauration en échec. */
static void parallel_import_stress_child(void)
{
    alarm(60); /* filet : un blocage tue le fils au lieu de figer le runner */
    for (int round = 0; round < 20; round++) {
        capture_stderr();
        /* `restore()` seul ne vide pas le disque (c'est `restore_apply`, par
           `stock_spill_prepare_restore`) : il est purgé à chaque tour. */
        stock_spill_configure(g_stress_dir, nb_file_possibility);
        datamanager_begin_maintenance();
        int rc = restore(g_stress_src);
        datamanager_end_maintenance();
        (void)restore_stderr_size();
        if (rc != 0) {
            exit(3);
        }
        if (datas_size() != 0 || stock_spill_tier_packets() + stock_spill_total_packets() != 2000) {
            exit(2);
        }
    }
    exit(0);
}

TEST parallel_import_never_refuses_a_block_it_has_room_for(void)
{
    char tmpl[64];
    const char *dir;
    tier_test_begin(tmpl, &dir);
    ASSERT(dir != NULL);
    datamanager_set_ram_tier_hooks(stock_spill_ram_tier_hooks());
    int allocs[2000];
    for (int i = 0; i < 2000; i++) {
        allocs[i] = i + 1;
    }
    add_packets(allocs, 2000);
    unsigned long long list_bytes = datamanager_pools_resident_bytes();
    snprintf(g_stress_dir, sizeof g_stress_dir, "%s", dir);
    snprintf(g_stress_src, sizeof g_stress_src, "%s/stress.back", dir);
    char src_an[PATH_MAX];
    snprintf(src_an, sizeof src_an, "%s/stress_an.back", dir);
    int rba = -99;
    ASSERT_EQ_FMT(BACKUP_OK, consistent_backup(g_stress_src, src_an, &rba, NULL, NULL), "%d");
    drain_datamanager();

    /* Un tiers du poids en liste : quelques centaines de petits blocs, dont
       une bonne part part sur disque. */
    stock_spill_set_import_workers_for_tests(4);
    datamanager_set_ram_limit_bytes_for_tests(list_bytes / 3);
    ASSERT_EQ_FMT(0, run_in_fork(parallel_import_stress_child, NULL), "%d");
    stock_spill_set_import_workers_for_tests(0);
    datamanager_set_ram_tier_hooks(NULL);
    tier_test_end(dir);
    PASS();
}

/* Un fil de compression peut chaîner son bloc ENTRE l'essai du disque (qui
 * n'a rien trouvé : tout était en vol) et la lecture des blocs en vol (qui
 * lit alors 0). Ce bloc tout juste chaîné peut partir sur disque : refuser
 * sur cette lecture renvoyait le bloc suivant aux listes, alors que l'étage
 * avait de quoi lui faire place (CI, PR #365 : 4 possibilités en liste sur
 * 30 dans restore_into_the_tier_makes_room_on_disk). La fenêtre est forcée :
 * les fils retiennent leurs blocs jusqu'au premier essai manqué du disque. */
TEST import_push_retries_the_disk_after_an_in_flight_block_lands(void)
{
    char tmpl[64];
    const char *dir;
    tier_test_begin(tmpl, &dir);
    ASSERT(dir != NULL);
    datamanager_set_ram_tier_hooks(stock_spill_ram_tier_hooks());
    add_marked(0, 30);
    unsigned long long list30 = datamanager_pools_resident_bytes();
    char path[PATH_MAX], path_an[PATH_MAX];
    snprintf(path, sizeof path, "%s/landing.back", dir);
    snprintf(path_an, sizeof path_an, "%s/landing_an.back", dir);
    int rba = -99;
    ASSERT_EQ_FMT(BACKUP_OK, consistent_backup(path, path_an, &rba, NULL, NULL), "%d");
    drain_datamanager();

    stock_spill_set_import_workers_for_tests(3);
    stock_spill_set_import_hold_until_disk_miss_for_tests(1);
    datamanager_set_ram_relief_hook(stock_spill_relieve);
    datamanager_set_ram_limit_bytes_for_tests(list30 / 3);
    capture_stderr();
    datamanager_begin_maintenance();
    int rc = restore(path);
    datamanager_end_maintenance();
    (void)restore_stderr_size();
    datamanager_set_ram_relief_hook(NULL);
    stock_spill_set_import_hold_until_disk_miss_for_tests(0);
    stock_spill_set_import_workers_for_tests(0);
    ASSERT_EQ_FMT(0, rc, "%d");
    ASSERT_EQ_FMT(0ULL, list_size_of_pool(0), "%llu");
    ASSERT(stock_spill_total_packets() > 0ULL);
    ASSERT_EQ_FMT(30ULL, stock_spill_tier_packets() + stock_spill_total_packets(), "%llu");

    datamanager_set_ram_limit_bytes_for_tests(1ULL << 30);
    for (int i = 0; i < 50 && (stock_spill_tier_packets() > 0 || stock_spill_total_packets() > 0); i++) {
        stock_spill_step(4096);
    }
    int m[64];
    ASSERT_EQ_FMT(30, list_markers_sorted(m, 64), "%d");
    for (int i = 0; i < 30; i++) {
        ASSERT_EQ_FMT(i, m[i], "%d");
    }
    datamanager_set_ram_tier_hooks(NULL);
    tier_test_end(dir);
    PASS();
}

/* Met les 30 possibilités marquées dans l'étage, en blocs de 2, sans rien
 * envoyer sur disque (le pas s'arrête dès la liste vide). */
static void tier_fill_by_pairs(void)
{
    add_marked(0, 30);
    datamanager_set_ram_limit_bytes_for_tests(1);
    for (int i = 0; i < 100 && datas_size() > 0; i++) {
        stock_spill_step(2);
    }
}

/* L'étage part sur disque par LOTS (un `fopen`/`fsync` par segment touché,
 * plus un par bloc : 81 % du temps d'une restauration qui déborde). Un lot de
 * quinze blocs traverse plusieurs segments de 5 possibilités, chacun
 * synchronisé avant de rouler, et tout revient. */
TEST tier_disk_batch_crosses_segment_boundaries(void)
{
    char tmpl[64];
    const char *dir;
    tier_test_begin(tmpl, &dir);
    ASSERT(dir != NULL);
    stock_spill_set_segment_records_for_tests(5);
    tier_fill_by_pairs();
    ASSERT_EQ_FMT(30ULL, stock_spill_tier_packets(), "%llu");
    ASSERT_EQ_FMT(0ULL, stock_spill_total_packets(), "%llu");

    /* Liste vide, au-dessus du seuil haut : un seul pas envoie tout l'étage. */
    ASSERT_EQ_FMT(30, stock_spill_step(30), "%d");
    ASSERT_EQ_FMT(0ULL, stock_spill_tier_packets(), "%llu");
    ASSERT_EQ_FMT(30ULL, stock_spill_total_packets(), "%llu");
    ASSERT(stock_spill_total_segments() >= 6ULL); /* 2 trames de 2 par segment de 5 */

    datamanager_set_ram_limit_bytes_for_tests(1ULL << 30);
    for (int i = 0; i < 50 && stock_spill_total_packets() > 0; i++) {
        stock_spill_step(4096);
    }
    int m[64];
    ASSERT_EQ_FMT(30, list_markers_sorted(m, 64), "%d");
    for (int i = 0; i < 30; i++) {
        ASSERT_EQ_FMT(i, m[i], "%d");
    }
    stock_spill_set_segment_records_for_tests(0);
    tier_test_end(dir);
    PASS();
}

/* Un lot qui échoue en route garde ce qui est déjà synchronisé et laisse le
 * reste dans l'étage : ici le segment courant accepte encore une trame, mais le
 * suivant ne peut pas être créé (répertoire en lecture seule). */
TEST tier_disk_batch_failure_keeps_the_synced_prefix(void)
{
    SKIP_IF_ROOT();
    char tmpl[64];
    const char *dir;
    tier_test_begin(tmpl, &dir);
    ASSERT(dir != NULL);
    stock_spill_set_segment_records_for_tests(5);
    tier_fill_by_pairs();
    ASSERT_EQ_FMT(2, stock_spill_step(2), "%d"); /* segment 1 : une trame de 2 */
    ASSERT_EQ_FMT(2ULL, stock_spill_total_packets(), "%llu");

    ASSERT_EQ_FMT(0, chmod(dir, 0555), "%d");
    capture_stderr();
    int moved = stock_spill_step(28);
    long err = restore_stderr_size();
    chmod(dir, 0755);
    ASSERT_EQ_FMT(2, moved, "%d");              /* complète le segment 1, pas plus */
    ASSERT(err > 0);                            /* « échec d'écriture du segment » */
    ASSERT_EQ_FMT(4ULL, stock_spill_total_packets(), "%llu");
    ASSERT_EQ_FMT(26ULL, stock_spill_tier_packets(), "%llu");

    /* Rien n'est perdu : le reste part au pas suivant, et tout revient. */
    ASSERT_EQ_FMT(26, stock_spill_step(26), "%d");
    datamanager_set_ram_limit_bytes_for_tests(1ULL << 30);
    for (int i = 0; i < 50 && stock_spill_total_packets() > 0; i++) {
        stock_spill_step(4096);
    }
    int m[64];
    ASSERT_EQ_FMT(30, list_markers_sorted(m, 64), "%d");
    for (int i = 0; i < 30; i++) {
        ASSERT_EQ_FMT(i, m[i], "%d");
    }
    stock_spill_set_segment_records_for_tests(0);
    tier_test_end(dir);
    PASS();
}

/* Crochets factices : l'étage accepte `fake_push_accept` blocs, puis répond
 * `fake_push_refusal` (0 : plus de place, -1 : échec). */
static int fake_push_accept;
static int fake_push_refusal;
static int fake_push_markers[64];
static int fake_push_count;

static size_t fake_import_block_bytes(void)
{
    return 1; /* un enregistrement par bloc */
}

static int fake_import_push(int is_checked, int file_index, const uint8_t *raw, size_t raw_bytes)
{
    (void)is_checked;
    (void)file_index;
    if (fake_push_accept <= 0) {
        return fake_push_refusal;
    }
    fake_push_accept--;
    int n = 0;
    size_t off = 0;
    while (off < raw_bytes) {
        struct possibility_packet p;
        size_t used = 0;
        if (packet_codec_decode(raw + off, raw_bytes - off, &p, &used) != 0) {
            return -1;
        }
        fake_push_markers[fake_push_count++] = p.grid[0][0] - MARK_BASE;
        off += used;
        n++;
    }
    return n;
}

/* L'étage qui refuse un bloc (plein sans disque, ou échec d'allocation) ne
 * fait rien perdre : ce bloc et tout le reste de l'import passent par les
 * listes, comme avant l'import direct. */
TEST restore_falls_back_to_the_lists_when_the_tier_refuses(void)
{
    static const datamanager_ram_tier_hooks_t fake = {
        NULL, NULL, NULL, NULL, fake_import_block_bytes, fake_import_push
    };
    int refusals[2] = { 0, -1 };
    for (int k = 0; k < 2; k++) {
        char tmpl[64];
        const char *dir;
        tier_test_begin(tmpl, &dir);
        ASSERT(dir != NULL);
        add_marked(0, 30);
        char path[PATH_MAX], path_an[PATH_MAX];
        snprintf(path, sizeof path, "%s/fb.back", dir);
        snprintf(path_an, sizeof path_an, "%s/fb_an.back", dir);
        int rba = -99;
        ASSERT_EQ_FMT(BACKUP_OK, consistent_backup(path, path_an, &rba, NULL, NULL), "%d");
        drain_datamanager();

        fake_push_accept = 12;
        fake_push_refusal = refusals[k];
        fake_push_count = 0;
        datamanager_set_ram_tier_hooks(&fake);
        capture_stderr();
        datamanager_begin_maintenance();
        int rc = restore(path);
        datamanager_end_maintenance();
        (void)restore_stderr_size();
        datamanager_set_ram_tier_hooks(NULL);
        ASSERT_EQ_FMT(0, rc, "%d");
        ASSERT_EQ_FMT(12, fake_push_count, "%d");
        ASSERT_EQ_FMT(18ULL, datas_size(), "%llu");

        int m[64];
        int n = list_markers_sorted(m, 64);
        ASSERT_EQ_FMT(18, n, "%d");
        for (int i = 0; i < 12; i++) {
            m[n++] = fake_push_markers[i];
        }
        qsort(m, (size_t)n, sizeof(int), int_cmp);
        for (int i = 0; i < 30; i++) {
            ASSERT_EQ_FMT(i, m[i], "%d");
        }
        tier_test_end(dir);
    }
    PASS();
}

/* Une passe d'expansion lit les blocs d'AVANT elle (le disque d'abord), ne
 * reprend jamais ceux qu'elle a elle-même évincés, et n'envoie sur disque
 * que ces derniers : un bloc d'avant la passe envoyé sur disque atterrirait
 * au-dessus de la frontière du disque et ne serait pas développé. */
TEST tier_expansion_reads_pre_pass_blocks_and_moves_only_its_own_to_disk(void)
{
    char tmpl[64];
    const char *dir;
    tier_test_begin(tmpl, &dir);
    ASSERT(dir != NULL);
    add_marked(0, 30);
    datamanager_set_ram_limit_bytes_for_tests(1);
    ASSERT_EQ_FMT(10, stock_spill_step(10), "%d");   /* A : 0..9 */
    ASSERT_EQ_FMT(10, stock_spill_step(10), "%d");   /* B : 10..19 */

    stock_spill_expansion_begin();
    ASSERT_EQ_FMT(10, stock_spill_step(10), "%d");   /* C : 20..29, pendant la passe */
    ASSERT_EQ_FMT(10, stock_spill_step(10), "%d");   /* liste vide : C, pas A, part sur disque */
    ASSERT_EQ_FMT(10ULL, stock_spill_total_packets(), "%llu");
    ASSERT_EQ_FMT(20ULL, stock_spill_tier_packets(), "%llu");

    datamanager_spill_stats_t st;
    stock_spill_expansion_stats(&st);
    ASSERT_EQ_FMT(20ULL, st.tier, "%llu");

    /* Place insuffisante : rien lu. */
    take_sink_t t = { {0}, {0}, 0, -1 };
    ASSERT_EQ_FMT(DATAMANAGER_DISK_TAKE_NO_ROOM, stock_spill_expansion_take(take_sink, &t, 3), "%d");
    /* Échec du collecteur : le bloc reste. */
    t.fail_at = 2;
    ASSERT_EQ_FMT(-1, stock_spill_expansion_take(take_sink, &t, 100), "%d");
    ASSERT_EQ_FMT(20ULL, stock_spill_tier_packets(), "%llu");

    take_sink_t a = { {0}, {0}, 0, -1 };
    ASSERT_EQ_FMT(10, stock_spill_expansion_take(take_sink, &a, 100), "%d");
    for (int i = 0; i < 10; i++) {
        ASSERT_EQ_FMT(MARK_BASE + i, a.markers[i], "%d");
        ASSERT_EQ_FMT(1, a.develop[i], "%d");
    }
    take_sink_t b = { {0}, {0}, 0, -1 };
    ASSERT_EQ_FMT(10, stock_spill_expansion_take(take_sink, &b, 100), "%d");
    ASSERT_EQ_FMT(MARK_BASE + 10, b.markers[0], "%d");
    /* C est sur disque, au-dessus de la frontière du disque : rien d'autre. */
    take_sink_t c = { {0}, {0}, 0, -1 };
    ASSERT_EQ_FMT(0, stock_spill_expansion_take(take_sink, &c, 100), "%d");
    stock_spill_expansion_end();

    ASSERT_EQ_FMT(0ULL, stock_spill_tier_packets(), "%llu");
    ASSERT_EQ_FMT(10ULL, stock_spill_total_packets(), "%llu");
    tier_test_end(dir);
    PASS();
}

/* Ramène tout l'étage et le disque en liste, puis vide la liste en relevant
 * `alloc` (tampon par défaut : le rechargement ne s'arrête pas en route). */
static int tier_collect_all_allocs(int *allocs, int max)
{
    stock_spill_configure_tier(STOCK_TIER_HOT_MAX_DEFAULT, STOCK_TIER_HOT_MIN_DEFAULT);
    datamanager_set_ram_limit_bytes_for_tests(1ULL << 30);
    for (int k = 0; k < 20000 && (stock_spill_tier_packets() > 0 || stock_spill_total_packets() > 0); k++) {
        stock_spill_step(4096);
    }
    int n = 0;
    while (datas_size() > 0) {
        array_possibility_packet *r = get_last_possibility(NULL, 1000, NULL);
        for (int i = 0; i < r->size && n < max; i++) {
            allocs[n++] = r->possibilities[i].alloc;
        }
        free_array_possibility_packet(r);
    }
    return n;
}

/* Source disque de production : `usable` dit que les enfants peuvent partir
 * sur disque, la passe attend alors de la place au lieu de cesser
 * d'approfondir. */
static const datamanager_expansion_disk_source_t g_spill_source_usable = {
    stock_spill_expansion_begin, stock_spill_expansion_take, stock_spill_expansion_end,
    stock_spill_expansion_stats, stock_spill_expansion_disk_usable
};

/* Dégagement qui, de temps en temps, ne déplace rien plusieurs fois de suite —
 * comme en production quand le verrou de la file est pris (les primitives
 * compactes n'attendent jamais) ou que rien n'est évinçable à cet instant.
 * Quatre échecs consécutifs : l'attente de place réessaie une fois après un
 * premier dégagement nul, et ne rapporte le refus qu'à partir de son deuxième
 * tour (`add_possibility_waiting_for_room`). */
static int g_flaky_relief_calls = 0;
static int flaky_relief(int max_packets)
{
    int k = g_flaky_relief_calls++ % 8;
    return (k < 4) ? 0 : stock_spill_relieve(max_packets);
}

/* Un niveau à franchir = UNE passe, même quand le dégagement échoue parfois :
 * avec un disque utilisable, un ADD non soulagé fait attendre la passe, il ne
 * la fait pas cesser d'approfondir. Contre-épreuve : suspendue, la passe
 * réinjecte le reste de son stock tel quel et il faut des passes de plus —
 * chacune relisant tout le stock (des heures sur un milliard de
 * possibilités). */
TEST tier_expansion_waits_for_room_instead_of_suspending_the_pass(void)
{
    char tmpl[64];
    const char *dir;
    tier_test_begin(tmpl, &dir);
    ASSERT(dir != NULL);
    stock_spill_set_tier_disk_factor_for_tests(0); /* lots disque de production */
    request = REQUEST_CONTINUE;
    stock_spill_set_segment_records_for_tests(4);
    enum { SEEDS = 400, LEVEL = 2 };
    int expected = expand_reference_count(SEEDS, LEVEL, EXPAND_MAX_LEVELS);

    for (int i = 0; i < SEEDS; i++) seed_genesis(1);
    stock_spill_set_hot_buffer_for_tests(2, 1);
    /* Les graines tiennent, pas leurs enfants : ils partent sur disque. */
    datamanager_set_ram_limit_bytes_for_tests(datamanager_resident_bytes() * 2);
    g_flaky_relief_calls = 0;
    datamanager_set_ram_relief_hook(flaky_relief);
    datamanager_set_expansion_disk_source(&g_spill_source_usable);
    /* Un seul fil : les deux dégagements nuls tombent sur le même ADD (à
     * plusieurs, un autre fil fait la place entre les deux). */
    datamanager_set_expand_threads(1);
    capture_stderr();
    int passes = expand_datas_to_level(LEVEL, make_expand_free_map(), make_expand_parts());
    (void)restore_stderr_size();
    datamanager_set_expand_threads(1);
    datamanager_set_expansion_disk_source(NULL);
    datamanager_set_ram_relief_hook(NULL);
    /* Les 4 premiers dégagements sont nuls et tombent sur le premier ADD
     * refusé : un cinquième prouve que ce refus a duré plus d'un tour
     * d'attente — donc qu'il a été rapporté à la passe. */
    ASSERT(g_flaky_relief_calls > 4);
    ASSERT_EQ_FMT(1, passes, "%d");

    static int allocs[1 << 16];
    int n = tier_collect_all_allocs(allocs, 1 << 16);
    stock_spill_set_segment_records_for_tests(0);
    ASSERT_EQ_FMT(expected, n, "%d");
    for (int i = 0; i < n; i++) {
        ASSERT_EQ_FMT(LEVEL, allocs[i], "%d");
    }
    tier_test_end(dir);
    PASS();
}

/* Dernier recours d'une passe : listes vides, et tout l'étage date d'AVANT la
 * passe (non transférable sur disque, la passe ne l'a pas lu). Sans lui, le
 * dégagement ne déplace rien et la passe ne peut ni lire ni insérer ; avec, le
 * bas de l'étage part sur disque, compté comme reporté à une passe suivante.
 * Hors de ce cas, les blocs d'avant la passe restent dans l'étage
 * (tier_expansion_reads_pre_pass_blocks_and_moves_only_its_own_to_disk). */
TEST tier_last_resort_sends_pre_pass_blocks_to_disk_when_nothing_else_moves(void)
{
    char tmpl[64];
    const char *dir;
    tier_test_begin(tmpl, &dir);
    ASSERT(dir != NULL);
    add_marked(0, 20);
    datamanager_set_ram_limit_bytes_for_tests(1);
    ASSERT_EQ_FMT(10, stock_spill_step(10), "%d");
    ASSERT_EQ_FMT(10, stock_spill_step(10), "%d");
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");

    stock_spill_expansion_begin();
    datamanager_spill_stats_t before, after;
    stock_spill_expansion_stats(&before);
    ASSERT_EQ_FMT(10, stock_spill_relieve(10), "%d");
    stock_spill_expansion_stats(&after);
    ASSERT_EQ_FMT(10ULL, stock_spill_total_packets(), "%llu");
    ASSERT_EQ_FMT(10ULL, after.prepass_deferred_total - before.prepass_deferred_total, "%llu");
    /* La passe ne les relit pas : ils sont au-dessus de la frontière. */
    take_sink_t c = { {0}, {0}, 0, -1 };
    ASSERT_EQ_FMT(10, stock_spill_expansion_take(take_sink, &c, 100), "%d");   /* le reste de l'étage */
    ASSERT_EQ_FMT(0, stock_spill_expansion_take(take_sink, &c, 100), "%d");
    stock_spill_expansion_end();

    ASSERT_EQ_FMT(1, stock_spill_expansion_disk_usable(), "%d");
    tier_test_end(dir);
    PASS();
}

/* Cas de production (1 milliard de possibilités sous 40 Go) : le stock d'avant
 * l'expansion est dans l'ÉTAGE — des blocs d'avant la passe, qui ne partent pas
 * sur disque tant qu'elle ne les a pas lus — et tient l'essentiel du plafond ;
 * 4 fils, un dégagement qui échoue parfois, et un budget de passes
 * (`--expand-max-levels`) égal aux niveaux à franchir. Tout le stock doit
 * atteindre le niveau visé, comme sans plafond. Contre-épreuves : une passe qui
 * cesse d'approfondir au premier ADD non soulagé (le « possibilité mise en
 * attente » du journal de production) laisse du stock sous le niveau ; une
 * passe ralentie comptée dans le budget l'épuise avant la fin. Rien n'était
 * perdu — seulement pas développé. */
TEST tier_expansion_under_the_cap_reaches_the_target_level(void)
{
    char tmpl[64];
    const char *dir;
    tier_test_begin(tmpl, &dir);
    ASSERT(dir != NULL);
    stock_spill_set_tier_disk_factor_for_tests(0); /* lots disque de production */
    request = REQUEST_CONTINUE;
    /* Échelle : en production un segment ou un bloc pèse un trois-centième de
     * millième du plafond. Ici des segments de 4 et une liste tenue à 2 (des
     * blocs de quelques possibilités), sous un plafond de quelques centaines :
     * une lecture tient dans la place que l'éviction dégage. */
    stock_spill_set_segment_records_for_tests(4);
    enum { SEEDS = 400, LEVEL = 3 };
    int expected = expand_reference_count(SEEDS, LEVEL, EXPAND_MAX_LEVELS);
    ASSERT(expected > SEEDS);

    for (int i = 0; i < SEEDS; i++) seed_genesis(1);
    stock_spill_set_hot_buffer_for_tests(2, 1);
    datamanager_set_ram_limit_bytes_for_tests(1ULL << 30);
    for (int k = 0; k < 10 * SEEDS && list_size_of_pool(0) > 2; k++) {
        stock_spill_step(1);
    }
    ASSERT(stock_spill_tier_packets() >= (unsigned long long)(SEEDS - 2));

    datamanager_set_ram_limit_bytes_for_tests(datamanager_resident_bytes() * 6 / 5);
    g_flaky_relief_calls = 0;
    datamanager_set_ram_relief_hook(flaky_relief);
    datamanager_set_expansion_disk_source(&g_spill_source_usable);
    datamanager_set_expand_threads(4);
    int saved_levels = expand_max_levels;
    expand_max_levels = LEVEL - 1; /* exactement les niveaux à franchir */
    capture_stderr();
    int passes = expand_datas_to_level(LEVEL, make_expand_free_map(), make_expand_parts());
    (void)restore_stderr_size();
    expand_max_levels = saved_levels;
    datamanager_set_expand_threads(1);
    datamanager_set_expansion_disk_source(NULL);
    datamanager_set_ram_relief_hook(NULL);

    static int allocs[1 << 16];
    int n = tier_collect_all_allocs(allocs, 1 << 16);
    stock_spill_set_segment_records_for_tests(0);
    int below = 0;
    for (int i = 0; i < n; i++) {
        below += (allocs[i] < LEVEL);
    }
    (void)passes;
    ASSERT_EQ_FMT(0, below, "%d");
    ASSERT_EQ_FMT(expected, n, "%d");
    tier_test_end(dir);
    PASS();
}

void stock_spill_set_trim_for_tests(void (*fn)(void), unsigned long long threshold_bytes);
static int g_trims = 0;
static void count_trim(void) { g_trims++; }

/* Compression PROACTIVE : la liste au-dessus de son tampon descend dans
 * l'étage SANS attendre le seuil haut (90 %) du plafond. Avant, l'étage
 * n'agissait qu'à 90 % et s'arrêtait à 75 % : sous 42 Go, l'occupation se
 * stabilisait vers 32 Go, presque tout en liste chaînée.
 *
 * Ici le tampon en nombre (un million par défaut) est loin : c'est la borne
 * en OCTETS d'un petit plafond qui décide. Contre-épreuve : une condition sur
 * le seul compte laisserait les 30 possibilités en liste. */
TEST tier_compresses_the_list_down_to_its_byte_guard_under_a_small_cap(void)
{
    char tmpl[64];
    const char *dir;
    tier_test_begin(tmpl, &dir);
    ASSERT(dir != NULL);
    add_marked(0, 30);
    unsigned long long list = datamanager_pools_resident_bytes();
    unsigned long long per = list / 30;
    unsigned long long cap = list * 2;          /* liste à 50 %, loin des 90 % */
    unsigned long long guard = cap * STOCK_TIER_HOT_GUARD_PERMILLE / 1000;
    datamanager_set_ram_limit_bytes_for_tests(cap);

    ASSERT(stock_spill_step(4096) > 0);
    unsigned long long hot = datamanager_pools_resident_bytes();
    ASSERT(hot <= guard);
    ASSERT(hot + per > guard);                                        /* pas une de trop */
    ASSERT(stock_spill_tier_packets() > 0ULL);
    ASSERT_EQ_FMT(0ULL, stock_spill_total_packets(), "%llu");       /* jamais le disque ici */
    ASSERT_EQ_FMT(30ULL, datas_size() + stock_spill_tier_packets(), "%llu");
    ASSERT(datamanager_resident_bytes() < list);                     /* la RAM a baissé */

    /* Liste entre ses deux seuils : plus rien ne bouge. */
    ASSERT_EQ_FMT(0, stock_spill_step(4096), "%d");
    tier_test_end(dir);
    PASS();
}

/* Le rechargement s'arrête au milieu des deux seuils, pas à la borne haute :
 * sinon la compression proactive renverrait aussitôt dans l'étage ce qui vient
 * d'en remonter, à chaque tick. */
TEST tier_reload_and_proactive_compression_do_not_ping_pong(void)
{
    char tmpl[64];
    const char *dir;
    tier_test_begin(tmpl, &dir);
    ASSERT(dir != NULL);
    add_marked(0, 30);
    unsigned long long list = datamanager_pools_resident_bytes();
    datamanager_set_ram_limit_bytes_for_tests(1);
    for (int i = 0; i < 10; i++) {
        ASSERT_EQ_FMT(3, stock_spill_step(3), "%d");   /* 10 blocs de 3 (10 % chacun) */
    }
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");

    /* Plafond = la liste entière : borne 12,5 %, rechargement sous 5 %, arrêt
     * du rechargement à 8,75 %. Un bloc remonte (0 % -> 10 %), pas deux : à
     * 20 %, la liste dépasserait sa borne et repartirait dans l'étage. */
    datamanager_set_ram_limit_bytes_for_tests(list);
    ASSERT_EQ_FMT(3, stock_spill_step(4096), "%d");
    unsigned long long hot = datamanager_pools_resident_bytes();
    ASSERT(hot <= list * STOCK_TIER_HOT_GUARD_PERMILLE / 1000);
    /* Liste entre ses deux seuils : ni recompression ni rechargement. */
    ASSERT_EQ_FMT(0, stock_spill_step(4096), "%d");
    ASSERT_EQ_FMT(27ULL, stock_spill_tier_packets(), "%llu");
    tier_test_end(dir);
    PASS();
}

/* Les pruners ne lisent que le pool NON vérifié, et ce qu'ils rendent va dans
 * le pool vérifié. Cas de production : la liste vérifiée restait pleine ;
 * jugé sur la SOMME des deux listes, le rechargement ne partait jamais, et
 * les pruners recevaient 0 possibilité devant un étage plein de non
 * vérifiées. Chaque pool a son propre tampon : le pool affamé est rechargé
 * pour lui-même, depuis SA pile, et le pool inactif garde le sien entier —
 * sans que l'un reprenne rien à l'autre, puis tout est stable. */
TEST tier_idle_pool_keeps_its_buffer_without_starving_the_other(void)
{
    char tmpl[64];
    const char *dir;
    tier_test_begin(tmpl, &dir);
    ASSERT(dir != NULL);
    add_marked_pool(0, 20, 0);
    datamanager_set_ram_limit_bytes_for_tests(1);
    ASSERT_EQ_FMT(10, stock_spill_step(10), "%d");
    ASSERT_EQ_FMT(10, stock_spill_step(10), "%d");
    ASSERT_EQ_FMT(20ULL, stock_spill_tier_packets(), "%llu");
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");

    /* Les pruners ont tout vérifié : la liste vérifiée est pleine (son
     * maximum, 10), la non vérifiée vide. Plafond large : seul le tampon en
     * nombre décide. */
    datamanager_set_ram_limit_packets_for_tests(0);
    add_marked_pool(20, 10, 1);
    stock_spill_set_hot_buffer_for_tests(10, 5);
    datamanager_set_ram_limit_bytes_for_tests(1ULL << 30);

    ASSERT_EQ_FMT(10, stock_spill_step(10), "%d");
    ASSERT_EQ_FMT(10ULL, list_size_of_pool(0), "%llu");  /* les pruners ont de quoi */
    ASSERT_EQ_FMT(10ULL, list_size_of_pool(1), "%llu");  /* le pool inactif garde tout */
    ASSERT_EQ_FMT(10ULL, stock_spill_tier_packets(), "%llu");

    /* Chaque liste dans son tampon : ni compression ni rechargement. */
    ASSERT_EQ_FMT(0, stock_spill_step(4096), "%d");
    ASSERT_EQ_FMT(30ULL, datas_size() + stock_spill_tier_packets(), "%llu");
    tier_test_end(dir);
    PASS();
}

/* En prunage, les retours des pruners font dépasser son maximum au pool
 * vérifié presque à chaque tick. Sa compression ne doit pas prendre le pas au
 * rechargement du pool non vérifié, vide : quand elle terminait le pas, les
 * pruners vidaient leur liste et n'étaient rechargés qu'aux rares ticks sans
 * retour. Contre-épreuve : un pas qui s'arrête après la compression ne
 * recharge rien ici. */
TEST tier_reload_is_not_preempted_by_the_other_pools_compression(void)
{
    char tmpl[64];
    const char *dir;
    tier_test_begin(tmpl, &dir);
    ASSERT(dir != NULL);
    add_marked_pool(0, 20, 0);
    datamanager_set_ram_limit_bytes_for_tests(1);
    ASSERT_EQ_FMT(10, stock_spill_step(10), "%d");
    ASSERT_EQ_FMT(10, stock_spill_step(10), "%d");
    ASSERT_EQ_FMT(0ULL, list_size_of_pool(0), "%llu");

    datamanager_set_ram_limit_packets_for_tests(0);
    add_marked_pool(20, 15, 1);                  /* vérifié : 5 au-dessus de son maximum */
    stock_spill_set_hot_buffer_for_tests(10, 5);
    datamanager_set_ram_limit_bytes_for_tests(1ULL << 30);

    ASSERT_EQ_FMT(15, stock_spill_step(10), "%d");       /* 5 comprimées + 10 rechargées */
    ASSERT_EQ_FMT(10ULL, list_size_of_pool(1), "%llu");
    ASSERT_EQ_FMT(10ULL, list_size_of_pool(0), "%llu");
    ASSERT_EQ_FMT(0, stock_spill_step(10), "%d");        /* stable */
    ASSERT_EQ_FMT(35ULL, datas_size() + stock_spill_tier_packets(), "%llu");
    tier_test_end(dir);
    PASS();
}

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Rechargement à la demande : un GET qui fait passer la liste des pruners
 * sous son minimum réveille le fil du débordement tout de suite, au lieu de
 * le laisser finir son tick de 100 ms ; au-dessus du minimum, aucun réveil.
 * Contre-épreuve : sans le signal des GET, l'attente va jusqu'à son terme. */
TEST tier_get_below_the_minimum_wakes_the_spill_thread(void)
{
    char tmpl[64];
    const char *dir;
    tier_test_begin(tmpl, &dir);
    ASSERT(dir != NULL);
    datamanager_set_stock_demand_hook(stock_spill_note_demand);
    stock_spill_set_hot_buffer_for_tests(30, 10);
    datamanager_set_ram_limit_bytes_for_tests(1ULL << 30);
    add_marked(0, 20);
    (void)stock_spill_wait_next_step(0);                 /* aucun réveil en attente */
    unsigned long long wakes = stock_spill_demand_wakes();

    array_possibility_packet *r = get_last_possibility_tocheck(5);   /* reste 15 >= 10 */
    free_array_possibility_packet(r);
    ASSERT_EQ_FMT(0, stock_spill_wait_next_step(STOCK_SPILL_WAKE_MIN_MS + 40), "%d");
    ASSERT_EQ_FMT(wakes, stock_spill_demand_wakes(), "%llu");

    r = get_last_possibility_tocheck(10);                /* reste 5 < 10 */
    free_array_possibility_packet(r);
    double t0 = now_seconds();
    ASSERT_EQ_FMT(1, stock_spill_wait_next_step(5000), "%d");
    ASSERT(now_seconds() - t0 < 1.0);
    ASSERT_EQ_FMT(wakes + 1, stock_spill_demand_wakes(), "%llu");

    datamanager_set_stock_demand_hook(NULL);
    tier_test_end(dir);
    PASS();
}

/* Une liste vide devant un étage plein est une FAMINE : comptée une fois à
 * l'entrée (pas à chaque pas), avec ce qui bloque le rechargement — ici une
 * sauvegarde —, et close dès que la liste est de nouveau servie. */
TEST tier_starvation_is_counted_once_per_episode(void)
{
    char tmpl[64];
    const char *dir;
    tier_test_begin(tmpl, &dir);
    ASSERT(dir != NULL);
    add_marked(0, 20);
    datamanager_set_ram_limit_bytes_for_tests(1);
    ASSERT_EQ_FMT(10, stock_spill_step(10), "%d");
    ASSERT_EQ_FMT(10, stock_spill_step(10), "%d");   /* liste vide, 20 dans l'étage */
    unsigned long long before = stock_spill_starvations(0);
    unsigned long long before_checked = stock_spill_starvations(1); /* cumulatif : les tests précédents comptent */

    stock_spill_set_hot_buffer_for_tests(12, 5);
    datamanager_set_ram_limit_bytes_for_tests(1ULL << 30);
    datamanager_begin_maintenance();
    ASSERT_EQ_FMT(0, stock_spill_step(10), "%d");    /* rien ne recharge */
    ASSERT_EQ_FMT(0, stock_spill_step(10), "%d");
    datamanager_end_maintenance();
    ASSERT_EQ_FMT(before + 1, stock_spill_starvations(0), "%llu");
    ASSERT_EQ_FMT(before_checked, stock_spill_starvations(1), "%llu");

    ASSERT_EQ_FMT(10, stock_spill_step(10), "%d");   /* rechargé */
    ASSERT_EQ_FMT(0, stock_spill_step(10), "%d");    /* famine close */
    int m[64];
    ASSERT_EQ_FMT(10, collect_markers(m, 64), "%d"); /* des GET revident la liste */
    datamanager_begin_maintenance();
    ASSERT_EQ_FMT(0, stock_spill_step(10), "%d");
    datamanager_end_maintenance();
    ASSERT_EQ_FMT(before + 2, stock_spill_starvations(0), "%llu");

    stock_spill_step(10);
    tier_test_end(dir);
    PASS();
}

/* Une liste passée sous son minimum est rechargée jusqu'au MILIEU du tampon,
 * même quand le budget d'un pas n'y suffit pas : l'état « en rechargement »
 * tient d'un pas à l'autre. Jugé à chaque pas sur le seul minimum, le
 * rechargement s'arrêtait au premier pas qui le franchissait — en production,
 * une liste vide ne remontait qu'à 32 988 pour un milieu à 110 000.
 * Contre-épreuve : sans `g_tier_refilling`, la liste s'arrête à 16. */
TEST tier_reload_refills_the_list_to_the_middle_of_its_buffer(void)
{
    char tmpl[64];
    const char *dir;
    tier_test_begin(tmpl, &dir);
    ASSERT(dir != NULL);
    stock_spill_set_hot_buffer_for_tests(40, 10); /* milieu : 25 */
    add_marked(0, 34);
    /* Tout dans l'étage, en blocs de 2 : le rechargement se fait à la maille. */
    datamanager_set_ram_limit_bytes_for_tests(1);
    for (int i = 0; i < 100 && datas_size() > 0; i++) {
        stock_spill_step(2);
    }
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");
    ASSERT_EQ_FMT(34ULL, stock_spill_tier_packets(), "%llu");

    /* Budget de 8 par pas : trois pas franchissent le minimum (10) sans
     * atteindre le milieu. */
    datamanager_set_ram_limit_bytes_for_tests(1ULL << 30);
    for (int i = 0; i < 10; i++) {
        stock_spill_step(1);
    }
    unsigned long long n = list_size_of_pool(0);
    ASSERT(n >= 25ULL);
    ASSERT(n <= 26ULL);
    ASSERT_EQ_FMT(34ULL, n + stock_spill_tier_packets(), "%llu");

    /* Au milieu, il s'arrête : entre le minimum et le milieu, sans y être
     * tombé par le bas, rien ne recharge. */
    stock_spill_step(1);
    ASSERT_EQ_FMT(n, list_size_of_pool(0), "%llu");
    while (list_size_of_pool(0) > 20ULL) {
        free_array_possibility_packet(get_last_possibility(NULL, 1, NULL));
    }
    stock_spill_step(1);
    ASSERT_EQ_FMT(20ULL, list_size_of_pool(0), "%llu");

    stock_spill_set_hot_buffer_for_tests(STOCK_TIER_HOT_MAX_DEFAULT, STOCK_TIER_HOT_MIN_DEFAULT);
    tier_test_end(dir);
    PASS();
}

/* Le tampon est un NOMBRE de possibilités par pool, pas une part du plafond :
 * la même liste reste en liste sous 1 Go et sous 40 Go. Contre-épreuve : en
 * pourcentage du plafond, un stock de 30 possibilités resterait en liste
 * entier sous les deux. */
TEST tier_hot_buffer_does_not_depend_on_the_cap(void)
{
    char tmpl[64];
    const char *dir;
    const unsigned long long caps[2] = { 1ULL << 30, 40ULL << 30 };
    for (int k = 0; k < 2; k++) {
        tier_test_begin(tmpl, &dir);
        ASSERT(dir != NULL);
        stock_spill_set_hot_buffer_for_tests(20, 5);
        add_marked(0, 30);
        datamanager_set_ram_limit_bytes_for_tests(caps[k]);
        ASSERT_EQ_FMT(10, stock_spill_step(4096), "%d");
        ASSERT_EQ_FMT(20ULL, list_size_of_pool(0), "%llu");
        ASSERT_EQ_FMT(10ULL, stock_spill_tier_packets(), "%llu");
        ASSERT_EQ_FMT(0, stock_spill_step(4096), "%d");   /* stable */
        tier_test_end(dir);
    }
    PASS();
}

/* `n` possibilités anonymes (une case, valeur cyclique) dans le pool non
 * vérifié : pour les volumes que les marqueurs ne couvrent pas. */
static void add_anonymous(int n)
{
    array_possibility_packet arr;
    arr.size = n;
    arr.possibilities = calloc((size_t)n, sizeof(struct possibility_packet));
    for (int i = 0; i < n; i++) {
        init_empty_grid(&arr.possibilities[i]);
        arr.possibilities[i].grid[0][0] = (int16_t)(1 + i % 30);
        arr.possibilities[i].alloc = 1;
    }
    add_possibility(NULL, &arr);
    free(arr.possibilities);
}

/* Le rechargement suit le MANQUE, pas un budget fixe : un pool qui a consommé
 * plus que le budget d'un pas (4096, soit ~41 000 possibilités/s au tick de
 * 100 ms) est ramené au milieu de son tampon en UN pas. Contre-épreuve : avec
 * le budget fixe, la moitié seulement remonterait. */
TEST tier_reload_budget_follows_the_deficit(void)
{
    char tmpl[64];
    const char *dir;
    tier_test_begin(tmpl, &dir);
    ASSERT(dir != NULL);
    enum { N = 10000 };
    add_anonymous(N);
    datamanager_set_ram_limit_bytes_for_tests(1ULL << 30);

    /* Compression par petits pas (budget proactif de 8 × 10) : liste à 2000,
     * 8000 dans l'étage en blocs d'au plus 80 — un bloc est insécable, un
     * seul gros bloc remonterait en entier quel que soit le budget. */
    stock_spill_set_hot_buffer_for_tests(2000, 1000);
    for (int k = 0; k < 200 && list_size_of_pool(0) > 2000; k++) {
        stock_spill_step(10);
    }
    ASSERT_EQ_FMT(2000ULL, list_size_of_pool(0), "%llu");
    ASSERT_EQ_FMT((unsigned long long)(N - 2000), stock_spill_tier_packets(), "%llu");

    /* Tampon relevé : la liste (2000) est sous son minimum, le milieu (17 500)
     * est au-delà du stock — tout l'étage remonte, en un pas de 4096. */
    stock_spill_set_hot_buffer_for_tests(20000, 15000);
    ASSERT_EQ_FMT(N - 2000, stock_spill_step(STOCK_SPILL_BLOCK_PACKETS), "%d");
    ASSERT_EQ_FMT((unsigned long long)N, list_size_of_pool(0), "%llu");
    ASSERT_EQ_FMT(0ULL, stock_spill_tier_packets(), "%llu");
    tier_test_end(dir);
    PASS();
}

/* Le transfert étage → disque d'un pas part par LOT entier
 * (`STOCK_TIER_DISK_FACTOR` × le budget du pas), pas au budget du pas : à 4096
 * par pas, 4 ou 5 blocs de 64 Kio par `fsync`, sous le verrou de l'étage —
 * seule sortie de l'étage pendant une passe d'expansion sous plafond, elle
 * bornait la passe en production (1 milliard de possibilités, plafond 40 Go
 * tenu pendant des heures). Ici des blocs de 10 : un pas de 10 en écrit
 * STOCK_TIER_DISK_FACTOR d'une même pile, au lieu d'un seul. Contre-épreuve :
 * au facteur 1, ce pas n'en déplace que 10. */
TEST tier_disk_transfer_moves_a_whole_batch_per_step(void)
{
    char tmpl[64];
    const char *dir;
    tier_test_begin(tmpl, &dir);
    ASSERT(dir != NULL);
    stock_spill_set_tier_disk_factor_for_tests(0); /* le lot de production */
    ASSERT(STOCK_TIER_DISK_FACTOR > 1);
    const int per_file = 10 * STOCK_TIER_DISK_FACTOR;
    const int n = per_file * nb_file_possibility;
    add_anonymous(n);
    ASSERT_EQ_FMT((unsigned long long)n, datas_size(), "%llu");
    datamanager_set_ram_limit_bytes_for_tests(1);
    for (int k = 0; k < n && datas_size() > 0ULL; k++) {
        ASSERT_EQ_FMT(10, stock_spill_step(10), "%d");   /* liste → étage, blocs de 10 */
    }
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");
    ASSERT_EQ_FMT((unsigned long long)n, stock_spill_tier_packets(), "%llu");

    /* Liste vide : le bas d'une pile part sur disque, la pile ENTIÈRE. */
    ASSERT_EQ_FMT(per_file, stock_spill_step(10), "%d");
    ASSERT_EQ_FMT((unsigned long long)per_file, stock_spill_total_packets(), "%llu");
    ASSERT_EQ_FMT((unsigned long long)(n - per_file), stock_spill_tier_packets(), "%llu");
    tier_test_end(dir);
    PASS();
}

/* Au tick fixe de 100 ms, la compression liste → étage plafonnait à
 * 8 × 4096 possibilités par tick (~30 Mo de liste par seconde en production,
 * à rythme constant : budget saturé) quel que soit le débit des ADD. Un pas
 * qui laisse une liste au-dessus de son tampon le signale, et le fil enchaîne
 * le suivant ; le signal tombe dès que la liste est revenue à son tampon, et
 * un dégagement ne le touche pas. */
TEST tier_step_reports_its_backlog(void)
{
    char tmpl[64];
    const char *dir;
    tier_test_begin(tmpl, &dir);
    ASSERT(dir != NULL);
    add_anonymous(1000);
    datamanager_set_ram_limit_bytes_for_tests(1ULL << 30);
    stock_spill_set_hot_buffer_for_tests(100, 10);

    /* Budget proactif de 8 × 10 : 80 par pas, 900 à comprimer. */
    ASSERT_EQ_FMT(8 * 10, stock_spill_step(10), "%d");
    ASSERT_EQ_FMT(1, stock_spill_step_has_backlog(), "%d");
    stock_spill_relieve(1);
    ASSERT_EQ_FMT(1, stock_spill_step_has_backlog(), "%d");

    int steps = 1;
    while (stock_spill_step_has_backlog() && steps < 100) {
        ASSERT(stock_spill_step(10) > 0);
        steps++;
    }
    ASSERT_EQ_FMT(0, stock_spill_step_has_backlog(), "%d");
    ASSERT_EQ_FMT(100ULL, list_size_of_pool(0), "%llu");
    ASSERT_EQ_FMT(900ULL, stock_spill_tier_packets(), "%llu");

    /* Un pas qui ne déplace rien ne réclame pas de suite. */
    ASSERT_EQ_FMT(0, stock_spill_step(10), "%d");
    ASSERT_EQ_FMT(0, stock_spill_step_has_backlog(), "%d");
    tier_test_end(dir);
    PASS();
}

/* Même famine, le stock non vérifié étant cette fois tout sur DISQUE et
 * l'étage ne tenant que des vérifiées. « Le disque ne recharge jamais
 * par-dessus l'étage » était jugé sur l'étage ENTIER : un seul bloc vérifié
 * bloquait le disque non vérifié pour toujours. L'ordre de pile ne vaut
 * qu'au sein d'un pool. */
TEST tier_disk_reload_feeds_a_starving_pool_despite_the_other_pools_tier(void)
{
    char tmpl[64];
    const char *dir;
    tier_test_begin(tmpl, &dir);
    ASSERT(dir != NULL);
    add_marked_pool(0, 20, 0);
    datamanager_set_ram_limit_bytes_for_tests(1);
    for (int i = 0; i < 4; i++) {
        ASSERT_EQ_FMT(10, stock_spill_step(10), "%d");  /* liste -> étage -> disque */
    }
    ASSERT_EQ_FMT(0ULL, stock_spill_tier_packets(), "%llu");
    ASSERT_EQ_FMT(20ULL, stock_spill_total_packets(), "%llu");

    datamanager_set_ram_limit_packets_for_tests(0);
    add_marked_pool(0, 20, 1);   /* marqueurs réutilisés : sous 4 x ETERN_PARTS en 4x4 */
    datamanager_set_ram_limit_bytes_for_tests(1);
    ASSERT_EQ_FMT(10, stock_spill_step(10), "%d");      /* 10 vérifiées dans l'étage */
    ASSERT_EQ_FMT(10ULL, stock_spill_tier_packets(), "%llu");
    ASSERT_EQ_FMT(10ULL, list_size_of_pool(1), "%llu");

    stock_spill_set_hot_buffer_for_tests(10, 5);   /* liste vérifiée pleine */
    datamanager_set_ram_limit_bytes_for_tests(1ULL << 30);
    ASSERT(stock_spill_step(10) > 0);
    ASSERT(list_size_of_pool(0) > 0ULL);
    ASSERT_EQ_FMT(20ULL, list_size_of_pool(0) + stock_spill_total_packets(), "%llu");
    ASSERT_EQ_FMT(10ULL, stock_spill_tier_packets(), "%llu");  /* l'étage vérifié reste */
    ASSERT_EQ_FMT(10ULL, list_size_of_pool(1), "%llu");
    tier_test_end(dir);
    PASS();
}

/* `check`, `stockMemory` et GET /api/v1/stats découpent l'étage et le disque
 * par pool : chaque pool y est compté à part (possibilités, octets, segments),
 * et les deux parts somment exactement aux totaux. Non vérifié sur disque,
 * vérifié dans l'étage : une part mal attribuée se voit sur le pool vide. */
TEST tier_and_disk_counters_are_split_per_pool(void)
{
    char tmpl[64];
    const char *dir;
    tier_test_begin(tmpl, &dir);
    ASSERT(dir != NULL);
    add_marked_pool(0, 20, 0);
    datamanager_set_ram_limit_bytes_for_tests(1);
    for (int i = 0; i < 4; i++) {
        ASSERT_EQ_FMT(10, stock_spill_step(10), "%d");  /* liste -> étage -> disque */
    }
    datamanager_set_ram_limit_packets_for_tests(0);
    add_marked_pool(0, 20, 1);
    datamanager_set_ram_limit_bytes_for_tests(1);
    ASSERT_EQ_FMT(10, stock_spill_step(10), "%d");      /* 10 vérifiées dans l'étage */

    ASSERT_EQ_FMT(20ULL, stock_spill_pool_packets(STOCK_SPILL_POOL_UNCHECKED), "%llu");
    ASSERT_EQ_FMT(0ULL, stock_spill_pool_packets(STOCK_SPILL_POOL_CHECKED), "%llu");
    ASSERT(stock_spill_pool_segments(STOCK_SPILL_POOL_UNCHECKED) > 0ULL);
    ASSERT_EQ_FMT(0ULL, stock_spill_pool_segments(STOCK_SPILL_POOL_CHECKED), "%llu");
    ASSERT_EQ_FMT(stock_spill_total_segments(), stock_spill_pool_segments(STOCK_SPILL_POOL_UNCHECKED), "%llu");

    ASSERT_EQ_FMT(0ULL, stock_spill_tier_pool_packets(STOCK_SPILL_POOL_UNCHECKED), "%llu");
    ASSERT_EQ_FMT(10ULL, stock_spill_tier_pool_packets(STOCK_SPILL_POOL_CHECKED), "%llu");
    ASSERT_EQ_FMT(0ULL, stock_spill_tier_pool_bytes(STOCK_SPILL_POOL_UNCHECKED), "%llu");
    ASSERT_EQ_FMT(stock_spill_tier_bytes(), stock_spill_tier_pool_bytes(STOCK_SPILL_POOL_CHECKED), "%llu");

    /* Retour à la liste : les parts par pool redescendent avec le total. */
    stock_spill_set_hot_buffer_for_tests(24, 15);
    datamanager_set_ram_limit_bytes_for_tests(1ULL << 30);
    for (int k = 0; k < 20 && (stock_spill_tier_packets() > 0 || stock_spill_total_packets() > 0); k++) {
        stock_spill_step(4096);
        int m[64];
        collect_markers(m, 64);   /* des GET vident les listes pour relancer le rechargement */
    }
    ASSERT_EQ_FMT(0ULL, stock_spill_tier_pool_packets(STOCK_SPILL_POOL_CHECKED), "%llu");
    ASSERT_EQ_FMT(0ULL, stock_spill_tier_pool_bytes(STOCK_SPILL_POOL_CHECKED), "%llu");
    ASSERT_EQ_FMT(0ULL, stock_spill_pool_packets(STOCK_SPILL_POOL_UNCHECKED), "%llu");
    ASSERT_EQ_FMT(0ULL, stock_spill_pool_segments(STOCK_SPILL_POOL_UNCHECKED), "%llu");
    tier_test_end(dir);
    PASS();
}

/* Vide les listes (les deux pools) en vérifiant que plus AUCUNE possibilité
 * n'y porte `checked`, et range leurs marqueurs triés. */
static int drain_markers_all_unchecked(int *out, int max, int *checked_seen)
{
    int n = 0;
    *checked_seen = 0;
    while (datas_size() > 0) {
        array_possibility_packet *r = get_last_possibility(NULL, 1000, NULL);
        for (int i = 0; i < r->size; i++) {
            if (r->possibilities[i].checked) {
                (*checked_seen)++;
            }
            if (n < max) {
                out[n++] = r->possibilities[i].grid[0][0] - MARK_BASE;
            }
        }
        free_array_possibility_packet(r);
    }
    qsort(out, (size_t)n, sizeof(int), int_cmp);
    return n;
}

/* `resetChecked` ne basculait que les LISTES : le pool vérifié de l'étage RAM
 * et du disque restait vérifié (production : l'étage vérifié intact après la
 * commande). Tout bascule désormais, drapeau remis à 0 DANS chaque
 * enregistrement — la preuve étant qu'une sauvegarde puis une restauration,
 * qui routent par ce drapeau, rangent tout dans le pool non vérifié. */
TEST reset_checked_moves_the_tier_and_the_disk_too(void)
{
    char tmpl[64];
    const char *dir;
    tier_test_begin(tmpl, &dir);
    ASSERT(dir != NULL);
    datamanager_set_ram_tier_hooks(stock_spill_ram_tier_hooks());
    stock_spill_set_segment_records_for_tests(4); /* une trame par segment */

    /* Non vérifié : 10 sur disque, rien en liste. */
    datamanager_reset_rr_state_for_tests();
    add_marked_pool(0, 10, 0);
    datamanager_set_ram_limit_bytes_for_tests(1);
    for (int i = 0; i < 10 && stock_spill_pool_packets(STOCK_SPILL_POOL_UNCHECKED) < 10; i++) {
        stock_spill_step(5);
    }
    ASSERT_EQ_FMT(10ULL, stock_spill_pool_packets(STOCK_SPILL_POOL_UNCHECKED), "%llu");

    /* Vérifié : réparti sur la liste, l'étage et plusieurs segments. */
    datamanager_set_ram_limit_packets_for_tests(0);
    datamanager_reset_rr_state_for_tests();
    add_marked_pool(10, 24, 1); /* marqueurs <= 64 (build 16) */
    datamanager_set_ram_limit_bytes_for_tests(1);
    for (int i = 0; i < 40 && stock_spill_pool_packets(STOCK_SPILL_POOL_CHECKED) < 10; i++) {
        stock_spill_step(5);
    }
    datamanager_set_ram_limit_packets_for_tests(0);
    unsigned long long c_list = list_size_of_pool(1);
    unsigned long long c_tier = stock_spill_tier_pool_packets(STOCK_SPILL_POOL_CHECKED);
    unsigned long long c_disk = stock_spill_pool_packets(STOCK_SPILL_POOL_CHECKED);
    unsigned long long u_list = list_size_of_pool(0);
    unsigned long long u_tier = stock_spill_tier_pool_packets(STOCK_SPILL_POOL_UNCHECKED);
    unsigned long long u_disk = stock_spill_pool_packets(STOCK_SPILL_POOL_UNCHECKED);
    ASSERT_EQ_FMT(24ULL, c_list + c_tier + c_disk, "%llu");
    ASSERT_EQ_FMT(10ULL, u_list + u_tier + u_disk, "%llu");
    ASSERT(c_tier > 0ULL);
    ASSERT(c_disk >= 10ULL);
    ASSERT(stock_spill_pool_segments(STOCK_SPILL_POOL_CHECKED) >= 2ULL);
    ASSERT(u_disk > 0ULL);
    unsigned long long resident = datamanager_resident_bytes();

    capture_stderr();
    unsigned long long moved = reset_checked_pool();
    (void)restore_stderr_size();
    ASSERT_EQ_FMT(24ULL, moved, "%llu");

    ASSERT_EQ_FMT(0ULL, list_size_of_pool(1), "%llu");
    ASSERT_EQ_FMT(0ULL, stock_spill_tier_pool_packets(STOCK_SPILL_POOL_CHECKED), "%llu");
    ASSERT_EQ_FMT(0ULL, stock_spill_tier_pool_bytes(STOCK_SPILL_POOL_CHECKED), "%llu");
    ASSERT_EQ_FMT(0ULL, stock_spill_pool_packets(STOCK_SPILL_POOL_CHECKED), "%llu");
    ASSERT_EQ_FMT(0ULL, stock_spill_pool_segments(STOCK_SPILL_POOL_CHECKED), "%llu");
    ASSERT_EQ_FMT(u_list + c_list, list_size_of_pool(0), "%llu");
    ASSERT_EQ_FMT(u_tier + c_tier, stock_spill_tier_pool_packets(STOCK_SPILL_POOL_UNCHECKED), "%llu");
    ASSERT_EQ_FMT(u_disk + c_disk, stock_spill_pool_packets(STOCK_SPILL_POOL_UNCHECKED), "%llu");
    /* Comptabilité tenue : tout l'étage est désormais au pool non vérifié, et
     * l'occupation reste liste + étage. Pas d'égalité à l'octet près avec
     * `resident` : sous `make ZSTD=1`, changer l'octet `checked` change la
     * taille compressée d'un bloc (290 -> 294 octets mesuré en CI). */
    ASSERT_EQ_FMT(stock_spill_tier_bytes(), stock_spill_tier_pool_bytes(STOCK_SPILL_POOL_UNCHECKED), "%llu");
    ASSERT_EQ_FMT(datamanager_pools_resident_bytes() + stock_spill_tier_bytes(), datamanager_resident_bytes(),
                  "%llu");
#ifndef ETII_ZSTD
    ASSERT_EQ_FMT(resident, datamanager_resident_bytes(), "%llu"); /* blocs bruts : même taille */
#else
    (void)resident;
#endif

    /* Sauvegarde puis restauration : routées par le drapeau, toutes dans le
     * pool non vérifié. */
    char path[PATH_MAX], path_an[PATH_MAX];
    snprintf(path, sizeof path, "%s/reset.back", dir);
    snprintf(path_an, sizeof path_an, "%s/reset_an.back", dir);
    int rba = -99;
    ASSERT_EQ_FMT(BACKUP_OK,
                  consistent_backup_self_contained(path, path_an, &rba, stock_spill_snapshot,
                                                   stock_spill_embed_snapshot), "%d");

    /* Rechargement de tout (étage puis disque) : 34 marqueurs uniques, aucun
     * vérifié. */
    stock_spill_configure_tier(STOCK_TIER_HOT_MAX_DEFAULT, STOCK_TIER_HOT_MIN_DEFAULT);
    datamanager_set_ram_limit_bytes_for_tests(1ULL << 30);
    int m[64], k = 0, checked_seen = 0, got = 0;
    for (; k < 40 && (stock_spill_tier_packets() > 0 || stock_spill_total_packets() > 0); k++) {
        stock_spill_step(100);
        ASSERT_EQ_FMT(0ULL, list_size_of_pool(1), "%llu");
        int seen = 0;
        got += drain_markers_all_unchecked(m + got, 64 - got, &seen);
        checked_seen += seen;
    }
    got += drain_markers_all_unchecked(m + got, 64 - got, &checked_seen);
    ASSERT_EQ_FMT(0, checked_seen, "%d");
    ASSERT_EQ_FMT(34, got, "%d");
    qsort(m, 34, sizeof(int), int_cmp);
    for (int i = 0; i < 34; i++) {
        ASSERT_EQ_FMT(i, m[i], "%d");
    }

    datamanager_set_ram_limit_packets_for_tests(0);
    capture_stderr();
    datamanager_begin_maintenance();
    int rc = restore(path);
    datamanager_end_maintenance();
    (void)restore_stderr_size();
    ASSERT_EQ_FMT(0, rc, "%d");
    ASSERT_EQ_FMT(0ULL, list_size_of_pool(1), "%llu");
    ASSERT_EQ_FMT(34ULL, list_size_of_pool(0), "%llu");

    stock_spill_set_segment_records_for_tests(0);
    datamanager_set_ram_tier_hooks(NULL);
    tier_test_end(dir);
    PASS();
}

/* Un segment vérifié illisible reste vérifié, avec tout ce qui est au-dessus :
 * ni perte, ni doublon (le nouveau segment non vérifié n'est acquis qu'une
 * fois l'ancien entièrement réécrit). */
TEST reset_checked_leaves_an_unreadable_segment_checked_without_duplicating(void)
{
    char tmpl[64];
    const char *dir;
    tier_test_begin(tmpl, &dir);
    ASSERT(dir != NULL);
    datamanager_set_ram_tier_hooks(stock_spill_ram_tier_hooks());
    stock_spill_set_segment_records_for_tests(4);
    datamanager_reset_rr_state_for_tests();
    add_marked_pool(0, 10, 1);
    datamanager_set_ram_limit_bytes_for_tests(1);
    for (int i = 0; i < 20 && stock_spill_pool_packets(STOCK_SPILL_POOL_CHECKED) < 10; i++) {
        stock_spill_step(5);
    }
    datamanager_set_ram_limit_packets_for_tests(0);
    ASSERT_EQ_FMT(10ULL, stock_spill_pool_packets(STOCK_SPILL_POOL_CHECKED), "%llu");
    ASSERT_EQ_FMT(2ULL, stock_spill_pool_segments(STOCK_SPILL_POOL_CHECKED), "%llu");

    /* Abîme la trame du segment du HAUT (le second) : le bas bascule. */
    char seg[PATH_MAX];
    int found = 0;
    for (int f = 0; f < nb_file_possibility && !found; f++) {
        snprintf(seg, sizeof seg, "%s/spill_c_%d_2.dat", dir, f);
        found = (access(seg, F_OK) == 0);
    }
    ASSERT(found);
    FILE *fs = fopen(seg, "r+b");
    ASSERT(fs != NULL);
    fputc('X', fs);   /* magic d'en-tête de trame */
    fclose(fs);

    capture_stderr();
    unsigned long long moved = reset_checked_pool();
    (void)restore_stderr_size();
    ASSERT_EQ_FMT(5ULL, moved, "%llu");
    ASSERT_EQ_FMT(5ULL, stock_spill_pool_packets(STOCK_SPILL_POOL_UNCHECKED), "%llu");
    ASSERT_EQ_FMT(5ULL, stock_spill_pool_packets(STOCK_SPILL_POOL_CHECKED), "%llu");
    ASSERT_EQ_FMT(1ULL, stock_spill_pool_segments(STOCK_SPILL_POOL_CHECKED), "%llu");

    stock_spill_set_segment_records_for_tests(0);
    datamanager_set_ram_tier_hooks(NULL);
    tier_test_end(dir);
    PASS();
}

/* Bascule progressive de `resetChecked` : le travail se fait HORS de la
 * fenêtre de maintenance, débordement actif. En production, 1,6 G
 * possibilités ont basculé en 25 min dans la fenêtre, pendant lesquelles le
 * pool non vérifié n'était plus rechargé. Le crochet de test est appelé entre
 * le travail fait hors verrou et son acquisition : il relève l'état de la
 * fenêtre à ce moment-là. */
static int g_reset_hook_calls = 0;
static int g_reset_hook_in_maintenance = 0;

static void reset_hook_note_maintenance(int is_disk, int file_index)
{
    (void)is_disk;
    (void)file_index;
    __atomic_add_fetch(&g_reset_hook_calls, 1, __ATOMIC_RELAXED);
    if (datamanager_is_maintenance_active()) {
        __atomic_add_fetch(&g_reset_hook_in_maintenance, 1, __ATOMIC_RELAXED);
    }
}

/* Remplit `count` (indexé par marqueur relatif) en rechargeant et vidant tout
 * le stock ; échoue si une possibilité revient vérifiée. */
static int reload_all_marker_counts(int *count, int max_marker, int *checked_seen)
{
    int m[128], total = 0;
    *checked_seen = 0;
    stock_spill_configure_tier(STOCK_TIER_HOT_MAX_DEFAULT, STOCK_TIER_HOT_MIN_DEFAULT);
    datamanager_set_ram_limit_bytes_for_tests(1ULL << 30);
    for (int k = 0; k < 60; k++) {
        int seen = 0;
        int got = drain_markers_all_unchecked(m, 128, &seen);
        *checked_seen += seen;
        for (int i = 0; i < got; i++) {
            if (m[i] >= 0 && m[i] < max_marker) {
                count[m[i]]++;
            }
        }
        total += got;
        if (stock_spill_tier_packets() == 0 && stock_spill_total_packets() == 0) {
            break;
        }
        stock_spill_step(100);
    }
    datamanager_set_ram_limit_packets_for_tests(0);
    return total;
}

TEST reset_checked_does_the_bulk_outside_the_maintenance_window(void)
{
    char tmpl[64];
    const char *dir;
    tier_test_begin(tmpl, &dir);
    ASSERT(dir != NULL);
    datamanager_set_ram_tier_hooks(stock_spill_ram_tier_hooks());
    stock_spill_set_segment_records_for_tests(4);
    stock_spill_set_reset_checked_workers_for_tests(4);

    /* Vérifié : réparti sur l'étage et plusieurs segments, plusieurs files. */
    datamanager_reset_rr_state_for_tests();
    add_marked_pool(0, 30, 1);
    datamanager_set_ram_limit_bytes_for_tests(1);
    for (int i = 0; i < 60 && stock_spill_pool_packets(STOCK_SPILL_POOL_CHECKED) < 15; i++) {
        stock_spill_step(5);
    }
    datamanager_set_ram_limit_packets_for_tests(0);
    unsigned long long c_tier = stock_spill_tier_pool_packets(STOCK_SPILL_POOL_CHECKED);
    unsigned long long c_disk = stock_spill_pool_packets(STOCK_SPILL_POOL_CHECKED);
    ASSERT(c_tier > 0ULL);
    ASSERT(c_disk >= 15ULL);
    ASSERT_EQ_FMT(30ULL, list_size_of_pool(1) + c_tier + c_disk, "%llu");

    g_reset_hook_calls = 0;
    g_reset_hook_in_maintenance = 0;
    stock_spill_set_reset_checked_before_commit_for_tests(reset_hook_note_maintenance);
    capture_stderr();
    unsigned long long moved = reset_checked_pool();
    (void)restore_stderr_size();
    stock_spill_set_reset_checked_before_commit_for_tests(NULL);
    ASSERT_EQ_FMT(30ULL, moved, "%llu");
    ASSERT(g_reset_hook_calls > 0);                 /* la phase progressive a tourné… */
    ASSERT_EQ_FMT(0, g_reset_hook_in_maintenance, "%d"); /* …hors de la fenêtre */
    ASSERT_EQ_FMT(0ULL, stock_spill_tier_pool_packets(STOCK_SPILL_POOL_CHECKED), "%llu");
    ASSERT_EQ_FMT(0ULL, stock_spill_pool_packets(STOCK_SPILL_POOL_CHECKED), "%llu");
    ASSERT_EQ_FMT(0ULL, list_size_of_pool(1), "%llu");

    int count[64] = { 0 }, checked_seen = 0;
    ASSERT_EQ_FMT(30, reload_all_marker_counts(count, 64, &checked_seen), "%d");
    ASSERT_EQ_FMT(0, checked_seen, "%d");
    for (int i = 0; i < 30; i++) {
        ASSERT_EQ_FMT(1, count[i], "%d");
    }

    stock_spill_set_reset_checked_workers_for_tests(0);
    stock_spill_set_segment_records_for_tests(0);
    datamanager_set_ram_tier_hooks(NULL);
    tier_test_end(dir);
    PASS();
}

/* Le bas de la pile vérifiée est réécrit hors verrou. S'il redevient sommet
 * pendant ce temps — rechargé en partie, puis ramené à son sommet logique et
 * recouvert par un nouveau segment —, il porte le même numéro, reste le bas
 * de la pile et n'est plus le sommet : seul `drops` dit qu'il a changé.
 * Contre-épreuve : sans ce contrôle, la réécriture (0..3) est acquise alors
 * que 2 et 3 ont déjà été servis — ils reviennent deux fois. */
static int g_reset_disk_hook_done = 0;

static void reset_hook_reload_then_roll(int is_disk, int file_index)
{
    if (!is_disk || file_index != 0 || g_reset_disk_hook_done) {
        return;
    }
    g_reset_disk_hook_done = 1;
    int m[64];
    /* Rechargement par le sommet, une trame à la fois : 6-7, 4-5 (le segment
     * 2 se vide, le 1 redevient sommet), puis 2-3. */
    stock_spill_set_hot_buffer_for_tests(3, 1);
    datamanager_set_ram_limit_bytes_for_tests(1ULL << 30);
    for (int k = 0; k < 3; k++) {
        stock_spill_step(2);
        (void)list_markers_sorted(m, 64);
    }
    /* Nouvelles possibilités vérifiées sur la même file : le segment 1 est
     * ramené à 0-1 et la pile roule. */
    datamanager_set_ram_limit_packets_for_tests(0);
    datamanager_reset_rr_state_for_tests();
    add_marked_pool(10, 4, 1);
    datamanager_set_ram_limit_bytes_for_tests(1);
    for (int k = 0; k < 20 && stock_spill_pool_segments(STOCK_SPILL_POOL_CHECKED) < 2; k++) {
        stock_spill_step(2);
    }
    datamanager_set_ram_limit_packets_for_tests(0);
}

TEST reset_checked_progressive_drops_a_segment_that_became_top_meanwhile(void)
{
    char tmpl[64];
    const char *dir;
    tier_test_begin(tmpl, &dir);
    ASSERT(dir != NULL);
    datamanager_set_ram_tier_hooks(stock_spill_ram_tier_hooks());
    stock_spill_set_segment_records_for_tests(4);
    stock_spill_set_reset_checked_workers_for_tests(1);

    /* Pile vérifiée de la file 0 : segment 1 = 0..3, segment 2 = 4..7. */
    for (int s = 0; s < 2; s++) {
        datamanager_reset_rr_state_for_tests();
        add_marked_pool(4 * s, 4, 1);
        datamanager_set_ram_limit_bytes_for_tests(1);
        for (int k = 0; k < 4; k++) {
            ASSERT_EQ_FMT(2, stock_spill_step(2), "%d");
        }
        datamanager_set_ram_limit_packets_for_tests(0);
    }
    ASSERT_EQ_FMT(8ULL, stock_spill_pool_packets(STOCK_SPILL_POOL_CHECKED), "%llu");
    ASSERT_EQ_FMT(2ULL, stock_spill_pool_segments(STOCK_SPILL_POOL_CHECKED), "%llu");

    g_reset_disk_hook_done = 0;
    stock_spill_set_reset_checked_before_commit_for_tests(reset_hook_reload_then_roll);
    unsigned long long moved = stock_spill_ram_tier_hooks()->reset_checked_progressive();
    stock_spill_set_reset_checked_before_commit_for_tests(NULL);
    ASSERT_EQ_FMT(1, g_reset_disk_hook_done, "%d");

    capture_stderr();
    moved += reset_checked_pool();
    (void)restore_stderr_size();
    ASSERT_EQ_FMT(6ULL, moved, "%llu"); /* 0, 1, 10..13 : basculées une fois chacune */

    /* Restent 0, 1 et 10..13, chacun une fois ; 2..7 ont été servis. */
    int count[64] = { 0 }, checked_seen = 0;
    ASSERT_EQ_FMT(6, reload_all_marker_counts(count, 64, &checked_seen), "%d");
    ASSERT_EQ_FMT(0, checked_seen, "%d");
    int expected[6] = { 0, 1, 10, 11, 12, 13 };
    for (int i = 0; i < 6; i++) {
        ASSERT_EQ_FMT(1, count[expected[i]], "%d");
    }
    for (int i = 2; i <= 7; i++) {
        ASSERT_EQ_FMT(0, count[i], "%d"); /* déjà servies : jamais rendues */
    }

    stock_spill_set_reset_checked_workers_for_tests(0);
    stock_spill_set_segment_records_for_tests(0);
    datamanager_set_ram_tier_hooks(NULL);
    tier_test_end(dir);
    PASS();
}

/* Un bloc de l'étage vérifié est recompressé hors verrou. Si le débordement
 * l'a envoyé sur disque entre-temps, la copie est jetée : ni chaînée en
 * double, ni retirée d'une pile à laquelle il n'appartient plus. Le bloc est
 * rattrapé sur disque. */
static int g_reset_tier_hook_done = 0;

static void reset_hook_send_tier_to_disk(int is_disk, int file_index)
{
    if (is_disk || g_reset_tier_hook_done) {
        return;
    }
    g_reset_tier_hook_done = 1;
    (void)file_index;
    datamanager_set_ram_limit_bytes_for_tests(1);
    for (int k = 0; k < 20 && stock_spill_tier_pool_packets(STOCK_SPILL_POOL_CHECKED) > 0; k++) {
        stock_spill_step(5);
    }
    datamanager_set_ram_limit_packets_for_tests(0);
}

TEST reset_checked_progressive_drops_a_tier_block_moved_meanwhile(void)
{
    char tmpl[64];
    const char *dir;
    tier_test_begin(tmpl, &dir);
    ASSERT(dir != NULL);
    datamanager_set_ram_tier_hooks(stock_spill_ram_tier_hooks());
    stock_spill_set_reset_checked_workers_for_tests(1);
    datamanager_reset_rr_state_for_tests();
    add_marked_pool(0, 20, 1);
    datamanager_set_ram_limit_bytes_for_tests(1);
    for (int k = 0; k < 4; k++) {
        stock_spill_step(5); /* liste -> étage seulement */
    }
    datamanager_set_ram_limit_packets_for_tests(0);
    ASSERT(stock_spill_tier_pool_packets(STOCK_SPILL_POOL_CHECKED) > 0ULL);

    g_reset_tier_hook_done = 0;
    stock_spill_set_reset_checked_before_commit_for_tests(reset_hook_send_tier_to_disk);
    unsigned long long moved = stock_spill_ram_tier_hooks()->reset_checked_progressive();
    stock_spill_set_reset_checked_before_commit_for_tests(NULL);
    ASSERT_EQ_FMT(1, g_reset_tier_hook_done, "%d");

    capture_stderr();
    moved += reset_checked_pool();
    (void)restore_stderr_size();
    ASSERT_EQ_FMT(20ULL, moved, "%llu"); /* chaque possibilité basculée une fois */

    int count[64] = { 0 }, checked_seen = 0;
    ASSERT_EQ_FMT(20, reload_all_marker_counts(count, 64, &checked_seen), "%d");
    ASSERT_EQ_FMT(0, checked_seen, "%d");
    for (int i = 0; i < 20; i++) {
        ASSERT_EQ_FMT(1, count[i], "%d");
    }

    stock_spill_set_reset_checked_workers_for_tests(0);
    datamanager_set_ram_tier_hooks(NULL);
    tier_test_end(dir);
    PASS();
}

/* Pendant l'éviction (au-dessus de 90 %, jusqu'à 75 %), le pool non vérifié
 * affamé se recharge depuis le disque au même pas : l'éviction porte sur
 * l'étage vérifié, pas sur lui. En prunage, les retours des pruners
 * déclenchaient une éviction toutes les ~53 min, et le rechargement suspendu
 * pendant ses ~80 s laissait les pruners à vide 3 à 48 s à chaque fois
 * (production, 08-09/10). Contre-épreuve : avec le rechargement suspendu en
 * mode éviction, la liste non vérifiée reste vide après le pas. */
TEST tier_eviction_still_reloads_a_starving_pool(void)
{
    char tmpl[64];
    const char *dir;
    tier_test_begin(tmpl, &dir);
    ASSERT(dir != NULL);

    /* Non vérifié : tout sur disque, liste et étage vides. */
    add_marked_pool(0, 10, 0);
    datamanager_set_ram_limit_bytes_for_tests(1);
    for (int i = 0; i < 20 && stock_spill_pool_packets(STOCK_SPILL_POOL_UNCHECKED) < 10; i++) {
        stock_spill_step(5);
    }
    ASSERT_EQ_FMT(10ULL, stock_spill_pool_packets(STOCK_SPILL_POOL_UNCHECKED), "%llu");
    ASSERT_EQ_FMT(0ULL, list_size_of_pool(0), "%llu");

    /* Vérifié : dans l'étage (les retours des pruners). */
    datamanager_set_ram_limit_packets_for_tests(0);
    add_marked_pool(10, 20, 1);
    datamanager_set_ram_limit_bytes_for_tests(1);
    for (int i = 0; i < 4; i++) {
        stock_spill_step(5);
    }
    ASSERT(stock_spill_tier_pool_packets(STOCK_SPILL_POOL_CHECKED) > 0ULL);

    /* Mode éviction (entré au plafond minuscule), occupation ramenée entre
     * 75 % et 90 % : le pas évince l'étage vérifié vers le disque ET recharge
     * le non vérifié. */
    stock_spill_set_hot_buffer_for_tests(4, 2);
    unsigned long long resident = datamanager_resident_bytes();
    datamanager_set_ram_limit_bytes_for_tests(resident * 100 / 85);
    (void)stock_spill_step(100);
    ASSERT(list_size_of_pool(0) > 0ULL);
    ASSERT_EQ_FMT(10ULL, list_size_of_pool(0) + stock_spill_pool_packets(STOCK_SPILL_POOL_UNCHECKED), "%llu");
    tier_test_end(dir);
    PASS();
}

/* Sans disque, l'éviction fait descendre les listes SOUS leur tampon, vers
 * l'étage : les recharger les y renverrait aussitôt, à chaque pas suivant
 * tant que l'étage seul tient le plafond au-dessus de 75 %. Le rechargement
 * attend la fin de l'épisode d'éviction. Contre-épreuve : jugé au pas près,
 * le second pas (listes vides, rien à évincer) recharge. */
TEST tier_eviction_without_disk_does_not_reload_what_it_evicts(void)
{
    stock_spill_set_tier_enabled_for_tests(1);
    capture_stderr();
    stock_spill_configure("/proc/etii-inexistant/spill", nb_file_possibility);
    (void)restore_stderr_size();
    drain_datamanager();
    datamanager_set_ram_limit_packets_for_tests(0);

    /* Un étage qui pèse à lui seul plus de 75 % du plafond (marqueurs
     * répétés : <= 64 en build 16)… */
    for (int k = 0; k < 16; k++) {
        add_marked(0, 25);
    }
    datamanager_set_ram_limit_bytes_for_tests(1);
    for (int i = 0; i < 40 && datas_size() > 0; i++) {
        stock_spill_step(100);
    }
    ASSERT_EQ_FMT(400ULL, stock_spill_tier_packets(), "%llu");
    /* …et une liste sous son tampon en nombre. */
    datamanager_set_ram_limit_packets_for_tests(0);
    add_marked(0, 15);
    stock_spill_set_hot_buffer_for_tests(40, 20);
    unsigned long long tier_bytes = stock_spill_tier_bytes();
    datamanager_set_ram_limit_bytes_for_tests(datamanager_resident_bytes() * 100 / 95);
    ASSERT(tier_bytes * 100 > datamanager_ram_limit_bytes() * 75);

    capture_stderr();
    (void)stock_spill_step(1000);  /* la liste part dans l'étage */
    unsigned long long after_first = datas_size();
    (void)stock_spill_step(1000);  /* plus rien à évincer */
    unsigned long long after_second = datas_size();
    (void)restore_stderr_size();
    ASSERT_EQ_FMT(0ULL, after_first, "%llu");
    ASSERT_EQ_FMT(0ULL, after_second, "%llu");
    ASSERT_EQ_FMT(415ULL, stock_spill_tier_packets(), "%llu");

    datamanager_set_ram_limit_packets_for_tests(0);
    drain_datamanager();
    stock_spill_configure_tier(STOCK_TIER_HOT_MAX_DEFAULT, STOCK_TIER_HOT_MIN_DEFAULT);
    capture_stderr();
    stock_spill_configure("/proc/etii-inexistant/spill", nb_file_possibility); /* vide l'étage */
    (void)restore_stderr_size();
    PASS();
}

/* Sans étage, même défaut : le rechargement disque partait sous 25 % de
 * l'occupation TOTALE, qu'une liste vérifiée à 50 % du plafond ne laissait
 * jamais atteindre. L'hystérésis 25 %/75 % est tenue par pool, chacun sur la
 * moitié des seuils quand les deux ont du stock. */
TEST spill_reload_feeds_a_starving_pool_beside_a_full_one(void)
{
    char tmpl[64];
    stock_spill_set_tier_enabled_for_tests(0);
    const char *dir = make_tmp_spill_dir(tmpl);
    ASSERT(dir != NULL);
    stock_spill_configure(dir, nb_file_possibility);
    drain_datamanager();
    datamanager_set_ram_limit_packets_for_tests(0);
    add_marked_pool(0, 20, 0);
    datamanager_set_ram_limit_bytes_for_tests(1);
    for (int i = 0; i < 10 && list_size_of_pool(0) > 0; i++) {
        stock_spill_step(10);
    }
    ASSERT_EQ_FMT(20ULL, stock_spill_total_packets(), "%llu");

    datamanager_set_ram_limit_packets_for_tests(0);
    add_marked_pool(20, 10, 1);
    datamanager_set_ram_limit_bytes_for_tests(datamanager_pool_resident_bytes(1) * 2);
    ASSERT(stock_spill_step(10) > 0);
    ASSERT(list_size_of_pool(0) > 0ULL);
    ASSERT_EQ_FMT(10ULL, list_size_of_pool(1), "%llu");
    ASSERT_EQ_FMT(20ULL, list_size_of_pool(0) + stock_spill_total_packets(), "%llu");

    stock_spill_set_tier_enabled_for_tests(1);
    tier_test_end(dir);
    PASS();
}

/* La mémoire libérée par l'éviction est rendue au système (malloc_trim) une
 * fois le seuil d'octets atteint — puis pas avant l'intervalle minimal. */
TEST tier_eviction_returns_memory_to_the_system_by_batches(void)
{
    char tmpl[64];
    const char *dir;
    tier_test_begin(tmpl, &dir);
    ASSERT(dir != NULL);
    g_trims = 0;
    stock_spill_set_trim_for_tests(count_trim, 1);
    add_marked(0, 30);
    datamanager_set_ram_limit_bytes_for_tests(datamanager_pools_resident_bytes() * 2);
    ASSERT(stock_spill_step(4096) > 0);
    ASSERT_EQ(1, g_trims);
    /* Nouvelle éviction dans la foulée : l'intervalle minimal l'empêche. */
    add_marked(30, 30);
    ASSERT(stock_spill_step(4096) > 0);
    ASSERT_EQ(1, g_trims);
    stock_spill_set_trim_for_tests(NULL, 0);

    /* Reliquat : l'éviction s'arrête (liste à son tampon) avec moins que le
     * seuil en attente — il est rendu quand même, au premier pas sans rien à
     * déplacer, dès un huitième du seuil. */
    tier_test_end(dir);
    tier_test_begin(tmpl, &dir);
    ASSERT(dir != NULL);
    add_marked(0, 30);
    unsigned long long list = datamanager_pools_resident_bytes();
    datamanager_set_ram_limit_bytes_for_tests(list * 2);
    /* Seuil = 2 × la liste : l'éviction (les trois quarts de la liste) n'y
     * suffit pas, mais le reliquat dépasse le huitième du seuil. */
    g_trims = 0;
    stock_spill_set_trim_for_tests(count_trim, list * 2);
    ASSERT(stock_spill_step(4096) > 0);
    ASSERT_EQ(0, g_trims);
    ASSERT_EQ_FMT(0, stock_spill_step(4096), "%d");   /* plus rien à évincer */
    ASSERT_EQ(1, g_trims);
    stock_spill_set_trim_for_tests(NULL, 0);
    tier_test_end(dir);
    PASS();
}

TEST trim_decision_needs_both_bytes_and_time(void)
{
    ASSERT_EQ(1, stock_spill_should_trim(512, 1000, 0, 512));       /* jamais encore */
    ASSERT_EQ(0, stock_spill_should_trim(511, 1000, 0, 512));
    ASSERT_EQ(0, stock_spill_should_trim(4096, 1000 + STOCK_TIER_TRIM_MIN_INTERVAL_SEC - 1, 1000, 512));
    ASSERT_EQ(1, stock_spill_should_trim(4096, 1000 + STOCK_TIER_TRIM_MIN_INTERVAL_SEC, 1000, 512));
    PASS();
}

/* Les deux primitives compactes du datamanager : un seul essai de verrou (une
 * sauvegarde gèle les pools puis prend le verrou de l'étage), et une
 * réinsertion tout-ou-rien. */
TEST pool_compact_primitives_never_wait_and_refill_all_or_nothing(void)
{
    drain_datamanager();
    datamanager_set_ram_limit_packets_for_tests(0);
    add_marked(0, 5);
    uint8_t raw[STOCK_TIER_BLOCK_BYTES];
    size_t used = 0;

    lock_all_file();
    ASSERT_EQ_FMT(0, datamanager_pool_drain_head_compact(0, 0, raw, sizeof raw, 100, &used), "%d");
    unlock_all_file();

    ASSERT_EQ_FMT(3, datamanager_pool_drain_head_compact(0, 0, raw, sizeof raw, 3, &used), "%d");
    ASSERT_EQ_FMT(2ULL, datas_size(), "%llu");

    lock_all_file();
    ASSERT_EQ_FMT(0, datamanager_pool_refill_compact(0, 0, raw, used), "%d");
    unlock_all_file();
    ASSERT_EQ_FMT(-1, datamanager_pool_refill_compact(0, 0, raw, used - 1), "%d");
    ASSERT_EQ_FMT(2ULL, datas_size(), "%llu");
    ASSERT_EQ_FMT(3, datamanager_pool_refill_compact(0, 0, raw, used), "%d");
    ASSERT_EQ_FMT(5ULL, datas_size(), "%llu");
    drain_datamanager();
    PASS();
}

/* Le tampon exige min >= 1, min < max, et plusieurs blocs d'écart : sinon un
 * rechargement (qui dépasse son arrêt d'un bloc) atteindrait la compression. */
TEST tier_hot_buffer_must_keep_min_well_under_max(void)
{
    ASSERT_EQ_FMT(-1, stock_spill_configure_tier(250000, 1000000), "%d");
    ASSERT_EQ_FMT(-1, stock_spill_configure_tier(250000, 250000), "%d");
    ASSERT_EQ_FMT(-1, stock_spill_configure_tier(1000000, 0), "%d");
    ASSERT_EQ_FMT(-1, stock_spill_configure_tier(STOCK_TIER_HOT_GAP_MIN, 1), "%d");
    ASSERT_EQ_FMT(0, stock_spill_configure_tier(STOCK_TIER_HOT_GAP_MIN + 1, 1), "%d");
    ASSERT_EQ_FMT(0, stock_spill_configure_tier(STOCK_TIER_HOT_MAX_DEFAULT, STOCK_TIER_HOT_MIN_DEFAULT), "%d");
    PASS();
}

SUITE(stock_spill_tier_suite)
{
    RUN_TEST(tier_eviction_moves_the_cold_head_into_ram_blocks);
    RUN_TEST(tier_reload_returns_the_newest_block_first);
    RUN_TEST(tier_reload_is_driven_by_the_list_not_the_total);
    RUN_TEST(tier_overflow_goes_to_disk_from_the_bottom_and_comes_back_in_order);
    RUN_TEST(tier_blocks_reach_the_disk_in_their_stored_form);
    RUN_TEST(tier_rollover_after_a_partial_reload_trims_the_left_segment);
    RUN_TEST(tier_eviction_stops_the_list_at_its_buffer);
    RUN_TEST(tier_relieves_the_cap_without_a_usable_spill_dir);
    RUN_TEST(tier_is_saved_by_backup_and_replaced_by_restore);
    RUN_TEST(tier_takes_the_overflow_of_a_restore_under_a_lower_cap);
    RUN_TEST(restore_under_a_cap_goes_straight_into_the_tier);
    RUN_TEST(restore_into_the_tier_makes_room_on_disk);
    RUN_TEST(restore_falls_back_to_the_lists_when_the_tier_refuses);
    RUN_TEST(restore_direct_copy_survives_records_split_across_reads);
    RUN_TEST(import_workers_keep_the_push_order_of_each_stack);
    RUN_TEST(restore_is_the_same_whatever_the_number_of_compression_workers);
    RUN_TEST(parallel_import_never_refuses_a_block_it_has_room_for);
    RUN_TEST(import_push_retries_the_disk_after_an_in_flight_block_lands);
    RUN_TEST(tier_disk_batch_crosses_segment_boundaries);
    RUN_TEST(tier_disk_batch_failure_keeps_the_synced_prefix);
    RUN_TEST(tier_expansion_reads_pre_pass_blocks_and_moves_only_its_own_to_disk);
    RUN_TEST(tier_expansion_under_the_cap_reaches_the_target_level);
    RUN_TEST(tier_last_resort_sends_pre_pass_blocks_to_disk_when_nothing_else_moves);
    RUN_TEST(tier_expansion_waits_for_room_instead_of_suspending_the_pass);
    RUN_TEST(pool_compact_primitives_never_wait_and_refill_all_or_nothing);
    RUN_TEST(tier_hot_buffer_must_keep_min_well_under_max);
    RUN_TEST(tier_compresses_the_list_down_to_its_byte_guard_under_a_small_cap);
    RUN_TEST(tier_reload_and_proactive_compression_do_not_ping_pong);
    RUN_TEST(tier_idle_pool_keeps_its_buffer_without_starving_the_other);
    RUN_TEST(tier_reload_is_not_preempted_by_the_other_pools_compression);
    RUN_TEST(tier_get_below_the_minimum_wakes_the_spill_thread);
    RUN_TEST(tier_starvation_is_counted_once_per_episode);
    RUN_TEST(tier_reload_refills_the_list_to_the_middle_of_its_buffer);
    RUN_TEST(tier_hot_buffer_does_not_depend_on_the_cap);
    RUN_TEST(tier_reload_budget_follows_the_deficit);
    RUN_TEST(tier_disk_transfer_moves_a_whole_batch_per_step);
    RUN_TEST(tier_step_reports_its_backlog);
    RUN_TEST(tier_disk_reload_feeds_a_starving_pool_despite_the_other_pools_tier);
    RUN_TEST(tier_and_disk_counters_are_split_per_pool);
    RUN_TEST(reset_checked_moves_the_tier_and_the_disk_too);
    RUN_TEST(reset_checked_leaves_an_unreadable_segment_checked_without_duplicating);
    RUN_TEST(reset_checked_does_the_bulk_outside_the_maintenance_window);
    RUN_TEST(reset_checked_progressive_drops_a_segment_that_became_top_meanwhile);
    RUN_TEST(reset_checked_progressive_drops_a_tier_block_moved_meanwhile);
    RUN_TEST(tier_eviction_still_reloads_a_starving_pool);
    RUN_TEST(tier_eviction_without_disk_does_not_reload_what_it_evicts);
    RUN_TEST(spill_reload_feeds_a_starving_pool_beside_a_full_one);
    RUN_TEST(tier_eviction_returns_memory_to_the_system_by_batches);
    RUN_TEST(trim_decision_needs_both_bytes_and_time);
}

SUITE(stock_spill_suite)
{
    /* Mécanique disque historique : liste -> disque, sans étage (cf.
     * stock_spill_tier_suite pour la chaîne avec étage). */
    stock_spill_set_tier_enabled_for_tests(0);
    RUN_TEST(configure_creates_directory_and_starts_empty);
    RUN_TEST(configure_degrades_gracefully_when_directory_unwritable);
    RUN_TEST(configure_purges_matching_segments_and_spares_others);
    RUN_TEST(step_is_noop_without_ram_cap);
    RUN_TEST(evict_removes_oldest_first_and_conserves_total);
    RUN_TEST(reload_restores_evicted_data_when_ram_drops_and_preserves_fields);
    RUN_TEST(step_logs_eviction_and_reload_transitions_to_events_log);
    RUN_TEST(evict_and_reload_span_multiple_segments);
    RUN_TEST(no_reload_while_an_expansion_is_running);
    RUN_TEST(eviction_still_runs_during_an_expansion);
    RUN_TEST(expansion_develops_the_spilled_stock_too);
    RUN_TEST(expansion_pass_never_retakes_its_own_evicted_children);
    RUN_TEST(expansion_threads_develop_the_spilled_stock_and_never_retake_their_children);
    RUN_TEST(expansion_take_reads_bottom_first_and_commits_only_on_success);
    RUN_TEST(snapshot_of_a_stack_consumed_from_the_bottom_restores_exactly);
    RUN_TEST(expansion_stats_count_evictions_and_reloads);
    RUN_TEST(evict_after_a_partial_reload_neither_duplicates_nor_loses);
    RUN_TEST(evict_after_a_partial_reload_leaves_a_linked_snapshot_intact);
    RUN_TEST(snapshot_links_full_segments_and_copies_tail);
    RUN_TEST(snapshot_refreshes_stale_reused_segment_number);
    RUN_TEST(restore_snapshot_no_collision_round_trip_preserves_data);
    RUN_TEST(restore_snapshot_collision_repacks_when_stock_files_shrinks);
    RUN_TEST(restore_snapshot_no_collision_missing_segment_reports_partial);
    RUN_TEST(restore_snapshot_collision_missing_segment_reports_partial);
    RUN_TEST(restore_under_a_ram_cap_loses_nothing);
    RUN_TEST(import_makes_room_itself_when_it_holds_the_maintenance_window);
    RUN_TEST(restore_keeps_the_maintenance_window_open_through_the_import);
    RUN_TEST(restore_snapshot_converts_a_legacy_format_snapshot);
    RUN_TEST(restore_snapshot_converts_a_v2_stride_snapshot);
    RUN_TEST(restore_snapshot_refuses_a_damaged_frame);
    RUN_TEST(restore_snapshot_tolerates_missing_manifest);
    RUN_TEST(restore_snapshot_replaces_current_live_segments);
    RUN_TEST(self_contained_backup_restores_alone_elsewhere_under_a_lower_cap);
    RUN_TEST(embed_snapshot_refuses_a_snapshot_with_a_missing_segment);
    RUN_TEST(prepare_restore_reads_the_snapshot_named_by_the_sidecar);
    RUN_TEST(configure_purges_leftover_embed_snapshots);
    /* Les suites suivantes retrouvent le défaut de production. */
    stock_spill_set_tier_enabled_for_tests(1);
}

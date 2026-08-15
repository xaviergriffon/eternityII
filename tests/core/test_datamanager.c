/*
 * Tests unitaires de datamanager.c — opérations sur les files de possibilités
 * et round-trip backup/restore.
 *
 * datamanager.c gère un état global (files statiques mutex-protégées). En
 * mono-thread, pthread_mutex_trylock réussit toujours ; on n'exerce donc QUE la
 * logique de structure de données et de sérialisation, pas la concurrence ni
 * les chemins réseau (server_ip reste NULL -> add/get travaillent en local ;
 * create_tcp_client est stubbé).
 *
 * Indépendance des tests : chaque test commence par drain_datamanager() qui
 * vide les files (l'état global est partagé entre tests).
 */
#include "greatest.h"
#include "fork_assert.h"
#include "core/datamanager.h"
#include "core/possibility.h"
#include "core/part.h"
#include "net/etii_protocol.h"
#include "net/client_identity.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <pthread.h>
#include <time.h>

/* Helpers internes de datamanager.c non exposés dans datamanager.h. */
int put_to_server(client_possibility_t *client_possibility, array_possibility_packet *possibilities);
unsigned long long count_combinations(unsigned long long x);
int check_and_connect_to_server(client_possibility_t *client_possibility);
void scroll_from_server(client_possibility_t *client_possibility, array_possibility_packet *result, int max_result);

/* Verrou global des files + variantes « nolock » (caller doit tenir le verrou). */
void lock_all_file(void);
void unlock_all_file(void);
unsigned long long regroup_datas_nolock(void);
int split_datas_nolock(int nbsplit);

/* Affichage de progression de check_duplicate (en prod, atteint uniquement après
   30 s d'attente d'un thread) + ses compteurs globaux (tableaux de nbDuplicateThread == 8). */
void print_duplicate_activity(unsigned long long dataSize, unsigned long long nbCombinations);
extern unsigned long long duplicateCount[];
extern unsigned long long duplicateErrors[];
extern unsigned long long duplicateFinish[];
extern unsigned long long duplicateAnalyzed[];

/* Globales de static_variables.c utilisées par les tests du chemin connexion. */
extern int SERVER_PORT;
extern volatile int request;
extern volatile uint16_t max_result;

/* Coupe temporairement stdout/stderr (fonctions verbeuses : tri, statistiques). */
static int g_fd1 = -1, g_fd2 = -1;
static void silence_std(void)
{
    fflush(stdout); fflush(stderr);
    g_fd1 = dup(1); g_fd2 = dup(2);
    int dn = open("/dev/null", O_WRONLY);
    dup2(dn, 1); dup2(dn, 2); close(dn);
}
static void restore_std(void)
{
    fflush(stdout); fflush(stderr);
    dup2(g_fd1, 1); dup2(g_fd2, 2);
    close(g_fd1); close(g_fd2);
}

/* Vide entièrement les pools locaux (vérifié + non vérifié). */
static void drain_datamanager(void)
{
    while (datas_size() > 0) {
        array_possibility_packet *r = get_last_possibility(NULL, 1000);
        free_array_possibility_packet(r);
    }
}

/* Vide aussi le pool « analysed » (réinjecté dans le stock puis drainé). */
static void drain_all(void)
{
    silence_std();
    restock_analysed();
    restore_std();
    drain_datamanager();
}

/* Somme des tailles du pool analysed sur toutes les files. */
static unsigned long long analysed_total(void)
{
    unsigned long long s = 0;
    for (int f = 0; f < 10; f++) s += file_analysed_size(f);
    return s;
}

/* Ajoute n possibilités non vérifiées (checked = 0) d'allocs donnés. */
static void add_packets(const int *allocs, int n)
{
    array_possibility_packet arr;
    arr.size = n;
    arr.possibilities = calloc(n, sizeof(struct possibility_packet));
    for (int i = 0; i < n; i++) {
        arr.possibilities[i].alloc = (uint16_t)allocs[i];
        arr.possibilities[i].checked = 0;
    }
    add_possibility(NULL, &arr); /* server_ip == NULL -> put_to_local */
    free(arr.possibilities);
}

/* --------------------------------------------------------------------------
 * set_server_ip / get_server_ip
 * ------------------------------------------------------------------------ */

TEST server_ip_round_trip(void)
{
    set_server_ip("192.168.1.42");
    char *ip = get_server_ip();
    ASSERT(ip != NULL);
    ASSERT_STR_EQ("192.168.1.42", ip);
    free(ip);

    /* Remise à NULL : get renvoie NULL. */
    set_server_ip(NULL);
    ASSERT_EQ(NULL, get_server_ip());

    /* Chaîne vide traitée comme « pas d'IP ». */
    set_server_ip("");
    ASSERT_EQ(NULL, get_server_ip());
    PASS();
}

/* --------------------------------------------------------------------------
 * add_possibility / datas_size / file_size
 * ------------------------------------------------------------------------ */

TEST add_increases_datas_size(void)
{
    drain_datamanager();
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");

    int allocs[] = { 3, 5, 7 };
    add_packets(allocs, 3);

    ASSERT_EQ_FMT(3ULL, datas_size(), "%llu");
    /* Toutes les possibilités non vérifiées atterrissent dans le pool 0. */
    ASSERT_EQ_FMT(3ULL, file_size(0), "%llu");

    drain_datamanager();
    PASS();
}

/* --------------------------------------------------------------------------
 * get_last_possibility : extraction (LIFO local)
 * ------------------------------------------------------------------------ */

TEST get_last_possibility_drains_pool(void)
{
    drain_datamanager();
    int allocs[] = { 2, 4 };
    add_packets(allocs, 2);

    array_possibility_packet *r = get_last_possibility(NULL, 10);
    ASSERT(r != NULL);
    ASSERT_EQ_FMT(2, r->size, "%d");
    free_array_possibility_packet(r);

    /* Le pool est vidé. */
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");

    /* Sur un pool vide, get renvoie un tableau de taille 0. */
    array_possibility_packet *empty = get_last_possibility(NULL, 10);
    ASSERT_EQ_FMT(0, empty->size, "%d");
    free_array_possibility_packet(empty);
    PASS();
}

/* --------------------------------------------------------------------------
 * put_to_pool / scroll_from_pool : le trylock réussit toujours quand au moins
 * une file du pool est libre (non régression du fallback usleep(MICRO_SLEEP)
 * ajouté sur un tour complet sans verrou pris : cf. add_possibility_analysed).
 * ------------------------------------------------------------------------ */

TEST put_and_scroll_round_trip_succeeds_when_pool_free(void)
{
    drain_datamanager();

    /* Plusieurs allers-retours consécutifs : chacun doit repartir d'une file
     * libre et aboutir immédiatement, sans jamais boucler indéfiniment. */
    for (int round = 0; round < 5; round++) {
        int allocs[] = { 1, 2, 3, 4, 5 };
        add_packets(allocs, 5);
        ASSERT_EQ_FMT(5ULL, datas_size(), "%llu");

        array_possibility_packet *r = get_last_possibility(NULL, 100);
        ASSERT(r != NULL);
        ASSERT_EQ_FMT(5, r->size, "%d");
        free_array_possibility_packet(r);
        ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");
    }
    PASS();
}

/* --------------------------------------------------------------------------
 * search_min_datas : plus petit alloc présent (0 si vide)
 * ------------------------------------------------------------------------ */

TEST search_min_datas_finds_minimum(void)
{
    drain_datamanager();
    ASSERT_EQ_FMT(0, search_min_datas(), "%d"); /* aucun élément */

    int allocs[] = { 9, 4, 7 };
    add_packets(allocs, 3);
    ASSERT_EQ_FMT(4, search_min_datas(), "%d"); /* le minimum */

    drain_datamanager();
    PASS();
}

/* --------------------------------------------------------------------------
 * backup / restore : round-trip sur fichier temporaire
 * ------------------------------------------------------------------------ */

TEST backup_then_restore_preserves_count(void)
{
    drain_datamanager();
    int allocs[] = { 1, 2, 3, 4 };
    add_packets(allocs, 4);

    char path[] = "/tmp/etii_back_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);
    close(fd);

    ASSERT_EQ_FMT(0, backup(path), "%d");

    /* On vide le stock courant, puis on restaure depuis le fichier. */
    drain_datamanager();
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");

    ASSERT_EQ_FMT(0, restore(path), "%d");
    ASSERT_EQ_FMT(4ULL, datas_size(), "%llu"); /* les 4 possibilités sont revenues */

    unlink(path);
    drain_datamanager();
    PASS();
}

/* restore sur un fichier inexistant échoue (-1) sans toucher au stock. */
TEST restore_missing_file_returns_error(void)
{
    drain_datamanager();
    int allocs[] = { 5 };
    add_packets(allocs, 1);

    ASSERT_EQ_FMT(-1, restore("/tmp/etii_no_such_back_zzz_999"), "%d");
    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu"); /* stock préservé */

    drain_datamanager();
    PASS();
}

/* Chemins d'erreur fopen de backup/import : un répertoire inexistant fait
 * échouer fopen("w") (backup, backup_analysed), un fichier absent fait échouer
 * fopen("r") (import, import_analysed, restore_analysed). Toutes retournent -1
 * sans toucher au stock. On met stdout/stderr en sourdine (log_error + perror). */
TEST backup_and_import_return_error_on_bad_path(void)
{
    drain_all();
    const char *nodir = "/tmp/etii_nonexistent_dir_zzz_42/x.back";
    const char *absent = "/tmp/etii_no_such_file_zzz_42";
    unlink(absent); /* on s'assure qu'il n'existe pas */

    silence_std();
    int rb  = backup((char *)nodir);
    int rba = backup_analysed((char *)nodir);
    int ri  = import(NULL, (char *)absent);
    int ria = import_analysed((char *)absent);
    int rra = restore_analysed((char *)absent);
    restore_std();

    ASSERT_EQ_FMT(-1, rb,  "%d"); /* backup : fopen("w") sur dir absent       */
    ASSERT_EQ_FMT(-1, rba, "%d"); /* backup_analysed : idem                   */
    ASSERT_EQ_FMT(-1, ri,  "%d"); /* import : fopen("r") sur fichier absent    */
    ASSERT_EQ_FMT(-1, ria, "%d"); /* import_analysed : idem                    */
    ASSERT_EQ_FMT(-1, rra, "%d"); /* restore_analysed : idem                   */

    drain_all();
    PASS();
}

/* backup()/backup_analysed() écrivent d'abord dans "<filename>.tmp" puis
 * rename() atomiquement vers filename : un backup réussi ne doit laisser
 * aucun ".tmp" résiduel dans le répertoire. */
TEST backup_leaves_no_residual_tmp_file(void)
{
    drain_datamanager();
    int allocs[] = { 1, 2 };
    add_packets(allocs, 2);

    char dir_template[] = "/tmp/etii_backup_tmp_XXXXXX";
    ASSERT(mkdtemp(dir_template) != NULL);

    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/store.back", dir_template);
    char tmp_path[PATH_MAX];
    snprintf(tmp_path, sizeof tmp_path, "%s.tmp", path);

    ASSERT_EQ_FMT(BACKUP_OK, backup(path), "%d");

    struct stat st;
    ASSERT_EQ_FMT(0, stat(path, &st), "%d");       /* le fichier final existe    */
    ASSERT(stat(tmp_path, &st) != 0);              /* pas de .tmp résiduel       */

    unlink(path);
    rmdir(dir_template);
    drain_datamanager();
    PASS();
}

/* Une sauvegarde déclenchée pendant une maintenance en cours (lock_all_file,
 * comme le fait déjà un backup/tri/regroup concurrent) doit être sautée — pas
 * silencieusement réussie — et ne doit créer aucun fichier cible. Avant le
 * correctif, backup()/backup_analysed() renvoyaient 0 (succès) dans ce cas. */
TEST backup_skipped_during_maintenance_reports_distinct_code(void)
{
    drain_all();
    int allocs[] = { 1 };
    add_packets(allocs, 1);
    struct possibility_packet pk;
    memset(&pk, 0, sizeof(pk));
    pk.alloc = 2;
    add_possibility_analysed(&pk, 0);

    char dir_template[] = "/tmp/etii_backup_maint_XXXXXX";
    ASSERT(mkdtemp(dir_template) != NULL);
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/store.back", dir_template);
    char path_an[PATH_MAX];
    snprintf(path_an, sizeof path_an, "%s/analysed.back", dir_template);

    lock_all_file(); /* simule une maintenance déjà en cours (positionne `maintenance`) */
    int rb  = backup(path);
    int rba = backup_analysed(path_an);
    unlock_all_file();

    ASSERT_EQ_FMT(BACKUP_SKIPPED_MAINTENANCE, rb,  "%d");
    ASSERT_EQ_FMT(BACKUP_SKIPPED_MAINTENANCE, rba, "%d");

    struct stat st;
    ASSERT(stat(path, &st) != 0);    /* le fichier cible n'a pas été créé */
    ASSERT(stat(path_an, &st) != 0);

    rmdir(dir_template);
    drain_all();
    PASS();
}

/* Un backup réussi suivi d'un backup vers un chemin invalide (répertoire
 * inexistant) ne doit pas altérer la sauvegarde précédente : le fichier
 * temporaire échoue seul, le rename() n'a jamais lieu. */
TEST backup_failure_preserves_previous_file(void)
{
    drain_datamanager();
    int allocs[] = { 7, 8, 9 };
    add_packets(allocs, 3);

    char dir_template[] = "/tmp/etii_backup_prev_XXXXXX";
    ASSERT(mkdtemp(dir_template) != NULL);
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/store.back", dir_template);

    ASSERT_EQ_FMT(BACKUP_OK, backup(path), "%d");

    struct stat before;
    ASSERT_EQ_FMT(0, stat(path, &before), "%d");

    /* Deuxième backup vers un sous-répertoire inexistant : doit échouer sans
     * toucher au fichier existant (même nom que la sauvegarde précédente). */
    char bad_path[PATH_MAX];
    snprintf(bad_path, sizeof bad_path, "%s/missing_subdir/store.back", dir_template);
    silence_std();
    int rb = backup(bad_path);
    restore_std();
    ASSERT_EQ_FMT(BACKUP_ERROR, rb, "%d");

    /* Le fichier original (même chemin cible) n'a pas bougé. */
    struct stat after;
    ASSERT_EQ_FMT(0, stat(path, &after), "%d");
    ASSERT_EQ_FMT((long long)before.st_size, (long long)after.st_size, "%lld");

    /* Round-trip : le contenu précédent est toujours restaurable intact. */
    drain_datamanager();
    ASSERT_EQ_FMT(0, restore(path), "%d");
    ASSERT_EQ_FMT(3ULL, datas_size(), "%llu");

    unlink(path);
    rmdir(dir_template);
    drain_datamanager();
    PASS();
}

/* --------------------------------------------------------------------------
 * consistent_backup (PR2, docs/conception/maitrise_charge_serveur.md) :
 * sauvegarde du stock ET du pool analysé à un instant T unique.
 *
 * `maintenance` (posé par consistent_backup avant tout verrou, levé après le
 * dernier déverrouillage) n'est déclaré extern nulle part dans les headers —
 * comme les autres internes de datamanager.c dont ce fichier a besoin
 * (lock_all_file_analysed, etc.), on le redéclare ici.
 * ------------------------------------------------------------------------ */
extern int maintenance;
void lock_all_file_analysed(void);
void unlock_all_file_analysed(void);

TEST consistent_backup_round_trip_preserves_both_pools(void)
{
    drain_all();
    int allocs[] = { 1, 2, 3 };
    add_packets(allocs, 3);
    struct possibility_packet pk;
    memset(&pk, 0, sizeof(pk));
    pk.alloc = 9;
    add_possibility_analysed(&pk, 0);

    char dir_template[] = "/tmp/etii_consistent_backup_XXXXXX";
    ASSERT(mkdtemp(dir_template) != NULL);
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/store.back", dir_template);
    char path_an[PATH_MAX];
    snprintf(path_an, sizeof path_an, "%s/analysed.back", dir_template);

    int rba = -99;
    int rb = consistent_backup(path, path_an, &rba);
    ASSERT_EQ_FMT(BACKUP_OK, rb, "%d");
    ASSERT_EQ_FMT(BACKUP_OK, rba, "%d");

    drain_all();
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");
    ASSERT_EQ_FMT(0ULL, analysed_total(), "%llu");

    ASSERT_EQ_FMT(0, restore(path), "%d");
    ASSERT_EQ_FMT(3ULL, datas_size(), "%llu");
    silence_std();
    ASSERT_EQ_FMT(0, restore_analysed(path_an), "%d");
    restore_std();
    ASSERT_EQ_FMT(1ULL, analysed_total(), "%llu");

    unlink(path);
    unlink(path_an);
    rmdir(dir_template);
    drain_all();
    PASS();
}

/* Même contrat que backup_skipped_during_maintenance_reports_distinct_code,
 * étendu aux deux volets : une maintenance déjà en cours (n'importe laquelle
 * des deux familles de verrous) fait sauter consistent_backup ENTIER — ni le
 * stock ni l'analysé ne doivent être touchés, jamais un volet sauté et
 * l'autre écrit (ça romprait précisément la cohérence à l'instant T que
 * cette fonction existe pour garantir). */
TEST consistent_backup_skipped_during_maintenance_reports_distinct_code(void)
{
    drain_all();
    int allocs[] = { 1 };
    add_packets(allocs, 1);
    struct possibility_packet pk;
    memset(&pk, 0, sizeof(pk));
    pk.alloc = 2;
    add_possibility_analysed(&pk, 0);

    char dir_template[] = "/tmp/etii_consistent_backup_maint_XXXXXX";
    ASSERT(mkdtemp(dir_template) != NULL);
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/store.back", dir_template);
    char path_an[PATH_MAX];
    snprintf(path_an, sizeof path_an, "%s/analysed.back", dir_template);

    lock_all_file_analysed(); /* simule une maintenance déjà en cours */
    int rba = -99;
    int rb = consistent_backup(path, path_an, &rba);
    unlock_all_file_analysed();

    ASSERT_EQ_FMT(BACKUP_SKIPPED_MAINTENANCE, rb,  "%d");
    ASSERT_EQ_FMT(BACKUP_SKIPPED_MAINTENANCE, rba, "%d");

    struct stat st;
    ASSERT(stat(path, &st) != 0);
    ASSERT(stat(path_an, &st) != 0);

    rmdir(dir_template);
    drain_all();
    PASS();
}

/* Ouverture du fichier analysé impossible (répertoire inexistant) : le volet
 * stock, déjà ouvert avec succès à ce stade, doit être proprement défait
 * (.tmp supprimé, rien renommé) plutôt que de laisser un fichier orphelin ou
 * de continuer sans le volet analysé — les deux volets réussissent ou aucun. */
TEST consistent_backup_analysed_open_failure_aborts_stock_too(void)
{
    drain_datamanager();
    int allocs[] = { 4, 5 };
    add_packets(allocs, 2);

    char dir_template[] = "/tmp/etii_consistent_backup_openfail_XXXXXX";
    ASSERT(mkdtemp(dir_template) != NULL);
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/store.back", dir_template);
    char bad_path_an[PATH_MAX];
    snprintf(bad_path_an, sizeof bad_path_an, "%s/missing_subdir/analysed.back", dir_template);

    silence_std();
    int rba = -99;
    int rb = consistent_backup(path, bad_path_an, &rba);
    restore_std();

    ASSERT_EQ_FMT(BACKUP_ERROR, rb, "%d");
    ASSERT_EQ_FMT(BACKUP_ERROR, rba, "%d");

    struct stat st;
    ASSERT(stat(path, &st) != 0);         /* jamais renommé */
    char stock_tmp[PATH_MAX];
    snprintf(stock_tmp, sizeof stock_tmp, "%s.tmp", path);
    ASSERT(stat(stock_tmp, &st) != 0);    /* .tmp nettoyé, pas laissé traîner */

    rmdir(dir_template);
    drain_datamanager();
    PASS();
}

/* --------------------------------------------------------------------------
 * split_datas / regroup_datas : redistribution puis consolidation
 * ------------------------------------------------------------------------ */

TEST split_then_regroup_preserves_count(void)
{
    drain_datamanager();
    int allocs[10];
    for (int i = 0; i < 10; i++) allocs[i] = i + 1;
    add_packets(allocs, 10);

    ASSERT_EQ_FMT(10ULL, datas_size(), "%llu");
    ASSERT_EQ_FMT(10ULL, file_size(0), "%llu"); /* tout dans le pool 0 au départ */

    split_datas();
    ASSERT_EQ_FMT(10ULL, datas_size(), "%llu"); /* total préservé */
    ASSERT(file_size(0) < 10);                  /* réparti sur plusieurs files */

    regroup_datas();
    ASSERT_EQ_FMT(10ULL, datas_size(), "%llu");
    ASSERT_EQ_FMT(10ULL, file_size(0), "%llu"); /* re-consolidé dans le pool 0 */

    drain_datamanager();
    PASS();
}

/* Depuis PR3, split_datas() converge par pas incrémentaux
 * (datamanager_rebalance_step) plutôt que par un quotient exact calculé
 * d'un coup — vérifie que le résultat reste effectivement équilibré (pas
 * seulement « réparti sur plusieurs files » comme le test ci-dessus). */
TEST split_datas_balances_within_one_of_target(void)
{
    drain_datamanager();
    int allocs[97]; /* premier, pour un reste non nul au quotient */
    for (int i = 0; i < 97; i++) allocs[i] = (i % 13) + 1;
    add_packets(allocs, 97);
    ASSERT_EQ_FMT(97ULL, file_size(0), "%llu"); /* tout dans le pool 0 au départ */

    split_datas();
    ASSERT_EQ_FMT(97ULL, datas_size(), "%llu"); /* total préservé */

    unsigned long long min_sz = ULLONG_MAX, max_sz = 0;
    for (int fp = 0; fp < NB_FILE_POSSIBILITY_DEFAULT; fp++) {
        unsigned long long sz = file_size(fp);
        if (sz < min_sz) min_sz = sz;
        if (sz > max_sz) max_sz = sz;
    }
    /* 97 / 10 = 9 reste 7 : au plus 7 files à 10, les autres à 9 -- jamais
     * plus d'un écart entre la plus pleine et la plus vide. */
    ASSERT(max_sz - min_sz <= 1);

    drain_datamanager();
    PASS();
}

/* --------------------------------------------------------------------------
 * datamanager_rebalance_step (PR3, docs/conception/maitrise_charge_serveur.md) :
 * rééquilibrage incrémental, file la plus pleine -> la plus vide.
 * ------------------------------------------------------------------------ */

TEST rebalance_step_preserves_total_count(void)
{
    drain_datamanager();
    int allocs[50];
    for (int i = 0; i < 50; i++) allocs[i] = (i % 13) + 1;
    add_packets(allocs, 50);
    ASSERT_EQ_FMT(50ULL, datas_size(), "%llu");

    for (int i = 0; i < 20; i++) {
        datamanager_rebalance_step(1000);
        ASSERT_EQ_FMT(50ULL, datas_size(), "%llu"); /* jamais perdu ni dupliqué */
    }

    drain_datamanager();
    PASS();
}

TEST rebalance_step_converges_to_balance(void)
{
    drain_datamanager();
    int allocs[83];
    for (int i = 0; i < 83; i++) allocs[i] = (i % 13) + 1;
    add_packets(allocs, 83);
    ASSERT_EQ_FMT(83ULL, file_size(0), "%llu");

    int moved;
    int rounds = 0;
    do {
        moved = datamanager_rebalance_step(1000);
        rounds++;
    } while (moved > 0 && rounds < NB_FILE_POSSIBILITY_DEFAULT * 4);

    ASSERT_EQ_FMT(83ULL, datas_size(), "%llu");
    unsigned long long min_sz = ULLONG_MAX, max_sz = 0;
    for (int fp = 0; fp < NB_FILE_POSSIBILITY_DEFAULT; fp++) {
        unsigned long long sz = file_size(fp);
        if (sz < min_sz) min_sz = sz;
        if (sz > max_sz) max_sz = sz;
    }
    ASSERT(max_sz - min_sz <= 1); /* 83 / 10 = 8 reste 3 */

    drain_datamanager();
    PASS();
}

/* Un budget de 1 par appel ne peut déplacer qu'UNE possibilité (par pool) :
 * borne le "temps de blocage" que datamanager_rebalance_step peut imposer à
 * un appelant (check_server_step, à chaque tour). */
TEST rebalance_step_respects_budget(void)
{
    drain_datamanager();
    int allocs[40];
    for (int i = 0; i < 40; i++) allocs[i] = (i % 13) + 1;
    add_packets(allocs, 40);
    ASSERT_EQ_FMT(40ULL, file_size(0), "%llu");

    /* Seul le pool non vérifié a du contenu ici : le budget s'applique par
     * pool, donc au plus 1 possibilité déplacée par cet appel. */
    int moved = datamanager_rebalance_step(1);
    ASSERT_EQ_FMT(1, moved, "%d");
    ASSERT_EQ_FMT(40ULL, datas_size(), "%llu"); /* rien perdu */

    drain_datamanager();
    PASS();
}

/* Un pas isolé (fullest -> emptiest) est souvent plafonné par le déficit de
 * la file la plus vide, pas par le budget : avec 1000 possibilités dans la
 * file 0 et une cible de 100 (1000/10), UN pas ne peut déplacer que 100 --
 * datamanager_rebalance_step doit enchaîner plusieurs paires pour consommer
 * tout le budget demandé (500) plutôt que de le laisser inutilisé. */
TEST rebalance_step_uses_full_budget_across_multiple_pairs(void)
{
    drain_datamanager();
    int allocs[1000];
    for (int i = 0; i < 1000; i++) allocs[i] = (i % 13) + 1;
    add_packets(allocs, 1000);
    ASSERT_EQ_FMT(1000ULL, file_size(0), "%llu");

    int moved = datamanager_rebalance_step(500);
    ASSERT_EQ_FMT(500, moved, "%d"); /* tout le budget consommé, pas juste 100 */
    ASSERT_EQ_FMT(1000ULL, datas_size(), "%llu"); /* rien perdu */

    drain_datamanager();
    PASS();
}

/* Un stock déjà équilibré ne bouge pas : évite un va-et-vient perpétuel pour
 * de petites variations sans intérêt. */
TEST rebalance_step_noop_when_already_balanced(void)
{
    drain_datamanager();
    /* Force explicitement un état équilibré : split_datas() sur un stock
     * multiple de NB_FILE_POSSIBILITY_DEFAULT donne un compte identique par file. */
    int allocs[NB_FILE_POSSIBILITY_DEFAULT * 3];
    for (int i = 0; i < NB_FILE_POSSIBILITY_DEFAULT * 3; i++) allocs[i] = (i % 13) + 1;
    add_packets(allocs, NB_FILE_POSSIBILITY_DEFAULT * 3);
    split_datas();
    for (int fp = 0; fp < NB_FILE_POSSIBILITY_DEFAULT; fp++) {
        ASSERT_EQ_FMT(3ULL, file_size(fp), "%llu");
    }

    ASSERT_EQ_FMT(0, datamanager_rebalance_step(1000), "%d");

    drain_datamanager();
    PASS();
}

/* --------------------------------------------------------------------------
 * datamanager_configure_stock_files (PR4, docs/conception/maitrise_charge_serveur.md)
 *
 * nb_file_possibility est un état GLOBAL qui persiste entre tests (comme
 * server_ip ou max_result) : chaque test qui le modifie le restaure à
 * NB_FILE_POSSIBILITY_DEFAULT avant PASS(), pour ne pas affecter les tests
 * suivants (beaucoup supposent implicitement le défaut).
 * ------------------------------------------------------------------------ */

TEST configure_stock_files_rejects_non_positive(void)
{
    ASSERT_EQ_FMT(NB_FILE_POSSIBILITY_DEFAULT, nb_file_possibility, "%d"); /* état de départ propre */
    ASSERT_EQ_FMT(-1, datamanager_configure_stock_files(0), "%d");
    ASSERT_EQ_FMT(-1, datamanager_configure_stock_files(-5), "%d");
    ASSERT_EQ_FMT(NB_FILE_POSSIBILITY_DEFAULT, nb_file_possibility, "%d"); /* inchangé */
    PASS();
}

TEST configure_stock_files_clamps_to_max(void)
{
    ASSERT_EQ_FMT(0, datamanager_configure_stock_files(NB_FILE_POSSIBILITY_MAX + 50), "%d");
    ASSERT_EQ_FMT(NB_FILE_POSSIBILITY_MAX, nb_file_possibility, "%d");

    datamanager_configure_stock_files(NB_FILE_POSSIBILITY_DEFAULT);
    PASS();
}

/* La propriété qui justifie toute la PR : une file au-delà du socle par
 * défaut (jamais initialisée statiquement) doit devenir réellement
 * utilisable après configuration -- pas seulement "ne pas planter". */
TEST configure_stock_files_new_files_are_usable(void)
{
    drain_all();
    ASSERT_EQ_FMT(0, datamanager_configure_stock_files(20), "%d");
    ASSERT_EQ_FMT(20, nb_file_possibility, "%d");

    struct possibility_packet pk;
    memset(&pk, 0, sizeof(pk));
    pk.alloc = 7;
    ASSERT_EQ_FMT(0, add_possibility_analysed(&pk, 15), "%d"); /* file au-delà du socle par défaut */
    ASSERT_EQ_FMT(1ULL, file_analysed_size(15), "%llu");

    drain_all();
    datamanager_configure_stock_files(NB_FILE_POSSIBILITY_DEFAULT);
    PASS();
}

/* Revenir à un compte plus petit ne doit ni planter ni perdre ce qui est
 * dans les files encore couvertes par le nouveau compte (aucune file n'est
 * jamais désinitialisée -- seules celles au-delà du nouveau compte
 * deviennent inertes/ignorées par les boucles). */
TEST configure_stock_files_shrinking_back_is_safe(void)
{
    drain_all();
    ASSERT_EQ_FMT(0, datamanager_configure_stock_files(20), "%d");
    int allocs[3] = { 1, 2, 3 };
    add_packets(allocs, 3); /* atterrit dans une file < NB_FILE_POSSIBILITY_DEFAULT */

    ASSERT_EQ_FMT(0, datamanager_configure_stock_files(NB_FILE_POSSIBILITY_DEFAULT), "%d");
    ASSERT_EQ_FMT(NB_FILE_POSSIBILITY_DEFAULT, nb_file_possibility, "%d");
    ASSERT_EQ_FMT(3ULL, datas_size(), "%llu"); /* rien perdu */

    drain_datamanager();
    PASS();
}

/* --------------------------------------------------------------------------
 * Pool « checked » : possibilités vérifiées par un pruner (checked == 1)
 * ------------------------------------------------------------------------ */

TEST checked_possibility_goes_to_checked_pool(void)
{
    drain_all();
    struct possibility_packet pk;
    memset(&pk, 0, sizeof(pk));
    pk.alloc = 6;
    pk.checked = 1; /* routé vers file_possibility_checked */
    array_possibility_packet arr = { .size = 1, .possibilities = &pk };
    add_possibility(NULL, &arr);

    ASSERT_EQ_FMT(1ULL, file_checked_size(0), "%llu");
    ASSERT_EQ_FMT(0ULL, file_size(0), "%llu");      /* pas dans le pool non vérifié */
    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu");      /* mais compté dans le total */

    drain_all();
    PASS();
}

/* --------------------------------------------------------------------------
 * Pool « analysed » : add / file_analysed_size / restock
 * ------------------------------------------------------------------------ */

TEST analysed_add_and_restock(void)
{
    drain_all();
    struct possibility_packet pk;
    memset(&pk, 0, sizeof(pk));
    pk.alloc = 5;
    add_possibility_analysed(&pk, 0); /* file analysed 0 */

    ASSERT_EQ_FMT(1ULL, file_analysed_size(0), "%llu");
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu"); /* le pool analysed n'est pas dans datas_size */

    /* restock : remet la possibilité dans le stock principal */
    silence_std();
    restock_analysed();
    restore_std();
    ASSERT_EQ_FMT(0ULL, analysed_total(), "%llu");
    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu");

    drain_all();
    PASS();
}

/* backup_analysed + restore_analysed : round-trip du pool analysed. */
TEST analysed_backup_restore_round_trip(void)
{
    drain_all();
    struct possibility_packet pk;
    memset(&pk, 0, sizeof(pk));
    for (int i = 0; i < 3; i++) {
        pk.alloc = (uint16_t)(i + 1);
        add_possibility_analysed(&pk, 0);
    }
    ASSERT_EQ_FMT(3ULL, analysed_total(), "%llu");

    char path[] = "/tmp/etii_back_an_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);
    close(fd);

    silence_std();
    ASSERT_EQ_FMT(0, backup_analysed(path), "%d");
    restock_analysed();
    restore_std();
    drain_datamanager();
    ASSERT_EQ_FMT(0ULL, analysed_total(), "%llu");

    silence_std();
    int rc = restore_analysed(path);
    restore_std();
    ASSERT_EQ_FMT(0, rc, "%d");
    ASSERT_EQ_FMT(3ULL, analysed_total(), "%llu"); /* les 3 sont revenues */

    unlink(path);
    drain_all();
    PASS();
}

/* restore_analysed vide totalement chaque file avant de réimporter : un paquet
 * présent avant restore mais absent de la sauvegarde ne doit plus être trouvable
 * ensuite (garde-fou contre une éventuelle entrée d'index restée pendante après
 * le vidage massif). */
TEST analysed_restore_clears_untracked_packet(void)
{
    drain_all();
    struct possibility_packet backed_up;
    memset(&backed_up, 0, sizeof(backed_up));
    backed_up.alloc = 9;
    add_possibility_analysed(&backed_up, 1);

    char path[] = "/tmp/etii_back_an2_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);
    close(fd);
    silence_std();
    ASSERT_EQ_FMT(0, backup_analysed(path), "%d");
    restore_std();

    /* Paquet supplémentaire, jamais sauvegardé, ajouté dans la même file. */
    struct possibility_packet extra;
    memset(&extra, 0, sizeof(extra));
    extra.alloc = 11;
    add_possibility_analysed(&extra, 1);
    ASSERT_EQ_FMT(2ULL, file_analysed_size(1), "%llu");

    silence_std();
    int rc = restore_analysed(path);
    restore_std();
    ASSERT_EQ_FMT(0, rc, "%d");

    /* Seul le paquet sauvegardé est revenu (import_analysed() ne préserve pas
     * l'index de file d'origine : recherche sur toutes les files). */
    ASSERT_EQ_FMT(1ULL, analysed_total(), "%llu");
    ASSERT_EQ_FMT(0, remove_possibility_analysed(&backed_up, -1), "%d");
    ASSERT_EQ_FMT(1, remove_possibility_analysed(&extra, -1), "%d");

    unlink(path);
    drain_all();
    PASS();
}

/* --------------------------------------------------------------------------
 * Tri / statistiques : exécution sans erreur, total préservé (sortie musellée)
 * ------------------------------------------------------------------------ */

TEST sort_preserves_count(void)
{
    drain_all();
    int allocs[] = { 5, 2, 8, 1, 6 };
    add_packets(allocs, 5);

    silence_std();
    int a = sort_ascending();
    int d = sort_descending();
    restore_std();

    ASSERT_EQ_FMT(0, a, "%d");
    ASSERT_EQ_FMT(0, d, "%d");
    ASSERT_EQ_FMT(5ULL, datas_size(), "%llu"); /* total inchangé par le tri */

    drain_all();
    PASS();
}

TEST statistic_and_print_run(void)
{
    drain_all();
    int allocs[] = { 3, 3 };
    add_packets(allocs, 2);

    silence_std();
    statistic_datas();
    printdatamanager();
    print_all_file_analysed();
    restore_std();

    ASSERT_EQ_FMT(2ULL, datas_size(), "%llu");
    drain_all();
    PASS();
}

/* Non-régression : alloc == ETERN_PARTS (plateau complet, atteignable via import
 * d'un .back complet où normalize_possibility_packet ne réduit pas alloc faute de
 * trou) ne doit pas écrire hors bornes dans countSize[]. Surtout probant sous
 * AddressSanitizer (make ASAN=1 ou équivalent), qui détecte l'écriture hors tableau
 * même quand elle ne provoque pas de crash observable en build normal. */
TEST statistic_datas_handles_full_board_alloc(void)
{
    drain_all();
    int allocs[] = { ETERN_PARTS };
    add_packets(allocs, 1);

    silence_std();
    int rc = statistic_datas();
    restore_std();

    ASSERT_EQ_FMT(0, rc, "%d");
    drain_all();
    PASS();
}

/* --------------------------------------------------------------------------
 * datamanager_stock_distribution : histogramme par `alloc`, source de la
 * commande console `statistic` et de GET /api/v1/stock-distribution.
 * ------------------------------------------------------------------------ */

/* Ajoute n possibilités VÉRIFIÉES (checked = 1) : put_to_local les route vers
 * le pool dédié, pas vers le pool historique. */
static void add_checked_packets(const int *allocs, int n)
{
    array_possibility_packet arr;
    arr.size = n;
    arr.possibilities = calloc(n, sizeof(struct possibility_packet));
    for (int i = 0; i < n; i++) {
        arr.possibilities[i].alloc = (uint16_t)allocs[i];
        arr.possibilities[i].checked = 1;
    }
    add_possibility(NULL, &arr);
    free(arr.possibilities);
}

/* Les trois pools sont comptés séparément, chacun sur son propre niveau. */
TEST stock_distribution_separates_the_three_pools(void)
{
    drain_all();

    int unchecked_allocs[] = { 3, 3, 5 };
    add_packets(unchecked_allocs, 3);
    int checked_allocs[] = { 3, 7 };
    add_checked_packets(checked_allocs, 2);

    struct possibility_packet pk;
    memset(&pk, 0, sizeof(pk));
    pk.alloc = 9;
    add_possibility_analysed(&pk, 0);

    stock_distribution_t d;
    datamanager_stock_distribution(&d);

    ASSERT_EQ_FMT(2ULL, d.unchecked[3], "%llu");
    ASSERT_EQ_FMT(1ULL, d.unchecked[5], "%llu");
    ASSERT_EQ_FMT(1ULL, d.checked[3], "%llu");
    ASSERT_EQ_FMT(1ULL, d.checked[7], "%llu");
    ASSERT_EQ_FMT(1ULL, d.analysed[9], "%llu");

    /* Aucun mélange entre pools sur un même niveau. */
    ASSERT_EQ_FMT(0ULL, d.checked[5], "%llu");
    ASSERT_EQ_FMT(0ULL, d.unchecked[7], "%llu");
    ASSERT_EQ_FMT(0ULL, d.unchecked[9], "%llu");

    ASSERT_EQ_FMT(3ULL, d.total_unchecked, "%llu");
    ASSERT_EQ_FMT(2ULL, d.total_checked, "%llu");
    ASSERT_EQ_FMT(1ULL, d.total_analysed, "%llu");

    drain_all();
    PASS();
}

/* Stock vide : histogramme entièrement nul, totaux nuls (pas de valeur résiduelle
 * d'un test précédent -> la fonction remet bien `out` à zéro). */
TEST stock_distribution_on_empty_stock_is_all_zero(void)
{
    drain_all();

    stock_distribution_t d;
    memset(&d, 0xFF, sizeof(d)); /* pré-salie : le memset interne doit l'écraser */
    datamanager_stock_distribution(&d);

    ASSERT_EQ_FMT(0ULL, d.total_unchecked, "%llu");
    ASSERT_EQ_FMT(0ULL, d.total_checked, "%llu");
    ASSERT_EQ_FMT(0ULL, d.total_analysed, "%llu");
    for (int i = 0; i < STOCK_DISTRIBUTION_LEVELS; i++) {
        ASSERT_EQ_FMT(0ULL, d.unchecked[i], "%llu");
        ASSERT_EQ_FMT(0ULL, d.checked[i], "%llu");
        ASSERT_EQ_FMT(0ULL, d.analysed[i], "%llu");
    }
    PASS();
}

/* Non-régression (même raison que statistic_datas_handles_full_board_alloc) :
 * alloc == ETERN_PARTS est un niveau VALIDE, il doit être compté dans la
 * dernière case du tableau, jamais écrit hors bornes. Probant sous ASan. */
TEST stock_distribution_counts_full_board_alloc(void)
{
    drain_all();
    int allocs[] = { ETERN_PARTS };
    add_packets(allocs, 1);

    stock_distribution_t d;
    datamanager_stock_distribution(&d);

    ASSERT_EQ_FMT(1ULL, d.unchecked[ETERN_PARTS], "%llu");
    ASSERT_EQ_FMT(1ULL, d.total_unchecked, "%llu");

    drain_all();
    PASS();
}

/* Le total d'un pool reste cohérent avec les compteurs déjà exposés
 * (`datas_size`), c'est-à-dire avec ce que rapporte GET /api/v1/stats. */
TEST stock_distribution_totals_match_datas_size(void)
{
    drain_all();
    int allocs[] = { 1, 2, 2, 4, 4, 4 };
    add_packets(allocs, 6);

    stock_distribution_t d;
    datamanager_stock_distribution(&d);

    ASSERT_EQ_FMT(datas_size(), d.total_unchecked + d.total_checked, "%llu");

    drain_all();
    PASS();
}

/* --------------------------------------------------------------------------
 * count_combinations : nombre de paires (x*(x-1)/2)
 * ------------------------------------------------------------------------ */

TEST count_combinations_is_triangular(void)
{
    ASSERT_EQ_FMT(0ULL, count_combinations(0), "%llu");
    ASSERT_EQ_FMT(0ULL, count_combinations(1), "%llu");
    ASSERT_EQ_FMT(1ULL, count_combinations(2), "%llu");
    ASSERT_EQ_FMT(6ULL, count_combinations(4), "%llu");
    ASSERT_EQ_FMT(10ULL, count_combinations(5), "%llu");
    PASS();
}

/* --------------------------------------------------------------------------
 * get_last_possibility_tocheck : extraction côté pruner (pool non vérifié)
 * ------------------------------------------------------------------------ */

TEST get_tocheck_drains_unchecked_pool(void)
{
    drain_all();
    int allocs[] = { 3, 4 };
    add_packets(allocs, 2); /* non vérifiées -> pool historique */

    array_possibility_packet *r = get_last_possibility_tocheck(10);
    ASSERT(r != NULL);
    ASSERT_EQ_FMT(2, r->size, "%d");
    free_array_possibility_packet(r);
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");

    drain_all();
    PASS();
}

/* --------------------------------------------------------------------------
 * remove_possibility_analysed : retrait ciblé dans le pool analysed
 * ------------------------------------------------------------------------ */

TEST remove_analysed_finds_then_misses(void)
{
    drain_all();
    struct possibility_packet pk;
    memset(&pk, 0, sizeof(pk));
    pk.alloc = 5;
    add_possibility_analysed(&pk, 0);
    ASSERT_EQ_FMT(1ULL, analysed_total(), "%llu");

    /* trouvée et retirée -> 0 */
    ASSERT_EQ_FMT(0, remove_possibility_analysed(&pk, 0), "%d");
    ASSERT_EQ_FMT(0ULL, analysed_total(), "%llu");

    /* deuxième passage : plus rien à retirer -> 1 */
    ASSERT_EQ_FMT(1, remove_possibility_analysed(&pk, 0), "%d");

    drain_all();
    PASS();
}

/* Deux paquets de contenu strictement identique dans la même file : chaque
 * retrait n'en enlève qu'un seul (la file décroît de 1 à chaque appel), et le
 * troisième retrait (plus rien de correspondant) échoue. */
TEST remove_analysed_handles_duplicate_packets(void)
{
    drain_all();
    struct possibility_packet pk;
    memset(&pk, 0, sizeof(pk));
    pk.alloc = 7;
    pk.x = 2;
    pk.y = 3;
    add_possibility_analysed(&pk, 0);
    add_possibility_analysed(&pk, 0);
    ASSERT_EQ_FMT(2ULL, analysed_total(), "%llu");

    ASSERT_EQ_FMT(0, remove_possibility_analysed(&pk, 0), "%d");
    ASSERT_EQ_FMT(1ULL, analysed_total(), "%llu");

    ASSERT_EQ_FMT(0, remove_possibility_analysed(&pk, 0), "%d");
    ASSERT_EQ_FMT(0ULL, analysed_total(), "%llu");

    ASSERT_EQ_FMT(1, remove_possibility_analysed(&pk, 0), "%d");

    drain_all();
    PASS();
}

/* Un paquet remis dans le stock principal par restock_analysed() ne doit plus
 * être trouvable dans le pool analysed ensuite : garde-fou contre une entrée
 * d'index restée pendante après le vidage massif de restock_analysed. */
TEST remove_analysed_after_restock_not_found(void)
{
    drain_all();
    struct possibility_packet pk;
    memset(&pk, 0, sizeof(pk));
    pk.alloc = 4;
    add_possibility_analysed(&pk, 2); /* file analysed 2 */
    ASSERT_EQ_FMT(1ULL, file_analysed_size(2), "%llu");

    silence_std();
    restock_analysed();
    restore_std();
    ASSERT_EQ_FMT(0ULL, analysed_total(), "%llu");

    ASSERT_EQ_FMT(1, remove_possibility_analysed(&pk, 2), "%d");

    drain_all();
    PASS();
}

/* thread < 0 : remove_possibility_analysed doit chercher dans toutes les
 * files, quelle que soit celle où le paquet a été ajouté. */
TEST remove_analysed_searches_all_files_when_thread_negative(void)
{
    drain_all();
    struct possibility_packet pk;
    memset(&pk, 0, sizeof(pk));
    pk.alloc = 6;
    add_possibility_analysed(&pk, 4); /* file analysed 4 */
    ASSERT_EQ_FMT(1ULL, file_analysed_size(4), "%llu");

    ASSERT_EQ_FMT(0, remove_possibility_analysed(&pk, -1), "%d");
    ASSERT_EQ_FMT(0ULL, analysed_total(), "%llu");

    drain_all();
    PASS();
}

/* --------------------------------------------------------------------------
 * remove_possibilities_with_no_next : élagage des impasses du stock
 * ------------------------------------------------------------------------ */

TEST remove_no_next_prunes_dead_packets(void)
{
    drain_all();
    /* map sans pièce « tout bord 0 » : une case (0,0) vide reste sans candidat. */
    struct part parts[] = { { .id = 0 }, { .id = 1, .top = 1, .right = 1, .bottom = 1, .left = 1 } };
    struct array_part rp = { .size = 2, .parts = parts };
    map_big_array *map = buildBigArray(&rp, search_max_face(&rp));

    struct possibility_packet pks[2];
    memset(pks, 0, sizeof(pks));
    /* pks[0] : grille pleine (tout à 0) -> a une suite, conservée */
    /* pks[1] : trou sur la 1re case du parcours, clé (0,0,0,0) sans candidat -> impasse */
    pks[1].grid[dirx[0]][diry[0]] = -2;
    array_possibility_packet arr = { .size = 2, .possibilities = pks };
    add_possibility(NULL, &arr);
    ASSERT_EQ_FMT(2ULL, datas_size(), "%llu");

    unsigned long long cells_before = pruner_cells_studied;
    remove_possibilities_with_no_next(map, &rp);
    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu"); /* l'impasse a été retirée */
    /* Études créditées : pks[0] balaie les ETERN_PARTS cases (grille pleine,
       jamais d'impasse), pks[1] s'arrête à la 1re case (morte) -> +1. */
    ASSERT_EQ_FMT(cells_before + ETERN_PARTS + 1, pruner_cells_studied, "%llu");

    free_bigarray(map);
    drain_all();
    PASS();
}

/* remove_possibilities_with_no_next : un packet complet (alloc == ETERN_PARTS)
   doit être traité comme solution et retiré de la file — sans appeler exit().
   Régression : avant le correctif, possibility_all_has_a_next appelait
   checkIfResultFound → exit() en contexte serveur, tuant le processus. */
TEST remove_no_next_handles_complete_solution(void)
{
    drain_all();
    /* map minimale (non utilisée car alloc == ETERN_PARTS → boucle vide) */
    struct part parts[] = { { .id = 0 }, { .id = 1, .top = 1, .right = 1, .bottom = 1, .left = 1 } };
    struct array_part rp = { .size = 2, .parts = parts };
    map_big_array *map = buildBigArray(&rp, search_max_face(&rp));

    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    pk.alloc = ETERN_PARTS; /* board complet */
    array_possibility_packet arr = { .size = 1, .possibilities = &pk };
    add_possibility(NULL, &arr);
    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu");

    /* stop_on_solution doit être 0 pour que la fonction ne call pas exit() */
    extern int stop_on_solution;
    int saved_sos = stop_on_solution;
    stop_on_solution = 0;

    unsigned long long cells_before = pruner_cells_studied;
    silence_std();
    remove_possibilities_with_no_next(map, &rp);
    restore_std();

    stop_on_solution = saved_sos;

    /* Plateau complet : balayage vide -> crédit forfaitaire d'un coup. */
    ASSERT_EQ_FMT(cells_before + 1, pruner_cells_studied, "%llu");

    /* Le packet complet doit avoir été retiré (traité comme solution, non redistributé) */
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");

    /* Nettoyer les fichiers solution éventuellement créés (./solution_server_<pid>_*) */
    char pattern[64];
    snprintf(pattern, sizeof pattern, "./solution_server_%i_*", (int)getpid());
    glob_t gp;
    if (glob(pattern, 0, NULL, &gp) == 0) {
        for (size_t i = 0; i < gp.gl_pathc; i++)
            unlink(gp.gl_pathv[i]);
        globfree(&gp);
    }

    free_bigarray(map);
    drain_all();
    PASS();
}

/* send_solution : en mode local (pas de serveur), doit renvoyer -1 sans toucher
   au réseau ni planter — la solution reste sauvegardée localement par ailleurs.
   Régression liée au signalement des solutions au serveur. */
TEST send_solution_without_client_is_local_noop(void)
{
    struct possibility_packet pkt;
    memset(&pkt, 0, sizeof pkt);
    pkt.alloc = ETERN_PARTS;
    ASSERT_EQ_FMT(-1, send_solution(NULL, &pkt), "%d");
    PASS();
}

TEST send_solution_without_server_configured_returns_error(void)
{
    set_server_ip(NULL);                 /* aucun serveur configuré (mode test/auto) */
    client_possibility_t client;
    memset(&client, 0, sizeof client);
    struct possibility_packet pkt;
    memset(&pkt, 0, sizeof pkt);
    pkt.alloc = ETERN_PARTS;
    /* Le garde server_ip==NULL renvoie -1 AVANT tout lock/connexion : le client
       zéro-initialisé (mutex non initialisé) n'est jamais déréférencé. */
    ASSERT_EQ_FMT(-1, send_solution(&client, &pkt), "%d");
    PASS();
}

/* ==========================================================================
 * Tests du chemin réseau : scroll_from_server, send_possibility_analysed,
 * send_solution et put_to_server via socketpair
 *
 * On pré-remplit cp.socket_id avec l'extrémité client d'un socketpair AF_UNIX,
 * ce qui court-circuite create_tcp_client().  Un thread pthread joue le rôle
 * d'un mini-serveur sur l'extrémité serveur : il répond au handshake de
 * is_connected (INST_TEST_CONNECTED → INST_TEST_CONNECTED) puis au protocole
 * de la fonction testée.  Le thread est joint avant la fin du test pour
 * éliminer toute race condition.
 * ========================================================================== */

/* Lecture robuste de exactement len octets (boucle recv côté mini-serveur). */
static void recv_exact_sv(int fd, void *buf, size_t len)
{
    size_t done = 0;
    while (done < len) {
        ssize_t r = recv(fd, (char *)buf + done, len - done, 0);
        if (r <= 0) break;
        done += (size_t)r;
    }
}

/* Mini-serveur scroll_from_server — renvoie un paquet (trame VERSION 7) :
 *   1. is_connected
 *   2. recv INST_GET → send int32 K=1 + possibility_packet
 */
static void *mini_srv_get_packet(void *arg)
{
    int fd = *(int *)arg;
    int8_t b;
    recv(fd, &b, 1, 0);
    b = INST_TEST_CONNECTED;
    send(fd, &b, 1, 0);
    recv(fd, &b, 1, 0);                              /* INST_GET */
    int32_t k = 1;
    send(fd, &k, sizeof k, 0);
    struct possibility_packet pkt;
    memset(&pkt, 0, sizeof pkt);
    pkt.alloc = 7;
    send(fd, &pkt, sizeof pkt, 0);
    close(fd);
    return NULL;
}

/* Mini-serveur scroll_from_server — aucune possibilité (compte K=0) :
 *   1. is_connected
 *   2. recv INST_GET → send int32 K=0
 */
static void *mini_srv_get_empty(void *arg)
{
    int fd = *(int *)arg;
    int8_t b;
    recv(fd, &b, 1, 0);
    b = INST_TEST_CONNECTED;
    send(fd, &b, 1, 0);
    recv(fd, &b, 1, 0);                              /* INST_GET */
    int32_t k = 0;
    send(fd, &k, sizeof k, 0);
    close(fd);
    return NULL;
}

/* Mini-serveur scroll_from_server — paquet envoyé en DEUX fragments TCP :
 *   1. is_connected
 *   2. recv INST_GET → send int32 K=1, puis le paquet en 2 send() séparés
 *      par un usleep (le client lit le 1er fragment seul).
 * Avant la trame VERSION 7, le client prenait le 1er fragment pour un paquet
 * complet (garbage dans le stock) et se désynchronisait sur le reste. */
static void *mini_srv_get_fragmented(void *arg)
{
    int fd = *(int *)arg;
    int8_t b;
    recv(fd, &b, 1, 0);
    b = INST_TEST_CONNECTED;
    send(fd, &b, 1, 0);
    recv(fd, &b, 1, 0);                              /* INST_GET */
    int32_t k = 1;
    send(fd, &k, sizeof k, 0);
    struct possibility_packet pkt;
    memset(&pkt, 0, sizeof pkt);
    pkt.alloc = 9;
    /* Coupe au milieu du paquet — relative à sizeof : le paquet ne fait que
     * 64 octets en build ETERN_PARTS=16 (une constante absolue déborderait). */
    const size_t cut = sizeof pkt / 2;
    send(fd, &pkt, cut, 0);
    usleep(50000);                     /* laisse le client consommer le fragment */
    send(fd, (const char *)&pkt + cut, sizeof pkt - cut, 0);
    close(fd);
    return NULL;
}

/* Paramètres du mini-serveur pruner batch : le serveur annonce `k_announced`
 * possibilités puis en envoie réellement `packets_to_send` (les deux diffèrent
 * pour les cas « k > requested » et « bloc incomplet »). */
struct batch_srv_arg {
    int fd;
    int32_t k_announced;
    int packets_to_send;
    uint16_t first_alloc;
};

/* Mini-serveur scroll_from_server, chemin pruner (INST_GET_TO_CHECK_BATCH) :
 *   1. is_connected
 *   2. recv INST_GET_TO_CHECK_BATCH + int32 requested
 *   3. send int32 k puis `packets_to_send` possibility_packet
 * Puis close(fd) : un bloc plus court que k annoncé provoque un EOF côté client
 * (recv_all partiel). */
static void *mini_srv_tocheck_batch(void *arg)
{
    struct batch_srv_arg *a = arg;
    int fd = a->fd;
    int8_t b;
    recv(fd, &b, 1, 0);                 /* sonde is_connected */
    b = INST_TEST_CONNECTED;
    send(fd, &b, 1, 0);
    recv(fd, &b, 1, 0);                 /* INST_GET_TO_CHECK_BATCH */
    int32_t requested = 0;
    recv_exact_sv(fd, &requested, sizeof requested);
    send(fd, &a->k_announced, sizeof a->k_announced, 0);
    for (int i = 0; i < a->packets_to_send; i++) {
        struct possibility_packet pkt;
        memset(&pkt, 0, sizeof pkt);
        pkt.alloc = (uint16_t)(a->first_alloc + i);
        send(fd, &pkt, sizeof pkt, 0);
    }
    close(fd);
    return NULL;
}

/* Mini-serveur send_possibility_analysed — acquittement :
 *   1. is_connected
 *   2. recv INST_POSSIBILITY_ANALYSED_BATCH + int32 M + M packets → INST_CONSIDERED
 *   Répète jusqu'à ce que le client n'envoie plus rien (connexion fermée côté test).
 */
static void *mini_srv_analysed_ok(void *arg)
{
    int fd = *(int *)arg;
    int8_t b;
    recv(fd, &b, 1, 0);
    b = INST_TEST_CONNECTED;
    send(fd, &b, 1, 0);
    /* Drain all batches until the client stops sending. */
    while (recv(fd, &b, 1, 0) == 1) {
        /* b == INST_POSSIBILITY_ANALYSED_BATCH */
        int32_t m = 0;
        recv_exact_sv(fd, &m, sizeof m);
        for (int32_t i = 0; i < m; i++) {
            struct possibility_packet pkt;
            recv_exact_sv(fd, &pkt, sizeof pkt);
        }
        b = INST_CONSIDERED;
        send(fd, &b, 1, 0);
    }
    close(fd);
    return NULL;
}

/* Mini-serveur send_possibility_analysed — mauvais ACK (bloc remis en file) :
 *   1. is_connected
 *   2. recv INST_POSSIBILITY_ANALYSED_BATCH + int32 M + M packets → INST_NULL (≠ CONSIDERED)
 */
static void *mini_srv_analysed_bad_ack(void *arg)
{
    int fd = *(int *)arg;
    int8_t b;
    recv(fd, &b, 1, 0);
    b = INST_TEST_CONNECTED;
    send(fd, &b, 1, 0);
    recv(fd, &b, 1, 0);                              /* INST_POSSIBILITY_ANALYSED_BATCH */
    int32_t m = 0;
    recv_exact_sv(fd, &m, sizeof m);
    for (int32_t i = 0; i < m; i++) {
        struct possibility_packet pkt;
        recv_exact_sv(fd, &pkt, sizeof pkt);
    }
    b = INST_NULL;
    send(fd, &b, 1, 0);
    close(fd);
    return NULL;
}

/* Mini-serveur put_to_server — mauvais ACK pour le premier paquet :
 *   pkt[0] → INST_NULL (bad ack, non-fatal : item remis en local, boucle continue)
 *   pkt[1] → INST_CONSIDERED
 *   Résultat : rc=0 mais datas_size()==1 (pkt[0] dans le stock local).
 */
static void *mini_srv_put_bad_ack(void *arg)
{
    int fd = *(int *)arg;
    int8_t b;
    recv(fd, &b, 1, 0);
    b = INST_TEST_CONNECTED;
    send(fd, &b, 1, 0);
    recv(fd, &b, 1, 0);                              /* INST_ADD pkt[0] */
    struct possibility_packet pkt;
    recv_exact_sv(fd, &pkt, sizeof pkt);
    b = INST_NULL;                                    /* ACK invalide, non-fatal */
    send(fd, &b, 1, 0);
    recv(fd, &b, 1, 0);                              /* INST_ADD pkt[1] */
    recv_exact_sv(fd, &pkt, sizeof pkt);
    b = INST_CONSIDERED;
    send(fd, &b, 1, 0);
    close(fd);
    return NULL;
}

/* Mini-serveur send_solution — succès :
 *   1. is_connected : reçoit INST_TEST_CONNECTED → répond INST_TEST_CONNECTED
 *   2. reçoit INST_SOLUTION + possibility_packet → répond INST_CONSIDERED
 */
static void *mini_srv_solution_ok(void *arg)
{
    int fd = *(int *)arg;
    int8_t b;
    recv(fd, &b, 1, 0);
    b = INST_TEST_CONNECTED;
    send(fd, &b, 1, 0);
    recv(fd, &b, 1, 0);                              /* INST_SOLUTION */
    struct possibility_packet pkt;
    recv_exact_sv(fd, &pkt, sizeof pkt);
    b = INST_CONSIDERED;
    send(fd, &b, 1, 0);
    close(fd);
    return NULL;
}

/* Mini-serveur send_solution — rejet :
 *   Même handshake, puis répond INST_NULL au lieu de INST_CONSIDERED.
 */
static void *mini_srv_solution_reject(void *arg)
{
    int fd = *(int *)arg;
    int8_t b;
    recv(fd, &b, 1, 0);
    b = INST_TEST_CONNECTED;
    send(fd, &b, 1, 0);
    recv(fd, &b, 1, 0);                              /* INST_SOLUTION */
    struct possibility_packet pkt;
    recv_exact_sv(fd, &pkt, sizeof pkt);
    b = INST_NULL;                                    /* acquittement refusé */
    send(fd, &b, 1, 0);
    close(fd);
    return NULL;
}

/* Mini-serveur put_to_server — succès (2 paquets) :
 *   handshake + INST_ADD + pkt → INST_CONSIDERED, deux fois.
 */
static void *mini_srv_put_ok(void *arg)
{
    int fd = *(int *)arg;
    int8_t b;
    recv(fd, &b, 1, 0);
    b = INST_TEST_CONNECTED;
    send(fd, &b, 1, 0);
    for (int i = 0; i < 2; i++) {
        recv(fd, &b, 1, 0);                          /* INST_ADD */
        struct possibility_packet pkt;
        recv_exact_sv(fd, &pkt, sizeof pkt);
        b = INST_CONSIDERED;
        send(fd, &b, 1, 0);
    }
    close(fd);
    return NULL;
}

/* Mini-serveur put_to_server — connexion perdue après le premier INST_ADD :
 *   handshake + reçoit INST_ADD + pkt[0] → ferme le socket sans ACK.
 *   Le client voit INST_END (recv == 0), remet pkt[1] en local et renvoie -1.
 */
static void *mini_srv_put_drop(void *arg)
{
    int fd = *(int *)arg;
    int8_t b;
    recv(fd, &b, 1, 0);
    b = INST_TEST_CONNECTED;
    send(fd, &b, 1, 0);
    recv(fd, &b, 1, 0);                              /* INST_ADD */
    struct possibility_packet pkt;
    recv_exact_sv(fd, &pkt, sizeof pkt);
    close(fd);                                        /* fermeture sans ACK */
    return NULL;
}

/* Mini-serveur générique — répond au handshake is_connected puis ferme :
 * pour les fonctions qui, après le contrôle de connexion, n'ont rien à
 * échanger (ex. send_possibility_analysed avec file vide). */
static void *mini_srv_handshake_then_close(void *arg)
{
    int fd = *(int *)arg;
    int8_t b;
    recv(fd, &b, 1, 0);
    b = INST_TEST_CONNECTED;
    send(fd, &b, 1, 0);
    close(fd);
    return NULL;
}

/* Mini-serveur scroll_from_server — coupe la connexion AVANT le compte K :
 * le client doit détecter l'EOF sur recv_all(K) et sortir proprement. */
static void *mini_srv_get_no_count(void *arg)
{
    int fd = *(int *)arg;
    int8_t b;
    recv(fd, &b, 1, 0);
    b = INST_TEST_CONNECTED;
    send(fd, &b, 1, 0);
    recv(fd, &b, 1, 0);                              /* INST_GET */
    close(fd);                                        /* EOF avant K */
    return NULL;
}

/* Mini-serveur scroll_from_server — compte K aberrant (négatif) :
 * le client doit rejeter la réponse sans lire de paquet. */
static void *mini_srv_get_bad_count(void *arg)
{
    int fd = *(int *)arg;
    int8_t b;
    recv(fd, &b, 1, 0);
    b = INST_TEST_CONNECTED;
    send(fd, &b, 1, 0);
    recv(fd, &b, 1, 0);                              /* INST_GET */
    int32_t k = -1;
    send(fd, &k, sizeof k, 0);
    close(fd);
    return NULL;
}

/* Mini-serveur scroll_from_server — annonce K=1 mais n'envoie qu'un demi
 * paquet avant de fermer : recv_all du paquet échoue (bloc incomplet). */
static void *mini_srv_get_half_packet(void *arg)
{
    int fd = *(int *)arg;
    int8_t b;
    recv(fd, &b, 1, 0);
    b = INST_TEST_CONNECTED;
    send(fd, &b, 1, 0);
    recv(fd, &b, 1, 0);                              /* INST_GET */
    int32_t k = 1;
    send(fd, &k, sizeof k, 0);
    struct possibility_packet pkt;
    memset(&pkt, 0, sizeof pkt);
    send(fd, &pkt, sizeof pkt / 2, 0);               /* moitié seulement */
    close(fd);
    return NULL;
}

/* Initialise un client_possibility_t minimal avec un socket préexistant. */
static void init_cp_with_socket(client_possibility_t *cp, int sock_fd)
{
    memset(cp, 0, sizeof *cp);
    pthread_mutex_init(&cp->socket_mutex, NULL);
    cp->socket_id = sock_fd;
    cp->id = 0;
    /* Timeout de réception : si le mini-serveur ne parle pas la trame attendue
     * (régression protocolaire), recv/recv_all échouent après 5 s au lieu de
     * bloquer le runner — le test devient rouge, pas suspendu. */
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}

/* scroll_from_server : le serveur répond avec un paquet.
 * Exercé via get_last_possibility avec pool local vide + server_ip configuré. */
TEST scroll_from_server_returns_packet(void)
{
    drain_datamanager();

    int fds[2];
    ASSERT_EQ_FMT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds), "%d");

    pthread_t srv;
    pthread_create(&srv, NULL, mini_srv_get_packet, &fds[1]);

    client_possibility_t cp;
    init_cp_with_socket(&cp, fds[0]);
    set_server_ip("127.0.0.1");

    silence_std();
    array_possibility_packet *r = get_last_possibility(&cp, 1);
    restore_std();

    ASSERT(r != NULL);
    ASSERT_EQ_FMT(1, r->size, "%d");
    ASSERT_EQ_FMT(7, (int)r->possibilities[0].alloc, "%d");
    free_array_possibility_packet(r);

    pthread_join(srv, NULL);
    set_server_ip(NULL);
    close(fds[0]);
    pthread_mutex_destroy(&cp.socket_mutex);
    drain_datamanager();
    PASS();
}

/* scroll_from_server : le serveur répond K=0 (stock vide côté serveur). */
TEST scroll_from_server_returns_empty(void)
{
    drain_datamanager();

    int fds[2];
    ASSERT_EQ_FMT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds), "%d");

    pthread_t srv;
    pthread_create(&srv, NULL, mini_srv_get_empty, &fds[1]);

    client_possibility_t cp;
    init_cp_with_socket(&cp, fds[0]);
    set_server_ip("127.0.0.1");

    silence_std();
    array_possibility_packet *r = get_last_possibility(&cp, 1);
    restore_std();

    ASSERT(r != NULL);
    ASSERT_EQ_FMT(0, r->size, "%d"); /* serveur n'a rien donné */
    free_array_possibility_packet(r);

    pthread_join(srv, NULL);
    set_server_ip(NULL);
    close(fds[0]);
    pthread_mutex_destroy(&cp.socket_mutex);
    drain_datamanager();
    PASS();
}

/* scroll_from_server : le paquet arrive en deux fragments TCP — recv_all doit
 * le réassembler (échouait avant la trame VERSION 7 : le 1er fragment était
 * pris pour un paquet complet, alloc était du garbage). */
TEST scroll_from_server_reassembles_fragmented_packet(void)
{
    drain_datamanager();

    int fds[2];
    ASSERT_EQ_FMT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds), "%d");

    pthread_t srv;
    pthread_create(&srv, NULL, mini_srv_get_fragmented, &fds[1]);

    client_possibility_t cp;
    init_cp_with_socket(&cp, fds[0]);
    set_server_ip("127.0.0.1");

    silence_std();
    array_possibility_packet *r = get_last_possibility(&cp, 1);
    restore_std();

    ASSERT(r != NULL);
    ASSERT_EQ_FMT(1, r->size, "%d");
    ASSERT_EQ_FMT(9, (int)r->possibilities[0].alloc, "%d");
    free_array_possibility_packet(r);

    pthread_join(srv, NULL);
    set_server_ip(NULL);
    close(fds[0]);
    pthread_mutex_destroy(&cp.socket_mutex);
    drain_datamanager();
    PASS();
}

/* Exécute un échange pruner batch complet contre le mini-serveur : pool local
 * vidé + pruner_mode=1 + server_ip => get_last_possibility route vers la branche
 * batch de scroll_from_server. Renvoie le résultat (à libérer par l'appelant). */
static array_possibility_packet *run_pruner_batch(int32_t k_announced, int packets_to_send,
                                                  int requested, uint16_t first_alloc)
{
    drain_datamanager();
    int saved_pm = pruner_mode;
    pruner_mode = 1;

    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) { pruner_mode = saved_pm; return NULL; }

    struct batch_srv_arg a = { .fd = fds[1], .k_announced = k_announced,
                               .packets_to_send = packets_to_send, .first_alloc = first_alloc };
    pthread_t srv;
    pthread_create(&srv, NULL, mini_srv_tocheck_batch, &a);

    client_possibility_t cp;
    init_cp_with_socket(&cp, fds[0]);
    set_server_ip("127.0.0.1");

    silence_std();
    array_possibility_packet *r = get_last_possibility(&cp, requested);
    restore_std();

    pthread_join(srv, NULL);
    set_server_ip(NULL);
    close(fds[0]);
    pthread_mutex_destroy(&cp.socket_mutex);
    pruner_mode = saved_pm;
    drain_datamanager();
    return r;
}

/* Pruner batch : le serveur annonce k=2 et envoie 2 paquets -> les deux reçus. */
TEST scroll_from_server_pruner_batch_receives_all(void)
{
    array_possibility_packet *r = run_pruner_batch(2, 2, 5, 11);
    ASSERT(r != NULL);
    ASSERT_EQ_FMT(2, r->size, "%d");
    ASSERT(r->possibilities != NULL);
    ASSERT_EQ_FMT(11, (int)r->possibilities[0].alloc, "%d");
    ASSERT_EQ_FMT(12, (int)r->possibilities[1].alloc, "%d");
    free_array_possibility_packet(r);
    PASS();
}

/* Pruner batch : k annoncé (5) > demandé (2) -> le client borne k à requested
 * (garde-fou), lit exactement 2 paquets. */
TEST scroll_from_server_pruner_batch_clamps_k_to_requested(void)
{
    array_possibility_packet *r = run_pruner_batch(5, 2, 2, 20);
    ASSERT(r != NULL);
    ASSERT_EQ_FMT(2, r->size, "%d"); /* clampé à requested, pas 5 */
    free_array_possibility_packet(r);
    PASS();
}

/* Pruner batch : k=0 (rien de disponible) -> résultat vide, aucune allocation. */
TEST scroll_from_server_pruner_batch_empty(void)
{
    array_possibility_packet *r = run_pruner_batch(0, 0, 5, 0);
    ASSERT(r != NULL);
    ASSERT_EQ_FMT(0, r->size, "%d");
    ASSERT(r->possibilities == NULL);
    free_array_possibility_packet(r);
    PASS();
}

/* Pruner batch : serveur annonce k=2 mais n'envoie qu'1 paquet puis ferme ->
 * recv_all partiel -> le bloc incomplet est rejeté (result vidé, size 0). */
TEST scroll_from_server_pruner_batch_incomplete_block(void)
{
    array_possibility_packet *r = run_pruner_batch(2, 1, 5, 30);
    ASSERT(r != NULL);
    ASSERT_EQ_FMT(0, r->size, "%d");        /* bloc incomplet -> rejeté */
    ASSERT(r->possibilities == NULL);       /* libéré et remis à NULL */
    free_array_possibility_packet(r);
    PASS();
}

/* send_possibility_analysed (chemin réseau) : envoie un batch INST_POSSIBILITY_ANALYSED_BATCH,
 * le serveur ACK → la file analysed[0] est vidée. */
TEST send_possibility_analysed_success(void)
{
    drain_all();

    int fds[2];
    ASSERT_EQ_FMT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds), "%d");

    pthread_t srv;
    pthread_create(&srv, NULL, mini_srv_analysed_ok, &fds[1]);

    /* Ajoute 1 paquet dans file_possibility_analysed[0]. */
    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    pk.alloc = 3;
    add_possibility_analysed(&pk, 0);
    ASSERT_EQ_FMT(1ULL, file_analysed_size(0), "%llu");

    client_possibility_t cp;
    init_cp_with_socket(&cp, fds[0]);
    set_server_ip("127.0.0.1");

    silence_std();
    send_possibility_analysed(&cp);
    restore_std();

    ASSERT_EQ_FMT(0ULL, file_analysed_size(0), "%llu"); /* file vidée */
    /* Le paquet drainé par le batch envoyé ne doit plus être trouvable :
     * garde-fou contre une entrée d'index laissée pendante par le drain. */
    ASSERT_EQ_FMT(1, remove_possibility_analysed(&pk, 0), "%d");

    /* Fermer fds[0] pour débloquer le mini-serveur (son recv retourne 0). */
    close(fds[0]);
    pthread_join(srv, NULL);
    set_server_ip(NULL);
    pthread_mutex_destroy(&cp.socket_mutex);
    drain_all();
    PASS();
}

/* send_possibility_analysed : le serveur rejette le batch (ACK ≠ CONSIDERED) ->
 * le paquet est remis dans la file ET doit rester trouvable/retirable ensuite
 * (garde-fou : le chemin de remise en file doit ré-indexer). */
TEST send_possibility_analysed_bad_ack_requeues_and_reindexes(void)
{
    drain_all();

    int fds[2];
    ASSERT_EQ_FMT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds), "%d");

    pthread_t srv;
    pthread_create(&srv, NULL, mini_srv_analysed_bad_ack, &fds[1]);

    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    pk.alloc = 3;
    add_possibility_analysed(&pk, 0);
    ASSERT_EQ_FMT(1ULL, file_analysed_size(0), "%llu");

    client_possibility_t cp;
    init_cp_with_socket(&cp, fds[0]);
    set_server_ip("127.0.0.1");

    silence_std();
    send_possibility_analysed(&cp);
    restore_std();

    /* Rejeté par le serveur -> remis dans la file. */
    ASSERT_EQ_FMT(1ULL, file_analysed_size(0), "%llu");
    ASSERT_EQ_FMT(0, remove_possibility_analysed(&pk, 0), "%d");
    ASSERT_EQ_FMT(0ULL, file_analysed_size(0), "%llu");

    close(fds[0]);
    pthread_join(srv, NULL);
    set_server_ip(NULL);
    pthread_mutex_destroy(&cp.socket_mutex);
    drain_all();
    PASS();
}

/* put_to_server : ACK invalide non-fatal pour pkt[0] (INST_NULL ≠ INST_CONSIDERED et ≠ INST_END).
 * L'item est remis en stock local et la boucle CONTINUE pour pkt[1] → rc=0 mais datas_size()==1. */
TEST put_to_server_bad_ack_non_fatal(void)
{
    drain_datamanager();

    int fds[2];
    ASSERT_EQ_FMT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds), "%d");

    pthread_t srv;
    pthread_create(&srv, NULL, mini_srv_put_bad_ack, &fds[1]);

    client_possibility_t cp;
    init_cp_with_socket(&cp, fds[0]);
    set_server_ip("127.0.0.1");

    struct possibility_packet pkts[2];
    memset(pkts, 0, sizeof pkts);
    pkts[0].alloc = 3;
    pkts[1].alloc = 4;
    array_possibility_packet arr = { .size = 2, .possibilities = pkts };

    silence_std();
    int rc = put_to_server(&cp, &arr);
    restore_std();

    ASSERT_EQ_FMT(0, rc, "%d");          /* pas de connection_lost : rc=0 */
    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu"); /* pkt[0] remis en local */

    pthread_join(srv, NULL);
    set_server_ip(NULL);
    close(fds[0]);
    pthread_mutex_destroy(&cp.socket_mutex);
    drain_datamanager();
    PASS();
}

TEST send_solution_success(void)
{
    drain_datamanager();

    int fds[2];
    ASSERT_EQ_FMT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds), "%d");

    pthread_t srv;
    pthread_create(&srv, NULL, mini_srv_solution_ok, &fds[1]);

    client_possibility_t cp;
    init_cp_with_socket(&cp, fds[0]);
    set_server_ip("127.0.0.1");

    struct possibility_packet pkt;
    memset(&pkt, 0, sizeof pkt);
    pkt.alloc = ETERN_PARTS;

    silence_std();
    int rc = send_solution(&cp, &pkt);
    restore_std();

    ASSERT_EQ_FMT(0, rc, "%d");

    pthread_join(srv, NULL);
    set_server_ip(NULL);
    close(fds[0]);                /* non fermé par send_solution en cas de succès */
    pthread_mutex_destroy(&cp.socket_mutex);
    drain_datamanager();
    PASS();
}

TEST send_solution_server_rejects(void)
{
    drain_datamanager();

    int fds[2];
    ASSERT_EQ_FMT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds), "%d");

    pthread_t srv;
    pthread_create(&srv, NULL, mini_srv_solution_reject, &fds[1]);

    client_possibility_t cp;
    init_cp_with_socket(&cp, fds[0]);
    set_server_ip("127.0.0.1");

    struct possibility_packet pkt;
    memset(&pkt, 0, sizeof pkt);
    pkt.alloc = ETERN_PARTS;

    silence_std();
    int rc = send_solution(&cp, &pkt);
    restore_std();

    ASSERT_EQ_FMT(-1, rc, "%d");

    pthread_join(srv, NULL);
    set_server_ip(NULL);
    close(fds[0]);
    pthread_mutex_destroy(&cp.socket_mutex);
    drain_datamanager();
    PASS();
}

TEST put_to_server_success(void)
{
    drain_datamanager();

    int fds[2];
    ASSERT_EQ_FMT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds), "%d");

    pthread_t srv;
    pthread_create(&srv, NULL, mini_srv_put_ok, &fds[1]);

    client_possibility_t cp;
    init_cp_with_socket(&cp, fds[0]);
    set_server_ip("127.0.0.1");

    struct possibility_packet pkts[2];
    memset(pkts, 0, sizeof pkts);
    pkts[0].alloc = 3;
    pkts[1].alloc = 4;
    array_possibility_packet arr = { .size = 2, .possibilities = pkts };

    silence_std();
    int rc = put_to_server(&cp, &arr);
    restore_std();

    ASSERT_EQ_FMT(0, rc, "%d");
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu"); /* aucune possibilité remise en local */

    pthread_join(srv, NULL);
    set_server_ip(NULL);
    close(fds[0]);
    pthread_mutex_destroy(&cp.socket_mutex);
    drain_datamanager();
    PASS();
}

/* put_to_server : connexion perdue après le premier INST_ADD.
 * Le mini-serveur ferme le socket sans ACK → le client reçoit INST_END pour
 * l'acquittement du paquet 0 → remet pkt[1] en local et renvoie -1.
 * Vérifie : retour -1 et datas_size() > 0 (pkt[1] remis en stock local). */
TEST put_to_server_connection_lost(void)
{
    drain_datamanager();

    int fds[2];
    ASSERT_EQ_FMT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds), "%d");

    pthread_t srv;
    pthread_create(&srv, NULL, mini_srv_put_drop, &fds[1]);

    client_possibility_t cp;
    init_cp_with_socket(&cp, fds[0]);
    set_server_ip("127.0.0.1");

    struct possibility_packet pkts[2];
    memset(pkts, 0, sizeof pkts);
    pkts[0].alloc = 3;
    pkts[1].alloc = 4;
    array_possibility_packet arr = { .size = 2, .possibilities = pkts };

    silence_std();
    int rc = put_to_server(&cp, &arr);
    restore_std();

    ASSERT_EQ_FMT(-1, rc, "%d");
    ASSERT(datas_size() > 0); /* pkts[1] doit avoir été remis en stock local */

    pthread_join(srv, NULL);
    set_server_ip(NULL);
    close(fds[0]);            /* put_to_server ne ferme pas le socket en cas d'erreur */
    pthread_mutex_destroy(&cp.socket_mutex);
    drain_datamanager();
    PASS();
}

/* ==========================================================================
 * Tests de check_and_connect_to_server : chemin « socket_id == -1 »
 *
 * Approche : mini-serveur TCP local sur 127.0.0.1:0 (port éphémère).
 * SERVER_PORT (extern int) est écrit avec le port assigné par bind() ; aucune
 * modification du Makefile n'est nécessaire — create_tcp_client() est exercé
 * tel quel.  Pour le cas « connexion refusée » on règle request=REQUEST_STOP
 * avant l'appel afin que la boucle de reconnexion s'arrête après la première
 * tentative (RECONNECT_SHOULD_ABORT() court-circuite le sleep de 1 s × 10).
 * ========================================================================== */

/* Ouvre un socket TCP en écoute sur 127.0.0.1:0, écrit le port dans *port et
 * renvoie le fd.  Retourne -1 en cas d'échec. */
static int make_local_tcp_server(int *port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(fd); return -1; }
    if (listen(fd, 1) < 0) { close(fd); return -1; }
    socklen_t len = sizeof(addr);
    getsockname(fd, (struct sockaddr *)&addr, &len);
    *port = ntohs(addr.sin_port);
    return fd;
}

/* Argument partagé entre le test et le thread mini-serveur de handshake. */
typedef struct {
    int     srv_fd;     /* fd d'écoute à passer à accept() */
    int8_t  response;   /* octet à renvoyer après avoir reçu INST_CHECK_VERSION */
} handshake_srv_arg_t;

/* Thread mini-serveur : accepte une connexion, lit INST_CHECK_VERSION + version,
 * renvoie response.  Simule exactement ce que etii_server fait au handshake.
 *
 * Sur HANDSHAKE_OK, check_and_connect_to_server() enchaîne immédiatement avec
 * INST_CLIENT_HELLO (v12, un byte d'instruction + une longueur int32 + le
 * payload de cette longueur) : il faut le drainer ici avant de fermer, sinon
 * le send() du client se heurte à une connexion déjà close côté serveur
 * (ECONNRESET/EPIPE) et check_and_connect_to_server() rapporte un échec alors
 * que le handshake lui-même a réussi. */
static void *mini_srv_handshake(void *arg)
{
    handshake_srv_arg_t *a = arg;
    struct sockaddr_in cli;
    socklen_t clen = sizeof(cli);
    int cli_fd = accept(a->srv_fd, (struct sockaddr *)&cli, &clen);
    if (cli_fd < 0) return NULL;
    int8_t b;
    recv(cli_fd, &b, 1, 0);                       /* INST_CHECK_VERSION */
    int ver;
    recv_exact_sv(cli_fd, &ver, sizeof(ver));      /* numéro de version */
    send(cli_fd, &a->response, 1, 0);
    if (a->response == INST_SUPPORTED_VERSION) {
        int8_t hello_inst;
        recv_exact_sv(cli_fd, &hello_inst, sizeof(hello_inst));
        int32_t hello_len = 0;
        recv_exact_sv(cli_fd, &hello_len, sizeof(hello_len));
        if (hello_len > 0) {
            char discard[256];
            if (hello_len <= (int32_t)sizeof(discard)) {
                recv_exact_sv(cli_fd, discard, (size_t)hello_len);
            }
        }
    }
    close(cli_fd);
    return NULL;
}

/* check_and_connect_to_server : handshake accepté (INST_SUPPORTED_VERSION).
 * Vérifie que la fonction retourne un fd >= 0 et met à jour cp.socket_id. */
TEST connect_and_handshake_ok(void)
{
    drain_datamanager();

    int port;
    int srv_fd = make_local_tcp_server(&port);
    ASSERT(srv_fd >= 0);

    handshake_srv_arg_t ha = { .srv_fd = srv_fd, .response = INST_SUPPORTED_VERSION };
    pthread_t srv;
    pthread_create(&srv, NULL, mini_srv_handshake, &ha);

    set_server_ip("127.0.0.1");
    SERVER_PORT = port;

    client_possibility_t cp;
    memset(&cp, 0, sizeof(cp));
    pthread_mutex_init(&cp.socket_mutex, NULL);
    cp.socket_id = -1;

    silence_std();
    int rc = check_and_connect_to_server(&cp);
    restore_std();

    ASSERT(rc >= 0);
    ASSERT_EQ_FMT(rc, cp.socket_id, "%d");

    pthread_join(srv, NULL);
    close(srv_fd);
    if (cp.socket_id >= 0) close(cp.socket_id);
    set_server_ip(NULL);
    pthread_mutex_destroy(&cp.socket_mutex);
    drain_datamanager();
    PASS();
}

/* check_and_connect_to_server : version refusée (INST_UNSUPPORTED_VERSION).
 * Vérifie retour == -1 et request == REQUEST_STOP. */
TEST connect_handshake_version_rejected(void)
{
    drain_datamanager();

    int saved_request = request;
    request = REQUEST_CONTINUE;

    int port;
    int srv_fd = make_local_tcp_server(&port);
    ASSERT(srv_fd >= 0);

    handshake_srv_arg_t ha = { .srv_fd = srv_fd, .response = INST_UNSUPPORTED_VERSION };
    pthread_t srv;
    pthread_create(&srv, NULL, mini_srv_handshake, &ha);

    set_server_ip("127.0.0.1");
    SERVER_PORT = port;

    client_possibility_t cp;
    memset(&cp, 0, sizeof(cp));
    pthread_mutex_init(&cp.socket_mutex, NULL);
    cp.socket_id = -1;

    silence_std();
    int rc = check_and_connect_to_server(&cp);
    restore_std();

    ASSERT_EQ_FMT(-1, rc, "%d");
    ASSERT_EQ_FMT(REQUEST_STOP, request, "%d");

    request = saved_request;
    pthread_join(srv, NULL);
    close(srv_fd);
    set_server_ip(NULL);
    pthread_mutex_destroy(&cp.socket_mutex);
    drain_datamanager();
    PASS();
}

/* check_and_connect_to_server : réponse de handshake non reconnue (INST_END →
 * HANDSHAKE_RETRY).  Vérifie retour == -1 sans modifier request. */
TEST connect_handshake_retry(void)
{
    drain_datamanager();

    int saved_request = request;
    request = REQUEST_CONTINUE;

    int port;
    int srv_fd = make_local_tcp_server(&port);
    ASSERT(srv_fd >= 0);

    handshake_srv_arg_t ha = { .srv_fd = srv_fd, .response = INST_END };
    pthread_t srv;
    pthread_create(&srv, NULL, mini_srv_handshake, &ha);

    set_server_ip("127.0.0.1");
    SERVER_PORT = port;

    client_possibility_t cp;
    memset(&cp, 0, sizeof(cp));
    pthread_mutex_init(&cp.socket_mutex, NULL);
    cp.socket_id = -1;

    silence_std();
    int rc = check_and_connect_to_server(&cp);
    restore_std();

    ASSERT_EQ_FMT(-1, rc, "%d");
    /* HANDSHAKE_RETRY ne doit PAS positionner REQUEST_STOP. */
    ASSERT_EQ_FMT(REQUEST_CONTINUE, request, "%d");

    request = saved_request;
    pthread_join(srv, NULL);
    close(srv_fd);
    set_server_ip(NULL);
    pthread_mutex_destroy(&cp.socket_mutex);
    drain_datamanager();
    PASS();
}

/* check_and_connect_to_server : create_tcp_client échoue (rien n'écoute).
 * On règle request=REQUEST_STOP pour que la boucle de reconnexion s'arrête
 * immédiatement après la première tentative (pas de sleep de 10 × 100 ms).
 * Vérifie retour == -1. */
TEST connect_create_tcp_client_fails(void)
{
    drain_datamanager();

    /* Obtient un port garanti libre : bind sans listen, ferme aussitôt. */
    int tmp = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT(tmp >= 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(tmp, (struct sockaddr *)&addr, sizeof(addr));
    socklen_t len = sizeof(addr);
    getsockname(tmp, (struct sockaddr *)&addr, &len);
    int free_port = ntohs(addr.sin_port);
    close(tmp);   /* personne n'écoute sur ce port */

    set_server_ip("127.0.0.1");
    SERVER_PORT = free_port;

    /* REQUEST_STOP court-circuite la boucle de retry dès la 1re tentative. */
    int saved_request = request;
    request = REQUEST_STOP;

    client_possibility_t cp;
    memset(&cp, 0, sizeof(cp));
    pthread_mutex_init(&cp.socket_mutex, NULL);
    cp.socket_id = -1;

    silence_std();
    int rc = check_and_connect_to_server(&cp);
    restore_std();

    ASSERT_EQ_FMT(-1, rc, "%d");

    request = saved_request;
    set_server_ip(NULL);
    pthread_mutex_destroy(&cp.socket_mutex);
    drain_datamanager();
    PASS();
}

/* ==========================================================================
 * Vérification d'intégrité des files (check_file / check_files / check_datas)
 *
 * check_one_file (static) est exercé transitivement par check_file/check_files.
 * Ces fonctions ne dépendent que de la cohérence interne des structures File ;
 * un stock construit via add_possibility est forcément cohérent -> 0.
 * ========================================================================== */

/* check_files / check_file : stock vide et stock cohérent -> 0 (pas d'incohérence). */
TEST check_files_reports_consistent_stock(void)
{
    drain_all();
    /* Stock vide : toutes les files sont cohérentes (size==0, start/end==NULL). */
    ASSERT_EQ_FMT(0, check_files(), "%d");
    ASSERT_EQ_FMT(0, check_file(0), "%d");

    /* Stock peuplé via add_possibility : la liste chaînée reste cohérente. */
    int allocs[] = { 1, 2, 3, 4, 5 };
    add_packets(allocs, 5);
    ASSERT_EQ_FMT(0, check_files(), "%d"); /* aucune incohérence détectée */
    ASSERT_EQ_FMT(0, check_file(0), "%d");

    drain_all();
    PASS();
}

/* file_size / file_checked_size / file_analysed_size : un index hors de
   [0, NB_FILE_POSSIBILITY_DEFAULT[ renvoie 0 (garde de borne, côté false jamais pris
   par les autres tests qui n'utilisent que des index valides). */
TEST file_size_accessors_reject_out_of_range(void)
{
    ASSERT_EQ_FMT(0ULL, file_size(-1), "%llu");
    ASSERT_EQ_FMT(0ULL, file_size(NB_FILE_POSSIBILITY_DEFAULT), "%llu");
    ASSERT_EQ_FMT(0ULL, file_checked_size(-1), "%llu");
    ASSERT_EQ_FMT(0ULL, file_checked_size(999), "%llu");
    ASSERT_EQ_FMT(0ULL, file_analysed_size(-7), "%llu");
    ASSERT_EQ_FMT(0ULL, file_analysed_size(NB_FILE_POSSIBILITY_DEFAULT), "%llu");
    PASS();
}

/* check_one_file : détecte chaque incohérence structurelle d'une File. Exposée
 * exprès (non statique) car aucune API publique ne permet de corrompre les pools
 * internes — on monte donc des File/Element à la main, sur la pile (la fonction
 * ne fait que LIRE, elle ne libère rien). Les log_info sont mis en sourdine.
 * Couvre les branches restées mortes : size==0+start/end résiduels, value NULL,
 * chaîne plus longue que size, end/size désynchronisés, et le retour 0. */
TEST check_one_file_flags_each_inconsistency(void)
{
    int dummy = 42; /* valeur non NULL pour les éléments « sains » */

    silence_std();

    /* (1) size==0 mais start != NULL (start résiduel). */
    Element e1 = { .value = &dummy, .previous = NULL, .next = NULL };
    File f1 = { .start = &e1, .end = NULL, .size = 0, .sizeofvalue = sizeof dummy };
    int r1 = check_one_file(&f1, 0, "test");

    /* (2) size==0 mais end != NULL (end résiduel). */
    Element e2 = { .value = &dummy, .previous = NULL, .next = NULL };
    File f2 = { .start = NULL, .end = &e2, .size = 0, .sizeofvalue = sizeof dummy };
    int r2 = check_one_file(&f2, 1, "test");

    /* (3) un élément unique dont value == NULL. */
    Element e3 = { .value = NULL, .previous = NULL, .next = NULL };
    File f3 = { .start = &e3, .end = &e3, .size = 1, .sizeofvalue = sizeof dummy };
    int r3 = check_one_file(&f3, 2, "test");

    /* (4) size annoncée (1) < longueur réelle (2) -> currElement non NULL en fin
       de boucle (chaîne plus longue que size). */
    Element a = { .value = &dummy, .previous = NULL, .next = NULL };
    Element b = { .value = &dummy, .previous = &a,   .next = NULL };
    a.next = &b;
    File f4 = { .start = &a, .end = &b, .size = 1, .sizeofvalue = sizeof dummy };
    int r4 = check_one_file(&f4, 3, "test");

    /* (5) taille cohérente (1 élément) mais pointeur end faux -> mismatch end. */
    Element c = { .value = &dummy, .previous = NULL, .next = NULL };
    File f5 = { .start = &c, .end = NULL, .size = 1, .sizeofvalue = sizeof dummy };
    int r5 = check_one_file(&f5, 4, "test");

    /* (0) File parfaitement cohérente -> 0 (retour OK exercé directement). */
    Element ok = { .value = &dummy, .previous = NULL, .next = NULL };
    File f0 = { .start = &ok, .end = &ok, .size = 1, .sizeofvalue = sizeof dummy };
    int r0 = check_one_file(&f0, 5, "test");

    restore_std();

    ASSERT_EQ_FMT(-1, r1, "%d"); /* start résiduel             */
    ASSERT_EQ_FMT(-1, r2, "%d"); /* end résiduel               */
    ASSERT_EQ_FMT(-1, r3, "%d"); /* value NULL                 */
    ASSERT_EQ_FMT(-1, r4, "%d"); /* chaîne > size              */
    ASSERT_EQ_FMT(-1, r5, "%d"); /* end/size désynchronisés    */
    ASSERT_EQ_FMT(0,  r0, "%d"); /* File cohérente             */
    PASS();
}

/* check_datas : sur un stock vide, lit le CSV (présent à la racine du dépôt),
 * ne trouve aucune possibilité -> 0 erreur. */
TEST check_datas_empty_stock_is_ok(void)
{
    drain_all();
    silence_std();
    int rc = check_datas();
    restore_std();
    ASSERT_EQ_FMT(0, rc, "%d");
    drain_all();
    PASS();
}

/* check_datas : une possibilité manifestement invalide (alloc > ETERN_PARTS)
 * est détectée par check_possibility (-4) -> check_datas renvoie -1.
 * Indépendant de la taille du puzzle (le garde alloc précède tout le reste). */
TEST check_datas_flags_invalid_packet(void)
{
    drain_all();
    int saved_max = max_result;

    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    pk.alloc = (uint16_t)(ETERN_PARTS + 1); /* > ETERN_PARTS -> check_possibility renvoie -4 */
    pk.checked = 0;
    array_possibility_packet arr = { .size = 1, .possibilities = &pk };
    add_possibility(NULL, &arr);

    silence_std();
    int rc = check_datas();
    restore_std();
    ASSERT_EQ_FMT(-1, rc, "%d"); /* au moins une possibilité invalide */

    max_result = saved_max; /* add_possibility a pu monter max_result, on le restaure */
    drain_all();
    PASS();
}

/* ==========================================================================
 * Tri multi-thread (sort_descending_mthread -> sortdmthread -> split_datas_nolock)
 * et détection de doublons (check_duplicate -> *_thread).
 * ========================================================================== */

/* sort_descending_mthread : variante parallèle du tri. Vérifie que le total
 * est préservé et que les files restent cohérentes après les passes de
 * split/regroup/tri exécutées par les threads. */
TEST sort_descending_mthread_preserves_count(void)
{
    drain_all();
    int allocs[] = { 5, 2, 8, 1, 6, 3, 9, 4 };
    add_packets(allocs, 8);
    ASSERT_EQ_FMT(8ULL, datas_size(), "%llu");

    silence_std();
    int rc = sort_descending_mthread();
    restore_std();

    ASSERT_EQ_FMT(0, rc, "%d");
    ASSERT_EQ_FMT(8ULL, datas_size(), "%llu"); /* total inchangé */
    ASSERT_EQ_FMT(0, check_files(), "%d");      /* files toujours cohérentes */

    drain_all();
    PASS();
}

/* regroup_datas_nolock / split_datas_nolock : variantes « caller tient le verrou ».
 * On encadre l'appel par lock_all_file()/unlock_all_file() comme le ferait le code
 * de production, et on vérifie la préservation du total. */
TEST regroup_split_nolock_preserve_count(void)
{
    drain_all();
    int allocs[10];
    for (int i = 0; i < 10; i++) allocs[i] = i + 1;
    add_packets(allocs, 10);

    silence_std();
    lock_all_file();
    split_datas_nolock(NB_FILE_POSSIBILITY_DEFAULT);   /* réparti sur toutes les files */
    regroup_datas_nolock();                    /* re-consolidé dans la file 0 */
    unlock_all_file();
    restore_std();

    ASSERT_EQ_FMT(10ULL, datas_size(), "%llu"); /* total préservé */
    ASSERT_EQ_FMT(10ULL, file_size(0), "%llu"); /* tout regroupé dans la file 0 */
    ASSERT_EQ_FMT(0, check_files(), "%d");

    drain_all();
    PASS();
}

/* check_duplicate : sur un stock vide, nbCombinations == 0 -> aucun thread lancé.
 * Régression : avant le correctif, la boucle de jointure attendait les 8 threads
 * (duplicateFinish[t]==1) alors qu'aucun n'était lancé -> blocage infini. Désormais
 * elle ne joint que les `spawned` threads réellement créés -> retour immédiat 0. */
TEST check_duplicate_empty_stock_returns_immediately(void)
{
    drain_all();
    silence_std();
    int rc = check_duplicate();
    restore_std();
    ASSERT_EQ_FMT(0, rc, "%d");
    drain_all();
    PASS();
}

/* check_duplicate : petit stock (3 possibilités) -> moins de nbDuplicateThread (8)
 * threads lancés. Régression du même blocage : la jointure attendait des threads
 * jamais créés. Les 3 possibilités sont deux à deux distinctes (allocs différents
 * + première case du parcours différente) -> aucun doublon -> 0. */
TEST check_duplicate_small_stock_no_error(void)
{
    drain_all();
    struct possibility_packet pks[3];
    memset(pks, 0, sizeof pks);
    for (int i = 0; i < 3; i++) {
        pks[i].alloc = (uint16_t)(i + 1);                  /* allocs distincts */
        pks[i].grid[dirx[0]][diry[0]] = (int16_t)(i + 1);  /* préfixes divergents */
    }
    array_possibility_packet arr = { .size = 3, .possibilities = pks };
    add_possibility(NULL, &arr);
    ASSERT_EQ_FMT(3ULL, datas_size(), "%llu");

    silence_std();
    int rc = check_duplicate();
    restore_std();
    ASSERT_EQ_FMT(0, rc, "%d"); /* aucun doublon, et surtout : pas de blocage */

    drain_all();
    PASS();
}

/* import_json : importe une possibilité depuis la chaîne JSON CODÉE EN DUR du
 * module (et non depuis STDIN). Elle draine d'abord toutes les files puis ajoute
 * exactement 1 possibilité au stock local (add_possibility, server_ip == NULL).
 * Le plateau JSON est 16×16 mais compute_grid borne l'écriture à ETERN_SIZE :
 * l'import est donc sûr aussi bien en build 256 qu'en build 16. Le flag `checked`
 * du paquet n'est pas initialisé par read_from_json, mais put_to_pool route
 * chaque paquet dans exactement un pool et datas_size() somme les deux -> le
 * total vaut 1 quelle que soit la valeur résiduelle. */
TEST import_json_loads_single_possibility(void)
{
    drain_all();
    silence_std();              /* read_from_json/compute_grid impriment la trace de parsing */
    int rc = import_json();
    restore_std();
    ASSERT_EQ_FMT(1, rc, "%d");
    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu");
    drain_all();
    PASS();
}

/* print_duplicate_activity : purement de l'affichage de progression. En prod elle
 * n'est atteinte qu'au bout de 30 s d'attente d'un thread dans la boucle de
 * jointure de check_duplicate -> jamais touchée par les tests fonctionnels. On
 * l'appelle directement avec les compteurs globaux positionnés à la main, en
 * couvrant les deux branches du décompte (thread actif vs terminé). */
TEST print_duplicate_activity_aggregates_counters(void)
{
    for (int t = 0; t < 8; t++) {                 /* nbDuplicateThread == 8 */
        duplicateCount[t]    = (unsigned long long)t;
        duplicateErrors[t]   = (t == 0) ? 2ULL : 0ULL;
        duplicateAnalyzed[t] = (unsigned long long)(t * 3);
        duplicateFinish[t]   = (unsigned long long)(t % 2); /* moitié actifs, moitié finis */
    }
    silence_std();
    print_duplicate_activity(100, 50);            /* nbCombinations > 0 -> ratio fini */
    restore_std();
    PASS();
}

/* ==========================================================================
 * Compléments de couverture (branches) : routage NULL/mixte, échecs réseau
 * gracieux, erreurs de trame GET, batchs analysed multiples du cap, backup du
 * pool vérifié, assainissement du flag checked, rename sur répertoire,
 * rmnonext (impasse en tête, --stop-on-solution), doublons multi-fichiers,
 * tris sur gros stock mélangé.
 * ========================================================================== */

/* add_possibility(NULL)/add_possibility_analysed(NULL) sont des no-ops ; un
 * tableau mixte route chaque possibilité vers le pool de son flag checked. */
TEST add_possibility_null_and_mixed_routing(void)
{
    drain_all();
    ASSERT_EQ_FMT(0, add_possibility(NULL, NULL), "%d");
    ASSERT_EQ_FMT(0, add_possibility_analysed(NULL, 0), "%d");

    struct possibility_packet pks[2];
    memset(pks, 0, sizeof pks);
    pks[0].alloc = 2; pks[0].checked = 0;
    pks[1].alloc = 3; pks[1].checked = 1;
    array_possibility_packet arr = { .size = 2, .possibilities = pks };
    add_possibility(NULL, &arr);

    unsigned long long ck = 0, unck = 0;
    for (int f = 0; f < 10; f++) { ck += file_checked_size(f); unck += file_size(f); }
    ASSERT_EQ_FMT(1ULL, ck, "%llu");
    ASSERT_EQ_FMT(1ULL, unck, "%llu");
    drain_all();
    PASS();
}

/* Serveur injoignable (socket -1 + connexion refusée) : toutes les fonctions
 * réseau échouent proprement — pas d'envoi, stock local intact. */
TEST network_paths_fail_gracefully_when_unreachable(void)
{
    drain_all();

    /* Port garanti libre : bind éphémère sans listen, fermé aussitôt. */
    int tmp = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT(tmp >= 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_port        = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(tmp, (struct sockaddr *)&addr, sizeof addr);
    socklen_t len = sizeof addr;
    getsockname(tmp, (struct sockaddr *)&addr, &len);
    int free_port = ntohs(addr.sin_port);
    close(tmp);

    set_server_ip("127.0.0.1");
    int saved_port = SERVER_PORT;
    SERVER_PORT = free_port;
    int saved_request = request;
    request = REQUEST_STOP;      /* court-circuite la boucle de retry connect */

    client_possibility_t cp;
    memset(&cp, 0, sizeof cp);
    pthread_mutex_init(&cp.socket_mutex, NULL);
    cp.socket_id = -1;

    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    pk.alloc = 3;
    array_possibility_packet arr = { .size = 1, .possibilities = &pk };

    silence_std();
    int rput = put_to_server(&cp, &arr);
    int rsol = send_solution(&cp, &pk);
    array_possibility_packet res = { .possibilities = NULL, .size = 0 };
    scroll_from_server(&cp, &res, 2);
    add_possibility_analysed(&pk, 0);
    send_possibility_analysed(&cp);          /* cp.id == 0 : file analysée 0 */
    restore_std();

    ASSERT_EQ_FMT(-1, rput, "%d");
    ASSERT_EQ_FMT(-1, rsol, "%d");
    ASSERT_EQ_FMT(0, res.size, "%d");
    ASSERT_EQ_FMT(1ULL, analysed_total(), "%llu"); /* rien parti, rien perdu */

    request = saved_request;
    SERVER_PORT = saved_port;
    set_server_ip(NULL);
    pthread_mutex_destroy(&cp.socket_mutex);
    drain_all();
    PASS();
}

/* put_to_server propage le record : max_result suit le plus grand alloc envoyé. */
TEST put_to_server_updates_max_result(void)
{
    drain_datamanager();
    int fds[2];
    ASSERT_EQ_FMT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds), "%d");
    pthread_t srv;
    pthread_create(&srv, NULL, mini_srv_put_ok, &fds[1]);

    client_possibility_t cp;
    init_cp_with_socket(&cp, fds[0]);
    set_server_ip("127.0.0.1");

    uint16_t saved_mr = max_result;
    max_result = 0;

    struct possibility_packet pks[2];
    memset(pks, 0, sizeof pks);
    pks[0].alloc = 6;
    pks[1].alloc = 7;
    array_possibility_packet arr = { .size = 2, .possibilities = pks };

    silence_std();
    int rc = put_to_server(&cp, &arr);
    restore_std();

    ASSERT_EQ_FMT(0, rc, "%d");
    ASSERT_EQ_FMT(7, (int)max_result, "%d");
    max_result = saved_mr;

    pthread_join(srv, NULL);
    set_server_ip(NULL);
    close(fds[0]);
    pthread_mutex_destroy(&cp.socket_mutex);
    drain_datamanager();
    PASS();
}

/* scroll_from_server : EOF avant le compte K → sortie propre, résultat vide. */
TEST scroll_from_server_count_recv_fails(void)
{
    drain_datamanager();
    int fds[2];
    ASSERT_EQ_FMT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds), "%d");
    pthread_t srv;
    pthread_create(&srv, NULL, mini_srv_get_no_count, &fds[1]);

    client_possibility_t cp;
    init_cp_with_socket(&cp, fds[0]);
    set_server_ip("127.0.0.1");

    silence_std();
    array_possibility_packet *r = get_last_possibility(&cp, 1);
    restore_std();

    ASSERT(r != NULL);
    ASSERT_EQ_FMT(0, r->size, "%d");
    free_array_possibility_packet(r);

    pthread_join(srv, NULL);
    set_server_ip(NULL);
    close(fds[0]);
    pthread_mutex_destroy(&cp.socket_mutex);
    drain_datamanager();
    PASS();
}

/* scroll_from_server : compte K aberrant (négatif) rejeté sans lire de paquet. */
TEST scroll_from_server_rejects_aberrant_count(void)
{
    drain_datamanager();
    int fds[2];
    ASSERT_EQ_FMT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds), "%d");
    pthread_t srv;
    pthread_create(&srv, NULL, mini_srv_get_bad_count, &fds[1]);

    client_possibility_t cp;
    init_cp_with_socket(&cp, fds[0]);
    set_server_ip("127.0.0.1");

    silence_std();
    array_possibility_packet *r = get_last_possibility(&cp, 1);
    restore_std();

    ASSERT(r != NULL);
    ASSERT_EQ_FMT(0, r->size, "%d");
    free_array_possibility_packet(r);

    pthread_join(srv, NULL);
    set_server_ip(NULL);
    close(fds[0]);
    pthread_mutex_destroy(&cp.socket_mutex);
    drain_datamanager();
    PASS();
}

/* scroll_from_server : paquet incomplet (K=1 mais bloc tronqué) détecté. */
TEST scroll_from_server_incomplete_packet_detected(void)
{
    drain_datamanager();
    int fds[2];
    ASSERT_EQ_FMT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds), "%d");
    pthread_t srv;
    pthread_create(&srv, NULL, mini_srv_get_half_packet, &fds[1]);

    client_possibility_t cp;
    init_cp_with_socket(&cp, fds[0]);
    set_server_ip("127.0.0.1");

    silence_std();
    array_possibility_packet *r = get_last_possibility(&cp, 1);
    restore_std();

    ASSERT(r != NULL);
    ASSERT_EQ_FMT(0, r->size, "%d");
    free_array_possibility_packet(r);

    pthread_join(srv, NULL);
    set_server_ip(NULL);
    close(fds[0]);
    pthread_mutex_destroy(&cp.socket_mutex);
    drain_datamanager();
    PASS();
}

/* send_possibility_analysed : stock multiple exact du cap (2 lots de 2 puis un
 * tour à vide) — le drainage par lots s'arrête proprement sur file vide. */
TEST send_analysed_batch_drains_multiple_of_cap(void)
{
    drain_all();
    int saved_cap = pruner_batch_size;
    pruner_batch_size = 2;

    for (int i = 0; i < 4; i++) {
        struct possibility_packet pk;
        memset(&pk, 0, sizeof pk);
        pk.alloc = (uint16_t)(1 + i);
        add_possibility_analysed(&pk, 0);
    }
    ASSERT_EQ_FMT(4ULL, analysed_total(), "%llu");

    int fds[2];
    ASSERT_EQ_FMT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds), "%d");
    pthread_t srv;
    pthread_create(&srv, NULL, mini_srv_analysed_ok, &fds[1]);

    client_possibility_t cp;
    init_cp_with_socket(&cp, fds[0]);
    set_server_ip("127.0.0.1");

    silence_std();
    send_possibility_analysed(&cp);
    restore_std();

    ASSERT_EQ_FMT(0ULL, analysed_total(), "%llu");

    close(fds[0]);        /* fin de session : débloque le mini-serveur */
    pthread_join(srv, NULL);
    set_server_ip(NULL);
    pruner_batch_size = saved_cap;
    pthread_mutex_destroy(&cp.socket_mutex);
    drain_all();
    PASS();
}

/* send_possibility_analysed : pruner_batch_size == 0 → cap plancher de 1. */
TEST send_analysed_batch_cap_defaults_to_one(void)
{
    drain_all();
    int saved_cap = pruner_batch_size;
    pruner_batch_size = 0;

    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    pk.alloc = 4;
    add_possibility_analysed(&pk, 0);

    int fds[2];
    ASSERT_EQ_FMT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds), "%d");
    pthread_t srv;
    pthread_create(&srv, NULL, mini_srv_analysed_ok, &fds[1]);

    client_possibility_t cp;
    init_cp_with_socket(&cp, fds[0]);
    set_server_ip("127.0.0.1");

    silence_std();
    send_possibility_analysed(&cp);
    restore_std();

    ASSERT_EQ_FMT(0ULL, analysed_total(), "%llu");

    close(fds[0]);
    pthread_join(srv, NULL);
    set_server_ip(NULL);
    pruner_batch_size = saved_cap;
    pthread_mutex_destroy(&cp.socket_mutex);
    drain_all();
    PASS();
}

/* send_possibility_analysed : file vide côté client — connexion contrôlée
 * mais aucun lot envoyé. */
TEST send_analysed_with_empty_file_sends_nothing(void)
{
    drain_all();
    int fds[2];
    ASSERT_EQ_FMT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds), "%d");
    pthread_t srv;
    pthread_create(&srv, NULL, mini_srv_handshake_then_close, &fds[1]);

    client_possibility_t cp;
    init_cp_with_socket(&cp, fds[0]);
    set_server_ip("127.0.0.1");

    silence_std();
    send_possibility_analysed(&cp);   /* file analysée 0 vide */
    restore_std();

    pthread_join(srv, NULL);
    set_server_ip(NULL);
    close(fds[0]);
    pthread_mutex_destroy(&cp.socket_mutex);
    PASS();
}

/* backup/backup_analysed : rename() du .tmp vers un répertoire existant échoue
 * (EISDIR) — BACKUP_ERROR et aucun .tmp résiduel. */
TEST backup_rename_onto_directory_fails(void)
{
    drain_datamanager();
    char dir[] = "/tmp/etii_dir_XXXXXX";
    ASSERT(mkdtemp(dir) != NULL);

    silence_std();
    int rb  = backup(dir);
    int rba = backup_analysed(dir);
    restore_std();

    ASSERT_EQ_FMT(BACKUP_ERROR, rb, "%d");
    ASSERT_EQ_FMT(BACKUP_ERROR, rba, "%d");

    char tmp_path[128];
    snprintf(tmp_path, sizeof tmp_path, "%s.tmp", dir);
    ASSERT(access(tmp_path, F_OK) != 0);   /* le .tmp a été nettoyé */

    rmdir(dir);
    PASS();
}

/* backup sérialise AUSSI le pool vérifié ; restore vide les DEUX pools avant
 * l'import et re-route chaque possibilité selon son flag checked. */
TEST backup_covers_checked_pool_and_restore_drains_both(void)
{
    drain_all();
    int allocs[] = { 3 };
    add_packets(allocs, 1);
    struct possibility_packet ck;
    memset(&ck, 0, sizeof ck);
    ck.alloc = 5; ck.checked = 1;
    array_possibility_packet arr = { .size = 1, .possibilities = &ck };
    add_possibility(NULL, &arr);
    ASSERT_EQ_FMT(2ULL, datas_size(), "%llu");

    char path[] = "/tmp/etii_back_ck_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);
    close(fd);
    ASSERT_EQ_FMT(BACKUP_OK, backup(path), "%d");

    /* Stock parasite dans chaque pool : restore doit le vider avant l'import. */
    int junk[] = { 8 };
    add_packets(junk, 1);
    struct possibility_packet ck2;
    memset(&ck2, 0, sizeof ck2);
    ck2.alloc = 9; ck2.checked = 1;
    array_possibility_packet arr2 = { .size = 1, .possibilities = &ck2 };
    add_possibility(NULL, &arr2);
    ASSERT_EQ_FMT(4ULL, datas_size(), "%llu");

    silence_std();
    int rr = restore(path);
    restore_std();
    ASSERT_EQ_FMT(0, rr, "%d");
    ASSERT_EQ_FMT(2ULL, datas_size(), "%llu");
    unsigned long long ck_total = 0;
    for (int f = 0; f < 10; f++) ck_total += file_checked_size(f);
    ASSERT_EQ_FMT(1ULL, ck_total, "%llu");

    unlink(path);
    drain_all();
    PASS();
}

/* Fichiers .back v4 : un flag checked hors {0,1} (padding) est assaini à 0 au
 * restore — la possibilité retourne au pool standard, jamais au pool vérifié. */
TEST restore_sanitizes_legacy_checked_flag(void)
{
    drain_datamanager();
    char path[] = "/tmp/etii_back_lg_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);
    FILE *f = fdopen(fd, "w");
    ASSERT(f != NULL);
    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    pk.alloc = 5; pk.checked = 2;              /* résidu de padding v4 */
    ASSERT(fwrite(&pk, sizeof pk, 1, f) == 1);
    memset(&pk, 0, sizeof pk);
    pk.alloc = 4; pk.checked = 1;              /* vraiment vérifiée */
    ASSERT(fwrite(&pk, sizeof pk, 1, f) == 1);
    fclose(f);

    silence_std();
    int rr = restore(path);
    restore_std();
    ASSERT_EQ_FMT(0, rr, "%d");
    ASSERT_EQ_FMT(2ULL, datas_size(), "%llu");
    unsigned long long ck_total = 0, unck_total = 0;
    for (int ff = 0; ff < 10; ff++) {
        ck_total += file_checked_size(ff);
        unck_total += file_size(ff);
    }
    ASSERT_EQ_FMT(1ULL, ck_total, "%llu");    /* checked==1 → pool vérifié   */
    ASSERT_EQ_FMT(1ULL, unck_total, "%llu");  /* checked==2 assaini → standard */

    unlink(path);
    drain_datamanager();
    PASS();
}

/* import_json vide le stock non vérifié existant avant d'importer le JSON. */
TEST import_json_drains_existing_stock(void)
{
    drain_all();
    int allocs[] = { 2, 3 };
    add_packets(allocs, 2);

    silence_std();
    int rc = import_json();
    restore_std();

    ASSERT_EQ_FMT(1, rc, "%d");
    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu"); /* l'ancien stock a été vidé */
    drain_all();
    PASS();
}

/* print_all_file_analysed parcourt et affiche les paquets en cours d'analyse. */
TEST print_all_file_analysed_lists_packets(void)
{
    drain_all();
    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    pk.alloc = 3;
    add_possibility_analysed(&pk, 0);

    silence_std();
    int rc = print_all_file_analysed();
    restore_std();

    ASSERT_EQ_FMT(0, rc, "%d");
    ASSERT_EQ_FMT(1ULL, analysed_total(), "%llu"); /* affichage non destructif */
    drain_all();
    PASS();
}

/* --------------------------------------------------------------------------
 * Export console vers fichier (P5) : fprint_datamanager / fprint_file /
 * fprint_file_analysed / fprint_all_file_analysed — commandes console
 * `print`/`printFile`/`printAnalysed [fichier]`.
 * ------------------------------------------------------------------------ */

/* fprint_datamanager écrit toutes les possibilités du stock (routées par
   add_possibility sur des files internes non déterministes depuis les tests)
   dans un fichier au lieu des logs : le compte retourné doit correspondre au
   nombre ajouté, et fprint_file, appelé sur CHAQUE file individuellement,
   doit répartir exactement ce même total (aucune possibilité perdue ni
   dupliquée par la variante « une file à la fois »). */
TEST fprint_datamanager_writes_all_possibilities_to_file(void)
{
    drain_all();
    int allocs[] = { 11, 22, 33, 44 };
    add_packets(allocs, 4);

    char path[] = "/tmp/etii_fprintdm_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);
    close(fd);
    FILE *out = fopen(path, "w");
    ASSERT(out != NULL);

    size_t count = 0;
    ASSERT_EQ_FMT(0, fprint_datamanager(out, &count), "%d");
    fclose(out);
    ASSERT_EQ_FMT(4ULL, (unsigned long long)count, "%llu");

    FILE *in = fopen(path, "r");
    ASSERT(in != NULL);
    char buf[16384] = {0};
    size_t n = fread(buf, 1, sizeof buf - 1, in);
    fclose(in);
    unlink(path);
    (void)n;
    ASSERT(strstr(buf, "\"alloc\": 11") != NULL);
    ASSERT(strstr(buf, "\"alloc\": 22") != NULL);
    ASSERT(strstr(buf, "\"alloc\": 33") != NULL);
    ASSERT(strstr(buf, "\"alloc\": 44") != NULL);

    /* fprint_file sur chaque file individuellement : le total cumulé doit
       redonner exactement 4 (couverture complète, pas de double-compte). */
    size_t per_file_total = 0;
    for (int fp = 0; fp < 10; fp++) {
        FILE *devnull = fopen("/dev/null", "w");
        ASSERT(devnull != NULL);
        ASSERT_EQ_FMT(0, fprint_file(devnull, fp, &per_file_total), "%d");
        fclose(devnull);
    }
    ASSERT_EQ_FMT(4ULL, (unsigned long long)per_file_total, "%llu");

    drain_all();
    PASS();
}

/* fprint_file_analysed ne doit exporter QUE la file demandée (sélectivité) :
   deux paquets d'alloc distincts placés dans deux files différentes, seul
   celui de la file interrogée doit apparaître dans l'export. */
TEST fprint_file_analysed_exports_only_requested_file(void)
{
    drain_all();
    struct possibility_packet pk0;
    memset(&pk0, 0, sizeof pk0);
    pk0.alloc = 55;
    add_possibility_analysed(&pk0, 0);

    struct possibility_packet pk1;
    memset(&pk1, 0, sizeof pk1);
    pk1.alloc = 66;
    add_possibility_analysed(&pk1, 1);

    char path[] = "/tmp/etii_fprintfa_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);
    close(fd);
    FILE *out = fopen(path, "w");
    ASSERT(out != NULL);

    size_t count = 0;
    ASSERT_EQ_FMT(0, fprint_file_analysed(out, 0, &count), "%d");
    fclose(out);
    ASSERT_EQ_FMT(1ULL, (unsigned long long)count, "%llu");

    FILE *in = fopen(path, "r");
    ASSERT(in != NULL);
    char buf[4096] = {0};
    size_t n = fread(buf, 1, sizeof buf - 1, in);
    fclose(in);
    unlink(path);
    (void)n;
    ASSERT(strstr(buf, "\"alloc\": 55") != NULL);
    ASSERT(strstr(buf, "\"alloc\": 66") == NULL); /* pas la file 1 */

    drain_all();
    PASS();
}

/* fprint_all_file_analysed agrège TOUTES les files d'analyse dans un seul
   export (pendant du print_all_file_analysed console, mais vers fichier). */
TEST fprint_all_file_analysed_aggregates_every_file(void)
{
    drain_all();
    struct possibility_packet pk0;
    memset(&pk0, 0, sizeof pk0);
    pk0.alloc = 77;
    add_possibility_analysed(&pk0, 0);

    struct possibility_packet pk1;
    memset(&pk1, 0, sizeof pk1);
    pk1.alloc = 88;
    add_possibility_analysed(&pk1, 1);

    char path[] = "/tmp/etii_fprintall_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);
    close(fd);
    FILE *out = fopen(path, "w");
    ASSERT(out != NULL);

    size_t count = 0;
    ASSERT_EQ_FMT(0, fprint_all_file_analysed(out, &count), "%d");
    fclose(out);
    ASSERT_EQ_FMT(2ULL, (unsigned long long)count, "%llu");

    FILE *in = fopen(path, "r");
    ASSERT(in != NULL);
    char buf[4096] = {0};
    size_t n = fread(buf, 1, sizeof buf - 1, in);
    fclose(in);
    unlink(path);
    (void)n;
    ASSERT(strstr(buf, "\"alloc\": 77") != NULL);
    ASSERT(strstr(buf, "\"alloc\": 88") != NULL);

    drain_all();
    PASS();
}

/* Décompte cumulatif : count n'est PAS remis à zéro par les fonctions
   d'export, pour permettre à l'appelant de cumuler sur plusieurs appels
   (ex. : boucle sur toutes les files du data manager). Un compteur non nul
   au départ doit être incrémenté, pas écrasé. */
TEST fprint_file_analysed_count_accumulates_across_calls(void)
{
    drain_all();
    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    pk.alloc = 99;
    add_possibility_analysed(&pk, 0);

    FILE *devnull = fopen("/dev/null", "w");
    ASSERT(devnull != NULL);
    size_t count = 5; /* préexistant : ne doit pas être écrasé */
    ASSERT_EQ_FMT(0, fprint_file_analysed(devnull, 0, &count), "%d");
    fclose(devnull);
    ASSERT_EQ_FMT(6ULL, (unsigned long long)count, "%llu"); /* 5 + 1 nouveau */

    drain_all();
    PASS();
}

/* count == NULL est accepté (l'appelant ne s'intéresse pas au décompte). */
TEST fprint_file_analysed_accepts_null_count(void)
{
    drain_all();
    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    pk.alloc = 1;
    add_possibility_analysed(&pk, 0);

    FILE *devnull = fopen("/dev/null", "w");
    ASSERT(devnull != NULL);
    ASSERT_EQ_FMT(0, fprint_file_analysed(devnull, 0, NULL), "%d");
    fclose(devnull);

    drain_all();
    PASS();
}

/* rmnonext : impasse EN TÊTE de file (previous NULL, next non NULL) — le
 * chaînage start/next->previous est recousu correctement. */
TEST remove_no_next_removes_dead_packet_at_head(void)
{
    drain_all();
    struct part parts[] = { { .id = 0 }, { .id = 1, .top = 1, .right = 1, .bottom = 1, .left = 1 } };
    struct array_part rp = { .size = 2, .parts = parts };
    map_big_array *map = buildBigArray(&rp, search_max_face(&rp));

    struct possibility_packet pks[2];
    memset(pks, 0, sizeof pks);
    /* pks[0] : trou sur la 1re case du parcours -> impasse, en tête de file */
    pks[0].grid[dirx[0]][diry[0]] = -2;
    /* pks[1] : grille pleine -> a une suite, conservée derrière l'impasse */
    array_possibility_packet arr = { .size = 2, .possibilities = pks };
    add_possibility(NULL, &arr);
    ASSERT_EQ_FMT(2ULL, datas_size(), "%llu");

    remove_possibilities_with_no_next(map, &rp);
    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu");

    free_bigarray(map);
    drain_all();
    PASS();
}

/* --------------------------------------------------------------------------
 * rmnonext + --stop-on-solution : la détection d'une solution complète doit
 * sauvegarder les files puis exit(EXIT_SUCCESS). exit() tuerait le runner :
 * scénario exécuté dans un fork (cf. fork_assert.h), chdir vers un répertoire
 * temporaire pour que les .back/solution ne polluent pas le dépôt.
 * ------------------------------------------------------------------------ */

static char g_sol_dir[64];

static void fork_rmnonext_solution(void)
{
    if (chdir(g_sol_dir) != 0) exit(9);
    extern int stop_on_solution;
    stop_on_solution = 1;

    struct part parts[] = { { .id = 0 }, { .id = 1, .top = 1, .right = 1, .bottom = 1, .left = 1 } };
    struct array_part rp = { .size = 2, .parts = parts };
    map_big_array *map = buildBigArray(&rp, search_max_face(&rp));

    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    pk.alloc = ETERN_PARTS;                    /* plateau complet */
    array_possibility_packet arr = { .size = 1, .possibilities = &pk };
    add_possibility(NULL, &arr);

    remove_possibilities_with_no_next(map, &rp);
    exit(7); /* la branche stop_on_solution aurait dû exit(EXIT_SUCCESS) */
}

TEST rmnonext_solution_with_stop_on_solution_exits(void)
{
    drain_all();
    strcpy(g_sol_dir, "/tmp/etii_sol_XXXXXX");
    ASSERT(mkdtemp(g_sol_dir) != NULL);

    pid_t child = 0;
    int code = run_in_fork(fork_rmnonext_solution, &child);
    ASSERT_EQ_FMT(0, code, "%d"); /* exit(EXIT_SUCCESS) après backup */

    /* Nettoyage : backups + solution écrite par le fils dans le tmpdir. */
    char p[160];
    snprintf(p, sizeof p, "%s/eternityII.back", g_sol_dir);
    unlink(p);
    snprintf(p, sizeof p, "%s/eternityII-in_analyse.back", g_sol_dir);
    unlink(p);
    snprintf(p, sizeof p, "%s/solution_server_%i_*", g_sol_dir, (int)child);
    glob_t gp;
    if (glob(p, 0, NULL, &gp) == 0) {
        for (size_t i = 0; i < gp.gl_pathc; i++)
            unlink(gp.gl_pathv[i]);
        globfree(&gp);
    }
    rmdir(g_sol_dir);
    PASS();
}

/* check_duplicate signale un doublon exact (compare == 0) ET une relation
 * ancêtre/descendant (is_origin_of == 1) : retour -1. */
TEST check_duplicate_flags_duplicates_and_origins(void)
{
    drain_all();
    struct possibility_packet pks[3];
    memset(pks, 0, sizeof pks);
    /* pks[0] (A) : préfixe commun, alloc=1 */
    pks[0].alloc = 1;
    pks[0].grid[dirx[0]][diry[0]] = 200;
    /* pks[1] (B) : descendant de A (même préfixe, alloc=2) -> erreur origin */
    pks[1].alloc = 2;
    pks[1].grid[dirx[0]][diry[0]] = 200;
    pks[1].grid[dirx[1]][diry[1]] = 201;
    /* pks[2] : copie exacte de A -> erreur duplicate */
    pks[2] = pks[0];
    array_possibility_packet arr = { .size = 3, .possibilities = pks };
    add_possibility(NULL, &arr);

    silence_std();
    int rc = check_duplicate();
    restore_std();

    ASSERT_EQ_FMT(-1, rc, "%d");
    drain_all();
    PASS();
}

/* check_duplicate sur un stock réparti sur plusieurs files : le partitionnement
 * multi-thread et le walker croisent les frontières de files, sans faux positif. */
TEST check_duplicate_multi_thread_across_files(void)
{
    drain_all();
    enum { N = 26 };
    struct possibility_packet pks[N];
    memset(pks, 0, sizeof pks);
    for (int i = 0; i < N; i++) {
        pks[i].alloc = 1;
        pks[i].grid[dirx[0]][diry[0]] = (int16_t)(100 + i); /* tous distincts */
    }
    array_possibility_packet arr = { .size = N, .possibilities = pks };
    add_possibility(NULL, &arr);

    silence_std();
    split_datas();               /* répartit sur les 10 files */
    int rc = check_duplicate();
    restore_std();

    ASSERT_EQ_FMT(0, rc, "%d");
    ASSERT_EQ_FMT((unsigned long long)N, datas_size(), "%llu");
    drain_all();
    PASS();
}

/* Tris sur un stock mélangé assez gros pour déclencher l'affichage de
 * progression et les déplacements dans les deux sens : comptage préservé. */
TEST sort_large_shuffled_stock_both_directions(void)
{
    drain_all();
    enum { N = 30 };
    struct possibility_packet pks[N];
    memset(pks, 0, sizeof pks);
    for (int i = 0; i < N; i++) {
        pks[i].alloc = (uint16_t)((i * 7) % 13 + 1); /* ordre pseudo-aléatoire */
    }
    array_possibility_packet arr = { .size = N, .possibilities = pks };
    add_possibility(NULL, &arr);

    silence_std();
    sort_ascending();
    sort_descending();
    restore_std();

    ASSERT_EQ_FMT((unsigned long long)N, datas_size(), "%llu");
    drain_all();
    PASS();
}

/* --------------------------------------------------------------------------
 * Contention : boucles trylock (wraparound de file + usleep)
 *
 * En mono-thread les trylock réussissent toujours : les branches « un tour
 * complet sans verrou -> usleep » restaient mortes. On les prend de façon
 * déterministe : le test tient lock_all_file[_analysed]() pendant qu'un thread
 * exécute l'opération (qui boucle sans retour anticipé), puis relâche après un
 * délai — l'opération aboutit alors et le thread se joint.
 * ------------------------------------------------------------------------ */

void lock_all_file_analysed(void);
void unlock_all_file_analysed(void);

/* PR1 (docs/conception/maitrise_charge_serveur.md) : les trois boucles
 * ci-dessus n'attendent plus indéfiniment un trylock — au-delà de
 * DATAMANAGER_TRYLOCK_MAX_SWEEPS tours elles abandonnent (résultat vide /
 * code d'erreur) plutôt que de bloquer le thread serveur qui les appelle.
 * Marge de sécurité ×3 sur le budget nominal pour rester robuste sous ASan
 * ou une machine chargée, sans jamais dépendre d'un pthread_join bloquant
 * qui ferait pendre CE test si jamais la régression revenait : on attend un
 * temps borné puis on LIT un drapeau, on ne joint qu'ensuite. */
#define TEST_TRYLOCK_BOUND_MARGIN_US ((useconds_t)DATAMANAGER_TRYLOCK_MAX_SWEEPS * MICRO_SLEEP * 3)

static void *th_add_possibility(void *arg)
{
    (void)arg;
    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    pk.alloc = 3;
    array_possibility_packet arr = { .size = 1, .possibilities = &pk };
    add_possibility(NULL, &arr);
    return NULL;
}

TEST add_possibility_spins_until_lock_released(void)
{
    drain_all();
    lock_all_file();
    pthread_t th;
    ASSERT_EQ(0, pthread_create(&th, NULL, th_add_possibility, NULL));
    usleep(60000);              /* laisse la boucle trylock faire > 1 tour complet */
    unlock_all_file();
    pthread_join(th, NULL);

    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu");
    drain_all();
    PASS();
}

static volatile int g_bounded_put_done = 0;
static int g_bounded_put_rc = -99;
static void *th_add_possibility_never_unlocked(void *arg)
{
    (void)arg;
    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    pk.alloc = 4;
    array_possibility_packet arr = { .size = 1, .possibilities = &pk };
    g_bounded_put_rc = add_possibility(NULL, &arr);
    g_bounded_put_done = 1;
    return NULL;
}

/* Contrepartie de add_possibility_spins_until_lock_released : si le verrou
 * n'est JAMAIS relâché (maintenance qui dure), put_to_pool doit rendre la
 * main — échec signalé, rien d'inséré — plutôt que de bloquer indéfiniment
 * le thread serveur qui sert ce client. */
TEST add_possibility_gives_up_when_stock_never_unlocked(void)
{
    drain_all();
    g_bounded_put_done = 0;
    g_bounded_put_rc = -99;
    lock_all_file();
    pthread_t th;
    ASSERT_EQ(0, pthread_create(&th, NULL, th_add_possibility_never_unlocked, NULL));
    usleep(TEST_TRYLOCK_BOUND_MARGIN_US);
    int returned_while_locked = g_bounded_put_done;
    unlock_all_file();
    pthread_join(th, NULL);

    ASSERT_EQ_FMT(1, returned_while_locked, "%d");
    ASSERT(g_bounded_put_rc != 0);
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");
    drain_all();
    PASS();
}

static array_possibility_packet *g_cont_result;
static void *th_get_possibility(void *arg)
{
    (void)arg;
    g_cont_result = get_last_possibility(NULL, 1);
    return NULL;
}

TEST get_last_possibility_spins_until_lock_released(void)
{
    drain_all();
    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    pk.alloc = 5;
    array_possibility_packet arr = { .size = 1, .possibilities = &pk };
    add_possibility(NULL, &arr);

    lock_all_file();
    g_cont_result = NULL;
    pthread_t th;
    ASSERT_EQ(0, pthread_create(&th, NULL, th_get_possibility, NULL));
    usleep(60000);
    unlock_all_file();
    pthread_join(th, NULL);

    ASSERT(g_cont_result != NULL);
    ASSERT_EQ_FMT(1, g_cont_result->size, "%d");
    free_array_possibility_packet(g_cont_result);
    drain_all();
    PASS();
}

static volatile int g_bounded_get_done = 0;
static array_possibility_packet *g_bounded_get_result = NULL;
static void *th_get_possibility_never_unlocked(void *arg)
{
    (void)arg;
    g_bounded_get_result = get_last_possibility_tocheck(1);
    g_bounded_get_done = 1;
    return NULL;
}

/* Contrepartie de get_last_possibility_spins_until_lock_released : si le
 * verrou n'est JAMAIS relâché, scroll_from_pool doit rendre la main avec un
 * résultat vide — indiscernable, côté appelant, d'un stock réellement vide
 * (réponse K=0 déjà normale du protocole depuis la v7) — plutôt que de
 * tourner indéfiniment. */
TEST get_last_possibility_gives_up_when_stock_never_unlocked(void)
{
    drain_all();
    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    pk.alloc = 5;
    array_possibility_packet arr = { .size = 1, .possibilities = &pk };
    add_possibility(NULL, &arr);

    g_bounded_get_done = 0;
    g_bounded_get_result = NULL;
    lock_all_file();
    pthread_t th;
    ASSERT_EQ(0, pthread_create(&th, NULL, th_get_possibility_never_unlocked, NULL));
    usleep(TEST_TRYLOCK_BOUND_MARGIN_US);
    int returned_while_locked = g_bounded_get_done;
    unlock_all_file();
    pthread_join(th, NULL);

    ASSERT_EQ_FMT(1, returned_while_locked, "%d");
    ASSERT(g_bounded_get_result != NULL);
    ASSERT_EQ_FMT(0, g_bounded_get_result->size, "%d");
    free_array_possibility_packet(g_bounded_get_result);
    /* La possibilité ajoutée avant le verrouillage est toujours là : rien
     * n'a été perdu, seulement pas servie tant que le stock était gelé. */
    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu");
    drain_all();
    PASS();
}

static void *th_add_analysed_any_file(void *arg)
{
    (void)arg;
    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    pk.alloc = 6;
    add_possibility_analysed(&pk, -1);   /* thread < 0 : wraparound de file */
    return NULL;
}

static void *th_add_analysed_fixed_file(void *arg)
{
    (void)arg;
    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    pk.alloc = 7;
    add_possibility_analysed(&pk, 2);    /* thread fixe : retente la même file */
    return NULL;
}

TEST add_possibility_analysed_spins_both_modes(void)
{
    drain_all();
    lock_all_file_analysed();
    pthread_t th_any, th_fixed;
    ASSERT_EQ(0, pthread_create(&th_any, NULL, th_add_analysed_any_file, NULL));
    ASSERT_EQ(0, pthread_create(&th_fixed, NULL, th_add_analysed_fixed_file, NULL));
    usleep(60000);
    unlock_all_file_analysed();
    pthread_join(th_any, NULL);
    pthread_join(th_fixed, NULL);

    ASSERT_EQ_FMT(2ULL, analysed_total(), "%llu");
    drain_all();
    PASS();
}

static volatile int g_bounded_analysed_done = 0;
static int g_bounded_analysed_rc = -99;
static void *th_add_analysed_any_file_never_unlocked(void *arg)
{
    (void)arg;
    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    pk.alloc = 8;
    g_bounded_analysed_rc = add_possibility_analysed(&pk, -1);   /* wraparound */
    g_bounded_analysed_done = 1;
    return NULL;
}

static void *th_add_analysed_fixed_file_never_unlocked(void *arg)
{
    (void)arg;
    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    pk.alloc = 9;
    g_bounded_analysed_rc = add_possibility_analysed(&pk, 2);    /* file fixe */
    g_bounded_analysed_done = 1;
    return NULL;
}

/* Contrepartie de add_possibility_analysed_spins_both_modes, mode « wraparound »
 * (thread < 0, utilisé côté serveur par record_possibility_analysed_for_client) :
 * si le verrou n'est JAMAIS relâché, la fonction doit rendre la main avec
 * -1 (contrat documenté) plutôt que de tourner indéfiniment. */
TEST add_possibility_analysed_gives_up_when_pool_never_unlocked_rotating(void)
{
    drain_all();
    g_bounded_analysed_done = 0;
    g_bounded_analysed_rc = -99;
    lock_all_file_analysed();
    pthread_t th;
    ASSERT_EQ(0, pthread_create(&th, NULL, th_add_analysed_any_file_never_unlocked, NULL));
    usleep(TEST_TRYLOCK_BOUND_MARGIN_US);
    int returned_while_locked = g_bounded_analysed_done;
    unlock_all_file_analysed();
    pthread_join(th, NULL);

    ASSERT_EQ_FMT(1, returned_while_locked, "%d");
    ASSERT_EQ_FMT(-1, g_bounded_analysed_rc, "%d");
    ASSERT_EQ_FMT(0ULL, analysed_total(), "%llu");
    drain_all();
    PASS();
}

/* Même contrat, mode « file fixe » (thread >= 0, utilisé côté client pour son
 * propre fork) : arithmétique de sortie différente (un usleep par tentative,
 * pas un par tour de NB_FILE_POSSIBILITY_DEFAULT tentatives) — verrouillée
 * indépendamment pour ne pas laisser cette branche régresser sans le voir. */
TEST add_possibility_analysed_gives_up_when_pool_never_unlocked_pinned(void)
{
    drain_all();
    g_bounded_analysed_done = 0;
    g_bounded_analysed_rc = -99;
    lock_all_file_analysed();
    pthread_t th;
    ASSERT_EQ(0, pthread_create(&th, NULL, th_add_analysed_fixed_file_never_unlocked, NULL));
    usleep(TEST_TRYLOCK_BOUND_MARGIN_US);
    int returned_while_locked = g_bounded_analysed_done;
    unlock_all_file_analysed();
    pthread_join(th, NULL);

    ASSERT_EQ_FMT(1, returned_while_locked, "%d");
    ASSERT_EQ_FMT(-1, g_bounded_analysed_rc, "%d");
    ASSERT_EQ_FMT(0ULL, analysed_total(), "%llu");
    drain_all();
    PASS();
}

static int g_cont_remove_rc = -99;
static void *th_remove_analysed(void *arg)
{
    (void)arg;
    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    pk.alloc = 8;
    g_cont_remove_rc = remove_possibility_analysed(&pk, -1);
    return NULL;
}

TEST remove_possibility_analysed_spins_until_lock_released(void)
{
    drain_all();
    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    pk.alloc = 8;
    add_possibility_analysed(&pk, 1);

    lock_all_file_analysed();
    pthread_t th;
    ASSERT_EQ(0, pthread_create(&th, NULL, th_remove_analysed, NULL));
    usleep(60000);
    unlock_all_file_analysed();
    pthread_join(th, NULL);

    ASSERT_EQ_FMT(0, g_cont_remove_rc, "%d");    /* trouvée et retirée */
    ASSERT_EQ_FMT(0ULL, analysed_total(), "%llu");
    drain_all();
    PASS();
}

static void *th_restock_analysed(void *arg)
{
    (void)arg;
    restock_analysed();
    return NULL;
}

/* restock_analysed draine le pool analysed (libre) puis réinjecte dans le stock
 * principal via trylock : on ne verrouille QUE les files du stock -> la boucle
 * de réinjection tourne (dest suivant + usleep) jusqu'au déverrouillage. */
TEST restock_analysed_spins_until_stock_unlocked(void)
{
    drain_all();
    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    pk.alloc = 9;
    add_possibility_analysed(&pk, 0);

    silence_std();               /* restock_analysed journalise sa progression */
    lock_all_file();
    pthread_t th;
    int created = pthread_create(&th, NULL, th_restock_analysed, NULL);
    usleep(60000);
    unlock_all_file();
    if (created == 0) pthread_join(th, NULL);
    restore_std();
    ASSERT_EQ_FMT(0, created, "%d");

    ASSERT_EQ_FMT(0ULL, analysed_total(), "%llu");
    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu");
    drain_all();
    PASS();
}

/* --------------------------------------------------------------------------
 * Index du pool analysed : chaînes de collision
 *
 * L'index a 8191 buckets par file : insérer nettement plus de possibilités
 * DISTINCTES dans une même file garantit (pigeonhole) des buckets à >= 2
 * nœuds. Les retraits parcourent alors les chaînes (tête ET milieu de chaîne),
 * branches jamais prises avec les petits volumes des autres tests.
 * ------------------------------------------------------------------------ */
TEST analysed_index_walks_collision_chains(void)
{
    drain_all();
    enum { NCOLL = 9000 };       /* > 8191 buckets -> collisions garanties */
    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    for (int i = 0; i < NCOLL; i++) {
        pk.alloc = (uint16_t)i;
        pk.grid[dirx[0]][diry[0]] = (int16_t)(i + 1);   /* contenus distincts */
        add_possibility_analysed(&pk, 0);
    }
    ASSERT_EQ_FMT((unsigned long long)NCOLL, file_analysed_size(0), "%llu");

    int failed = 0;
    for (int i = 0; i < NCOLL; i++) {
        pk.alloc = (uint16_t)i;
        pk.grid[dirx[0]][diry[0]] = (int16_t)(i + 1);
        if (remove_possibility_analysed(&pk, 0) != 0) failed++;
    }
    ASSERT_EQ_FMT(0, failed, "%d");
    ASSERT_EQ_FMT(0ULL, file_analysed_size(0), "%llu");
    drain_all();
    PASS();
}

/* --------------------------------------------------------------------------
 * Attribution des analyses en cours (PR6) : table latérale adossée à
 * l'index « analysed », consultation
 * « que travaille X ? » via datamanager_analysed_owned_by.
 * ------------------------------------------------------------------------ */

static void fill_owner(uint8_t owner[CLIENT_UID_BYTES], uint8_t base)
{
    for (int i = 0; i < CLIENT_UID_BYTES; i++) {
        owner[i] = (uint8_t)(base + i);
    }
}

/* Callbacks de vivacité factices pour datamanager_reclaim_expired_leases (PR7,
 * correctif) : évite toute dépendance à control_registry dans ces tests --
 * c'est précisément ce que le callback découple (cf. datamanager.h). */
static uint8_t g_alive_owner[CLIENT_UID_BYTES];
static int g_alive_owner_set = 0;

static int fake_owner_alive(const uint8_t owner_uid[CLIENT_UID_BYTES])
{
    return g_alive_owner_set && memcmp(owner_uid, g_alive_owner, CLIENT_UID_BYTES) == 0;
}

static int fake_owner_never_alive(const uint8_t owner_uid[CLIENT_UID_BYTES])
{
    (void)owner_uid;
    return 0;
}

TEST add_possibility_analysed_owned_visible_via_query(void)
{
    drain_all();
    uint8_t owner_a[CLIENT_UID_BYTES], owner_b[CLIENT_UID_BYTES];
    fill_owner(owner_a, 0x10);
    fill_owner(owner_b, 0x80);

    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    pk.alloc = 42;
    add_possibility_analysed_owned(&pk, -1, owner_a);

    unsigned long long count = 999;
    int max_alloc = -999;
    ASSERT_EQ_FMT(0, datamanager_analysed_owned_by(owner_a, &count, &max_alloc), "%d");
    ASSERT_EQ_FMT(1ULL, count, "%llu");
    ASSERT_EQ_FMT(42, max_alloc, "%d");

    /* Un autre client_uid ne voit rien : la table latérale distingue bien
     * les propriétaires, elle n'est pas juste « attribué ou non ». */
    count = 999; max_alloc = -999;
    ASSERT_EQ_FMT(0, datamanager_analysed_owned_by(owner_b, &count, &max_alloc), "%d");
    ASSERT_EQ_FMT(0ULL, count, "%llu");
    ASSERT_EQ_FMT(-1, max_alloc, "%d");

    drain_all();
    PASS();
}

TEST add_possibility_analysed_without_owner_not_counted(void)
{
    drain_all();
    uint8_t owner[CLIENT_UID_BYTES];
    fill_owner(owner, 0x20);

    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    pk.alloc = 5;
    add_possibility_analysed(&pk, -1);   /* pas d'attribution (client, ou restore) */

    unsigned long long count = 999;
    int max_alloc = -999;
    ASSERT_EQ_FMT(0, datamanager_analysed_owned_by(owner, &count, &max_alloc), "%d");
    ASSERT_EQ_FMT(0ULL, count, "%llu");
    ASSERT_EQ_FMT(-1, max_alloc, "%d");

    drain_all();
    PASS();
}

TEST datamanager_analysed_owned_by_tracks_max_alloc(void)
{
    drain_all();
    uint8_t owner[CLIENT_UID_BYTES];
    fill_owner(owner, 0x30);

    int allocs[] = {12, 90, 33};
    for (int i = 0; i < 3; i++) {
        struct possibility_packet pk;
        memset(&pk, 0, sizeof pk);
        pk.alloc = (uint16_t)allocs[i];
        pk.grid[dirx[0]][diry[0]] = (int16_t)(i + 1);   /* contenus distincts */
        add_possibility_analysed_owned(&pk, -1, owner);
    }

    unsigned long long count = 0;
    int max_alloc = -1;
    ASSERT_EQ_FMT(0, datamanager_analysed_owned_by(owner, &count, &max_alloc), "%d");
    ASSERT_EQ_FMT(3ULL, count, "%llu");
    ASSERT_EQ_FMT(90, max_alloc, "%d");

    drain_all();
    PASS();
}

TEST datamanager_analysed_owned_by_null_args_returns_minus_one(void)
{
    uint8_t owner[CLIENT_UID_BYTES];
    fill_owner(owner, 0x40);
    unsigned long long count;
    int max_alloc;
    ASSERT_EQ_FMT(-1, datamanager_analysed_owned_by(NULL, &count, &max_alloc), "%d");
    ASSERT_EQ_FMT(-1, datamanager_analysed_owned_by(owner, NULL, &max_alloc), "%d");
    ASSERT_EQ_FMT(-1, datamanager_analysed_owned_by(owner, &count, NULL), "%d");
    PASS();
}

TEST remove_possibility_analysed_clears_owner_attribution(void)
{
    drain_all();
    uint8_t owner[CLIENT_UID_BYTES];
    fill_owner(owner, 0x50);

    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    pk.alloc = 17;
    add_possibility_analysed_owned(&pk, -1, owner);

    unsigned long long count = 0;
    int max_alloc = -1;
    ASSERT_EQ_FMT(0, datamanager_analysed_owned_by(owner, &count, &max_alloc), "%d");
    ASSERT_EQ_FMT(1ULL, count, "%llu");

    ASSERT_EQ_FMT(0, remove_possibility_analysed(&pk, -1), "%d");   /* acquittée */

    count = 999; max_alloc = -999;
    ASSERT_EQ_FMT(0, datamanager_analysed_owned_by(owner, &count, &max_alloc), "%d");
    ASSERT_EQ_FMT(0ULL, count, "%llu");
    ASSERT_EQ_FMT(-1, max_alloc, "%d");

    drain_all();
    PASS();
}

/* --------------------------------------------------------------------------
 * Bail à expiration des analyses en cours (PR7) :
 * analysed_lease_is_expired (fonction pure) et
 * datamanager_reclaim_expired_leases (balayage + remise en stock).
 * ------------------------------------------------------------------------ */

/* analysed_lease_is_expired : fonction pure, jamais d'horloge réelle -- toute
 * la fenêtre (avant/à/après l'échéance, et la sentinelle « désactivé ») est
 * couverte sans sleep. */
TEST analysed_lease_is_expired_pure_predicate(void)
{
    /* Sentinelle « bail désactivé » : jamais expiré, quel que soit `now`. */
    ASSERT_EQ_FMT(0, analysed_lease_is_expired(0, 0), "%d");
    ASSERT_EQ_FMT(0, analysed_lease_is_expired(0, 1000000), "%d");

    /* Avant l'échéance : pas expiré. */
    ASSERT_EQ_FMT(0, analysed_lease_is_expired(1000, 999), "%d");
    /* Pile à l'échéance : expiré (borne inclusive, cf. contrat). */
    ASSERT_EQ_FMT(1, analysed_lease_is_expired(1000, 1000), "%d");
    /* Après l'échéance : expiré. */
    ASSERT_EQ_FMT(1, analysed_lease_is_expired(1000, 1001), "%d");

    PASS();
}

/* Une possibilité attribuée dont le bail est expiré est rendue au stock non
 * vérifié, et disparaît de la consultation d'attribution (clientsWork).
 *
 * Comme `analysed_lease_seconds <= 0` désactive le bail (sentinelle « jamais
 * expiré », cf. AnalysedIndexNode / commande `leaseDuration`), on ne peut pas
 * simuler une échéance déjà passée avec une durée négative. On garde donc un
 * bail positif à l'insertion (sa valeur exacte n'importe pas ici) et on
 * simule le passage du temps en avançant `now` : `datamanager_reclaim_expired_leases`
 * ne consulte JAMAIS l'horloge elle-même, `now` est le seul levier — exactement
 * ce qui rend ce test possible sans `sleep`. */
TEST reclaim_expired_leases_returns_owned_possibility_to_stock(void)
{
    drain_all();
    int saved_lease = analysed_lease_seconds;
    analysed_lease_seconds = 60;

    uint8_t owner[CLIENT_UID_BYTES];
    fill_owner(owner, 0x60);
    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    pk.alloc = 55;
    add_possibility_analysed_owned(&pk, -1, owner);
    ASSERT_EQ_FMT(1ULL, analysed_total(), "%llu");
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");

    unsigned long long reclaimed = datamanager_reclaim_expired_leases(time(NULL) + 1000000, NULL);
    ASSERT_EQ_FMT(1ULL, reclaimed, "%llu");

    ASSERT_EQ_FMT(0ULL, analysed_total(), "%llu");
    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu");

    unsigned long long count = 999;
    int max_alloc = -999;
    ASSERT_EQ_FMT(0, datamanager_analysed_owned_by(owner, &count, &max_alloc), "%d");
    ASSERT_EQ_FMT(0ULL, count, "%llu");

    analysed_lease_seconds = saved_lease;
    drain_all();
    PASS();
}

/* Une possibilité attribuée dont le bail n'est PAS encore expiré reste en
 * place (le balayage ne rend au stock que ce qui a réellement expiré). */
TEST reclaim_expired_leases_leaves_not_yet_expired_alone(void)
{
    drain_all();
    int saved_lease = analysed_lease_seconds;
    analysed_lease_seconds = 1000000; /* très large : jamais expiré dans ce test */

    uint8_t owner[CLIENT_UID_BYTES];
    fill_owner(owner, 0x61);
    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    pk.alloc = 56;
    add_possibility_analysed_owned(&pk, -1, owner);

    unsigned long long reclaimed = datamanager_reclaim_expired_leases(time(NULL), NULL);
    ASSERT_EQ_FMT(0ULL, reclaimed, "%llu");
    ASSERT_EQ_FMT(1ULL, analysed_total(), "%llu");
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");

    analysed_lease_seconds = saved_lease;
    drain_all();
    PASS();
}

/* Une possibilité sans propriétaire connu (client ancien, ou restaurée depuis
 * un backup) n'a pas de bail : jamais rendue par ce mécanisme, même quand
 * `now` est loin dans le futur (aurait expiré si elle avait été attribuée). */
TEST reclaim_expired_leases_ignores_unowned_possibilities(void)
{
    drain_all();
    int saved_lease = analysed_lease_seconds;
    analysed_lease_seconds = 60;

    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    pk.alloc = 57;
    add_possibility_analysed(&pk, -1);   /* pas de owner_uid */

    unsigned long long reclaimed = datamanager_reclaim_expired_leases(time(NULL) + 1000000, NULL);
    ASSERT_EQ_FMT(0ULL, reclaimed, "%llu");
    ASSERT_EQ_FMT(1ULL, analysed_total(), "%llu");

    analysed_lease_seconds = saved_lease;
    drain_all();
    PASS();
}

/* Idempotence vis-à-vis d'un acquittement concurrent (section 4.3, point
 * délicat explicitement demandé par la doc) : que l'acquittement ou
 * l'expiration gagne la course, la possibilité n'est jamais ni dupliquée dans
 * le stock, ni source d'erreur pour l'autre chemin. */
TEST reclaim_expired_leases_idempotent_with_prior_ack(void)
{
    drain_all();
    int saved_lease = analysed_lease_seconds;
    analysed_lease_seconds = 60;

    uint8_t owner[CLIENT_UID_BYTES];
    fill_owner(owner, 0x62);
    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    pk.alloc = 58;
    add_possibility_analysed_owned(&pk, -1, owner);

    /* L'acquittement client (remove_possibility_analysed) gagne la course :
     * trouvé et retiré (retour 0). */
    ASSERT_EQ_FMT(0, remove_possibility_analysed(&pk, -1), "%d");
    ASSERT_EQ_FMT(0ULL, analysed_total(), "%llu");

    /* Le balayage d'expiration arrive ensuite, avec un `now` largement au-delà
     * de l'échéance qu'aurait eue cette possibilité si elle était toujours là :
     * plus rien à réclamer, jamais un doublon dans le stock. */
    unsigned long long reclaimed = datamanager_reclaim_expired_leases(time(NULL) + 1000000, NULL);
    ASSERT_EQ_FMT(0ULL, reclaimed, "%llu");
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");   /* pas de doublon */

    analysed_lease_seconds = saved_lease;
    drain_all();
    PASS();
}

TEST reclaim_expired_leases_idempotent_with_later_ack(void)
{
    drain_all();
    int saved_lease = analysed_lease_seconds;
    analysed_lease_seconds = 60;

    uint8_t owner[CLIENT_UID_BYTES];
    fill_owner(owner, 0x63);
    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    pk.alloc = 59;
    add_possibility_analysed_owned(&pk, -1, owner);

    /* Le balayage d'expiration gagne la course cette fois : rendue au stock. */
    unsigned long long reclaimed = datamanager_reclaim_expired_leases(time(NULL) + 1000000, NULL);
    ASSERT_EQ_FMT(1ULL, reclaimed, "%llu");
    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu");

    /* L'acquittement arrive ensuite, trop tard : la possibilité n'est plus
     * dans le pool analysed -- « non trouvée » (retour 1), jamais une erreur
     * bruyante ni un retrait du stock déjà reconstitué. */
    ASSERT_EQ_FMT(1, remove_possibility_analysed(&pk, -1), "%d");
    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu");   /* toujours dans le stock, intacte */

    analysed_lease_seconds = saved_lease;
    drain_all();
    PASS();
}

/* Plusieurs files : le balayage couvre bien NB_FILE_POSSIBILITY_DEFAULT files, pas
 * seulement la file 0 (thread >= 0 force la file cible à l'insertion). */
TEST reclaim_expired_leases_covers_all_files(void)
{
    drain_all();
    int saved_lease = analysed_lease_seconds;
    analysed_lease_seconds = 60;

    uint8_t owner[CLIENT_UID_BYTES];
    fill_owner(owner, 0x64);
    for (int f = 0; f < 10; f++) {
        struct possibility_packet pk;
        memset(&pk, 0, sizeof pk);
        pk.alloc = (uint16_t)(60 + f);
        pk.grid[dirx[0]][diry[0]] = (int16_t)(f + 1);   /* contenus distincts */
        add_possibility_analysed_owned(&pk, f, owner);
    }
    ASSERT_EQ_FMT(10ULL, analysed_total(), "%llu");

    unsigned long long reclaimed = datamanager_reclaim_expired_leases(time(NULL) + 1000000, NULL);
    ASSERT_EQ_FMT(10ULL, reclaimed, "%llu");
    ASSERT_EQ_FMT(0ULL, analysed_total(), "%llu");
    ASSERT_EQ_FMT(10ULL, datas_size(), "%llu");

    analysed_lease_seconds = saved_lease;
    drain_all();
    PASS();
}

/* --------------------------------------------------------------------------
 * Correctif (retour d'essais réels) : l'échéance seule ne suffit pas -- un
 * client vivant (encore observable via owner_alive) ne doit JAMAIS voir son
 * travail réclamé, aussi longtemps qu'une possibilité mette à s'analyser.
 * ------------------------------------------------------------------------ */

/* Échéance largement dépassée, mais owner_alive dit "vivant" : jamais réclamée. */
TEST reclaim_expired_leases_skips_alive_owner(void)
{
    drain_all();
    int saved_lease = analysed_lease_seconds;
    analysed_lease_seconds = 60;

    uint8_t owner[CLIENT_UID_BYTES];
    fill_owner(owner, 0x65);
    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    pk.alloc = 70;
    add_possibility_analysed_owned(&pk, -1, owner);

    memcpy(g_alive_owner, owner, CLIENT_UID_BYTES);
    g_alive_owner_set = 1;

    unsigned long long reclaimed = datamanager_reclaim_expired_leases(time(NULL) + 1000000, fake_owner_alive);
    ASSERT_EQ_FMT(0ULL, reclaimed, "%llu");
    ASSERT_EQ_FMT(1ULL, analysed_total(), "%llu");
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");

    g_alive_owner_set = 0;
    analysed_lease_seconds = saved_lease;
    drain_all();
    PASS();
}

/* Échéance dépassée ET owner_alive dit "mort" : réclamée normalement. */
TEST reclaim_expired_leases_reclaims_when_owner_not_alive(void)
{
    drain_all();
    int saved_lease = analysed_lease_seconds;
    analysed_lease_seconds = 60;

    uint8_t owner[CLIENT_UID_BYTES];
    fill_owner(owner, 0x66);
    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    pk.alloc = 71;
    add_possibility_analysed_owned(&pk, -1, owner);

    unsigned long long reclaimed = datamanager_reclaim_expired_leases(time(NULL) + 1000000, fake_owner_never_alive);
    ASSERT_EQ_FMT(1ULL, reclaimed, "%llu");
    ASSERT_EQ_FMT(0ULL, analysed_total(), "%llu");
    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu");

    analysed_lease_seconds = saved_lease;
    drain_all();
    PASS();
}

/* Le correctif AJOUTE une condition (vivacité), il n'en retire pas
 * (échéance) : sans échéance dépassée, jamais réclamée même si owner_alive
 * dit "mort". */
TEST reclaim_expired_leases_still_requires_expired_deadline_even_if_not_alive(void)
{
    drain_all();
    int saved_lease = analysed_lease_seconds;
    analysed_lease_seconds = 1000000; /* jamais expiré dans ce test */

    uint8_t owner[CLIENT_UID_BYTES];
    fill_owner(owner, 0x67);
    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    pk.alloc = 72;
    add_possibility_analysed_owned(&pk, -1, owner);

    unsigned long long reclaimed = datamanager_reclaim_expired_leases(time(NULL), fake_owner_never_alive);
    ASSERT_EQ_FMT(0ULL, reclaimed, "%llu");
    ASSERT_EQ_FMT(1ULL, analysed_total(), "%llu");

    analysed_lease_seconds = saved_lease;
    drain_all();
    PASS();
}

/* check_duplicate : le stock est réparti sur plusieurs files (split_datas) et
 * assez fourni pour que le partitionnement entre threads fasse avancer son
 * curseur d'une file à la suivante (fp++/position=0) — en plus de la
 * comparaison croisée inter-files qui détecte le doublon. */
TEST check_duplicate_detects_identical_packets_across_files(void)
{
    drain_all();
    enum { NDUP = 24 };
    struct possibility_packet pks[NDUP];
    memset(pks, 0, sizeof pks);
    for (int i = 0; i < NDUP; i++) {
        pks[i].alloc = 2;
        /* 22 contenus distincts + 1 paire identique (i == 0 et i == 1). */
        pks[i].grid[dirx[0]][diry[0]] = (int16_t)(i == 1 ? 1 : i + 1);
    }
    array_possibility_packet arr = { .size = NDUP, .possibilities = pks };
    add_possibility(NULL, &arr);

    silence_std();
    split_datas();               /* redistribue le stock sur plusieurs files */
    int rc = check_duplicate();
    restore_std();
    ASSERT_EQ_FMT(-1, rc, "%d");

    drain_all();
    PASS();
}

/* check_duplicate : deux possibilités STRICTEMENT identiques dans le stock ->
 * le doublon est détecté (branche « equals » + compteur d'erreurs) et la
 * fonction renvoie -1. */
TEST check_duplicate_detects_identical_packets(void)
{
    drain_all();
    struct possibility_packet pks[2];
    memset(pks, 0, sizeof pks);
    pks[0].alloc = 2;
    pks[0].grid[dirx[0]][diry[0]] = 5;
    pks[1] = pks[0];             /* copie parfaite : doublon */
    array_possibility_packet arr = { .size = 2, .possibilities = pks };
    add_possibility(NULL, &arr);

    silence_std();
    int rc = check_duplicate();
    restore_std();
    ASSERT_EQ_FMT(-1, rc, "%d");

    drain_all();
    PASS();
}

/* --------------------------------------------------------------------------
 * expand_datas_to_level : expansion du stock au démarrage du serveur
 *
 * Fixtures autonomes (indépendantes de pieces.csv / ETERN_PARTS) : une map
 * « libre » dont chaque clé renvoie les mêmes 8 pièces candidates (ids 1..8),
 * et un tableau de rotations aux faces PETITES (< sizearray) pour que les clés
 * calculées par what_search_to_key indexent flat[3^4] sans déborder. 8
 * candidats entretiennent le branchement sur > EXPAND_MAX_LEVELS niveaux (avec
 * seulement 2 pièces, toutes les branches mourraient dès le 2e placement).
 * ------------------------------------------------------------------------ */
static struct array_part *make_expand_parts(void)
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

static map_big_array *make_expand_free_map(void)
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
static void seed_genesis(uint16_t alloc)
{
    struct possibility_packet g;
    memset(&g, 0, sizeof g);
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            g.grid[x][y] = -2;
    g.alloc = alloc;
    g.x = dirx[alloc];
    g.y = diry[alloc];
    g.checked = 0;
    array_possibility_packet arr = { .size = 1, .possibilities = &g };
    add_possibility(NULL, &arr);
}

/* Développe le stock et fait grossir le nombre de possibilités jusqu'au niveau
   cible ; toutes atteignent alloc >= cible. */
TEST expand_grows_stock_and_advances_level(void)
{
    drain_all();
    seed_genesis(0);
    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu");

    int passes = expand_datas_to_level(2, make_expand_free_map(), make_expand_parts());

    ASSERT_EQ_FMT(2, passes, "%d");                 /* alloc 0 → 2 : 2 passes */
    ASSERT(datas_size() > 1);                        /* le stock a grossi */
    /* Toutes les possibilités produites ont atteint le niveau cible. */
    unsigned long long n = datas_size();
    array_possibility_packet *r = get_last_possibility(NULL, (int)n);
    for (int i = 0; i < r->size; i++) {
        ASSERT(r->possibilities[i].alloc >= 2);
    }
    free_array_possibility_packet(r);

    drain_all();
    PASS();
}

/* Possibilité déjà au niveau cible : aucune passe, stock inchangé. */
TEST expand_noop_when_already_deep_enough(void)
{
    drain_all();
    seed_genesis(5);                                 /* alloc 5 ≥ cible 3 */
    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu");

    int passes = expand_datas_to_level(3, make_expand_free_map(), make_expand_parts());

    ASSERT_EQ_FMT(0, passes, "%d");                  /* rien à approfondir */
    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu");        /* stock inchangé */
    array_possibility_packet *r = get_last_possibility(NULL, 1);
    ASSERT_EQ_FMT(1, r->size, "%d");
    ASSERT_EQ_FMT(5, (int)r->possibilities[0].alloc, "%d");
    free_array_possibility_packet(r);

    drain_all();
    PASS();
}

/* Consigne de niveau très élevée : le plafond de PROFONDEUR (EXPAND_MAX_LEVELS)
   arrête l'expansion — pas de boucle infinie, stock borné. */
TEST expand_depth_cap_limits_passes(void)
{
    /* expand_max_levels/expand_max_stock sont des globales runtime (options CLI
       --expand-max-levels/--expand-max-stock) : les remettre à leur défaut ici
       protège cette assertion d'une pollution par un autre test du même
       binaire qui les aurait modifiées. */
    expand_max_levels = EXPAND_MAX_LEVELS;
    expand_max_stock = EXPAND_MAX_STOCK;

    drain_all();
    seed_genesis(0);

    int passes = expand_datas_to_level(100, make_expand_free_map(), make_expand_parts());

    ASSERT_EQ_FMT(expand_max_levels, passes, "%d"); /* borné par la profondeur */
    ASSERT(datas_size() > 1);                         /* a bien produit du stock */
    ASSERT(datas_size() < (unsigned long long)expand_max_stock); /* et resté borné */

    drain_all();
    PASS();
}

/* Niveau ≤ 0 : no-op strict (garde-fou d'appel). */
TEST expand_zero_level_is_noop(void)
{
    drain_all();
    seed_genesis(0);

    int passes = expand_datas_to_level(0, make_expand_free_map(), make_expand_parts());

    ASSERT_EQ_FMT(0, passes, "%d");
    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu");

    drain_all();
    PASS();
}

SUITE(datamanager_suite)
{
    RUN_TEST(server_ip_round_trip);
    RUN_TEST(send_solution_without_client_is_local_noop);
    RUN_TEST(send_solution_without_server_configured_returns_error);
    RUN_TEST(add_increases_datas_size);
    RUN_TEST(get_last_possibility_drains_pool);
    RUN_TEST(put_and_scroll_round_trip_succeeds_when_pool_free);
    RUN_TEST(search_min_datas_finds_minimum);
    RUN_TEST(backup_then_restore_preserves_count);
    RUN_TEST(restore_missing_file_returns_error);
    RUN_TEST(backup_and_import_return_error_on_bad_path);
    RUN_TEST(backup_leaves_no_residual_tmp_file);
    RUN_TEST(backup_skipped_during_maintenance_reports_distinct_code);
    RUN_TEST(backup_failure_preserves_previous_file);
    RUN_TEST(consistent_backup_round_trip_preserves_both_pools);
    RUN_TEST(consistent_backup_skipped_during_maintenance_reports_distinct_code);
    RUN_TEST(consistent_backup_analysed_open_failure_aborts_stock_too);
    RUN_TEST(split_then_regroup_preserves_count);
    RUN_TEST(split_datas_balances_within_one_of_target);
    RUN_TEST(rebalance_step_preserves_total_count);
    RUN_TEST(rebalance_step_converges_to_balance);
    RUN_TEST(rebalance_step_respects_budget);
    RUN_TEST(rebalance_step_uses_full_budget_across_multiple_pairs);
    RUN_TEST(rebalance_step_noop_when_already_balanced);
    RUN_TEST(configure_stock_files_rejects_non_positive);
    RUN_TEST(configure_stock_files_clamps_to_max);
    RUN_TEST(configure_stock_files_new_files_are_usable);
    RUN_TEST(configure_stock_files_shrinking_back_is_safe);
    RUN_TEST(checked_possibility_goes_to_checked_pool);
    RUN_TEST(analysed_add_and_restock);
    RUN_TEST(analysed_backup_restore_round_trip);
    RUN_TEST(analysed_restore_clears_untracked_packet);
    RUN_TEST(sort_preserves_count);
    RUN_TEST(statistic_and_print_run);
    RUN_TEST(statistic_datas_handles_full_board_alloc);
    RUN_TEST(stock_distribution_separates_the_three_pools);
    RUN_TEST(stock_distribution_on_empty_stock_is_all_zero);
    RUN_TEST(stock_distribution_counts_full_board_alloc);
    RUN_TEST(stock_distribution_totals_match_datas_size);
    RUN_TEST(count_combinations_is_triangular);
    RUN_TEST(get_tocheck_drains_unchecked_pool);
    RUN_TEST(remove_analysed_finds_then_misses);
    RUN_TEST(remove_analysed_handles_duplicate_packets);
    RUN_TEST(remove_analysed_after_restock_not_found);
    RUN_TEST(remove_analysed_searches_all_files_when_thread_negative);
    RUN_TEST(remove_no_next_prunes_dead_packets);
    RUN_TEST(remove_no_next_handles_complete_solution);
    RUN_TEST(scroll_from_server_returns_packet);
    RUN_TEST(scroll_from_server_returns_empty);
    RUN_TEST(scroll_from_server_reassembles_fragmented_packet);
    RUN_TEST(scroll_from_server_pruner_batch_receives_all);
    RUN_TEST(scroll_from_server_pruner_batch_clamps_k_to_requested);
    RUN_TEST(scroll_from_server_pruner_batch_empty);
    RUN_TEST(scroll_from_server_pruner_batch_incomplete_block);
    RUN_TEST(send_possibility_analysed_success);
    RUN_TEST(send_possibility_analysed_bad_ack_requeues_and_reindexes);
    RUN_TEST(put_to_server_bad_ack_non_fatal);
    RUN_TEST(send_solution_success);
    RUN_TEST(send_solution_server_rejects);
    RUN_TEST(put_to_server_success);
    RUN_TEST(put_to_server_connection_lost);
    RUN_TEST(connect_and_handshake_ok);
    RUN_TEST(connect_handshake_version_rejected);
    RUN_TEST(connect_handshake_retry);
    RUN_TEST(connect_create_tcp_client_fails);
    RUN_TEST(check_files_reports_consistent_stock);
    RUN_TEST(file_size_accessors_reject_out_of_range);
    RUN_TEST(check_one_file_flags_each_inconsistency);
    RUN_TEST(check_datas_empty_stock_is_ok);
    RUN_TEST(check_datas_flags_invalid_packet);
    RUN_TEST(sort_descending_mthread_preserves_count);
    RUN_TEST(regroup_split_nolock_preserve_count);
    RUN_TEST(check_duplicate_empty_stock_returns_immediately);
    RUN_TEST(check_duplicate_small_stock_no_error);
    RUN_TEST(check_duplicate_detects_identical_packets);
    RUN_TEST(check_duplicate_detects_identical_packets_across_files);

    RUN_TEST(add_possibility_spins_until_lock_released);
    RUN_TEST(add_possibility_gives_up_when_stock_never_unlocked);
    RUN_TEST(get_last_possibility_spins_until_lock_released);
    RUN_TEST(get_last_possibility_gives_up_when_stock_never_unlocked);
    RUN_TEST(add_possibility_analysed_spins_both_modes);
    RUN_TEST(add_possibility_analysed_gives_up_when_pool_never_unlocked_rotating);
    RUN_TEST(add_possibility_analysed_gives_up_when_pool_never_unlocked_pinned);
    RUN_TEST(remove_possibility_analysed_spins_until_lock_released);
    RUN_TEST(restock_analysed_spins_until_stock_unlocked);
    RUN_TEST(analysed_index_walks_collision_chains);
    RUN_TEST(add_possibility_analysed_owned_visible_via_query);
    RUN_TEST(add_possibility_analysed_without_owner_not_counted);
    RUN_TEST(datamanager_analysed_owned_by_tracks_max_alloc);
    RUN_TEST(datamanager_analysed_owned_by_null_args_returns_minus_one);
    RUN_TEST(remove_possibility_analysed_clears_owner_attribution);
    RUN_TEST(analysed_lease_is_expired_pure_predicate);
    RUN_TEST(reclaim_expired_leases_returns_owned_possibility_to_stock);
    RUN_TEST(reclaim_expired_leases_leaves_not_yet_expired_alone);
    RUN_TEST(reclaim_expired_leases_ignores_unowned_possibilities);
    RUN_TEST(reclaim_expired_leases_idempotent_with_prior_ack);
    RUN_TEST(reclaim_expired_leases_idempotent_with_later_ack);
    RUN_TEST(reclaim_expired_leases_covers_all_files);
    RUN_TEST(reclaim_expired_leases_skips_alive_owner);
    RUN_TEST(reclaim_expired_leases_reclaims_when_owner_not_alive);
    RUN_TEST(reclaim_expired_leases_still_requires_expired_deadline_even_if_not_alive);
    RUN_TEST(import_json_loads_single_possibility);
    RUN_TEST(print_duplicate_activity_aggregates_counters);
    RUN_TEST(add_possibility_null_and_mixed_routing);
    RUN_TEST(network_paths_fail_gracefully_when_unreachable);
    RUN_TEST(put_to_server_updates_max_result);
    RUN_TEST(scroll_from_server_count_recv_fails);
    RUN_TEST(scroll_from_server_rejects_aberrant_count);
    RUN_TEST(scroll_from_server_incomplete_packet_detected);
    RUN_TEST(send_analysed_batch_drains_multiple_of_cap);
    RUN_TEST(send_analysed_batch_cap_defaults_to_one);
    RUN_TEST(send_analysed_with_empty_file_sends_nothing);
    RUN_TEST(backup_rename_onto_directory_fails);
    RUN_TEST(backup_covers_checked_pool_and_restore_drains_both);
    RUN_TEST(restore_sanitizes_legacy_checked_flag);
    RUN_TEST(import_json_drains_existing_stock);
    RUN_TEST(print_all_file_analysed_lists_packets);
    RUN_TEST(fprint_datamanager_writes_all_possibilities_to_file);
    RUN_TEST(fprint_file_analysed_exports_only_requested_file);
    RUN_TEST(fprint_all_file_analysed_aggregates_every_file);
    RUN_TEST(fprint_file_analysed_count_accumulates_across_calls);
    RUN_TEST(fprint_file_analysed_accepts_null_count);
    RUN_TEST(remove_no_next_removes_dead_packet_at_head);
    RUN_TEST(rmnonext_solution_with_stop_on_solution_exits);
    RUN_TEST(check_duplicate_flags_duplicates_and_origins);
    RUN_TEST(check_duplicate_multi_thread_across_files);
    RUN_TEST(sort_large_shuffled_stock_both_directions);
    RUN_TEST(expand_grows_stock_and_advances_level);
    RUN_TEST(expand_noop_when_already_deep_enough);
    RUN_TEST(expand_depth_cap_limits_passes);
    RUN_TEST(expand_zero_level_is_noop);
}

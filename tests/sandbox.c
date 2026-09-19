/*
 * Implémentation du bac à sable du runner. Le POURQUOI est dans sandbox.h.
 */
#include "sandbox.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "core/core_static_variables.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* --- Photographie d'un répertoire (noms de ses entrées directes) ----------- */
typedef struct {
    char **names;
    size_t count;
} dir_listing_t;

static char      s_origin[PATH_MAX];
static char      s_dir[PATH_MAX];
static dir_listing_t s_before;
static pid_t     s_owner;
static int       s_keep;
static int       s_active;

static int name_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void listing_free(dir_listing_t *l)
{
    for (size_t i = 0; i < l->count; i++) free(l->names[i]);
    free(l->names);
    l->names = NULL;
    l->count = 0;
}

/* Lit les entrées directes de `dir` (hors "." et ".."), triées. 0 = ok. */
static int listing_read(const char *dir, dir_listing_t *out)
{
    out->names = NULL;
    out->count = 0;

    DIR *d = opendir(dir);
    if (d == NULL) return -1;

    size_t cap = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (out->count == cap) {
            size_t ncap = (cap == 0) ? 64 : cap * 2;
            char **grown = realloc(out->names, ncap * sizeof *grown);
            if (grown == NULL) { closedir(d); listing_free(out); return -1; }
            out->names = grown;
            cap = ncap;
        }
        char *dup = strdup(e->d_name);
        if (dup == NULL) { closedir(d); listing_free(out); return -1; }
        out->names[out->count++] = dup;
    }
    closedir(d);

    if (out->count > 1) qsort(out->names, out->count, sizeof *out->names, name_cmp);
    return 0;
}

/* --- Effacement récursif du bac à sable ------------------------------------ */
/*
 * On n'utilise volontairement ni nftw() ni `rm -rf` : la première demande des
 * macros de test de fonctionnalités qui varient entre glibc et libSystem, la
 * seconde un shell. unlink() traite aussi bien les fichiers ordinaires que les
 * sockets Unix laissées par l'IPC parent<->fork.
 */
static void remove_tree(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) return;

    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (d != NULL) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
                char child[PATH_MAX];
                int n = snprintf(child, sizeof child, "%s/%s", path, e->d_name);
                if (n > 0 && (size_t)n < sizeof child) remove_tree(child);
            }
            closedir(d);
        }
        rmdir(path);
    } else {
        unlink(path);
    }
}

static void sandbox_cleanup(void)
{
    /*
     * Un fils forké par un test (fork_assert.h) hérite de la chaîne atexit :
     * sans ce garde-fou, son exit() effacerait le bac à sable SOUS le parent
     * encore vivant — c'est le même piège que les sockets Unix de
     * local_socket.c, qui ne sont supprimées que par le pid propriétaire.
     */
    if (!s_active || getpid() != s_owner) return;

    if (s_keep) {
        fprintf(stderr, "\nbac à sable conservé pour analyse : %s\n", s_dir);
        return;
    }
    /* Ne pas rester dans un répertoire qu'on efface. */
    if (chdir(s_origin) != 0) { /* sans conséquence : le process se termine */ }
    remove_tree(s_dir);
}

/* --- Résolution des chemins de lecture ------------------------------------- */
/*
 * Préfixe un chemin relatif par le répertoire de lancement. On n'appelle PAS
 * realpath() : le fichier peut ne pas exister (tailles « clone », dont aucun jeu
 * de pièces n'est livré) et on veut malgré tout que le chemin désigne encore
 * l'endroit qu'il désignait avant le chdir. La mémoire n'est jamais libérée :
 * elle vit le temps du process, comme le littéral qu'elle remplace.
 */
static void absolutize(char **slot)
{
    if (*slot == NULL || (*slot)[0] == '/') return;

    const char *rel = *slot;
    if (rel[0] == '.' && rel[1] == '/') rel += 2;

    size_t n = strlen(s_origin) + 1 + strlen(rel) + 1;
    char *abs = malloc(n);
    if (abs == NULL) {
        fprintf(stderr, "bac à sable : allocation impossible pour « %s »\n", *slot);
        exit(EXIT_FAILURE);
    }
    snprintf(abs, n, "%s/%s", s_origin, rel);
    *slot = abs;
}

/* --- API ------------------------------------------------------------------- */
void test_sandbox_enter(void)
{
    if (s_active) return;

    if (getcwd(s_origin, sizeof s_origin) == NULL) {
        fprintf(stderr, "bac à sable : getcwd a échoué (%s) — on refuse de "
                        "lancer les tests sans isolation\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    /* Chemins de LECTURE du dépôt : absolus AVANT le chdir. */
    absolutize(&parts_files);
    absolutize(&indices_file);

    if (listing_read(s_origin, &s_before) != 0) {
        fprintf(stderr, "bac à sable : impossible de photographier « %s » (%s)\n",
                s_origin, strerror(errno));
        exit(EXIT_FAILURE);
    }

    /*
     * Base /tmp et non $TMPDIR : sous macOS $TMPDIR est un chemin
     * /var/folders/… d'une soixantaine de caractères, et les tests créent dans
     * le répertoire courant des sockets Unix (etii_main.<pid>) dont le chemin
     * complet doit tenir dans les ~104 octets de sun_path.
     */
    int n = snprintf(s_dir, sizeof s_dir, "/tmp/etii_run_tests_XXXXXX");
    if (n <= 0 || (size_t)n >= sizeof s_dir || mkdtemp(s_dir) == NULL) {
        fprintf(stderr, "bac à sable : mkdtemp a échoué (%s)\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
    if (chdir(s_dir) != 0) {
        fprintf(stderr, "bac à sable : chdir(%s) a échoué (%s)\n", s_dir, strerror(errno));
        rmdir(s_dir);
        exit(EXIT_FAILURE);
    }
    /*
     * On relit le chemin CANONIQUE : sous macOS /tmp est un lien vers
     * /private/tmp, donc getcwd() ne rend pas la chaîne passée à mkdtemp. Sans
     * cette normalisation, tout test comparant son getcwd() au bac à sable
     * échouerait sur un simple écart de forme.
     */
    if (getcwd(s_dir, sizeof s_dir) == NULL) {
        fprintf(stderr, "bac à sable : getcwd après chdir a échoué (%s)\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    s_owner  = getpid();
    s_active = 1;
    atexit(sandbox_cleanup);
}

const char *test_sandbox_origin(void) { return s_origin; }
const char *test_sandbox_dir(void)    { return s_active ? s_dir : ""; }
void        test_sandbox_keep(void)   { s_keep = 1; }

int test_sandbox_origin_diff(char *out, size_t outsz)
{
    if (outsz > 0) out[0] = '\0';
    if (!s_active) {
        snprintf(out, outsz, "bac à sable non entré (test_sandbox_enter non appelé)");
        return -1;
    }

    dir_listing_t now;
    if (listing_read(s_origin, &now) != 0) {
        snprintf(out, outsz, "répertoire de lancement illisible : %s", s_origin);
        return -1;
    }

    /* Fusion de deux listes triées : '+' = apparu, '-' = disparu. */
    int    diffs = 0;
    size_t i = 0, j = 0, used = 0;
    while (i < s_before.count || j < now.count) {
        int cmp;
        if      (i == s_before.count) cmp =  1;
        else if (j == now.count)      cmp = -1;
        else                          cmp = strcmp(s_before.names[i], now.names[j]);

        const char *sign = NULL, *name = NULL;
        if (cmp == 0)      { i++; j++; }
        else if (cmp < 0)  { sign = "-"; name = s_before.names[i++]; }
        else               { sign = "+"; name = now.names[j++]; }

        if (sign != NULL) {
            diffs++;
            if (used + 1 < outsz) {
                int w = snprintf(out + used, outsz - used, "%s%s %s",
                                 used ? ", " : "", sign, name);
                if (w > 0) used += (size_t)w;
                if (used >= outsz) used = outsz - 1;
            }
        }
    }

    listing_free(&now);
    return diffs;
}

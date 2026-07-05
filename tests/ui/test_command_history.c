/*
 * Tests unitaires du module command_history.c (historique de commandes en
 * mémoire, ring buffer de HISTORY_MAX=100 entrées).
 *
 * command_history.c n'a aucune dépendance hors libc mais conserve un état
 * global statique persistant entre les tests, sans API de remise à zéro. Les
 * tests sont donc écrits pour être indépendants de l'ordre d'exécution : ils
 * raisonnent en relatif (delta de history_size, dernières entrées ajoutées)
 * plutôt que sur une taille absolue.
 */
#include "greatest.h"
#include "ui/command_history.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HISTORY_MAX 100 /* doit correspondre à la constante de command_history.c */

/* history_add ignore les chaînes vides et NULL. */
TEST add_ignores_null_and_empty(void)
{
    int before = history_size();
    history_add(NULL);
    history_add("");
    ASSERT_EQ_FMT(before, history_size(), "%d");
    PASS();
}

/* Après un ajout, l'entrée la plus récente est à l'index 0. */
TEST get_returns_most_recent_at_index_zero(void)
{
    history_add("alpha");
    history_add("beta");
    ASSERT_STR_EQ("beta", history_get(0));
    ASSERT_STR_EQ("alpha", history_get(1));
    PASS();
}

/* Les doublons consécutifs sont ignorés (comportement shell). */
TEST add_ignores_consecutive_duplicates(void)
{
    history_add("uniqueA");
    int after_first = history_size();
    history_add("uniqueA");
    history_add("uniqueA");
    ASSERT_EQ_FMT(after_first, history_size(), "%d");
    ASSERT_STR_EQ("uniqueA", history_get(0));
    PASS();
}

/* Un doublon non consécutif est bien réenregistré. */
TEST add_keeps_non_consecutive_duplicates(void)
{
    history_add("dupX");
    history_add("between");
    history_add("dupX");
    ASSERT_STR_EQ("dupX", history_get(0));
    ASSERT_STR_EQ("between", history_get(1));
    ASSERT_STR_EQ("dupX", history_get(2));
    PASS();
}

/* Index hors bornes (négatif ou >= taille) renvoie NULL. */
TEST get_out_of_bounds_returns_null(void)
{
    history_add("only");
    ASSERT_EQ(NULL, history_get(-1));
    ASSERT_EQ(NULL, history_get(history_size())); /* premier index hors bornes */
    ASSERT_EQ(NULL, history_get(history_size() + 5));
    PASS();
}

/* history_size est plafonné à HISTORY_MAX même après débordement du ring. */
TEST size_is_capped_at_history_max(void)
{
    char buf[32];
    for (int i = 0; i < HISTORY_MAX + 50; i++) {
        snprintf(buf, sizeof(buf), "cmd-%d", i);
        history_add(buf);
    }
    ASSERT_EQ_FMT(HISTORY_MAX, history_size(), "%d");

    /* La dernière ajoutée reste accessible à l'index 0... */
    snprintf(buf, sizeof(buf), "cmd-%d", HISTORY_MAX + 50 - 1);
    ASSERT_STR_EQ(buf, history_get(0));

    /* ...et l'entrée la plus ancienne encore conservée est cohérente avec le
       ring (la plus vieille a été évincée). L'index HISTORY_MAX est hors bornes. */
    ASSERT_EQ(NULL, history_get(HISTORY_MAX));
    PASS();
}

/* ------------------------------------------------------------------------- */
/*  Persistance sur disque : history_load / history_save / chemin par défaut   */
/* ------------------------------------------------------------------------- */

/* Crée un chemin de fichier temporaire unique (jamais dans le HOME ni le repo)
   et le renvoie dans out (taille out_sz). Le fichier créé par mkstemp est
   supprimé aussitôt : on ne garde que le chemin, réécrit par les tests. */
static int make_temp_path(char *out, size_t out_sz)
{
    char template[] = "/tmp/etii_hist_XXXXXX";
    int fd = mkstemp(template);
    if (fd < 0) return -1;
    close(fd);
    remove(template);
    if (strlen(template) + 1 > out_sz) return -1;
    strcpy(out, template);
    return 0;
}

/* history_default_path construit $HOME/.eternityII_history, ou ./… sans HOME. */
TEST default_path_uses_home_then_cwd(void)
{
    char buf[512];

    /* Mémorise le HOME courant pour le restaurer en fin de test (ne pas
       polluer l'environnement des autres suites). */
    char *orig_home = getenv("HOME");
    char orig_home_copy[4096] = {0};
    if (orig_home != NULL) {
        strncpy(orig_home_copy, orig_home, sizeof orig_home_copy - 1);
    }

    setenv("HOME", "/home/tester", 1);
    ASSERT_EQ(buf, history_default_path(buf, sizeof buf));
    ASSERT_STR_EQ("/home/tester/.eternityII_history", buf);

    unsetenv("HOME");
    ASSERT_EQ(buf, history_default_path(buf, sizeof buf));
    ASSERT_STR_EQ("./.eternityII_history", buf);

    /* HOME défini mais vide → traité comme absent (repli sur le CWD). */
    setenv("HOME", "", 1);
    ASSERT_EQ(buf, history_default_path(buf, sizeof buf));
    ASSERT_STR_EQ("./.eternityII_history", buf);

    /* Entrées invalides → NULL, pas de chemin tronqué. */
    setenv("HOME", "/home/tester", 1);
    ASSERT_EQ(NULL, history_default_path(buf, 4));   /* tampon trop petit */
    ASSERT_EQ(NULL, history_default_path(buf, 0));    /* taille nulle       */
    ASSERT_EQ(NULL, history_default_path(NULL, sizeof buf)); /* buf NULL    */

    if (orig_home != NULL) {
        setenv("HOME", orig_home_copy, 1);
    } else {
        unsetenv("HOME");
    }
    PASS();
}

/* Charger un fichier absent ne change rien et ne plante pas (premier lancement). */
TEST load_missing_file_is_noop(void)
{
    int before = history_size();
    history_load("/tmp/etii_hist_does_not_exist_42424242");
    history_load(NULL);
    ASSERT_EQ_FMT(before, history_size(), "%d");
    PASS();
}

/* save écrit les entrées dans l'ordre chronologique (plus ancienne en premier). */
TEST save_writes_chronological_order(void)
{
    char path[512];
    ASSERT_EQ(0, make_temp_path(path, sizeof path));

    history_add("chronoA");
    history_add("chronoB");
    history_add("chronoC");
    ASSERT_EQ(0, history_save(path));

    /* Relit le fichier : les trois dernières lignes doivent être A, B, C. */
    FILE *f = fopen(path, "r");
    ASSERT(f != NULL);
    char lines[3][64] = {{0}, {0}, {0}};
    char line[256];
    int n = 0;
    while (fgets(line, sizeof line, f) != NULL) {
        size_t l = strlen(line);
        while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
        /* garde une fenêtre glissante des trois dernières lignes */
        strcpy(lines[0], lines[1]);
        strcpy(lines[1], lines[2]);
        strncpy(lines[2], line, sizeof lines[2] - 1);
        n++;
    }
    fclose(f);
    remove(path);
    ASSERT(n >= 3);
    ASSERT_STR_EQ("chronoA", lines[0]);
    ASSERT_STR_EQ("chronoB", lines[1]);
    ASSERT_STR_EQ("chronoC", lines[2]);
    PASS();
}

/* load respecte l'ordre, la dédup des doublons consécutifs et la troncature à
   HISTORY_MAX. On écrit >HISTORY_MAX lignes distinctes : le buffer est alors
   entièrement remplacé par la queue chargée, ce qui isole l'état des tests
   précédents. */
TEST load_respects_order_dedup_and_cap(void)
{
    char path[512];
    ASSERT_EQ(0, make_temp_path(path, sizeof path));

    FILE *f = fopen(path, "w");
    ASSERT(f != NULL);
    /* n0 … n99 (HISTORY_MAX lignes distinctes)… */
    for (int i = 0; i < HISTORY_MAX; i++) {
        fprintf(f, "n%d\n", i);
    }
    /* …un doublon consécutif de la dernière (doit être ignoré au chargement)… */
    fprintf(f, "n%d\n", HISTORY_MAX - 1);
    /* …puis une entrée finale. */
    fprintf(f, "tail\n");
    fclose(f);

    history_load(path);
    remove(path);

    /* Le ring est plafonné à HISTORY_MAX. */
    ASSERT_EQ_FMT(HISTORY_MAX, history_size(), "%d");

    /* Séquence effective après dédup : n0 … n99, tail (101 items) → on conserve
       les 100 derniers : n1 … n99, tail. */
    ASSERT_STR_EQ("tail", history_get(0));
    ASSERT_STR_EQ("n99", history_get(1));
    /* Si la dédup avait échoué, get(2) vaudrait encore "n99" ; avec dédup c'est n98. */
    ASSERT_STR_EQ("n98", history_get(2));
    /* Entrée la plus ancienne encore conservée : n1 (n0 évincé par la troncature). */
    ASSERT_STR_EQ("n1", history_get(HISTORY_MAX - 1));
    PASS();
}

/* Round-trip complet save → load : ce qui est sauvegardé est rechargé à
   l'identique (ordre + contenu), en repartant d'un buffer entièrement remplacé. */
TEST save_then_load_round_trip(void)
{
    char path[512];
    ASSERT_EQ(0, make_temp_path(path, sizeof path));

    /* Remplit avec HISTORY_MAX entrées distinctes pour maîtriser tout le ring. */
    char buf[32];
    for (int i = 0; i < HISTORY_MAX; i++) {
        snprintf(buf, sizeof buf, "rt-%d", i);
        history_add(buf);
    }
    ASSERT_EQ(0, history_save(path));

    /* Recharge : le buffer (déjà plein d'exactement ces entrées) est réécrit à
       l'identique — l'ordre et le contenu doivent être préservés. */
    history_load(path);
    remove(path);

    ASSERT_EQ_FMT(HISTORY_MAX, history_size(), "%d");
    snprintf(buf, sizeof buf, "rt-%d", HISTORY_MAX - 1);
    ASSERT_STR_EQ(buf, history_get(0));         /* plus récente */
    ASSERT_STR_EQ("rt-0", history_get(HISTORY_MAX - 1)); /* plus ancienne */
    PASS();
}

/* save renvoie -1 sans crasher sur les entrées/chemins invalides. */
TEST save_reports_failure_on_bad_paths(void)
{
    /* Chemin NULL. */
    ASSERT_EQ(-1, history_save(NULL));

    /* Répertoire cible inexistant → fopen du fichier .tmp échoue. */
    ASSERT_EQ(-1, history_save("/no_such_dir_4242/etii_hist"));

    /* Chemin trop long pour le tampon temporaire interne (« %s.tmp ») → -1. */
    char longpath[1400];
    memset(longpath, 'a', sizeof longpath - 1);
    longpath[sizeof longpath - 1] = '\0';
    ASSERT_EQ(-1, history_save(longpath));
    PASS();
}

/* load tolère les fins de ligne Windows (\r\n) : le \r est retiré. */
TEST load_strips_crlf_line_endings(void)
{
    char path[512];
    ASSERT_EQ(0, make_temp_path(path, sizeof path));

    FILE *f = fopen(path, "wb");
    ASSERT(f != NULL);
    /* >HISTORY_MAX lignes distinctes en CRLF pour remplacer tout le ring. */
    for (int i = 0; i < HISTORY_MAX + 5; i++) {
        fprintf(f, "crlf%d\r\n", i);
    }
    fclose(f);

    history_load(path);
    remove(path);

    /* Le \r ne doit pas subsister dans l'entrée la plus récente. */
    char expected[32];
    snprintf(expected, sizeof expected, "crlf%d", HISTORY_MAX + 5 - 1);
    ASSERT_STR_EQ(expected, history_get(0));
    PASS();
}

SUITE(command_history_suite)
{
    RUN_TEST(add_ignores_null_and_empty);
    RUN_TEST(get_returns_most_recent_at_index_zero);
    RUN_TEST(add_ignores_consecutive_duplicates);
    RUN_TEST(add_keeps_non_consecutive_duplicates);
    RUN_TEST(get_out_of_bounds_returns_null);
    RUN_TEST(size_is_capped_at_history_max);
    RUN_TEST(default_path_uses_home_then_cwd);
    RUN_TEST(load_missing_file_is_noop);
    RUN_TEST(save_writes_chronological_order);
    RUN_TEST(load_respects_order_dedup_and_cap);
    RUN_TEST(save_then_load_round_trip);
    RUN_TEST(save_reports_failure_on_bad_paths);
    RUN_TEST(load_strips_crlf_line_endings);
}

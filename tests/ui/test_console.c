/*
 * Test de console.c (boucle interactive ANSI).
 *
 * console() boucle indéfiniment en lisant stdin jusqu'à ce qu'une commande
 * provoque exit(). On l'exécute donc dans un processus fils, stdin alimenté par
 * un fichier contenant "exit\n" : en mode serveur (server = 1) l'interprète
 * `exit` appelle exit(EXIT_SUCCESS) immédiatement.
 *
 * Couvre : console(), getcmdline() / getcmdline_cooked() (stdin non-tty ->
 * fallback ligne-par-ligne), status_zone_init() (non-tty -> no-op), et le
 * dispatch do_command_line("exit").
 */
/* Doit précéder tout include : sur la glibc, posix_openpt/grantpt/unlockpt/ptsname
   ne sont déclarés qu'avec _XOPEN_SOURCE >= 600 (ou _GNU_SOURCE). Sans ça, sous
   -std=gnu99 (qui n'active que _DEFAULT_SOURCE) ils sont implicitement déclarés et
   ptsname() est supposée renvoyer un int -> pointeur tronqué -> open() échoue. */
#define _GNU_SOURCE 1
#include "greatest.h"
#include "ui/console.h"
#include "app/app_static_variables.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>

/* console() n'est pas déclarée dans console.h (seul run_console l'est). */
void *console(void *param);

TEST console_reads_exit_command_and_exits(void)
{
    char path[] = "/tmp/etii_cmd_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);
    ASSERT_EQ_FMT(5, (int)write(fd, "exit\n", 5), "%d");

    pid_t pid = fork();
    if (pid == 0) {
        /* Enfant : stdin <- fichier de commandes, sorties -> /dev/null. */
        lseek(fd, 0, SEEK_SET);
        dup2(fd, 0);
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, 1); dup2(dn, 2); if (dn > 2) close(dn); }
        server = 1;        /* exit_interpreter -> exit(EXIT_SUCCESS) direct */
        console(NULL);     /* lit "exit" puis exit(0) */
        _exit(123);        /* non atteint */
    }

    close(fd);
    unlink(path);
    int status = 0;
    waitpid(pid, &status, 0);
    ASSERT(WIFEXITED(status));
    ASSERT_EQ_FMT(0, WEXITSTATUS(status), "%d");
    PASS();
}

/*
 * Régression du bug « spin console » : en entrée non interactive, sur EOF de
 * stdin, console() doit RENDRE LA MAIN (et non reboucler à l'infini en affichant
 * « commande : » à pleine vitesse). On exécute console() dans un fils avec stdin
 * = /dev/null (EOF immédiat) et un alarm(5) en garde-fou : si la boucle régresse
 * et tourne sans fin, SIGALRM tue le fils → WIFSIGNALED → test rouge. Avec le
 * correctif, console() revient aussitôt et le fils sort proprement (code 0).
 */
TEST console_returns_on_eof_without_spinning(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        /* Enfant : stdin <- /dev/null (EOF immédiat), sorties -> /dev/null. */
        int in = open("/dev/null", O_RDONLY);
        if (in >= 0) { dup2(in, 0); if (in > 2) close(in); }
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, 1); dup2(dn, 2); if (dn > 2) close(dn); }
        alarm(5);          /* garde-fou : tue le fils si console() boucle sans fin */
        console(NULL);     /* doit RETOURNER sur EOF (ni exit, ni boucle infinie)  */
        /* exit() (pas _exit()) : _exit() sauterait le hook de flush de la
           couverture de code (gcov/llvm-cov), qui n'enregistrerait alors jamais
           les lignes du chemin EOF pourtant bien exécutées dans ce fils. */
        exit(0);
    }

    ASSERT(pid > 0);
    int status = 0;
    waitpid(pid, &status, 0);
    /* Sortie normale code 0 = retour propre sur EOF. Un kill par SIGALRM (spin)
       ou tout autre signal fait échouer le test. */
    ASSERT(WIFEXITED(status));
    ASSERT_EQ_FMT(0, WEXITSTATUS(status), "%d");
    PASS();
}

/*
 * Chemin RAW de la console : quand stdin EST un terminal (isatty vrai), getcmdline
 * passe par try_enable_raw_mode() + getcmdline_raw() au lieu du fallback cooked.
 * On fournit un vrai TTY via un pseudo-terminal (posix_openpt -> POSIX, pas de
 * dépendance -lutil). L'enfant lit "exit" en mode raw et, en mode serveur,
 * exit_interpreter termine le process proprement (code 0). alarm() est un
 * garde-fou : si la lecture raw bloquait, SIGALRM tuerait l'enfant -> test rouge.
 */
TEST console_raw_mode_over_pty_reads_command(void)
{
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    ASSERT(master >= 0);
    ASSERT_EQ_FMT(0, grantpt(master), "%d");
    ASSERT_EQ_FMT(0, unlockpt(master), "%d");
    char *slave_name = ptsname(master);
    ASSERT(slave_name != NULL);
    int slave = open(slave_name, O_RDWR | O_NOCTTY);
    ASSERT(slave >= 0);

    pid_t pid = fork();
    if (pid == 0) {
        /* Enfant : stdin = esclave PTY (un vrai tty -> chemin raw), sorties muettes. */
        close(master);
        dup2(slave, 0);
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, 1); dup2(dn, 2); if (dn > 2) close(dn); }
        if (slave > 2) close(slave);
        server = 1;        /* exit_interpreter -> exit(EXIT_SUCCESS) direct */
        alarm(5);          /* garde-fou : tue l'enfant si la lecture raw bloque */
        console(NULL);     /* isatty(0) vrai -> try_enable_raw_mode + getcmdline_raw */
        _exit(123);        /* non atteint (sortie via la commande exit) */
    }

    ASSERT(pid > 0);
    close(slave);
    /* Le parent « tape » une session au terminal maître. La séquence exerce les
       branches d'édition de getcmdline_raw au-delà de la simple frappe :
         "help\n"        commande dispatchée + ajoutée à l'historique
         "\033[A"        flèche ↑  -> rappelle "help" (ESC '[' 'A' + historique)
         "\033[B"        flèche ↓  -> revient au brouillon vide (ESC '[' 'B')
         "zz\177\177"    deux frappes puis deux backspaces (0x7f) -> ligne vidée
         "\014"          Ctrl-L    -> clear_console (no-op : stdout non-tty)
                         + redessin de la ligne de saisie en cours
         "exit\n"        commande finale -> exit(EXIT_SUCCESS) en mode serveur
       L'entrée est entièrement bufferisée par stdio dès la première lecture, donc
       le déroulé est déterministe quel que soit l'ordonnancement. */
    const char seq[] = "help\n\033[A\033[Bzz\177\177\014exit\n";
    ssize_t w = write(master, seq, sizeof(seq) - 1);
    ASSERT_EQ_FMT((int)(sizeof(seq) - 1), (int)w, "%d");

    int status = 0;
    waitpid(pid, &status, 0);
    close(master);

    ASSERT(WIFEXITED(status));               /* sortie propre (pas tué par SIGALRM) */
    ASSERT_EQ_FMT(0, WEXITSTATUS(status), "%d");
    PASS();
}

/*
 * Pager « --Suite-- » sur un VRAI terminal : la pagination ne s'active que si
 * stdin ET stdout sont des TTY et que la hauteur est connue — on branche donc
 * les deux sur l'esclave d'un PTY (12 rangées via TIOCSWINSZ → page de 4
 * lignes une fois la zone Events réservée). La sortie de `help` dépasse
 * largement une page : la première pause « --Suite-- » doit apparaître, le
 * « q » envoyé ensuite déroule le reste sans pause, puis `exit` termine
 * proprement. Si le pager ne s'engage pas, « q » devient une commande inconnue
 * (inoffensive) et le test échoue sur l'absence du marqueur --Suite--.
 */
TEST console_pager_paginates_long_output_over_pty(void)
{
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    ASSERT(master >= 0);
    ASSERT_EQ_FMT(0, grantpt(master), "%d");
    ASSERT_EQ_FMT(0, unlockpt(master), "%d");
    char *slave_name = ptsname(master);
    ASSERT(slave_name != NULL);
    int slave = open(slave_name, O_RDWR | O_NOCTTY);
    ASSERT(slave >= 0);

    /* Petit terminal : 12 rangées -> zone Events active (12 > 8) et page de
       12 - 7 (zone) - 1 (invite --Suite--) = 4 lignes (>= 3, le minimum sous
       lequel la pagination se désactive) -> pagination certaine sur `help`. */
    struct winsize ws = { .ws_row = 12, .ws_col = 80, .ws_xpixel = 0, .ws_ypixel = 0 };
    ioctl(slave, TIOCSWINSZ, &ws);

    pid_t pid = fork();
    if (pid == 0) {
        /* Enfant : stdin ET stdout = esclave PTY (pager actif), stderr muet. */
        close(master);
        dup2(slave, 0);
        dup2(slave, 1);
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, 2); if (dn > 2) close(dn); }
        if (slave > 2) close(slave);
        server = 1;        /* exit_interpreter -> exit(EXIT_SUCCESS) direct */
        alarm(10);         /* garde-fou : pause --Suite-- jamais servie = blocage */
        console(NULL);
        _exit(123);        /* non atteint (sortie via la commande exit) */
    }

    ASSERT(pid > 0);
    close(slave);

    /* "help\n" -> sortie longue -> pause ; "q" -> déroule le reste ; "\n" ->
       commande vide (inoffensive) ; "exit\n" -> sortie propre. */
    const char seq[] = "help\nq\nexit\n";
    ssize_t w = write(master, seq, sizeof(seq) - 1);
    ASSERT_EQ_FMT((int)(sizeof(seq) - 1), (int)w, "%d");

    /* Draine l'affichage jusqu'à EOF/EIO (mort de l'enfant) en gardant les
       64 premiers Ko pour l'assertion. */
    static char out[65536];
    size_t used = 0;
    char drainbuf[4096];
    ssize_t n;
    while ((n = read(master, drainbuf, sizeof drainbuf)) > 0) {
        if (used < sizeof out - 1) {
            size_t keep = (size_t)n;
            if (keep > sizeof out - 1 - used) keep = sizeof out - 1 - used;
            memcpy(out + used, drainbuf, keep);
            used += keep;
        }
    }
    out[used] = '\0';

    int status = 0;
    waitpid(pid, &status, 0);
    close(master);

    ASSERT(WIFEXITED(status));               /* sortie propre (pas tué par SIGALRM) */
    ASSERT_EQ_FMT(0, WEXITSTATUS(status), "%d");
    ASSERT(strstr(out, "--Suite--") != NULL);   /* la pause du pager a eu lieu   */
    ASSERT(strstr(out, "clientsCommand") != NULL); /* fin de l'aide déroulée par q */
    PASS();
}

/*
 * run_console() démarre un thread pthread DÉTACHÉ (pas un fork) : rien à
 * flusher côté couverture de code, pas de fork() à gérer — on redirige juste
 * le stdin DU PROCESS de test courant vers /dev/null pour que le thread
 * détaché reçoive un EOF immédiat et se termine vite, puis on restaure stdin.
 */
TEST run_console_thread_returns_on_eof(void)
{
    fflush(stdout);
    int saved_stdin = dup(0);
    ASSERT(saved_stdin >= 0);
    int devnull = open("/dev/null", O_RDONLY);
    ASSERT(devnull >= 0);
    dup2(devnull, 0);
    close(devnull);

    run_console(0);
    usleep(50000); /* laisse le thread détaché lire l'EOF et se terminer */

    dup2(saved_stdin, 0);
    close(saved_stdin);
    PASS();
}

SUITE(console_suite)
{
    /* console() persiste désormais l'historique dans $HOME/.eternityII_history
       (repli ./ sans HOME). Ces tests exécutent console() en fils ET, pour
       run_console_thread_returns_on_eof, dans un thread du process de test
       courant : on isole donc HOME dans un répertoire temporaire jetable pour
       ne JAMAIS lire/écrire le HOME réel ni le dépôt. */
    char home_tmpl[] = "/tmp/etii_home_XXXXXX";
    char *sandbox_home = mkdtemp(home_tmpl);
    char *saved_home = getenv("HOME");
    char saved_home_copy[4096] = {0};
    if (saved_home != NULL) {
        strncpy(saved_home_copy, saved_home, sizeof saved_home_copy - 1);
    }
    if (sandbox_home != NULL) {
        setenv("HOME", sandbox_home, 1);
    }

    RUN_TEST(console_reads_exit_command_and_exits);
    RUN_TEST(console_returns_on_eof_without_spinning);
    RUN_TEST(console_raw_mode_over_pty_reads_command);
    RUN_TEST(console_pager_paginates_long_output_over_pty);
    RUN_TEST(run_console_thread_returns_on_eof);

    /* Nettoyage : supprime le fichier d'historique éventuellement écrit dans le
       HOME jeté puis le répertoire, et restaure le HOME d'origine. */
    if (sandbox_home != NULL) {
        char hist[4160];
        snprintf(hist, sizeof hist, "%s/.eternityII_history", sandbox_home);
        remove(hist);
        rmdir(sandbox_home);
    }
    if (saved_home != NULL) {
        setenv("HOME", saved_home_copy, 1);
    } else {
        unsetenv("HOME");
    }
}

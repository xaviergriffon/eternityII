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
#include "greatest.h"
#include "ui/console.h"
#include "app/static_variables.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
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
        _exit(0);          /* atteint uniquement si console() est revenue          */
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

SUITE(console_suite)
{
    RUN_TEST(console_reads_exit_command_and_exits);
    RUN_TEST(console_returns_on_eof_without_spinning);
}

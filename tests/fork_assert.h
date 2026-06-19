#ifndef ETII_TEST_FORK_ASSERT_H
#define ETII_TEST_FORK_ASSERT_H

/*
 * Petit harnais pour tester les chemins de code qui appellent exit().
 *
 * greatest exécute tout dans un seul processus : un exit() tuerait le runner
 * entier. On exécute donc le scénario dans un processus fils via fork() et on
 * inspecte son code de sortie côté parent.
 *
 * La fonction testée est une `void(void)` qui construit son propre contexte
 * (éventuellement depuis des globaux de fichier positionnés avant l'appel,
 * fork() copiant la mémoire du parent).
 */
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

/**
 * @brief Exécute `fn` dans un processus fils et renvoie son code de sortie.
 *
 * La sortie standard et d'erreur du fils sont redirigées vers /dev/null pour ne
 * pas polluer la sortie du runner. Si `fn` revient sans appeler exit(), le fils
 * sort avec le code 0.
 *
 * @param fn      Fonction sans argument à exécuter dans le fils (censée exit()).
 * @param out_pid Si non NULL, reçoit le PID du fils (utile pour nettoyer des
 *                fichiers nommés d'après getpid()).
 * @return        WEXITSTATUS du fils, ou -1 s'il a été terminé par un signal.
 */
static inline int run_in_fork(void (*fn)(void), pid_t *out_pid)
{
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, 1);
            dup2(devnull, 2);
            if (devnull > 2) close(devnull);
        }
        fn();
        _exit(0); /* fn était censée exit() ; si elle revient, succès */
    }
    if (out_pid != NULL) {
        *out_pid = pid;
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
}

#endif /* ETII_TEST_FORK_ASSERT_H */

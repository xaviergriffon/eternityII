#include "app/etii_control.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <pthread.h>

#include "app/static_variables.h"
#include "app/etii_client.h"
#include "net/etii_protocol.h"
#include "net/tcpclient.h"
#include "ui/command_lines.h"
#include "ui/logger.h"

/**
 * @brief Longueur maximale (avec le nul terminal) d'une ligne de commande
 *        reçue via `CTRL_COMMAND`, une fois recopiée localement. Le payload
 *        réseau reste borné par `CTRL_PAYLOAD_MAX` (4000) côté codec ; cette
 *        borne-ci est celle du buffer local utilisé comme chaîne C, cohérente
 *        avec l'ordre de grandeur des lignes de commande de la console
 *        (cf. `IPC_LINE_MAX`, src/net/ipc_protocol.h).
 */
#define CONTROL_COMMAND_LINE_MAX 512

/**
 * @brief Paramètres transmis à `run_control_channel` par `start_control_channel`
 *        (allocation malloc, libérée par le thread dès que les champs sont
 *        recopiés localement — même patron que les threads de `app_runtime.c`
 *        qui reçoivent un pointeur de paramètre via `pthread_create`).
 */
typedef struct {
    char server_ip[256];
    int nb_forks;
} control_channel_params_t;

void control_channel_build_stats(control_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    for (int f = 0; f < NB_THREADS; f++) {
        out->shots_per_second += fork_statistics[f].shots_per_second;
        out->possibility_stock += fork_statistics[f].possibilities_in_stock;
        out->analysed_stock += fork_statistics[f].analyses_in_stock;
        out->pruner_checked += fork_statistics[f].pruner_checked;
        out->pruner_removed += fork_statistics[f].pruner_removed;
        out->pruner_cells_per_second += fork_statistics[f].pruner_cells_per_second;
        if (fork_statistics[f].max_result > out->max_result) {
            out->max_result = fork_statistics[f].max_result;
        }
    }
    if (max_result > out->max_result) {
        out->max_result = max_result;
    }
}

int control_channel_handle_frame(int socket_id, uint8_t cmd, const void *payload, int32_t len)
{
    switch (cmd) {
    case CTRL_PING:
        return ctrl_send_frame(socket_id, CTRL_ACK, NULL, 0);

    case CTRL_GET_STATS: {
        control_stats_t stats;
        control_channel_build_stats(&stats);
        uint8_t buf[CONTROL_STATS_WIRE_SIZE];
        int32_t wlen = control_stats_encode(&stats, buf);
        return ctrl_send_frame(socket_id, CTRL_STATS, buf, wlen);
    }

    case CTRL_COMMAND: {
        // Copie locale null-terminée bornée : le payload réseau n'est pas
        // garanti terminé par un nul, et do_command_line/control_command_allowed
        // attendent une chaîne C classique.
        char command_line[CONTROL_COMMAND_LINE_MAX];
        int32_t copy_len = len;
        if (copy_len < 0) {
            copy_len = 0;
        }
        if (copy_len > (int32_t)sizeof(command_line) - 1) {
            copy_len = (int32_t)sizeof(command_line) - 1;
        }
        if (copy_len > 0 && payload != NULL) {
            memcpy(command_line, payload, (size_t)copy_len);
        }
        command_line[copy_len] = '\0';

        int32_t result;
        // Défense en profondeur : le serveur (PR3) filtre déjà côté
        // clientsCmd, mais ce client ne fait JAMAIS confiance aveuglément à
        // ce qui arrive sur ce socket. control_command_allowed ne regarde que
        // le premier mot de la ligne, pas la peine de le découper ici.
        if (!control_command_allowed(command_line)) {
            log_error("canal de contrôle : commande refusée par la liste blanche : \"%s\"\n",
                      command_line);
            result = -1;
        } else {
            result = (int32_t)do_command_line(command_line);
        }

        uint8_t rbuf[sizeof(int32_t)];
        memcpy(rbuf, &result, sizeof(result));
        return ctrl_send_frame(socket_id, CTRL_RESULT, rbuf, (int32_t)sizeof(rbuf));
    }

    default:
        // Trame inattendue mais non dangereuse : on journalise et on
        // continue la session plutôt que de la fermer.
        log_error("canal de contrôle : commande de trame inconnue (cmd=%u)\n",
                  (unsigned)cmd);
        return 0;
    }
}

/**
 * @brief Pause interruptible par `REQUEST_STOP`, découpée en tranches de
 *        `THREAD_MICRO_SLEEP` (même patron que la boucle de back-off de
 *        `run_client`, src/app/etii_client.c) : un `usleep` monolithique
 *        retarderait l'arrêt du process jusqu'à `NO_WORK_SLEEP_MAX`.
 */
static void control_channel_backoff_sleep(useconds_t total)
{
    useconds_t remaining = total;
    while (remaining > 0 && request_keeps_running(request)) {
        useconds_t step = remaining < THREAD_MICRO_SLEEP ? remaining : THREAD_MICRO_SLEEP;
        usleep(step);
        remaining -= step;
    }
}

void *run_control_channel(void *param)
{
    control_channel_params_t *params = (control_channel_params_t *)param;
    char server_ip[256];
    strncpy(server_ip, params->server_ip, sizeof(server_ip) - 1);
    server_ip[sizeof(server_ip) - 1] = '\0';
    int nb_forks = params->nb_forks;
    free(params);

    useconds_t backoff = 0;

    while (request_keeps_running(request)) {
        int socket_id = create_tcp_client(server_ip, SERVER_PORT);
        if (socket_id == -1) {
            backoff = next_no_work_sleep(backoff);
            control_channel_backoff_sleep(backoff);
            continue;
        }

        // Handshake de version : patron EXACT de check_and_connect_to_server
        // (src/core/datamanager.c) — toute connexion TCP du protocole doit
        // négocier la version avant d'échanger quoi que ce soit d'autre.
        send_instruction(socket_id, INST_CHECK_VERSION);
        send(socket_id, &version, sizeof(int), 0);
        int8_t handshake_result = recv_instruction(socket_id);
        handshake_verdict_t verdict = handshake_verdict(handshake_result);
        if (verdict == HANDSHAKE_VERSION_REJECTED) {
            // Refus explicite : incompatibilité réelle. Ce n'est PAS à ce
            // thread annexe de poser REQUEST_STOP (il ne pilote pas le sort
            // du process principal) — on journalise clairement et on sort.
            log_error("canal de contrôle : version %i refusée par le serveur — canal arrêté\n",
                      version);
            close_socket(socket_id);
            return NULL;
        }
        if (verdict == HANDSHAKE_RETRY) {
            log_info("canal de contrôle : handshake sans réponse (serveur occupé ?) — "
                      "nouvelle tentative ultérieure\n");
            close_socket(socket_id);
            backoff = next_no_work_sleep(backoff);
            control_channel_backoff_sleep(backoff);
            continue;
        }
        // HANDSHAKE_OK : connexion utilisable, back-off réinitialisé.
        backoff = 0;

        control_hello_t hello;
        hello.pid = (int32_t)getpid();
        hello.nb_forks = (int32_t)nb_forks;
#ifdef WITH_CUDA
        hello.mode = (uint8_t)(gpu_pruner_mode ? 2 : (pruner_mode ? 1 : 0));
#else
        hello.mode = (uint8_t)(pruner_mode ? 1 : 0);
#endif
        uint8_t hello_buf[CONTROL_HELLO_WIRE_SIZE];
        int32_t hello_len = control_hello_encode(&hello, hello_buf);

        int hello_ok = 1;
        if (send_instruction(socket_id, INST_CONTROL_HELLO) <= 0) {
            hello_ok = 0;
        }
        if (hello_ok && send_all(socket_id, &hello_len, sizeof(hello_len)) != (long)sizeof(hello_len)) {
            hello_ok = 0;
        }
        if (hello_ok && send_all(socket_id, hello_buf, (size_t)hello_len) != (long)hello_len) {
            hello_ok = 0;
        }
        if (!hello_ok) {
            log_error("canal de contrôle : échec de l'envoi du hello — reconnexion\n");
            close_socket(socket_id);
            backoff = next_no_work_sleep(backoff);
            control_channel_backoff_sleep(backoff);
            continue;
        }

        // Timeout de lecture pour la boucle de service : sur ce canal le
        // SERVEUR devient l'initiateur, donc un ctrl_recv_frame bloquant sans
        // borne empêcherait toute reconnexion propre à REQUEST_STOP.
        struct timeval tv;
        tv.tv_sec = tcp_timeout;
        tv.tv_usec = 0;
        setsockopt(socket_id, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        int service_ok = 1;
        while (service_ok && request_keeps_running(request)) {
            void *payload = NULL;
            int32_t len = 0;
            int cmd = ctrl_recv_frame(socket_id, &payload, &len);
            if (cmd < 0) {
                // Timeout (ETIMEDOUT/EAGAIN) ou vraie erreur réseau : on ne
                // les distingue pas, la politique la plus sûre dans les deux
                // cas est de reconnecter.
                service_ok = 0;
                break;
            }
            if (control_channel_handle_frame(socket_id, (uint8_t)cmd, payload, len) != 0) {
                free(payload);
                service_ok = 0;
                break;
            }
            free(payload);
        }

        close_socket(socket_id);
        if (request_keeps_running(request)) {
            backoff = next_no_work_sleep(backoff);
            control_channel_backoff_sleep(backoff);
        }
    }

    return NULL;
}

void start_control_channel(const char *server_ip, int nb_forks)
{
    control_channel_params_t *params = malloc(sizeof(*params));
    if (params == NULL) {
        log_error("start_control_channel : allocation échouée — canal de contrôle désactivé\n");
        return;
    }
    memset(params, 0, sizeof(*params));
    if (server_ip != NULL) {
        strncpy(params->server_ip, server_ip, sizeof(params->server_ip) - 1);
    }
    params->nb_forks = nb_forks;

    pthread_attr_t *thread_attributes = malloc(sizeof *thread_attributes);
    pthread_attr_init(thread_attributes);
    pthread_attr_setdetachstate(thread_attributes, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    if (0 != pthread_create(&thread, thread_attributes, run_control_channel, params)) {
        // Non fatal, comme run_server_thread/run_checker/run_console : le
        // process continue en mode dégradé (sans canal de contrôle) plutôt
        // que de planter et d'orphaniser les process enfants.
        log_error("start_control_channel : pthread_create a échoué — canal de contrôle désactivé\n");
        pthread_attr_destroy(thread_attributes);
        free(thread_attributes);
        free(params);
        return;
    }
    pthread_attr_destroy(thread_attributes);
    free(thread_attributes);
}

#include "app/etii_server.h"
#define closesocket(s) close(s)

#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#include "ui/logger.h"
#include "app/static_variables.h"
#include "net/etii_protocol.h"
#include "net/control_protocol.h"
#include "app/control_registry.h"
#include "app/known_clients_registry.h"
#include "core/datamanager.h"
#include "core/possibility.h"
#include "core/best_board.h"
#include "core/part.h"
#include "core/readdata.h"
#include "net/tcpserver.h"
#include "net/http_server.h"

client_t *thread_params = NULL;

struct array_part *g_server_rotate_parts = NULL;

// Nombre de modification des files par client
unsigned long long *fileUpdates = NULL;

int32_t clamp_pruner_batch(int32_t requested) {
    if (requested < 1) return 1;
    if (requested > PRUNER_BATCH_MAX) return PRUNER_BATCH_MAX;
    return requested;
}

int32_t compute_server_hunger(unsigned long long stock, int active_clients) {
    if (active_clients < 1) {
        return 0;
    }
    unsigned long long target = (unsigned long long)active_clients * SERVER_HUNGER_PER_CLIENT;
    if (stock >= target) {
        return 0;
    }
    unsigned long long missing = target - stock;
    if (missing > SERVER_HUNGER_CAP) {
        return SERVER_HUNGER_CAP;
    }
    return (int32_t)missing;
}

int find_free_thread_slot(client_t *threads, int nb) {
    for (int t = 0; t < nb; t++) {
        if (threads[t].exist != 0 && threads[t].socket_id == -1) return t;
    }
    return -1;
}

int find_empty_thread_slot(client_t *threads, int nb) {
    for (int i = 0; i < nb; i++) {
        if (threads[i].exist == 0) return i;
    }
    return -1;
}

/**
 * @brief Compte le nombre de threads serveur actuellement connectés à un client.
 *
 * Un thread est considéré actif si son `socket_id` est différent de -1.
 *
 * @param thread_params Tableau des contextes de threads serveur.
 * @return              Nombre de threads actifs.
 */
int get_active_threads(client_t *thread_params) {
    int activeThread = 0;
    if (thread_params != NULL) {
        for (int t = 0; t < NB_THREADS; t++) {
            if (thread_params[t].socket_id != -1) {
                activeThread++;
            }
        }
    }
    
    return activeThread;
}

/**
 * @brief Voir la doc dans etii_server.h.
 */
int server_active_client_count(void) {
    return get_active_threads(thread_params);
}

/**
 * @brief Construit le tableau « File queues » du rapport serveur (une ligne par
 *        file : unchecked / checked / analysed, plus la ligne Total) dans une
 *        chaîne fraîchement allouée. Renvoie les totaux par pool via les
 *        out-params (NULL accepté).
 *
 * Extrait du corps de boucle de check_server pour être testable hors thread :
 * pure (lit l'instantané des tailles de files), sans I/O ni sleep.
 *
 * @return Chaîne allouée (à libérer par l'appelant).
 */
char *build_file_queues_table(unsigned long long *out_unchecked,
                              unsigned long long *out_checked,
                              unsigned long long *out_analysed)
{
    unsigned long long unchecked = 0, checked = 0, analysed = 0;
    size_t size = 256 + (size_t)NB_FILE_POSSIBILITY * 64;
    char *table = calloc(size, sizeof(char));
    int off = snprintf(table, size,
        "File queues\n"
        "File |    Unchecked |      Checked |     Analysed\n"
        "-----+--------------+--------------+-------------\n");
    for (int f = 0; f < NB_FILE_POSSIBILITY; f++) {
        unsigned long long u = file_size(f);
        unsigned long long c = file_checked_size(f);
        unsigned long long a = file_analysed_size(f);
        off += snprintf(table + off, size - off,
                        "%4i | %12llu | %12llu | %12llu\n", f, u, c, a);
        unchecked += u; checked += c; analysed += a;
    }
    snprintf(table + off, size - off,
             "-----+--------------+--------------+-------------\n"
             "Total| %12llu | %12llu | %12llu\n", unchecked, checked, analysed);
    if (out_unchecked) *out_unchecked = unchecked;
    if (out_checked)   *out_checked   = checked;
    if (out_analysed)  *out_analysed  = analysed;
    return table;
}

/**
 * @brief Cadence de la sauvegarde automatique (logique extraite de check_server).
 *
 * Voir etii_server.h pour le contrat. Le seuil est de 6 tours.
 */
int should_autobackup(int *lastBack, unsigned long long *lastBackupUpdates,
                      unsigned long long currentUpdates)
{
    if (*lastBack >= 6 && *lastBackupUpdates != currentUpdates)
    {
        *lastBackupUpdates = currentUpdates;
        *lastBack = 0;
        return 1;
    }
    else if (*lastBack < 6)
    {
        (*lastBack)++;
    }
    return 0;
}

/**
 * @brief Thread de statistiques du serveur (lancé par `run_checker`).
 *
 * Toutes les 10 secondes, collecte dans un buffer local le stock de chaque
 * file, les possibilités en cours d'analyse, le débit global et le meilleur
 * résultat, puis le publie dans `lastcheck` via `lastcheck_publish()`.
 * Déclenche automatiquement une sauvegarde (`temp.back`) toutes les minutes
 * si le stock a évolué depuis le dernier backup.
 *
 * @param param Non utilisé.
 * @return      NULL (boucle infinie).
 */
/**
 * @brief Un tour de la boucle de `check_server` (corps extrait pour être testable
 *        hors thread, comme `communicate_with_client_step`).
 *
 * Construit et publie le rapport de statistiques serveur, met à jour le
 * bandeau `log_status`, détecte un nouveau record et déclenche l'autobackup
 * périodique. Ne contient PAS le `sleep(sleep_time)` de fin de tour : c'est
 * l'appelant (la boucle `while(1)` de `check_server`) qui rythme les tours.
 *
 * @param lastactive                In/out : compteur cumulé de coups joués (fenêtre glissante).
 * @param lastClientsFileUpdateBackup In/out : total des mises à jour de files au dernier backup.
 * @param lastBack                  In/out : nombre de tours écoulés depuis le dernier backup.
 * @param last_record                In/out : meilleur résultat déjà annoncé (détection de record).
 * @param sleep_time                 Durée nominale du tour (secondes), utilisée pour le débit rapporté.
 */
void check_server_step(unsigned long long *lastactive, unsigned long long *lastClientsFileUpdateBackup,
                       int *lastBack, int *last_record, int sleep_time)
{
    // Rapport construit dans un buffer LOCAL (jamais dans `lastcheck`
    // directement) : les strcat/sprintf qui suivent ne touchent aucun état
    // partagé, donc aucun besoin de tenir un verrou pendant leur exécution.
    // `lastcheck_publish()` n'est appelée qu'une fois le rapport complet,
    // ce qui réduit la section critique au seul échange de pointeur (voir
    // static_variables.h pour le détail de la race corrigée).
    char *report = calloc(4000, sizeof(char));
    unsigned long long currentactive = *lastactive;
    unsigned long long clientsFileUpdates = 0;
    int c;
    *lastactive = 0;
    for(c=0; c < NB_THREADS;c++)
    {
        *lastactive = *lastactive + counters[c];
        if (fileUpdates != NULL) {
            clientsFileUpdates = clientsFileUpdates + fileUpdates[c];
        }
    }
    currentactive = *lastactive - currentactive;
    non_null_possibilities = *lastactive;

    // Pool des possibilités vérifiées par les pruners : 10 files, comme le
    // pool standard (trylock pour servir plusieurs requêtes en parallèle)
    unsigned long long file_possibility_stock = 0;
    unsigned long long file_possibility_checked_stock = 0;
    unsigned long long file_possibility_analysed_stock = 0;
    char *table = build_file_queues_table(&file_possibility_stock,
                                          &file_possibility_checked_stock,
                                          &file_possibility_analysed_stock);
    strcat(report, table);
    free(table);

    unsigned long long bys = currentactive / sleep_time;

    // Indice cumulé « études/s » : somme de deux flux DISJOINTS — les requêtes
    // servies (`bys`) et les études de prunage case par case (élagage rmnonext,
    // delta de pruner_cells_studied depuis le tour précédent). « dont
    // prunage/s » isole la part de l'élagage.
    static unsigned long long last_prune_cells = 0;
    unsigned long long prune_cells_now = pruner_cells_studied;
    unsigned long long prune_bys = (prune_cells_now - last_prune_cells) / sleep_time;
    last_prune_cells = prune_cells_now;

    // Publié pour l'API REST (GET /api/v1/stats) : même indicateur « coups/s »
    // que le bandeau log_status ci-dessous (bys seul, pas bys+prune_bys), lu
    // sans verrou (cf. static_variables.h).
    server_shots_per_second = bys;

    int activeThread = get_active_threads(thread_params);

    char *temp = calloc(1000, sizeof(char));
    sprintf(temp, "active thread last %isec :%lli\nactive thread/s :%lli\nétudes/s (recherche+prunage) :%llu\ndont prunage/s :%llu\npossibility in stock :%lli (checked:%llu) (analysed:%llu)\ngetted possibility not null :%lli\nmax result on server :%i\nactive Thread :%i\n",sleep_time,currentactive, bys,(unsigned long long)bys + prune_bys,prune_bys,file_possibility_stock,file_possibility_checked_stock,file_possibility_analysed_stock,non_null_possibilities, max_result, activeThread);
    strcat(report, temp);
    free(temp);

    lastcheck_publish(report);

    /* Bandeau de stats « live » : résumé compact poussé à chaque tour.
       En mode ncurses il s'affiche en continu ; en mode ANSI, no-op. */
    log_status(" coups/s:%llu  stock:%llu  checked:%llu  analyse:%llu  record:%i/%i  threads:%i ",
               bys, file_possibility_stock, file_possibility_checked_stock,
               file_possibility_analysed_stock, max_result, ETERN_PARTS, activeThread);

    if (max_result > *last_record) {
        *last_record = max_result;
        log_event("new record: %i pieces placed", max_result);
    }

    if (should_autobackup(lastBack, lastClientsFileUpdateBackup, clientsFileUpdates))
    {
        int rb = backup("./temp.back");
        if (rb == BACKUP_SKIPPED_MAINTENANCE) {
            log_error("autobackup : sauté (maintenance en cours) sur ./temp.back\n");
        } else if (rb != BACKUP_OK) {
            log_error("autobackup : échec sur ./temp.back\n");
        }
        int rba = backup_analysed("./temp_analysed.back");
        if (rba == BACKUP_SKIPPED_MAINTENANCE) {
            log_error("autobackup : sauté (maintenance en cours) sur ./temp_analysed.back\n");
        } else if (rba != BACKUP_OK) {
            log_error("autobackup : échec sur ./temp_analysed.back\n");
        }
        // Représentation du meilleur plateau connu (pas seulement max_result) :
        // même cadence que le reste du stock, fichier dédié (cf. core/best_board.h).
        if (best_board_save(&g_server_best_board, "./temp-best_board.back") != 0) {
            log_error("autobackup : échec sur ./temp-best_board.back\n");
        }
    }
}

void *check_server(void *param)
{
    (void)param;
    unsigned long long lastactive = 0;
    unsigned long long lastClientsFileUpdateBackup = 0;
    int sleep_time = 10;
    int lastBack = 0;
    int last_record = max_result;
    // Comme les autres threads (rmnonext, server_tcp, …) : la boucle s'arrête
    // sur REQUEST_STOP — en production celui-ci n'arrive qu'à l'arrêt du
    // processus, le comportement est donc inchangé (et le thread testable).
    while(request != REQUEST_STOP)
    {
        check_server_step(&lastactive, &lastClientsFileUpdateBackup, &lastBack, &last_record, sleep_time);
        sleep(sleep_time);
    }

    return NULL;
}

/**
 * @brief Renvoie au stock local les possibilités servies mais non acquittées.
 *
 * Extrait du bloc de fin de `communicate_with_client` (voir etii_server.h).
 */
void requeue_last_sent_possibility(array_possibility_packet *lastSent)
{
    if (lastSent == NULL)
    {
        return;
    }
    // On ne réinjecte QUE les possibilités encore présentes dans file_analysed :
    // si le client l'avait acquittée (INST_POSSIBILITY_ANALYSED),
    // remove_possibility_analysed renvoie ≠ 0 (déjà retirée) et on ne la remet
    // pas — pas de doublon de travail déjà terminé.
    for (int rp = 0; rp < lastSent->size; rp++)
    {
        struct possibility_packet *possibility = &lastSent->possibilities[rp];
        if (remove_possibility_analysed(possibility, -1) != 0)
        {
            // Déjà acquittée par le client : rien à rendre.
            continue;
        }
        array_possibility_packet *single = build_single_array_possibility_packet(possibility);
        if (add_possibility(NULL, single))
        {
            log_error("Error with possibility : \n");
            print_possibility_packet(possibility);
            save_possibility("./error_possibility", possibility);
        }
        free_array_possibility_packet(single);
    }
}

/**
 * @brief Traite une instruction reçue d'un client (un tour de la boucle de
 *        `communicate_with_client`).
 *
 * Extrait du corps du `while` pour être testable hors thread, via un socketpair
 * jouant le rôle du socket client. `*lastSent` mémorise le dernier lot servi (à
 * rendre au stock à la déconnexion) ; `*version_supported` porte l'état du
 * handshake de version d'un tour à l'autre. Comportement strictement identique.
 *
 * @param client            Contexte du thread (socket_id, compteur, rotate_parts).
 * @param instruction        Instruction reçue à traiter.
 * @param lastSent           In/out : dernier lot envoyé (libéré/réaffecté ici).
 * @param version_supported  In/out : 1 si le handshake de version a réussi.
 * @return 1 pour poursuivre la boucle, 0 pour s'arrêter (anciens `break`).
 */
int communicate_with_client_step(client_t *client, int8_t instruction,
                                 array_possibility_packet **lastSent,
                                 int *version_supported,
                                 int *out_control_session_index)
{
        if (out_control_session_index != NULL) {
            *out_control_session_index = -1;
        }
        if (instruction == INST_CHECK_VERSION) {
            int client_version = -1;
            long ssize = recv_all(client->socket_id, &client_version, sizeof(int));
            if (ssize != (long)sizeof(int)) {
                log_error("error on recept client version\n");
                return 0;
            }
            if (version == client_version) {
                send_instruction(client->socket_id, INST_SUPPORTED_VERSION);
                *version_supported = 1;
            } else {
                log_error("Version of client unsupported\n");
                log_event("client rejeté : version %i incompatible (serveur %i)", client_version, version);
                send_instruction(client->socket_id, INST_UNSUPPORTED_VERSION);
            }
        } else if(instruction == INST_GET && *version_supported == 1)
        {
            if(*lastSent != NULL)
            {
                free_array_possibility_packet(*lastSent);
                *lastSent = NULL;
            }

            *lastSent = get_last_possibility(NULL, 1);
            int32_t k = (int32_t)(*lastSent)->size;
            for (int p = 0; p < k; p++)
            {
                add_possibility_analysed(&(*lastSent)->possibilities[p], -1);
                counters[client->compteur]++;
                fileUpdates[client->compteur]++;
            }
            // Réponse cadrée (VERSION 7) : compte K puis, si K > 0, le bloc
            // contigu des K paquets. send_all réassemble les envois partiels —
            // l'ancien send() brut pouvait tronquer un paquet et désynchroniser
            // tout le flux de la connexion.
            if (send_all(client->socket_id, &k, sizeof(k)) != (long)sizeof(k))
            {
                log_errno("Erreur d'envoi (compte GET) => ");
            }
            else if (k > 0
                     && send_all(client->socket_id, (*lastSent)->possibilities,
                                 (size_t)k * sizeof(struct possibility_packet)) < 0)
            {
                log_errno("Erreur d'envoi (bloc GET) => ");
            }

        } else if(instruction == INST_GET_TO_CHECK && *version_supported == 1)
        {
            // Client pruner : sert le pool non vérifié uniquement (pas de repli)
            if(*lastSent != NULL)
            {
                free_array_possibility_packet(*lastSent);
                *lastSent = NULL;
            }

            *lastSent = get_last_possibility_tocheck(1);
            int32_t k = (int32_t)(*lastSent)->size;
            for (int p = 0; p < k; p++)
            {
                add_possibility_analysed(&(*lastSent)->possibilities[p], -1);
                counters[client->compteur]++;
                fileUpdates[client->compteur]++;
            }
            // Réponse cadrée (VERSION 7) — même trame que INST_GET.
            if (send_all(client->socket_id, &k, sizeof(k)) != (long)sizeof(k))
            {
                log_errno("Erreur d'envoi (compte GET tocheck) => ");
            }
            else if (k > 0
                     && send_all(client->socket_id, (*lastSent)->possibilities,
                                 (size_t)k * sizeof(struct possibility_packet)) < 0)
            {
                log_errno("Erreur d'envoi (bloc GET tocheck) => ");
            }

        } else if(instruction == INST_GET_TO_CHECK_BATCH && *version_supported == 1)
        {
            // Client pruner (échange par lot) : un seul aller-retour pour N
            // possibilités non vérifiées. N est borné par le client (sa mémoire)
            // et re-borné ici (équité entre pruners, mémoire serveur).
            int32_t requested = 0;
            if (recv_all(client->socket_id, &requested, sizeof(requested)) != (long)sizeof(requested))
            {
                log_error("batch tocheck : nombre demandé non reçu\n");
                return 0;
            }
            requested = clamp_pruner_batch(requested);

            if(*lastSent != NULL)
            {
                free_array_possibility_packet(*lastSent);
                *lastSent = NULL;
            }
            *lastSent = get_last_possibility_tocheck(requested);
            int32_t k = (int32_t)(*lastSent)->size;
            for (int p = 0; p < k; p++)
            {
                add_possibility_analysed(&(*lastSent)->possibilities[p], -1);
                counters[client->compteur]++;
                fileUpdates[client->compteur]++;
            }
            // Envoi : compte K puis, si K > 0, le bloc contigu des K paquets.
            if (send_all(client->socket_id, &k, sizeof(k)) != (long)sizeof(k))
            {
                log_errno("Erreur d'envoi (compte lot) => ");
            }
            else if (k > 0)
            {
                if (send_all(client->socket_id, (*lastSent)->possibilities,
                             (size_t)k * sizeof(struct possibility_packet)) < 0)
                {
                    log_errno("Erreur d'envoi (bloc lot) => ");
                }
            }

        } else if(instruction == INST_ADD && *version_supported == 1)
        {
            array_possibility_packet *aposs = malloc(sizeof(array_possibility_packet));
            struct possibility_packet *possibilityPacket = malloc(sizeof(struct possibility_packet));
            // recv_all réassemble les lectures partielles ; un résultat court ne
            // peut venir que d'un EOF/erreur socket → flux irrécupérable, on clôt
            // la session (l'ancien recv() brut laissait la fin du paquet dans le
            // flux, relue ensuite comme des instructions).
            long receive = recv_all(client->socket_id, possibilityPacket, sizeof(struct possibility_packet));
            if((long)sizeof(struct possibility_packet) == receive)
            {
                aposs->possibilities = possibilityPacket;
                aposs->size = 1;

                if(add_possibility(NULL, aposs) == 0)
                {
                    send_instruction(client->socket_id, INST_CONSIDERED);
                    fileUpdates[client->compteur]++;

                } else{
                    send_instruction(client->socket_id, INST_ERROR);
                }
            } else{
                log_error("bad possibility recept");
                if (receive < 0) {
                    log_errno(" => ");
                }
                log_error("\n");
                free(possibilityPacket);
                free(aposs);
                return 0;
            }
            free(possibilityPacket);
            free(aposs);


        } else if (instruction == INST_POSSIBILITY_ANALYSED && *version_supported == 1) {
            struct possibility_packet *possibilityPacket = malloc(sizeof(struct possibility_packet));
            // recv_all : même durcissement que INST_ADD — un paquet incomplet
            // signifie un flux mort, on clôt la session au lieu de continuer
            // sur un flux désynchronisé.
            long ssize = recv_all(client->socket_id, possibilityPacket, sizeof(struct possibility_packet));
            if ((long)sizeof(struct possibility_packet) == ssize) {
                if(remove_possibility_analysed(possibilityPacket, -1) == 0)
                {
                    send_instruction(client->socket_id,INST_CONSIDERED);

                } else{
                    log_error("possibility analysed not removed\n");
                    print_possibility_packet(possibilityPacket);
                    send_instruction(client->socket_id,INST_ERROR);
                }
            } else {
                log_error("bad possibility recept");
                if (ssize < 0) {
                    log_errno(" => ");
                }
                log_error("\n");
                free(possibilityPacket);
                return 0;
            }
            free (possibilityPacket);
        } else if (instruction == INST_POSSIBILITY_ANALYSED_BATCH && *version_supported == 1) {
            // Acquittement par lot : M paquets en un aller-retour, un seul INST_CONSIDERED.
            int32_t m = 0;
            if (recv_all(client->socket_id, &m, sizeof(m)) != (long)sizeof(m)) {
                log_error("batch analysed : nombre non reçu\n");
                return 0;
            }
            if (m < 0 || m > PRUNER_BATCH_MAX) {
                log_error("batch analysed : compte hors borne (%d)\n", m);
                return 0;
            }
            int transfer_ok = 1;
            struct possibility_packet pkt;
            for (int p = 0; p < m; p++) {
                if (recv_all(client->socket_id, &pkt, sizeof(pkt)) != (long)sizeof(pkt)) {
                    log_error("batch analysed : paquet %d incomplet\n", p);
                    transfer_ok = 0;
                    break;
                }
                if (remove_possibility_analysed(&pkt, -1) != 0) {
                    // Non bloquant : l'entrée « en analyse » a pu déjà être retirée.
                    log_error("batch analysed : possibilité non retirée (%d)\n", p);
                }
            }
            if (transfer_ok) {
                send_instruction(client->socket_id, INST_CONSIDERED);
            } else {
                send_instruction(client->socket_id, INST_ERROR);
                return 0;
            }
        } else if (instruction == INST_SOLUTION && *version_supported == 1) {
            // Un client a trouvé une solution complète : on la reçoit, on
            // l'affiche de façon visible (événement + journal), on la sauvegarde
            // côté serveur et on acquitte. Avec --stop-on-solution, on s'arrête
            // ensuite en préservant le stock ; sinon on reste en service (les
            // clients continuent d'explorer, d'autres solutions sont possibles).
            struct possibility_packet *sol = malloc(sizeof(struct possibility_packet));
            if (recv_all(client->socket_id, sol, sizeof(struct possibility_packet))
                    == (long)sizeof(struct possibility_packet)) {
                // Nom unique : <pid>_<seq> → plusieurs solutions ne s'écrasent pas.
                static unsigned solution_seq = 0;
                unsigned seq = __atomic_fetch_add(&solution_seq, 1, __ATOMIC_RELAXED);
                char fileName[64];
                snprintf(fileName, sizeof fileName, "./solution_server_%i_%u.csv", (int)getpid(), seq);
                log_event("SOLUTION reçue d'un client (%i pièces placées)", sol->alloc);
                log_info("*** SOLUTION reçue d'un client (%i pièces) ***\n", sol->alloc);
                save_solution_csv(fileName, sol, client->rotate_parts);
                log_info("solution sauvegardée dans %s\n", fileName);
                send_instruction(client->socket_id, INST_CONSIDERED);
                free(sol);

                if (stop_on_solution) {
                    // Garde un seul gagnant si deux clients signalent une
                    // solution quasi simultanément.
                    static volatile int solution_shutdown = 0;
                    if (__atomic_test_and_set(&solution_shutdown, __ATOMIC_SEQ_CST) == 0) {
                        request = REQUEST_STOP;
                        // Sauvegarde du stock sous les noms par défaut de `restore`
                        // (./eternityII.back, ./eternityII-in_analyse.back) : aucun
                        // travail perdu, le serveur peut reprendre au redémarrage.
                        // Codes de retour non ignorés : un arrêt sur solution qui
                        // croirait à tort avoir sauvegardé le stock serait un piège
                        // classique de reprise sur crash.
                        int rb = backup("./eternityII.back");
                        if (rb == BACKUP_SKIPPED_MAINTENANCE) {
                            log_error("arrêt sur solution : backup sauté (maintenance en cours) sur ./eternityII.back\n");
                        } else if (rb != BACKUP_OK) {
                            log_error("arrêt sur solution : échec du backup sur ./eternityII.back\n");
                        }
                        int rba = backup_analysed("./eternityII-in_analyse.back");
                        if (rba == BACKUP_SKIPPED_MAINTENANCE) {
                            log_error("arrêt sur solution : backup sauté (maintenance en cours) sur ./eternityII-in_analyse.back\n");
                        } else if (rba != BACKUP_OK) {
                            log_error("arrêt sur solution : échec du backup sur ./eternityII-in_analyse.back\n");
                        }
                        if (best_board_save(&g_server_best_board, "./eternityII-best_board.back") != 0) {
                            log_error("arrêt sur solution : échec du backup sur ./eternityII-best_board.back\n");
                        }
                        log_event("serveur arrêté suite à la solution (stock sauvegardé)");
                        log_info("serveur arrêté suite à la solution — stock sauvegardé\n");
                        flush_info();
                        exit(EXIT_SUCCESS);
                    }
                    return 0;
                }
                // Sinon : on continue à servir ce client.
            } else {
                // recv_all réassemble les lectures partielles ; un résultat court ne
                // peut venir que d'un EOF/erreur socket → flux irrécupérable, on clôt
                // la session (l'ancien recv() brut laissait la fin du paquet dans le
                // flux, relue ensuite comme des instructions).
                log_error("réception de la solution incomplète\n");
                free(sol);
                return 0;
            }
        } else if (instruction == INST_NEED_WORK && *version_supported == 1) {
            // Sonde de faim (v8) : le client demande combien de possibilités le
            // serveur souhaiterait recevoir pour nourrir les autres sessions.
            // Sert aussi de keepalive (échange = preuve d'activité).
            int32_t hunger = compute_server_hunger(datas_size(),
                                                   get_active_threads(thread_params));
            if (send_all(client->socket_id, &hunger, sizeof(hunger)) != (long)sizeof(hunger))
            {
                log_errno("Erreur d'envoi (need work) => ");
            }
        } else if (instruction == INST_CONTROL_HELLO && *version_supported == 1) {
            // Annonce d'un canal de contrôle (v9) : le client (processus
            // parent) envoie un int32 de longueur puis le payload hello
            // cadré (control_hello_encode). On enregistre la session dans
            // control_registry et on signale la bascule à l'appelant via
            // out_control_session_index (cf. contrat dans etii_server.h) :
            // c'est run_control_session, pas cette boucle, qui gèrera la
            // suite (et l'épilogue socket) de cette connexion.
            int32_t len = 0;
            if (recv_all(client->socket_id, &len, sizeof(len)) != (long)sizeof(len)) {
                log_error("control hello : longueur non reçue\n");
                return 0;
            }
            if (len < 0 || len > CTRL_PAYLOAD_MAX) {
                log_error("control hello : longueur hors borne (%d)\n", len);
                return 0;
            }
            uint8_t buf[CTRL_PAYLOAD_MAX];
            if (len > 0 && recv_all(client->socket_id, buf, (size_t)len) != (long)len) {
                log_error("control hello : payload incomplet\n");
                return 0;
            }
            control_hello_t hello;
            if (control_hello_decode(buf, len, &hello) != 0) {
                log_error("control hello : décodage échoué\n");
                return 0;
            }
            int idx = control_registry_register(client->socket_id, client->peer_ip, &hello);
            if (idx < 0) {
                log_error("control hello : registre de sessions de contrôle plein, session refusée\n");
                return 0;
            }
            // Registre de clients CONNUS (PR4, cumul par machine_uid) : distinct de
            // control_registry ci-dessus, jamais vidé à la déconnexion — cf.
            // known_clients_registry.h. Purement observationnel, ne peut jamais faire
            // échouer cette session.
            known_clients_registry_on_connect(&hello.identity, client->peer_ip);
            log_event("session de contrôle enregistrée (pid=%d, forks=%d, mode=%u, label=\"%s\") -> slot %d",
                      hello.pid, hello.nb_forks, (unsigned)hello.identity.mode, hello.identity.label, idx);
            if (out_control_session_index != NULL) {
                *out_control_session_index = idx;
            }
        } else if (instruction == INST_CLIENT_HELLO && *version_supported == 1) {
            // Annonce d'identité sur la connexion de TRAVAIL (v12, cf.
            // net/etii_protocol.h). Best-effort et déclaratif : une longueur
            // hors borne désynchroniserait le flux (fermeture nécessaire,
            // comme pour INST_CONTROL_HELLO), mais un contenu qui ne décode
            // pas ne doit JAMAIS casser une connexion de travail — ce serait
            // sacrifier le débit de recherche pour un champ d'affichage.
            int32_t len = 0;
            if (recv_all(client->socket_id, &len, sizeof(len)) != (long)sizeof(len)) {
                log_error("client hello : longueur non reçue\n");
                return 0;
            }
            if (len < 0 || len > CLIENT_IDENTITY_WIRE_MAX_SIZE) {
                log_error("client hello : longueur hors borne (%d)\n", len);
                return 0;
            }
            uint8_t buf[CLIENT_IDENTITY_WIRE_MAX_SIZE];
            if (len > 0 && recv_all(client->socket_id, buf, (size_t)len) != (long)len) {
                log_error("client hello : payload incomplet\n");
                return 0;
            }
            if (client_identity_decode(buf, len, &client->identity) != 0) {
                log_error("client hello : décodage échoué (connexion conservée)\n");
            } else {
                client->has_identity = 1;
#ifdef DEBUG_SOCKET
                log_event("connexion de travail identifiée (fork_seq=%d, label=\"%s\")",
                          client->identity.fork_seq, client->identity.label);
#endif // DEBUG_SOCKET
            }
        } else if (instruction == INST_TEST_CONNECTED) {
            send_instruction(client->socket_id, INST_TEST_CONNECTED);
        } else if (*version_supported == 0) {
            log_error("Version of client unsupported\n");
            log_event("client rejeté : requête sans handshake de version valide");
            send_instruction(client->socket_id, INST_UNSUPPORTED_VERSION);
            return 0;
        } else
        {
            inst_unknow++;
            log_error("server instruction inconnu: %i\n",instruction);
            log_error("nb inst inconnu%li\n",inst_unknow);

            return 0;
        }
    return 1;
}

/**
 * @brief Thread de communication avec un client TCP connecté.
 *
 * Gère le protocole etii : vérification de version, puis boucle sur les instructions :
 * - INST_GET : envoie une possibilité depuis le datamanager au client.
 * - INST_ADD : reçoit une possibilité du client et l'ajoute au datamanager.
 * - INST_POSSIBILITY_ANALYSED : signale qu'une possibilité a été traitée.
 * - INST_SOLUTION : reçoit une solution complète, l'affiche et la sauvegarde.
 * - INST_TEST_CONNECTED : répond pour maintenir la connexion.
 * À la déconnexion, remet les dernières possibilités envoyées dans le datamanager.
 *
 * @param userdata Pointeur vers le `client_t` du thread.
 * @return         NULL.
 */
/**
 * @brief Libellé (haut niveau) de la cause de fin de session d'un client.
 *
 * Fonction pure : classe la dernière instruction observée par la boucle de
 * `communicate_with_client` en un motif lisible pour le flux d'évènements.
 *
 * @param last_instruction  Dernière instruction reçue (INST_END, -1, ou l'inst.
 *                          courante quand un `step` a demandé l'arrêt).
 * @return Chaîne statique décrivant la cause.
 */
const char *client_disconnect_reason(int8_t last_instruction)
{
    if (last_instruction == INST_END) {
        return "fin de session";
    }
    if (last_instruction == -1) {
        return "connexion perdue";
    }
    return "protocole interrompu";
}

/**
 * @brief Effectue un aller-retour CTRL_GET_STATS/CTRL_STATS sur la session de
 *        contrôle, met à jour le registre, et tire le plateau record
 *        (CTRL_GET_BEST_BOARD/CTRL_BEST_BOARD) si le client dépasse le record
 *        déjà connu du serveur. Factorisé entre le déclenchement manuel
 *        (commande `CTRL_GET_STATS` postée via `clientsStats`/l'API HTTP) et
 *        le sondage automatique périodique (voir `control_session_step`) :
 *        les deux ont exactement le même besoin — un `CTRL_STATS` frais pour
 *        détecter un nouveau record côté client.
 *
 * @return 1 si l'échange a réussi (registre à jour), 0 en cas d'erreur réseau
 *         ou de protocole (l'appelant doit alors clore la session).
 */
static int control_session_poll_stats(client_t *client, int session_index)
{
    if (ctrl_send_frame(client->socket_id, CTRL_GET_STATS, NULL, 0) != 0) {
        log_error("session de contrôle : échec d'envoi de CTRL_GET_STATS\n");
        return 0;
    }
    void *payload = NULL;
    int32_t plen = 0;
    int rcmd = ctrl_recv_frame(client->socket_id, &payload, &plen);
    if (rcmd != CTRL_STATS) {
        log_error("session de contrôle : réponse CTRL_STATS attendue, reçu %d\n", rcmd);
        free(payload);
        return 0;
    }
    control_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    int decoded = (payload != NULL) ? control_stats_decode(payload, plen, &stats) : -1;
    free(payload);
    if (decoded != 0) {
        log_error("session de contrôle : décodage CTRL_STATS échoué\n");
        return 0;
    }
    control_registry_record_stats(session_index, &stats);
    // Registre de clients CONNUS (PR4) : cumule cette lecture dans le total de
    // la machine plutôt que de simplement remplacer l'instantané (rôle de
    // control_registry ci-dessus). Résolution de l'identité par indice de
    // session, cette fonction ne détenant que session_index.
    client_identity_t known_identity;
    if (control_registry_get_identity(session_index, &known_identity) == 0) {
        known_clients_registry_on_stats(known_identity.machine_uid, known_identity.client_uid, &stats);
    }
    // Le protocole de travail (INST_ADD/…) ne fait progresser max_result
    // que quand ce client pousse effectivement des possibilités par cette
    // voie ; sans cette resynchronisation, un client qui n'annonce son
    // record QUE via CTRL_STATS ne le fait jamais apparaître dans les
    // stats globales du serveur (logs, GET /api/v1/stats), qui restent
    // en retard sur GET /api/v1/clients (alimenté par control_registry
    // ci-dessus) et sur GET /api/v1/best-board (g_server_best_board,
    // mis à jour juste plus bas).
    if (stats.max_result > max_result) {
        max_result = (uint16_t)stats.max_result;
    }
    log_info("stats client : coups/s=%llu stock=%llu analyse=%llu record=%llu pruner_checked=%llu pruner_removed=%llu pruner_cases/s=%llu\n",
              (unsigned long long)stats.shots_per_second,
              (unsigned long long)stats.possibility_stock,
              (unsigned long long)stats.analysed_stock,
              (unsigned long long)stats.max_result,
              (unsigned long long)stats.pruner_checked,
              (unsigned long long)stats.pruner_removed,
              (unsigned long long)stats.pruner_cells_per_second);
    control_registry_touch(session_index);

    // Le client rapporte un record supérieur à celui déjà connu du
    // serveur : on tire sa représentation (pas seulement le compte),
    // sur CETTE MÊME connexion, avant de repasser en attente de la
    // prochaine commande — c'est le serveur qui demande, uniquement
    // quand il en a besoin (jamais à chaque CTRL_STATS).
    if (stats.max_result > (unsigned long long)best_board_result(&g_server_best_board)) {
        if (ctrl_send_frame(client->socket_id, CTRL_GET_BEST_BOARD, NULL, 0) != 0) {
            log_error("session de contrôle : échec d'envoi de CTRL_GET_BEST_BOARD\n");
            return 0;
        }
        void *bpayload = NULL;
        int32_t blen = 0;
        int brcmd = ctrl_recv_frame(client->socket_id, &bpayload, &blen);
        if (brcmd != CTRL_BEST_BOARD) {
            log_error("session de contrôle : réponse CTRL_BEST_BOARD attendue, reçu %d\n", brcmd);
            free(bpayload);
            return 0;
        }
        if (bpayload != NULL && blen == (int32_t)(1 + sizeof(struct possibility_packet))
            && ((uint8_t *)bpayload)[0] != 0) {
            struct possibility_packet board;
            memcpy(&board, (uint8_t *)bpayload + 1, sizeof(board));
            if (best_board_try_record(&g_server_best_board, &board, board.alloc)) {
                log_event("nouveau plateau record reçu d'un client (%u pièces)",
                          (unsigned)board.alloc);
            }
        }
        free(bpayload);
        control_registry_touch(session_index);
    }
    return 1;
}

/**
 * @brief Un tour de la boucle de session de contrôle (voir etii_server.h pour
 *        le contrat complet).
 *
 * Le timeout de l'attente d'une commande postée est celui passé par l'appelant
 * (`run_control_session` le dérive de `tcp_timeout`, cf. sa doc) : rester sous
 * le `SO_RCVTIMEO` déjà posé sur le socket (configure_client_socket) pour ne
 * jamais laisser le pair croire la session morte pendant qu'on attend une
 * commande côté serveur.
 */
int control_session_step(client_t *client, int session_index, int timeout_ms)
{
    uint8_t cmd = 0;
    char line[CONTROL_COMMAND_LINE_MAX];
    int wr = control_registry_wait_command(session_index, &cmd, line, sizeof(line), timeout_ms);

    if (wr < 0) {
        // Session désenregistrée entre-temps (ou indice invalide) : rien à
        // faire côté réseau, la session est de toute façon morte.
        return 0;
    }

    if (wr == 0) {
        // Une commande a été postée pour cette session (console serveur).
        if (cmd == CTRL_COMMAND) {
            if (ctrl_send_frame(client->socket_id, CTRL_COMMAND, line, (int32_t)strlen(line)) != 0) {
                log_error("session de contrôle : échec d'envoi de CTRL_COMMAND\n");
                return 0;
            }
            void *payload = NULL;
            int32_t plen = 0;
            int rcmd = ctrl_recv_frame(client->socket_id, &payload, &plen);
            if (rcmd != CTRL_RESULT) {
                log_error("session de contrôle : réponse CTRL_RESULT attendue, reçu %d\n", rcmd);
                free(payload);
                return 0;
            }
            int32_t retcode = -1;
            if (plen >= (int32_t)sizeof(retcode) && payload != NULL) {
                memcpy(&retcode, payload, sizeof(retcode));
            }
            free(payload);
            log_event("commande distante \"%s\" exécutée (code retour %d)", line, retcode);
            control_registry_touch(session_index);
        } else if (cmd == CTRL_GET_STATS) {
            return control_session_poll_stats(client, session_index);
        } else {
            log_error("session de contrôle : commande interne inconnue (%u)\n", (unsigned)cmd);
        }
        return 1;
    }

    // wr == 1 : timeout, aucune commande en attente. Si le sondage
    // automatique est dû (CONTROL_AUTO_STATS_INTERVAL_SEC), un CTRL_GET_STATS
    // remplace le keepalive de ce tour — c'est ce qui permet à un nouveau
    // record côté client d'être tiré côté serveur sans attendre qu'un
    // opérateur lance `clientsStats` manuellement (cf. static_variables.h).
    if (control_registry_auto_stats_due(session_index, CONTROL_AUTO_STATS_INTERVAL_SEC)) {
        return control_session_poll_stats(client, session_index);
    }

    // Sinon : keepalive ping/pong, borné par le SO_RCVTIMEO déjà posé sur le
    // socket.
    if (ctrl_send_frame(client->socket_id, CTRL_PING, NULL, 0) != 0) {
        log_error("session de contrôle : échec d'envoi de CTRL_PING\n");
        return 0;
    }
    void *payload = NULL;
    int32_t plen = 0;
    int rcmd = ctrl_recv_frame(client->socket_id, &payload, &plen);
    free(payload);
    if (rcmd != CTRL_ACK) {
        log_error("session de contrôle : CTRL_ACK attendu, reçu %d\n", rcmd);
        return 0;
    }
    control_registry_touch(session_index);
    return 1;
}

/**
 * @brief Boucle de session de contrôle (voir etii_server.h pour le contrat).
 */
void run_control_session(client_t *client, int session_index)
{
    // Cadence de l'attente d'une commande postée : on reste sous le
    // SO_RCVTIMEO du socket (tcp_timeout, secondes) pour ne jamais laisser le
    // pair croire la session morte, plafonné à 2 s pour rester réactif.
    int timeout_ms = tcp_timeout * 500;
    if (timeout_ms > 2000) {
        timeout_ms = 2000;
    }
    if (timeout_ms < 1) {
        timeout_ms = 1;
    }

    while (request != REQUEST_STOP) {
        if (!control_session_step(client, session_index, timeout_ms)) {
            break;
        }
    }

    // Registre de clients CONNUS (PR4) : résoudre l'identité AVANT
    // control_registry_unregister, qui efface le hello de ce slot.
    client_identity_t known_identity;
    if (control_registry_get_identity(session_index, &known_identity) == 0) {
        known_clients_registry_on_disconnect(known_identity.machine_uid, known_identity.client_uid);
    }
    control_registry_unregister(session_index);
    log_event("session de contrôle déconnectée (slot %d)", session_index);

    shutdown(client->socket_id, 2);
    int err = closesocket(client->socket_id);
#ifdef DEBUG_SOCKET
    opened_tcp--;
#endif // DEBUG_SOCKET
    if (0 != err) {
        log_error("erreur close (session de contrôle) :%i\n", err);
    }

    usleep(THREAD_MICRO_SLEEP);

    client->socket_id = -1;
    client->exist = 0;
}

void *communicate_with_client (void *userdata)
{
    client_t *client = userdata;
    while (client->socket_id == -1)
    {
        usleep(MICRO_SLEEP);
    }

    int8_t instruction = recv_instruction(client->socket_id);

    array_possibility_packet *lastPossibilityPacketSend = NULL;
    int version_supported = 0;
    int control_session_index = -1;
    while(instruction != -1 && instruction != INST_END)
    {
        if (!communicate_with_client_step(client, instruction,
                                          &lastPossibilityPacketSend, &version_supported,
                                          &control_session_index))
        {
            break;
        }

        if (control_session_index >= 0)
        {
            // Bascule en canal de contrôle (INST_CONTROL_HELLO traité avec
            // succès) : run_control_session gère seule la suite ET l'épilogue
            // de cette connexion (fermeture socket incluse) — on ne doit PAS
            // continuer la boucle normale ni refermer le socket ici, sous
            // peine de double-close.
            if (lastPossibilityPacketSend != NULL) {
                // Aucune possibilité n'a pu être en cours d'envoi à ce stade
                // (INST_CONTROL_HELLO ne sert pas de possibilités), mais par
                // sécurité on applique le même traitement qu'à la déconnexion
                // normale avant de lâcher la main à la session de contrôle.
                requeue_last_sent_possibility(lastPossibilityPacketSend);
                free_array_possibility_packet(lastPossibilityPacketSend);
            }
            run_control_session(client, control_session_index);
            return NULL;
        }

        instruction = recv_instruction(client->socket_id);
    }
    log_event("client déconnecté (%s)", client_disconnect_reason(instruction));
    if(lastPossibilityPacketSend != NULL)
    {
        // À la déconnexion (propre OU non), la dernière possibilité servie au
        // client peut être restée « en analyse » : il l'a abandonnée sans
        // acquittement — il a quitté sur une solution, expiré, ou fermé. On la
        // rend alors au stock pour qu'elle reste exploitable (cf.
        // requeue_last_sent_possibility).
        requeue_last_sent_possibility(lastPossibilityPacketSend);
        free_array_possibility_packet(lastPossibilityPacketSend);
    }

    shutdown(client->socket_id, 2);
    int err = closesocket(client->socket_id);
#ifdef DEBUG_SOCKET
    opened_tcp--;
#endif // DEBUG_SOCKET
    if(0 != err)
    {
        log_error("erreur close :%i\n",err);
    }
    
    usleep(THREAD_MICRO_SLEEP);
    
    client->socket_id = -1;

    client->exist =0;
    
    return NULL;
}

/**
 * @brief Crée (ou recrée) le thread de communication pour le slot client `i`.
 *
 * Réinitialise le `socket_id` à -1 avant de créer le thread : celui-ci attendra
 * dans `communicate_with_client` que le socket soit affecté par la boucle principale.
 *
 * @param thread_params Tableau des contextes de threads serveur.
 * @param i             Indice du slot à initialiser.
 */
void create_server_thread(client_t *thread_params, int i) {
    client_t clientt = thread_params[i];
    /* création d'un nouveau thread */
    if(clientt.tid != NULL)
    {
        if (thread_params[i].tid != NULL) {
            free(thread_params[i].tid);
        }
        thread_params[i].tid = NULL;
    }
    thread_params[i].socket_id = -1;
    
    
    pthread_attr_t *thread_attributes = malloc(sizeof *thread_attributes);
    pthread_attr_init(thread_attributes);
    pthread_attr_setdetachstate(thread_attributes, PTHREAD_CREATE_DETACHED);
    /* Création du thread */
    thread_params[i].tid = malloc(sizeof(pthread_t));
    if(0 != pthread_create((thread_params[i].tid), thread_attributes, communicate_with_client, &(thread_params[i])))
    {
        log_error("Problème avec pthread_create()\n");
        free(thread_attributes);
        exit(EXIT_FAILURE);
    }
    pthread_attr_destroy(thread_attributes);
    free(thread_attributes);

    thread_params[i].exist = 1;
}

/**
 * @brief Thread d'élagage automatique des possibilités sans suite.
 *
 * Toutes les `server_rmnonext_timing` secondes, si aucun client n'est connecté,
 * appelle `remove_possibilities_with_no_next` pour nettoyer le datamanager.
 * L'élagage est suspendu tant que des clients sont actifs afin de ne pas
 * bloquer les files (mutex) pendant qu'elles sont en cours d'alimentation.
 *
 * @param param Non utilisé.
 * @return      NULL.
 */
/**
 * @brief Une passe d'élagage automatique (corps de boucle de `rmnonext_thread`,
 *        extrait pour être testable hors thread).
 *
 * Élague le datamanager (`remove_possibilities_with_no_next`) uniquement si
 * aucun client n'est connecté : l'élagage verrouille les files, il est suspendu
 * tant qu'elles sont en cours d'alimentation.
 */
void rmnonext_pass(map_big_array *map_parts, struct array_part *rotateParts)
{
    if (get_active_threads(thread_params) <= 0) {
#ifdef DEBUG_RM_NO_NEXT
        log_debug("Auto rmnonext\n");
#endif // DEBUG_RM_NO_NEXT
        remove_possibilities_with_no_next(map_parts, rotateParts);
#ifdef DEBUG_RM_NO_NEXT
    } else {

        log_debug("No auto rmnonext because thread active\n");
#endif // DEBUG_RM_NO_NEXT
    }
}

void *rmnonext_thread(void *param) {
    (void)param;
    struct array_part *apart= read_parts(parts_files);
    struct array_part *rotateParts = rotate_all_parts(apart);
    map_big_array *map_parts = prepare_map_part(rotateParts);
    while(request != REQUEST_STOP) {
        rmnonext_pass(map_parts, rotateParts);
        sleep(server_rmnonext_timing);
    }
    free_bigarray(map_parts);
    free_array_part(rotateParts);
    free_array_part(apart);

    return NULL;
}

/**
 * @brief Démarre le thread d'élagage automatique en mode détaché.
 */
void create_rmnonext_thread(void) {
    pthread_attr_t *thread_attributes = malloc(sizeof *thread_attributes);
    pthread_attr_init(thread_attributes);
    pthread_attr_setdetachstate(thread_attributes, PTHREAD_CREATE_DETACHED);
    /* Création du thread */
    pthread_t thread;
    if(0 != pthread_create(&thread, thread_attributes, rmnonext_thread, NULL))
    {
        log_error("create_rmnonext_thread : Problème avec pthread_create()\n");
        free(thread_attributes);
        exit(EXIT_FAILURE);
    }
    pthread_attr_destroy(thread_attributes);
    free(thread_attributes);
}

/**
 * @brief Point d'entrée du serveur EternityII.
 *
 * Initialise les possibilités de départ, démarre le thread d'élagage, crée
 * le pool de threads de communication, puis entre dans la boucle d'acceptation
 * TCP. Chaque client accepté est affecté à un slot libre du pool ; si tous les
 * slots sont occupés, un nouveau slot est créé dynamiquement dans la limite de
 * `NB_THREADS`.
 *
 * @param file Chemin du fichier CSV de définition des pièces.
 */
/**
 * @brief Alloue et initialise le pool de threads de communication du serveur
 *        (extrait de `runserver` pour être testable hors boucle accept).
 *
 * Positionne les globales `thread_params` (un `client_t` par thread, slot vide :
 * exist=0, socket=-1) et `fileUpdates` (compteurs à zéro), dimensionnées sur
 * `NB_THREADS`. `rotateParts` est partagé par tous les slots (sérialisation CSV
 * des solutions).
 */
void init_server_thread_pool(struct array_part *rotateParts)
{
    /* création du tableau de structures client_t avec un élément par thread */
    if(NULL == (thread_params = malloc(sizeof(*thread_params) * NB_THREADS)))
    {
        log_error("Problème avec malloc()\n");
        exit(EXIT_FAILURE);
    }
    fileUpdates = malloc(sizeof(unsigned long long) * NB_THREADS);
    for(int i = 0; i < NB_THREADS; i++)
    {
        thread_params[i].exist = 0;
        thread_params[i].socket_id = -1;
        thread_params[i].tid = NULL;
        thread_params[i].compteur = i;
        thread_params[i].rotate_parts = rotateParts;
        thread_params[i].peer_ip[0] = '\0';
        thread_params[i].has_identity = 0;
        fileUpdates[i] = 0;
    }
}

/**
 * @brief Applique les timeouts de session au socket d'un client fraîchement
 *        accepté (extrait de `runserver`).
 */
void configure_client_socket(int client_id)
{
    struct timeval tv;
    tv.tv_sec = tcp_timeout;
    tv.tv_usec = 0;
    // Timeout sur les sessions
    setsockopt(client_id, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(struct timeval));
    setsockopt(client_id, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(struct timeval));
}

/**
 * @brief Tente d'affecter un client accepté à un slot du pool (un tour de la
 *        boucle d'affectation de `runserver`, extrait pour être testable).
 *
 * Cherche d'abord un thread libre (exist, en attente de socket) ; à chaque
 * tour, régénère au plus un slot vide (`create_server_thread`) et l'affecte
 * directement si le client ne l'a pas encore été. Si tous les threads sont
 * occupés et qu'aucun slot n'est régénérable, journalise l'épisode UNE fois
 * (`*busy_logged`) puis cède le CPU (`usleep`) le temps qu'une session se
 * termine — sans pause, cette boucle saturait un cœur.
 *
 * @param client_id   Socket du client accepté.
 * @param busy_logged In/out : 1 si « all threads busy » a déjà été journalisé
 *                    pour cet épisode d'attente.
 * @return L'indice du slot affecté, ou -1 (l'appelant réessaie).
 */
int try_assign_client_slot(int client_id, const char *peer_ip, int *busy_logged)
{
    int thread_id = -1;
    /* recherche d'un thread libre (existe mais en attente de client) */
    int t = find_free_thread_slot(thread_params, NB_THREADS);
    if (t >= 0) {
        thread_id = t;
        thread_params[t].socket_id = client_id;
        times(&thread_params[t].start_socket);
        if (peer_ip != NULL) {
            strncpy(thread_params[t].peer_ip, peer_ip, PEER_IP_MAX_LEN - 1);
            thread_params[t].peer_ip[PEER_IP_MAX_LEN - 1] = '\0';
        }
        // Nouvelle connexion sur ce slot : toute identité laissée par
        // l'occupant précédent doit être oubliée avant le prochain hello.
        thread_params[t].has_identity = 0;
    }

    int nbCreated = 0;
    // A chaque affectation, on vérifie les threads pour en regéréner 1 si besoin
    // et affecter directement le client si il ne l'a pas été.
    int e = find_empty_thread_slot(thread_params, NB_THREADS);
    if (e >= 0) {
        create_server_thread(thread_params, e);
        if (thread_id == -1) {
            thread_id = e;
            thread_params[e].socket_id = client_id;
            times(&thread_params[e].start_socket);
            if (peer_ip != NULL) {
                strncpy(thread_params[e].peer_ip, peer_ip, PEER_IP_MAX_LEN - 1);
                thread_params[e].peer_ip[PEER_IP_MAX_LEN - 1] = '\0';
            }
            thread_params[e].has_identity = 0;
        }
        nbCreated++;
    }
    if (thread_id == -1 && nbCreated == 0) {
        // Tous les threads sont occupés et aucun slot libre : on attend
        // qu'un thread se libère.
        if (!*busy_logged) {
            log_event("request unfulfilled: all threads busy (NB_THREADS=%i) — attente d'un thread libre", NB_THREADS);
            *busy_logged = 1;
        }
        usleep(MICRO_SLEEP);
    }
    return thread_id;
}

void runserver(const char* file)
{
    struct array_part *apart= read_parts(file);
    struct array_part *rotateParts = rotate_all_parts(apart);
    map_big_array *map_parts = prepare_map_part(rotateParts);
    free_array_part(apart);
    first_possibility(map_parts, rotateParts);
    // Expansion du stock au démarrage (option --expand-level) : développe la
    // genèse en de nombreuses possibilités distribuables tant que la map est
    // vivante, pour que les clients trouvent tous du travail dès la connexion
    // (anti-famine du démarrage). No-op si expand_min_level == 0.
    if (expand_min_level > 0) {
        log_event("expansion du stock au démarrage : niveau visé %i", expand_min_level);
        expand_datas_to_level(expand_min_level, map_parts, rotateParts);
    }
    free_bigarray(map_parts);
    /* rotateParts reste en vie : les threads TCP l'utilisent pour sérialiser
     * les solutions en CSV avec les couleurs de bord. Libéré en fin de runserver. */
    // Même table, exposée en globale pour que l'API HTTP (src/net/http_server.c)
    // décode grid[x][y] en pièce réelle sans dupliquer la lecture du CSV.
    g_server_rotate_parts = rotateParts;

    // Demarrage d'un thread de nettoyage des possibilités sans suite
    create_rmnonext_thread();

    init_server_thread_pool(rotateParts);

    // API HTTP REST admin (option --http-port) : désactivée par défaut
    // (HTTP_PORT == 0). Démarrage fatal en cas d'échec car --http-port est
    // une demande explicite de l'utilisateur (port déjà pris, par exemple).
    if (HTTP_PORT > 0 && http_server_start(HTTP_PORT) != 0) {
        exit(EXIT_FAILURE);
    }

    int socket_id = create_tcp_server(SERVER_PORT, NB_THREADS);
    while (request != REQUEST_STOP) {
        int client_id;
        int thread_id;
        struct sockaddr_in peer_addr;
        socklen_t peer_addr_len = sizeof(peer_addr);

        if((client_id = accept(socket_id, (struct sockaddr *)&peer_addr, &peer_addr_len)) < 0)
        {
            if (errno == EINTR) {
                /* accept() interrompu par un signal (ex. SIGWINCH installé par
                   ncurses sans SA_RESTART lors d'un redimensionnement). Ce
                   n'est PAS une erreur : on réessaie (ou on sort proprement si
                   un arrêt a été demandé entre-temps). */
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                log_errno("resource blocked, try again => ");
                continue;
            } else {
                log_errno("Erreur sur accept() => ");
                exit(EXIT_FAILURE);
            }
        }
        configure_client_socket(client_id);
        char peer_ip[PEER_IP_MAX_LEN];
        if (inet_ntop(AF_INET, &peer_addr.sin_addr, peer_ip, sizeof(peer_ip)) == NULL) {
            snprintf(peer_ip, sizeof(peer_ip), "?");
        }
        log_event("nouveau client connecté (%s)", peer_ip);

        thread_id = -1;
        int busy_logged = 0; /* « all threads busy » : journalisé une seule fois par épisode d'attente */
        while (thread_id == -1) {
            thread_id = try_assign_client_slot(client_id, peer_ip, &busy_logged);
        }
    }
    g_server_rotate_parts = NULL;
    free_array_part(rotateParts);
}

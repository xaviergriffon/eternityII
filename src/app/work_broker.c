#include "app/work_broker.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "app/app_static_variables.h"
#include "app/etii_client.h"
#include "app/fork_gate.h"
#include "core/core_static_variables.h"
#include "core/datamanager.h"
#include "core/lifo.h"
#include "net/ipc_protocol.h"
#include "net/etii_protocol.h"
#include "net/local_socket.h"
#include "ui/logger.h"

/* Cadence du thread de relais quand le tampon est vide. Assez court pour que le
   travail cédé parte vite (c'est l'objectif : rendre au serveur ce qui stagne),
   assez long pour ne pas occuper un cœur à vide sur un client au repos. */
#define WORK_BROKER_IDLE_SLEEP_US 20000

/* Période du résumé d'activité du courtier (s). */
#define WORK_BROKER_SUMMARY_PERIOD_S 10

/* ------------------------------------------------------------------ */
/* Fonctions pures                                                     */
/* ------------------------------------------------------------------ */

int32_t work_broker_offer_encode(uint32_t seq, const struct possibility_packet *pkts,
                                 int count, void *buf, size_t bufsz)
{
    if (count < 0 || buf == NULL) {
        return -1;
    }
    size_t need = IPC_WORK_OFFER_HEADER_SIZE + (size_t)count * sizeof(struct possibility_packet);
    if (need > bufsz) {
        return -1;
    }
    uint8_t *p = buf;
    uint32_t s = seq;
    int32_t c = count;
    memcpy(p, &s, sizeof s);
    memcpy(p + 4, &c, sizeof c);
    if (count > 0) {
        memcpy(p + IPC_WORK_OFFER_HEADER_SIZE, pkts,
               (size_t)count * sizeof(struct possibility_packet));
    }
    return (int32_t)need;
}

int work_broker_offer_decode(const void *buf, size_t len, uint32_t *out_seq,
                             const struct possibility_packet **out_pkts, int *out_count)
{
    if (buf == NULL || len < IPC_WORK_OFFER_HEADER_SIZE) {
        return -1;
    }
    const uint8_t *p = buf;
    uint32_t seq;
    int32_t count;
    memcpy(&seq, p, sizeof seq);
    memcpy(&count, p + 4, sizeof count);
    if (count < 0) {
        return -1;
    }
    /* Longueur EXACTE exigée : un datagramme plus court que son `count` ferait
       lire au-delà du tampon reçu, un datagramme plus long trahit un cadrage
       incohérent. Dans les deux cas on jette plutôt que d'interpréter. */
    size_t expect = IPC_WORK_OFFER_HEADER_SIZE + (size_t)count * sizeof(struct possibility_packet);
    if (len != expect) {
        return -1;
    }
    if (out_seq != NULL) *out_seq = seq;
    if (out_count != NULL) *out_count = count;
    if (out_pkts != NULL) {
        *out_pkts = (const struct possibility_packet *)(p + IPC_WORK_OFFER_HEADER_SIZE);
    }
    return 0;
}

uint32_t work_broker_settled_advance(uint32_t known, uint32_t incoming)
{
    /* Un règlement ne recule jamais : un datagramme réordonné annonçant un
       `seq` plus ancien ne doit pas rouvrir une fenêtre déjà refermée. La
       comparaison se fait sur la distance non signée (moitié haute = « plus
       ancien »), pour rester juste au rebouclage de `seq`. */
    if (incoming == known || (uint32_t)(incoming - known) >= 0x80000000u) {
        return known;
    }
    return incoming;
}

int work_broker_window_allows(uint32_t last_offer, uint32_t last_settled, uint32_t window)
{
    /* Soustraction non signée : correcte même après rebouclage de `last_offer`
       (2^32 offres), là où une comparaison signée deviendrait fausse. */
    return (uint32_t)(last_offer - last_settled) < window;
}

/* ------------------------------------------------------------------ */
/* Côté fils                                                           */
/* ------------------------------------------------------------------ */

/* Écrits par le thread de recherche (offres) et le thread fork_udp (règlements),
   lus par le thread d'alimentation : accès atomiques relâchés, même motif que
   `server_hunger`. */
static uint32_t child_last_offer_seq = 0;
static uint32_t child_last_settled_seq = 0;

void work_broker_child_reset(void)
{
    __atomic_store_n(&child_last_offer_seq, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&child_last_settled_seq, 0, __ATOMIC_RELAXED);
}

int work_broker_ack_allowed(void)
{
    uint32_t offered = __atomic_load_n(&child_last_offer_seq, __ATOMIC_RELAXED);
    uint32_t settled = __atomic_load_n(&child_last_settled_seq, __ATOMIC_RELAXED);
    return offered == settled;
}

void work_broker_child_on_settled(const void *payload, size_t len)
{
    if (payload == NULL || len < sizeof(uint32_t)) {
        return;
    }
    uint32_t seq;
    memcpy(&seq, payload, sizeof seq);
    uint32_t known = __atomic_load_n(&child_last_settled_seq, __ATOMIC_RELAXED);
    __atomic_store_n(&child_last_settled_seq, work_broker_settled_advance(known, seq),
                     __ATOMIC_RELAXED);
}

/**
 * @brief Envoie UNE offre (au plus un datagramme) au parent.
 * @return 0 si le datagramme est parti, -1 sinon.
 */
static int child_send_one_offer(const struct possibility_packet *pkts, int count)
{
    if (main_addr == NULL || fork_checker_socket_id <= 0) {
        return -1;
    }
    size_t frame_cap = ipc_max_datagram();
    uint8_t *frame = malloc(frame_cap);
    if (frame == NULL) {
        return -1;
    }
    /* Le numéro n'est consommé qu'une fois le cadrage réussi : un `seq` brûlé
       sur un échec d'encodage bloquerait l'acquittement pour toujours (le
       courtier ne le réglerait jamais, faute de l'avoir reçu). */
    uint32_t next = __atomic_load_n(&child_last_offer_seq, __ATOMIC_RELAXED) + 1;
    frame[0] = (uint8_t)IPC_MSG_WORK_OFFER;
    int32_t body = work_broker_offer_encode(next, pkts, count, frame + 1, frame_cap - 1);
    if (body < 0) {
        free(frame);
        return -1;
    }
    ssize_t sent = sendto(fork_checker_socket_id, frame, (size_t)(1 + body), MSG_DONTWAIT,
                          (struct sockaddr *)main_addr, sizeof(struct sockaddr_un));
    free(frame);
    if (sent != (ssize_t)(1 + body)) {
        /* Tampon de réception du parent plein (EAGAIN) ou socket disparue :
           cas nominal de saturation, pas une anomalie. L'appelant retombe sur
           l'envoi direct au serveur. */
        return -1;
    }
    __atomic_store_n(&child_last_offer_seq, next, __ATOMIC_RELAXED);
    return 0;
}

/**
 * @brief Crochet installé sur `datamanager_set_local_offer`.
 *
 * Découpe le lot en datagrammes et s'arrête au premier échec : `*consumed`
 * porte alors le préfixe réellement cédé, et `add_possibility` envoie le reste
 * au serveur — jamais le lot entier, qui dupliquerait le préfixe.
 */
static int child_offer_hook(array_possibility_packet *aposs, int *consumed)
{
    if (consumed != NULL) {
        *consumed = 0;
    }
    if (aposs == NULL || aposs->size <= 0) {
        return -1;
    }
    int per_dgram = (int)ipc_work_offer_max_packets();
    if (per_dgram <= 0) {
        return -1;
    }
    int done = 0;
    while (done < aposs->size) {
        uint32_t offered = __atomic_load_n(&child_last_offer_seq, __ATOMIC_RELAXED);
        uint32_t settled = __atomic_load_n(&child_last_settled_seq, __ATOMIC_RELAXED);
        if (!work_broker_window_allows(offered, settled, WORK_BROKER_OFFER_WINDOW)) {
            break; /* fenêtre pleine : le reste part au serveur */
        }
        int chunk = aposs->size - done;
        if (chunk > per_dgram) {
            chunk = per_dgram;
        }
        if (child_send_one_offer(&aposs->possibilities[done], chunk) != 0) {
            break;
        }
        done += chunk;
    }
    if (consumed != NULL) {
        *consumed = done;
    }
    return (done == aposs->size) ? 0 : -1;
}

void work_broker_child_install(void)
{
    datamanager_set_local_offer(child_offer_hook);
    datamanager_set_ack_gate(work_broker_ack_allowed);
}

/* ------------------------------------------------------------------ */
/* Côté parent                                                         */
/* ------------------------------------------------------------------ */

/** Une entrée du tampon : le paquet et l'origine dont le règlement a besoin. */
typedef struct {
    int slot;       /**< indice du fils dans forkId[] */
    uint32_t seq;   /**< numéro de l'offre dont ce paquet provient */
    struct possibility_packet pkt;
} broker_entry_t;

static pthread_mutex_t broker_lock = PTHREAD_MUTEX_INITIALIZER;
static File broker_queue;
static int broker_queue_ready = 0;
static client_possibility_t *broker_client = NULL;
static pthread_t broker_thread;
static int broker_thread_running = 0;
static volatile int broker_stop_requested = 0;

/* Dernier `seq` rendu durable, par fils. Lu/écrit sous broker_lock. */
static uint32_t broker_settled_seq[FORK_GATE_MAX_PARTICIPANTS > 64 ? FORK_GATE_MAX_PARTICIPANTS : 64];
/* Cumul depuis le démarrage, pour le résumé périodique et `work_broker_relayed_total`. */
static unsigned long long broker_relayed_total = 0;
#define BROKER_MAX_SLOTS ((int)(sizeof broker_settled_seq / sizeof broker_settled_seq[0]))

void work_broker_parent_reset(void)
{
    pthread_mutex_lock(&broker_lock);
    if (broker_queue_ready) {
        /* Vidange élément par élément : `free_file` libère AUSSI la structure
           `File` elle-même (cf. sa doc), or la nôtre est statique — la lui
           passer corrompt le tas. */
        broker_entry_t drop;
        while (scroll_fifo(&broker_queue, &drop)) {
            ;
        }
    }
    init_file(&broker_queue, sizeof(broker_entry_t));
    broker_queue_ready = 1;
    memset(broker_settled_seq, 0, sizeof broker_settled_seq);
    __atomic_store_n(&broker_relayed_total, 0, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&broker_lock);
}

unsigned long long work_broker_relayed_total(void)
{
    return __atomic_load_n(&broker_relayed_total, __ATOMIC_RELAXED);
}

unsigned long long work_broker_pending_packets(void)
{
    pthread_mutex_lock(&broker_lock);
    unsigned long long n = broker_queue_ready ? broker_queue.size : 0;
    pthread_mutex_unlock(&broker_lock);
    return n;
}

void work_broker_on_offer(int fork_slot, const void *payload, size_t len)
{
    uint32_t seq = 0;
    const struct possibility_packet *pkts = NULL;
    int count = 0;
    if (fork_slot < 0 || fork_slot >= BROKER_MAX_SLOTS) {
        return;
    }
    if (work_broker_offer_decode(payload, len, &seq, &pkts, &count) != 0) {
        log_error("courtier : offre mal formée d'un fils (slot %d, %zu octets), ignorée\n",
                  fork_slot, len);
        return;
    }
    pthread_mutex_lock(&broker_lock);
    if (!broker_queue_ready) {
        init_file(&broker_queue, sizeof(broker_entry_t));
        broker_queue_ready = 1;
    }
    for (int i = 0; i < count; i++) {
        broker_entry_t e;
        e.slot = fork_slot;
        e.seq = seq;
        memcpy(&e.pkt, &pkts[i], sizeof e.pkt);
        if (!put(&broker_queue, &e)) {
            /* OOM : on n'avance JAMAIS le règlement de ce `seq`. Le fils ne
               pourra donc plus acquitter, et sa racine restera attribuée
               côté serveur — le travail sera refait plutôt que perdu. */
            log_error("courtier : mémoire épuisée, offre %u du fils %d abandonnée "
                      "(la racine restera attribuée, aucun acquittement)\n", seq, fork_slot);
            break;
        }
    }
    pthread_mutex_unlock(&broker_lock);
}

/**
 * @brief Notifie un fils du `seq` réglé pour lui.
 */
static void broker_notify_settled(int slot, uint32_t seq)
{
    if (main_socket_id == NULL || forkId == NULL || slot < 0 || slot >= NB_THREADS) {
        return;
    }
    if (forkId[slot] == NULL || strcmp(forkId[slot], "") == 0) {
        return;
    }
    uint8_t frame[1 + sizeof(uint32_t)];
    frame[0] = (uint8_t)IPC_MSG_WORK_SETTLED;
    memcpy(frame + 1, &seq, sizeof seq);
    struct sockaddr_un *addr = build_sockaddr(forkId[slot]);
    if (addr == NULL) {
        return;
    }
    /* Best effort : un règlement perdu ne perd aucun travail, il ne fait que
       retarder l'acquittement du fils — le prochain règlement porte un `seq`
       plus grand et rattrape celui-ci (valeur absolue, pas un incrément). */
    sendto(*main_socket_id, frame, sizeof frame, MSG_DONTWAIT,
           (struct sockaddr *)addr, sizeof(struct sockaddr_un));
    free(addr);
}

/**
 * @brief Pousse UNE possibilité au serveur depuis la connexion du courtier.
 *
 * Reproduit le cadrage de `put_to_server` (INST_ADD + paquet + INST_CONSIDERED)
 * plutôt que de l'appeler : celle-ci verse dans les pools `datamanager` LOCAUX
 * ce que le serveur refuse, or côté parent rien ne les draine et l'origine
 * (`slot`, `seq`) du paquet y serait perdue — le fils resterait bloqué sans
 * que le travail ne reparte jamais. Ici, un refus laisse simplement le paquet
 * en tête du tampon, réessayé au tour suivant.
 *
 * @return 0 si le serveur a pris le paquet en compte, -1 sinon.
 */
static int broker_send_one(const struct possibility_packet *pkt)
{
    server_socket_io_lock(broker_client);
    int socket_id = check_and_connect_to_server(broker_client);
    if (socket_id == -1) {
        server_socket_io_unlock(broker_client);
        return -1;
    }
    int ok = send_instruction(socket_id, INST_ADD) > 0
             && send_all(socket_id, (void *)pkt, sizeof *pkt) == (long)sizeof *pkt;
    if (ok) {
        int8_t ack = recv_instruction(socket_id);
        if (ack != INST_CONSIDERED) {
            /* INST_ERROR = stock serveur momentanément verrouillé (sauvegarde
               cohérente) : dégradation NOMINALE, on réessaiera. Toute autre
               valeur est une vraie anomalie protocolaire. */
            if (ack != INST_ERROR) {
                log_error("courtier : ack serveur inattendu (%d)\n", (int)ack);
            }
            ok = 0;
        }
    }
    server_socket_io_unlock(broker_client);
    return ok ? 0 : -1;
}

int work_broker_relay_step(void)
{
    if (broker_client == NULL) {
        return 0;
    }
    int per_dgram = (int)ipc_work_offer_max_packets();
    int cap = per_dgram > 0 ? per_dgram * WORK_BROKER_OFFER_WINDOW : 32;

    uint32_t high[BROKER_MAX_SLOTS];
    int touched[BROKER_MAX_SLOTS];
    memset(high, 0, sizeof high);
    memset(touched, 0, sizeof touched);

    int sent = 0;
    while (sent < cap) {
        /* On COPIE la tête sans la retirer, et on ne la retire qu'une fois le
           serveur d'accord : un paquet n'est jamais « en vol » hors de la file,
           donc jamais perdu si l'envoi échoue — et l'ordre FIFO par fils est
           préservé, ce dont dépend l'avancement du règlement (un `seq` réglé
           vaut pour tous les précédents de ce fils). Seul CE thread retire ;
           les fils n'ajoutent qu'en queue. La tête copiée reste donc la tête. */
        broker_entry_t head;
        pthread_mutex_lock(&broker_lock);
        int have = (broker_queue_ready && broker_queue.start != NULL);
        if (have) {
            memcpy(&head, broker_queue.start->value, sizeof head);
        }
        pthread_mutex_unlock(&broker_lock);
        if (!have) {
            break;
        }

        if (broker_send_one(&head.pkt) != 0) {
            break; /* la tête reste en place, réessayée au tour suivant */
        }

        pthread_mutex_lock(&broker_lock);
        broker_entry_t discarded;
        scroll_fifo(&broker_queue, &discarded);
        pthread_mutex_unlock(&broker_lock);
        sent++;

        if (head.slot >= 0 && head.slot < BROKER_MAX_SLOTS) {
            if (!touched[head.slot] || head.seq > high[head.slot]) {
                high[head.slot] = head.seq;
                touched[head.slot] = 1;
            }
        }
    }

    if (sent == 0) {
        return 0;
    }
    pthread_mutex_lock(&broker_lock);
    for (int s = 0; s < BROKER_MAX_SLOTS; s++) {
        if (touched[s] && high[s] > broker_settled_seq[s]) {
            broker_settled_seq[s] = high[s];
        }
    }
    pthread_mutex_unlock(&broker_lock);
    for (int s = 0; s < BROKER_MAX_SLOTS; s++) {
        if (touched[s]) {
            broker_notify_settled(s, high[s]);
        }
    }
    __atomic_fetch_add(&broker_relayed_total, (unsigned long long)sent, __ATOMIC_RELAXED);
    return sent;
}

static void *broker_relay_thread(void *unused)
{
    (void)unused;
    int gate_slot = fork_gate_register("work_broker");
    time_t last_summary = time(NULL);
    unsigned long long last_summary_total = 0;
    while (!broker_stop_requested && request != REQUEST_STOP) {
        fork_gate_checkpoint(gate_slot);
        /* L'échange TCP est borné par tcp_timeout (SO_RCVTIMEO/SO_SNDTIMEO),
           donc potentiellement plus long que le budget de quiescence : on se
           déclare BLOCKED autour, comme la console autour de son read. Sans
           cela, une demande de fork pendant un relais expirerait et le fork
           serait refusé. */
        fork_gate_mark_blocked(gate_slot, 1);
        int moved = work_broker_relay_step();
        fork_gate_mark_blocked(gate_slot, 0);
        fork_gate_checkpoint(gate_slot);
        if (moved == 0) {
            usleep(WORK_BROKER_IDLE_SLEEP_US);
        }
        /* Résumé périodique plutôt qu'une ligne par lot : sans lui, un
           opérateur n'a AUCUN moyen de savoir si le courtier travaille ou si
           tout retombe sur l'envoi direct — les deux se ressemblent de
           l'extérieur. Cadence basse, sur la même horloge que les autres
           statistiques serveur. */
        time_t now = time(NULL);
        if (now - last_summary >= WORK_BROKER_SUMMARY_PERIOD_S) {
            last_summary = now;
            unsigned long long total = work_broker_relayed_total();
            if (total != last_summary_total) {
                log_event("courtier : %llu possibilités relayées au serveur (%llu en tampon)",
                          total, work_broker_pending_packets());
                last_summary_total = total;
            }
        }
    }
    fork_gate_unregister(gate_slot);
    return NULL;
}

int work_broker_parent_start(const char *server_host)
{
    if (!local_dispatch_enabled || broker_thread_running) {
        return -1;
    }
    if (server_host == NULL) {
        return -1;
    }
    set_server_ip(server_host);
    broker_client = malloc(sizeof(*broker_client));
    if (broker_client == NULL) {
        return -1;
    }
    /* `fork_seq = -1` : la convention du hello d'identité pour « le process
       parent, pas un fork particulier » (cf. net/client_identity.h). */
    init_client_possibility(broker_client, NULL, NULL, 0, 0, getpid(), -1);
    work_broker_parent_reset();
    broker_stop_requested = 0;
    if (pthread_create(&broker_thread, NULL, broker_relay_thread, NULL) != 0) {
        log_error("courtier : pthread_create a échoué — chaque fork enverra lui-même\n");
        free(broker_client);
        broker_client = NULL;
        return -1;
    }
    broker_thread_running = 1;
    log_info("courtier de travail actif (fenêtre %d offres/fils, %zu paquets/offre)\n",
             WORK_BROKER_OFFER_WINDOW, ipc_work_offer_max_packets());
    return 0;
}

void work_broker_parent_stop(void)
{
    if (!broker_thread_running) {
        return;
    }
    broker_stop_requested = 1;
    pthread_join(broker_thread, NULL);
    broker_thread_running = 0;
    /* Dernier passage : ce qui reste part au serveur tant que la connexion le
       permet. Ce qui ne passe pas n'est jamais acquitté par son fils. */
    while (work_broker_relay_step() > 0) {
        ;
    }
    if (broker_client != NULL) {
        if (broker_client->socket_id != -1) {
            close_socket(broker_client->socket_id);
            broker_client->socket_id = -1;
        }
        free(broker_client);
        broker_client = NULL;
    }
}

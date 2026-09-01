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

int work_broker_acc_add(work_broker_offer_acc_t *ring, int n, uint32_t seq, int count)
{
    for (int i = 0; i < n; i++) {
        if (!ring[i].used) {
            ring[i].used = 1;
            ring[i].seq = seq;
            ring[i].remaining = count;
            return 0;
        }
    }
    return -1;
}

int work_broker_acc_dispose(work_broker_offer_acc_t *ring, int n, uint32_t seq)
{
    for (int i = 0; i < n; i++) {
        if (ring[i].used && ring[i].seq == seq) {
            if (ring[i].remaining > 0) {
                ring[i].remaining--;
            }
            return 0;
        }
    }
    return -1;
}

uint32_t work_broker_acc_settle(work_broker_offer_acc_t *ring, int n, uint32_t settled)
{
    for (;;) {
        uint32_t want = settled + 1;
        int found = -1;
        for (int i = 0; i < n; i++) {
            if (ring[i].used && ring[i].seq == want) {
                found = i;
                break;
            }
        }
        /* Offre inconnue (pas encore reçue) ou incomplète : on s'arrête ici.
           Sauter par-dessus laisserait le fils acquitter une racine dont du
           travail circule encore. */
        if (found < 0 || ring[found].remaining > 0) {
            return settled;
        }
        ring[found].used = 0;
        settled = want;
    }
}

int work_broker_tag_encode(int32_t slot, uint32_t seq, void *buf, size_t bufsz)
{
    if (buf == NULL || bufsz < IPC_WORK_TAG_SIZE) {
        return -1;
    }
    uint8_t *p = buf;
    memcpy(p, &slot, sizeof slot);
    memcpy(p + 4, &seq, sizeof seq);
    return IPC_WORK_TAG_SIZE;
}

int work_broker_tag_decode(const void *buf, size_t len, int32_t *out_slot, uint32_t *out_seq)
{
    if (buf == NULL || len < IPC_WORK_TAG_SIZE) {
        return -1;
    }
    const uint8_t *p = buf;
    if (out_slot != NULL) memcpy(out_slot, p, sizeof(int32_t));
    if (out_seq != NULL) memcpy(out_seq, p + 4, sizeof(uint32_t));
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

/* Attribution détenue par ce fils : au plus une (un fork n'étudie qu'une
   racine à la fois). Écrite par le thread fork_udp, lue/consommée par le thread
   d'alimentation — d'où le mutex, la charge n'étant pas un simple entier. */
static pthread_mutex_t child_grant_lock = PTHREAD_MUTEX_INITIALIZER;
static int child_grant_pending = 0;   /* reçu, pas encore pris */
static int child_grant_held = 0;      /* pris, pas encore terminé */
static int32_t child_grant_slot = -1;
static uint32_t child_grant_seq = 0;
static struct possibility_packet child_grant_pkt;

void work_broker_child_reset(void)
{
    __atomic_store_n(&child_last_offer_seq, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&child_last_settled_seq, 0, __ATOMIC_RELAXED);
    pthread_mutex_lock(&child_grant_lock);
    child_grant_pending = 0;
    child_grant_held = 0;
    child_grant_slot = -1;
    child_grant_seq = 0;
    pthread_mutex_unlock(&child_grant_lock);
}

int work_broker_ack_allowed(void)
{
    if (local_dispatch_enabled) {
        /* Mode exclusif : ce fils n'a AUCUNE connexion de travail, et rien à
           acquitter — c'est le parent qui détient les racines et les acquitte.
           Sans ce refus, `send_possibility_analysed` ouvrirait une connexion
           au serveur (elle se connecte avant de regarder si la file est vide),
           ce qui ferait réapparaître exactement les N connexions par client
           que l'arbitrage A2 supprime. */
        return 0;
    }
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
    if (done < aposs->size && local_dispatch_enabled) {
        /* Mode exclusif : pas de serveur pour ce fils. Le reste va dans ses
           propres pools, d'où il le reprendra (cf. work_broker_child_take_local).
           Perdu si le fils meurt — sans conséquence : le parent réinjecte alors
           la racine attribuée, et rien n'a été acquitté. */
        array_possibility_packet rest = {
            .size = aposs->size - done,
            .possibilities = aposs->possibilities + done
        };
        put_to_local(&rest);
        done = aposs->size;
    }
    if (consumed != NULL) {
        *consumed = done;
    }
    return (done == aposs->size) ? 0 : -1;
}

array_possibility_packet *work_broker_child_take_local(void)
{
    array_possibility_packet *out = malloc(sizeof(*out));
    if (out == NULL) {
        return NULL;
    }
    out->size = 0;
    out->possibilities = NULL;
    scroll_from_local(out, 1);
    if (out->size == 0) {
        free_array_possibility_packet(out);
        return NULL;
    }
    return out;
}

void work_broker_child_on_grant(const void *payload, size_t len)
{
    if (payload == NULL || len < IPC_WORK_TAG_SIZE + sizeof(struct possibility_packet)) {
        return;
    }
    int32_t slot = -1;
    uint32_t seq = 0;
    if (work_broker_tag_decode(payload, len, &slot, &seq) != 0) {
        return;
    }
    pthread_mutex_lock(&child_grant_lock);
    if (child_grant_pending || child_grant_held) {
        /* Le courtier n'en émet pas deux : un doublon serait un rejeu, et
           l'écraser perdrait la possibilité déjà reçue. */
        pthread_mutex_unlock(&child_grant_lock);
        return;
    }
    child_grant_slot = slot;
    child_grant_seq = seq;
    memcpy(&child_grant_pkt, (const uint8_t *)payload + IPC_WORK_TAG_SIZE, sizeof child_grant_pkt);
    child_grant_pending = 1;
    pthread_mutex_unlock(&child_grant_lock);
}

void work_broker_child_request_work(void)
{
    if (main_addr == NULL || fork_checker_socket_id <= 0) {
        return;
    }
    pthread_mutex_lock(&child_grant_lock);
    int busy = child_grant_pending || child_grant_held;
    pthread_mutex_unlock(&child_grant_lock);
    if (busy) {
        return;
    }
    uint8_t frame = (uint8_t)IPC_MSG_WORK_REQUEST;
    sendto(fork_checker_socket_id, &frame, sizeof frame, MSG_DONTWAIT,
           (struct sockaddr *)main_addr, sizeof(struct sockaddr_un));
}

array_possibility_packet *work_broker_child_take_grant(void)
{
    pthread_mutex_lock(&child_grant_lock);
    if (!child_grant_pending) {
        pthread_mutex_unlock(&child_grant_lock);
        return NULL;
    }
    array_possibility_packet *out = malloc(sizeof(*out));
    if (out != NULL) {
        out->possibilities = malloc(sizeof(struct possibility_packet));
    }
    if (out == NULL || out->possibilities == NULL) {
        free(out);
        pthread_mutex_unlock(&child_grant_lock);
        return NULL; /* l'attribution reste en attente, reprise au tour suivant */
    }
    memcpy(out->possibilities, &child_grant_pkt, sizeof child_grant_pkt);
    out->size = 1;
    child_grant_pending = 0;
    child_grant_held = 1;
    pthread_mutex_unlock(&child_grant_lock);
    return out;
}

void work_broker_child_report_done(void)
{
    pthread_mutex_lock(&child_grant_lock);
    if (!child_grant_held) {
        pthread_mutex_unlock(&child_grant_lock);
        return;
    }
    int32_t slot = child_grant_slot;
    uint32_t seq = child_grant_seq;
    child_grant_held = 0;
    pthread_mutex_unlock(&child_grant_lock);

    if (main_addr == NULL || fork_checker_socket_id <= 0) {
        return;
    }
    uint8_t frame[1 + IPC_WORK_TAG_SIZE];
    frame[0] = (uint8_t)IPC_MSG_WORK_DONE;
    work_broker_tag_encode(slot, seq, frame + 1, IPC_WORK_TAG_SIZE);
    sendto(fork_checker_socket_id, frame, sizeof frame, MSG_DONTWAIT,
           (struct sockaddr *)main_addr, sizeof(struct sockaddr_un));
}

int work_broker_child_is_exclusive(void)
{
    return local_dispatch_enabled ? 1 : 0;
}

void work_broker_child_on_hunger(const void *payload, size_t len)
{
    if (payload == NULL || len < sizeof(int32_t)) {
        return;
    }
    int32_t hunger;
    memcpy(&hunger, payload, sizeof hunger);
    if (hunger < 0) {
        hunger = 0;
    }
    __atomic_store_n(&server_hunger, (int)hunger, __ATOMIC_RELAXED);
}

void work_broker_child_install(void)
{
    datamanager_set_local_offer(child_offer_hook);
    datamanager_set_ack_gate(work_broker_ack_allowed);
    // `delegate_shallow_first` n'est PAS forcé ici : l'arbitrage A6 (« vers un
    // courtier local, céder les frères les moins profonds ») attendait sa
    // mesure, et la première ne le confirme pas. Il reste piloté par
    // --split-shallow-first, pour être comparé proprement en PR6.
}

/* ------------------------------------------------------------------ */
/* Côté parent                                                         */
/* ------------------------------------------------------------------ */

/** Une entrée du tampon : le paquet et l'origine dont le règlement a besoin. */
typedef struct {
    int slot;             /**< indice du fils dans forkId[] */
    uint32_t seq;         /**< numéro de l'offre dont ce paquet provient */
    long long enqueued_ms;/**< horloge monotone d'entrée en tampon (sursis A3) */
    struct possibility_packet pkt;
} broker_entry_t;

/** Horloge monotone en millisecondes (jamais l'heure murale : un changement
    d'heure ne doit pas rendre une entrée éligible ou non). */
static long long broker_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static pthread_mutex_t broker_lock = PTHREAD_MUTEX_INITIALIZER;
static File broker_queue;
static int broker_queue_ready = 0;
static client_possibility_t *broker_client = NULL;
static pthread_t broker_thread;
static int broker_thread_running = 0;
static volatile int broker_stop_requested = 0;
/* Vidage final : le sursis A3 n'a plus de sens, plus aucun fils ne réclamera. */
static int broker_draining = 0;

/* Comptabilité par fils : une offre reste suivie tant que tous ses paquets ne
   sont pas disposés (poussés au serveur, ou prouvés morts par un fils). */
static work_broker_offer_acc_t broker_acc[64][WORK_BROKER_OFFER_WINDOW];

/** Attribution en cours d'un fils : au plus UNE (un fork n'étudie qu'une
    racine à la fois). Conservée pour pouvoir la réinjecter si le fils meurt. */
typedef struct {
    int valid;
    broker_entry_t entry;
} broker_inflight_t;
static broker_inflight_t broker_granted[64];

/* Dernier `seq` rendu durable, par fils. Lu/écrit sous broker_lock. */
static uint32_t broker_settled_seq[FORK_GATE_MAX_PARTICIPANTS > 64 ? FORK_GATE_MAX_PARTICIPANTS : 64];
/* Cumul depuis le démarrage, pour le résumé périodique et `work_broker_relayed_total`. */
static unsigned long long broker_relayed_total = 0;
/* Possibilités attribuées à un fils plutôt que poussées au serveur : c'est la
   mesure de la redistribution elle-même, invisible du compteur de relais. */
static unsigned long long broker_granted_total = 0;
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
    memset(broker_acc, 0, sizeof broker_acc);
    memset(broker_granted, 0, sizeof broker_granted);
    __atomic_store_n(&broker_relayed_total, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&broker_granted_total, 0, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&broker_lock);
}

unsigned long long work_broker_relayed_total(void)
{
    return __atomic_load_n(&broker_relayed_total, __ATOMIC_RELAXED);
}

unsigned long long work_broker_granted_total(void)
{
    return __atomic_load_n(&broker_granted_total, __ATOMIC_RELAXED);
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
    /* Suivi AVANT insertion : si l'anneau est plein, le fils a dépassé sa
       fenêtre (bogue) — on refuse l'offre entière plutôt que de n'en suivre
       qu'une partie, ce qui rendrait le règlement incohérent. Le fils reste
       alors bloqué sur son acquittement : sûr, et visible. */
    if (work_broker_acc_add(broker_acc[fork_slot], WORK_BROKER_OFFER_WINDOW, seq, count) != 0) {
        pthread_mutex_unlock(&broker_lock);
        log_error("courtier : fenêtre dépassée par le fils %d (offre %u), offre refusée\n",
                  fork_slot, seq);
        return;
    }
    long long now_ms = broker_now_ms();
    for (int i = 0; i < count; i++) {
        broker_entry_t e;
        e.slot = fork_slot;
        e.seq = seq;
        e.enqueued_ms = now_ms;
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
 * @brief Envoie un datagramme cadré à un fils. @return 0 si parti, -1 sinon.
 */
static int broker_send_to_child(int slot, const void *frame, size_t len)
{
    if (main_socket_id == NULL || forkId == NULL || slot < 0 || slot >= NB_THREADS) {
        return -1;
    }
    if (forkId[slot] == NULL || strcmp(forkId[slot], "") == 0) {
        return -1;
    }
    struct sockaddr_un *addr = build_sockaddr(forkId[slot]);
    if (addr == NULL) {
        return -1;
    }
    ssize_t sent = sendto(*main_socket_id, frame, len, MSG_DONTWAIT,
                          (struct sockaddr *)addr, sizeof(struct sockaddr_un));
    free(addr);
    return (sent == (ssize_t)len) ? 0 : -1;
}

/**
 * @brief Notifie un fils du `seq` réglé pour lui.
 *
 * Best effort : un règlement perdu ne perd aucun travail, il ne fait que
 * retarder l'acquittement du fils — le prochain porte un `seq` plus grand et
 * rattrape celui-ci (valeur absolue, pas un incrément).
 */
static void broker_notify_settled(int slot, uint32_t seq)
{
    uint8_t frame[1 + sizeof(uint32_t)];
    frame[0] = (uint8_t)IPC_MSG_WORK_SETTLED;
    memcpy(frame + 1, &seq, sizeof seq);
    broker_send_to_child(slot, frame, sizeof frame);
}

/**
 * @brief Pousse UNE possibilité au serveur depuis la connexion du courtier.
 *
 * Reproduit le cadrage de `put_to_server` (INST_ADD + paquet + INST_CONSIDERED)
 * plutôt que de l'appeler : celle-ci verse dans les pools `datamanager` LOCAUX
 * ce que le serveur refuse, or côté parent rien ne les draine et l'origine
 * (`slot`, `seq`) du paquet y serait perdue — le fils resterait bloqué sans
 * que le travail ne reparte jamais.
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

/**
 * @brief Décompte un paquet disposé et notifie le fils si son règlement avance.
 *
 * `broker_lock` NE doit PAS être tenu à l'appel : la notification est une I/O.
 */
static void broker_dispose_and_settle(int slot, uint32_t seq)
{
    /* slot < 0 : racine obtenue du serveur par le parent, pas l'offre d'un
       fils — rien à décompter ni à régler. */
    if (slot < 0 || slot >= BROKER_MAX_SLOTS) {
        return;
    }
    pthread_mutex_lock(&broker_lock);
    work_broker_acc_dispose(broker_acc[slot], WORK_BROKER_OFFER_WINDOW, seq);
    uint32_t before = broker_settled_seq[slot];
    uint32_t after = work_broker_acc_settle(broker_acc[slot], WORK_BROKER_OFFER_WINDOW, before);
    broker_settled_seq[slot] = after;
    pthread_mutex_unlock(&broker_lock);
    if (after != before) {
        broker_notify_settled(slot, after);
    }
}

/**
 * @brief Annonce à tous les fils que le courtier manque de travail.
 *
 * La valeur est un ORDRE DE GRANDEUR, pas une cible exacte : elle borne ce
 * qu'un fils cède d'un coup (`bt_delegation_quota` en prend au plus la moitié
 * de son stock implicite). Un fils presque à sec ne se vide donc jamais pour
 * cette annonce.
 */
static void broker_broadcast_hunger(void)
{
    int32_t hunger = (int32_t)(NB_THREADS > 0 ? NB_THREADS : 1)
                     * (int32_t)ipc_work_offer_max_packets();
    uint8_t frame[1 + sizeof(int32_t)];
    frame[0] = (uint8_t)IPC_MSG_WORK_HUNGER;
    memcpy(frame + 1, &hunger, sizeof hunger);
    for (int s = 0; s < NB_THREADS; s++) {
        broker_send_to_child(s, frame, sizeof frame);
    }
}

void work_broker_on_request(int fork_slot)
{
    if (fork_slot < 0 || fork_slot >= BROKER_MAX_SLOTS) {
        return;
    }
    broker_entry_t e;
    pthread_mutex_lock(&broker_lock);
    if (broker_granted[fork_slot].valid || !broker_queue_ready
        || !scroll_fifo(&broker_queue, &e)) {
        /* Déjà servi, ou rien en tampon : pas d'attribution. En mode
           historique le fils enchaîne sur le serveur ; en mode exclusif il
           n'a personne d'autre, donc on RÉCLAME aux autres fils — c'est la
           délégation anticipée de la v8, repointée sur le courtier. Sans
           cela, un fils tenant une racine profonde ne cède rien avant
           `max_stock_by_thread` et ses frères restent à sec pendant ce
           temps. */
        int was_empty = !broker_granted[fork_slot].valid;
        pthread_mutex_unlock(&broker_lock);
        if (was_empty && local_dispatch_enabled) {
            broker_broadcast_hunger();
        }
        return;
    }
    broker_granted[fork_slot].valid = 1;
    broker_granted[fork_slot].entry = e;
    pthread_mutex_unlock(&broker_lock);

    uint8_t frame[1 + IPC_WORK_TAG_SIZE + sizeof(struct possibility_packet)];
    frame[0] = (uint8_t)IPC_MSG_WORK_GRANT;
    work_broker_tag_encode((int32_t)e.slot, e.seq, frame + 1, IPC_WORK_TAG_SIZE);
    memcpy(frame + 1 + IPC_WORK_TAG_SIZE, &e.pkt, sizeof e.pkt);
    if (broker_send_to_child(fork_slot, frame, sizeof frame) == 0) {
        __atomic_fetch_add(&broker_granted_total, 1ULL, __ATOMIC_RELAXED);
    } else {
        /* Datagramme perdu : on remet la possibilité en tampon plutôt que de la
           laisser dans une attribution que le fils ignore — sinon elle
           n'existerait plus nulle part et son offre ne serait jamais réglée. */
        pthread_mutex_lock(&broker_lock);
        broker_granted[fork_slot].valid = 0;
        put(&broker_queue, &e);
        pthread_mutex_unlock(&broker_lock);
    }
}

void work_broker_on_done(int fork_slot, const void *payload, size_t len)
{
    int32_t origin_slot = -1;
    uint32_t origin_seq = 0;
    if (fork_slot < 0 || fork_slot >= BROKER_MAX_SLOTS) {
        return;
    }
    if (work_broker_tag_decode(payload, len, &origin_slot, &origin_seq) != 0) {
        return;
    }
    pthread_mutex_lock(&broker_lock);
    int matches = broker_granted[fork_slot].valid
                  && broker_granted[fork_slot].entry.slot == origin_slot
                  && broker_granted[fork_slot].entry.seq == origin_seq;
    if (matches) {
        broker_granted[fork_slot].valid = 0;
    }
    pthread_mutex_unlock(&broker_lock);
    if (!matches) {
        /* Message périmé (attribution déjà réinjectée, ou rejeu) : décompter
           quand même retirerait une unité à une offre encore en vol. */
        return;
    }
    /* Sous-arbre prouvé mort : ce paquet n'a plus besoin d'exister nulle part,
       il est donc « disposé » au même titre qu'un paquet poussé au serveur. */
    broker_dispose_and_settle(origin_slot, origin_seq);
}

/**
 * @brief Réinjecte les attributions des fils qui ne tournent plus.
 *
 * Auto-réparation plutôt que rappel depuis le faucheur : un fils mort a son
 * entrée `forkId` vidée (`reap_dead_child_slots`), ce qui suffit à le détecter
 * ici. Couvre aussi le cas où il meurt sans être encore fauché.
 *
 * @param all 1 = tout réinjecter sans condition (arrêt du courtier).
 */
static void broker_reclaim_dead_grants(int all)
{
    pthread_mutex_lock(&broker_lock);
    for (int s = 0; s < BROKER_MAX_SLOTS; s++) {
        if (!broker_granted[s].valid) {
            continue;
        }
        int alive = (!all && forkId != NULL && s < NB_THREADS
                     && forkId[s] != NULL && forkId[s][0] != '\0');
        if (alive) {
            continue;
        }
        broker_granted[s].valid = 0;
        /* Remise en tampon avec un horodatage neuf : le sursis repart, un
           autre fils a donc sa chance avant que ça ne parte au serveur. */
        broker_granted[s].entry.enqueued_ms = broker_now_ms();
        if (!put(&broker_queue, &broker_granted[s].entry)) {
            log_error("courtier : réinjection impossible (mémoire) — offre %u du fils %d "
                      "restera non réglée\n",
                      broker_granted[s].entry.seq, broker_granted[s].entry.slot);
        }
    }
    pthread_mutex_unlock(&broker_lock);
}

int work_broker_relay_step(void)
{
    if (broker_client == NULL) {
        return 0;
    }
    int per_dgram = (int)ipc_work_offer_max_packets();
    int cap = per_dgram > 0 ? per_dgram * WORK_BROKER_OFFER_WINDOW : 32;

    broker_reclaim_dead_grants(0);

    int sent = 0;
    long long now = broker_now_ms();
    while (sent < cap) {
        /* La file est FIFO : si la tête n'a pas fini son sursis, aucune entrée
           ne l'a. On retire AVANT d'envoyer et on remet en queue si l'envoi
           échoue — la comptabilité étant par offre et non par ordre d'envoi,
           ce réordonnancement n'affecte pas le règlement. */
        broker_entry_t e;
        pthread_mutex_lock(&broker_lock);
        int have = 0;
        if (broker_queue_ready && broker_queue.start != NULL) {
            const broker_entry_t *head = broker_queue.start->value;
            if (broker_draining || now - head->enqueued_ms >= WORK_BROKER_HOLD_MS) {
                have = scroll_fifo(&broker_queue, &e);
            }
        }
        pthread_mutex_unlock(&broker_lock);
        if (!have) {
            break;
        }

        if (broker_send_one(&e.pkt) != 0) {
            pthread_mutex_lock(&broker_lock);
            put(&broker_queue, &e); /* réessayé plus tard, jamais perdu */
            pthread_mutex_unlock(&broker_lock);
            break;
        }
        sent++;
        broker_dispose_and_settle(e.slot, e.seq);
    }

    if (sent == 0) {
        return 0;
    }
    __atomic_fetch_add(&broker_relayed_total, (unsigned long long)sent, __ATOMIC_RELAXED);
    return sent;
}

/**
 * @brief Nombre d'attributions en cours (fils en train d'explorer).
 */
static int broker_grants_outstanding(void)
{
    int n = 0;
    pthread_mutex_lock(&broker_lock);
    for (int s = 0; s < BROKER_MAX_SLOTS; s++) {
        if (broker_granted[s].valid) {
            n++;
        }
    }
    pthread_mutex_unlock(&broker_lock);
    return n;
}

/**
 * @brief Acquitte les racines terminées, puis en réclame une au serveur.
 *
 * N'agit QUE lorsque le tampon est vide ET qu'aucun fils n'explore : à cet
 * instant précis, tout ce qui descendait des racines détenues a été soit
 * poussé au serveur (donc durable), soit prouvé mort. C'est exactement la
 * condition de l'invariant A4 — ni conservatrice, ni optimiste — et c'est
 * aussi le moment où le client n'a plus rien à faire, donc où il faut
 * redemander.
 *
 * @return 1 si une racine a été obtenue, 0 sinon.
 */
static int broker_ack_and_refill(void)
{
    if (broker_client == NULL || !local_dispatch_enabled) {
        return 0;
    }
    if (work_broker_pending_packets() > 0 || broker_grants_outstanding() > 0) {
        return 0;
    }
    /* Le sous-arbre local est épuisé : les racines détenues peuvent partir. */
    send_possibility_analysed(broker_client);

    int from_server = 0;
    array_possibility_packet *aposs = get_last_possibility(broker_client, 1, &from_server);
    if (aposs == NULL) {
        return 0;
    }
    int got = 0;
    if (aposs->size > 0) {
        long long now_ms = broker_now_ms();
        pthread_mutex_lock(&broker_lock);
        for (int i = 0; i < aposs->size; i++) {
            if (from_server) {
                /* Enregistrée comme « en analyse » : c'est ce que le futur
                   acquittement retirera côté serveur. */
                add_possibility_analysed(&aposs->possibilities[i], broker_client->id);
            }
            broker_entry_t e;
            /* Racine venue du serveur : aucune offre d'un fils à régler, d'où
               le slot sentinelle — `broker_dispose_and_settle` l'ignore. */
            e.slot = -1;
            e.seq = 0;
            e.enqueued_ms = now_ms;
            memcpy(&e.pkt, &aposs->possibilities[i], sizeof e.pkt);
            if (put(&broker_queue, &e)) {
                got++;
            }
        }
        pthread_mutex_unlock(&broker_lock);
    }
    free_array_possibility_packet(aposs);
    return got;
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
        /* En mode exclusif le parent est la SEULE source de travail du client :
           s'il ne réclame pas au serveur, personne ne le fait. */
        moved += broker_ack_and_refill();
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
            unsigned long long granted = work_broker_granted_total();
            if (total + granted != last_summary_total) {
                log_event("courtier : %llu possibilités redistribuées aux fils, "
                          "%llu relayées au serveur (%llu en tampon)",
                          granted, total, work_broker_pending_packets());
                last_summary_total = total + granted;
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
    /* Plus aucun fils ne rendra son attribution : on la reprend pour la
       pousser au serveur avec le reste. */
    broker_reclaim_dead_grants(1);
    broker_draining = 1;
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

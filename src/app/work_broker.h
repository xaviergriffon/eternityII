/**
 * @file  work_broker.h
 * @brief Courtier de travail du process client parent.
 *
 * Un fork de recherche qui délègue du travail l'envoie aujourd'hui au serveur
 * lui-même, par `put_to_server` : **un aller-retour TCP synchrone par
 * possibilité**, jusqu'à `max_stock_by_thread` d'affilée, exécutés par le thread
 * de recherche et sous `socket_mutex` — donc en bloquant aussi le thread
 * d'alimentation du même fork (cf. `docs/conception/dispatch_local_possibilites_forks.md`
 * §1.3).
 *
 * Ce module intercale le **processus parent** : le fils lui *offre* le lot par
 * IPC (`IPC_MSG_WORK_OFFER`, un datagramme, pas d'attente de réponse), le parent
 * l'empile et le pousse au serveur depuis sa propre connexion de travail. Le
 * thread de recherche ne paie plus que le coût d'un `sendto` local.
 *
 * Désactivé par défaut : sans `--local-dispatch`, aucun crochet n'est installé
 * et tout le chemin historique est repris à l'identique.
 *
 * ## Invariant d'acquittement
 *
 * Le tampon du courtier n'est **pas durable** : rien n'y survit à un arrêt
 * brutal du parent, alors que le stock serveur est sauvegardé. Un fils ne doit
 * donc pas acquitter la racine dont descend un travail encore en transit —
 * sinon cette racine quitte le pool analysé du serveur et la branche est perdue.
 *
 * Chaque offre porte un numéro (`seq`) strictement croissant par fils. Le
 * courtier renvoie (`IPC_MSG_WORK_SETTLED`) le plus grand `seq` de ce fils qu'il
 * a rendu **durable** (poussé au serveur et acquitté par lui). Tant que
 * `last_settled_seq < last_offer_seq`, `work_broker_ack_allowed` renvoie 0 et
 * `send_possibility_analysed` ne fait rien.
 *
 * Ne rien acquitter est le repli SÛR, jamais une fuite : un client **vivant**
 * n'est jamais réclamé par le serveur, quelle que soit la durée de l'analyse
 * (`datamanager_reclaim_expired_leases`) ; et s'il meurt, la réclamation du bail
 * réinjecte la racine et purge ses descendants — exactement ce qu'il faut.
 *
 * ## Contrôle de flux
 *
 * Le tampon du parent est borné **par construction**, pas par un plafond
 * contrôlé à l'insertion : un fils ne garde jamais plus de
 * `WORK_BROKER_OFFER_WINDOW` offres non réglées. Au-delà il cesse d'offrir et
 * retombe sur l'envoi direct au serveur. Le parent ne peut donc pas héberger
 * plus de `nb_forks × WORK_BROKER_OFFER_WINDOW × ipc_work_offer_max_packets()`
 * paquets — aucune décision de rejet asynchrone à faire remonter au fils, ce
 * qu'un datagramme UDP ne permettrait de toute façon pas.
 *
 * ## Écart assumé avec l'arbitrage A1 du document de conception
 *
 * A1 annonçait réutiliser les pools `datamanager` du parent (« `put_to_local`,
 * le plafond RAM et `stockDistribution` viennent gratuitement »). Le courtier
 * utilise en fait une file privée : les pools du parent sont ceux que
 * `backup`/`restore`/`stockDistribution` manipulent, et y verser un tampon de
 * transit ferait écrire sur disque, sous le nom de « stock », de la donnée qui
 * n'en est pas. La file privée porte en plus l'origine (`slot`, `seq`) de chaque
 * paquet, dont le règlement a besoin et qu'un pool ne transporte pas.
 */
#ifndef work_broker_h
#define work_broker_h

#include <stddef.h>
#include <stdint.h>

#include "core/possibility.h"

/**
 * @brief Nombre maximal d'offres non réglées qu'un fils garde en vol.
 *
 * Fenêtre de contrôle de flux (cf. en-tête). Assez large pour absorber une
 * délégation complète sans repli, assez étroite pour borner le tampon du
 * parent : à 6 paquets par offre sur le puzzle 256, 8 offres ≈ 48 paquets par
 * fils, soit ≈ 27 Ko.
 */
#define WORK_BROKER_OFFER_WINDOW 8

/**
 * @brief Délai minimal (ms) avant qu'une possibilité en tampon parte au serveur.
 *
 * Sans ce sursis, le thread de relais viderait le tampon aussitôt et il n'y
 * aurait jamais rien à redistribuer : les fils au repos ne verraient que du
 * vide. C'est le réglage de « péremption » de l'arbitrage A3 — passé ce délai,
 * ce qu'aucun fils n'a réclamé repart au serveur plutôt que de dormir en RAM.
 */
#define WORK_BROKER_HOLD_MS 200

/* ------------------------------------------------------------------ */
/* Comptabilité des offres — le cœur de l'invariant d'acquittement.    */
/* ------------------------------------------------------------------ */

/**
 * @brief Suivi d'UNE offre : combien de ses paquets restent à disposer.
 *
 * Un paquet est « disposé » quand il est devenu durable (poussé au serveur) OU
 * qu'un fils a prouvé son sous-arbre mort (`IPC_MSG_WORK_DONE`). Les deux
 * comptent : dans un cas le travail est chez le serveur, dans l'autre il n'y a
 * plus de travail du tout.
 */
typedef struct {
    uint32_t seq;   /**< numéro de l'offre */
    int remaining;  /**< paquets pas encore disposés */
    int used;       /**< 0 = entrée libre */
} work_broker_offer_acc_t;

/**
 * @brief Enregistre une offre de `count` paquets dans l'anneau d'un fils.
 *
 * @return 0 si l'offre est suivie, -1 si l'anneau est plein — ce qui signifie
 *         que le fils a dépassé sa fenêtre, donc un bogue : l'appelant doit
 *         alors REFUSER l'offre plutôt que de la suivre à moitié.
 */
int work_broker_acc_add(work_broker_offer_acc_t *ring, int n, uint32_t seq, int count);

/**
 * @brief Décompte un paquet disposé de l'offre `seq`.
 * @return 0 si l'offre était suivie, -1 si elle est inconnue (message périmé).
 */
int work_broker_acc_dispose(work_broker_offer_acc_t *ring, int n, uint32_t seq);

/**
 * @brief Fait avancer le `seq` réglé aussi loin que possible.
 *
 * Avance de `settled + 1` en `settled + 1` tant que l'offre correspondante est
 * entièrement disposée, en libérant les entrées au passage. **Ne saute jamais
 * une offre incomplète** : régler `n+1` alors que `n` est encore en vol
 * laisserait le fils acquitter une racine dont du travail circule encore.
 *
 * @return Le nouveau `seq` réglé (égal à `settled` si rien n'a pu avancer).
 */
uint32_t work_broker_acc_settle(work_broker_offer_acc_t *ring, int n, uint32_t settled);

/* ------------------------------------------------------------------ */
/* Fonctions pures — cadrage et politique, testables sans socket.      */
/* ------------------------------------------------------------------ */

/**
 * @brief Sérialise une offre : `seq`, `count`, puis `count` paquets.
 *
 * @param seq    Numéro d'offre du fils.
 * @param pkts   Paquets à cadrer.
 * @param count  Nombre de paquets (doit tenir dans un datagramme).
 * @param buf    Tampon de sortie, sans l'octet de type.
 * @param bufsz  Capacité de `buf`.
 * @return       Nombre d'octets écrits, ou -1 si `bufsz` est insuffisant.
 */
int32_t work_broker_offer_encode(uint32_t seq, const struct possibility_packet *pkts,
                                 int count, void *buf, size_t bufsz);

/**
 * @brief Décode une offre produite par `work_broker_offer_encode`.
 *
 * @param buf        Charge utile reçue (sans l'octet de type).
 * @param len        Longueur reçue.
 * @param out_seq    Reçoit le numéro d'offre.
 * @param out_pkts   Reçoit un pointeur DANS `buf` sur le premier paquet.
 * @param out_count  Reçoit le nombre de paquets.
 * @return           0 si l'offre est bien formée, -1 sinon (trop courte,
 *                   `count` négatif, ou longueur incohérente avec `count`).
 */
int work_broker_offer_decode(const void *buf, size_t len, uint32_t *out_seq,
                             const struct possibility_packet **out_pkts, int *out_count);

/**
 * @brief Nouvelle valeur du `seq` réglé, sachant celle connue et celle reçue.
 *
 * Un règlement ne recule JAMAIS : un datagramme réordonné annonçant un `seq`
 * plus ancien ne doit pas rouvrir une fenêtre déjà refermée, ce qui laisserait
 * un fils acquitter une racine dont du travail est encore en transit.
 * Comparaison sur la distance non signée, donc juste au rebouclage de `seq`.
 *
 * @param known    `seq` réglé déjà connu du fils.
 * @param incoming `seq` annoncé par le courtier.
 * @return         `incoming` s'il est plus récent, `known` sinon.
 */
uint32_t work_broker_settled_advance(uint32_t known, uint32_t incoming);

/**
 * @brief La fenêtre autorise-t-elle une offre de plus ?
 *
 * Fonction pure, extraite pour être testable aux bornes : arithmétique non
 * signée, donc correcte même si `last_offer` a rebouclé (2^32 offres).
 *
 * @param last_offer   Plus grand `seq` émis par ce fils (0 = aucune offre).
 * @param last_settled Plus grand `seq` réglé par le courtier.
 * @param window       Fenêtre (`WORK_BROKER_OFFER_WINDOW`).
 * @return             1 si une offre de plus est permise, 0 sinon.
 */
int work_broker_window_allows(uint32_t last_offer, uint32_t last_settled, uint32_t window);

/* ------------------------------------------------------------------ */
/* Côté fils (fork de recherche)                                       */
/* ------------------------------------------------------------------ */

/**
 * @brief Cadre le couple (origin_slot, origin_seq) commun à GRANT et DONE.
 * @return `IPC_WORK_TAG_SIZE`, ou -1 si `bufsz` est insuffisant.
 */
int work_broker_tag_encode(int32_t slot, uint32_t seq, void *buf, size_t bufsz);

/**
 * @brief Décode le couple cadré par `work_broker_tag_encode`.
 * @return 0 si bien formé, -1 sinon.
 */
int work_broker_tag_decode(const void *buf, size_t len, int32_t *out_slot, uint32_t *out_seq);

/**
 * @brief Installe les crochets `datamanager` de ce fils (offre + verrou
 *        d'acquittement). À appeler dans l'enfant, après le `fork()`, et
 *        seulement si `--local-dispatch` est actif.
 */
void work_broker_child_install(void);

/**
 * @brief Traite un `IPC_MSG_WORK_SETTLED` reçu par ce fils.
 *
 * @param payload Charge utile (un `int32` : le `seq` réglé).
 * @param len     Longueur de la charge utile.
 */
void work_broker_child_on_settled(const void *payload, size_t len);

/**
 * @brief L'acquittement des possibilités analysées est-il autorisé ?
 *
 * Crochet installé sur `datamanager_set_ack_gate`. Exposé pour les tests.
 *
 * @return 1 si tout le travail offert est durable, 0 pour différer.
 */
int work_broker_ack_allowed(void);

/**
 * @brief Traite un `IPC_MSG_WORK_GRANT` reçu par ce fils : mémorise la
 *        possibilité attribuée et l'origine à régler.
 *
 * Une seule attribution est détenue à la fois (un fork de recherche n'étudie
 * qu'une racine à la fois) : un second GRANT arrivé alors que le précédent
 * n'est pas terminé est ignoré, le courtier n'en émet pas.
 */
void work_broker_child_on_grant(const void *payload, size_t len);

/**
 * @brief Demande du travail au courtier (datagramme, sans attente).
 *
 * Sans effet si ce fils détient déjà une attribution non terminée.
 */
void work_broker_child_request_work(void);

/**
 * @brief Prélève l'attribution reçue, s'il y en a une.
 *
 * @return Un tableau d'UN paquet, à libérer par `free_array_possibility_packet`,
 *         ou NULL si rien n'a été attribué. Le paquet n'est PAS acquitté auprès
 *         du serveur (il ne lui a jamais été soumis) : l'appelant ne doit pas
 *         l'enregistrer dans le pool analysé.
 */
array_possibility_packet *work_broker_child_take_grant(void);

/**
 * @brief Signale au courtier que l'attribution en cours est entièrement
 *        explorée. Sans effet si ce fils n'en détient aucune.
 */
void work_broker_child_report_done(void);

/**
 * @brief Réinitialise l'état du fils (numéros d'offre, attribution). Réservé aux tests.
 */
void work_broker_child_reset(void);

/* ------------------------------------------------------------------ */
/* Côté parent (courtier)                                              */
/* ------------------------------------------------------------------ */

/**
 * @brief Démarre le courtier : contexte de connexion au serveur + thread de
 *        relais. Sans effet si `--local-dispatch` n'est pas actif.
 *
 * @param server_host Hôte du serveur (celui des forks).
 * @return            0 si le courtier tourne, -1 sinon (le client fonctionne
 *                    alors sans courtier, chaque fork envoyant lui-même).
 */
int work_broker_parent_start(const char *server_host);

/**
 * @brief Arrête le thread de relais et libère le courtier. Idempotent.
 *
 * Ne jette rien : ce qui reste en tampon est poussé au serveur avant le
 * retour, dans la limite de ce que la connexion accepte. Ce qui ne passe pas
 * n'est de toute façon jamais acquitté par son fils (invariant ci-dessus).
 */
void work_broker_parent_stop(void);

/**
 * @brief Encaisse une offre reçue d'un fils (appelé par `server_tcp`).
 *
 * @param fork_slot Indice du fils dans `forkId[]`, tel que résolu par
 *                  `find_fork_index` (< 0 = expéditeur inconnu, offre ignorée).
 * @param payload   Charge utile du datagramme (sans l'octet de type).
 * @param len       Longueur de la charge utile.
 */
void work_broker_on_offer(int fork_slot, const void *payload, size_t len);

/**
 * @brief Sert une demande de travail d'un fils (appelé par `server_tcp`).
 */
void work_broker_on_request(int fork_slot);

/**
 * @brief Encaisse un `IPC_MSG_WORK_DONE` d'un fils (appelé par `server_tcp`).
 */
void work_broker_on_done(int fork_slot, const void *payload, size_t len);

/**
 * @brief Un tour de relais : draine le tampon vers le serveur, puis règle les
 *        fils dont le travail est devenu durable.
 *
 * Extrait de la boucle du thread pour être testable hors thread.
 *
 * @return Nombre de paquets effectivement poussés au serveur.
 */
int work_broker_relay_step(void);

/**
 * @brief Nombre total de possibilités relayées au serveur depuis le démarrage.
 *
 * Seul moyen, pour un opérateur, de distinguer « le courtier travaille » de
 * « tout retombe sur l'envoi direct » — de l'extérieur les deux se
 * ressemblent. Journalisé périodiquement par le thread de relais.
 */
unsigned long long work_broker_relayed_total(void);

/**
 * @brief Nombre total de possibilités ATTRIBUÉES à un fils depuis le démarrage.
 *
 * C'est la mesure de la redistribution elle-même : le compteur de relais ne la
 * voit pas (une possibilité attribuée n'est jamais poussée au serveur). Sans
 * ces deux nombres côte à côte, « le courtier redistribue » et « le courtier ne
 * fait que relayer » sont indiscernables de l'extérieur.
 */
unsigned long long work_broker_granted_total(void);

/**
 * @brief Nombre de paquets actuellement en tampon (diagnostic et tests).
 */
unsigned long long work_broker_pending_packets(void);

/**
 * @brief Réinitialise l'état du parent (tampon, numéros réglés). Réservé aux tests.
 */
void work_broker_parent_reset(void);

#endif /* work_broker_h */

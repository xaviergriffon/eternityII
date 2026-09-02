/**
 * @file etii_server.h
 * @brief Méthodes pour un serveur EternityII
 */
#ifndef etii_server_h
#define etii_server_h

#include <pthread.h>
#include <sys/times.h>
#include <stdint.h>
#include "core/part.h"
#include "core/possibility.h"
#include "app/app_static_variables.h"

/**
 * @brief Contexte d'un thread de communication serveur.
 *
 * Un slot par thread de communication (un par connexion client TCP potentielle).
 * `exist == 0` → slot libre ; `socket_id == -1` → thread en attente de client.
 */
typedef struct
{
    int exist;
    pthread_t *tid;
    int socket_id;
    map_big_array *map_part;
    int compteur;
    struct tms start_socket;
    struct array_part *rotate_parts;
    /// Adresse IP du pair de la connexion TCP courante (`accept()`, formatée par
    /// `inet_ntop`), affectée avec `socket_id` dans `try_assign_client_slot`.
    /// Vide (`""`) tant qu'aucun client n'a jamais occupé ce slot.
    char peer_ip[PEER_IP_MAX_LEN];
    /// Identité déclarée par le fork sur CETTE connexion de travail (v12,
    /// INST_CLIENT_HELLO), valide seulement si `has_identity`. Déclarative,
    /// jamais vérifiée (à la différence de `peer_ip`).
    client_identity_t identity;
    /// 1 si `identity` a été renseignée par un INST_CLIENT_HELLO reçu sur
    /// cette connexion, 0 sinon (client trop ancien, ou hello pas encore
    /// reçu). Remis à 0 par `try_assign_client_slot` à chaque réutilisation
    /// du slot, comme `peer_ip`.
    int has_identity;
} client_t;

/**
 * @brief État de la porte d'autobackup indépendante pour UN artefact,
 *        consultée/mise à jour par `should_autobackup`.
 */
typedef struct
{
    /// Nombre de tours (10s) écoulés depuis la dernière écriture, plafonné à 6.
    int lastBack;
    /// Compteur de mutations vu à la dernière écriture (comparé par égalité).
    unsigned long long lastUpdates;
} autobackup_gate_t;

/**
 * @brief Regroupe les quatre portes d'autobackup indépendantes de
 *        `check_server_step` : stock (pools non vérifié + vérifié),
 *        pool analysé, meilleur plateau connu, registre des clients connus.
 *        Chaque artefact n'est réécrit que si SON compteur de mutations a
 *        bougé depuis SA dernière écriture — `consistent_backup` reste
 *        appelée en un seul appel couvrant stock+analysé dès que L'UN DES
 *        DEUX a une mutation en attente (cohérence à l'instant T préservée,
 *        `best_board`/`known_clients` sont deux portes
 *        entièrement indépendantes l'une de l'autre et du stock.
 */
typedef struct
{
    autobackup_gate_t stock;
    autobackup_gate_t analysed;
    autobackup_gate_t best_board;
    autobackup_gate_t known_clients;
} autobackup_state_t;

/**
 * @brief Décision produite par `compute_desired_role_mix` : sens dans lequel
 *        faire évoluer le dosage recherche/contrôle diffusé au parc, jamais
 *        une valeur absolue.
 *
 * Un pas de ±1 (jamais un saut direct vers une cible calculée) est ce qui
 * rend l'hystérésis triviale à appliquer côté appelant : chaque changement
 * effectif coûte un `stopForks`+re-fork chez le client visé, donc
 * chaque incrément doit rester une décision consciente, jamais un rattrapage
 * brutal d'un déséquilibre mesuré une seule fois.
 */
typedef enum {
    ROLE_MIX_DECREASE_PRUNE = -1,
    ROLE_MIX_KEEP = 0,
    ROLE_MIX_INCREASE_PRUNE = 1,
} role_mix_decision_t;

/**
 * @brief État persistant d'un tour à l'autre de la politique automatique de
 *        dosage recherche/contrôle — mêmes conventions in/out que
 *        `autobackup_state_t` : paramètre explicite plutôt que statique
 *        cachée, pour rester testable sans dépendre de l'ordre d'exécution
 *        des tests.
 */
typedef struct {
    /// Valeur cumulée de `server_search_starved`/`server_prune_starved` au
    /// tour précédent, pour calculer un delta (compteurs cumulatifs, jamais
    /// remis à zéro).
    unsigned long long last_search_starved;
    unsigned long long last_prune_starved;
    /// Dernier dosage (`pruner_forks`) effectivement diffusé au parc.
    int current_dosage;
    /// Nombre de tours (10s) écoulés depuis le dernier changement appliqué —
    /// délai minimal avant d'accepter un nouveau changement (hystérésis).
    int ticks_since_change;
} auto_role_mix_state_t;

/**
 * @brief Initialise et démarre le serveur EternityII.
 *
 * Charge les pièces depuis `file`, construit la map de lookup, génère le paquet
 * genèse et lance le thread de statistiques (`check_server`) puis la boucle
 * principale d'acceptation des connexions TCP clientes.
 *
 * @param file Chemin du fichier CSV de définition des pièces.
 */
void runserver(const char* file);

/**
 * @brief Journalise dans `events.log` un instantané de la configuration
 *        effective/de l'environnement du serveur (jamais sur la console) —
 *        voir la documentation détaillée dans etii_server.c.
 *
 * Extraite de `runserver` pour être testable sans socket ni boucle
 * `accept()` réelle : ne fait qu'un appel à `log_file` à partir de globales
 * déjà résolues par le CLI.
 *
 * @param file Chemin du fichier de pièces effectivement utilisé.
 */
void log_server_startup_diagnostics(const char *file);

/**
 * @brief Thread de statistiques du serveur.
 *
 * Toutes les 10 secondes, collecte le stock de chaque file, les possibilités
 * en cours d'analyse, le débit global et le meilleur résultat. Déclenche
 * automatiquement une sauvegarde (`temp.back`) toutes les minutes si le stock
 * a évolué depuis le dernier backup.
 *
 * @param param Non utilisé.
 * @return      NULL (boucle infinie).
 */
void *check_server(void *param);

/**
 * @brief Un tour de la boucle de `check_server` (rapport + autobackup), sans le
 *        `sleep` de fin de tour.
 *
 * Extrait pour être testable hors thread. Voir etii_server.c pour le détail
 * des paramètres in/out (état persistant d'un tour à l'autre).
 *
 * @param role_mix_state État de la politique automatique de dosage — ignoré
 *                        (aucune lecture, aucune écriture) si
 *                        `auto_roles_requested` est faux ou si ce pointeur
 *                        est NULL.
 */
void check_server_step(unsigned long long *lastactive, autobackup_state_t *backup_state,
                       int *last_record, int sleep_time, auto_role_mix_state_t *role_mix_state);

/**
 * @brief Borne le nombre de possibilités demandées en lot par un pruner.
 *
 * Fonction pure : garantit `1 ≤ result ≤ PRUNER_BATCH_MAX`.
 *
 * @param requested Valeur brute reçue du client.
 * @return          Valeur bornée.
 */
int32_t clamp_pruner_batch(int32_t requested);

/**
 * @brief Calcule la « faim » du serveur : le nombre de possibilités qu'il
 *        souhaiterait recevoir des clients occupés (réponse à INST_NEED_WORK).
 *
 * Fonction pure : le serveur vise un stock d'au moins
 * `SERVER_HUNGER_PER_CLIENT × active_clients` (chaque session connectée doit
 * pouvoir être servie au prochain GET, avec marge). La faim est le manque par
 * rapport à cette cible, plafonné à `SERVER_HUNGER_CAP` (tous les threads
 * occupés du réseau peuvent répondre en même temps).
 *
 * @param stock          Taille du stock distribuable (non vérifié + vérifié).
 * @param active_clients Nombre de sessions client connectées.
 * @return               Faim (0 = stock suffisant), toujours dans [0, SERVER_HUNGER_CAP].
 */
int32_t compute_server_hunger(unsigned long long stock, int active_clients);

/**
 * @brief Compteurs de service à vide : combien de fois
 *        `communicate_with_client_step` a répondu `K = 0` à un `INST_GET`
 *        (`server_search_starved`) ou à un `INST_GET_TO_CHECK[_BATCH]`
 *        (`server_prune_starved`).
 *
 * Les deux handlers de service étant déjà distincts, la famine se ventile
 * naturellement par rôle. Incrémentés via `__atomic_fetch_add` relâché :
 * plusieurs threads serveur incrémentent concurremment, une statistique
 * d'affichage tolère la rare imprécision plutôt que de payer un verrou.
 * Cumulatifs, jamais remis à zéro.
 */
extern unsigned long long server_search_starved;
extern unsigned long long server_prune_starved;

/**
 * @brief Calcule le sens d'ajustement du dosage recherche/contrôle à partir
 *        des signaux de besoin déjà mesurés côté serveur.
 *
 * Fonction PURE — deltas de famine et ratio de pression RAM sont calculés
 * par l'appelant (`check_server_step`).
 *
 * Priorité des signaux, du plus urgent au plus indicatif : (1) 0 chercheur
 * avec un pruner présent = violation d'invariant, corrigée en priorité ;
 * (2) parc vide : rien à décider ; (3) famine (chercheur ou pruner) : réduire
 * le dosage, la recherche étant le seul rôle qui régénère du stock ;
 * (4) garde-fou : jamais d'augmentation avec un seul chercheur ou moins ;
 * (5) pression RAM haute : plus de vérification pour éliminer le mort plus
 * vite ; (6) ratio stock non-vérifié/vérifié déséquilibré dans un sens ou
 * l'autre.
 *
 * Seuils choisis comme point de départ raisonnable (hystérésis et délai
 * minimal restent la vraie garantie de stabilité, cf. `check_server_step`) —
 * à remesurer une fois `--auto-roles` exercé en conditions réelles.
 *
 * @param unchecked_stock       Σ taille des files non vérifiées (travail
 *                              disponible pour un pruner).
 * @param checked_stock         Σ taille des files vérifiées (travail
 *                              disponible pour un chercheur).
 * @param ram_pressure_high     1 si `resident/limite ≥ STOCK_SPILL_HIGH_PERCENT`
 *                              (0 si le plafond RAM est désactivé — pas de
 *                              notion de pression sans plafond).
 * @param search_starved_delta  Δ `server_search_starved` depuis le tour précédent.
 * @param prune_starved_delta   Δ `server_prune_starved` depuis le tour précédent.
 * @param nb_search             Σ `nb_forks` des sessions en rôle recherche
 *                              (`control_registry_count_role_forks`, pas un
 *                              compte de sessions — avec une seule machine
 *                              connectée, un compte de sessions vaudrait
 *                              toujours au plus 1, déclenchant à tort le
 *                              garde-fou ci-dessous quel que soit son nombre
 *                              réel de forks).
 * @param nb_prune              Σ `nb_forks` des sessions en rôle contrôle,
 *                              même remarque.
 * @return                      Le sens d'ajustement (jamais une cible absolue).
 */
role_mix_decision_t compute_desired_role_mix(unsigned long long unchecked_stock,
                                              unsigned long long checked_stock,
                                              int ram_pressure_high,
                                              unsigned long long search_starved_delta,
                                              unsigned long long prune_starved_delta,
                                              int nb_search,
                                              int nb_prune);

/**
 * @brief Cherche un slot de thread serveur occupé mais en attente de client.
 *
 * Un slot « libre » vérifie `exist != 0 && socket_id == -1`.
 *
 * @param threads Tableau des contextes de threads serveur.
 * @param nb      Nombre de slots dans le tableau.
 * @return        Indice du premier slot libre, ou -1 si aucun.
 */
int find_free_thread_slot(client_t *threads, int nb);

/**
 * @brief Cherche un slot de thread serveur non encore créé (`exist == 0`).
 *
 * @param threads Tableau des contextes de threads serveur.
 * @param nb      Nombre de slots dans le tableau.
 * @return        Indice du premier slot vide, ou -1 si aucun.
 */
int find_empty_thread_slot(client_t *threads, int nb);

/**
 * @brief Compte les threads serveur actuellement connectés à un client.
 *
 * Parcourt les `NB_THREADS` premiers slots (la fonction lit la globale, sans
 * paramètre de taille) et compte ceux dont `socket_id != -1` (un slot connecté),
 * indépendamment de `exist`. Renvoie 0 si `thread_params` est NULL.
 *
 * @param thread_params Tableau des contextes de threads serveur (≥ NB_THREADS).
 * @return              Nombre de slots connectés.
 */
int get_active_threads(client_t *thread_params);

/**
 * @brief Enrobe `get_active_threads(thread_params)` sur la globale du module,
 *        pour un appelant externe (ex. `src/net/http_server.c`) qui n'a pas à
 *        connaître/manipuler directement le pool de threads serveur.
 *
 * @return Nombre de slots connectés (0 avant `init_server_thread_pool`).
 */
int server_active_client_count(void);

/**
 * @brief Un fork de travail actuellement connecté, avec son rôle déclaré.
 */
typedef struct {
    int32_t fork_seq;
    uint8_t mode; /**< CLIENT_MODE_SEARCH/PRUNER/GPU_PRUNER (net/client_identity.h). */
} client_work_fork_t;

/**
 * @brief Indique si ce client a au moins une connexion de travail ouverte.
 *
 * Complète `control_registry_has_active_client` pour juger la vivacité d'un
 * client avant de réclamer son bail : le canal de contrôle d'un client qui
 * s'arrête se ferme avant que ses forks de travail aient fini de vider leur
 * file, si bien que le seul canal de contrôle déclare mort un client dont
 * les forks travaillent encore.
 *
 * Un slot dont la connexion est fermée (`socket_id == -1`) ne compte pas,
 * même s'il porte encore une identité — `has_identity` n'est remis à zéro
 * qu'à la réutilisation du slot, jamais à la déconnexion. Se fier à la
 * seule identité ferait vivre un client indéfiniment et le bail ne serait
 * plus jamais réclamé : le défaut exactement inverse.
 *
 * @return 1 si au moins une connexion de travail ouverte lui appartient, 0 sinon.
 */
int client_has_open_work_connection(const uint8_t client_uid[CLIENT_UID_BYTES]);

/**
 * @brief Liste, pour un `client_uid` donné, le rôle déclaré (`mode`) de
 *        chaque fork de travail actuellement connecté — pas le mode unique,
 *        par session, du canal de contrôle, qui ne reflète que le mode de
 *        lancement du process et jamais le détail d'un dosage mixte.
 *
 * Balaie `thread_params[0..NB_THREADS[` sans verrou : lecture best-effort,
 * une rare incohérence transitoire est acceptable pour une commande de
 * diagnostic.
 *
 * @return Nombre d'entrées écrites (0 si aucun fork de ce client n'est
 *         actuellement connecté, `out == NULL`, ou `max <= 0`).
 */
int client_work_fork_roles(const uint8_t client_uid[CLIENT_UID_BYTES],
                           client_work_fork_t *out, int max);

/**
 * @brief Table complète des pièces + toutes leurs rotations, construite par
 *        `runserver` et déjà partagée avec chaque `client_t.rotate_parts`
 *        pour sérialiser les solutions en CSV. Exposée en globale pour que
 *        `http_server.c` puisse décoder `possibility_packet.grid[x][y]` en
 *        pièce réelle pour `GET /api/v1/best-board`, sans dupliquer la
 *        lecture du CSV.
 *
 *        NULL avant que `runserver` ait construit la table (mode
 *        client/test) et remis à NULL après sa libération en fin de
 *        `runserver` — un appelant doit vérifier NULL avant de déréférencer.
 */
extern struct array_part *g_server_rotate_parts;

/**
 * @brief Construit le tableau « File queues » du rapport serveur (une ligne par
 *        file + Total) dans une chaîne allouée ; renvoie les totaux par pool via
 *        les out-params (NULL accepté). À libérer par l'appelant.
 */
char *build_file_queues_table(unsigned long long *out_unchecked,
                              unsigned long long *out_checked,
                              unsigned long long *out_analysed);

/**
 * @brief File du pool analysé assignée à CETTE connexion serveur
 *        (répartition de charge ADD/GET par connexion, cf. datamanager.c).
 *
 * `client->compteur` (slot de thread serveur, stable pour la durée de la
 * connexion) modulo `nb_file_possibility` : tous les GET et tous les ACK
 * d'une même connexion tombent sur la même file, ce qui rend le retrait
 * (`remove_possibility_analysed`, paramètre `preferred_file`) direct plutôt
 * que de balayer toutes les files.
 *
 * @param client Connexion concernée ; `NULL` toléré (retourne -1).
 * @return       Indice de file dans `[0, nb_file_possibility)`, ou -1
 *               (« pas de préférence », comportement historique) si
 *               `client == NULL` ou si `nb_file_possibility <= 0`.
 */
int server_analysed_file_hint(client_t *client);

/**
 * @brief Enregistre une possibilité servie comme « en cours d'analyse »,
 *        attribuée au client courant si son identité est connue.
 *
 * Extrait des trois points de service (`INST_GET`/`INST_GET_TO_CHECK[_BATCH]`)
 * pour être testable hors thread (comme `communicate_with_client_step`) et
 * pour n'écrire cette décision qu'à un seul endroit.
 *
 * Peut échouer (pool analysé intégralement verrouillé par une maintenance en
 * cours, au-delà d'un délai borné) — l'appelant NE DOIT PAS servir cette possibilité au
 * client dans ce cas, sous peine de l'échapper au bail et à
 * `requeue_last_sent_possibility`.
 *
 * @param client      Contexte du thread serveur (identité déclarée si connue).
 * @param possibility Paquet tout juste extrait du stock et envoyé au client.
 * @return            0 si enregistrée, -1 sinon (rien n'est enregistré).
 */
int record_possibility_analysed_for_client(client_t *client, struct possibility_packet *possibility);

/**
 * @brief Renvoie au stock local les possibilités servies au client mais
 *        jamais acquittées, à la déconnexion (propre ou brutale).
 *
 * Extrait du bloc de fin de `communicate_with_client` pour être testable
 * hors de la boucle d'événements. Pour chaque possibilité de `lastSent`
 * encore présente dans `file_analysed`, elle est retirée de l'« en analyse »
 * et réinjectée dans le stock. Une possibilité déjà acquittée (absence
 * confirmée) n'est pas réinjectée ; une absence non confirmée (budget borné
 * épuisé) l'est quand même, jamais perdue dans le doute. NULL accepté pour
 * `lastSent`/`client`. Ne libère pas `lastSent`.
 *
 * `client` sert uniquement à vérifier si le client reste vivant (canal de
 * contrôle toujours enregistré) : si oui, rien n'est remis au stock — même
 * critère de vivacité que le bail d'expiration.
 *
 * @param lastSent Dernier lot de possibilités envoyé au client (peut être NULL).
 * @param client   Client dont la connexion de travail se termine (peut être NULL).
 */
void requeue_last_sent_possibility(array_possibility_packet *lastSent, client_t *client);

/**
 * @brief Traite une instruction reçue d'un client (un tour de la boucle de
 *        `communicate_with_client`).
 *
 * Extrait du corps du `while` pour être testable hors thread (le socket peut
 * être un socketpair). `*lastSent` mémorise le dernier lot servi à rendre au
 * stock à la déconnexion ; `*version_supported` porte l'état du handshake
 * d'un tour à l'autre.
 *
 * `out_control_session_index` est un pur out-param : écrit -1 sauf quand
 * l'instruction est `INST_CONTROL_HELLO` et que l'enregistrement dans
 * `control_registry` réussit, auquel cas il porte l'indice de session
 * obtenu. L'appelant doit tester cette valeur après chaque appel : un
 * indice ≥ 0 signifie que la session vient de basculer en canal de contrôle
 * et que l'appelant doit quitter la boucle normale pour entrer dans
 * `run_control_session`, qui gère alors seule l'épilogue (fermeture socket
 * incluse), sans jamais repasser par la boucle habituelle (piège du
 * double-close à éviter).
 *
 * @return 1 pour poursuivre la boucle, 0 pour s'arrêter.
 */
int communicate_with_client_step(client_t *client, int8_t instruction,
                                 array_possibility_packet **lastSent,
                                 int *version_supported,
                                 int *out_control_session_index);

/**
 * @brief Boucle de session de contrôle : le serveur devient l'initiateur des
 *        échanges (`CTRL_PING`/`CTRL_GET_STATS`/`CTRL_COMMAND`) sur une
 *        session déjà enregistrée dans `control_registry`.
 *
 * Tant que `request != REQUEST_STOP` et que la session reste vivante,
 * répète `control_session_step`. En sortie de boucle, désenregistre la
 * session, ferme le socket (même épilogue que `communicate_with_client`) et
 * journalise. Ne fait jamais double emploi avec l'épilogue de
 * `communicate_with_client` : c'est cette fonction, et uniquement elle, qui
 * ferme le socket dans ce cas.
 */
void run_control_session(client_t *client, int session_index);

/**
 * @brief Un tour de la boucle de session de contrôle (extrait de
 *        `run_control_session` pour être testable par socketpair, même patron
 *        que `communicate_with_client_step`).
 *
 * 1. Attend une commande postée pour `session_index` via
 *    `control_registry_wait_command` (borné à `timeout_ms`).
 * 2. Si une commande est dépilée : `CTRL_COMMAND` l'envoie au client et
 *    attend `CTRL_RESULT` (journalisé) ; `CTRL_GET_STATS` envoie la demande et
 *    attend `CTRL_STATS` (journalisé, décodé via `control_stats_decode`). Tout
 *    échec d'envoi/réception (codec renvoie -1) est traité comme une session
 *    morte.
 * 3. Sinon (timeout) : envoie `CTRL_PING`, attend `CTRL_ACK` — échec = session
 *    morte.
 *
 * @param client        Contexte du thread (socket_id).
 * @param session_index Indice de la session dans `control_registry`.
 * @param timeout_ms     Délai d'attente d'une commande postée (cf.
 *                      `run_control_session` pour le calcul à partir de
 *                      `tcp_timeout`).
 * @return 1 pour poursuivre la boucle, 0 pour arrêter (session morte).
 */
int control_session_step(client_t *client, int session_index, int timeout_ms);

/**
 * @brief Libellé (haut niveau) de la cause de fin de session d'un client.
 *
 * Fonction pure : classe la dernière instruction observée par la boucle de
 * `communicate_with_client` (INST_END → « fin de session », -1 → « connexion
 * perdue », sinon → « protocole interrompu ») pour le flux d'évènements.
 *
 * @param last_instruction  Dernière instruction reçue par la boucle.
 * @return Chaîne statique décrivant la cause.
 */
const char *client_disconnect_reason(int8_t last_instruction);

/**
 * @brief Décide si la sauvegarde automatique périodique doit avoir lieu ce tour
 *        (logique de cadence extraite de la boucle `check_server`).
 *
 * Fonction pure (aucune I/O). La sauvegarde n'a lieu que tous les 6 tours ET
 * uniquement si le total de mises à jour des files a changé depuis le dernier
 * backup (inutile de resauvegarder un stock figé). Met à jour l'état en place :
 * au déclenchement, `*lastBack` est remis à 0 et `*lastBackupUpdates` mémorise
 * `currentUpdates` ; sinon, tant que la fenêtre n'est pas pleine, `*lastBack`
 * est incrémenté.
 *
 * @param lastBack          In/out : nombre de tours écoulés depuis le dernier backup.
 * @param lastBackupUpdates In/out : total des mises à jour au dernier backup.
 * @param currentUpdates    Total courant des mises à jour des files.
 * @return 1 si un backup doit être effectué ce tour, 0 sinon.
 */
int should_autobackup(int *lastBack, unsigned long long *lastBackupUpdates,
                      unsigned long long currentUpdates);

/**
 * @brief Une passe d'élagage automatique (corps de boucle de `rmnonext_thread`,
 *        extrait pour être testable hors thread).
 *
 * Élague le datamanager (`remove_possibilities_with_no_next`) uniquement si
 * aucun client n'est connecté (`get_active_threads(thread_params) <= 0`) :
 * l'élagage verrouille les files, il est suspendu tant qu'elles sont en cours
 * d'alimentation.
 *
 * @param map_parts   Map de lookup des pièces.
 * @param rotateParts Pièces en rotation.
 */
void rmnonext_pass(map_big_array *map_parts, struct array_part *rotateParts);

/**
 * @brief Une passe de tri périodique du stock par file (corps de boucle de
 *        `sort_periodic_thread`, extrait pour être testable hors thread).
 *
 * Trie chaque file EN PLACE (`sort_ascending_files`/`sort_descending_files`
 * selon `server_sort_direction`), SANS regroupement — préserve la
 * distribution round-robin. Suspendue tant qu'un client est connecté
 * (`get_active_threads(thread_params) <= 0`), même garde-fou que
 * `rmnonext_pass`.
 */
void sort_periodic_pass(void);

/**
 * @brief Alloue et initialise le pool de threads de communication du serveur
 *        (extrait de `runserver` pour être testable hors boucle accept).
 *
 * Positionne les globales `thread_params` (un `client_t` par thread, slots
 * vides : exist=0, socket=-1, compteur=i) et `fileUpdates` (à zéro),
 * dimensionnées sur `NB_THREADS`.
 *
 * @param rotateParts Pièces en rotation, partagées par tous les slots
 *                    (sérialisation CSV des solutions).
 */
void init_server_thread_pool(struct array_part *rotateParts);

/**
 * @brief Applique les timeouts de session (`tcp_timeout`, réception et envoi)
 *        au socket d'un client fraîchement accepté (extrait de `runserver`).
 *
 * @param client_id Socket du client.
 */
void configure_client_socket(int client_id);

/**
 * @brief Tente d'affecter un client accepté à un slot du pool (un tour de la
 *        boucle d'affectation de `runserver`, extrait pour être testable).
 *
 * Cherche un thread libre ; régénère au plus un slot vide par tour
 * (`create_server_thread`) et l'affecte si le client ne l'a pas encore été.
 * Si tout est occupé, journalise « all threads busy » une fois par épisode
 * (`*busy_logged`) puis cède le CPU.
 *
 * @param client_id   Socket du client accepté.
 * @param peer_ip     Adresse IP du pair (`inet_ntop` sur le `sockaddr` renvoyé
 *                    par `accept()`), copiée dans le slot affecté avec
 *                    `socket_id`. `NULL` accepté (le slot garde son
 *                    `peer_ip` précédent, ou `""` si jamais affecté).
 * @param busy_logged In/out : 1 si l'épisode d'attente a déjà été journalisé.
 * @return L'indice du slot affecté, ou -1 (l'appelant réessaie).
 */
int try_assign_client_slot(int client_id, const char *peer_ip, int *busy_logged);

#endif /* etii_server_h */

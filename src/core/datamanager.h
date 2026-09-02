/**
 * @file datamanager.h
 * @brief Méthodes pour gérer les files de possibilités
 */
#ifndef eternityII_datamanager_h
#define eternityII_datamanager_h

#include <pthread.h>
#include <time.h>
#include "app/etii_client.h"
#include "core/possibility.h"
#include "core/lifo.h"

// Nombre de files configurable au démarrage (ferme le @todo qui vivait ici).
// `file_possibility`/`file_possibility_checked`/
// `file_possibility_analysed`/`analysed_index` (datamanager.c) sont des tableaux de
// POINTEURS, alloués/agrandis par `datamanager_configure_stock_files` — le coût mémoire
// suit le nombre de files RÉELLEMENT actif, jamais un plafond pré-alloué. Deux constantes
// de compilation subsistent néanmoins : NB_FILE_POSSIBILITY_DEFAULT (10, nombre appliqué
// au démarrage quand --stock-files est absent) et NB_FILE_POSSIBILITY_MAX (128, garde-fou
// de bon sens contre une valeur absurde — coûte maintenant seulement 8 octets par entrée
// non utilisée dans les tableaux de POINTEURS, pas 8 Kio comme avec l'ancien tableau
// statique de structures). `nb_file_possibility` (variable) est le compte réellement actif
// — c'est CETTE valeur, jamais les deux constantes ci-dessus, que lisent les boucles
// `for (fp = 0; fp < nb_file_possibility; fp++)`.
#define NB_FILE_POSSIBILITY_DEFAULT 10
#define NB_FILE_POSSIBILITY_MAX 128

/**
 * @brief Nombre de files RÉELLEMENT actif (option CLI `--stock-files <n>`,
 *        appliqué une seule fois au démarrage via `datamanager_configure_stock_files`,
 *        jamais à chaud).
 *
 * Défaut `NB_FILE_POSSIBILITY_DEFAULT` (10). Un plus grand nombre de files réduit le
 * temps d'écriture par file de la sauvegarde cohérente et affine la granularité du
 * rééquilibrage incrémental. Lu par TOUTES les boucles `for (fp = 0; fp <
 * nb_file_possibility; fp++)` de ce fichier (et de `src/net/http_codec.c`,
 * `src/net/http_server.c`, `src/app/etii_server.c`, `src/app/app_runtime.c`).
 *
 * **Vaut 0 tant que `datamanager_configure_stock_files` n'a pas été appelée** — appel
 * OBLIGATOIRE, une fois, avant tout autre usage de ce fichier. Trois points d'entrée de
 * processus de ce projet (les trois seuls `main()` réels du dépôt) l'appellent chacun en
 * tout premier : `src/app/main.c` (mode production), `tests/test_main.c` (suite de tests)
 * et `tests/bench/bench_refutation.c` (banc de mesure, par prudence — il n'exerce
 * aujourd'hui aucune fonction de pool, mais lie `datamanager.c` via `TEST_MODULES`).
 * Contrairement au reste de ce module — dont les échecs dégradent toujours gracieusement
 * — indexer une file avant cet appel est un déréférencement de pointeur NULL, pas
 * une dégradation : ce n'est PAS un état à tolérer, seulement à ne jamais créer.
 */
extern int nb_file_possibility;

/**
 * @brief (Ré)alloue les files de stock pour couvrir `n` — croît, ne
 *        réduit jamais la mémoire déjà allouée.
 *
 * Premier appel du processus (`nb_file_possibility` encore à 0) : alloue les
 * `n` premières files depuis rien. Appels suivants : n'alloue que les files
 * AU-DELÀ de la capacité déjà acquise (`nb_file_possibility_capacity`,
 * interne) — jamais de perte ni de réinitialisation de ce qui existe déjà.
 * Un `n` inférieur au compte actuel réduit `nb_file_possibility` (les files
 * au-delà deviennent inertes, ignorées par les boucles) SANS libérer leur
 * mémoire — un futur agrandissement au-delà les retrouve déjà valides.
 *
 * @param n Nombre de files demandé. `n <= 0` est refusé (retour -1, aucun
 *          effet) ; `n > NB_FILE_POSSIBILITY_MAX` est silencieusement
 *          plafonné à `NB_FILE_POSSIBILITY_MAX` (garde-fou de bon sens, pas
 *          une limite de pré-allocation — cf. commentaire plus haut).
 * @return  0 si appliqué (y compris plafonné), -1 si `n <= 0` ou en cas
 *          d'échec d'allocation (déjà journalisé).
 */
int datamanager_configure_stock_files(int n);

/**
 * @brief Coût mémoire RÉEL, en octets, d'une possibilité résidente dans un
 *        des pools de stock (non vérifié ou vérifié).
 *
 * `sizeof(struct possibility_packet)` seul (576 octets sur le puzzle 256)
 * sous-estime l'empreinte réelle d'environ 10 % : `put()` (`core/lifo.c`)
 * fait DEUX `malloc()` par possibilité stockée — un `Element` (le nœud de
 * liste chaînée) et sa valeur (une copie du paquet) — jamais un seul. Cette
 * fonction additionne `sizeof(Element)` + `sizeof(struct possibility_packet)`
 * + un surcoût d'allocateur estimé par `malloc` (`DATAMANAGER_MALLOC_OVERHEAD`,
 * deux fois). Utilise `sizeof` directement : reste juste aussi bien sous
 * `ETERN_PARTS=256` (build par défaut) que sous `ETERN_PARTS=16` (CI/tests).
 *
 * @return Octets estimés par possibilité résidente.
 */
unsigned long long datamanager_bytes_per_possibility(void);

/**
 * @brief Convertit un plafond exprimé en Mo (tel que fourni par l'opérateur,
 *        option CLI `--stock-max-ram`) en NOMBRE de possibilités — l'unité
 *        réellement comparée par `put_to_pool`.
 *
 * Fonction pure : ne lit ni n'écrit aucun état module, testable isolément.
 *
 * @param megabytes Plafond en Mo. `<= 0` signifie « illimité » (même
 *                   convention que `limit 0`/`leaseDuration 0`).
 * @return           `0` si `megabytes <= 0` (illimité) ; sinon le nombre de
 *                    possibilités que `megabytes` Mo peuvent contenir, calculé
 *                    via `datamanager_bytes_per_possibility()`.
 */
unsigned long long datamanager_ram_limit_to_packets(int megabytes);

/**
 * @brief Convertit un nombre de possibilités résidentes en Mo — le sens
 *        inverse de `datamanager_ram_limit_to_packets`, pour l'affichage
 *        (console `stockMemory`, `GET /api/v1/status`).
 *
 * Fonction pure, mêmes garanties que `datamanager_ram_limit_to_packets`.
 *
 * @param packets Nombre de possibilités résidentes.
 * @return        Mo occupés (arrondi au Mo supérieur), via
 *                `datamanager_bytes_per_possibility()`.
 */
unsigned long long datamanager_packets_to_ram_mb(unsigned long long packets);

/**
 * @brief Applique le plafond RAM du stock (option CLI `--stock-max-ram`, ou
 *        commande console `stockMaxRam`) — convertit une seule fois en
 *        possibilités et publie le résultat pour `put_to_pool`.
 *
 * Ne couvre QUE les deux pools de stock (non vérifié + vérifié) — jamais le
 * pool analysé (cf. le commentaire de `stock_max_ram_mb`,
 * `app/app_static_variables.h`). Applicable à chaud : contrairement à
 * `datamanager_configure_stock_files` (une seule fois, avant tout fork), un
 * plafond RAM peut être resserré ou desserré en cours d'exécution sans
 * reconstruire quoi que ce soit — il n'influence que la décision d'accepter
 * ou de refuser un futur ADD, jamais l'état déjà résident.
 *
 * @param megabytes Nouveau plafond en Mo. `<= 0` = illimité.
 */
void datamanager_configure_ram_limit(int megabytes);

/**
 * @brief Plafond RAM actif du stock, en NOMBRE de possibilités (0 =
 *        illimité) — tel que publié par `datamanager_configure_ram_limit`.
 *
 * Lu par `put_to_pool` à chaque insertion ; exposé pour les tests et pour la
 * commande console `stockMemory`/la route `GET /api/v1/status`, qui affichent
 * la limite effective sans dupliquer la conversion Mo -> possibilités.
 *
 * @return Plafond en possibilités, 0 = illimité.
 */
unsigned long long datamanager_ram_limit_packets(void);

/**
 * @brief Nombre de possibilités actuellement résidentes dans les deux pools
 *        de stock (non vérifié + vérifié) — alias de `datas_size()`, sous ce
 *        nom pour symétrie avec `datamanager_ram_limit_packets()`.
 *
 * Lecture sans verrou (même convention que `datas_size()`/`file_size()`) :
 * une estimation, jamais un compte exact instantané sous contention.
 *
 * @return Possibilités résidentes, tous pools de stock confondus.
 */
unsigned long long datamanager_resident_packets(void);

/**
 * @brief 1 si une opération de maintenance (sauvegarde, restauration,
 *        tri…) tient actuellement toutes les files verrouillées, 0 sinon.
 *
 * Accesseur pour l'état interne `maintenance` — réservé à
 * `core/stock_spill.c`, pour suspendre l'éviction/le rechargement
 * pendant qu'un cliché RAM est en train d'être pris.
 */
int datamanager_is_maintenance_active(void);

/**
 * @brief Pose/lève `maintenance` pour un appelant EXTERNE à ce module
 *        — réservé à `restore_apply` (`ui/command_lines.c`), pour encadrer
 *        `stock_spill_restore_snapshot` (`core/stock_spill.c`) PUIS
 *        `restore`/`restore_analysed` dans une seule fenêtre où
 *        `stock_spill_step` reste garanti inactif. Non ré-entrant : ne
 *        jamais appeler depuis l'intérieur d'une fenêtre déjà posée par
 *        `consistent_backup`/`sort_*`.
 */
void datamanager_begin_maintenance(void);
void datamanager_end_maintenance(void);

/**
 * @brief Draine jusqu'à `max_packets` possibilités depuis la tête (mode
 *        FIFO) de la file `file_index` du pool désigné — interface étroite
 *        réservée à `core/stock_spill.c`. Un seul essai de verrou,
 *        jamais de ré-essai (rattrapé au tick suivant).
 *
 * @param is_checked  0 = pool non vérifié, 1 = pool vérifié.
 * @param file_index  Indice de file, `[0, nb_file_possibility[`.
 * @param out         Tampon de sortie, au moins `max_packets` éléments.
 * @param max_packets Nombre maximum de possibilités à extraire.
 * @return            Nombre réellement extrait (0 : file vide, index hors
 *                     bornes, ou verrou momentanément indisponible).
 */
int datamanager_pool_drain_head(int is_checked, int file_index, struct possibility_packet *out, int max_packets);

/**
 * @brief Réinsère `count` possibilités au bout chaud de la file
 *        `file_index` du pool désigné — interface étroite réservée à
 *        `core/stock_spill.c`. DOIT réussir (ces possibilités n'ont
 *        nulle part ailleurs où aller) : trylock + rotation + micro-sommeil
 *        sans budget borné, même discipline que la réinsertion de
 *        `rebalance_pool_step`.
 *
 * @param is_checked 0 = pool non vérifié, 1 = pool vérifié.
 * @param file_index Indice de file, `[0, nb_file_possibility[`.
 * @param in         Possibilités à réinsérer.
 * @param count      Nombre de possibilités dans `in`.
 * @return           `count` en fonctionnement normal ; peut être inférieur
 *                    seulement sur OOM de `put()`.
 */
int datamanager_pool_refill(int is_checked, int file_index, const struct possibility_packet *in, int count);

/**
 * @brief Indice de file de départ pour un balayage round-robin ADD/GET.
 *
 * `put_to_pool`/`scroll_from_pool` trylock la première file libre à partir
 * de cet indice plutôt que de toujours démarrer à la file 0 : sinon un
 * appel non contesté tombait systématiquement sur la file 0, où tout le
 * trafic se concentrait, à l'opposé de l'objectif de `--stock-files`.
 *
 * Fonction pure : l'état vit dans `*counter`, fourni par l'appelant, ce qui
 * la rend directement testable. Incrémente `*counter` atomiquement, jamais
 * remis à 0 : sûr à appeler concurremment depuis plusieurs threads serveur.
 *
 * @param counter État partagé, incrémenté à chaque appel.
 * @param n       Nombre de files ; `n <= 0` renvoie 0.
 * @return        Indice de file dans `[0, n[`.
 */
int datamanager_rr_next_start(unsigned int *counter, int n);

/**
 * @brief Structure représentant un file de possibilités
 * 
 * Cette structure permet d'indiquer qu'une file est "lockée" en mutli-thread
 */
typedef struct
{
    File file;
    pthread_mutex_t lock;
} file_possibility_t;

/**
 * @brief Ajoute des possibilités dans le datamanager (local ou serveur distant).
 *
 * Si une IP serveur est configurée et que `client_possibility` est non NULL,
 * les possibilités sont envoyées au serveur TCP ; sinon elles sont insérées dans
 * les files locales.
 *
 * @param client_possibility Contexte du thread client (peut être NULL en mode local).
 * @param possibilities      Tableau de paquets à ajouter.
 * @return                   0 si OK, non nul en cas d'erreur (-1 : connexion
 *                           serveur perdue ; 1 : pool local resté verrouillé
 *                           au-delà d'un délai borné — rien n'a été
 *                           inséré dans les deux cas, sûr à réessayer).
 */
int add_possibility(client_possibility_t *client_possibility, array_possibility_packet *possibilities);

/**
 * @brief Extrait des possibilités à traiter pour un thread de recherche.
 *
 * Essaie d'abord les files locales, puis le serveur TCP si disponible.
 *
 * @param client_possibility Contexte du thread client.
 * @param max_result         Nombre maximum de possibilités à extraire.
 * @param from_server Optionnel. En sortie, mis à 1 si le lot vient
 *                    réellement d'un `INST_GET_TO_CHECK[_BATCH]` serveur (le
 *                    serveur a donc enregistré ces possibilités comme « en
 *                    analyse »), 0 s'il vient des files locales — auquel cas
 *                    le serveur n'a rien à acquitter. Sans cette
 *                    distinction, un lot recyclé localement pourrait
 *                    ré-acquitter une possibilité déjà retirée du pool
 *                    analysé serveur.
 * @return Tableau alloué (à libérer avec `free_array_possibility_packet`).
 */
array_possibility_packet *get_last_possibility(client_possibility_t *client_possibility, int max_result, int *from_server);
/**
 * @brief Extrait des possibilités non vérifiées du datamanager local (côté serveur).
 *
 * @param max_result Nombre maximum de possibilités à extraire.
 * @return           Tableau alloué (à libérer avec `free_array_possibility_packet`).
 */
array_possibility_packet *get_last_possibility_tocheck(int max_result);
/**
 * @brief Enregistre une possibilité en cours d'analyse dans la file dédiée.
 *
 * Utilise `thread` comme index de file cible (rotation automatique si `thread < 0`).
 * Met à jour `max_result` si la possibilité établit un nouveau record.
 *
 * @param possiblity Paquet à enregistrer.
 * @param thread     Index du thread (−1 = choix automatique).
 * @return           0 si ajouté, -1 si le pool est resté intégralement
 *                    verrouillé au-delà d'un délai borné (maintenance
 *                    en cours) — rien n'est inséré dans ce cas.
 */
int add_possibility_analysed(struct possibility_packet *possiblity, int thread);

/**
 * @brief Comme `add_possibility_analysed`, mais enregistre en plus le
 *        `client_uid` du client à qui LE SERVEUR sert cette possibilité
 *        (attribution des analyses en cours). Réservé au côté serveur — côté
 *        client, `thread` est un index de fork local, sans rapport avec
 *        cette notion d'attribution, et `add_possibility_analysed` reste
 *        l'appel à utiliser (aucune attribution enregistrée).
 *
 * @param possiblity Paquet à enregistrer.
 * @param thread     Index du thread (−1 = choix automatique).
 * @param owner_uid  `client_uid` (16 octets) du client servi, jamais NULL
 *                    (utiliser `add_possibility_analysed` sinon).
 * @return           0 si ajouté, -1 si le pool est resté intégralement
 *                    verrouillé au-delà d'un délai borné (maintenance
 *                    en cours) — rien n'est inséré dans ce cas.
 */
int add_possibility_analysed_owned(struct possibility_packet *possiblity, int thread,
                                    const uint8_t owner_uid[CLIENT_UID_BYTES]);

/**
 * @brief Résume ce qu'un client (`client_uid`) détient actuellement dans le
 *        pool « analysed » — consultation « que travaille X ? ».
 *
 * Balaye l'index latéral d'attribution adossé à `analysed_index` (jamais
 * `possibility_packet` lui-même, cf. arbitrage C du document de conception) :
 * une possibilité restaurée depuis un backup, ou servie côté client (jamais
 * via `add_possibility_analysed_owned`), n'a pas de propriétaire connu et
 * n'est donc jamais comptée ici, pour aucun `client_uid`.
 *
 * @param owner_uid     `client_uid` recherché (16 octets, jamais NULL).
 * @param out_count     Out : nombre de possibilités actuellement attribuées.
 * @param out_max_alloc Out : le plus grand `alloc` parmi elles, -1 si `*out_count == 0`.
 * @return              0 si OK, -1 si un paramètre est NULL.
 */
int datamanager_analysed_owned_by(const uint8_t owner_uid[CLIENT_UID_BYTES],
                                   unsigned long long *out_count, int *out_max_alloc);

/**
 * @brief Décide si un bail est expiré à l'instant `now` (bail à
 *        expiration).
 *
 * Fonction pure : ne consulte JAMAIS l'horloge réelle elle-même, `now` est
 * toujours fourni par l'appelant — ce qui la rend testable sans `sleep`.
 *
 * @param lease_deadline Échéance du bail, telle qu'enregistrée par
 *                        `add_possibility_analysed_owned` (0 = bail
 *                        désactivé/non applicable : jamais expiré).
 * @param now             Horodatage de référence.
 * @return                1 si expiré, 0 sinon.
 */
int analysed_lease_is_expired(time_t lease_deadline, time_t now);

/**
 * @brief Callback de vivacité consulté par `datamanager_reclaim_expired_leases`
 *        (correctif) : indique si le propriétaire `owner_uid` est encore
 *        observable côté serveur (typiquement, un canal de contrôle toujours
 *        enregistré — `control_registry_has_active_client`, `src/app/control_registry.h`).
 *
 * `datamanager.c` (domaine `core/`) ne dépend volontairement PAS de
 * `control_registry.h` (domaine `app/`, serveur uniquement) : c'est
 * l'appelant serveur (`check_server_step`, `src/app/etii_server.c`) qui
 * fournit ce callback, gardant `datamanager_reclaim_expired_leases` testable
 * sans faire vivre un registre de sessions de contrôle.
 *
 * @param owner_uid `client_uid` du propriétaire (16 octets).
 * @return          1 si vivant (ne JAMAIS réclamer, quelle que soit l'échéance),
 *                  0 si mort/inconnu (l'échéance du bail décide seule).
 */
typedef int (*analysed_owner_alive_fn)(const uint8_t owner_uid[CLIENT_UID_BYTES]);

/**
 * @brief Balaie la table latérale d'attribution et remet dans le stock non
 *        vérifié toute possibilité dont le bail a expiré à `now` **et** dont
 *        le propriétaire n'est plus vivant.
 *
 * Un client disparu sans avoir acquitté ce qu'il tenait ne gèle plus
 * indéfiniment sa part du stock. L'échéance seule ne suffit pas : un client
 * occupé mais vivant (répond toujours aux `CTRL_PING` de son canal de
 * contrôle) verrait son travail réclamé à tort. `owner_alive`, si non-NULL,
 * est donc consulté en plus de l'échéance — réclamé seulement si les deux
 * sont vrais. `owner_alive == NULL` retombe sur l'échéance seule.
 *
 * Balayage borné et périodique, jamais dans un chemin chaud, verrouillant
 * chaque `file_possibility_analysed[f]` le temps de son propre passage. Ce
 * verrou par file rend l'opération idempotente vis-à-vis d'un acquittement
 * concurrent (`remove_possibility_analysed`) : les deux passent par le même
 * verrou, jamais de double retrait. N'affecte que les entrées attribuées :
 * une possibilité sans propriétaire connu n'expire jamais par ce mécanisme.
 *
 * @param now         Injecté (jamais `time(NULL)` en interne) : testable
 *                    sans horloge réelle ni sleep.
 * @param owner_alive Callback de vivacité, ou `NULL` pour ignorer la
 *                    vivacité (échéance seule).
 * @return            Nombre de possibilités rendues au stock.
 */
/**
 * @brief Supprime toute possibilité dont l'une des `origins` est la racine.
 *
 * Rendre au stock une possibilité en cours d'analyse (bail expiré) la remet
 * en concurrence avec les enfants que le client avait déjà poussés avant de
 * disparaître : leur sous-arbre est dès lors couvert deux fois. Ce nettoyage
 * supprime ces descendants (stock ET pool analysé). L'origine elle-même
 * n'est jamais touchée — même arbitrage que `check_origin`.
 *
 * Verrouillage en deux temps, pool analysé puis stock, jamais les deux
 * familles de verrous en même temps : pas de risque d'interblocage avec
 * `INST_GET`, mais atomicité imparfaite — une possibilité servie entre les
 * deux temps échappe à la passe. Nettoyage au mieux, pas une garantie.
 *
 * @param origins Tableau de paquets racines (jamais supprimés). `NULL` -> 0.
 * @param n       Nombre d'origines. `0` -> 0, sans prendre le moindre verrou.
 * @return        Nombre de possibilités supprimées.
 */
unsigned long long datamanager_purge_descendants_of(const struct possibility_packet *origins,
                                                    unsigned long long n);

unsigned long long datamanager_reclaim_expired_leases(time_t now, analysed_owner_alive_fn owner_alive);

/**
 * @brief Verrouille `client_possibility->socket_mutex` ET signale
 *        (`server_io_active`, `src/core/core_static_variables.h`) que ce fork est
 *        en train d'échanger avec le serveur.
 *
 * Remplace un `pthread_mutex_lock(&client_possibility->socket_mutex)` nu
 * PARTOUT où ce mutex borne exactement un échange réseau (connexion, envoi,
 * réception) — `put_to_server`, `send_solution`, `send_possibility_analysed`,
 * `scroll_from_server`, et la sonde de faim (`poll_server_hunger`,
 * `src/app/etii_client.c`). Ne remplace PAS un verrouillage de ce même mutex
 * qui ne borne PAS un échange serveur (ex. `run_mono_client` verrouillant
 * juste pour fermer le socket en fin de vie du fork) — un seul `client_possibility_t`
 * par fork, donc un seul mutex, partagé sans distinction entre le thread
 * d'alimentation et le thread de recherche (délégation via `add_possibility`) :
 * aucune notion de « par thread » n'est nécessaire, le mutex sérialise déjà
 * tout.
 *
 * @param client_possibility Contexte du thread client dont le socket va être
 *                            utilisé.
 */
void server_socket_io_lock(client_possibility_t *client_possibility);

/**
 * @brief Symétrique de `server_socket_io_lock` : efface `server_io_active`
 *        PUIS déverrouille `client_possibility->socket_mutex` (dans cet ordre,
 *        pour qu'aucun lecteur ne puisse observer le mutex libre alors que
 *        `server_io_active` prétend encore un échange en cours).
 *
 * @param client_possibility Contexte du thread client dont le socket vient
 *                            d'être utilisé.
 */
void server_socket_io_unlock(client_possibility_t *client_possibility);

/**
 * @brief Renvoie au serveur les possibilités analysées depuis les files locales.
 *
 * Extrait les paquets de la file d'analyse et les transmet via le socket TCP
 * du thread client (instructions INST_ADD / INST_POSSIBILITY_ANALYSED).
 *
 * @param client_possibility Contexte du thread client.
 */
void send_possibility_analysed(client_possibility_t *client_possibility);

/**
 * @brief Signale une solution complète au serveur (instruction INST_SOLUTION).
 *
 * Envoie le paquet solution sur le socket TCP du thread client, sous
 * `socket_mutex`, et attend l'acquittement `INST_CONSIDERED`. Sans serveur
 * configuré (mode local) ou si la connexion échoue, journalise et renvoie -1
 * sans bloquer : la solution reste sauvegardée localement par `log_solution`.
 *
 * @param client_possibility Contexte du thread client (socket vers le serveur).
 * @param possibility        Paquet solution à transmettre.
 * @return                   0 si le serveur a acquitté, -1 sinon.
 */
int send_solution(client_possibility_t *client_possibility, struct possibility_packet *possibility);

/**
 * @brief Retire une possibilité de la file des analyses en cours.
 *
 * Parcourt les files d'analyse en cherchant le paquet correspondant et le supprime.
 *
 * @param possiblity     Paquet à retirer.
 * @param thread         Index de file exact et exclusif (≥ 0 : une seule
 *                        file essayée, absence confirmée si absente de
 *                        celle-ci, aucun repli — réservé à l'usage client,
 *                        une file dédiée par fork) ; -1 pour balayer toutes
 *                        les files, voir `preferred_file`.
 * @param preferred_file Indice de file à essayer en premier quand
 *                        `thread < 0` (répartition de charge par connexion
 *                        serveur) — ignoré si `thread >= 0`. Sur un manque à
 *                        cette file, le balayage se poursuit sur toutes les
 *                        autres. `-1` (ou hors bornes) : balayage démarrant
 *                        à la file 0.
 * @return 0 si trouvé et retiré ; 1 si absent (toutes les files concernées
 *         ont été verrouillées et parcourues sans le trouver — absence
 *         confirmée) ; -1 si le budget borné a été épuisé sans jamais
 *         réussir à verrouiller ne serait-ce qu'une file (maintenance en
 *         cours) — absence non confirmée, à ne jamais traiter comme un 1.
 */
int remove_possibility_analysed(struct possibility_packet *possiblity, int thread, int preferred_file);
/** @brief Nombre de possibilités dans la file `nfile` du pool principal (non vérifiées). */
unsigned long long file_size(int nfile);
/** @brief Nombre de possibilités dans la file `nfile` du pool vérifié (validées par un pruner). */
unsigned long long file_checked_size(int nfile);
/** @brief Nombre de possibilités en cours d'analyse dans la file `nfile`. */
unsigned long long file_analysed_size(int nfile);
/** @brief Taille totale des deux pools de possibilités (non vérifiées + vérifiées). */
unsigned long long datas_size(void);

/**
 * @brief Renseigne l'IP du serveur
 *
 * @param server IP du serveur au format texte.
 */
void set_server_ip(const char *server);
/**
 * @return l'IP du serveur
 */
char *get_server_ip(void);

/** @brief Code de retour de `backup`/`backup_analysed` : sauvegarde effectuée. */
#define BACKUP_OK 0
/** @brief Code de retour de `backup`/`backup_analysed` : échec (I/O, fopen, fwrite, rename…). */
#define BACKUP_ERROR (-1)
/**
 * @brief Code de retour de `backup`/`backup_analysed` : sauvegarde sautée car une
 * maintenance (tri, regroup, autre backup…) était déjà en cours (`maintenance` != 0).
 * Le fichier cible n'est ni créé ni modifié : l'ancienne sauvegarde reste intacte.
 * Les appelants critiques (arrêt sur solution, autobackup, commande console) doivent
 * journaliser ce cas : il ne s'agit PAS d'un succès silencieux.
 */
#define BACKUP_SKIPPED_MAINTENANCE 1

/**
 * @brief Effectue une sauvegarde fichier des files de possiblités.
 *
 * Écrit dans un fichier temporaire (`<filename>.tmp`, même répertoire) puis le
 * bascule atomiquement (`rename`) vers `filename` : un crash pendant l'écriture
 * ne corrompt jamais la sauvegarde précédente. `fwrite`/`fclose` sont contrôlés ;
 * en cas d'échec le `.tmp` est supprimé et `filename` reste inchangé.
 *
 * @param filename nom du fichier dans lequel faire la sauvegarde
 * @return BACKUP_OK (0) si la sauvegarde a été écrite et publiée,
 *         BACKUP_SKIPPED_MAINTENANCE (1) si elle a été sautée (maintenance en cours,
 *         fichier cible non touché), BACKUP_ERROR (-1) en cas d'erreur d'E/S.
 */
int backup(char *filename);
/**
 * @brief Effectue une sauvegarde fichier des possibilités en cours d'anlayse.
 * Les possibilités en cours d'analyse sont les possibilités fournies par le serveur.
 *
 * Mêmes garanties atomiques que `backup` (fichier temporaire + rename).
 *
 * @param filename nom du fichier dans lequel faire la sauvegarde
 * @return BACKUP_OK (0), BACKUP_SKIPPED_MAINTENANCE (1) ou BACKUP_ERROR (-1) — voir `backup`.
 */
int backup_analysed(char *filename);
/**
 * @brief Sauvegarde le pool analysé et le stock à un instant T unique —
 *        préférer à `backup()` + `backup_analysed()` séparés, qui laissent
 *        une fenêtre où une possibilité acquittée peut disparaître des deux
 *        sauvegardes. Toutes les files sont gelées d'un coup avant la
 *        première écriture, puis libérées progressivement.
 *
 * `spill_snapshot_fn` (optionnel) est appelée une fois, pendant la fenêtre
 * `maintenance`, avec `spill_snapshot_dir` : point d'intégration du
 * débordement disque — `core/` ne pouvant dépendre de `stock_spill.h`,
 * l'appelant injecte la fonction réelle (`stock_spill_snapshot`) ; les appels
 * internes passent `NULL`. Son retour (possibilités déportées) est écrit
 * dans `<stock_filename>.spillcount` dès que le volet stock a réussi (jamais
 * si `spill_snapshot_fn` est `NULL`) — relu par `restore` pour détecter une
 * restauration partielle du débordement plutôt que de la tolérer en silence.
 *
 * @param out_analysed_status Sur retour : code du volet analysé (mêmes
 *                            constantes BACKUP_* que le retour de la
 *                            fonction, qui porte le code du volet stock).
 * @param spill_snapshot_dir  Répertoire cible du cliché, ou `NULL`.
 * @param spill_snapshot_fn   Fonction de cliché (typiquement
 *                            `stock_spill_snapshot`), ou `NULL`.
 * @return Code du volet stock — BACKUP_OK (0), BACKUP_SKIPPED_MAINTENANCE (1)
 *         ou BACKUP_ERROR (-1).
 */
typedef unsigned long long (*consistent_backup_spill_snapshot_fn)(const char *snapshot_dir);
int consistent_backup(char *stock_filename, char *analysed_filename, int *out_analysed_status,
                       const char *spill_snapshot_dir, consistent_backup_spill_snapshot_fn spill_snapshot_fn);

/**
 * @brief Lit le fichier accessoire `<stock_filename>.spillcount` écrit par
 *        `consistent_backup`.
 *
 * Réservé à `restore_apply` : compare ce compte à ce que
 * `stock_spill_restore_snapshot` a réellement récupéré, pour détecter une
 * restauration partielle du débordement plutôt que de la tolérer en silence.
 *
 * Absence tolérée (retour 0) : sauvegarde antérieure à ce mécanisme, ou
 * `consistent_backup` appelée sans `spill_snapshot_fn` — rien à vérifier,
 * jamais une anomalie en soi.
 *
 * @param out_count Sur retour (si la fonction renvoie 1) : nombre de
 *                  possibilités déportées au moment de cette sauvegarde.
 * @return 1 si trouvé et lu, 0 sinon (absent, illisible, ou contenu invalide).
 */
int datamanager_read_spillcount_sidecar(const char *stock_filename, unsigned long long *out_count);

/**
 * @brief Reconstruit les files avec le contenu du fichier
 * 
 * @param filename nom du fichier contenant une sauvegarde de file de possibilité
 * @return 0 si OK ou -1 en cas d'erreur
 */
int restore(char *filename);
/**
 * @brief Reconstruit les files de possibiltés en cours d'analyse avec le contenu du fichier
 * 
 * @param filename nom du fichier contenant une sauvegarde de file de possibilité en cours d'analyse
 * @return 0 si OK ou -1 en cas d'erreur
 */
int restore_analysed(char *filename);
/**
 * @brief Importe le contenu d'un fichier dans les files
 * 
 * @param client_possibility Thread client à qui sont destinées ces possibilités
 * @param filename nom du fichier contenant un ensemble de possibilité
 * @return 0 si OK ou -1 en cas d'erreur
 */
int import(client_possibility_t *client_possibility, char *filename);
/**
 * @brief Importe le contenu du fichier dans les possiblités en cours d'analyse
 * 
 * @param filename nom du fichier contenant une liste de possibilités en cours d'analyse
 * @return 0 si OK ou -1 en cas d'erreur
 */
int import_analysed(char *filename);
/**
 * @brief Effectue un import au format JSON
 * @deprecated Il s'agit d'une méthode pour débugguer
 * @return 0 si OK ou -1 en cas d'erreur
 */
int import_json(void);

/**
 * @brief Affiche dans la console les possibilités, au format JSON, de toutes les files.
 * 
 * @return 0 si OK ou -1 en cas d'erreur
 */
int printdatamanager(void);
/**
 * @brief Affiche dans la console les possibilités, au format JSON, contenu dans une file
 *
 * @param fp numéro de la file à afficher
 * @return 0 si OK ou -1 en cas d'erreur
 */
int print_file(int fp);

/**
 * @brief Variante fichier de printdatamanager : exporte toutes les files au
 *        format JSON dans @p out au lieu des logs (commande console
 *        `print [fichier]`).
 * @param out   Flux ouvert en écriture (ouverture/fermeture à la charge de l'appelant).
 * @param count Décompte cumulé des possibilités écrites, NULL si inutile.
 * @return      0 en cas de succès, -1 si une écriture a échoué (export interrompu).
 */
int fprint_datamanager(FILE *out, size_t *count);

/**
 * @brief Variante fichier de print_file : exporte la file `fp` au format
 *        JSON dans @p out (commande console `printFile <n> [fichier]`).
 * @param out   Flux ouvert en écriture.
 * @param fp    Numéro de la file à exporter.
 * @param count Accumulateur du nombre de possibilités écrites, NULL si inutile.
 * @return      0 en cas de succès, -1 dès la première écriture en échec.
 */
int fprint_file(FILE *out, int fp, size_t *count);

/**
 * @brief Affiche au format JSON les possibilités en cours d'analyse dans la file `fp`.
 * @param fp Numéro de la file analysée à afficher.
 * @return   0 si OK ou -1 en cas d'erreur.
 */
int print_file_analysed(int fp);

/**
 * @brief Variante fichier de print_file_analysed (commande console
 *        `printAnalysed [fichier]`, cette fonction traite une seule file).
 * @param out   Flux ouvert en écriture.
 * @param fp    Numéro de la file d'analyse à exporter.
 * @param count Accumulateur du nombre de possibilités écrites, NULL si inutile.
 * @return      0 en cas de succès, -1 dès la première écriture en échec.
 */
int fprint_file_analysed(FILE *out, int fp, size_t *count);

/** @brief Affiche au format JSON toutes les files d'analyse en cours. */
int print_all_file_analysed(void);

/**
 * @brief Variante fichier de print_all_file_analysed : exporte toutes les
 *        files d'analyse dans @p out (commande console `printAnalysed [fichier]`).
 * @param out   Flux ouvert en écriture.
 * @param count Décompte cumulé des possibilités écrites, NULL si inutile.
 * @return      0 en cas de succès, -1 si une file a échoué à s'écrire.
 */
int fprint_all_file_analysed(FILE *out, size_t *count);

/**
 * @brief Remet dans le stock toutes les possibilités en cours d'analyse.
 *
 * Vide les files `file_possibility_analysed` et réinjecte les paquets dans le
 * stock non vérifié. À appeler quand des clients sont morts sans signaler la
 * fin de leur traitement.
 *
 * @return 0.
 */
int restock_analysed(void);

/**
 * @brief Rééquilibre les deux pools de stock (non vérifié et vérifié) :
 *        déplace jusqu'à `max_packets` possibilités PAR POOL de la ou des files les plus
 *        pleines vers les plus vides, en enchaînant autant de paires
 *        fullest→emptiest que le budget le permet (pas un seul pas isolé) —
 *        converge donc le plus vite possible pour un budget donné, plutôt
 *        que de laisser le budget inutilisé dès que la première paire est
 *        plus petite que lui.
 *
 * Chaque paire reste un pas COURT : un seul verrou de pool tenu à la fois
 * (jamais deux ensemble, même discipline que `restock_analysed`/
 * `datamanager_reclaim_expired_leases`) — seul le NOMBRE de paires par appel
 * change, pas leur coût unitaire. Pensé pour être appelé fréquemment à petit
 * budget (une fois par tour de `check_server_step`, jamais un chemin chaud),
 * de sorte que les files restent de taille comparable sans jamais
 * monopoliser un tour entier. `split_datas` l'appelle une seule fois avec un
 * budget illimité (`INT_MAX`) : la boucle interne convergeant déjà jusqu'à
 * l'équilibre complet, un seul appel suffit.
 *
 * @param max_packets Borne du nombre de possibilités déplacées PAR POOL,
 *                     toutes paires confondues.
 * @return            Nombre total de possibilités déplacées (les deux pools confondus).
 */
int datamanager_rebalance_step(int max_packets);

/** @brief Trie toutes les files de possibilités par ordre croissant de `alloc`. */
int sort_ascending(void);

/**
 * @brief Trie chaque file de possibilités par ordre croissant de `alloc`,
 *        individuellement, sans les regrouper dans une seule file.
 *
 * Consomme (`scroll`, LIFO) en priorité les possibilités les plus avancées de
 * CHAQUE file, contrairement à `sort_ascending()` qui ne concentre le tri que
 * sur la file 0 après fusion.
 */
int sort_ascending_files(void);

/** @brief Trie toutes les files de possibilités par ordre décroissant de `alloc`. */
int sort_descending(void);

/**
 * @brief Trie chaque file de possibilités par ordre décroissant de `alloc`,
 *        individuellement, sans les regrouper dans une seule file.
 *
 * Consomme (`scroll`, LIFO) en priorité les possibilités les MOINS avancées
 * de CHAQUE file, contrairement à `sort_descending()`/`sort_descending_mthread()`
 * qui ne concentrent le tri que sur la file 0 après fusion.
 */
int sort_descending_files(void);

/** @brief Trie les files par ordre décroissant en parallèle (multi-thread). */
int sort_descending_mthread(void);

/**
 * @brief Fonction de tri décroissant exécutée par un thread (entrée `pthread_create`).
 * @param f Pointeur vers la `file_possibility_t` à trier.
 * @return  NULL.
 */
void *sort_d_mono(void *f);

/**
 * @brief Regroupe toutes les files de possibilités dans la file 0.
 *
 * Utile avant une opération globale (export, split) pour rassembler toutes
 * les possibilités en un seul endroit.
 *
 * @return 0.
 */
int regroup_datas(void);

/**
 * @brief Répartit équitablement les possibilités de la file 0 vers toutes les files.
 *
 * Inverse de `regroup_datas` ; permet de paralléliser à nouveau le traitement
 * entre les threads après un regroupement.
 *
 * @return 0.
 */
int split_datas(void);

/**
 * @brief Vérifie la cohérence de toutes les possibilités des files.
 *
 * Appelle `check_possibility` sur chaque paquet et rapporte les erreurs.
 *
 * @return 0 si aucune erreur, -1 si des paquets invalides ont été détectés.
 */
int check_datas(void);

/**
 * @brief Détecte les paquets en double dans les files de possibilités.
 *
 * Compare chaque paire de paquets et log les doublons détectés.
 *
 * @return 0.
 */
int check_duplicate(void);

/**
 * @brief Vérifie qu'aucune possibilité du stock n'est la racine d'une autre.
 *
 * @param purge 0 : rapport seul. 1 : supprime en plus chaque descendant.
 * @return      0 si aucune relation racine/descendant, -1 sinon.
 */
int check_origin(int purge);

/**
 * @brief Renvoie l'intégralité du pool vérifié dans le pool non vérifié
 *        (`checked` remis à 0 sur chaque possibilité déplacée).
 *
 * Déplacement EN PLACE (même index de file), sans passer par
 * `put_back_to_stock`. Le pool « en cours d'analyse » n'est pas concerné.
 *
 * @return Nombre de possibilités déplacées.
 */
unsigned long long reset_checked_pool(void);

/**
 * @brief Nombre de niveaux d'un histogramme de répartition par `alloc`.
 *
 * `+1` : `alloc` peut valoir `ETERN_PARTS` (plateau complet), donc les
 * niveaux valides vont de 0 à ETERN_PARTS inclus.
 */
#define STOCK_DISTRIBUTION_LEVELS (ETERN_PARTS + 1)

/**
 * @brief Répartition du stock par niveau de nombre de pièces posées (`alloc`).
 *
 * Trois histogrammes indépendants, un par pool (non vérifié / vérifié / en
 * cours d'analyse), indexés par `alloc` (0 à `ETERN_PARTS` inclus). C'est la
 * donnée que la commande console `statistic` imprime — et que
 * `GET /api/v1/stock-distribution` sérialise en JSON.
 */
typedef struct {
    /// Nombre de possibilités du pool non vérifié ayant cet `alloc`.
    unsigned long long unchecked[STOCK_DISTRIBUTION_LEVELS];
    /// Nombre de possibilités du pool vérifié (`checked == 1`) ayant cet `alloc`.
    unsigned long long checked[STOCK_DISTRIBUTION_LEVELS];
    /// Nombre de possibilités du pool « en cours d'analyse » ayant cet `alloc`.
    unsigned long long analysed[STOCK_DISTRIBUTION_LEVELS];
    /// Somme de `unchecked[]` (== `possibility_stock` de `GET /api/v1/stats`).
    unsigned long long total_unchecked;
    /// Somme de `checked[]`.
    unsigned long long total_checked;
    /// Somme de `analysed[]`.
    unsigned long long total_analysed;
    /// Seconde coordonnée : somme des `min_candidats` connus par niveau
    /// (exclut `POSSIBILITY_MIN_CANDIDATS_UNKNOWN`), un tableau par pool —
    /// à diviser par le `_known[]` correspondant pour la moyenne. Un niveau
    /// sans aucune valeur connue (`*_known[i] == 0`) n'a pas de moyenne
    /// définie : difficulté non mesurée pour ce niveau, pas 0 (0 candidats
    /// n'est d'ailleurs jamais stocké — un sous-arbre à 0 candidat est mort,
    /// jamais matérialisé).
    unsigned long long unchecked_min_candidats_sum[STOCK_DISTRIBUTION_LEVELS];
    unsigned long long unchecked_min_candidats_known[STOCK_DISTRIBUTION_LEVELS];
    unsigned long long checked_min_candidats_sum[STOCK_DISTRIBUTION_LEVELS];
    unsigned long long checked_min_candidats_known[STOCK_DISTRIBUTION_LEVELS];
    unsigned long long analysed_min_candidats_sum[STOCK_DISTRIBUTION_LEVELS];
    unsigned long long analysed_min_candidats_known[STOCK_DISTRIBUTION_LEVELS];
} stock_distribution_t;

/**
 * @brief Construit la répartition du stock par `alloc` (cf. `stock_distribution_t`).
 *
 * Deux passes de verrouillage SUCCESSIVES, jamais imbriquées : les deux pools
 * de stock sous `lock_all_file()`, puis le pool analysé sous
 * `lock_all_file_analysed()` — même discipline que partout ailleurs dans ce
 * module (les deux familles de verrous ne sont jamais tenues ensemble).
 * Conséquence assumée : l'instantané n'est pas atomique ENTRE le stock et le
 * pool analysé, une possibilité servie pile entre les deux passes peut être
 * comptée deux fois ou zéro fois. C'est une donnée d'observation, pas une
 * source de vérité comptable.
 *
 * Verrous bloquants (pas de `trylock`) : chemin de diagnostic/console, on veut
 * une réponse exacte, pas rendre la main.
 *
 * @param out Répartition à remplir (remise à zéro par la fonction ; NULL toléré, no-op).
 */
void datamanager_stock_distribution(stock_distribution_t *out);

/**
 * @brief Cumul d'événements d'ajout/consommation du stock sur trois fenêtres
 *        glissantes — voir `stock_rate_windows` (`core/stock_rate.h`) pour
 *        la sémantique exacte de chaque champ.
 */
typedef struct {
    unsigned long long adds_last_1m;
    unsigned long long adds_last_1h;
    unsigned long long adds_last_1d;
    unsigned long long removes_last_1m;
    unsigned long long removes_last_1h;
    unsigned long long removes_last_1d;
    /// Même mesure, VENTILÉE par pool : permet de distinguer si les ADD/GET
    /// consommés viennent du pool non
    /// vérifié (pruners) ou vérifié (chercheurs) — les six champs ci-dessus
    /// restent l'agrégat des deux, inchangé.
    unsigned long long adds_unchecked_last_1m;
    unsigned long long adds_unchecked_last_1h;
    unsigned long long adds_unchecked_last_1d;
    unsigned long long adds_checked_last_1m;
    unsigned long long adds_checked_last_1h;
    unsigned long long adds_checked_last_1d;
    unsigned long long removes_unchecked_last_1m;
    unsigned long long removes_unchecked_last_1h;
    unsigned long long removes_unchecked_last_1d;
    unsigned long long removes_checked_last_1m;
    unsigned long long removes_checked_last_1h;
    unsigned long long removes_checked_last_1d;
} stock_rate_stats_t;

/**
 * @brief Nombre d'ajouts (`put_to_pool`) et de consommations
 *        (`scroll_from_pool`) du stock, tous pools confondus (non vérifié +
 *        vérifié) ET ventilés par pool, cumulés sur la dernière minute/heure/jour.
 *
 * Consulté par `statistic_datas()` (commande console `stats`/`statistic`) et
 * par `http_stats_collect()` (`GET /api/v1/stats`) — mesure côté serveur
 * uniquement, sans équivalent côté client/fork.
 *
 * @param out Structure à remplir (aucun effet si NULL).
 */
void datamanager_stock_rate_stats(stock_rate_stats_t *out);

/**
 * @brief Affiche des statistiques sur la distribution des possibilités par `alloc`.
 * @return 0.
 */
int statistic_datas(void);

/**
 * @brief Vérifie la cohérence structurelle d'une `File` (taille, chaînage, fin).
 *
 * Exposée (plutôt que `static`) pour être testable en isolation : aucune API
 * publique ne permet de fabriquer une `File` incohérente via les pools internes,
 * donc les tests construisent des `File`/`Element` à la main.
 *
 * @param file  File à contrôler.
 * @param f     Indice de la file (pour les messages de log).
 * @param label Nom du pool (« unchecked » / « checked ») pour les messages.
 * @return      0 si cohérent, -1 sinon.
 */
int check_one_file(File *file, int f, const char *label);

/**
 * @brief Vérifie l'intégrité d'une file individuelle de possibilités.
 * @param f Numéro de la file à vérifier.
 * @return  0 si OK, -1 si une incohérence est détectée.
 */
int check_file(int f);

/**
 * @brief Vérifie l'intégrité de toutes les files de possibilités.
 * @return 0 si OK, -1 si une incohérence est détectée dans au moins une file.
 */
int check_files(void);

/**
 * @brief Identifie et affiche la possibilité ayant le plus petit `alloc` dans les files.
 * @return 0.
 */
int search_min_datas(void);

/**
 * @brief Supprime des files les possibilités sans suite (impasses garanties).
 *
 * Parcourt chaque paquet et appelle `possibility_has_a_next` : si la case
 * courante n'admet plus aucune pièce compatible, le paquet est retiré des files.
 *
 * @param mapParts         Tableau 4D de lookup.
 * @param all_rotate_part  Tableau de toutes les rotations.
 * @return                 Nombre de possibilités supprimées.
 */
int remove_possibilities_with_no_next(map_big_array *mapParts, struct array_part *all_rotate_part);

/**
 * @brief Développe le stock du serveur jusqu'à un nombre de pièces posées cible.
 *
 * Transforme un stock maigre (typiquement le paquet genèse et ses premiers
 * enfants) en de nombreuses possibilités distribuables, en développant chaque
 * possibilité case par case (une pièce candidate par successeur, sur la case
 * la plus contrainte choisie par `search_possiblity_light`) jusqu'à ce que
 * `alloc` (nombre de pièces posées, cf. `possibility_placed_count`) atteigne
 * `target_level`. But : supprimer la famine du démarrage, où un seul client
 * retient tout l'arbre pendant que le serveur n'a rien à servir aux autres.
 * Calcul purement serveur (avant toute connexion) : impact client nul.
 *
 * Bornée sur deux axes pour ne pas mettre le serveur au travail trop longtemps :
 *  - `expand_max_levels` passes maximum (borne en profondeur, quelle que soit
 *    la consigne `target_level`, défaut `EXPAND_MAX_LEVELS`, configurable via
 *    l'option CLI `--expand-max-levels <n>`) ;
 *  - `expand_max_stock` possibilités (borne en nombre, contrôlée entre passes,
 *    défaut `EXPAND_MAX_STOCK`, configurable via l'option CLI
 *    `--expand-max-stock <n>`) — garde-fou contre un facteur de branchement
 *    élevé.
 *
 * Les branches mortes (aucun successeur) sont élaguées au passage. À appeler
 * pendant que la `map_big_array` est encore vivante (dans `runserver`, avant
 * `free_bigarray`, ou après reconstruction pour la commande console `expand`).
 *
 * @param target_level    Niveau de curseur `alloc` minimal visé (≤ 0 : no-op).
 * @param mapParts        Tableau 4D de lookup.
 * @param all_rotate_part Tableau de toutes les rotations.
 * @return                Nombre de passes d'expansion réellement effectuées.
 */
int expand_datas_to_level(int target_level, map_big_array *mapParts, struct array_part *all_rotate_part);
#endif

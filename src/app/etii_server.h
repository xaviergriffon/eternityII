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
#include "app/static_variables.h"

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
} client_t;

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
 * @brief Borne le nombre de possibilités demandées en lot par un pruner.
 *
 * Fonction pure : garantit `1 ≤ result ≤ PRUNER_BATCH_MAX`.
 *
 * @param requested Valeur brute reçue du client.
 * @return          Valeur bornée.
 */
int32_t clamp_pruner_batch(int32_t requested);

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
 * @brief Construit le tableau « File queues » du rapport serveur (une ligne par
 *        file + Total) dans une chaîne allouée ; renvoie les totaux par pool via
 *        les out-params (NULL accepté). À libérer par l'appelant.
 */
char *build_file_queues_table(unsigned long long *out_unchecked,
                              unsigned long long *out_checked,
                              unsigned long long *out_analysed);

/**
 * @brief Renvoie au stock local les possibilités servies au client mais jamais
 *        acquittées, à la déconnexion (propre ou brutale).
 *
 * Extrait du bloc de fin de `communicate_with_client` pour être testable hors de
 * la boucle d'événements. Pour chaque possibilité de `lastSent` encore présente
 * dans `file_analysed` (le client ne l'a pas acquittée via INST_POSSIBILITY_ANALYSED),
 * elle est retirée de l'« en analyse » et réinjectée dans le stock via
 * `add_possibility(NULL, …)`. Une possibilité déjà acquittée
 * (`remove_possibility_analysed != 0`) n'est pas réinjectée : pas de doublon de
 * travail terminé. NULL accepté (no-op). Ne libère PAS `lastSent`.
 *
 * @param lastSent Dernier lot de possibilités envoyé au client (peut être NULL).
 */
void requeue_last_sent_possibility(array_possibility_packet *lastSent);

/**
 * @brief Traite une instruction reçue d'un client (un tour de la boucle de
 *        `communicate_with_client`).
 *
 * Extrait du corps du `while` pour être testable hors thread (le socket peut
 * être un socketpair). `*lastSent` mémorise le dernier lot servi à rendre au
 * stock à la déconnexion ; `*version_supported` porte l'état du handshake d'un
 * tour à l'autre.
 *
 * @param client            Contexte du thread (socket_id, compteur, rotate_parts).
 * @param instruction        Instruction reçue à traiter.
 * @param lastSent           In/out : dernier lot envoyé (libéré/réaffecté ici).
 * @param version_supported  In/out : 1 si le handshake de version a réussi.
 * @return 1 pour poursuivre la boucle, 0 pour s'arrêter.
 */
int communicate_with_client_step(client_t *client, int8_t instruction,
                                 array_possibility_packet **lastSent,
                                 int *version_supported);

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

#endif /* etii_server_h */

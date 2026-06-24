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

#endif /* etii_server_h */

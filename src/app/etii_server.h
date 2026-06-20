/**
 * @file etii_server.h
 * @brief Méthodes pour un serveur EternityII
 */
#ifndef etii_server_h
#define etii_server_h

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

#endif /* etii_server_h */

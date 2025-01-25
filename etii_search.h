/**
 * @file etii_search.h
 * @brief Méthodes pour la recherche de possibilités.
 */
#ifndef etii_search_h
#define etii_search_h

#include <stdio.h>

/**
 * @brief Recherche des possibilités
 * 
 * @param userdata contexte du thread client (le type attendu est client_possibility_t mais utilisation de void pour les thread)
 * @return void* null. Retourne un pointeur afin de respecter le format d'une méthode de thread.
 */
void *autosearch (void *userdata);
#endif /* etii_search_h */

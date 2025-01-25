/**
 * @file console.h
 * @brief Méthodes pour gérer la console
 */
#ifndef console_h
#define console_h

#include <stdio.h>

/**
 * @brief Lance un thread chargé d'écouter les saisies console
 * 
 * @param[in] server 1(true) si la console est pour un serveur
 */
void run_console(int server);

#endif /* console_h */

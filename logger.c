#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>

#include "logger.h"

/**
 * @brief Affiche un message d'erreur suivi du message système correspondant à `errno`.
 *
 * Écrit sur stderr. Appelle automatiquement `flush_error` en fin.
 *
 * @param format Format printf.
 * @param ...    Arguments du format.
 */
void log_errno(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    log_error("%i : %s \n", errno, strerror(errno));
}

/**
 * @brief Affiche un message d'erreur sur stderr et vide le tampon.
 * @param format Format printf.
 * @param ...    Arguments du format.
 */
void log_error(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    flush_error();
}

/**
 * @brief Affiche un message informatif sur stdout.
 * @param format Format printf.
 * @param ...    Arguments du format.
 */
void log_info(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stdout, format, args);
    va_end(args);
}

/**
 * @brief Affiche un message de débogage sur stdout.
 *
 * Produit une sortie uniquement quand les flags DEBUG_* correspondants sont actifs
 * dans le code appelant. Nécessite un `flush_debug` explicite pour être certain
 * que la sortie est visible en cas de crash.
 *
 * @param format Format printf.
 * @param ...    Arguments du format.
 */
void log_debug(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stdout, format, args);
    va_end(args);
}

/**
 * @brief Affiche un message destiné à l'affichage interactif de la console.
 *
 * Écrit sur stdout via `vprintf` (non bufférisé côté appel ; utiliser `flush_console`
 * pour forcer l'affichage immédiat sur les terminaux en mode ligne).
 *
 * @param format Format printf.
 * @param ...    Arguments du format.
 */
void log_console(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
}

/** @brief Vide le tampon de sortie standard (pour `log_console`). */
void flush_console(void)
{
    fflush(stdout);
}

/** @brief Vide le tampon de sortie standard (pour `log_debug`). */
void flush_debug(void)
{
    fflush(stdout);
}

/** @brief Vide le tampon d'erreur standard (pour `log_error` / `log_errno`). */
void flush_error(void)
{
    fflush(stderr);
}

/** @brief Vide le tampon de sortie standard (pour `log_info`). */
void flush_info(void)
{
    fflush(stdout);
}

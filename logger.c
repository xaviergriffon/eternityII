#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>

#include "logger.h"

void log_errno(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    log_error("%i : %s \n", errno, strerror(errno));
}

void log_error(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    flush_error();
}

void log_info(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stdout, format, args);
    va_end(args);
}

void log_debug(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stdout, format, args);
    va_end(args);
}

void log_console(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
}

void flush_console(void)
{
    fflush(stdout);
}

void flush_debug(void)
{
    fflush(stdout);
}

void flush_error(void)
{
    fflush(stderr);
}

void flush_info(void)
{
    fflush(stdout);
}

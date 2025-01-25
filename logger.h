#ifndef logger_h
#define logger_h
void log_errno(const char *format, ...);
void log_error(const char *format, ...);
void log_info(const char *format, ...);
void log_debug(const char *format, ...);
void log_console(const char *format, ...);
void flush_console(void);
void flush_debug(void);
void flush_error(void);
void flush_info(void);
#endif /* logger_h */

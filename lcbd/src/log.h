#ifndef LCBD_LOG_H
#define LCBD_LOG_H

#include <stdbool.h>

#define LOG_RECENT_MAX 300
#define LOG_TEXT_MAX 200
#define LOG_LINE_MAX (LOG_TEXT_MAX + 16) // time stamp and text

void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
bool log_start(void);
void log_stop(void);
int log_recent(char lines[LOG_RECENT_MAX][LOG_LINE_MAX]);
double log_uptime(void);

#endif

#ifndef LCBD_PERSIST_H
#define LCBD_PERSIST_H

#include <stdbool.h>
#include <stddef.h>

bool data_file_read(const char *dir, const char *name, char *buf, size_t size);
// With an overlayroot also saved below /media/root-ro. On success err may hold a warning
bool data_file_save(const char *dir, const char *name, const char *data, size_t len, bool *permanent, char *err,
                    size_t err_size);

#endif

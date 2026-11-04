#ifndef LCBD_WEB_ASSETS_H
#define LCBD_WEB_ASSETS_H

#include <stddef.h>

typedef struct {
    const char *path; // URL path, for example "/index.html"
    const char *type; // content type
    const unsigned char *data;
    size_t len;
} WebAsset;

extern const WebAsset web_assets[];
extern const size_t n_web_assets;

#endif

#include "persist.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <unistd.h>

#define DATA_PATH_MAX 300
#define FILE_MODE 0644
#define OVERLAYROOT_MOUNT "overlayroot / " // start of the /proc/mounts line of an overlayroot
#define LOWER_ROOT "/media/root-ro"        // read-only lower file system of the overlayroot

bool data_file_read(const char *dir, const char *name, char *buf, size_t size)
{
    char path[DATA_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *file = fopen(path, "r");
    if (!file) {
        return false;
    }

    size_t n = fread(buf, 1, size - 1, file);
    buf[n] = '\0';
    fclose(file);

    return true;
}

static bool write_atomic(const char *path, const char *data, size_t len, char *err, size_t err_size)
{
    char new_path[PATH_MAX];
    snprintf(new_path, sizeof(new_path), "%s.new", path);
    int fd = open(new_path, O_WRONLY | O_CREAT | O_TRUNC, FILE_MODE);
    if (fd < 0) {
        snprintf(err, err_size, "cannot write %s: %s", new_path, strerror(errno));
        return false;
    }

    bool ok = write(fd, data, len) == (ssize_t)len && fsync(fd) == 0;
    ok &= close(fd) == 0;
    if (!ok || rename(new_path, path) != 0) {
        snprintf(err, err_size, "cannot save %s: %s", path, strerror(errno));
        unlink(new_path);
        return false;
    }

    return true;
}

static bool is_overlayroot(void)
{
    FILE *mounts = fopen("/proc/mounts", "r");
    char line[512];
    bool found = false;
    if (!mounts) {
        return false;
    }

    while (!found && fgets(line, sizeof(line), mounts)) {
        found = strncmp(line, OVERLAYROOT_MOUNT, sizeof(OVERLAYROOT_MOUNT) - 1) == 0;
    }

    fclose(mounts);

    return found;
}

static bool resolve_path(const char *path, char *out, size_t size)
{
    char dir[PATH_MAX], base[PATH_MAX], real_dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", path);
    snprintf(base, sizeof(base), "%s", path);
    if (!realpath(dirname(dir), real_dir)) {
        return false;
    }

    return snprintf(out, size, "%s/%s", real_dir, basename(base)) < (int)size;
}

bool data_file_save(const char *dir, const char *name, const char *data, size_t len, bool *permanent, char *err,
                    size_t err_size)
{
    char path[DATA_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    *permanent = false;
    if (!write_atomic(path, data, len, err, err_size)) {
        return false;
    }

    if (!is_overlayroot()) {
        *permanent = true;
        return true;
    }

    char absolute[PATH_MAX], lower_path[PATH_MAX + sizeof(LOWER_ROOT)];
    if (!resolve_path(path, absolute, sizeof(absolute))) {
        snprintf(err, err_size, "saved, but the path cannot be resolved for the lower file system");
        return true;
    }

    snprintf(lower_path, sizeof(lower_path), "%s%s", LOWER_ROOT, absolute);
    if (mount(NULL, LOWER_ROOT, NULL, MS_REMOUNT, NULL) != 0) {
        snprintf(err, err_size, "saved until the next power cycle only: remount of %s failed: %s", LOWER_ROOT,
                 strerror(errno));
        return true;
    }

    *permanent = write_atomic(lower_path, data, len, err, err_size);
    sync();
    if (mount(NULL, LOWER_ROOT, NULL, MS_REMOUNT | MS_RDONLY, NULL) != 0) {
        snprintf(err, err_size, "saved, but %s could not be remounted read-only: %s", LOWER_ROOT, strerror(errno));
    }

    return true;
}

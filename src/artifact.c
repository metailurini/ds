#include "artifact.h"

#include <unistd.h>

bool ds_artifact_write_atomic(const char *path, const char *data, size_t len) {
    return ds_file_write_atomic(path, data, len);
}

void ds_artifact_remove(const char *path) {
    unlink(path);
}

#ifndef DS_ARTIFACT_H
#define DS_ARTIFACT_H

#include "ds_common.h"

/* Application artifact boundary: atomic writes and failed-output cleanup. */
bool ds_artifact_write_atomic(const char *path, const char *data, size_t len);
void ds_artifact_remove(const char *path);

#endif

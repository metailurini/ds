#ifndef DS_H
#define DS_H

/*
 * Public façade for the ds tool/library surface.
 *
 * Implementation details are grouped in focused internal headers under src/.
 * This umbrella remains for compatibility with existing small unit harnesses and
 * tool entrypoints while the codebase migrates toward narrower includes.
 */
#include "../src/ds_common.h"
#include "../src/ds_command.h"
#include "../src/ds_ast.h"
#include "../src/frontend.h"
#include "../src/ds_hir.h"
#include "../src/ds_runtime.h"
#include "../src/ds_stdlib.h"
#include "../src/backend.h"

#endif

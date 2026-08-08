#ifndef DS_SIGNAL_H
#define DS_SIGNAL_H

#include "ds_ast.h"

#include <stdbool.h>

/*
 * Shared trap/defer/signal runtime contract.
 *
 * Lowering owns which source-level signal names are supported. VM and emitted
 * Bash consume this metadata for the accepted runtime cleanup subset so INT and
 * TERM status/classification policy cannot drift between backends.
 */
const char *ds_handler_signal_name(DsHandlerSignal signal);
DsHandlerSignal ds_handler_signal_parse(DsStr name);
bool ds_handler_signal_is_runtime_cleanup(DsHandlerSignal signal);
int ds_handler_signal_default_status(DsHandlerSignal signal);

DsHandlerSignal ds_handler_signal_from_posix(int sig);
bool ds_posix_signal_is_runtime_cleanup(int sig);
int ds_posix_signal_default_status(int sig);

#endif

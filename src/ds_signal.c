#include "ds_signal.h"

#include <signal.h>

const char *ds_handler_signal_name(DsHandlerSignal signal) {
    static const char *const names[] = {"EXIT", "INT", "TERM", "<invalid>"};
    return (unsigned)signal < DS_ARRAY_LEN(names) ? names[signal] : "<invalid>";
}

bool ds_handler_signal_is_runtime_cleanup(DsHandlerSignal signal) {
    return signal == DS_HANDLER_INT || signal == DS_HANDLER_TERM;
}

int ds_handler_signal_default_status(DsHandlerSignal signal) {
    static const int statuses[] = {0, 130, 143, 0};
    return (unsigned)signal < DS_ARRAY_LEN(statuses) ? statuses[signal] : 0;
}

DsHandlerSignal ds_handler_signal_from_posix(int sig) {
    if (sig == SIGINT) return DS_HANDLER_INT;
    if (sig == SIGTERM) return DS_HANDLER_TERM;
    return DS_HANDLER_INVALID;
}

bool ds_posix_signal_is_runtime_cleanup(int sig) {
    return ds_handler_signal_is_runtime_cleanup(ds_handler_signal_from_posix(sig));
}

int ds_posix_signal_default_status(int sig) {
    return ds_handler_signal_default_status(ds_handler_signal_from_posix(sig));
}

#include "ds_signal.h"

#include <signal.h>

const char *ds_handler_signal_name(DsHandlerSignal signal) {
    switch (signal) {
        case DS_HANDLER_EXIT: return "EXIT";
        case DS_HANDLER_INT: return "INT";
        case DS_HANDLER_TERM: return "TERM";
        case DS_HANDLER_INVALID: return "<invalid>";
    }
    return "<invalid>";
}

bool ds_handler_signal_is_runtime_cleanup(DsHandlerSignal signal) {
    return signal == DS_HANDLER_INT || signal == DS_HANDLER_TERM;
}

int ds_handler_signal_default_status(DsHandlerSignal signal) {
    switch (signal) {
        case DS_HANDLER_INT: return 130;
        case DS_HANDLER_TERM: return 143;
        case DS_HANDLER_EXIT:
        case DS_HANDLER_INVALID:
            return 0;
    }
    return 0;
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

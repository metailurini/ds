#include "ds_signal.h"

#include <signal.h>

typedef struct {
    const char *name;
    int default_status;
} DsHandlerSignalInfo;

static const DsHandlerSignalInfo handler_signals[] = {
    [DS_HANDLER_EXIT] = {"EXIT", 0},
    [DS_HANDLER_INT] = {"INT", 130},
    [DS_HANDLER_TERM] = {"TERM", 143},
    [DS_HANDLER_INVALID] = {"<invalid>", 0},
};

const char *ds_handler_signal_name(DsHandlerSignal signal) {
    return (unsigned)signal < DS_ARRAY_LEN(handler_signals) ? handler_signals[signal].name : "<invalid>";
}

DsHandlerSignal ds_handler_signal_parse(DsStr name) {
    for (unsigned signal = DS_HANDLER_EXIT; signal < DS_HANDLER_INVALID; signal++) {
        if (ds_str_eq_cstr(name, handler_signals[signal].name)) return (DsHandlerSignal)signal;
    }
    return DS_HANDLER_INVALID;
}

bool ds_handler_signal_is_runtime_cleanup(DsHandlerSignal signal) {
    return signal == DS_HANDLER_INT || signal == DS_HANDLER_TERM;
}

int ds_handler_signal_default_status(DsHandlerSignal signal) {
    return (unsigned)signal < DS_ARRAY_LEN(handler_signals) ? handler_signals[signal].default_status : 0;
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

#ifndef DS_VM_OPTIONS_H
#define DS_VM_OPTIONS_H

#include "ds_common.h"

typedef struct {
    bool trace_cmd;
    bool trace_vm;
    bool test_mode;
    DsStr test_name;
} DsVmOptions;

#endif

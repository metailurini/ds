#ifndef DS_CLI_H
#define DS_CLI_H

#include "app.h"

/* CLI policy boundary: argv parsing and command dispatch only. */
int ds_cli_run(DsApp *app, int argc, char **argv);

#endif

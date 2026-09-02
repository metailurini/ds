#include "cli.h"

int main(int argc, char **argv) {
    DsApp app = ds_app_default();
    return ds_cli_run(&app, argc, argv);
}

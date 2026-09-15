#ifndef LOUTRE_CLI_H
#define LOUTRE_CLI_H
#include "model.h"
#include <stdio.h>
void print_usage(FILE *stream);
int parse_args(int argc, char **argv, Options *options);
#endif

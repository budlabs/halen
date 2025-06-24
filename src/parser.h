#ifndef PARSER_H
#define PARSER_H

#include "halen.h"

void config_init(config_t *config);
int config_parse_file(config_t *config, const char *filename);
int config_apply(config_t *config);
void config_print(const config_t *config);
void config_free(config_t *config);

#endif // PARSER_H

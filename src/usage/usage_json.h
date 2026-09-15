#ifndef LOUTRE_USAGE_JSON_H
#define LOUTRE_USAGE_JSON_H

#include "usage.h"

bool usage_parse_bridge_json(const char *input, size_t length, const char *provider,
                             ProviderUsage *output);
bool usage_json_number(const char *input, size_t length, const char *key,
                       double *value);

#endif

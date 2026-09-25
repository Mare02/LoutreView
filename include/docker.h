#ifndef LOUTRE_DOCKER_H
#define LOUTRE_DOCKER_H

#include "model.h"

DockerSnapshot collect_docker_snapshot(void);
void sample_docker_usage(DockerSnapshot *current, const DockerSnapshot *previous,
                         double elapsed_seconds);

#endif

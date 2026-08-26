#ifndef __NF_EPOLL_H
#define __NF_EPOLL_H

#include "net_framework.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize epoll backend for the given framework.
 * Allocates impl, sets nf->ops and nf->impl.
 * Returns 0 on success, -1 on failure. */
int nf_epoll_init(net_framework_t *nf);

#ifdef __cplusplus
}
#endif

#endif

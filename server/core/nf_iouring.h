#ifndef __NF_IOURING_H
#define __NF_IOURING_H

#include "net_framework.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize io_uring backend for the given framework.
 * Allocates impl, sets nf->ops and nf->impl.
 * Returns 0 on success, -1 on failure. */
int nf_iouring_init(net_framework_t *nf);

#ifdef __cplusplus
}
#endif

#endif

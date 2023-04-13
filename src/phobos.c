#define _GNU_SOURCE

#include "common.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <phobos_admin.h>

int pho_xfer_add_tag(struct pho_xfer_desc *xfer, const char *new_tag)
{
    struct tags *tags = &xfer->xd_params.put.tags;
    void *tmp;

    tags->n_tags++;
    tmp = realloc(tags->tags, tags->n_tags * sizeof(*tags->tags));
    if (!tmp)
        return -errno;

    tags->tags = tmp;
    tags->tags[tags->n_tags - 1] = strdup(new_tag);
    pho_verb("adding tag '%s'", new_tag);

    if (!tags->tags[tags->n_tags - 1])
        return -errno;

    return 0;
}

#define _GNU_SOURCE

#include "common.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <phobos_admin.h>

int pho_xfer_add_tag(struct pho_xfer_desc *xfer, const char *new_tag)
{
    struct string_array *tags = &xfer->xd_params.put.tags;
    void *tmp;

    tags->count++;
    tmp = realloc(tags->strings, tags->count * sizeof(*tags->strings));
    if (!tmp)
        return -errno;

    tags->strings = tmp;
    tags->strings[tags->count - 1] = strdup(new_tag);
    pho_verb("adding tag '%s'", new_tag);

    if (!tags->strings[tags->count - 1])
        return -errno;

    return 0;
}

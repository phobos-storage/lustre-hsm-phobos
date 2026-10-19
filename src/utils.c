#define _GNU_SOURCE

#include "utils.h"

#include <errno.h>
#include <stdio.h>

int fid2objid(const char *fs_name, const struct lu_fid *fid, char *objid)
{
    int rc;

    if (!objid || !fid)
        return -EINVAL;

    /* object id is "fsname:fid" */
    rc = sprintf(objid, "%s:"DFID_NOBRACE, fs_name, PFID(fid));

    return rc < 0 ? rc : 0;
}

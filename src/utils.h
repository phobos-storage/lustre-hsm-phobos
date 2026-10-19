#ifndef _UTILS_H
#define _UTILS_H

#include <linux/lustre/lustre_fid.h>

/**
 * Build a Phobos object id ("<fs_name>:<fid>") from a filesystem name and
 * a Lustre FID.
 *
 * @return 0 on success, a negative errno value on error.
 */
int fid2objid(const char *fs_name, const struct lu_fid *fid, char *objid);

#endif

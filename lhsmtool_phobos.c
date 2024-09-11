/*
 *  vim:expandtab:shiftwidth=4:tabstop=4:
 *
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.gnu.org/licenses/gpl-2.0.htm
 *
 * GPL HEADER END
 */
/*
 * (C) Copyright 2012 Commissariat a l'energie atomique et aux energies
 * alternatives
 *
 * Copyright (c) 2013, 2016, Intel Corporation.
 */
/* HSM copytool program for Phobos.
 *
 * An HSM copytool daemon acts on action requests from Lustre to copy files
 * to and from an HSM archive system. This one in particular makes regular
 * call to Phobos.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/xattr.h>
#include <sys/syscall.h>
#include <sys/types.h>

/* Lustre header */
#include <lustre/lustreapi.h>

/* Phobos headers */
#include "phobos_store.h"
#include "pho_common.h"

#include "layout.h"
#include "common.h"

#define LL_HSM_ORIGIN_MAX_ARCHIVE (sizeof(__u32) * 8)
#define XATTR_TRUSTED_PREFIX      "trusted."

#define XATTR_TRUSTED_FUID_XATTR_DEFAULT "trusted.hsm_fuid"

#define HINT_HSM_FUID "hsm_fuid"

#define UNUSED __attribute__((unused))

/* everything else is zeroed */
struct options opt = {
    .o_verbose         = LLAPI_MSG_INFO,
    .o_default_family  = PHO_RSC_INVAL,
    .o_restore_lov     = false,
};

/*
 * hsm_copytool_private will hold an open FD on the lustre mount point
 * for us. Additionally open one on the archive FS root to make sure
 * it doesn't drop out from under us (and remind the admin to shutdown
 * the copytool before unmounting).
 */

static int err_major;
static int err_minor;

static char cmd_name[PATH_MAX];
static char fs_name[MAX_OBD_NAME + 1];
static char trusted_fuid_xattr[MAXNAMLEN];

static struct hsm_copytool_private *ctdata;

static inline double ct_now(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);

    return tv.tv_sec + 0.000001 * tv.tv_usec;
}

static void usage(int rc)
{
    fprintf(stdout,
            "Usage: %s [options]... <mode> <lustre_mount_point>\n"
            "The Lustre HSM Posix copy tool can be used as a daemon or "
            "as a command line tool\n"
            "The Lustre HSM daemon acts on action requests from Lustre\n"
            "to copy files to and from an HSM archive system.\n"
            "This Phobos-flavored daemon makes calls to Phobos storage\n"
            "The Lustre HSM tool performs administrator-type actions\n"
            "on a Lustre HSM archive.\n"
            "This Phobos-flavored tool can link an existing HSM namespace\n"
            "into a Lustre filesystem.\n\n"
            "Usage:\n"
            "        --daemon                 Daemon mode, run in background\n"
            "        --abort-on-error         Abort operation on major error\n"
            "    -A, --archive <#>            Archive number (repeatable)\n"
            "        --dry-run                Don't run, just show what would be done\n"
            "    -f, --event-fifo <path>      Write events stream to fifo\n"
            "    -F, --default-family <name>  Set the default family\n"
            "    -q, --quiet                  Produce less verbose output\n"
            "    -x, --fuid-xattr             Change value of xattr for restore\n"
            "    -v, --verbose                Produce more verbose output\n"
#ifdef HAVE_LLAPI_LAYOUT_SET_BY_FD
            "    -l, --restore-lov            Use the striping that the file "
            "had when archived (off by default)\n"
#endif
        , cmd_name);

    exit(rc);
}

static bool garray_contains(GArray *array, int value)
{
    guint i;

    for (i = 0; i < array->len; i++)
        if (g_array_index(array, int, i) == value)
            return true;

    return false;
}

static bool support_all_archive_ids(GArray *archive_ids)
{
    return archive_ids->len == 1 &&
        g_array_index(archive_ids, int, 0) == 0;
}

static void get_archive_id_array(GArray *archive_ids,
                                 int **values, int *size)
{
    if (support_all_archive_ids(archive_ids)) {
        *values = NULL;
        *size = 0;
    } else {
        *values = (int *)archive_ids->data;
        *size = archive_ids->len;
    }
}

static int parse_archive_id(const char *archive_value, GArray *archive_ids)
{
    uint64_t archive_id;
    int value;
    int rc;

    rc = str2uint64_t(archive_value, &archive_id);
    if (rc)
        return rc;

    if (archive_id > INT_MAX)
        return -ERANGE;

    if (support_all_archive_ids(archive_ids))
        return 0;

    value = (int) archive_id;
    if (value == 0) {
        g_array_remove_range(archive_ids, 0, archive_ids->len);
        g_array_append_val(archive_ids, value);

        return 0;
    }

    if (!garray_contains(archive_ids, value))
        g_array_append_val(archive_ids, value);

    return 0;
}

#ifdef HAVE_LLAPI_LAYOUT_SET_BY_FD
#define GETOPTS_STRING "A:b:c:f:F:hqx:vl"
#else
#define GETOPTS_STRING "A:b:c:f:F:hqx:v"
#endif

static int ct_parseopts(int argc, char * const *argv)
{
    struct option long_opts[] = {
        { .val = 1,    .name = "abort-on-error",
            .flag = &opt.o_abort_on_error,
            .has_arg = no_argument },
        { .val = 1,    .name = "abort_on_error",
            .flag = &opt.o_abort_on_error,
            .has_arg = no_argument },
        { .val = 'A',    .name = "archive",
            .has_arg = required_argument },
        { .val = 'b',    .name = "bandwidth",
            .has_arg = required_argument },
        { .val = 1,    .name = "daemon",
            .has_arg = no_argument,
            .flag = &opt.o_daemonize },
        { .val = 'f',    .name = "event-fifo",
            .has_arg = required_argument },
        { .val = 'f',    .name = "event_fifo",
            .has_arg = required_argument },
        { .val = 'F',    .name = "default-family",
            .has_arg = required_argument },
        { .val = 'F',    .name = "default_family",
            .has_arg = required_argument },
        { .val = 1,    .name = "dry-run",
            .has_arg = no_argument,
            .flag = &opt.o_dry_run },
        { .val = 'h',    .name = "help",
            .has_arg = no_argument },
        { .val = 'P',    .name = "pid-file",
            .has_arg = required_argument },
#ifdef HAVE_LLAPI_LAYOUT_SET_BY_FD
        { .val = 'l',    .name = "restore-lov",
            .has_arg = no_argument },
#endif
        { .val = 'q',    .name = "quiet",
            .has_arg = no_argument },
        { .val = 'u',    .name = "update-interval",
            .has_arg = required_argument },
        { .val = 'v',    .name = "verbose",
            .has_arg = no_argument },
        { .val = 'x',    .name = "fuid-xattr",
            .has_arg = required_argument},
        { .name = NULL }
    };
    int rc;
    int c;

    optind = 0;

    opt.o_archive_ids = g_array_new(false, false, sizeof(int));
    if (opt.o_archive_ids == NULL)
        return -ENOMEM;

    while ((c = getopt_long(argc, argv, GETOPTS_STRING,
                            long_opts, NULL)) != -1) {
        switch (c) {
        case 'A':
            rc = parse_archive_id(optarg, opt.o_archive_ids);
            if (rc) {
                pho_error(rc, "Invalid archive ID '%s'", optarg);
                g_array_free(opt.o_archive_ids, true);
                return rc;
            }
            break;
        case 'f':
            opt.o_event_fifo = optarg;
            break;
        case 'F':
            opt.o_default_family = str2rsc_family(optarg);
            break;
        case 'h':
            /* free the array since usage will call exit */
            g_array_free(opt.o_archive_ids, true);
            usage(0);
            break;
#ifdef HAVE_LLAPI_LAYOUT_SET_BY_FD
        case 'l':
            opt.o_restore_lov = true;
            break;
#endif
        case 'q':
             opt.o_verbose--;
             break;
        case 'P':
             opt.o_pid_file = optarg;
             break;
        case 'v':
             opt.o_verbose++;
             break;
        case 'x':
            strncpy(trusted_fuid_xattr, optarg, MAXNAMLEN);
            break;
        case 0:
             break;
        default:
             return -EINVAL;
        }
    }

    if (argc != optind + 1) {
        rc = -EINVAL;
        pho_error(rc, "no mount point specified");
        return rc;
    }

    opt.o_mnt = argv[optind];
    opt.o_mnt_fd = -1;

    pho_info("mount_point: %s", opt.o_mnt);

    return 0;
}

/*
 * Phobos functions
 */
static int fid2objid(const struct lu_fid *fid, char *objid)
{
    if (!objid || !fid)
        return -EINVAL;

    /* object id is "fsname:fid" */
    return sprintf(objid, "%s:"DFID_NOBRACE, fs_name, PFID(fid));
}

static int phobos_op_del(const struct lu_fid *fid, const struct buf *hints)
{
    struct pho_xfer_desc xfer = {0};
    struct hinttab hinttab;
    const char *obj = NULL;
    char objid[MAXNAMLEN];
    bool objset = false;
    int rc;

    if (hints->data) {
        size_t i;
        int rc;

        pho_verb("hints provided hints='%.*s', len=%lu",
                 (int)hints->len, hints->data, hints->len);
        rc = process_hints(hints, &hinttab);
        if (rc) {
            pho_error(rc, "Failed to process hints");
            return rc;
        }

        for (i = 0 ; i < hinttab.count; i++) {
            pho_verb("hints #%lu  key='%s' val='%s'",
                        i, hinttab.hints[i].key, hinttab.hints[i].value);

            if (!strcmp(hinttab.hints[i].key, HINT_HSM_FUID)) {
                obj = hinttab.hints[i].value;
                objset = true;
            }
        }
    }

    if (!objset) {
        rc = fid2objid(fid, objid);
        if (rc)
            goto free_hints;

        obj = objid;
    }

    /* DO THE DELETE */
    memset(&xfer, 0, sizeof(xfer));
    xfer.xd_op = PHO_XFER_OP_DEL;
    xfer.xd_objid = strdup(obj);
    if (!xfer.xd_objid)
        return -errno;

    rc = phobos_delete(&xfer, 1);
    if (rc)
        pho_error(rc, "Failed to delete '%s' from Phobos", obj);

    /* Cleanup and exit */
    pho_xfer_desc_clean(&xfer);
    free(xfer.xd_objid);

free_hints:
    if (hints->data)
        hinttab_free(&hinttab);

    return rc;
}

static int component_to_attr(struct llapi_layout *layout, void *user_data)
{
    struct pho_attrs *attrs = (struct pho_attrs *)user_data;
    char *component_str = NULL;
    char *id_str = NULL;
    uint32_t id;
    int rc;

    rc = llapi_layout_comp_id_get(layout, &id);
    if (rc)
        return -errno;

    if (llapi_layout_is_composite(layout)) {
        rc = asprintf(&id_str, "layout_comp%u", id);
        if (rc == -1)
            return -ENOMEM;
    }

    rc = layout_component2str(layout, &component_str);
    if (rc)
        goto out_free;

    if (llapi_layout_is_composite(layout))
        pho_attr_set(attrs, id_str, component_str);
    else
        pho_attr_set(attrs, "layout", component_str);

    pho_info("adding layout information key='%s', value='%s'",
             id_str ? : "layout", component_str);

    free(component_str);
out_free:
    free(id_str);
    if (!rc)
        return LLAPI_LAYOUT_ITER_CONT;

    return rc;
}

static int phobos_op_put(const struct lu_fid *fid,
                         const int fd,
                         struct llapi_layout *layout,
                         const struct buf *hints,
                         char **oid)
{
    struct pho_xfer_desc xfer = {0};
    struct pho_attrs attrs = {0};
    struct hinttab hinttab = {0};
    char objid[MAXNAMLEN];
    struct stat st;
    int rc;

    rc = fid2objid(fid, objid);
    if (rc < 0)
        return rc;

    /* only used for logging, not an error if allocation failed */
    *oid = strdup(objid);

    pho_attr_set(&attrs, "program", "copytool");

    if (layout) {
        rc = llapi_layout_comp_iterate(layout, component_to_attr,
                                       (void *)&attrs);
        if (rc < 0) {
            rc = -errno;
            goto free_xfer;
        }
    }

    memset(&xfer, 0, sizeof(xfer));
    xfer.xd_op = PHO_XFER_OP_PUT;
    xfer.xd_fd = fd;
    xfer.xd_flags = 0;

    /* set oid in xattr for later use */
    rc = fsetxattr(fd, trusted_fuid_xattr, objid, strlen(objid), XATTR_CREATE);
    if (rc && errno != EEXIST)
        pho_error(rc, "failed to set '%s' to '%s'", trusted_fuid_xattr, objid);

    if (fstat(fd, &st) < 0) {
        rc = -errno;
        goto free_xfer;
    }

    xfer.xd_params.put.size = st.st_size;

    /* Using default family (can be amended later by a hint) */
    xfer.xd_params.put.family = opt.o_default_family;

    /* Use content of hints to modify fields in xfer_desc */
    if (hints->data) {
        size_t i;
        int rc;

        pho_verb("hints provided hints='%.*s', len=%lu",
                 (int)hints->len, hints->data, hints->len);
        rc = process_hints(hints, &hinttab);
        if (rc)
            goto free_xfer;

        for (i = 0 ; i < hinttab.count; i++) {
            pho_verb("hints #%lu  key='%s' val='%s'",
                     i, hinttab.hints[i].key, hinttab.hints[i].value);

            if (!strcmp(hinttab.hints[i].key, "family")) {
                /* Deal with storage family */
                xfer.xd_params.put.family =
                    str2rsc_family(hinttab.hints[i].value);

                if (xfer.xd_params.put.family == PHO_RSC_INVAL)
                    pho_warn("unknown family '%s'",  hinttab.hints[i].value);

            } else if (!strcmp(hinttab.hints[i].key, "layout")) {
                /* Deal with storage layout */
                xfer.xd_params.put.layout_name = hinttab.hints[i].value;

            } else if (!strcmp(hinttab.hints[i].key, "alias")) {
                xfer.xd_params.put.alias = hinttab.hints[i].value;

            } else if (!strcmp(hinttab.hints[i].key, "tag")) {
                /* Deal with storage tags */
                rc = pho_xfer_add_tag(&xfer, hinttab.hints[i].value);
                if (rc)
                    goto free_xfer;
            } else {
                pho_warn("unknown hint '%s'",  hinttab.hints[i].key);
            }
        }
    }

    /* Finalize xfer_desc and to the PUT operation */
    xfer.xd_objid = objid;
    xfer.xd_attrs = attrs;
    xfer.xd_params.put.overwrite = true;
    rc = phobos_put(&xfer, 1, NULL, NULL);

free_xfer:
    /* free the tags as well */
    pho_xfer_desc_clean(&xfer);
    if (hints->data)
        hinttab_free(&hinttab);

    if (rc)
        pho_error(rc, "Failed to write '%s' in Phobos", objid);

    return rc;
}

static int phobos_op_get(const struct lu_fid *fid,
                         char *altobjid,
                         UNUSED const struct buf *hints,
                         int fd)
{
    struct pho_xfer_desc xfer = {0};
    char objid[MAXNAMLEN];
    char *obj = NULL;
    int rc;

    /* If provided altobjid is used as objectid */
    if (altobjid) {
        obj = altobjid;
    } else {
        rc = fid2objid(fid, objid);
        if (rc < 0)
            return rc;
        obj = objid;
    }

    memset(&xfer, 0, sizeof(xfer));
    xfer.xd_op = PHO_XFER_OP_GET;
    xfer.xd_fd = fd;
    xfer.xd_flags = 0;
    xfer.xd_objid = obj;

    rc = phobos_get(&xfer, 1, NULL, NULL);

    pho_xfer_desc_clean(&xfer);

    if (rc)
        pho_error(rc, "failed to read '%s' from Phobos", obj);

    return rc;
}

static int phobos_op_getlayout(const struct lu_fid *fid,
                               char *altobjid,
                               UNUSED const struct buf *hints,
                               struct llapi_layout **layout)
{
    struct pho_xfer_desc xfer = {0};
    char objid[MAXNAMLEN];
    char *obj = NULL;
    int rc;

    /* If provided altobjid is used as objectid */
    if (altobjid)
        obj = altobjid;
    else {
        rc = fid2objid(fid, objid);
        if (rc < 0)
            return rc;
        obj = objid;
    }

    memset(&xfer, 0, sizeof(xfer));
    xfer.xd_objid = obj;
    xfer.xd_op = PHO_XFER_OP_GETMD;
    xfer.xd_flags = 0;

    rc = phobos_getmd(&xfer, 1, NULL, NULL);
    if (rc) {
        pho_error(rc, "failed to get metadata of '%s' from Phobos", obj);
        return rc;
    }

    rc = layout_from_object_md(&xfer.xd_attrs, layout);
    if (rc)
        return -rc;

    pho_xfer_desc_clean(&xfer);

    return rc;
}

/*
 * A set of function to encode buffer into strings
 */
void bin2hexstr(const char *bin, size_t len, char *out)
{
    size_t  i;
    char    tmp[10];

    if (bin == NULL || len == 0)
        return;

    /* Size is encoded at the beginning */
    out[0] = 0;
    sprintf(out, "%02x|", (unsigned int)len);

    /* Next each char, one by one */
    for (i = 0; i < len; i++) {
        sprintf(tmp, "%08x:", bin[i]);
        strcat(out, tmp);
    }
}

unsigned int hexstr2bin(const char *hex, char *out)
{
    unsigned int    len;
    char            tmp[10]; /* too big */
    size_t            i;
    int                rc;

    if (hex == NULL || out == 0)
        return 0;

    /* get size first */
    memcpy(tmp, hex, 3);
    tmp[3] = 0;
    rc = sscanf(tmp, "%02x|", &len);
    if (!rc)
        return 0;

    /* Remind that 3 first characters encodes the size */
    out[0] = 0;

    for (i = 0; i < len; i++) {
        memcpy(tmp, &(hex[9*i+3]), 8);
        tmp[9] = 0;

        rc = sscanf(tmp, "%08x:", (unsigned int *)&(out[i]));
        if (!rc)
            return 0;
    }

    return len;
}

/*
 * Copytool functions (with ct_ prefix)
 */

static int ct_get_altobjid(const struct hsm_action_item *hai,
                           char *altobjid)
{
    char xattr_buf[XATTR_SIZE_MAX + 1];
    ssize_t xattr_size;
    int fd;
    int rc;

    fd = llapi_open_by_fid(opt.o_mnt, &hai->hai_fid, O_RDONLY);
    if (fd < 0)
        return -errno;

    xattr_size = fgetxattr(fd, trusted_fuid_xattr, xattr_buf,
                           XATTR_SIZE_MAX);
    if (xattr_size < 0) {
        rc = -errno;
        close(fd);
        return rc;
    }

    memcpy(altobjid, xattr_buf, xattr_size);
    altobjid[xattr_size] = 0; /* String trailing zero */
    close(fd);

    return 0;
}
/* FIXME the layout API doesn't provide a function to restore \p layout
 * from \p dst_fd directly. So this function does nothing at the moment.
 */
static int ct_restore_layout(UNUSED const char *dst,
                             UNUSED int dst_fd,
                             UNUSED struct llapi_layout *layout)
{
    return 0;
}

static int ct_path_lustre(char *buf, int sz, const char *mnt,
                          const struct lu_fid *fid)
{
    return snprintf(buf, sz, "%s/%s/fid/"DFID_NOBRACE, mnt,
                    dot_lustre_name, PFID(fid));
}

static bool ct_is_retryable(int err)
{
    return err == -ETIMEDOUT;
}

static int ct_begin_restore(struct hsm_copyaction_private **phcp,
                            const struct hsm_action_item *hai,
                            int mdt_index, int open_flags)
{
    char src[PATH_MAX];
    int rc;

    rc = llapi_hsm_action_begin(phcp, ctdata, hai, mdt_index, open_flags,
                                false);
    if (rc < 0) {
        ct_path_lustre(src, sizeof(src), opt.o_mnt, &hai->hai_fid);
        pho_error(rc, "llapi_hsm_action_begin() failed for '%s'", src);
    }

    return rc;
}

static int ct_begin(struct hsm_copyaction_private **phcp,
                    const struct hsm_action_item *hai)
{
    /*
     * Restore takes specific parameters. Call the same function w/ default
     * values for all other operations.
     */
    return ct_begin_restore(phcp, hai, -1, 0);
}

static int ct_fini(struct hsm_copyaction_private **phcp,
                   const struct hsm_action_item *hai, int hp_flags, int ct_rc)
{
    struct hsm_copyaction_private *hcp;
    char lstr[PATH_MAX];
    int rc;

    pho_info("Action completed, notifying coordinator "
             "cookie=%#jx, FID="DFID", hp_flags=%d err=%d",
             (uintmax_t)hai->hai_cookie, PFID(&hai->hai_fid),
             hp_flags, -ct_rc);

    ct_path_lustre(lstr, sizeof(lstr), opt.o_mnt, &hai->hai_fid);

    if (phcp == NULL || *phcp == NULL) {
        rc = llapi_hsm_action_begin(&hcp, ctdata, hai, -1, 0, true);
        if (rc < 0) {
            pho_error(rc, "llapi_hsm_action_begin() failed for '%s'",
                      lstr);
            return rc;
        }
        phcp = &hcp;
    }

    rc = llapi_hsm_action_end(phcp, &hai->hai_extent, hp_flags, abs(ct_rc));
    if (rc == -ECANCELED)
        pho_error(rc, "completed action on '%s' has been canceled: "
                      "cookie=%#jx, FID="DFID, lstr,
                  (uintmax_t)hai->hai_cookie, PFID(&hai->hai_fid));
    else if (rc < 0)
        pho_error(rc, "llapi_hsm_action_end() failed for '%s'", lstr);
    else
        pho_info("llapi_hsm_action_end() on '%s' ok (rc=%d)", lstr, rc);

    return rc;
}

static void hai_get_user_data(const struct hsm_action_item *hai,
                              struct buf *user_data)
{
    if (hai->hai_len - offsetof(struct hsm_action_item, hai_data) > 0) {
        user_data->data = (char *)hai->hai_data;
        user_data->len = hai->hai_len - offsetof(struct hsm_action_item,
                                                 hai_data);

    } else {
        user_data->data = NULL;
        user_data->len = 0;
    }
}

static int ct_archive(const struct hsm_action_item *hai,
                      UNUSED const long hal_flags)
{
    struct hsm_copyaction_private *hcp = NULL;
    struct llapi_layout *layout;
    char src[PATH_MAX];
    int hp_flags = 0;
    struct buf hints;
    char *oid = NULL;
    int src_fd = -1;
    int open_flags;
    int rcf = 0;
    int rc = 0;

    rc = ct_begin(&hcp, hai);
    if (rc < 0)
        goto fini_major;

    /* we fill archive so:
     * source = data FID
     * destination = lustre FID
     */
    ct_path_lustre(src, sizeof(src), opt.o_mnt, &hai->hai_dfid);

    pho_info("archiving '%s'", src);

    if (opt.o_dry_run) {
        rc = 0;
        goto fini_major;
    }

    src_fd = llapi_hsm_action_get_fd(hcp);
    if (src_fd < 0) {
        rc = src_fd;
        pho_error(rc, "cannot open '%s' for read", src);
        goto fini_major;
    }

    open_flags = O_WRONLY | O_NOFOLLOW;
    /* If extent is specified, don't truncate an old archived copy */
    open_flags |= O_CREAT | ((hai->hai_extent.length == (unsigned long long)-1)
                              ? O_TRUNC : 0);

    layout = llapi_layout_get_by_fd(src_fd, 0);
    if (!layout)
        pho_error(-errno, "cannot read layout of '%s'", src);

    hai_get_user_data(hai, &hints);

    /* Do phobos xfer */
    rc = phobos_op_put(&hai->hai_fid, src_fd, layout, &hints, &oid);
    llapi_layout_free(layout);
    pho_info("phobos_put (archive): rc=%d: %s", rc, strerror(-rc));
    if (rc)
        goto fini_major;
    /** @todo make clear rc management */

    if (!rc)
        goto out;

fini_major:
    err_major++;

    if (ct_is_retryable(rc))
        hp_flags |= HP_FLAG_RETRY;

out:
    if (!(src_fd < 0))
        close(src_fd);

    rcf = rc;
    rc = ct_fini(&hcp, hai, hp_flags, rcf);
    if (rc && !rcf) {
        int rc2;

        pho_error(rc, "failed to end ARCHIVE action, deleting '%s' from phobos",
                  oid);
        rc2 = phobos_op_del(&hai->hai_fid, &hints);
        if (rc2)
            pho_error(rc2, "Failed to remove '%s'", oid);
    }
    free(oid);

    return rc;
}

static int get_file_layout(const struct hsm_action_item *hai,
                           const struct buf *hints, int *open_flags,
                           struct llapi_layout **layout)
{
    int rc;

    rc = phobos_op_getlayout(&hai->hai_fid, NULL, hints, layout);

    *open_flags |= O_LOV_DELAY_CREATE;

    return rc;
}

static int ct_restore(const struct hsm_action_item *hai,
                      UNUSED const long hal_flags,
                      bool restore_lov)
{
    struct hsm_copyaction_private *hcp = NULL;
    struct llapi_layout *layout = NULL;
    char altobjid[PATH_MAX];
    char *altobj = NULL;
    struct lu_fid dfid;
    char dst[PATH_MAX];
    int open_flags = 0;
    int mdt_index = -1;
    int hp_flags = 0;
    struct buf hints;
    int dst_fd = -1;
    int rc;

    /*
     * we fill lustre so:
     * source = lustre FID in the backend
     * destination = data FID = volatile file
     */

    /* build backend file name from released file FID */
    rc = llapi_get_mdt_index_by_fid(opt.o_mnt_fd, &hai->hai_fid,
                                    &mdt_index);
    if (rc < 0) {
        pho_error(rc, "cannot get mdt index "DFID"",
                  PFID(&hai->hai_fid));
        return rc;
    }

    hai_get_user_data(hai, &hints);

    if (restore_lov) {
        rc = get_file_layout(hai, &hints, &open_flags, &layout);
        if (rc < 0) {
            pho_warn("Could not get file layout for "DFID", will proceed with "
                     "default striping.", PFID(&hai->hai_fid));
            /* use the default layout if not found in user metadata */
            restore_lov = false;
        }
    }

    /* start the restore operation */
    rc = ct_begin_restore(&hcp, hai, mdt_index, open_flags);
    if (rc < 0)
        goto fini;

    /* get the FID of the volatile file */
    rc = llapi_hsm_action_get_dfid(hcp, &dfid);
    if (rc < 0) {
        pho_error(rc, "restoring " DFID ", "
                      "cannot get FID of created volatile file",
                  PFID(&hai->hai_fid));
        goto fini;
    }

    snprintf(dst, sizeof(dst), DFID, PFID(&dfid));

    pho_info("restoring data from '" DFID "' to '%s'",
             PFID(&hai->hai_fid), dst);

    if (opt.o_dry_run) {
        rc = 0;
        goto fini;
    }

    dst_fd = llapi_hsm_action_get_fd(hcp);
    if (dst_fd < 0) {
        rc = dst_fd;
        pho_error(rc, "cannot open '%s' for write", dst);
        goto fini;
    }

    rc = ct_get_altobjid(hai, altobjid);
    if (!rc) {
        pho_verb("Found objid from xattr of "DFID" : %s",
                 PFID(&hai->hai_fid), altobjid);
        altobj = altobjid;
    } else {
        altobj = NULL;
    }

    if (restore_lov) {
        rc = ct_restore_layout(dst, dst_fd, layout);
        if (rc < 0)
            pho_warn("cannot restore file layout for '%s', will use default "
                     "(rc=%d)", dst, rc);
    }

    /* Do phobos xfer */
    rc = phobos_op_get(&hai->hai_fid, altobj, &hints, dst_fd);
    pho_info("phobos_get: rc=%d", rc);
    /** @todo make clear rc management */

fini:
    llapi_layout_free(layout);
    rc = ct_fini(&hcp, hai, hp_flags, rc);

    if (dst_fd >= 0)
        close(dst_fd);

    return rc;
}

static int ct_remove(const struct hsm_action_item *hai,
                     UNUSED const long hal_flags)
{
    struct hsm_copyaction_private *hcp = NULL;
    struct buf hints;
    int rc;

    rc = ct_begin(&hcp, hai);
    if (rc < 0)
        goto fini;

    pho_info("removing '" DFID "'", PFID(&hai->hai_fid));

    hai_get_user_data(hai, &hints);

    if (opt.o_dry_run) {
        rc = 0;
        goto fini;
    }

    rc = phobos_op_del(&hai->hai_fid, &hints);

fini:
    rc = ct_fini(&hcp, hai, 0, rc);

    return rc;
}

static int ct_process_item(struct hsm_action_item *hai, const long hal_flags)
{
    int rc = 0;

    if (opt.o_verbose >= LLAPI_MSG_INFO || opt.o_dry_run) {
        char fid[FID_LEN];

        sprintf(fid, DFID, PFID(&hai->hai_fid));
        pho_info("'%s' action %s reclen %d, cookie=%#jx",
                 fid, hsm_copytool_action2name(hai->hai_action),
                 hai->hai_len, (uintmax_t)hai->hai_cookie);
    }

    switch (hai->hai_action) {
        /* set err_major, minor inside these functions */
    case HSMA_ARCHIVE:
        rc = ct_archive(hai, hal_flags);
        break;
    case HSMA_RESTORE:
        rc = ct_restore(hai, hal_flags, opt.o_restore_lov);
        break;
    case HSMA_REMOVE:
        rc = ct_remove(hai, hal_flags);
        break;
    case HSMA_CANCEL:
        pho_warn("cancel not implemented for file system '%s'",
                 opt.o_mnt);
        /*
         * Don't report progress to coordinator for this cookie:
         * the copy function will get ECANCELED when reporting
         * progress.
         */
        err_minor++;
        return 0;
        break;
    default:
        rc = -EINVAL;
        pho_error(rc, "unknown action %d, on '%s'", hai->hai_action,
                  opt.o_mnt);
        err_minor++;
        ct_fini(NULL, hai, 0, rc);
    }

    return 0;
}

struct ct_th_data {
    long                    hal_flags;
    struct hsm_action_item *hai;
};

static void *ct_thread(void *data)
{
    struct ct_th_data *cttd = data;
    int rc;

    rc = ct_process_item(cttd->hai, cttd->hal_flags);

    free(cttd->hai);
    free(cttd);
    pthread_exit((void *)(intptr_t)rc);
}

static int ct_process_item_async(const struct hsm_action_item *hai,
                                 long hal_flags)
{
    pthread_attr_t attr;
    pthread_t thread;
    struct ct_th_data *data;
    int rc;

    data = malloc(sizeof(*data));
    if (data == NULL)
        return -ENOMEM;

    data->hai = malloc(hai->hai_len);
    if (data->hai == NULL) {
        free(data);
        return -ENOMEM;
    }

    memcpy(data->hai, hai, hai->hai_len);
    data->hal_flags = hal_flags;

    rc = pthread_attr_init(&attr);
    if (rc != 0) {
        pho_error(rc, "pthread_attr_init failed for '%s' service",
                  opt.o_mnt);
        free(data->hai);
        free(data);
        return -rc;
    }

    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    rc = pthread_create(&thread, &attr, ct_thread, data);
    if (rc != 0)
        pho_error(rc, "cannot create thread for '%s' service",
                  opt.o_mnt);

    pthread_attr_destroy(&attr);
    return 0;
}

static void handler(int signal)
{
    psignal(signal, "exiting");
    /*
     * If we don't clean up upon interrupt, umount thinks there's a ref
     * and doesn't remove us from mtab (EINPROGRESS). The lustre client
     * does successfully unmount and the mount is actually gone, but the
     * mtab entry remains. So this just makes mtab happier.
     */
    llapi_hsm_copytool_unregister(&ctdata);

    /* Also remove fifo upon signal as during normal/error exit */
    if (opt.o_event_fifo != NULL)
        llapi_hsm_unregister_event_fifo(opt.o_event_fifo);

    _exit(0);
}

static void create_pid_file(void)
{
    pid_t mypid = getpid();
    int mypid_str_len;
    char *mypid_str;
    int save_errno;
    int fd;
    int rc;

    if (!opt.o_pid_file)
        return;

    pho_verb("Pid file '%s'", opt.o_pid_file);

    fd = open(opt.o_pid_file, O_WRONLY | O_CREAT, 0640);
    if (fd == -1) {
        pho_error(-errno, "Could not create pid file '%s', will continue",
                  opt.o_pid_file);
        return;
    }

    rc = asprintf(&mypid_str, "%u", mypid);
    if (rc == -1) {
        pho_error(-errno, "Failed to allocate string");
        return;
    }

    mypid_str_len = strlen(mypid_str);
    rc = write(fd, mypid_str, mypid_str_len);
    save_errno = errno;
    close(fd);
    free(mypid_str);
    if (rc != mypid_str_len || rc == -1)
        pho_error(-save_errno, "Failed to write pid to '%s', will continue",
                  opt.o_pid_file);
}

/* Daemon waits for messages from the kernel; run it in the background. */
static int ct_run(void)
{
    struct sigaction cleanup_sigaction;
    int archive_ids_count;
    int *archive_ids;
    int rc;

    if (opt.o_daemonize) {
        rc = daemon(1, 1);
        if (rc < 0) {
            rc = -errno;
            pho_error(rc, "cannot daemonize");
            return rc;
        }
    }
    ct_log_configure(&opt);

    create_pid_file();

    setbuf(stdout, NULL);

    if (opt.o_event_fifo != NULL) {
        rc = llapi_hsm_register_event_fifo(opt.o_event_fifo);
        if (rc < 0) {
            pho_error(rc, "failed to register event fifo");
            return rc;
        }
        // llapi_error_callback_set(llapi_hsm_log_error);
    }

    get_archive_id_array(opt.o_archive_ids, &archive_ids, &archive_ids_count);
    rc = llapi_hsm_copytool_register(&ctdata, opt.o_mnt,
                                     archive_ids_count, archive_ids, 0);
    g_array_free(opt.o_archive_ids, true);
    if (rc < 0) {
        pho_error(rc, "failed to register service as a copytool");
        return rc;
    }

    memset(&cleanup_sigaction, 0, sizeof(cleanup_sigaction));
    cleanup_sigaction.sa_handler = handler;
    sigemptyset(&cleanup_sigaction.sa_mask);
    sigaction(SIGINT, &cleanup_sigaction, NULL);
    sigaction(SIGTERM, &cleanup_sigaction, NULL);

    while (1) {
        struct hsm_action_list *hal;
        struct hsm_action_item *hai;
        unsigned int i = 0;
        int msgsize;

        pho_info("waiting for message from kernel");

        rc = llapi_hsm_copytool_recv(ctdata, &hal, &msgsize);
        if (rc == -ESHUTDOWN) {
            pho_warn("coordinator stopped, shutting down");
            break;
        } else if (rc < 0) {
            pho_error(rc, "llapi_hsm_copytool_recv failed");
            err_major++;
            if (opt.o_abort_on_error)
                break;
            else
                continue;
        }

        pho_info("copytool fs=%s archive#=%d item_count=%d",
                 hal->hal_fsname, hal->hal_archive_id, hal->hal_count);

        if (strcmp(hal->hal_fsname, fs_name) != 0) {
            rc = -EINVAL;
            pho_error(rc, "'%s' invalid fs name, expecting: %s",
                      hal->hal_fsname, fs_name);
            err_major++;
            if (opt.o_abort_on_error)
                break;
            else
                continue;
        }

        hai = hai_first(hal);
        while (++i <= hal->hal_count) {
            if ((char *)hai - (char *)hal > msgsize) {
                rc = -EPROTO;
                pho_error(rc, "'%s' item %d past end of message",
                          opt.o_mnt, i);
                err_major++;
                break;
            }
            rc = ct_process_item_async(hai, hal->hal_flags);
            if (rc < 0)
                pho_error(rc, "error while processing '"DFID"'",
                          PFID(&hai->hai_fid));
            if (opt.o_abort_on_error && err_major)
                break;
            hai = hai_next(hai);
        }

        if (opt.o_abort_on_error && err_major)
            break;
    }

    llapi_hsm_copytool_unregister(&ctdata);
    if (opt.o_event_fifo != NULL)
        llapi_hsm_unregister_event_fifo(opt.o_event_fifo);

    return rc;
}

static int ct_setup(void)
{
    int rc;

    /* set llapi message level */
    llapi_msg_set_level(opt.o_verbose);

    rc = llapi_search_fsname(opt.o_mnt, fs_name);
    if (rc < 0) {
        pho_error(rc, "cannot find a Lustre filesystem mounted at '%s'",
                  opt.o_mnt);
        return rc;
    }

    opt.o_mnt_fd = open(opt.o_mnt, O_RDONLY);
    if (opt.o_mnt_fd < 0) {
        rc = -errno;
        pho_error(rc, "cannot open mount point at '%s'", opt.o_mnt);
        return rc;
    }

    return rc;
}

static int ct_cleanup(void)
{
    int rc;

    if (opt.o_mnt_fd >= 0) {
        rc = close(opt.o_mnt_fd);
        if (rc < 0) {
            rc = -errno;
            pho_error(rc, "cannot close mount point");
            return rc;
        }
    }

    return 0;
}

int main(int argc, char **argv)
{
    int rc;

#ifdef HAVE_PHOBOS_INIT
    phobos_init();
    atexit(phobos_fini);
#endif

    strncpy(trusted_fuid_xattr, XATTR_TRUSTED_FUID_XATTR_DEFAULT, MAXNAMLEN);

    snprintf(cmd_name, sizeof(cmd_name), "%s", basename(argv[0]));
    rc = ct_parseopts(argc, argv);
    if (rc < 0) {
        pho_error(rc, "Failed to parse command line arguments. "
                  "Try '%s --help' for more information",
                  cmd_name);
        return -rc;
    }

    rc = ct_setup();
    if (rc < 0)
        goto error_cleanup;

    /* Trace the lustre fsname */
    pho_info("fs_name=%s", fs_name);
    pho_info("trusted_fuid_xattr=%s", trusted_fuid_xattr);

    rc = ct_run();

    pho_info("process finished, errs: %d major, %d minor, rc=%d (%s)",
             err_major, err_minor, rc, strerror(-rc));

error_cleanup:
    ct_cleanup();

    return -rc;
}

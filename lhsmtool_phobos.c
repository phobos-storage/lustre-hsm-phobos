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
/* HSM copytool program for Phobos HSM's.
 *
 * An HSM copytool daemon acts on action requests from Lustre to copy files
 * to and from an HSM archive system. This one in particular makes regular
 * call to Phobos
 *
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
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

#include "layout.h"

#define LL_HSM_ORIGIN_MAX_ARCHIVE (sizeof(__u32) * 8)
#define XATTR_TRUSTED_PREFIX      "trusted."

#define XATTR_TRUSTED_FUID_XATTR_DEFAULT "trusted.hsm_fuid"

#define HINT_HSM_FUID "hsm_fuid"

#define MAXTAGS 10

#define UNUSED __attribute__((unused))

/* Progress reporting period */
#define REPORT_INTERVAL_DEFAULT 30
/* HSM hash subdir permissions */
#define DIR_PERM 0700
/* HSM hash file permissions */
#define FILE_PERM 0600

#define ONE_MB 0x100000

#ifndef NSEC_PER_SEC
# define NSEC_PER_SEC 1000000000UL
#endif

#define NB_HINTS_MAX 10

enum ct_action {
    CA_IMPORT = 1,
    CA_REBIND,
    CA_MAXSEQ,
};

struct options {
    int              o_daemonize;
    int              o_dry_run;
    int              o_abort_on_error;
    int              o_verbose;
    int              o_archive_id_used;
    int              o_archive_id_cnt;
    int             *o_archive_id;
    char            *o_event_fifo;
    char            *o_mnt;
    int              o_mnt_fd;
    enum rsc_family  o_default_family;
    bool             o_restore_lov;
    const char      *o_pid_file;
};

/* everything else is zeroed */
struct options opt = {
    .o_verbose         = LLAPI_MSG_INFO,
    .o_default_family  = PHO_RSC_INVAL,
    .o_restore_lov     = false,
};

struct buf {
    char *data;
    size_t len;
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

#define CT_ERROR(_rc, _format, ...)       \
    llapi_error(LLAPI_MSG_ERROR, _rc,     \
                "%f %s[%ld]: "_format,    \
                ct_now(), cmd_name, syscall(SYS_gettid), ## __VA_ARGS__)

#define CT_DEBUG(_format, ...)                           \
    llapi_error(LLAPI_MSG_DEBUG | LLAPI_MSG_NO_ERRNO, 0, \
                "%f %s[%ld]: "_format,                   \
                ct_now(), cmd_name, syscall(SYS_gettid), ## __VA_ARGS__)

#define CT_WARN(_format, ...)                           \
    llapi_error(LLAPI_MSG_WARN | LLAPI_MSG_NO_ERRNO, 0, \
                "%f %s[%ld]: "_format,                  \
                ct_now(), cmd_name, syscall(SYS_gettid), ## __VA_ARGS__)

#define CT_TRACE(_format, ...)                           \
    llapi_error(LLAPI_MSG_INFO | LLAPI_MSG_NO_ERRNO, 0,  \
                "%f %s[%ld] %s: "_format,                \
                ct_now(), cmd_name, syscall(SYS_gettid), \
                __func__, ## __VA_ARGS__)

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
#ifdef LLAPI_LAYOUT_SET_BY_FD
            "    -l, --restore-lov            Use the striping that the file "
            "had when archived (off by default)\n"
#endif
        , cmd_name);

    exit(rc);
}

#ifdef LLAPI_LAYOUT_SET_BY_FD
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
#ifdef LLAPI_LAYOUT_SET_BY_FD
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
    bool all_id = false;
    int rc;
    int c;
    int i;

    optind = 0;

    opt.o_archive_id_cnt = LL_HSM_ORIGIN_MAX_ARCHIVE;
    opt.o_archive_id = malloc(opt.o_archive_id_cnt *
                              sizeof(*opt.o_archive_id));
    if (opt.o_archive_id == NULL)
        return -ENOMEM;

repeat:
    while ((c = getopt_long(argc, argv, GETOPTS_STRING,
                            long_opts, NULL)) != -1) {
        switch (c) {
        case 'A': {
            char *end = NULL;
            int val = strtol(optarg, &end, 10);

            if (*end != '\0') {
                rc = -EINVAL;
                CT_ERROR(rc, "invalid archive-id: '%s'",
                         optarg);
                return rc;
            }
            /* if archiveID is zero, any archiveID is accepted */
            if (all_id == true)
                goto repeat;

            if (val == 0) {
                free(opt.o_archive_id);
                opt.o_archive_id = NULL;
                opt.o_archive_id_cnt = 0;
                opt.o_archive_id_used = 0;
                all_id = true;
                CT_WARN("archive-id = 0 is found,"
                        " any backend will be served\n");
                goto repeat;
            }

            /* skip the duplicated id */
            for (i = 0; i < opt.o_archive_id_used; i++) {
                if (opt.o_archive_id[i] == val)
                    goto repeat;
            }
            /* extend the space */
            if (opt.o_archive_id_used >= opt.o_archive_id_cnt) {
                int *tmp;

                opt.o_archive_id_cnt *= 2;
                tmp = realloc(opt.o_archive_id,
                              sizeof(*opt.o_archive_id) *
                              opt.o_archive_id_cnt);
                if (tmp == NULL)
                    return -ENOMEM;

                opt.o_archive_id = tmp;
            }

            opt.o_archive_id[opt.o_archive_id_used++] = val;
            break;
        }
        case 'f':
            opt.o_event_fifo = optarg;
            break;
        case 'F':
            opt.o_default_family = str2rsc_family(optarg);
            break;
        case 'h':
            usage(0);
            break;
#ifdef LLAPI_LAYOUT_SET_BY_FD
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
        CT_ERROR(rc, "no mount point specified");
        return rc;
    }

    opt.o_mnt = argv[optind];
    opt.o_mnt_fd = -1;

    CT_TRACE("mount_point=%s", opt.o_mnt);

    return 0;
}

/*
 * A strtok-r based function for parsing the
 * hints provided during an archive request
 */

#define HINTMAX 80

struct hinttab {
    char k[HINTMAX];
    char v[HINTMAX];
};

static int process_hints(const struct buf *hints,
                         int hinttablen,
                         struct hinttab *hinttab)
{
    const char comma[2] = ",";
    const char equal[2] = "=";
    char work2[HINTMAX];
    char *saveptr1;
    char *saveptr2;
    char *token1;
    char *token2;
    int pos1 = 0;
    int pos2 = 0;
    char *work1;

    /* No hints to parse */
    if (!hints->data)
        return 0;

    work1 = strndup(hints->data, hints->len);

    /* get the first token */
    token1 = strtok_r(work1, comma, &saveptr1);

    /* walk through other tokens */
    while (token1 != NULL) {
        /*
         * strtok_r corrupts the strings
         * we must work on a copy
         */
        /* FIXME this doesn't work if the token is larger than 80 characters...
         * This function needs a bit of refactoring.
         */
        strncpy(work2, token1, HINTMAX);
        pos2 = 0;

        /* Second strtok, to get equal */
        token2 = strtok_r(work2, equal, &saveptr2);

        while (token2 != NULL) {
            if (pos2 == 0)
                strncpy(hinttab[pos1].k, token2, HINTMAX);
            else if (pos2 == 1)
                strncpy(hinttab[pos1].v, token2, HINTMAX);
            else
                break;

            token2 = strtok_r(NULL, equal, &saveptr2);
            pos2 += 1;
        }

        token1 = strtok_r(NULL, comma, &saveptr1);
        pos1 += 1;

        if (pos1 == hinttablen)
            return pos1;
    }

    return pos1;
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
    struct hinttab hinttab[NB_HINTS_MAX];
    struct pho_xfer_desc xfer = {0};
    char objid[MAXNAMLEN];
    bool objset = false;
    char *obj = NULL;
    int nbhints;
    int rc;

    if (hints->data) {
        int i = 0;

        CT_TRACE("hints provided hints='%s', len=%lu",
                 hints->data, hints->len);
        nbhints = process_hints(hints, NB_HINTS_MAX, hinttab);

        for (i = 0 ; i < nbhints; i++) {
            CT_TRACE("hints #%d  key='%s' val='%s'",
                     i, hinttab[i].k, hinttab[i].v);

            if (!strncmp(hinttab[i].k, HINT_HSM_FUID, HINTMAX)) {
                /* Deal with storage family */
                obj = hinttab[i].v;
                objset = true;
            }
        }
    }

    if (!objset) {
        rc = fid2objid(fid, objid);
        if (rc < 0)
            return rc;

        obj = objid;
    }

    /* DO THE DELETE */
    memset(&xfer, 0, sizeof(xfer));
    xfer.xd_op = PHO_XFER_OP_DEL;
    xfer.xd_objid = obj;

    rc = phobos_delete(&xfer, 1);

    /* Cleanup and exit */
    pho_xfer_desc_clean(&xfer);

    if (rc)
        CT_ERROR(rc, "DEL failed");

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
        rc = pho_attr_set(attrs, id_str, component_str);
    else
        rc = pho_attr_set(attrs, "layout", component_str);

    CT_TRACE("adding layout information key='%s', value='%s'",
             id_str ? : "layout", component_str);

    free(component_str);
out_free:
    free(id_str);
    if (!rc)
        return LLAPI_LAYOUT_ITER_CONT;

    return rc;
}

static int phobos_op_put(const struct lu_fid *fid,
                         char *altobjid,
                         const int fd,
                         struct llapi_layout *layout,
                         const struct buf *hints,
                         char **oid)
{
    struct hinttab hinttab[NB_HINTS_MAX];
    struct pho_xfer_desc xfer = {0};
    struct pho_attrs attrs = {0};
    char objid[MAXNAMLEN];
    char *obj = NULL;
    struct stat st;
    int nbhints;
    int i = 0;
    int rc;

    /* If provided altobjid as objectid, if not set by hints */
    if (altobjid) {
        obj = altobjid;
    } else {
        rc = fid2objid(fid, objid);
        if (rc < 0)
            return rc;
        obj = objid;
    }
    /* only used for logging, not an error if allocation failed */
    *oid = strdup(obj);

    /**
     * @todo:
     *    - management of the size of string objid
     */

    rc = pho_attr_set(&attrs, "program", "copytool");
    if (rc)
        return rc;

    if (layout) {
        rc = llapi_layout_comp_iterate(layout, component_to_attr,
                                       (void *)&attrs);
        if (rc < 0)
            return -errno;
    }

    memset(&xfer, 0, sizeof(xfer));
    xfer.xd_op = PHO_XFER_OP_PUT;
    xfer.xd_fd = fd;
    xfer.xd_flags = 0;

    /* set oid in xattr for later use */
    rc = fsetxattr(xfer.xd_fd, trusted_fuid_xattr, obj, strlen(obj),
                   XATTR_CREATE);
    if (rc)
        CT_TRACE("failed to write xattr: %s\n", strerror(errno));

    /* fstat on lustre fd seems to fail */
    fstat(xfer.xd_fd, &st);
    xfer.xd_params.put.size = st.st_size;

    /* Using default family (can be amened later by a hint) */
    xfer.xd_params.put.family = opt.o_default_family;

    /* Use content of hints to modify fields in xfer_desc */
    if (hints->data) {
        CT_TRACE("hints provided hints='%s', len=%lu",
                 hints->data, hints->len);
        nbhints = process_hints(hints, NB_HINTS_MAX, hinttab);

        for (i = 0 ; i < nbhints; i++) {
            CT_TRACE("hints #%d  key='%s' val='%s'",
                     i, hinttab[i].k, hinttab[i].v);

            if (!strncmp(hinttab[i].k, "family", HINTMAX)) {
                /* Deal with storage family */
                xfer.xd_params.put.family = str2rsc_family(hinttab[i].v);

                if (xfer.xd_params.put.family == PHO_RSC_INVAL)
                    CT_TRACE("unknown hint '%s'",  hinttab[i].k);

            } else if (!strncmp(hinttab[i].k, "layout", HINTMAX)) {
                /* Deal with storage layout */
                xfer.xd_params.put.layout_name = hinttab[i].v;

            } else if (!strncmp(hinttab[i].k, "alias", HINTMAX)) {
                xfer.xd_params.put.alias = hinttab[i].v;

            } else if (!strncmp(hinttab[i].k, "tag", HINTMAX)) {
                /* Deal with storage tags */
                if (xfer.xd_params.put.tags.n_tags == 0) {
                    xfer.xd_params.put.tags.tags = malloc(sizeof(char **));

                    if (!xfer.xd_params.put.tags.tags)
                        CT_ERROR(errno, "Out of memory !! Malloc failed");
                }

                /* I use this #define to deal with loooooong structures names */
#define EASIER xfer.xd_params.put.tags.tags[xfer.xd_params.put.tags.n_tags]
                EASIER = malloc(HINTMAX);
                if (!EASIER)
                    CT_ERROR(errno, "Out of memory !! Malloc failed");

                strncpy(EASIER, hinttab[i].v, HINTMAX);
                xfer.xd_params.put.tags.n_tags += 1;
#undef EASIER
            } else {
                CT_TRACE("unknown hint '%s'",  hinttab[i].k);
            }
        }
    }

    /* Finalize xfer_desc and to the PUT operation */
    xfer.xd_objid = obj;
    xfer.xd_attrs = attrs;
    xfer.xd_params.put.overwrite = true;
    rc = phobos_put(&xfer, 1, NULL, NULL);

    /* free the tags as well */
    pho_xfer_desc_clean(&xfer);

    if (rc)
        CT_ERROR(rc, "PUT failed");

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
        CT_ERROR(rc, "GET failed");

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
        CT_ERROR(rc, "GETMD failed");
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
        CT_ERROR(rc, "llapi_hsm_action_begin() on '%s' failed", src);
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

    CT_TRACE("Action completed, notifying coordinator "
             "cookie=%#jx, FID="DFID", hp_flags=%d err=%d",
             (uintmax_t)hai->hai_cookie, PFID(&hai->hai_fid),
             hp_flags, -ct_rc);

    ct_path_lustre(lstr, sizeof(lstr), opt.o_mnt, &hai->hai_fid);

    if (phcp == NULL || *phcp == NULL) {
        rc = llapi_hsm_action_begin(&hcp, ctdata, hai, -1, 0, true);
        if (rc < 0) {
            CT_ERROR(rc, "llapi_hsm_action_begin() on '%s' failed",
                     lstr);
            return rc;
        }
        phcp = &hcp;
    }

    rc = llapi_hsm_action_end(phcp, &hai->hai_extent, hp_flags, abs(ct_rc));
    if (rc == -ECANCELED)
        CT_ERROR(rc, "completed action on '%s' has been canceled: "
                 "cookie=%#jx, FID="DFID, lstr,
                 (uintmax_t)hai->hai_cookie, PFID(&hai->hai_fid));
    else if (rc < 0)
        CT_ERROR(rc, "llapi_hsm_action_end() on '%s' failed", lstr);
    else
        CT_TRACE("llapi_hsm_action_end() on '%s' ok (rc=%d)", lstr, rc);

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

    CT_TRACE("archiving '%s'", src);

    if (opt.o_dry_run) {
        rc = 0;
        goto fini_major;
    }

    src_fd = llapi_hsm_action_get_fd(hcp);
    if (src_fd < 0) {
        rc = src_fd;
        CT_ERROR(rc, "cannot open '%s' for read", src);
        goto fini_major;
    }

    open_flags = O_WRONLY | O_NOFOLLOW;
    /* If extent is specified, don't truncate an old archived copy */
    open_flags |= O_CREAT | ((hai->hai_extent.length == (unsigned long long)-1)
                              ? O_TRUNC : 0);

    layout = llapi_layout_get_by_fd(src_fd, 0);
    if (!layout)
        CT_ERROR(-errno, "cannot read layout of '%s'", src);

    hai_get_user_data(hai, &hints);

    /* Do phobos xfer */
    rc = phobos_op_put(&hai->hai_fid, NULL, src_fd, layout, &hints, &oid);
    CT_TRACE("phobos_put (archive): rc=%d", rc);
    if (rc)
        goto fini_major;
    /** @todo make clear rc management */

    CT_TRACE("data archiving for '%s' done", src);
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

        CT_ERROR(rc, "failed to end ARCHIVE action, deleting '%s' from phobos",
                 oid);
        rc2 = phobos_op_del(&hai->hai_fid, &hints);
        if (rc2)
            CT_ERROR(rc2, "Failed to remove '%s'", oid);
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
        CT_ERROR(rc, "cannot get mdt index "DFID"",
                 PFID(&hai->hai_fid));
        return rc;
    }

    hai_get_user_data(hai, &hints);

    if (restore_lov) {
        rc = get_file_layout(hai, &hints, &open_flags, &layout);
        if (rc < 0) {
            CT_WARN("Could not get file layout for "DFID", will proceed with "
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
        CT_ERROR(rc, "restoring "DFID", "
                 "cannot get FID of created volatile file",
                 PFID(&hai->hai_fid));
        goto fini;
    }

    /* build volatile "file name", for messages */
    snprintf(dst, sizeof(dst), DFID, PFID(&dfid));

    CT_TRACE("restoring data from to '%s'", dst);

    if (opt.o_dry_run) {
        rc = 0;
        goto fini;
    }

    dst_fd = llapi_hsm_action_get_fd(hcp);
    if (dst_fd < 0) {
        rc = dst_fd;
        CT_ERROR(rc, "cannot open '%s' for write", dst);
        goto fini;
    }

    rc = ct_get_altobjid(hai, altobjid);
    if (!rc) {
        CT_TRACE("Found objid as xattr for "DFID" : %s",
                 PFID(&hai->hai_fid), altobjid);
        altobj = altobjid;
    } else {
        altobj = NULL;
    }

    if (restore_lov) {
        rc = ct_restore_layout(dst, dst_fd, layout);
        if (rc < 0)
            CT_WARN("cannot restore file layout for '%s', will use default "
                    "(rc=%d)", dst, rc);
    }

    /* Do phobos xfer */
    rc = phobos_op_get(&hai->hai_fid, altobj, &hints, dst_fd);
    CT_TRACE("phobos_get (restore): rc=%d", rc);
    /** @todo make clear rc management */

    CT_TRACE("data restore to '%s' done", dst);

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

    CT_TRACE("removing an entry");

    hai_get_user_data(hai, &hints);

    /** @todo : add remove as it will be available in Phobos */
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
        /* Print the original path */
        long long recno = -1;
        char path[PATH_MAX];
        int linkno = 0;
        char fid[128];

        sprintf(fid, DFID, PFID(&hai->hai_fid));
        CT_TRACE("'%s' action %s reclen %d, cookie=%#jx",
                 fid, hsm_copytool_action2name(hai->hai_action),
                 hai->hai_len, (uintmax_t)hai->hai_cookie);
        rc = llapi_fid2path(opt.o_mnt, fid, path,
                            sizeof(path), &recno, &linkno);
        if (rc < 0)
            CT_ERROR(rc, "cannot get path of FID %s", fid);
        else
            CT_TRACE("processing file '%s'", path);
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
        CT_TRACE("cancel not implemented for file system '%s'",
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
        CT_ERROR(rc, "unknown action %d, on '%s'", hai->hai_action,
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
        CT_ERROR(rc, "pthread_attr_init failed for '%s' service",
                 opt.o_mnt);
        free(data->hai);
        free(data);
        return -rc;
    }

    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    rc = pthread_create(&thread, &attr, ct_thread, data);
    if (rc != 0)
        CT_ERROR(rc, "cannot create thread for '%s' service",
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

    CT_TRACE("Pid file '%s'", opt.o_pid_file);

    fd = open(opt.o_pid_file, O_WRONLY | O_CREAT, 0640);
    if (fd == -1) {
        CT_ERROR(-errno, "Could not create pid file '%s', will continue",
                 opt.o_pid_file);
        return;
    }

    rc = asprintf(&mypid_str, "%u", mypid);
    if (rc == -1) {
        CT_ERROR(-errno, "Failed to allocate string");
        return;
    }

    mypid_str_len = strlen(mypid_str);
    rc = write(fd, mypid_str, mypid_str_len);
    save_errno = errno;
    close(fd);
    free(mypid_str);
    if (rc != mypid_str_len || rc == -1)
        CT_ERROR(-save_errno, "Failed to write pid to '%s', will continue",
                 opt.o_pid_file);
}

/* Daemon waits for messages from the kernel; run it in the background. */
static int ct_run(void)
{
    struct sigaction cleanup_sigaction;
    int rc;

    if (opt.o_daemonize) {
        rc = daemon(1, 1);
        if (rc < 0) {
            rc = -errno;
            CT_ERROR(rc, "cannot daemonize");
            return rc;
        }
    }

    create_pid_file();

    setbuf(stdout, NULL);

    if (opt.o_event_fifo != NULL) {
        rc = llapi_hsm_register_event_fifo(opt.o_event_fifo);
        if (rc < 0) {
            CT_ERROR(rc, "failed to register event fifo");
            return rc;
        }
        llapi_error_callback_set(llapi_hsm_log_error);
    }

    rc = llapi_hsm_copytool_register(&ctdata, opt.o_mnt,
                                     opt.o_archive_id_used,
                                     opt.o_archive_id, 0);
    if (rc < 0) {
        CT_ERROR(rc, "cannot start copytool interface");
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

        CT_TRACE("waiting for message from kernel");

        rc = llapi_hsm_copytool_recv(ctdata, &hal, &msgsize);
        if (rc == -ESHUTDOWN) {
            CT_TRACE("shutting down");
            break;
        } else if (rc < 0) {
            CT_WARN("cannot receive action list: %s",
                    strerror(-rc));
            err_major++;
            if (opt.o_abort_on_error)
                break;
            else
                continue;
        }

        CT_TRACE("copytool fs=%s archive#=%d item_count=%d",
                 hal->hal_fsname, hal->hal_archive_id, hal->hal_count);

        if (strcmp(hal->hal_fsname, fs_name) != 0) {
            rc = -EINVAL;
            CT_ERROR(rc, "'%s' invalid fs name, expecting: %s",
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
                CT_ERROR(rc,
                         "'%s' item %d past end of message!",
                         opt.o_mnt, i);
                err_major++;
                break;
            }
            rc = ct_process_item_async(hai, hal->hal_flags);
            if (rc < 0)
                CT_ERROR(rc, "'%s' item %d process",
                         opt.o_mnt, i);
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
        CT_ERROR(rc, "cannot find a Lustre filesystem mounted at '%s'",
                 opt.o_mnt);
        return rc;
    }

    opt.o_mnt_fd = open(opt.o_mnt, O_RDONLY);
    if (opt.o_mnt_fd < 0) {
        rc = -errno;
        CT_ERROR(rc, "cannot open mount point at '%s'",
                 opt.o_mnt);
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
            CT_ERROR(rc, "cannot close mount point");
            return rc;
        }
    }

    if (opt.o_archive_id_cnt > 0) {
        free(opt.o_archive_id);
        opt.o_archive_id = NULL;
        opt.o_archive_id_cnt = 0;
    }

    return 0;
}

int main(int argc, char **argv)
{
    int rc;

    phobos_init();
    atexit(phobos_fini);

    strncpy(trusted_fuid_xattr, XATTR_TRUSTED_FUID_XATTR_DEFAULT, MAXNAMLEN);

    snprintf(cmd_name, sizeof(cmd_name), "%s", basename(argv[0]));
    rc = ct_parseopts(argc, argv);
    if (rc < 0) {
        CT_WARN("try '%s --help' for more information", cmd_name);
        return -rc;
    }

    rc = ct_setup();
    if (rc < 0)
        goto error_cleanup;

    /* Trace the lustre fsname */
    CT_TRACE("fs_name=%s", fs_name);
    CT_TRACE("trusted_fuid_xattr=%s", trusted_fuid_xattr);

    rc = ct_run();

    CT_TRACE("process finished, errs: %d major, %d minor, "
             "rc=%d (%s)", err_major, err_minor, rc,
             strerror(-rc));

error_cleanup:
    ct_cleanup();

    return -rc;
}

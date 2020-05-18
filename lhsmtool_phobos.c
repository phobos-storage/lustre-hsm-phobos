/*
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
 *     alternatives
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
#include "pho_common.h"
#include "phobos_store.h"

#define LL_HSM_ORIGIN_MAX_ARCHIVE    (sizeof(__u32) * 8)
#define XATTR_TRUSTED_PREFIX    "trusted."

#define XATTR_TRUSTED_HSM_FSUID_DEFAULT	"trusted.hsm_fuid"

/* Progress reporting period */
#define REPORT_INTERVAL_DEFAULT 30
/* HSM hash subdir permissions */
#define DIR_PERM S_IRWXU
/* HSM hash file permissions */
#define FILE_PERM (S_IRUSR | S_IWUSR)

#define ONE_MB 0x100000

#ifndef NSEC_PER_SEC
# define NSEC_PER_SEC 1000000000UL
#endif

enum ct_action {
	CA_IMPORT = 1,
	CA_REBIND,
	CA_MAXSEQ,
};

struct options {
	int			 o_daemonize;
	int			 o_dry_run;
	int			 o_abort_on_error;
	int			 o_verbose;
	int			 o_archive_id_used;
	int			 o_archive_id_cnt;
	int			*o_archive_id;
	int			 o_report_int;
	unsigned long long	 o_bandwidth;
	size_t			 o_chunk_size;
	char			*o_event_fifo;
	char			*o_mnt;
	int			 o_mnt_fd;
};

/* everything else is zeroed */
struct options opt = {
	.o_verbose = LLAPI_MSG_INFO,
	.o_report_int = REPORT_INTERVAL_DEFAULT,
	.o_chunk_size = ONE_MB,
};

/* hsm_copytool_private will hold an open FD on the lustre mount point
 * for us. Additionally open one on the archive FS root to make sure
 * it doesn't drop out from under us (and remind the admin to shutdown
 * the copytool before unmounting). */

static int err_major;
static int err_minor;

static char cmd_name[PATH_MAX];
static char fs_name[MAX_OBD_NAME + 1];
static char trusted_hsm_fsuid[MAXNAMLEN];

static struct hsm_copytool_private *ctdata;

static inline double ct_now(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);

	return tv.tv_sec + 0.000001 * tv.tv_usec;
}

#define CT_ERROR(_rc, _format, ...)					\
	llapi_error(LLAPI_MSG_ERROR, _rc,				\
		    "%f %s[%ld]: "_format,				\
		    ct_now(), cmd_name, syscall(SYS_gettid), ## __VA_ARGS__)

#define CT_DEBUG(_format, ...)						\
	llapi_error(LLAPI_MSG_DEBUG | LLAPI_MSG_NO_ERRNO, 0,		\
		    "%f %s[%ld]: "_format,				\
		    ct_now(), cmd_name, syscall(SYS_gettid), ## __VA_ARGS__)

#define CT_WARN(_format, ...) \
	llapi_error(LLAPI_MSG_WARN | LLAPI_MSG_NO_ERRNO, 0,		\
		    "%f %s[%ld]: "_format,				\
		    ct_now(), cmd_name, syscall(SYS_gettid), ## __VA_ARGS__)

#define CT_TRACE(_format, ...)						\
	llapi_error(LLAPI_MSG_INFO | LLAPI_MSG_NO_ERRNO, 0,		\
		    "%f %s[%ld]: "_format,				\
		    ct_now(), cmd_name, syscall(SYS_gettid), ## __VA_ARGS__)

static void usage(const char *name, int rc)
{
	fprintf(stdout,
	" Usage: %s [options]... <mode> <lustre_mount_point>\n"
	"The Lustre HSM Posix copy tool can be used as a daemon or "
	"as a command line tool\n"
	"The Lustre HSM daemon acts on action requests from Lustre\n"
	"to copy files to and from an HSM archive system.\n"
	"This Phobos-flavored daemon makes calls toà a Phobos storage\n"
	"   --daemon            Daemon mode, run in background\n"
	" Options:\n"
	"   --no-attr           Don't copy file attributes\n"
	"   --no-shadow         Don't create shadow namespace in archive\n"
	"   --no-xattr          Don't copy file extended attributes\n"
	"The Lustre HSM tool performs administrator-type actions\n"
	"on a Lustre HSM archive.\n"
	"This Phobos-flavored tool can link an existing HSM namespace\n"
	"into a Lustre filesystem.\n"
	" Usage:\n"
	"   --abort-on-error          Abort operation on major error\n"
	"   -A, --archive <#>         Archive number (repeatable)\n"
	"   -b, --bandwidth <bw>      Limit I/O bandwidth (unit can be used\n,"
	"                             default is MB)\n"
	"   --dry-run                 Don't run, just show what would be done\n"
	"   -c, --chunk-size <sz>     I/O size used during data copy\n"
	"                             (unit can be used, default is MB)\n"
	"   -f, --event-fifo <path>   Write events stream to fifo\n"
	"   -q, --quiet               Produce less verbose output\n"
	"   -t, --hsm_fsuid           change value of xattr for restore\n"
	"   -u, --update-interval <s> Interval between progress reports sent\n"
	"                             to Coordinator\n"
	"   -v, --verbose             Produce more verbose output\n",
	cmd_name);

	exit(rc);
}

static int ct_parseopts(int argc, char * const *argv)
{
	struct option long_opts[] = {
	{ .val = 1,	.name = "abort-on-error",
	  .flag = &opt.o_abort_on_error,	.has_arg = no_argument },
	{ .val = 1,	.name = "abort_on_error",
	  .flag = &opt.o_abort_on_error,	.has_arg = no_argument },
	{ .val = 'A',	.name = "archive",	.has_arg = required_argument },
	{ .val = 'b',	.name = "bandwidth",	.has_arg = required_argument },
	{ .val = 'c',	.name = "chunk-size",	.has_arg = required_argument },
	{ .val = 'c',	.name = "chunk_size",	.has_arg = required_argument },
	{ .val = 1,	.name = "daemon",	.has_arg = no_argument,
	  .flag = &opt.o_daemonize },
	{ .val = 'f',	.name = "event-fifo",	.has_arg = required_argument },
	{ .val = 'f',	.name = "event_fifo",	.has_arg = required_argument },
	{ .val = 1,	.name = "dry-run",	.has_arg = no_argument,
	  .flag = &opt.o_dry_run },
	{ .val = 'h',	.name = "help",		.has_arg = no_argument },
	{ .val = 'i',	.name = "import",	.has_arg = no_argument },
	{ .val = 'M',	.name = "max-sequence",	.has_arg = no_argument },
	{ .val = 'M',	.name = "max_sequence",	.has_arg = no_argument },
	{ .val = 'p',	.name = "hsm-root",	.has_arg = required_argument },
	{ .val = 'q',	.name = "quiet",	.has_arg = no_argument },
	{ .val = 'r',	.name = "rebind",	.has_arg = no_argument },
	{ .val = 'u',	.name = "update-interval",
						.has_arg = required_argument },
	{ .val = 'u',	.name = "update_interval",
						.has_arg = required_argument },
	{ .val = 'v',	.name = "verbose",	.has_arg = no_argument },
	{ .val = 't',	.name = "hsm_fsuid", 	.has_arg = required_argument},
	{ .name = NULL } };
	unsigned long long value;
	unsigned long long unit;
	bool all_id = false;
	int c, rc;
	int i;

	optind = 0;

	opt.o_archive_id_cnt = LL_HSM_ORIGIN_MAX_ARCHIVE;
	opt.o_archive_id = malloc(opt.o_archive_id_cnt *
				  sizeof(*opt.o_archive_id));
	if (opt.o_archive_id == NULL)
		return -ENOMEM;
repeat:
	while ((c = getopt_long(argc, argv, "A:b:c:f:hqt:u:v",
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
				CT_WARN("archive-id = 0 is found, any backend will be served\n");
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
		case 'b': /* -b and -c have both a number with unit as arg */
		case 'c':
			unit = ONE_MB;
			if (llapi_parse_size(optarg, &value, &unit, 0) < 0) {
				rc = -EINVAL;
				CT_ERROR(rc, "bad value for -%c '%s'", c,
					 optarg);
				return rc;
			}
			if (c == 'c')
				opt.o_chunk_size = value;
			else
				opt.o_bandwidth = value;
			break;
		case 'f':
			opt.o_event_fifo = optarg;
			break;
		case 't':
			strncpy(trusted_hsm_fsuid, optarg, MAXNAMLEN);
			break;
		case 'h':
			usage(argv[0], 0);
		case 'q':
			opt.o_verbose--;
			break;
		case 'u':
			opt.o_report_int = atoi(optarg);
			if (opt.o_report_int < 0) {
				rc = -EINVAL;
				CT_ERROR(rc, "bad value for -%c '%s'", c,
					 optarg);
				return rc;
			}
			break;
		case 'v':
			opt.o_verbose++;
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

	CT_TRACE("mount_point=%s",
		 opt.o_mnt);

	return 0;
}

enum fid_seq {
        FID_SEQ_OST_MDT0        = 0,
        FID_SEQ_LLOG            = 1, /* unnamed llogs */
        FID_SEQ_ECHO            = 2,
        FID_SEQ_UNUSED_START    = 3,
        FID_SEQ_UNUSED_END      = 9,
        FID_SEQ_LLOG_NAME       = 10, /* named llogs */
        FID_SEQ_RSVD            = 11,
        FID_SEQ_IGIF            = 12,
        FID_SEQ_IGIF_MAX        = 0x0ffffffffULL,
        FID_SEQ_IDIF            = 0x100000000ULL,
        FID_SEQ_IDIF_MAX        = 0x1ffffffffULL,
        /* Normal FID sequence starts from this value, i.e. 1<<33 */
        FID_SEQ_START           = 0x200000000ULL,
        /* sequence for local pre-defined FIDs listed in local_oid */
        FID_SEQ_LOCAL_FILE      = 0x200000001ULL,
        FID_SEQ_DOT_LUSTRE      = 0x200000002ULL,
        /* sequence is used for local named objects FIDs generated
         * by local_object_storage library */
        FID_SEQ_LOCAL_NAME      = 0x200000003ULL,
        /* Because current FLD will only cache the fid sequence, instead
	 * of oid on the client side, if the FID needs to be exposed to
         * clients sides, it needs to make sure all of fids under one
         * sequence will be located in one MDT. */
        FID_SEQ_SPECIAL         = 0x200000004ULL,
        FID_SEQ_QUOTA           = 0x200000005ULL,
        FID_SEQ_QUOTA_GLB       = 0x200000006ULL,
        FID_SEQ_ROOT            = 0x200000007ULL,  /* Located on MDT0 */
        FID_SEQ_LAYOUT_RBTREE   = 0x200000008ULL,
        /* sequence is used for update logs of cross-MDT operation */
        FID_SEQ_UPDATE_LOG      = 0x200000009ULL,
        /* Sequence is used for the directory under which update logs
         * are created. */
        FID_SEQ_UPDATE_LOG_DIR  = 0x20000000aULL,
        FID_SEQ_NORMAL          = 0x200000400ULL,
        FID_SEQ_LOV_DEFAULT     = 0xffffffffffffffffULL
};

enum root_oid {
        FID_OID_ROOT            = 1UL,
        FID_OID_ECHO_ROOT       = 2UL,
};

static inline __u64 fid_seq(const struct lu_fid *fid)
{
        return fid->f_seq;
}

static inline __u32 fid_oid(const struct lu_fid *fid)
{
        return fid->f_oid;
}

static inline bool fid_seq_is_norm(__u64 seq)
{
        return (seq >= FID_SEQ_NORMAL);
}

static inline bool fid_seq_is_mdt0(__u64 seq)
{
        return seq == FID_SEQ_OST_MDT0;
}

static inline bool fid_is_mdt0(const struct lu_fid *fid)
{
        return fid_seq_is_mdt0(fid_seq(fid));
}

static inline bool fid_seq_is_igif(__u64 seq)
{
        return seq >= FID_SEQ_IGIF && seq <= FID_SEQ_IGIF_MAX;
}

static inline bool fid_is_igif(const struct lu_fid *fid)
{
        return fid_seq_is_igif(fid_seq(fid));
}

static inline bool fid_is_norm(const struct lu_fid *fid)
{
        return fid_seq_is_norm(fid_seq(fid));
}

static inline int fid_is_root(const struct lu_fid *fid)
{
        return (fid_seq(fid) == FID_SEQ_ROOT &&
                fid_oid(fid) == FID_OID_ROOT);
}

static inline bool fid_is_md_operative(const struct lu_fid *fid)
{
        return fid_is_mdt0(fid) || fid_is_igif(fid) ||
               fid_is_norm(fid) || fid_is_root(fid);
}

int llapi_fid_parse(const char *fidstr, struct lu_fid *fid, char **endptr)
{
        unsigned long long val;
        bool bracket = false;
        char *end = (char *)fidstr;
        int rc = 0;

        if (!fidstr || !fid) {
                rc = -EINVAL;
                goto out;
        }

        while (isspace(*fidstr))
                fidstr++;
        while (*fidstr == '[') {
                bracket = true;
                fidstr++;
        }
        errno = 0;
        val = strtoull(fidstr, &end, 0);
        if ((val == 0 && errno == EINVAL) || *end != ':') {
                rc = -EINVAL;
                goto out;
        }
        if (val >= UINT64_MAX)
                rc = -ERANGE;
        else
                fid->f_seq = val;

        fidstr = end + 1; /* skip first ':', checked above */
        errno = 0;
        val = strtoull(fidstr, &end, 0);
        if ((val == 0 && errno == EINVAL) || *end != ':') {
                rc = -EINVAL;
                goto out;
        }
        if (val > UINT32_MAX)
                rc = -ERANGE;
        else
                fid->f_oid = val;

        fidstr = end + 1; /* skip second ':', checked above */
        errno = 0;
        val = strtoull(fidstr, &end, 0);
        if (val == 0 && errno == EINVAL) {
                rc = -EINVAL;
                goto out;
        }
        if (val > UINT32_MAX)
                rc = -ERANGE;
        else
                fid->f_ver = val;

        if (bracket && *end == ']')
                end++;
out:
        if (endptr)
                *endptr = end;

        errno = -rc;
        return rc;
}

/*
 * Phobos functions
 */
static int  fid2objid(const struct lu_fid *fid, char *objid)
{
	if (!objid || !fid)
		return -EINVAL;

	/* object id is "fsname:fid" */	
	/* /!\ additionnal letter only because of pcocc side effect */
	return sprintf(objid, "L%s:"DFID, fs_name, PFID(fid));
}

static int phobos_op_put(const struct lu_fid *fid, char *altobjid, 
			 const int fd, char *hexstripe)
{
        struct pho_xfer_desc    xfer = {0};
        struct pho_attrs        attrs = {0};
        int rc;
        struct stat st;
	char objid[MAXNAMLEN];
	char *obj = NULL; 

	/* If provided altobjid as objectid */
	if (altobjid)
		obj = altobjid; 
	else {
		rc = fid2objid(fid, objid);
		if (rc < 0)
			return rc;
		obj = objid;
	}	

	/*
 	 * @todo: 
 	 *    - indentation style
 	 *    - should we use Phobos errors or Lustre ones ?
 	 *    - convert error Phobos/lustre
 	 *    - remove that stupid exists that are here for debug purpose
 	 *    - management of the size of string objid
 	 */

        rc = pho_attr_set(&attrs, "program", "copytool");
        if (rc)
		return rc; 

        rc = pho_attr_set(&attrs, "hexstripe", hexstripe);
        if (rc)
		return rc; 

        memset(&xfer, 0, sizeof(xfer));
        xfer.xd_op = PHO_XFER_OP_PUT;
        xfer.xd_fd = fd;
        xfer.xd_flags = 0;

        /* fstat on lustre fd seems to fail */
        fstat(xfer.xd_fd, &st);
        xfer.xd_params.put.size = st.st_size;

        xfer.xd_params.put.family = PHO_RSC_DIR;
        xfer.xd_objid = obj;
        xfer.xd_attrs = attrs;

        rc = phobos_put(&xfer, 1, NULL, NULL);
	
	pho_xfer_desc_destroy(&xfer);
	
        if (rc)
                CT_ERROR(rc, "PUT failed");

        return rc;
}

static int phobos_op_get(const struct lu_fid *fid, char *altobjid, int fd)
{
        struct pho_xfer_desc    xfer = {0};
        int rc;
	char objid[MAXNAMLEN];
	char *obj = NULL;

	/* If provided altobjid as objectid */
	if (altobjid)
		obj = altobjid; 
	else {
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

	pho_xfer_desc_destroy(&xfer);

        if (rc)
                CT_ERROR(rc, "PUT failed");

        return rc;
}

static int phobos_op_getstripe(const struct lu_fid *fid, char *altobjid,
			       char *hexstripe)
{
        struct pho_xfer_desc    xfer = {0};
        int rc;
	char objid[MAXNAMLEN];
        const char *val = NULL;
	char *obj = NULL;

	/* If provided altobjid as objectid */
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

        rc = phobos_get(&xfer, 1, NULL, NULL);
        if (rc) {
                CT_ERROR(rc, "PUT failed");
		return rc;
	}
	
        if (pho_attrs_is_empty(&xfer.xd_attrs))
		return -ENOENT;
	else {
                val = pho_attr_get(&xfer.xd_attrs, "hexstripe");
		strcpy(hexstripe, val);
        }

	pho_xfer_desc_destroy(&xfer);

	return rc; 
}
/*
 * A set of function to encode buffer into strings
 */
void bin2hexstr(const char *bin, size_t len, char *out)
{
        size_t  i;
        char tmp[10];

        if (bin == NULL || len == 0)
                return;

        /* Size is encoded at the beginning */
        out[0] = 0;
        sprintf(out, "%02x|", (unsigned int)len);

        /* Next each char, one by one */
        for (i=0; i<len; i++) {
                sprintf(tmp, "%08x:", bin[i]);
                strcat(out, tmp);
        }
}

unsigned int hexstr2bin(const char *hex, char *out)
{
        unsigned int len ;
        char tmp[10]; /* too big */
        size_t  i;

        if (hex == NULL || out == 0)
                return 0;

        /* get size first */
        tmp[0] = hex[0];
        tmp[1] = hex[1];
        tmp[2] = hex[2];
        tmp[3] = 0;
        sscanf(tmp, "%02x|", &len);

        /* Remind that 3 first characters encodes the size */
        out[0] = 0;
        for (i=0; i < len; i++) {
                tmp[0] = hex[9*i+3];
                tmp[1] = hex[9*i+4];
                tmp[2] = hex[9*i+5];
                tmp[3] = hex[9*i+6];
                tmp[4] = hex[9*i+7];
                tmp[5] = hex[9*i+8];
                tmp[6] = hex[9*i+9];
                tmp[7] = hex[9*i+10];
                tmp[8] = hex[9*i+11];
                tmp[9] = 0;
                sscanf(tmp, "%08x:", (unsigned int *)&(out[i]));
        }

        return len;
}

/*
 * Copytool functions (with ct_ prefix)
 */

static int ct_get_altobjid(const struct hsm_action_item *hai, 
			   char *altobjid)
{
	char			 xattr_buf[XATTR_SIZE_MAX];
	ssize_t			 xattr_size;
	int 			 fd;

	fd = llapi_open_by_fid(opt.o_mnt, &hai->hai_fid, O_RDONLY);
	if (fd < 0)
		return -errno;

	xattr_size = fgetxattr(fd, trusted_hsm_fsuid, xattr_buf,
			       XATTR_SIZE_MAX);
	if (xattr_size < 0) {
		close(fd);
		return -errno;
	}
	
	memcpy(altobjid, xattr_buf, xattr_size);
	altobjid[xattr_size] = 0; /* String trailing zero */
	close(fd);

	return 0;
}

static int ct_save_stripe(int src_fd, const char *src, char *hexstripe)
{
	char			 lov_buf[XATTR_SIZE_MAX];
	struct lov_user_md	*lum;
	int			 rc;
	ssize_t			 xattr_size;

	xattr_size = fgetxattr(src_fd, XATTR_LUSTRE_LOV, lov_buf,
			       sizeof(lov_buf));
	if (xattr_size < 0) {
		rc = -errno;
		CT_ERROR(rc, "cannot get stripe info on '%s'", src);
		return rc;
	}

	/* read content of read xattr */
	lum = (struct lov_user_md *)lov_buf;

	if (lum->lmm_magic == LOV_USER_MAGIC_V1 ||
	    lum->lmm_magic == LOV_USER_MAGIC_V3) {
		/* Set stripe_offset to -1 so that it is not interpreted as a
		 * hint on restore. */
		lum->lmm_stripe_offset = -1;
	}

	/* Save the stripe as an hexa string to save it as 
 	 * an attr in Phobos object's metadata */ 
	bin2hexstr((const char *)lov_buf, xattr_size, hexstripe);

	return 0;
}

static int ct_restore_stripe(const char *dst, int dst_fd,
			     const void *lovea, size_t lovea_size)
{
	int	rc;

	rc = fsetxattr(dst_fd, XATTR_LUSTRE_LOV, lovea, lovea_size,
		       XATTR_CREATE);
	if (rc < 0) {
		rc = -errno;
		CT_ERROR(rc, "cannot set lov EA on '%s'", dst);
	}

	return rc;
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
	char	 src[PATH_MAX];
	int	 rc;

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
	/* Restore takes specific parameters. Call the same function w/ default
	 * values for all other operations. */
	return ct_begin_restore(phcp, hai, -1, 0);
}

static int ct_fini(struct hsm_copyaction_private **phcp,
		   const struct hsm_action_item *hai, int hp_flags, int ct_rc)
{
	struct hsm_copyaction_private	*hcp;
	char				 lstr[PATH_MAX];
	int				 rc;

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
		CT_TRACE("llapi_hsm_action_end() on '%s' ok (rc=%d)",
			 lstr, rc);

	return rc;
}

static int ct_archive(const struct hsm_action_item *hai, const long hal_flags)
{
	struct hsm_copyaction_private	*hcp = NULL;
	char				 src[PATH_MAX];
	char				 hexstripe[PATH_MAX] = "";
	int				 rc = 0;
	int				 rcf = 0;
	int				 hp_flags = 0;
	int				 open_flags;
	int				 src_fd = -1;

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
	open_flags |= ((hai->hai_extent.length == -1) ? O_TRUNC : 0) | O_CREAT;

	/* saving stripe is not critical */
	rc = ct_save_stripe(src_fd, src, hexstripe);
	if (rc < 0) {
		CT_ERROR(rc, "cannot save file striping info of '%s'", src);
		goto fini_major;
	}

	/* Do phobos xfer */
	rc = phobos_op_put(&hai->hai_fid, NULL, src_fd, hexstripe);
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

	return rc;
}

static int ct_restore(const struct hsm_action_item *hai, const long hal_flags)
{
	struct hsm_copyaction_private	*hcp = NULL;
	char				 dst[PATH_MAX];
	char				 hexstripe[PATH_MAX];
	char				 altobjid[PATH_MAX];
	char				*altobj = NULL;
	char				 lov_buf[XATTR_SIZE_MAX];
	size_t				 lov_size = sizeof(lov_buf);
	int				 rc;
	int				 hp_flags = 0;
	int				 dst_fd = -1;
	int				 mdt_index = -1;
	int				 open_flags = 0;
	bool				 set_lovea;
	struct lu_fid			 dfid;
	/* we fill lustre so:
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
	/* restore loads and sets the LOVEA w/o interpreting it to avoid
	 * dependency on the structure format. */
	rc = phobos_op_getstripe(&hai->hai_fid, NULL, hexstripe);
	if (rc) {
		CT_WARN("cannot get stripe rules for "DFID"  (%s), use default",
			PFID(&hai->hai_fid), strerror(-rc));
		set_lovea = false;
	} else {
		open_flags |= O_LOV_DELAY_CREATE;
		set_lovea = true;
	}
	lov_size = hexstr2bin(hexstripe, lov_buf);

	/* start the restore operation */
	rc = ct_begin_restore(&hcp, hai, mdt_index, open_flags);
	if (rc < 0)
		goto fini;

	/* get the FID of the volatile file */
	rc = llapi_hsm_action_get_dfid(hcp, &dfid);
	if (rc < 0) {
		CT_ERROR(rc, "restoring "DFID
			 ", cannot get FID of created volatile file",
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
	} else
		altobj = NULL; 

	if (set_lovea) {
		/* the layout cannot be allocated through .fid so we have to
		 * restore a layout */
		rc = ct_restore_stripe(dst, dst_fd, lov_buf, lov_size);
		if (rc < 0) {
			CT_ERROR(rc, "cannot restore file striping info"
				 " for '%s'", dst);
			err_major++;
			goto fini;
		}
	}

	/* Do phobos xfer */
	rc = phobos_op_get(&hai->hai_fid, altobj, dst_fd);
	CT_TRACE("phobos_get (restore): rc=%d", rc);
	/** @todo make clear rc management */

	CT_TRACE("data restore to '%s' done", dst);

fini:
	rc = ct_fini(&hcp, hai, hp_flags, rc);

	if (!(dst_fd < 0))
		close(dst_fd);

	return rc;
}

static int ct_remove(const struct hsm_action_item *hai, const long hal_flags)
{
	struct hsm_copyaction_private	*hcp = NULL;
	int				 rc;

	rc = ct_begin(&hcp, hai);
	if (rc < 0)
		goto fini;

	// CT_TRACE("removing file '%s'", dst);

	/** @todo : add remove as it will be available in Phobos */
	if (opt.o_dry_run) {
		rc = 0;
		goto fini;
	}

fini:
	rc = ct_fini(&hcp, hai, 0, rc);

	return rc;
}

static int ct_process_item(struct hsm_action_item *hai, const long hal_flags)
{
	int	rc = 0;

	if (opt.o_verbose >= LLAPI_MSG_INFO || opt.o_dry_run) {
		/* Print the original path */
		char		fid[128];
		char		path[PATH_MAX];
		long long	recno = -1;
		int		linkno = 0;

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
		rc = ct_restore(hai, hal_flags);
		break;
	case HSMA_REMOVE:
		rc = ct_remove(hai, hal_flags);
		break;
	case HSMA_CANCEL:
		CT_TRACE("cancel not implemented for file system '%s'",
			 opt.o_mnt);
		/* Don't report progress to coordinator for this cookie:
		 * the copy function will get ECANCELED when reporting
		 * progress. */
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
	long			 hal_flags;
	struct hsm_action_item	*hai;
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
	pthread_attr_t		 attr;
	pthread_t		 thread;
	struct ct_th_data	*data;
	int			 rc;

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
	/* If we don't clean up upon interrupt, umount thinks there's a ref
	 * and doesn't remove us from mtab (EINPROGRESS). The lustre client
	 * does successfully unmount and the mount is actually gone, but the
	 * mtab entry remains. So this just makes mtab happier. */
	llapi_hsm_copytool_unregister(&ctdata);

	/* Also remove fifo upon signal as during normal/error exit */
	if (opt.o_event_fifo != NULL)
		llapi_hsm_unregister_event_fifo(opt.o_event_fifo);
	_exit(1);
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
		int msgsize;
		int i = 0;

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
	int	rc;

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
	int	rc;

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
	int	rc;

	strncpy(trusted_hsm_fsuid, XATTR_TRUSTED_HSM_FSUID_DEFAULT, MAXNAMLEN);

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
	CT_TRACE("trusted_hsm_fsuid=%s", trusted_hsm_fsuid);

	rc = ct_run();

	CT_TRACE("process finished, errs: %d major, %d minor,"
		 " rc=%d (%s)", err_major, err_minor, rc,
		 strerror(-rc));

error_cleanup:
	ct_cleanup();

	return -rc;
}

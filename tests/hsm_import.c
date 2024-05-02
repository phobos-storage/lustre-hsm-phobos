/*
 * Copyright (C) 2022 Commissariat a l'energie atomique et aux energies
 *                    alternatives
 *
 * SPDX-License-Identifer: GPL-2.0-only
 */
#include <lustre/lustreapi.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#define error(rc, fmt, ...)                         \
    do {                                            \
        fprintf(stderr, "[error] %s:%d " fmt,       \
                __func__, __LINE__, __VA_ARGS__);   \
        fprintf(stderr, ": %s\n", strerror(-(rc))); \
    } while (0)

struct conf {
    const char *path;
    const char *data_file;
    int archive_id;
    bool do_undelete;
    bool do_stat;
};

static struct option cliopts[] = {
    {
        .name = "path",
        .has_arg = required_argument,
        .flag = NULL,
        .val = 'p',
    },
    {
        .name = "stat-file",
        .has_arg = required_argument,
        .flag = NULL,
        .val = 's',
    },
    {
        .name = "archive-id",
        .has_arg = required_argument,
        .flag = NULL,
        .val = 'A',
    },
    {
        .name = "undelete",
        .has_arg = no_argument,
        .flag = NULL,
        .val = 'u',
    },
    {
        .name = "stat",
        .has_arg = no_argument,
        .flag = NULL,
        .val = 'S',
    },
    {
        .name = "help",
        .has_arg = no_argument,
        .flag = NULL,
        .val = 'h',
    },
    { 0 }
};

int parse_int(const char *archive_id, int *res)
{
    unsigned long value;
    char *end;

    errno = 0;
    value = strtoul(archive_id, &end, 10);
    if (errno)
        return -errno;
    else if (value == 0 && end == archive_id)
        return -EINVAL;

    *res = value;

    return 0;
}

void usage(const char *progname)
{
    printf(
        "Usage: %s [--archive-id|-A <id>] [--path|-p <path>] [--undelete|-u] "
        "[--stat|-S] [--stat-file|-s] [--help|-h]\n",
        progname);
}

/* Returns the file descriptor on success, -1 on error and sets error
 * appropriately.
 */
int open_with_extension(const char *base, const char *extension, int flags)
{
    size_t extension_len = strlen(extension);
    size_t base_len = strlen(base);
    char *buf;
    int fd;
    int rc;

    buf = malloc(base_len + extension_len + 1);
    if (!buf) {
        errno = -ENOMEM;
        return -1;
    }

    memcpy(buf, base, base_len);
    memcpy(buf + base_len, extension, extension_len);
    buf[base_len + extension_len] = '\0';

    fd = open(buf, flags);
    if (fd == -1) {
        rc = -errno;
        error(rc, "Could not open '%s'", buf);
    } else {
        rc = fd;
    }

    free(buf);

    if (rc < 0) {
        errno = -rc;
        rc = -1;
    }

    return rc;
}

/* Return the length of the data read from the file, negative error code on
 * error
 */
int load_buffer(const char *base, const char *extension,
                char *buf, size_t len)
{
    int fd;
    int rc;

    fd = open_with_extension(base, extension, O_RDONLY);
    if (fd == -1)
        return -errno;

    rc = read(fd, buf, len);
    /* assume read succeed in one call */
    if (rc == -1) {
        rc = -errno;
        error(rc, "Failed to read %lu bytes from '%s%s'", len, base, extension);
    }
    close(fd);

    return rc;
}

/* return 0 on success, -errno on error */
int dump_buffer(const char *base, const char *extension,
                char *buf, size_t len)
{
    int fd;
    int rc;

    fd = open_with_extension(base, extension, O_WRONLY | O_CREAT);
    if (fd == -1)
        return -errno;

    rc = write(fd, buf, len);
    if (rc == -1) {
        rc = -errno;
        error(rc, "Failed to write %lu bytes to '%s%s'", len, base, extension);
    } else {
        /* assume write succeed in one call */
        assert((size_t)rc == len);
        rc = 0;
    }
    close(fd);

    return rc;
}

int dump_file_stat(struct conf *conf)
{
    struct stat st;
    int rc = 0;

    if (!conf->data_file) {
        fprintf(stderr, "No output file provided for stat\n");
        return -EINVAL;
    }

    rc = stat(conf->path, &st);
    if (rc == -1) {
        rc = -errno;
        error(rc, "Cannot stat '%s'", conf->path);
        return rc;
    }

    return dump_buffer(conf->data_file, ".stat", (char *)&st, sizeof(st));
}

int dump_fuid(struct conf *conf)
{
    char hsm_fuid[XATTR_SIZE_MAX + 1];
    ssize_t hsm_fuid_len;
    int rc;

    if (!conf->data_file) {
        fprintf(stderr, "No output file provided for fuid\n");
        return -EINVAL;
    }

    hsm_fuid_len = lgetxattr(conf->path, "trusted.hsm_fuid",
                             hsm_fuid, XATTR_SIZE_MAX);
    if (hsm_fuid_len < 0) {
        rc = -errno;
        error(rc, "Failed to read xattr 'trusted.hsm_fuid' for '%s'",
              conf->path);
        return rc;
    }

    return dump_buffer(conf->data_file, ".fuid", hsm_fuid, hsm_fuid_len);
}

int dump_file_info(struct conf *conf)
{
    int rc;

    rc = dump_file_stat(conf);
    if (rc)
        return rc;

    return dump_fuid(conf);
}

int set_fuid(const struct conf *conf)
{
    char buf[sizeof("lustre:") + FID_LEN];
    int rc;

    rc = load_buffer(conf->data_file, ".fuid", buf, sizeof(buf));
    if (rc == -1)
        return -errno;

    rc = lsetxattr(conf->path, "trusted.hsm_fuid", buf, rc, 0);
    if (rc == -1) {
        rc = -errno;
        error(rc, "Failed to set hsm_fuid xattr for '%s'", conf->path);
    }

    return rc;
}

int do_hsm_import(const struct conf *conf)
{
    struct lu_fid newfid;
    struct stat st;
    int rc;

    rc = load_buffer(conf->data_file, ".stat", (char *)&st, sizeof(st));
    if (rc == -1)
        return -errno;

    rc = llapi_hsm_import(conf->path, conf->archive_id, &st,
                          0, -1, 0, 0, NULL,
                          &newfid);
    if (rc) {
        error(rc, "Failed to create released file '%s'", conf->path);
        return -rc;
    }

    printf(DFID "\n", PFID(&newfid));

    rc = set_fuid(conf);
    if (rc)
        return rc;

    return 0;
}

int main(int argc, char **argv)
{
    struct conf opts = {0};
    int rc;
    char c;

    while ((c = getopt_long(argc, argv, "p:A:s:uSh", cliopts, NULL)) != -1) {
        rc = 0;

        switch (c) {
        case 'p':
            opts.path = optarg;
            break;
        case 'A':
            rc = parse_int(optarg, &opts.archive_id);
            break;
        case 'u':
            if (opts.do_stat) {
                fprintf(stderr, "Cannot perform stat and undelete\n");
                return EXIT_FAILURE;
            }
            opts.do_undelete = true;
            break;
        case 'S':
            if (opts.do_undelete) {
                fprintf(stderr, "Cannot perform stat and undelete\n");
                return EXIT_FAILURE;
            }
            opts.do_stat = true;
            break;
        case 's':
            opts.data_file = optarg;
            break;
        case 'h':
            usage(argv[0]);
            exit(EXIT_SUCCESS);
        case '?':
        default:
            return EXIT_FAILURE;
        }

        if (rc)
            break;
    }

    if (!opts.do_stat && !opts.do_undelete)
        return EXIT_SUCCESS;

    if (!opts.path) {
        fprintf(stderr, "No file name provided\n");
        return EXIT_FAILURE;
    }

    if (opts.do_stat)
        rc = dump_file_info(&opts);
    else
        rc = do_hsm_import(&opts);

    return -rc;
}

/*
 * vim:expandtab:shiftwidth=4:tabstop=4:
 */

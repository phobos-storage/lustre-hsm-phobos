#define _GNU_SOURCE

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <dirent.h>
#include <error.h>
#include <getopt.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/types.h>
#include <unistd.h>

#include <linux/lustre/lustre_fid.h>
#include <lustre/lustreapi.h>
#include <phobos_store.h>

#define VERSION "0.0.1"

#define USAGE                                                        \
    "Usage: %s [option] <src>\n"                                     \
    "Arguments:\n"                                                   \
    "    <src>          Object ID to recover.\n"                     \
    "Options:\n"                                                     \
    "    --dry-run      Simulate recovery without making changes.\n" \
    "    -p <new_path>  Specify a new path for the object.\n"        \
    "    -g <group>     Specify a group for the object owner.\n"     \
    "Help:\n"                                                        \
    "    -h, --help     Print this message.\n"                       \
    "    --version      Print version information.\n"

static char fs_name[MAX_OBD_NAME + 1];

static int fid2objid(const struct lu_fid *fid, char *objid)
{
    int rc;

    if (!objid || !fid)
        return -EINVAL;

    rc = sprintf(objid, "%s:"DFID_NOBRACE, fs_name, PFID(fid));

    return rc < 0 ? rc : 0;
}

static int ct_recover(const char *fid, const char *newpath,
                      const char *groupname)
{
    struct pho_xfer_target xtgt = {};
    struct pho_xfer_desc xfer = {};
    char objid[MAXNAMLEN];
    struct lu_fid newfid;
    struct lu_fid lu_fid;
    struct stat inode = {
        .st_mode = 00640,
    };
    const char *username;
    const char *path;
    char *endptr;
    int rc = 0;

    rc = llapi_fid_parse(fid, &lu_fid, &endptr);
    if (rc || *endptr != '\0') {
        pho_error(-EINVAL, "invalid fid parsing '%s'", fid);
        return -EINVAL;
    }

    rc = fid2objid(&lu_fid, objid);
    if (rc) {
        pho_error(rc, "failed to build object id for '%s'", fid);
        return rc;
    }

    if (groupname && groupname[0] != '\0') {
        struct group *grp;

        errno = 0;
        grp = getgrnam(groupname);
        if (!grp) {
            rc = -errno;
            pho_error(rc, "failed to retrieve group id '%s'", groupname);
            return rc;
        }
        inode.st_gid = grp->gr_gid;
    }

    xfer.xd_op = PHO_XFER_OP_GETMD;
    xfer.xd_params.get.scope = DSS_OBJ_ALIVE;
    xfer.xd_flags = 0;
    xfer.xd_ntargets = 1;
    xfer.xd_targets = &xtgt;
    xtgt.xt_objid = objid;

    rc = phobos_getmd(&xfer, 1, NULL, NULL);
    if (rc) {
        pho_error(rc, "user_md retrieval for fid '%s' failed\n", fid);
        return rc;
    }

    if (newpath && newpath[0] != '\0') {
        path = newpath;
    } else {
        path = pho_attr_get(&xtgt.xt_attrs, "fullpath");
        if (!*path) {
            pho_error(rc, "'fullpath' is not registered in user_md of '%s'", fid);
            return rc;
        }
    }

    username = pho_attr_get(&xtgt.xt_attrs, "username");
    if (username) {
        struct passwd *pwd;

        pwd = getpwnam(username);
        if (pwd != NULL)
            inode.st_uid = pwd->pw_uid;
    }

    rc = llapi_hsm_import(path, 0, &inode,
                          0, -1, 0, 0, NULL, &newfid);

    pho_xfer_desc_clean(&xfer);

    return rc;
}

void print_help(const char *prog_name) {
    fprintf(stderr, USAGE, prog_name);
}

void print_version(const char *prog_name) {
    fprintf(stderr, "%s v%s\n", prog_name, VERSION);
}

void recover_object(int dry_run, char *new_path, char *group, char *object_id) {
    if (dry_run) {
        printf("[DRY RUN] Would recover object %s", object_id);
        if (new_path) printf(" to path %s", new_path);
        if (group) printf(" with group %s", group);
        printf(".\n");
    } else {
        printf("Recovering object %s", object_id);
        if (new_path) printf(" to path %s", new_path);
        if (group) printf(" with group %s", group);
        printf(".\n");
        ct_recover(object_id, new_path, group);
    }
}

int main(int argc, char *argv[]) {
    char *object_id = NULL;
    char *mount_pt = NULL;
    char *new_path = NULL;
    char *group = NULL;
    int dry_run = 0;
    int opt;
    int rc;

    struct option longopts[] = {
        {
            .name = "dry-run",
            .has_arg = no_argument,
            .flag = 0,
            .val = 'd',
        },
        {
            .name = "version",
            .has_arg = no_argument,
            .flag = 0,
            .val = 'v',
        },
        {
            .name = "help",
            .has_arg = no_argument,
            .flag = 0,
            .val = 'h',
        },
        {0},
    };

#ifdef HAVE_PHOBOS_INIT
    phobos_init();
    atexit(phobos_fini);
#endif

    while ((opt = getopt_long(argc, argv, "dg:hp:v", longopts, NULL)) != -1) {
        switch (opt) {
            case 'd':
                dry_run = 1;
                break;
            case 'g':
                group = optarg;
                break;
            case 'h':
                print_help(argv[0]);
                exit(EXIT_SUCCESS);
            case 'p':
                new_path = optarg;
                break;
            case 'v':
                print_version(argv[0]);
                exit(EXIT_SUCCESS);
            default:
                fprintf(stderr, "Unknown option: %c\n", opt);
                print_help(argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "Error: Missing <mnt> <src>.\n");
        print_help(argv[0]);
        exit(EXIT_FAILURE);
    }
    mount_pt = argv[optind++];
    object_id = argv[optind];

    rc = llapi_search_fsname(mount_pt, fs_name);
    if (rc < 0) {
        pho_error(rc, "cannot find a Lustre filesystem mounted at '%s'",
                  mount_pt);
        return rc;
    }

    recover_object(dry_run, new_path, group, object_id);

    return EXIT_SUCCESS;
}

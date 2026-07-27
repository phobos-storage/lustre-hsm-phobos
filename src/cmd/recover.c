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
    "    -u <user>      Specify an owner for the object.\n"          \
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

static int retrieve_usermd(const char *fid, struct pho_xfer_desc *xfer)
{
    char objid[MAXNAMLEN];
    struct lu_fid lu_fid;
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

    xfer->xd_op = PHO_XFER_OP_GETMD;
    xfer->xd_params.get.scope = DSS_OBJ_ALIVE;
    xfer->xd_flags = 0;
    xfer->xd_ntargets = 1;
    xfer->xd_targets[0].xt_objid = objid;

    rc = phobos_getmd(xfer, 1, NULL, NULL);
    if (rc) {
        pho_error(rc, "user_md retrieval for fid '%s' failed\n", fid);
        return rc;
    }

    return 0;
}

static int retrieve_user(struct stat *inode, struct pho_xfer_desc *xfer,
                         const char *newuser)
{
    const char *username;

    if (newuser && newuser[0] != '\0')
        username = newuser;
    else
        username = pho_attr_get(&xfer->xd_targets[0].xt_attrs, "username");

    if (username) {
        struct passwd *pwd;

        pwd = getpwnam(username);
        if (pwd)
            inode->st_uid = pwd->pw_uid;
        else
            pho_info("user '%s' was not found, will use 'root' instead",
                     username);
    }

    return 0;
}

static int retrieve_group(struct stat *inode, const char *newgroup)
{
    int rc;

    if (newgroup && newgroup[0] != '\0') {
        struct group *grp;

        errno = 0;
        grp = getgrnam(newgroup);
        if (!grp) {
            rc = -errno;
            pho_error(rc, "failed to retrieve group id '%s'", newgroup);
            return rc ? rc : -EINVAL;
        }
        inode->st_gid = grp->gr_gid;
    }

    return 0;
}

static int retrieve_path(const char **path, struct pho_xfer_desc *xfer,
                         const char *newpath)
{
    if (newpath && newpath[0] != '\0') {
        *path = newpath;
    } else {
        *path = pho_attr_get(&xfer->xd_targets[0].xt_attrs, "fullpath");
        if (!*path) {
            pho_error(-EINVAL, "'fullpath' is not registered in user_md");
            return -EINVAL;
        }
    }

    return 0;
}

static int ct_recover(const char *path,
                      __attribute__((unused))struct pho_xfer_desc *xfer,
                      struct stat *inode)
{
    struct lu_fid newfid;
    int rc;

    rc = llapi_hsm_import(path, 0, inode,
                          0, -1, 0, 0, NULL, &newfid);

    return rc;
}

void print_help(const char *prog_name) {
    fprintf(stderr, USAGE, prog_name);
}

void print_version(const char *prog_name) {
    fprintf(stderr, "%s v%s\n", prog_name, VERSION);
}

int recover_object(int dry_run, char *new_path, char *user, char *group,
                   char *object_id) {
    struct pho_xfer_target xtgt = {};
    struct pho_xfer_desc xfer = {};
    struct stat inode = {
        .st_mode = 00640,
    };
    const char *path;
    int rc;

    xfer.xd_targets = &xtgt;
    rc = retrieve_usermd(object_id, &xfer);
    if (rc)
        return rc;

    rc = retrieve_user(&inode, &xfer, user);
    if (rc)
        return rc;

    rc = retrieve_group(&inode, group);
    if (rc)
        return rc;

    rc = retrieve_path(&path, &xfer, new_path);
    if (rc)
        return rc;

    if (dry_run)
        printf("[DRY RUN] would recover ");
    else
        printf("recovering ");

    printf("object '%s' to path '%s' ", object_id, path);
    printf("(user: %u, group: %u)\n", inode.st_uid, inode.st_gid);

    if (!dry_run)
        rc = ct_recover(path, &xfer, &inode);

    pho_xfer_desc_clean(&xfer);

    return rc;
}

int main(int argc, char *argv[]) {
    char *object_id = NULL;
    char *mount_pt = NULL;
    char *new_path = NULL;
    char *group = NULL;
    char *user = NULL;
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

    while ((opt = getopt_long(argc, argv, "dg:hp:u:v", longopts, NULL)) != -1) {
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
            case 'u':
                user = optarg;
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

    rc = recover_object(dry_run, new_path, user, group, object_id);

    return rc ? rc : EXIT_SUCCESS;
}

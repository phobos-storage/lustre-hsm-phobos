#ifndef _COMMON_H
#define _COMMON_H

#include <phobos_store.h>

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

/**
 * Set log callback
 */
void ct_log_configure(struct options *opts);

#endif

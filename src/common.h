/*
 * Copyright (C) 2023 Commissariat a l'energie atomique et aux energies
 *                    alternatives
 *
 * SPDX-License-Identifer: GPL-2.0-only
 */
#ifndef _COMMON_H
#define _COMMON_H

#include <phobos_store.h>
#include <glib.h>

struct options {
    int              o_daemonize;
    int              o_dry_run;
    int              o_abort_on_error;
    int              o_verbose;
    GArray          *o_archive_ids;
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

struct buf {
    char *data;
    size_t len;
};

struct hinttab {
    size_t count;
    struct {
        const char *key;
        const char *value;
    } *hints;
    char *context;
};

char *get_key_value(char *input, char **key, char **value);

int process_hints(const struct buf *hints, struct hinttab *hinttab);

void hinttab_free(struct hinttab *hinttab);

int pho_xfer_add_tag(struct pho_xfer_desc *xfer, const char *new_tag);

#endif

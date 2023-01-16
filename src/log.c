#include "common.h"
#include "pho_common.h"
#include <lustre/lustreapi.h>

static void ct_log_callback_fifo_wrapper(const struct pho_logrec *rec, ...)
{
    enum llapi_message_level level;
    va_list list;

    switch (rec->plr_level) {
    case PHO_LOG_DISABLED:
        return;
    case PHO_LOG_ERROR:
        level = LLAPI_MSG_ERROR;
        break;
    case PHO_LOG_WARN:
        level = LLAPI_MSG_WARN;
        break;
    case PHO_LOG_INFO:
        level = LLAPI_MSG_NORMAL;
        break;
    case PHO_LOG_VERB:
        level = LLAPI_MSG_INFO;
        break;
    case PHO_LOG_DEBUG:
        level = LLAPI_MSG_DEBUG;
        break;
    }

    va_start(list, rec);
    llapi_hsm_log_error(level, rec->plr_err, rec->plr_msg, list);
    va_end(list);
}

static void ct_log_callback_fifo(const struct pho_logrec *rec)
{
    ct_log_callback_fifo_wrapper(rec);
}


void ct_log_configure(struct options *opts)
{
    if (opts->o_event_fifo)
        pho_log_callback_set(ct_log_callback_fifo);

    if (opts->o_verbose < PHO_LOG_DISABLED)
        opts->o_verbose = PHO_LOG_DISABLED;
    else if (opts->o_verbose > PHO_LOG_DEBUG)
        opts->o_verbose = PHO_LOG_DEBUG;

    pho_log_level_set(opts->o_verbose);
}

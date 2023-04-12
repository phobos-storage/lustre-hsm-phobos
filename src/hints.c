#define _GNU_SOURCE

#include "common.h"
#include "phobos_admin.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

char *get_key_value(char *input, char **key, char **value)
{
    char *last = input + strlen(input);
    char *end;

    *key = NULL;
    *value = NULL;

    end = strchr(input, '=');
    if (!end && *input == '\0')
        /* empty string, end of parsing */
        return NULL;

    if (!end || (end == input)) {
        pho_error(EINVAL, "failed to parse hint '%s', missing key",
                  input);
        errno = EINVAL;
        return NULL;
    }

    if (end + 1 == last) {
        pho_error(EINVAL, "failed to parse hint '%s', missing value",
                  input);
        errno = EINVAL;
        return NULL;
    }

    *key = input;
    *end++ = '\0';
    input = end;
    end = strchr(input, ',');

    if (input != end)
        *value = input;

    if (end) {
        *end = '\0';
        return end + 1;
    }

    return NULL;
}

static size_t count_char(const char *input, char c)
{
    size_t count = 0;

    while (*input)
        if (*input++ == c)
            count++;

    return count;
}

int process_hints(const struct buf *hints, struct hinttab *hinttab)
{
    size_t i = 0;
    char *hint;

    hinttab->context = strndup(hints->data, hints->len);
    if (!hinttab->context)
        return -errno;

    hinttab->count = count_char(hinttab->context, ',') + 1;
    hinttab->hints = malloc(sizeof(*hinttab->hints) * hinttab->count);
    if (!hinttab->hints) {
        int rc = -errno;

        free(hinttab->context);

        return rc;
    }

    hint = hinttab->context;
    do {
        char *value;
        char *key;

        errno = 0;
        hint = get_key_value(hint, &key, &value);
        if (!hint && errno)
            break;

        if (!errno && key && value) {
            assert(i < hinttab->count);

            hinttab->hints[i].key = key;
            hinttab->hints[i].value = value;
            i++;

        }
        if (!hint) {
            /* a trailing ',' will add one more element to count, i is the exact
             * number of hints found.
             */
            hinttab->count = i;
            /* nothing more to parse */
            break;
        }

    } while (hint);

    return -errno;
}

void hinttab_free(struct hinttab *hinttab)
{
    free(hinttab->hints);
    free(hinttab->context);
}

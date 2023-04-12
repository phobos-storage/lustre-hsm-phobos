#define _GNU_SOURCE

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <cmocka.h>

#include <string.h>

#include "common.h"

struct test_input {
    const char *input;
    const char *expected_key;
    const char *expected_value;
    const char *expected_end;
    int expected_errno;
};

static void assert_string_value(const char *expected, const char *actual)
{
    if (expected)
        assert_string_equal(expected, actual);
    else
        assert_null(actual);
}

void parse_hint_key_value(void **data)
{
    struct test_input values[] = {
        {
            .input = "",
            .expected_key = NULL,
            .expected_value = NULL,
            .expected_end = NULL,
        },
        {
            .input = "key=value",
            .expected_key = "key",
            .expected_value = "value",
            .expected_end = NULL,
        },
        {
            .input = "key=value,end",
            .expected_key = "key",
            .expected_value = "value",
            .expected_end = "end",
        },
        {
            .input = "key=,end",
            .expected_key = "key",
            .expected_value = NULL,
            .expected_end = "end",
        },
        {
            .input = "key=,",
            .expected_key = "key",
            .expected_value = NULL,
            .expected_end = "",
        },
    };
    size_t i;

    (void) data;

    for (i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        char *input = strdup(values[i].input);
        char *value;
        char *key;
        char *end;

        assert_non_null(input);
        fprintf(stderr, "input: '%s'\n", input);

        errno = 0;
        end = get_key_value(input, &key, &value);

        assert_int_equal(errno, 0);
        assert_string_value(values[i].expected_key, key);
        assert_string_value(values[i].expected_value, value);
        assert_string_value(values[i].expected_end, end);

        free(input);
    }
}

void parse_hint_failure(void **data)
{
    struct test_input values[] = {
        {
            .input = "value,",
            .expected_errno = EINVAL,
        },
        {
            .input = "=value,",
            .expected_errno = EINVAL,
        },
        {
            .input = "key=",
            .expected_errno = EINVAL,
        },
        {
            .input = ",",
            .expected_errno = EINVAL,
        },
    };
    size_t i;

    (void) data;

    for (i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        char *input = strdup(values[i].input);
        char *value;
        char *key;
        char *end;

        assert_non_null(input);

        errno = 0;
        end = get_key_value(input, &key, &value);

        assert_int_equal(errno, values[i].expected_errno);
        assert_null(end);

        free(input);
    }
}

struct process_hint_test_data {
    const char *input;
    const char **expected_keys;
    const char **expected_values;
    size_t size;
};

static void parse_hint_list_key_value(void **data)
{
    const char *expected_keys[] = {
        "key", "key2"
    };
    const char *expected_values[] = {
        "value", "value2"
    };
    struct process_hint_test_data values[] = {
        {
            .input = "key=value",
            .expected_keys = expected_keys,
            .expected_values = expected_values,
            .size = 1,
        },
        {
            .input = "key=value,key2=value2",
            .expected_keys = expected_keys,
            .expected_values = expected_values,
            .size = 2,
        },
        {
            .input = "key=value,key2=value2,",
            .expected_keys = expected_keys,
            .expected_values = expected_values,
            .size = 2,
        },
    };
    size_t i;

    (void) data;

    for (i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        struct hinttab hinttab;
        struct buf hints;
        size_t j;
        int rc;

        hints.data = (char *)values[i].input;
        hints.len = strlen(values[i].input);

        rc = process_hints(&hints, &hinttab);
        assert_return_code(rc, -rc);

        assert_int_equal(values[i].size, hinttab.count);
        for (j = 0; j < values[i].size; j++) {
            assert_string_equal(values[i].expected_keys[j],
                                hinttab.hints[j].key);
            assert_string_equal(values[i].expected_values[j],
                                hinttab.hints[j].value);
        }

        free(hinttab.context);
    }
}

int main(void)
{
    const struct CMUnitTest test_hints[] = {
        cmocka_unit_test(parse_hint_key_value),
        cmocka_unit_test(parse_hint_failure),
        cmocka_unit_test(parse_hint_list_key_value),
    };

    phobos_init();
    atexit(phobos_fini);

    return cmocka_run_group_tests(test_hints, NULL, NULL);
}

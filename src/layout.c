/*
 * Copyright (C) 2022 Commissariat a l'energie atomique et aux energies
 *                    alternatives
 *
 * SPDX-License-Identifer: GPL-2.0-only
 */
#define _GNU_SOURCE /* asprintf */
#include "layout.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

struct layout_comp {
    uint64_t stripe_count;
    uint64_t stripe_size;
    uint64_t pattern;
    char pool_name[LOV_MAXPOOLNAME + 1];
    struct {
        uint64_t start;
        uint64_t end;
    } extent;
};

const char *layout_pattern2str(uint32_t pattern)
{
    switch (pattern) {
    case LLAPI_LAYOUT_RAID0:
        return "raid0";
    case LLAPI_LAYOUT_MDT:
        return "mdt";
    default:
        return "unknown";
    }
}

int csv_add_key_value_u64(char **str, const char *key, uint64_t value,
                          uint64_t default_value, const char *default_str)
{
    char *tmp;
    int rc;

    if (*str) {
        if (default_str && value == default_value)
            rc = asprintf(&tmp, "%s,%s=%s", *str, key, default_str);
        else
            rc = asprintf(&tmp, "%s,%s=%lu", *str, key, value);
    } else {
        if (default_str && value == default_value)
            rc = asprintf(&tmp, "%s=%s", key, default_str);
        else
            rc = asprintf(&tmp, "%s=%lu", key, value);
    }

    if (rc == -1)
        return -ENOMEM;

    free(*str);
    *str = tmp;
    return 0;
}

int csv_add_key_value_str(char **str, const char *key, const char *value)
{
    char *tmp;
    int rc;

    rc = asprintf(&tmp, "%s,%s=%s", *str, key, value);
    if (rc == -1)
        return -ENOMEM;

    free(*str);
    *str = tmp;
    return 0;
}

int layout_comp2str(struct layout_comp *component, char **str,
                    bool is_composite)
{
    int rc;

    rc = csv_add_key_value_u64(str, "stripe_count", component->stripe_count,
                               LLAPI_LAYOUT_WIDE, "-1");
    if (rc)
        return rc;

    rc = csv_add_key_value_u64(str, "stripe_size", component->stripe_size,
                               -1, NULL);
    if (rc)
        goto free_str;

    rc = csv_add_key_value_str(str, "pattern",
                               layout_pattern2str(component->pattern));
    if (rc)
        goto free_str;

    if (*component->pool_name != '\0') {
        rc = csv_add_key_value_str(str, "pool_name", component->pool_name);
        if (rc)
            goto free_str;
    }

    if (is_composite) {
        rc = csv_add_key_value_u64(str, "extent_start", component->extent.start,
                                   -1, NULL);
        if (rc)
            goto free_str;

        rc = csv_add_key_value_u64(str, "extent_end", component->extent.end,
                                   LUSTRE_EOF, "EOF");
        if (rc)
            goto free_str;
    }

    return 0;

free_str:
    free(*str);
    *str = NULL;
    return rc;
}

int layout_component2str(struct llapi_layout *layout, char **str)
{
    struct layout_comp component;
    bool is_composite;
    int rc;

    is_composite = llapi_layout_is_composite(layout);

    rc = llapi_layout_stripe_count_get(layout, &component.stripe_count);
    if (rc)
        return -errno;

    rc = llapi_layout_stripe_size_get(layout, &component.stripe_size);
    if (rc)
        return -errno;

    if (is_composite) {
        rc = llapi_layout_comp_extent_get(layout,
                                          &component.extent.start,
                                          &component.extent.end);
        if (rc)
            return -errno;
    }

    rc = llapi_layout_pattern_get(layout, &component.pattern);
    if (rc)
        return -errno;

    rc = llapi_layout_pool_name_get(layout,
                                    component.pool_name,
                                    sizeof(component.pool_name));
    if (rc)
        return -errno;

    rc = layout_comp2str(&component, str, is_composite);
    if (rc)
        return -ENOMEM;

    return 0;
}

/**
 * Search for the first key-value pair in a string. Each pair is seperated by
 * a comma. Key and values are seperated by an equal sign.
 *
 * Example:
 * input = strdup("k1=v1,k2=v2,k3=v3\0");
 *
 * rest = csv_next_key_value(input, &key, &value);
 *
 * input -> "k1\0v1\0k2=v2,k3=v3\0"
 * rest  -> "k2=v2,k3=v3\0"
 * key   -> "k1\0"
 * value -> "v1\0"
 *
 * @param[in]  input   input string to parse (this string is modified by the
 *                     function)
 * @param[out] key     beginning of the first key (null-terminated)
 * @param[out] value   beginning of the value associated to \p key
 *                     (null-terminated)
 *
 * @return             Pointer to the rest of the input string. This pointer can
 *                     be passed to this function to parse the rest of the
 *                     string.
 *                     NULL if no pair is found.
 */
char *csv_next_key_value(char *input, const char **key, const char **value)
{
    char *equal;
    char *comma;
    bool end;

    equal = strchr(input, '=');
    if (!equal) {
        errno = ENODATA;
        return NULL;
    }

    *equal = '\0';
    *key = input;

    comma = strchrnul(equal + 1, ',');
    if (*comma == '\0' && comma == (equal - 1)) {
        errno = ENODATA;
        return NULL;
    }

    end = (*comma == '\0');
    *comma = '\0';
    *value = equal + 1;

    return !end ? comma + 1 : NULL;

}

int str2uint64_t(const char *value, uint64_t *result)
{
    char *end;

    *result = strtoull(value, &end, 10);

    if ((!*result && end == value) || *end != '\0')
        return -EINVAL;
    else if (*result == ULLONG_MAX && errno)
        return -errno;
    else
        return 0;
}

int layout_add_stripe_size(struct llapi_layout *layout, const char *value)
{
    uint64_t stripe_size;
    int rc;

    rc = str2uint64_t(value, &stripe_size);
    if (rc)
        return rc;

    rc = llapi_layout_stripe_size_set(layout, stripe_size);
    if (rc)
        return -errno;

    return 0;
}

int layout_add_stripe_count(struct llapi_layout *layout, const char *value)
{
    uint64_t stripe_count;
    int rc;

    rc = str2uint64_t(value, &stripe_count);
    if (rc)
        return rc;

    rc = llapi_layout_stripe_count_set(layout, stripe_count);
    if (rc)
        return -errno;

    return 0;
}

int str2pattern(const char *value, uint64_t *pattern)
{
    if (!strcmp(value, "raid0"))
        *pattern = LLAPI_LAYOUT_RAID0;
    else if (!strcmp(value, "mdt"))
        *pattern = LLAPI_LAYOUT_MDT;
    else
        return -EINVAL;

    return 0;
}

int layout_add_pattern(struct llapi_layout *layout, const char *value)
{
    uint64_t pattern;
    int rc;

    rc = str2pattern(value, &pattern);
    if (rc)
        return rc;

    rc = llapi_layout_pattern_set(layout, pattern);
    if (rc)
        return -errno;

    return 0;
}

int add_component_from_string(struct llapi_layout *layout, const char *csv_comp)
{
    bool start_seen = false;
    bool end_seen = false;
    uint64_t extent_start;
    uint64_t extent_end;
    char *striping_base;
    const char *value;
    const char *key;
    char *striping;
    int rc = 0;

    striping = strdup(csv_comp);
    if (!striping)
        return -errno;

    striping_base = striping;

    while ((striping = csv_next_key_value(striping, &key, &value))) {
        if (!strcmp(key, "stripe_size")) {
            rc = layout_add_stripe_size(layout, value);
        } else if (!strcmp(key, "stripe_count")) {
            rc = layout_add_stripe_count(layout, value);
        } else if (!strcmp(key, "pattern")) {
            rc = layout_add_pattern(layout, value);
        } else if (!strcmp(key, "pool_name")) {
            rc = llapi_layout_pool_name_set(layout, value);
            if (rc)
                rc = -errno;
        } else if (!strcmp(key, "extent_start")) {
            rc = str2uint64_t(value, &extent_start);
            start_seen = true;
        } else if (!strcmp(key, "extent_end")) {
            rc = str2uint64_t(value, &extent_end);
            end_seen = true;
        }

        if (rc)
            goto free_striping;
    }

    if (start_seen && end_seen) {
        rc = llapi_layout_comp_extent_set(layout, extent_start, extent_end);
        if (rc)
            rc = -errno;
    }

free_striping:
    free(striping_base);
    return rc;
}

int build_component_name(int index, char **name)
{
    int rc;

    rc = asprintf(name, "layout_comp%d", index);
    if (rc == -1)
        return -ENOMEM;

    return 0;
}

int layout_from_object_md(struct pho_attrs *attrs,
                          struct llapi_layout **layout)
{
    const char *value = NULL;
    int rc = 0;

    if (pho_attrs_is_empty(attrs))
        return -ENODATA;

    *layout = llapi_layout_alloc();
    if (!*layout)
        return -ENOMEM;

    value = pho_attr_get(attrs, "layout");
    if (value) {
        rc = add_component_from_string(*layout, value);
        if (rc)
            goto free_layout;
    } else {
        char *comp_name;
        int i = 0;

        do {
            rc = build_component_name(i, &comp_name);
            if (rc)
                return -ENOMEM;

            value = pho_attr_get(attrs, comp_name);
            if (!value)
                break;

            if (i > 0) {
                rc = llapi_layout_comp_add(*layout);
                if (rc)
                    goto free_layout;
            }

            rc = add_component_from_string(*layout, value);
            if (rc)
                goto free_layout;

            i++;
        } while (value);
    }

    return 0;

free_layout:
    llapi_layout_free(*layout);
    return rc;
}
/* vim:expandtab:shiftwidth=4:tabstop=4: */

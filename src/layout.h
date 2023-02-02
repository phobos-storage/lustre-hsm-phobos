/*
 * Copyright (C) 2022 Commissariat a l'energie atomique et aux energies
 *                    alternatives
 *
 * SPDX-License-Identifer: GPL-2.0-only
 */
#ifndef LAYOUT_H
#define LAYOUT_H

#include <lustre/lustreapi.h>
#include <phobos_store.h>

/**
 * Fetch the layout information from Phobos metadata.
 *
 * @param[in]  attrs    User metadata of the object
 * @param[out] layout   Layout of the file
 *
 * @return     0 on success, negative POSIX error code on failure
 */
int layout_from_object_md(struct pho_attrs *attrs,
                          struct llapi_layout **layout);

/**
 * Convert each element of the current component of the layout to a comma
 * separated list.
 *
 * @param[in]  layout  layout whose component to convert
 * @param[out] str     allocated string containing the layout information
 *
 * @return     0 on success, negative POSIX error on failure
 */
int layout_component2str(struct llapi_layout *layout, char **str);

/**
 * Convert the string pointed to by value into a uint64_t
 *
 * \param[in]  value  string representing an unsigned integer
 * \param[out] result integer value represented by the string
 *
 * \return            0 on sucess, negative POSIX error code on error
 */
int str2uint64_t(const char *value, uint64_t *result);

#endif
/*
 * vim:expandtab:shiftwidth=4:tabstop=4:
 */

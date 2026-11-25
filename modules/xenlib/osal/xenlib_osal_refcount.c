/*
 * SPDX-FileCopyrightText: 2026 EPAM Systems
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <xenlib_osal_platform.h>

void xenlib_osal_refcount_init(xenlib_osal_refcount_t *ref)
{
	sys_refcount_init(ref);
}

void xenlib_osal_refcount_get(xenlib_osal_refcount_t *ref)
{
	sys_refcount_inc(ref);
}

bool xenlib_osal_refcount_put(xenlib_osal_refcount_t *ref)
{
	return sys_refcount_dec(ref);
}

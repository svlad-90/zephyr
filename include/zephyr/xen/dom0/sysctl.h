/*
 * Copyright (c) 2025 EPAM Systems
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 *
 * @brief Xen System Control Interface
 */

#ifndef __XEN_DOM0_SYSCTL_H__
#define __XEN_DOM0_SYSCTL_H__
#include <zephyr/xen/generic.h>
#include <zephyr/xen/public/sysctl.h>
#include <zephyr/xen/public/xen.h>

/**
 * @brief Retrieves information about the host system.
 *
 * @param[out] info A pointer to a `struct xen_sysctl_physinfo` structure where the
 *             retrieved information will be stored.
 * @retval 0 on success.
 * @retval -errno on failure.
 */
int xen_sysctl_physinfo(struct xen_sysctl_physinfo *info);

/**
 * @brief Performs a Xen trace buffer sysctl operation.
 *
 * @param[in,out] tbuf_op A pointer to a `struct xen_sysctl_tbuf_op` object
 *                        that defines the trace buffer operation and receives
 *                        any output values returned by Xen.
 * @retval 0 on success.
 * @retval -errno on failure.
 */
int xen_sysctl_tbuf_op(struct xen_sysctl_tbuf_op *tbuf_op);

/**
 * @brief Performs a Xen CPU hotplug sysctl operation.
 *
 * @param[in] cpu_hotplug A pointer to a `struct xen_sysctl_cpu_hotplug` object
 *                        that defines the physical CPU and hotplug operation.
 *
 * @return 0 on success, negative errno value on failure.
 * @retval -EINVAL @p cpu_hotplug is ``NULL``.
 */
int xen_sysctl_cpu_hotplug(struct xen_sysctl_cpu_hotplug *cpu_hotplug);

/**
 * @brief Retrieves information about Xen domains.
 *
 * @param[out] domaininfo A pointer to the `xen_domctl_getdomaininfo` structure
 *                        to store the retrieved domain information.
 * @param first The first domain ID to retrieve information for.
 * @param num The maximum number of domains to retrieve information for.
 * @retval 0 on success.
 * @retval -errno on failure.
 */
int xen_sysctl_getdomaininfo(struct xen_domctl_getdomaininfo *domaininfo,
			     uint16_t first, uint16_t num);

#endif /* __XEN_DOM0_SYSCTL_H__ */

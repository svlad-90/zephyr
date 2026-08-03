/*
 * Copyright (c) 2026 EPAM Systems
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_XEN_VCPU_H_
#define ZEPHYR_INCLUDE_XEN_VCPU_H_

#include <stdint.h>

#include <zephyr/xen/public/vcpu.h>

/**
 * @brief Register a guest-owned vcpu_info location for a VCPU.
 *
 * @param vcpuid VCPU identifier to operate on.
 * @param mfn Machine frame containing the vcpu_info structure.
 * @param offset Offset of the vcpu_info structure within @p mfn.
 * @return 0 on success, negative errno value on failure.
 */
int xen_vcpu_register_vcpu_info(unsigned int vcpuid, uint64_t mfn, uint32_t offset);

#endif /* ZEPHYR_INCLUDE_XEN_VCPU_H_ */

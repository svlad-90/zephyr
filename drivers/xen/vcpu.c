/*
 * Copyright (c) 2026 EPAM Systems
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/arch/arm64/hypercall.h>
#include <zephyr/xen/public/vcpu.h>
#include <zephyr/xen/vcpu.h>

int xen_vcpu_register_vcpu_info(unsigned int vcpuid, uint64_t mfn, uint32_t offset)
{
	struct vcpu_register_vcpu_info info = {
		.mfn = mfn,
		.offset = offset,
	};

	return HYPERVISOR_vcpu_op(VCPUOP_register_vcpu_info, vcpuid, &info);
}

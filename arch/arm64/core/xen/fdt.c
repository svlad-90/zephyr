/*
 * Copyright (c) 2026 EPAM Systems
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/xen/generic.h>

#include <zephyr/arch/common/init.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include <zephyr/xen/fdt.h>

LOG_MODULE_REGISTER(xen_fdt);

#define FDT_MAGIC 0xd00dfeed

/* Raw early-boot handoff from reset.S. Survives data/BSS initialization. */
__noinit uintptr_t z_arm64_xen_fdt_addr;
__noinit uint32_t z_arm64_xen_fdt_size;

/* Validated Zephyr-owned FDT copy and CPU-endian size for runtime consumers. */
static uint8_t xen_fdt[CONFIG_XEN_FDT_MAX_SIZE] __aligned(XEN_PAGE_SIZE);
static uint32_t xen_fdt_size;

uintptr_t get_xen_fdt_ptr(uint32_t *fdt_size)
{
	if (fdt_size != NULL) {
		*fdt_size = xen_fdt_size;
	}

	return (uintptr_t)xen_fdt;
}

void xen_copy_fdt(void)
{
	const uint8_t *src = (const uint8_t *)z_arm64_xen_fdt_addr;
	uint32_t magic = sys_get_be32(src);
	uint32_t size = sys_be32_to_cpu(z_arm64_xen_fdt_size);

	if (magic != FDT_MAGIC) {
		LOG_ERR("Invalid Xen device tree magic: 0x%x", magic);
		k_panic();
	}

	if (size > CONFIG_XEN_FDT_MAX_SIZE) {
		LOG_ERR("Xen device tree size 0x%x exceeds max 0x%x",
			size, CONFIG_XEN_FDT_MAX_SIZE);
		k_panic();
	}

	arch_early_memcpy(xen_fdt, src, size);
	xen_fdt_size = size;
}

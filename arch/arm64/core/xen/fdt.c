/*
 * Copyright (c) 2023 EPAM Systems
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/xen/generic.h>

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/sys/byteorder.h>

#define FDT_SIZE_BYTES	0x8000

/* x0 from bootloader stored on boot */
__attribute__((__section__(".data")))
uintptr_t xen_fdt_addr;

__attribute__((__section__(".data")))
uint32_t xen_fdt_size;

/* buffer for device tree blob */
__attribute__((__section__(".data")))
uint8_t xen_fdt[FDT_SIZE_BYTES] __aligned(XEN_PAGE_SIZE);

/*
 * xen_copy_fdt() runs very early (from z_prep_c, right after arch_data_copy()),
 * so it uses a self-contained byte copy/set rather than any libc or early
 * helper symbol (Zephyr 4.4 provides no z_early_memcpy/z_early_memset).
 */
static void fdt_bset(uint8_t *dst, uint8_t c, size_t n)
{
	while (n--) {
		*dst++ = c;
	}
}

static void fdt_bcopy(uint8_t *dst, const uint8_t *src, size_t n)
{
	while (n--) {
		*dst++ = *src++;
	}
}

void xen_copy_fdt(void)
{
	uint32_t size = sys_be32_to_cpu(xen_fdt_size);

	if (size > FDT_SIZE_BYTES) {
		size = FDT_SIZE_BYTES;
	}
	fdt_bset(xen_fdt, 0, FDT_SIZE_BYTES);
	fdt_bcopy(xen_fdt, (const uint8_t *)xen_fdt_addr, size);
}

static int xen_print_fdt(void)
{
	printk("Saved Xen device tree address is 0x%lx, size = 0x%x\n",
	       xen_fdt_addr, sys_be32_to_cpu(xen_fdt_size));
	return 0;
}

SYS_INIT(xen_print_fdt, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE);

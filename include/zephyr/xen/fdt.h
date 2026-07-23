/*
 * Copyright (c) 2026 EPAM Systems
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_ZEPHYR_XEN_FDT_H_
#define ZEPHYR_INCLUDE_ZEPHYR_XEN_FDT_H_

#include <stdint.h>

/**
 * @brief Return a pointer to the saved Xen-provided FDT copy.
 *
 * The returned pointer refers to the Zephyr-owned copy made during early boot
 * by xen_copy_fdt(). If @p fdt_size is not NULL, it is set to the number of
 * valid bytes in the saved copy. A reported size of 0 means no valid FDT copy
 * has been recorded.
 *
 * @param fdt_size Optional output for the saved FDT size in bytes.
 *
 * @return Pointer to the saved FDT copy.
 */
uintptr_t get_xen_fdt_ptr(uint32_t *fdt_size);

/**
 * @brief Early boot hook that saves the Xen-provided boot FDT.
 *
 * This function is intended for the arm64 startup path. It validates the
 * Xen-provided FDT header and copies the blob into internal storage used by
 * get_xen_fdt_ptr(). Runtime consumers should use get_xen_fdt_ptr() instead
 * of reading the early boot handoff symbols directly.
 */
void xen_copy_fdt(void);

#endif /* ZEPHYR_INCLUDE_ZEPHYR_XEN_FDT_H_ */

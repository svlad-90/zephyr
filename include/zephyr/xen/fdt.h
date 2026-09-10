/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (c) 2026 EPAM Systems
 */

#ifndef ZEPHYR_INCLUDE_ZEPHYR_XEN_FDT_H_
#define ZEPHYR_INCLUDE_ZEPHYR_XEN_FDT_H_

#include <stdint.h>

/**
 * @brief Return a pointer to the saved Xen-provided FDT copy.
 *
 * The returned pointer refers to the Zephyr-owned copy made by the early
 * firmware argument parse hook. If @p fdt_size is not NULL, it is set to the
 * number of valid bytes in the saved copy. A reported size of 0 means no valid
 * FDT copy has been recorded.
 *
 * @param fdt_size Optional output for the saved FDT size in bytes.
 *
 * @return Pointer to the saved FDT copy.
 */
uintptr_t get_xen_fdt_ptr(uint32_t *fdt_size);

#endif /* ZEPHYR_INCLUDE_ZEPHYR_XEN_FDT_H_ */

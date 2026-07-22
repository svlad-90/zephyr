/* SPDX-License-Identifier: MIT */
/******************************************************************************
 * vcpu.h
 *
 * VCPU initialisation, query, and hotplug.
 *
 * Copyright (c) 2005, Keir Fraser <keir@xensource.com>
 */

#ifndef __XEN_PUBLIC_VCPU_H__
#define __XEN_PUBLIC_VCPU_H__

#include "xen.h"

/*
 * Prototype for this hypercall is:
 *  long vcpu_op(int cmd, unsigned int vcpuid, void *extra_args)
 * @cmd        == VCPUOP_??? (VCPU operation).
 * @vcpuid     == VCPU to operate on.
 * @extra_args == Operation-specific extra arguments (NULL if none).
 */

/*
 * Register a memory location in the guest address space for the
 * vcpu_info structure.  This allows the guest to place the vcpu_info
 * structure in a convenient place, such as in a per-cpu data area.
 * The pointer need not be page aligned, but the structure must not
 * cross a page boundary.
 *
 * This may be called only once per vcpu.
 */
#define VCPUOP_register_vcpu_info   10  /* arg == vcpu_register_vcpu_info_t */
struct vcpu_register_vcpu_info {
	uint64_t mfn;    /* mfn of page to place vcpu_info */
	uint32_t offset; /* offset within page */
	uint32_t rsvd;   /* unused */
};
typedef struct vcpu_register_vcpu_info vcpu_register_vcpu_info_t;
DEFINE_XEN_GUEST_HANDLE(vcpu_register_vcpu_info_t);

#endif /* __XEN_PUBLIC_VCPU_H__ */

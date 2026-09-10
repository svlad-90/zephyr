# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2021 EPAM Systems

# Needed to separate definitions in common Xen headers
zephyr_compile_options($<$<COMPILE_LANGUAGE:ASM>:-D__ASSEMBLY__>)

zephyr_library_sources(hypercall.S)
zephyr_library_sources_ifdef(CONFIG_XEN_EVENTS enlighten.c)
zephyr_library_sources_ifdef(CONFIG_SAVE_XEN_FDT fdt.c)

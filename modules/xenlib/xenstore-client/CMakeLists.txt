# SPDX-License-Identifier: Apache-2.0

add_library(XEN_STORE_CLIENT INTERFACE)

target_include_directories(XEN_STORE_CLIENT INTERFACE
  ${ZEPHYR_XENLIB_MODULE_DIR}/xenstore-client/include)

zephyr_library()
xenlib_include_zephyr_headers()
zephyr_library_sources(
  ${ZEPHYR_XENLIB_MODULE_DIR}/xenstore-client/src/xenstore_client.c)
zephyr_library_link_libraries(XEN_STORE_CLIENT XEN_STORE_COMMON)
zephyr_include_directories(
  ${ZEPHYR_XENLIB_MODULE_DIR}/xenstore-client/include)

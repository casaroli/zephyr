# Copyright (c) 2023 Antmicro
# SPDX-License-Identifier: Apache-2.0

add_library(TMPFS INTERFACE)
target_include_directories(TMPFS INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

zephyr_library()
zephyr_library_sources(
  tmpfs_impl.c
#   ext2_disk_access.c
#   ext2_bitmap.c
#   ext2_diskops.c
)
zephyr_library_sources_ifdef(CONFIG_FILE_SYSTEM tmpfs_ops.c)
# zephyr_library_sources_ifdef(CONFIG_FILE_SYSTEM_MKFS tmp_format.c)

zephyr_library_link_libraries(TMPFS)

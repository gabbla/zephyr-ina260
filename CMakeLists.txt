# Copyright (c) 2023 Golioth, Inc.
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources_ifdef(CONFIG_INA260 PRIVATE ina260.c)
zephyr_library_sources_ifdef(CONFIG_INA260_FAKE PRIVATE ina260_fake.c)

zephyr_include_directories_ifdef(CONFIG_INA260 include)

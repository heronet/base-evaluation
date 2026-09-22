# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(base_test4)
# Keep DWARF for native footprint reports and structure layout inspection.
# Apply uniformly to Zephyr and the app; no CONFIG_DEBUG_INFO symbol is needed.
zephyr_compile_options(-g)
target_sources(app PRIVATE src/main.c)

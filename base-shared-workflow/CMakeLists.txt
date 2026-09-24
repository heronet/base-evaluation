# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(base_shared_workflow)
target_sources(app PRIVATE src/main.c)
if(DEFINED BASE_BUILD_TAG)
  target_compile_definitions(app PRIVATE BASE_BUILD_TAG="${BASE_BUILD_TAG}")
endif()

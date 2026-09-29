# PS5 RetroArch - cross-compile RPCS3's libretro core and its static
# dependencies (LLVM, FFmpeg's CMake users) with this repository's SDK; never
# search host headers or libraries.
# Copyright (C) 2026 Mihawk
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The console's CPU is Zen 2 (AVX2, FMA, BMI2), so code is built for it rather
# than for RPCS3's -march=native. The core's link, as every native core's:
# tools/build-rpcs3.sh passes PS5_CORE_LINK_INPUTS (the core-local destructor
# registry) and PS5_EMPTY_LIBS (empty libm, librt, libpthread, libdl and libutil
# archives, whose functions are the console's own).
set(CMAKE_SYSTEM_NAME FreeBSD)
set(CMAKE_SYSTEM_VERSION 14)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER "$ENV{PS5_PAYLOAD_SDK}/bin/prospero-clang")
set(CMAKE_CXX_COMPILER "$ENV{PS5_PAYLOAD_SDK}/bin/prospero-clang++")
set(CMAKE_AR "$ENV{PS5_PAYLOAD_SDK}/bin/prospero-ar")
set(CMAKE_RANLIB "$ENV{PS5_PAYLOAD_SDK}/bin/prospero-ranlib")
set(CMAKE_FIND_ROOT_PATH "$ENV{PS5_PAYLOAD_SDK}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# pkg-config answers with no package: the host's would add its /usr/include,
# which shadows the SDK's headers (OpenAL Soft found the host's D-Bus that way).
set(ENV{PKG_CONFIG_LIBDIR} "/nonexistent-pkg-config-for-ps5")
set(ENV{PKG_CONFIG_PATH} "")
set(CMAKE_C_FLAGS_INIT "-march=znver2")
set(CMAKE_CXX_FLAGS_INIT "-march=znver2")
# Configure-time test programs link like an executable against the console's
# libraries; the core itself is a shared object the title's loader binds.
# The empty archives answer configure checks that link -lm and the like.
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "-L${PS5_EMPTY_LIBS} -nostdlib -nostartfiles -nodefaultlibs -Wl,-e,0 -lkernel_web -lSceLibcInternal -lScePosixForWebKit")
set(CMAKE_SHARED_LINKER_FLAGS_INIT
    "-nostdlib -nodefaultlibs -Wl,-z,undefs -Wl,--build-id=sha1 -Wl,-T,${CMAKE_CURRENT_LIST_DIR}/../native/ps5-core.ld ${PS5_CORE_LINK_INPUTS} -L${PS5_EMPTY_LIBS} -lkernel_web -lSceLibcInternal -lScePosixForWebKit")
# OpenAL Soft is C++20 modules; its dependency scan needs the SDK's view.
set(CMAKE_CXX_COMPILER_CLANG_SCAN_DEPS "${CMAKE_CURRENT_LIST_DIR}/clang-scan-deps")

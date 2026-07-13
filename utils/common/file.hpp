// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_UTILS_FILE_COMMON_HPP
#define WFX_UTILS_FILE_COMMON_HPP

/* Common stuff in file operations */
#include <sys/types.h>
using WFXFileDescriptor = int;
using WFXFileSize = off_t;

constexpr WFXFileDescriptor WFX_INVALID_FILE = -1;

#endif // WFX_UTILS_FILE_COMMON_HPP
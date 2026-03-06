// Copyright 2026 Yuan Tong. All rights reserved.
// SPDX-License-Identifier: BSD-2-Clause

#include "nvenc.h"

namespace avif_nvenc::nvenc
{

#if defined(_WIN32)
static const std::string NVENC_MODULE_NAME = "nvEncodeAPI64.dll";
#else
static const std::string NVENC_MODULE_NAME = "libnvidia-encode.so.1";
#endif

module_loader module { NVENC_MODULE_NAME };

#pragma clang diagnostic push
#pragma ide diagnostic ignored "bugprone-macro-parentheses"
#define NVENC_API_DEF(name)               \
    nvenc_loader<decltype(::name) *> name \
    {                                     \
        #name                             \
    }
#pragma clang diagnostic pop

NVENC_API_DEF(NvEncodeAPIGetMaxSupportedVersion);
NVENC_API_DEF(NvEncodeAPICreateInstance);

} // namespace avif_nvenc::nvenc
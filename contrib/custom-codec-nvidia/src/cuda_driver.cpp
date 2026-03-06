// Copyright 2026 Yuan Tong. All rights reserved.
// SPDX-License-Identifier: BSD-2-Clause

#include "cuda_driver.h"

namespace avif_nvenc::cuda
{
#if defined(_WIN32)
static const std::string CUDA_MODULE_NAME = "nvcuda.dll";
#else
static const std::string CUDA_MODULE_NAME = "libcuda.so.1";
#endif

module_loader module { CUDA_MODULE_NAME };

#pragma clang diagnostic push
#pragma ide diagnostic ignored "bugprone-macro-parentheses"
#define CUDA_API_DEF(name)               \
    cuda_loader<decltype(::name) *> name \
    {                                    \
        #name                            \
    }

#define CUDA_API_DEF_ALIAS(name, symbol)   \
    cuda_loader<decltype(::symbol) *> name \
    {                                      \
        #symbol                            \
    }
#pragma clang diagnostic pop

CUDA_API_DEF(cuInit);
CUDA_API_DEF(cuDeviceGet);
CUDA_API_DEF(cuDevicePrimaryCtxRetain);
CUDA_API_DEF_ALIAS(cuDevicePrimaryCtxRelease, cuDevicePrimaryCtxRelease_v2);
CUDA_API_DEF(cuGetErrorString);
CUDA_API_DEF(cuGetErrorName);

#undef CUDA_API_DEF
#undef CUDA_API_DEF_ALIAS
} // namespace avif_nvenc::cuda

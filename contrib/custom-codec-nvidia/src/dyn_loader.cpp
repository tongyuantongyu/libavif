// Copyright 2026 Yuan Tong. All rights reserved.
// SPDX-License-Identifier: BSD-2-Clause

#include "dyn_loader.h"

namespace avif_nvenc {

void atomic_loader_loading_sentinel()
{
  std::abort();
}

void atomic_loader_bad_sentinel()
{
  std::abort();
}

}

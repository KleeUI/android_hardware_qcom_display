/*
 * Copyright (c) 2018-2021 The Linux Foundation. All rights reserved.
 * Copyright (C) 2026 The KleeUI Project
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef QTI_ALLOCATOR4_H
#define QTI_ALLOCATOR4_H

#include <vendor/qti/hardware/display/allocator/4.0/IQtiAllocator.h>

#include "gr_buf_mgr.h"

namespace vendor::qti::hardware::display::allocator::V4_0::implementation {

class QtiAllocator4 final : public IQtiAllocator {
 public:
  QtiAllocator4();

  ::android::hardware::Return<void> allocate(
      const ::android::hardware::hidl_vec<uint8_t> &descriptor, uint32_t count,
      allocate_cb callback) override;

 private:
  gralloc::BufferManager *buffer_manager_ = nullptr;
};

}  // namespace vendor::qti::hardware::display::allocator::V4_0::implementation

#endif  // QTI_ALLOCATOR4_H

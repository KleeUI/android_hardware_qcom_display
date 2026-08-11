/*
 * Copyright (c) 2018-2021 The Linux Foundation. All rights reserved.
 * Copyright (C) 2026 The KleeUI Project
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "QtiAllocator4.h"

#include <cutils/properties.h>
#include <log/log.h>

#include <vector>

#include "QtiMapper4.h"
#include "gr_utils.h"

namespace vendor::qti::hardware::display::allocator::V4_0::implementation {
namespace {

using ::android::hardware::Void;
using ::android::hardware::graphics::mapper::V4_0::Error;
using ::android::hardware::hidl_handle;
using ::android::hardware::hidl_vec;

void ReadGrallocProperties(gralloc::GrallocProperties *properties) {
  properties->use_system_heap_for_sensors =
      property_get_bool("vendor.gralloc.use_system_heap_for_sensors", true);
  properties->ubwc_disable = property_get_bool("vendor.gralloc.disable_ubwc", false);
  properties->ahardware_buffer_disable =
      property_get_bool("vendor.gralloc.disable_ahardware_buffer", false);
}

}  // namespace

QtiAllocator4::QtiAllocator4() {
  gralloc::GrallocProperties properties;
  ReadGrallocProperties(&properties);
  buffer_manager_ = gralloc::BufferManager::GetInstance();
  buffer_manager_->SetGrallocDebugProperties(properties);
}

::android::hardware::Return<void> QtiAllocator4::allocate(
    const hidl_vec<uint8_t> &descriptor, uint32_t count, allocate_cb callback) {
  gralloc::BufferDescriptor decoded_descriptor;
  auto error =
      ::vendor::qti::hardware::display::mapper::V4_0::implementation::QtiMapper::Decode(
          descriptor, &decoded_descriptor);
  if (error != gralloc::Error::NONE) {
    callback(static_cast<Error>(error), 0, hidl_vec<hidl_handle>());
    return Void();
  }

  std::vector<hidl_handle> buffers;
  buffers.reserve(count);
  for (uint32_t index = 0; index < count; ++index) {
    buffer_handle_t buffer = nullptr;
    error = buffer_manager_->AllocateBuffer(decoded_descriptor, &buffer);
    if (error != gralloc::Error::NONE) {
      break;
    }
    buffers.emplace_back(buffer);
  }

  uint32_t stride = 0;
  hidl_vec<hidl_handle> hidl_buffers;
  if (error == gralloc::Error::NONE && !buffers.empty()) {
    stride = static_cast<uint32_t>(
        QTI_HANDLE_CONST(buffers.front().getNativeHandle())->width);
    hidl_buffers.setToExternal(buffers.data(), buffers.size());
  }
  callback(static_cast<Error>(error), stride, hidl_buffers);

  for (const auto &buffer : buffers) {
    buffer_manager_->ReleaseBuffer(QTI_HANDLE_CONST(buffer.getNativeHandle()));
  }
  return Void();
}

}  // namespace vendor::qti::hardware::display::allocator::V4_0::implementation

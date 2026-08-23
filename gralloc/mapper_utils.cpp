/*
 * Copyright 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Changes from Qualcomm Innovation Center are provided under the following license:
 * Copyright (c) 2023-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "gr_snap_helper.h"
#include "mapper_utils.h"

namespace mapper {

gralloc::GrallocSnapHelper *snap_helper_ = nullptr;

AIMapper_Error LazyInit() {
  if (snap_helper_ == nullptr) {
    snap_helper_ = gralloc::GrallocSnapHelper::GetInstance();
    if (snap_helper_ == nullptr) {
      ALOGW("Unable to get snap helper");
      return AIMAPPER_ERROR_NO_RESOURCES;
    }
  }
  return AIMAPPER_ERROR_NONE;
}

bool IsMetadataStateSupported() {
  return LazyInit() == AIMAPPER_ERROR_NONE && snap_helper_->IsSnapAllocEnabled();
}

AIMapper_Error GetMetadataState(buffer_handle_t buffer_handle, SnapMetadataType metadata_type,
                                bool *out) {
  if (!buffer_handle) {
    return AIMAPPER_ERROR_BAD_BUFFER;
  }
  if (!out) {
    return AIMAPPER_ERROR_BAD_VALUE;
  }

  AIMapper_Error error = LazyInit();
  if (error != AIMAPPER_ERROR_NONE) {
    return error;
  }

  if (!snap_helper_->IsSnapAllocEnabled()) {
    // Legacy MetaData_t getters validate their own set bits. Let callers probe
    // the real getter instead of treating the missing Snap state backend as if
    // every optional metadata item were absent.
    *out = true;
    return AIMAPPER_ERROR_NONE;
  }

  return static_cast<AIMapper_Error>(snap_helper_->GetMetadataState(
      const_cast<native_handle_t *>(buffer_handle), metadata_type, out));
}

gralloc::BufferDescriptor ConvertAidlToGrallocDescriptor(const BufferDescriptorInfo &info) {
  gralloc::BufferDescriptor desc;

  desc.SetName(std::string(reinterpret_cast<const char *>(info.name.data())));
  desc.SetDimensions(static_cast<int>(info.width), static_cast<int>(info.height));
  desc.SetLayerCount(static_cast<uint32_t>(info.layerCount));
  desc.SetColorFormat(static_cast<int>(info.format));
  desc.SetUsage(static_cast<uint64_t>(info.usage));
  desc.SetReservedSize(static_cast<uint64_t>(info.reservedSize));

  return desc;
}

BufferDescriptorInfo ConvertGrallocToAidlDescriptor(const gralloc::BufferDescriptor &info) {
  BufferDescriptorInfo desc{
      .width = info.GetWidth(),
      .height = info.GetHeight(),
      .layerCount = static_cast<int>(info.GetLayerCount()),
      .format = static_cast<GrallocPixelFormat>(info.GetFormat()),
      .usage = static_cast<GrallocBufferUsage>(info.GetUsage()),
      .reservedSize = static_cast<int64_t>(info.GetReservedSize()),
  };
  auto nameLength = std::min(info.GetName().size(), desc.name.size() - 1);
  memcpy(desc.name.data(), info.GetName().data(), nameLength);

  return desc;
}

AIMapper_Error GetFromBufferDescriptor(BufferDescriptorInfo aidl_desc,
                                       SnapMetadataType metadata_type, void *out,
                                       bool convert_to_hidl_bytestream) {
  AIMapper_Error error = LazyInit();
  if (error == AIMAPPER_ERROR_NONE) {
    return (static_cast<AIMapper_Error>(snap_helper_->GetFromBufferDescriptor(
        ConvertAidlToGrallocDescriptor(aidl_desc), static_cast<uint64_t>(metadata_type), out,
        convert_to_hidl_bytestream)));
  }
  return error;
}

}  // namespace mapper

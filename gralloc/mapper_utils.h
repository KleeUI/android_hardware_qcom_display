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

#pragma once

#include <inttypes.h>

#include <android/hardware/graphics/mapper/IMapper.h>
#include <aidl/android/hardware/graphics/allocator/IAllocator.h>
#include <android/hardware/graphics/mapper/utils/IMapperMetadataTypes.h>
#include <aidl/android/hardware/graphics/allocator/BufferDescriptorInfo.h>
#include <MetadataType.h>
#include <dlfcn.h>
#include <vndksupport/linker.h>
#include <android/binder_manager.h>

#include "gr_buf_descriptor.h"

#define VENDOR_QTI_METADATA(type) \
  { mapper::VENDOR_QTI_METADATA_NAME, static_cast<int64_t>(type) }
#define STANDARD_METADATA(type) \
  { mapper::STANDARD_METADATA_NAME, static_cast<int64_t>(type) }
#define STABLEMAPPER(mapperptr) mapperptr->v5

// When support for explicit UBWC formats is deprecated in gralloc, remove the macro
#define SNAP_SUPPORTS_EXPLICIT_UBWC_FORMATS 1

using aidl::android::hardware::graphics::allocator::BufferDescriptorInfo;
using android::hardware::graphics::mapper::StandardMetadata;

using SnapMetadataType = vendor_qti_hardware_display_common_MetadataType;

typedef AIMapper_Error (*AIMapper_loadIMapperFn)(AIMapper *_Nullable *_Nonnull outImplementation);

namespace gralloc {
class GrallocSnapHelper;
}

namespace mapper {

// Mapper utilities
constexpr const char *_Nonnull STANDARD_METADATA_NAME =
    "android.hardware.graphics.common.StandardMetadataType";
constexpr const char *_Nonnull VENDOR_QTI_METADATA_NAME = "QTI";
static bool isStandardMetadata(AIMapper_MetadataType metadataType) {
  return strcmp(STANDARD_METADATA_NAME, metadataType.name) == 0;
}
static bool isVendorMetadata(AIMapper_MetadataType metadataType) {
  return strcmp(VENDOR_QTI_METADATA_NAME, metadataType.name) == 0;
}

AIMapper_Error GetMetadataState(buffer_handle_t _Nonnull buffer_handle,
                                SnapMetadataType metadata_type, bool *_Nonnull out);
bool IsMetadataStateSupported();
gralloc::BufferDescriptor ConvertAidlToGrallocDescriptor(const BufferDescriptorInfo &info);
BufferDescriptorInfo ConvertGrallocToAidlDescriptor(const gralloc::BufferDescriptor &info);
AIMapper_Error GetFromBufferDescriptor(BufferDescriptorInfo aidl_desc,
                                       SnapMetadataType metadata_type, void *_Nonnull out,
                                       bool convert_to_hidl_bytestream);
AIMapper *_Nullable GetMapperInstance();
bool IsSettable(AIMapper *_Nonnull mapper_, SnapMetadataType type);

static AIMapper_Error GetVendorMetadata(AIMapper *_Nonnull mapper_,
                                        buffer_handle_t _Nonnull buf_hnd, SnapMetadataType type,
                                        void *_Nonnull dest, size_t dest_size) {
  auto size_required =
      STABLEMAPPER(mapper_).getMetadata(buf_hnd, VENDOR_QTI_METADATA(type), dest, dest_size);

  if (size_required < 0) {
    const auto error = static_cast<AIMapper_Error>(-size_required);
    ALOGW_IF(error != AIMAPPER_ERROR_UNSUPPORTED,
             "%s: getMetadata (%" PRId64 ") failed: %d", __FUNCTION__,
             static_cast<int64_t>(type), error);
    return error;
  }

  if ((size_t)size_required != dest_size) {
    ALOGW("getMetadata failed, received %d with buffer size %zu", size_required, dest_size);
    return AIMAPPER_ERROR_BAD_VALUE;
  }
  return AIMAPPER_ERROR_NONE;
}

template <aidl::android::hardware::graphics::common::StandardMetadataType T>
auto GetStandardMetadata(AIMapper *_Nonnull mapper_, buffer_handle_t _Nonnull buf_hnd)
    -> decltype(StandardMetadata<T>::value::decode(nullptr, 0)) {
  using Value = typename StandardMetadata<T>::value;
  std::vector<uint8_t> bytestream;
  bytestream.resize(128);

  auto size_required = STABLEMAPPER(mapper_).getStandardMetadata(
      buf_hnd, static_cast<int64_t>(T), bytestream.data(), bytestream.size());
  if (size_required < 0) {
    ALOGW_IF(-AIMAPPER_ERROR_UNSUPPORTED != size_required,
             "%s: Unexpected error %d from valid getMetadata (%" PRId64 ") call", __FUNCTION__,
             -size_required, static_cast<int64_t>(T));
    return std::nullopt;
  }
  if ((size_t)size_required > bytestream.size()) {
    bytestream.resize(size_required);
    size_required = STABLEMAPPER(mapper_).getStandardMetadata(buf_hnd, static_cast<int64_t>(T),
                                                              bytestream.data(), bytestream.size());
  }
  if (size_required < 0 || (size_t)size_required > bytestream.size()) {
    ALOGW("getMetadata (%" PRId64 ") failed, received %d with buffer size %zu",
          static_cast<int64_t>(T), size_required, bytestream.size());
    return std::nullopt;
  }
  return Value::decode(bytestream.data(), size_required);
}

template <aidl::android::hardware::graphics::common::StandardMetadataType T>
AIMapper_Error SetStandardMetadata(AIMapper *_Nonnull mapper_, buffer_handle_t _Nonnull buf_hnd,
                                   const typename StandardMetadata<T>::value_type &value) {
  using Value = typename StandardMetadata<T>::value;
  std::vector<uint8_t> bytestream;

  auto size_required = Value::encode(value, nullptr, 0);
  if (size_required < 0) {
    ALOGW_IF(-AIMAPPER_ERROR_UNSUPPORTED != size_required,
             "%s: Unexpected error %d during size calculation for setMetadata (%" PRId64 ") call",
             __FUNCTION__, -size_required, static_cast<int64_t>(T));
    return static_cast<AIMapper_Error>(-size_required);
  }
  bytestream.resize(size_required);
  size_required = Value::encode(value, bytestream.data(), bytestream.size());
  if (size_required < 0 || (size_t)size_required > bytestream.size()) {
    ALOGW("setMetadata (%" PRId64 ") failed, calculated size %d with buffer size %zu",
          static_cast<int64_t>(T), size_required, bytestream.size());
    return static_cast<AIMapper_Error>(-size_required);
  }
  return STABLEMAPPER(mapper_).setStandardMetadata(buf_hnd, static_cast<int64_t>(T),
                                                   bytestream.data(), size_required);
}
}  // namespace mapper

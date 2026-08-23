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

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace mapper {

namespace {

std::atomic<AIMapper *> mapper_instance_ = nullptr;
std::mutex mapper_mutex_;
void *mapper_library_handle_ = nullptr;
gralloc::GrallocSnapHelper *snap_helper_ = nullptr;
std::atomic<bool> snap_helper_initialized_ = false;
std::mutex snap_helper_mutex_;

}  // namespace

AIMapper *GetMapperInstance() {
  AIMapper *instance = mapper_instance_.load(std::memory_order_acquire);
  if (instance) {
    return instance;
  }

  std::lock_guard<std::mutex> lock(mapper_mutex_);
  instance = mapper_instance_.load(std::memory_order_relaxed);
  if (instance) {
    return instance;
  }

  std::string suffix = "qti";
  auto allocator =
      aidl::android::hardware::graphics::allocator::IAllocator::fromBinder(ndk::SpAIBinder(
          AServiceManager_checkService("android.hardware.graphics.allocator.IAllocator/default")));
  if (allocator == nullptr) {
    ALOGW("Unable to get allocator, using IMapper library suffix 'qti'");
  } else if (!allocator->getIMapperLibrarySuffix(&suffix).isOk()) {
    ALOGW("Unable to query IMapper library suffix, using 'qti'");
    suffix = "qti";
  }

  std::string lib_name = "mapper." + suffix + ".so";
  void *so = android_load_sphal_library(lib_name.c_str(), RTLD_LOCAL | RTLD_NOW);
  if (!so) {
    ALOGE("Failed to load %s", lib_name.c_str());
    return nullptr;
  }

  auto load_mapper = reinterpret_cast<AIMapper_loadIMapperFn>(dlsym(so, "AIMapper_loadIMapper"));
  if (!load_mapper) {
    ALOGE("Failed to load AIMapper entry point from %s", lib_name.c_str());
    dlclose(so);
    return nullptr;
  }

  AIMapper *loaded_mapper = nullptr;
  AIMapper_Error error = load_mapper(&loaded_mapper);
  if (error != AIMAPPER_ERROR_NONE || loaded_mapper == nullptr) {
    ALOGE("AIMapper_loadIMapper failed %d", error);
    dlclose(so);
    return nullptr;
  }

  auto mapper_version = reinterpret_cast<int32_t *>(dlsym(so, "ANDROID_HAL_MAPPER_VERSION"));
  if (!mapper_version || *mapper_version != AIMAPPER_VERSION_5 ||
      *mapper_version != static_cast<int32_t>(loaded_mapper->version)) {
    ALOGE("IMapper version is missing or does not match stable version %d", AIMAPPER_VERSION_5);
    dlclose(so);
    return nullptr;
  }

  // Keep the DSO loaded for the lifetime of the published AIMapper table.
  mapper_library_handle_ = so;
  mapper_instance_.store(loaded_mapper, std::memory_order_release);
  return loaded_mapper;
}

bool IsSettable(AIMapper *mapper, SnapMetadataType type) {
  static const std::unordered_map<int64_t, bool> is_settable = [mapper]() {
    const AIMapper_MetadataTypeDescription *descriptions = nullptr;
    size_t description_count = 0;
    STABLEMAPPER(mapper).listSupportedMetadataTypes(&descriptions, &description_count);

    std::unordered_map<int64_t, bool> supported_settable;
    for (size_t i = 0; i < description_count; i++) {
      supported_settable.emplace(static_cast<int64_t>(descriptions[i].metadataType.value),
                                 descriptions[i].isSettable);
    }
    return supported_settable;
  }();

  auto it = is_settable.find(static_cast<int64_t>(type));
  if (it != is_settable.end()) {
    return it->second;
  }

  ALOGW("%s: Couldn't find provided type %" PRId64 " in list!", __FUNCTION__,
        static_cast<int64_t>(type));
  return false;
}

AIMapper_Error LazyInit() {
  if (snap_helper_initialized_.load(std::memory_order_acquire)) {
    return AIMAPPER_ERROR_NONE;
  }

  std::lock_guard<std::mutex> lock(snap_helper_mutex_);
  if (snap_helper_initialized_.load(std::memory_order_relaxed)) {
    return AIMAPPER_ERROR_NONE;
  }

  auto *snap_helper = gralloc::GrallocSnapHelper::GetInstance();
  if (snap_helper == nullptr) {
    ALOGW("Unable to get snap helper");
    return AIMAPPER_ERROR_NO_RESOURCES;
  }

  snap_helper_ = snap_helper;
  snap_helper_initialized_.store(true, std::memory_order_release);
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

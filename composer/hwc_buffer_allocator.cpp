/*
 * Copyright (c) 2015-2021, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials provided
 *    with the distribution.
 *  * Neither the name of The Linux Foundation nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Changes from Qualcomm Innovation Center are provided under the following license:
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

/*
 * Changes from Qualcomm Innovation Center are provided under the following license:
 *
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <QtiGralloc.h>

#include <gralloctypes/Gralloc4.h>
#include <limits>
#include <utility>
#include <core/buffer_allocator.h>
#include <utils/constants.h>
#include <utils/debug.h>
#include <gr_utils.h>

#include "hwc_buffer_allocator.h"
#include "hwc_debugger.h"
#include "hwc_layers.h"
#include "gr_snap_helper.h"
#include "mapper_utils.h"

#include <aidl/android/hardware/graphics/allocator/BufferDescriptorInfo.h>
#include <android/rect.h>

#define __CLASS__ "HWCBufferAllocator"

using mapper::GetMapperInstance;
using mapper::GetMetadataState;
using mapper::GetStandardMetadata;
using mapper::GetVendorMetadata;
using mapper::IsSettable;

using android::hardware::hidl_handle;
using android::hardware::hidl_vec;
using APixelFormat = aidl::android::hardware::graphics::common::PixelFormat;
using ::aidl::android::hardware::graphics::allocator::BufferDescriptorInfo;
using aidl::android::hardware::graphics::common::Rect;
using aidl::android::hardware::graphics::common::StandardMetadataType;
using ABufferUsage = aidl::android::hardware::graphics::common::BufferUsage;

namespace sdm {

int HWCBufferAllocator::InitializeMapper() {
  if (mapper_initialized_.load(std::memory_order_acquire)) {
    return kErrorNone;
  }

  std::lock_guard<std::mutex> lock(mapper_init_mutex_);
  if (mapper_initialized_.load(std::memory_order_relaxed)) {
    return kErrorNone;
  }

  AIMapper *mapper = GetMapperInstance();
  if (mapper == nullptr) {
    DLOGE("Unable to get mapper");
    return kErrorCriticalResource;
  }

  mapper_ = mapper;
  mapper_initialized_.store(true, std::memory_order_release);
  return kErrorNone;
}

int HWCBufferAllocator::InitializeSnapHelper() {
  if (snap_helper_initialized_.load(std::memory_order_acquire)) {
    return kErrorNone;
  }

  std::lock_guard<std::mutex> lock(snap_helper_init_mutex_);
  if (snap_helper_initialized_.load(std::memory_order_relaxed)) {
    return kErrorNone;
  }

  auto *snap_helper = gralloc::GrallocSnapHelper::GetInstance();
  if (snap_helper == nullptr) {
    DLOGE("Unable to get snap helper");
    return kErrorCriticalResource;
  }

  snap_helper_ = snap_helper;
  snap_helper_initialized_.store(true, std::memory_order_release);
  return kErrorNone;
}

int HWCBufferAllocator::InitializeAllocator() {
  if (allocator_initialized_.load(std::memory_order_acquire)) {
    return kErrorNone;
  }

  std::lock_guard<std::mutex> lock(allocator_init_mutex_);
  if (allocator_initialized_.load(std::memory_order_relaxed)) {
    return kErrorNone;
  }

  auto allocator = IAllocator::fromBinder(ndk::SpAIBinder(
      AServiceManager_waitForService("android.hardware.graphics.allocator.IAllocator/default")));
  if (allocator == nullptr) {
    DLOGE("Unable to get allocator");
    return kErrorCriticalResource;
  }

  allocator_ = std::move(allocator);
  allocator_initialized_.store(true, std::memory_order_release);
  return kErrorNone;
}

int HWCBufferAllocator::GetGrallocInstance() {
  // Allocation needs both services. Metadata-only paths initialize the mapper
  // independently so a late allocator service cannot leave mapper_ null.
  int err = InitializeMapper();
  if (err != kErrorNone) {
    return err;
  }

  return InitializeAllocator();
}

static BufferDescriptorInfo CreateDescriptor(std::string name, uint32_t width, uint32_t height,
                                             int format, uint32_t layer_count, uint64_t usage) {
  BufferDescriptorInfo descriptorInfo{
      .width = static_cast<int32_t>(width),
      .height = static_cast<int32_t>(height),
      .layerCount = static_cast<int32_t>(layer_count),
      .format = static_cast<APixelFormat>(format),
      .usage = static_cast<ABufferUsage>(usage),
  };
  auto nameLength = std::min(name.length(), descriptorInfo.name.size() - 1);
  memcpy(descriptorInfo.name.data(), name.data(), nameLength);
  return descriptorInfo;
}

int HWCBufferAllocator::AllocateBuffer(BufferInfo *buffer_info) {
  if (!buffer_info) {
    return kErrorParameters;
  }

  auto err = GetGrallocInstance();
  if (err != 0) {
    return err;
  }
  const BufferConfig &buffer_config = buffer_info->buffer_config;
  AllocatedBufferInfo *alloc_buffer_info = &buffer_info->alloc_buffer_info;
  BufferPermission buf_perm[BUFFER_CLIENT_MAX];
  int format;
  uint64_t alloc_flags = 0;
  int error = SetBufferInfo(buffer_config.format, &format, &alloc_flags);
  if (error != 0) {
    return -EINVAL;
  }

  if (buffer_config.access_control.empty()) {
    if (buffer_config.secure) {
      alloc_flags |= static_cast<uint64_t>(ABufferUsage::PROTECTED);
    }

    if (buffer_config.secure_camera) {
      alloc_flags |= static_cast<uint64_t>(ABufferUsage::CAMERA_OUTPUT);
    }

    if (!buffer_config.cache) {
      // Allocate uncached buffers
      alloc_flags |= GRALLOC_USAGE_PRIVATE_UNCACHED;
    }

    if (buffer_config.trusted_ui) {
      // Allocate cached buffers for trusted UI
      alloc_flags |= GRALLOC_USAGE_PRIVATE_TRUSTED_VM;
      alloc_flags &= ~GRALLOC_USAGE_PRIVATE_UNCACHED;
    }

    if (buffer_config.gfx_client) {
      alloc_flags |= static_cast<uint64_t>(ABufferUsage::GPU_TEXTURE);
    }

    alloc_flags |= static_cast<uint64_t>(ABufferUsage::COMPOSER_OVERLAY);
  } else {
    for (uint32_t i = 0; i < BUFFER_CLIENT_MAX; i++) {
      buf_perm[i].permission = 0;
    }
    auto it = buffer_config.access_control.find(kBufferClientUnTrustedVM);
    if (it != buffer_config.access_control.end()) {
      if (!buffer_config.cache) {
        // Allocate uncached buffers
        alloc_flags |= GRALLOC_USAGE_PRIVATE_UNCACHED;
      }
      SetBufferAccessControlInfo(it->second, &buf_perm[kBufferClientUnTrustedVM]);
    } else {
      alloc_flags |= static_cast<uint64_t>(ABufferUsage::PROTECTED);
    }

    it = buffer_config.access_control.find(kBufferClientTrustedVM);
    if (it != buffer_config.access_control.end()) {
      alloc_flags |= GRALLOC_USAGE_PRIVATE_TRUSTED_VM;
      SetBufferAccessControlInfo(it->second, &buf_perm[kBufferClientTrustedVM]);
    }

    it = buffer_config.access_control.find(kBufferClientDPU);
    if (it != buffer_config.access_control.end()) {
      alloc_flags |= static_cast<uint64_t>(ABufferUsage::COMPOSER_OVERLAY);
      SetBufferAccessControlInfo(it->second, &buf_perm[kBufferClientDPU]);
    }
  }

  buffer_handle_t buf = nullptr;

  BufferDescriptorInfo descriptor_info = CreateDescriptor(
      std::string("HWC_Buffer"), buffer_config.width, buffer_config.height, format, 1, alloc_flags);

  AllocationResult result;
  auto status = allocator_->allocate2(descriptor_info, 1, &result);
  if (!status.isOk() || result.buffers.size() != 1) {
    DLOGE("Failed to allocate buffer");
    return kErrorMemory;
  }
  native_handle *raw_handle = android::makeFromAidl(result.buffers[0]);
  if (!raw_handle) {
    DLOGE("Failed to convert allocated buffer handle");
    return kErrorMemory;
  }

  auto mapper_err = STABLEMAPPER(mapper_).importBuffer(raw_handle, &buf);

  if (raw_handle) {
    // Locally created raw handle use is over here, so need to deallocate corresponding memory
    // to avoid memory leak.
    native_handle_delete(raw_handle);
    raw_handle = nullptr;
  }

  if (mapper_err != AIMAPPER_ERROR_NONE) {
    DLOGE("Failed to import buffer into HWC");
    return kErrorMemory;
  }

  uint32_t tmp_width;
  void *buf_handle = reinterpret_cast<void *>(const_cast<native_handle *>(buf));

  if (!buffer_config.access_control.empty()) {
    mapper_err = STABLEMAPPER(mapper_).setMetadata(
        buf, VENDOR_QTI_METADATA(SnapMetadataType::BUFFER_PERMISSION), buf_perm, sizeof(buf_perm));
    if (mapper_err != AIMAPPER_ERROR_NONE) {
      DLOGE("setMetadata failed for SnapMetadataType::BUFFER_PERMISSION %d", mapper_err);
      err = -EINVAL;
      goto cleanup;
    }
    auto error =
        GetMetadataValue(buf_handle, SnapMetadataType::MEM_HANDLE, &alloc_buffer_info->mem_handle,
                         sizeof(alloc_buffer_info->mem_handle));
    if (error) {
      err = -EINVAL;
      goto cleanup;
    }
  }

  err = GetFd(buf_handle, alloc_buffer_info->fd);
  if (err != kErrorNone)
    goto cleanup;

  err = GetWidth(buf_handle, tmp_width);
  if (err != kErrorNone)
    goto cleanup;
  alloc_buffer_info->stride = tmp_width;
  alloc_buffer_info->aligned_width = tmp_width;

  err = GetHeight(buf_handle, alloc_buffer_info->aligned_height);
  if (err != kErrorNone)
    goto cleanup;

  err = GetAllocationSize(buf_handle, alloc_buffer_info->size);
  if (err != kErrorNone)
    goto cleanup;

  err = GetBufferId(buf_handle, alloc_buffer_info->id);
  if (err != kErrorNone)
    goto cleanup;

  err = GetSDMFormat(buf_handle, alloc_buffer_info->format);
  if (err != kErrorNone)
    goto cleanup;

  buffer_info->private_data = buf_handle;
  return 0;

cleanup:
  if (buf) {
    STABLEMAPPER(mapper_).freeBuffer(buf);
  }
  return err;
}

int HWCBufferAllocator::FreeBuffer(BufferInfo *buffer_info) {
  if (!buffer_info || !buffer_info->private_data) {
    return kErrorParameters;
  }

  int err = InitializeMapper();
  if (err != kErrorNone) {
    return err;
  }

  auto hnd = reinterpret_cast<buffer_handle_t>(buffer_info->private_data);
  if (STABLEMAPPER(mapper_).freeBuffer(hnd) != AIMAPPER_ERROR_NONE) {
    return kErrorParameters;
  }

  AllocatedBufferInfo &alloc_buffer_info = buffer_info->alloc_buffer_info;

  alloc_buffer_info.fd = -1;
  alloc_buffer_info.stride = 0;
  alloc_buffer_info.size = 0;
  buffer_info->private_data = NULL;
  return kErrorNone;
}

int HWCBufferAllocator::GetHeight(void *buf, uint32_t &height) {
  height = 0;
  if (!buf) {
    return kErrorParameters;
  }
  int init_err = InitializeMapper();
  if (init_err != kErrorNone) {
    return init_err;
  }

  uint32_t tmp_height = 0;
  auto err = GetVendorMetadata(mapper_, static_cast<buffer_handle_t>(buf),
                               SnapMetadataType::ALIGNED_HEIGHT_IN_PIXELS, &tmp_height,
                               sizeof(tmp_height));
  if (err == AIMAPPER_ERROR_NONE) {
    height = tmp_height;
    return kErrorNone;
  }
  return kErrorParameters;
}

int HWCBufferAllocator::GetWidth(void *buf, uint32_t &width) {
  width = 0;
  if (!buf) {
    return kErrorParameters;
  }
  int err = InitializeMapper();
  if (err != kErrorNone) {
    return err;
  }

  auto result =
      GetStandardMetadata<StandardMetadataType::STRIDE>(mapper_, static_cast<buffer_handle_t>(buf));

  if (result.has_value() &&
      static_cast<uint64_t>(*result) <= std::numeric_limits<uint32_t>::max()) {
    width = static_cast<uint32_t>(*result);
    return kErrorNone;
  }
  return kErrorParameters;
}

int HWCBufferAllocator::GetUnalignedHeight(void *buf, uint32_t &height) {
  height = 0;
  if (!buf) {
    return kErrorParameters;
  }
  int err = InitializeMapper();
  if (err != kErrorNone) {
    return err;
  }

  auto result =
      GetStandardMetadata<StandardMetadataType::HEIGHT>(mapper_, static_cast<buffer_handle_t>(buf));

  if (result.has_value() &&
      static_cast<uint64_t>(*result) <= std::numeric_limits<uint32_t>::max()) {
    height = static_cast<uint32_t>(*result);
    return kErrorNone;
  }
  return kErrorParameters;
}

int HWCBufferAllocator::GetUnalignedWidth(void *buf, uint32_t &width) {
  width = 0;
  if (!buf) {
    return kErrorParameters;
  }
  int err = InitializeMapper();
  if (err != kErrorNone) {
    return err;
  }

  auto result =
      GetStandardMetadata<StandardMetadataType::WIDTH>(mapper_, static_cast<buffer_handle_t>(buf));

  if (result.has_value() &&
      static_cast<uint64_t>(*result) <= std::numeric_limits<uint32_t>::max()) {
    width = static_cast<uint32_t>(*result);
    return kErrorNone;
  }
  return kErrorParameters;
}

int HWCBufferAllocator::GetFd(void *buf, int &fd) {
  fd = -1;
  if (!buf) {
    return kErrorParameters;
  }
  int init_err = InitializeMapper();
  if (init_err != kErrorNone) {
    return init_err;
  }

  int tmp_fd = -1;
  auto err = GetVendorMetadata(mapper_, static_cast<buffer_handle_t>(buf), SnapMetadataType::FD,
                               &tmp_fd, sizeof(tmp_fd));
  if (err == AIMAPPER_ERROR_NONE) {
    fd = tmp_fd;
    return kErrorNone;
  }
  return kErrorParameters;
}

int HWCBufferAllocator::GetAllocationSize(void *buf, uint32_t &alloc_size) {
  alloc_size = 0;
  if (!buf) {
    return kErrorParameters;
  }
  int err = InitializeMapper();
  if (err != kErrorNone) {
    return err;
  }

  auto result = GetStandardMetadata<StandardMetadataType::ALLOCATION_SIZE>(
      mapper_, static_cast<buffer_handle_t>(buf));

  if (result.has_value() &&
      static_cast<uint64_t>(*result) <= std::numeric_limits<uint32_t>::max()) {
    alloc_size = static_cast<uint32_t>(*result);
    return kErrorNone;
  }
  return kErrorParameters;
}

int HWCBufferAllocator::GetBufferId(void *buf, uint64_t &id) {
  id = 0;
  if (!buf) {
    return kErrorParameters;
  }
  int init_err = InitializeMapper();
  if (init_err != kErrorNone) {
    return init_err;
  }

  auto result = GetStandardMetadata<StandardMetadataType::BUFFER_ID>(
      mapper_, static_cast<buffer_handle_t>(buf));

  if (result.has_value()) {
    id = static_cast<uint64_t>(*result);
    return kErrorNone;
  }
  return kErrorParameters;
}

int HWCBufferAllocator::GetFormat(void *buf, int32_t &format) {
  format = 0;
  if (!buf) {
    return kErrorParameters;
  }
  int init_err = InitializeMapper();
  if (init_err != kErrorNone) {
    return init_err;
  }

  int32_t ret_format = 0;
  int err =
      GetVendorMetadata(mapper_, static_cast<buffer_handle_t>(buf),
                        SnapMetadataType::PIXEL_FORMAT_ALLOCATED, &ret_format, sizeof(ret_format));

  if (err == AIMAPPER_ERROR_NONE) {
    format = ret_format;
    return kErrorNone;
  }
  return kErrorParameters;
}

int HWCBufferAllocator::GetPrivateFlags(void *buf, int32_t &flags) {
  flags = 0;
  if (!buf) {
    return kErrorParameters;
  }
  int init_err = InitializeMapper();
  if (init_err != kErrorNone) {
    return init_err;
  }

  int64_t is_ubwc = 0, is_tile_rendered = 0, is_cached = 0;
  uint64_t buffer_usage = 0;
  if (GetVendorMetadata(mapper_, static_cast<buffer_handle_t>(buf), SnapMetadataType::IS_UBWC,
                        &is_ubwc, sizeof(is_ubwc)) == AIMAPPER_ERROR_NONE &&
      GetVendorMetadata(mapper_, static_cast<buffer_handle_t>(buf),
                        SnapMetadataType::IS_TILE_RENDERED, &is_tile_rendered,
                        sizeof(is_tile_rendered)) == AIMAPPER_ERROR_NONE &&
      GetVendorMetadata(mapper_, static_cast<buffer_handle_t>(buf), SnapMetadataType::IS_CACHED,
                        &is_cached, sizeof(is_cached)) == AIMAPPER_ERROR_NONE &&
      GetVendorMetadata(mapper_, static_cast<buffer_handle_t>(buf), SnapMetadataType::USAGE,
                        &buffer_usage, sizeof(buffer_usage)) == AIMAPPER_ERROR_NONE) {
    // Private flags are being set here until pending changes to use snapalloc in SDM directly
    flags = is_ubwc ? (flags | qtigralloc::PRIV_FLAGS_UBWC_ALIGNED) : flags;
    flags = is_tile_rendered ? (flags | qtigralloc::PRIV_FLAGS_TILE_RENDERED) : flags;
    flags = is_cached ? (flags | qtigralloc::PRIV_FLAGS_CACHED) : flags;

    bool secure = buffer_usage & vendor_qti_hardware_display_common_BufferUsage::PROTECTED;
    flags = (secure) ? (flags | qtigralloc::PRIV_FLAGS_SECURE_BUFFER) : flags;
    flags =
        (secure && (buffer_usage & vendor_qti_hardware_display_common_BufferUsage::CAMERA_OUTPUT))
            ? (flags | qtigralloc::PRIV_FLAGS_CAMERA_WRITE)
            : flags;
    flags =
        (buffer_usage & vendor_qti_hardware_display_common_BufferUsage::QTI_PRIVATE_SECURE_DISPLAY)
            ? (flags | qtigralloc::PRIV_FLAGS_SECURE_DISPLAY)
            : flags;

    return kErrorNone;
  }
  return kErrorParameters;
}

int HWCBufferAllocator::GetSDMFormat(void *buf, LayerBufferFormat &sdm_format) {
  sdm_format = kFormatInvalid;
  int32_t tmp_format = 0, tmp_flags = 0, err;
  err = GetFormat(buf, tmp_format);
  if (err != kErrorNone)
    return kErrorUndefined;

  err = GetPrivateFlags(buf, tmp_flags);
  if (err != kErrorNone)
    return kErrorUndefined;

  sdm_format = HWCLayer::GetSDMFormat(tmp_format, tmp_flags);
  return kErrorNone;
}

int HWCBufferAllocator::GetBufferType(void *buf, uint32_t &buffer_type) {
  buffer_type = 0;
  if (!buf) {
    return kErrorParameters;
  }
  int init_err = InitializeMapper();
  if (init_err != kErrorNone) {
    return init_err;
  }

  uint32_t tmp_buffer_type = 0;
  auto err =
      GetVendorMetadata(mapper_, static_cast<buffer_handle_t>(buf), SnapMetadataType::BUFFER_TYPE,
                        &tmp_buffer_type, sizeof(tmp_buffer_type));
  if (err == AIMAPPER_ERROR_NONE) {
    buffer_type = tmp_buffer_type;
    return kErrorNone;
  }
  return kErrorParameters;
}

int HWCBufferAllocator::GetBufferGeometry(void *buf, int32_t &slice_width, int32_t &slice_height) {
  slice_width = 0;
  slice_height = 0;
  if (!buf) {
    return kErrorParameters;
  }
  int init_err = InitializeMapper();
  if (init_err != kErrorNone) {
    return init_err;
  }

  auto result =
      GetStandardMetadata<StandardMetadataType::CROP>(mapper_, static_cast<buffer_handle_t>(buf));

  if (result.has_value() && !result->empty() && result.value()[0].right > 0 &&
      result.value()[0].bottom > 0) {
    slice_width = result.value()[0].right;
    slice_height = result.value()[0].bottom;
    return kErrorNone;
  }
  return kErrorParameters;
}

int HWCBufferAllocator::GetCustomWidthAndHeight(const native_handle_t *handle, int *width,
                                                int *height) {
  if (!handle || !width || !height) {
    return -EINVAL;
  }

  *width = 0;
  *height = 0;
  auto err = InitializeMapper();
  if (err != kErrorNone) {
    return err;
  }
  err = InitializeSnapHelper();
  if (err != kErrorNone) {
    return err;
  }

  void *hnd = const_cast<native_handle_t *>(handle);
  if (GetMetadataValue(hnd, SnapMetadataType::STRIDE, width, sizeof(*width)) != 0 ||
      GetMetadataValue(hnd, SnapMetadataType::ALIGNED_HEIGHT_IN_PIXELS, height, sizeof(*height)) !=
          0) {
    return -EINVAL;
  }

  int ret = 0;
  if (snap_helper_->IsSnapAllocEnabled()) {
    int custom_width = 0;
    int custom_height = 0;
    if (GetMetadataValue(hnd, SnapMetadataType::CUSTOM_DIMENSIONS_STRIDE, &custom_width,
                         sizeof(custom_width)) == 0 &&
        GetMetadataValue(hnd, SnapMetadataType::CUSTOM_DIMENSIONS_HEIGHT, &custom_height,
                         sizeof(custom_height)) == 0) {
      *width = custom_width;
      *height = custom_height;
    }
  } else {
    auto private_handle = static_cast<private_handle_t *>(hnd);
    if (private_handle_t::validate(private_handle) != 0) {
      return -EINVAL;
    }
    ret = gralloc::GetCustomDimensions(private_handle, width, height);
  }

  if (ret || *width <= 0 || *height <= 0) {
    ALOGW(
        "%s: Error obtaining custom dimensions. "
        "stride: %d, height: %d",
        __FUNCTION__, *width, *height);
    return -EINVAL;
  }

  return kErrorNone;
}

int HWCBufferAllocator::GetAlignedWidthAndHeight(int width, int height, int format,
                                                 uint32_t alloc_type, int *aligned_width,
                                                 int *aligned_height) {
  uint64_t usage = 0;
  unsigned int alignedw, alignedh;
  if (alloc_type & GRALLOC_USAGE_HW_FB) {
    usage |= static_cast<uint64_t>(ABufferUsage::COMPOSER_CLIENT_TARGET);
  }
  if (alloc_type & GRALLOC_USAGE_PRIVATE_ALLOC_UBWC) {
    usage |= GRALLOC_USAGE_PRIVATE_ALLOC_UBWC;
  }
  *aligned_width = width;
  *aligned_height = height;

  int err;
  err = InitializeSnapHelper();
  if (err != 0) {
    DLOGE("Failed to retrieve snap helper");
    return err;
  }
  if (snap_helper_->IsSnapAllocEnabled()) {
    uint64_t alignedw_ul = 0;
    uint64_t alignedh_ul = 0;
    gralloc::BufferDescriptor desc;
    desc.SetUsage(usage);
    desc.SetColorFormat(format);
    desc.SetDimensions(width, height);

    err = mapper::GetFromBufferDescriptor(mapper::ConvertGrallocToAidlDescriptor(desc),
                                          SnapMetadataType::ALIGNED_WIDTH_IN_PIXELS, &alignedw_ul,
                                          false);  // false -> convert_to_hidl_bytestream
    if (err == AIMAPPER_ERROR_NONE) {
      err = mapper::GetFromBufferDescriptor(mapper::ConvertGrallocToAidlDescriptor(desc),
                                            SnapMetadataType::ALIGNED_HEIGHT_IN_PIXELS,
                                            &alignedh_ul, false);
      alignedw = static_cast<unsigned int>(alignedw_ul);
      alignedh = static_cast<unsigned int>(alignedh_ul);
    }
  } else {
    gralloc::BufferInfo info(width, height, format, usage);
    err = gralloc::GetAlignedWidthAndHeight(info, &alignedw, &alignedh);
  }
  if (err) {
    return -EINVAL;
  } else {
    *aligned_width = static_cast<int>(alignedw);
    *aligned_height = static_cast<int>(alignedh);
  }

  return err;
}

uint32_t HWCBufferAllocator::GetBufferSize(BufferInfo *buffer_info) {
  int err;
  err = InitializeSnapHelper();
  if (err != 0) {
    DLOGE("Failed to retrieve snap helper");
    return 0;
  }

  const BufferConfig &buffer_config = buffer_info->buffer_config;
  uint64_t alloc_flags = GRALLOC_USAGE_PRIVATE_IOMMU_HEAP;

  int width = INT(buffer_config.width);
  int height = INT(buffer_config.height);
  int format;
  uint32_t buffer_size;

  if (buffer_config.secure) {
    alloc_flags |= INT(GRALLOC_USAGE_PROTECTED);
  }

  if (!buffer_config.cache) {
    // Allocate uncached buffers
    alloc_flags |= GRALLOC_USAGE_PRIVATE_UNCACHED;
  }

  if (SetBufferInfo(buffer_config.format, &format, &alloc_flags) < 0) {
    return 0;
  }

  if (snap_helper_->IsSnapAllocEnabled()) {
    gralloc::BufferDescriptor buf_desc;
    buf_desc.SetColorFormat(format);
    buf_desc.SetDimensions(width, height);
    buf_desc.SetUsage(alloc_flags);
    if (!mapper::GetFromBufferDescriptor(mapper::ConvertGrallocToAidlDescriptor(buf_desc),
                                         SnapMetadataType::ALLOCATION_SIZE,
                                         static_cast<void *>(&buffer_size), false)) {
      return buffer_size;
    }
  } else {
    uint32_t aligned_width = 0, aligned_height = 0;
    gralloc::BufferInfo info(static_cast<int>(width), static_cast<int>(height),
                             static_cast<int>(static_cast<APixelFormat>(format)), alloc_flags);
    if (gralloc::GetBufferSizeAndDimensions(info, &buffer_size, &aligned_width, &aligned_height) ==
        0) {
      return buffer_size;
    }
  }

  return 0;
}

int HWCBufferAllocator::SetBufferInfo(LayerBufferFormat format, int *target, uint64_t *flags) {
  switch (format) {
    case kFormatRGBA8888:
      *target = static_cast<int>(APixelFormat::RGBA_8888);
      break;
    case kFormatRGBX8888:
      *target = static_cast<int>(APixelFormat::RGBX_8888);
      break;
    case kFormatRGB888:
      *target = static_cast<int>(APixelFormat::RGB_888);
      break;
    case kFormatRGB565:
      *target = static_cast<int>(APixelFormat::RGB_565);
      break;
    case kFormatBGR565:
      *target = HAL_PIXEL_FORMAT_BGR_565;
      break;
    case kFormatBGR888:
      *target = HAL_PIXEL_FORMAT_BGR_888;
      break;
    case kFormatBGRA8888:
    case kFormatARGB8888:
      *target = static_cast<int>(APixelFormat::BGRA_8888);
      break;
    case kFormatYCrCb420PlanarStride16:
      *target = static_cast<int>(APixelFormat::YV12);
      break;
    case kFormatYCrCb420SemiPlanar:
      *target = static_cast<int>(APixelFormat::YCRCB_420_SP);
      break;
    case kFormatYCbCr420SemiPlanar:
      *target = HAL_PIXEL_FORMAT_YCbCr_420_SP;
      break;
    case kFormatYCbCr422H2V1Packed:
      *target = HAL_PIXEL_FORMAT_YCbCr_422_I;
      break;
    case kFormatCbYCrY422H2V1Packed:
      *target = HAL_PIXEL_FORMAT_CbYCrY_422_I;
      break;
    case kFormatYCbCr422H2V1SemiPlanar:
      *target = static_cast<int>(APixelFormat::YCBCR_422_SP);
      break;
    case kFormatYCbCr420SemiPlanarVenus:
      *target = HAL_PIXEL_FORMAT_YCbCr_420_SP_VENUS;
      break;
    case kFormatYCrCb420SemiPlanarVenus:
      *target = HAL_PIXEL_FORMAT_YCrCb_420_SP_VENUS;
      break;
    case kFormatYCbCr420SPVenusUbwc:
    case kFormatYCbCr420SPVenusTile:
      *target = HAL_PIXEL_FORMAT_YCbCr_420_SP_VENUS_UBWC;
      *flags |= GRALLOC_USAGE_PRIVATE_ALLOC_UBWC;
      break;
    case kFormatRGBA5551:
      *target = HAL_PIXEL_FORMAT_RGBA_5551;
      break;
    case kFormatRGBA4444:
      *target = HAL_PIXEL_FORMAT_RGBA_4444;
      break;
    case kFormatRGBA1010102:
      *target = static_cast<int>(APixelFormat::RGBA_1010102);
      break;
    case kFormatARGB2101010:
      *target = HAL_PIXEL_FORMAT_ARGB_2101010;
      break;
    case kFormatRGBX1010102:
      *target = HAL_PIXEL_FORMAT_RGBX_1010102;
      break;
    case kFormatXRGB2101010:
      *target = HAL_PIXEL_FORMAT_XRGB_2101010;
      break;
    case kFormatBGRA1010102:
      *target = HAL_PIXEL_FORMAT_BGRA_1010102;
      break;
    case kFormatABGR2101010:
      *target = HAL_PIXEL_FORMAT_ABGR_2101010;
      break;
    case kFormatBGRX1010102:
      *target = HAL_PIXEL_FORMAT_BGRX_1010102;
      break;
    case kFormatXBGR2101010:
      *target = HAL_PIXEL_FORMAT_XBGR_2101010;
      break;
    case kFormatYCbCr420P010:
      *target = HAL_PIXEL_FORMAT_YCbCr_420_P010;
      break;
    case kFormatYCbCr420TP10Ubwc:
    case kFormatYCbCr420TP10Tile:
      *target = HAL_PIXEL_FORMAT_YCbCr_420_TP10_UBWC;
      *flags |= GRALLOC_USAGE_PRIVATE_ALLOC_UBWC;
      break;
    case kFormatYCbCr420P010Ubwc:
    case kFormatYCbCr420P010Tile:
      *target = HAL_PIXEL_FORMAT_YCbCr_420_P010_UBWC;
      *flags |= GRALLOC_USAGE_PRIVATE_ALLOC_UBWC;
      break;
    case kFormatYCbCr420P010Venus:
      *target = HAL_PIXEL_FORMAT_YCbCr_420_P010_VENUS;
      break;
    case kFormatRGBA8888Ubwc:
      *target = static_cast<int>(APixelFormat::RGBA_8888);
      *flags |= GRALLOC_USAGE_PRIVATE_ALLOC_UBWC;
      break;
    case kFormatRGBX8888Ubwc:
      *target = static_cast<int>(APixelFormat::RGBX_8888);
      *flags |= GRALLOC_USAGE_PRIVATE_ALLOC_UBWC;
      break;
    case kFormatBGR565Ubwc:
      *target = HAL_PIXEL_FORMAT_BGR_565;
      *flags |= GRALLOC_USAGE_PRIVATE_ALLOC_UBWC;
      break;
    case kFormatRGBA1010102Ubwc:
      *target = static_cast<int>(APixelFormat::RGBA_1010102);
      *flags |= GRALLOC_USAGE_PRIVATE_ALLOC_UBWC;
      break;
    case kFormatRGBX1010102Ubwc:
      *target = HAL_PIXEL_FORMAT_RGBX_1010102;
      *flags |= GRALLOC_USAGE_PRIVATE_ALLOC_UBWC;
      break;
    case kFormatBlob:
      *target = static_cast<int>(APixelFormat::BLOB);
      break;
    case kFormatRGBA16161616F:
      *target = HAL_PIXEL_FORMAT_RGBA_FP16;
      break;
    case kFormatRGBA16161616FUbwc:
      *target = HAL_PIXEL_FORMAT_RGBA_FP16;
      *flags |= GRALLOC_USAGE_PRIVATE_ALLOC_UBWC;
      break;
    default:
      DLOGW("Unsupported format = 0x%x", format);
      return -EINVAL;
  }
  return 0;
}

int HWCBufferAllocator::GetAllocatedBufferInfo(const BufferConfig &buffer_config,
                                               AllocatedBufferInfo *allocated_buffer_info) {
  BufferInfo buffer_info = {buffer_config, *allocated_buffer_info};
  // TODO(user): This API should pass the buffer_info of the already allocated buffer
  // The private_data can then be typecast to the private_handle and used directly.
  uint64_t alloc_flags = GRALLOC_USAGE_PRIVATE_IOMMU_HEAP;

  int width = INT(buffer_config.width);
  int height = INT(buffer_config.height);
  int format;

  if (buffer_config.secure) {
    alloc_flags |= INT(GRALLOC_USAGE_PROTECTED);
  }

  if (!buffer_config.cache) {
    // Allocate uncached buffers
    alloc_flags |= GRALLOC_USAGE_PRIVATE_UNCACHED;
  }

  if (SetBufferInfo(buffer_config.format, &format, &alloc_flags) < 0) {
    return -EINVAL;
  }

  uint32_t buffer_size = 0;
  int aligned_width = 0, aligned_height = 0;
  int ret =
      GetAlignedWidthAndHeight(width, height, format, alloc_flags, &aligned_width, &aligned_height);
  buffer_size = GetBufferSize(&buffer_info);
  if (ret < 0 || buffer_size == 0) {
    return -EINVAL;
  }
  allocated_buffer_info->stride = UINT32(aligned_width);
  allocated_buffer_info->aligned_width = UINT32(aligned_width);
  allocated_buffer_info->aligned_height = UINT32(aligned_height);
  allocated_buffer_info->size = UINT32(buffer_size);

  return 0;
}

int HWCBufferAllocator::GetBufferLayout(const AllocatedBufferInfo &buf_info, uint32_t stride[4],
                                        uint32_t offset[4], uint32_t *num_planes) {
  int err;
  err = InitializeSnapHelper();
  if (err != 0) {
    DLOGE("Failed to retrieve snap helper");
    return err;
  }

  int format = static_cast<int>(APixelFormat::RGBA_8888);
  int plane_count = 0;
  uint64_t flags = 0;
  uint32_t size = 0;
  gralloc::PlaneLayoutInfo *plane_layout_info_ptr;
  SetBufferInfo(buf_info.format, &format, &flags);
  // Setup only the required stuff, skip rest
  if (flags & GRALLOC_USAGE_PRIVATE_ALLOC_UBWC) {
    flags = qtigralloc::PRIV_FLAGS_UBWC_ALIGNED;
  }

  DLOGV("%s: Input parameters - wxh: %dx%d usage: 0x%" PRIu64 " format: %d", __FUNCTION__,
        buf_info.aligned_width, buf_info.aligned_height, buf_info.usage, format);

  gralloc::BufferInfo info(buf_info.aligned_width, buf_info.aligned_height, format, buf_info.usage);
  if (snap_helper_->IsSnapAllocEnabled()) {
    // TODO: reduce code duplication here
    BufferDescriptorInfo info_aidl{
        .width = static_cast<int32_t>(buf_info.aligned_width),
        .height = static_cast<int32_t>(buf_info.aligned_height),
        .layerCount = 1,
        .format = static_cast<GrallocPixelFormat>(format),
        .usage = static_cast<GrallocBufferUsage>(buf_info.usage),
    };
    SnapBufferLayout buffer_layout = {};
    auto &plane_layout_info = buffer_layout.planes;
    if (!mapper::GetFromBufferDescriptor(info_aidl, SnapMetadataType::PLANE_LAYOUTS, &buffer_layout,
                                         false)) {
      int type;
      plane_count = buffer_layout.plane_count;
      DLOGV("%s: Number of planes - %d gralloc format %d", __FUNCTION__, plane_count, format);
      for (int i = 0; i < plane_count; i++) {
        type = 0;
        for (int j = 0; j < plane_layout_info[i].component_count; j++) {
          type |= static_cast<int>(plane_layout_info[i].components[j].type);
        }
        DLOGV("%s: plane info: component - %d", __FUNCTION__, type);
        DLOGV("h_subsampling - %u, v_subsampling - %u, offset - %u, pixel_increment/step - %d",
              plane_layout_info[i].horizontal_subsampling,
              plane_layout_info[i].vertical_subsampling, plane_layout_info[i].offset_in_bytes,
              (plane_layout_info[i].sample_increment_bits / 8));
        DLOGV("stride_pixel - %d, stride_bytes - %d, scanlines - %d, size - %u",
              static_cast<unsigned int>(
                  static_cast<float>(plane_layout_info[i].horizontal_stride_in_bytes) /
                  (static_cast<float>(plane_layout_info[i].sample_increment_bits) / 8.0f)),
              plane_layout_info[i].horizontal_stride_in_bytes, plane_layout_info[i].scanlines,
              plane_layout_info[i].size_in_bytes);
      }
      // We are only returning buffer layout for progressive or single field formats.
      *num_planes = (plane_count > 3) ? 2 : plane_count;

      for (int i = 0; i < *num_planes; i++) {
        offset[i] = static_cast<uint32_t>(plane_layout_info[i].offset_in_bytes);
        stride[i] = static_cast<uint32_t>(plane_layout_info[i].horizontal_stride_in_bytes);
      }
    } else {
      return -EINVAL;
    }
  } else {
    unsigned int alignedw = 0, alignedh = 0;
    int custom_format = gralloc::GetImplDefinedFormat(buf_info.usage, format);
    err = gralloc::GetBufferSizeAndDimensions(info, &size, &alignedw, &alignedh);
    if (err) {
      return -EINVAL;
    }

    gralloc::PlaneLayoutInfo plane_layout_info[8] = {};
    DLOGV("%s: Aligned width and height - wxh: %ux%u custom_format = %d", __FUNCTION__, alignedw,
          alignedh, custom_format);
    if (gralloc::IsYuvFormat(custom_format)) {
      // flags here only refers to layout (interlaced) flags, not private or buffer usage flags
      gralloc::GetYUVPlaneInfo(info, custom_format, alignedw, alignedh, flags, &plane_count,
                               plane_layout_info);
    } else if (gralloc::IsUncompressedRGBFormat(custom_format) ||
               gralloc::IsCompressedRGBFormat(custom_format)) {
      gralloc::GetRGBPlaneInfo(info, custom_format, alignedw, alignedh, flags, &plane_count,
                               plane_layout_info);
    } else {
      return -EINVAL;
    }
    // We are only returning buffer layout for progressive or single field formats.
    *num_planes = (plane_count > 3) ? 2 : plane_count;

    for (int i = 0; i < *num_planes; i++) {
      offset[i] = static_cast<uint32_t>(plane_layout_info[i].offset);
      stride[i] = static_cast<uint32_t>(plane_layout_info[i].stride_bytes);
    }
    DLOGV("%s: Number of plane - %d, custom_format - %d", __FUNCTION__, plane_count, custom_format);
  }

  if (flags & qtigralloc::PRIV_FLAGS_UBWC_ALIGNED) {
    std::fill(offset, offset + 4, 0);
  }

  return kErrorNone;
}

int HWCBufferAllocator::MapBuffer(const native_handle_t *handle, shared_ptr<Fence> acquire_fence,
                                  void **base_ptr) {
  if (!handle || !base_ptr) {
    return kErrorParameters;
  }

  *base_ptr = nullptr;
  auto err = InitializeMapper();
  if (err != 0) {
    DLOGW("Could not get mapper instance");
    return err;
  }

  Fence::ScopedRef scoped_ref;
  NATIVE_HANDLE_DECLARE_STORAGE(acquire_fence_storage, 1, 0);
  int acquire_fence_fd = -1;
  if (acquire_fence) {
    acquire_fence_fd = scoped_ref.Get(acquire_fence);
  }

  auto hnd = static_cast<buffer_handle_t>(const_cast<native_handle_t *>(handle));
  ARect access_region = {.left = 0, .top = 0, .right = 0, .bottom = 0};
  auto error = STABLEMAPPER(mapper_).lock(hnd, (uint64_t)ABufferUsage::CPU_READ_OFTEN,
                                          access_region, acquire_fence_fd, base_ptr);

  if (!*base_ptr || error != AIMAPPER_ERROR_NONE) {
    DLOGW("*base_ptr is NULL! lock(%p, ...) failed: %d", hnd, error);
    return kErrorUndefined;
  }

  return kErrorNone;
}

int HWCBufferAllocator::UnmapBuffer(const native_handle_t *handle, int *release_fence) {
  if (!handle || !release_fence) {
    return kErrorParameters;
  }

  int err = kErrorNone;
  *release_fence = -1;
  err = InitializeMapper();
  if (err != kErrorNone) {
    return err;
  }

  auto hnd = static_cast<buffer_handle_t>(const_cast<native_handle_t *>(handle));
  auto error = STABLEMAPPER(mapper_).unlock(hnd, release_fence);

  if (error != AIMAPPER_ERROR_NONE) {
    ALOGW("unlock failed with error %d", error);
    err = -EINVAL;
  }

  return err;
}

void HWCBufferAllocator::SetBufferAccessControlInfo(std::bitset<kBufferPermMax> permission,
                                                    BufferPermission *buf_perm) {
  buf_perm->read = permission.test(kBufferPermRead);
  buf_perm->write = permission.test(kBufferPermWrite);
  buf_perm->execute = permission.test(kBufferPermExecute);
}

int HWCBufferAllocator::GetCustomContentMetadata(void *buf, CustomContentMetadata *dest) {
  if (!buf || !dest) {
    return -EINVAL;
  }

  *dest = {};
  int err = InitializeMapper();
  if (err != kErrorNone) {
    DLOGE("Failed to retrieve mapper instance");
    return err;
  }

  auto error = GetVendorMetadata(mapper_, static_cast<buffer_handle_t>(buf),
                                 SnapMetadataType::CUSTOM_CONTENT_METADATA,
                                 static_cast<void *>(dest), sizeof(*dest));
  return error == AIMAPPER_ERROR_NONE ? 0 : -ENOTSUP;
}

int HWCBufferAllocator::GetMetadataValue(void *buf, SnapMetadataType type, void *dest,
                                         size_t dest_size) {
  if (!buf || !dest || dest_size == 0) {
    return -EINVAL;
  }

  int err = InitializeMapper();
  if (err != kErrorNone) {
    DLOGE("Failed to retrieve mapper instance");
    return err;
  }

  if (IsSettable(mapper_, type) && mapper::IsMetadataStateSupported()) {
    bool metadata_set = false;
    const AIMapper_Error state_error =
        GetMetadataState(static_cast<buffer_handle_t>(buf), type, &metadata_set);
    if (state_error != AIMAPPER_ERROR_NONE || !metadata_set) {
      return -ENOTSUP;
    }
  }

  const AIMapper_Error error =
      GetVendorMetadata(mapper_, static_cast<buffer_handle_t>(buf), type, dest, dest_size);
  return error == AIMAPPER_ERROR_NONE ? 0 : -ENOTSUP;
}

int HWCBufferAllocator::ImportBufferHandle(native_handle_t **handle, bool is_aidl_duped) {
  if (!handle || !(*handle)) {
    return -EINVAL;
  }

  int err = InitializeMapper();
  if (err != kErrorNone) {
    return err;
  }

  buffer_handle_t buf = nullptr;
  auto mapper_err = STABLEMAPPER(mapper_).importBuffer(*handle, &buf);

  if (mapper_err != AIMAPPER_ERROR_NONE) {
    DLOGE("Failed to import buffer into HWC");
    return kErrorMemory;
  }

  if (is_aidl_duped) {
    native_handle_close(*handle);
  }

  native_handle_delete(*handle);
  *handle = const_cast<native_handle *>(buf);

  return 0;
}

void HWCBufferAllocator::ReleaseBufferHandle(const native_handle_t *handle) {
  if (!handle) {
    return;
  }

  if (InitializeMapper() != kErrorNone) {
    DLOGE("Failed to retrieve mapper instance");
    return;
  }

  STABLEMAPPER(mapper_).freeBuffer(const_cast<native_handle_t *>(handle));
}

}  // namespace sdm

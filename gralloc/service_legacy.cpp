/*
 * Copyright (c) 2018-2021 The Linux Foundation. All rights reserved.
 * Copyright (C) 2026 The KleeUI Project
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <hidl/LegacySupport.h>
#include <log/log.h>

#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <sched.h>

#include "QtiAllocator4.h"

using ::android::hardware::configureRpcThreadpool;
using ::android::hardware::joinRpcThreadpool;
using IQtiAllocator4 =
    ::vendor::qti::hardware::display::allocator::V4_0::IQtiAllocator;
using QtiAllocator4 =
    ::vendor::qti::hardware::display::allocator::V4_0::implementation::QtiAllocator4;

int main(int, char **) {
  sched_param parameter = {};
  parameter.sched_priority = 2;
  if (sched_setscheduler(0, SCHED_FIFO | SCHED_RESET_ON_FORK, &parameter) != 0) {
    ALOGW("Unable to set allocator scheduling policy: %s", strerror(errno));
  }

  configureRpcThreadpool(4, true);
  ::android::sp<IQtiAllocator4> allocator = new QtiAllocator4();
  if (allocator->registerAsService() != ::android::OK) {
    ALOGE("Unable to register the QTI allocator 4 service");
    return EXIT_FAILURE;
  }

  ALOGI("Registered QTI allocator 4 compatibility service");
  joinRpcThreadpool();
  return EXIT_FAILURE;
}

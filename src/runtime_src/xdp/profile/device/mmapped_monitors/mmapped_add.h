/*
 * Copyright (C) 2021 Xilinx Inc - All rights reserved
 * Xilinx Debug & Profile (XDP) APIs
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You may
 * not use this file except in compliance with the License. A copy of the
 * License is located at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

#ifndef XDP_PROFILE_DEVICE_MMAPPED_ADD_H
#define XDP_PROFILE_DEVICE_MMAPPED_ADD_H

#include "xdp/profile/device/add.h"

namespace xdp {

/** Specialization for DeadlockDetector Profile IP with support for open+mmap on device driver based DeadlockDetector subdevice
 */

class MMappedDeadlockDetector : public DeadlockDetector
{
public:
  MMappedDeadlockDetector(Device*  handle  /* XDP Device Abstraction handle : xrt or HAL device handle */
                      ,uint64_t index   /* Index of the IP in Debug_IP_Layout */
                      ,debug_ip_data* data = nullptr);

  virtual ~MMappedDeadlockDetector();

  virtual int read(uint64_t offset, size_t size, void* data);
  virtual int write(uint64_t offset, size_t size, void* data);

  virtual bool isMMapped();

protected:
  int      driver_FD     = -1;
  char*    mapped_device =  nullptr;
};

}

#endif

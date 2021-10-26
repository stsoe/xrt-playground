/**
 * Copyright (C) 2016-2020 Xilinx, Inc
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

#include "appdebugmanager.h"
#include "appdebug.h"

namespace appdebug {

  static bool active ;

  AppDebugManager::AppDebugManager()
  {
    Platform = xocl::get_shared_platform();

    register_xocl_appdebug_callbacks() ;
    active = true ;
  }

  AppDebugManager::~AppDebugManager()
  {
    active = false ;
  }

  bool AppDebugManager::isActive()
  {
    return active ;
  }

} // end namespace appdebug

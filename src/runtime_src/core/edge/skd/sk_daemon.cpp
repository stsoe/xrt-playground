/**
 * Copyright (C) 2019-2021 Xilinx, Inc
 * Author(s): Min Ma	<min.ma@xilinx.com>
 *          : Larry Liu	<yliu@xilinx.com>
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

#include <dlfcn.h>
#include <execinfo.h>
#include <string.h>
#include <cstdarg>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/prctl.h>

#include "sk_types.h"
#include "sk_daemon.h"
#include "core/common/config_reader.h"
#include "core/common/message.h"
#include "core/common/pskernel_parse.h"
#include "core/edge/user/shim.h"

xclDeviceHandle devHdl;

static unsigned int getHostBO(unsigned long paddr, size_t size)
{
  unsigned int boHandle;

  /* Call XRT library */
  boHandle = xclGetHostBO(devHdl, paddr, size);
  return boHandle;
}

static void *mapBO(unsigned int boHandle, bool write)
{
  void *buf;
  buf = xclMapBO(devHdl, boHandle, write);
  return buf;
}

static void freeBO(unsigned int boHandle)
{
  xclFreeBO(devHdl, boHandle);
}

static int logMsg(xrtLogMsgLevel level, const char* tag,
		       const char* format, ...)
{
  static auto verbosity = xrt_core::config::get_verbosity();
  if (level > verbosity) {
    return 0;
  }
  va_list args;
  va_start(args, format);
  int ret = -1;
  xrt_core::message::sendv(static_cast<xrt_core::message::severity_level>(level), tag, format, args);
  va_end(args);

  return 0;
}

static int getBufferFd(unsigned int boHandle)
{
    return xclExportBO(devHdl, boHandle);
}

/*
 * This function calls XRT interface to create a soft kernel compute
 * unit. Before create soft kernel CU, we allocate a BO to hold the
 * reg file for that CU.
 */
static int createSoftKernel(unsigned int *boh, uint32_t cu_idx)
{
  int ret;

  *boh = xclAllocBO(devHdl, SOFT_KERNEL_REG_SIZE, 0, 0);
  if (*boh == 0xFFFFFFFF) {
    syslog(LOG_ERR, "Cannot alloc bo for soft kernel.\n");
    return -1;
  }

  ret = xclSKCreate(devHdl, *boh, cu_idx);

  return ret;
}

/* This function release the resources allocated for soft kernel. */
static int destroySoftKernel(unsigned int boh, void *mapAddr)
{
  int ret;

  ret = munmap(mapAddr, SOFT_KERNEL_REG_SIZE);
  if (ret) {
    syslog(LOG_ERR, "Cannot munmap BO %d, at %p\n", boh, mapAddr);
    return ret;
  }
  xclFreeBO(devHdl, boh);

  return 0;
}

/*
 * This function calls XRT interface to notify a soft kenel is idle
 * and wait for next command.
 */
static int waitNextCmd(uint32_t cu_idx)
{
  return xclSKReport(devHdl, cu_idx, XRT_SCU_STATE_DONE);
}

/*
 * The arguments for soft kernel CU to run is copied into its reg
 * file. By mapping the reg file BO, we get the process's memory
 * address for the soft kernel argemnts.
 *
 * Note: since we are reusing reg file BO to store soft kernel
 * return value, we need to map the BO as writable.
 */
static void *getKernelArg(unsigned int boHdl, uint32_t cu_idx)
{
  return xclMapBO(devHdl, boHdl, true);
}

xclDeviceHandle initXRTHandle(unsigned deviceIndex)
{
  return xclOpen(deviceIndex, NULL, XCL_QUIET);
}

/*
 * This is the main loop for a soft kernel CU.
 * name   : soft kernel function name to run it.
 * path   : the full path that soft kernel locates in file system.
 * cu_idx : the Compute Index.
 */
static void softKernelLoop(char *name, char *path, uint32_t cu_idx)
{
  void *sk_handle;
  void *kernel;
  kernel_t old_kernel;
  struct sk_operations ops;
  uint32_t *args_from_host;
  int32_t kernel_return = 0;
  unsigned int boh;
  int ret;
  

  devHdl = initXRTHandle(0);
  if (!devHdl) {
	  syslog(LOG_ERR, "Cannot open XRT device.\n");
	  return;
  }

  ret = createSoftKernel(&boh, cu_idx);
  if (ret) {
    syslog(LOG_ERR, "Cannot create soft kernel.");
    return;
  }

  /* Open and load the soft kernel. */
  sk_handle = dlopen(path, RTLD_LAZY | RTLD_GLOBAL);
  char *errstr = dlerror();
  if(errstr != NULL) {
    syslog(LOG_ERR, "Dynamic Link error: %s\n", errstr);
    return;
  }
  if (!sk_handle) {
    syslog(LOG_ERR, "Cannot open %s\n", path);
    return;
  }

  // Parse PS kernel for arguments and map buffers
  std::vector<xrt_core::pskernel::kernel_argument> args = xrt_core::pskernel::pskernel_parse(path,name);
  syslog(LOG_INFO,"PS kernel arguments parsed.  Num Arguments = %d\n",args.size());
  
  old_kernel = (kernel_t)dlsym(sk_handle, name);
  if (!old_kernel) {
    syslog(LOG_ERR, "Cannot find kernel %s\n", name);
    return;
  }

  kernel = dlsym(sk_handle, name);
  if (!kernel) {
    syslog(LOG_ERR, "Cannot find kernel %s\n", name);
    return;
  }

  syslog(LOG_INFO, "%s_%d start running\n", name, cu_idx);

  /* Set Kernel Ops */
  ops.getHostBO     = &getHostBO;
  ops.mapBO         = &mapBO;
  ops.freeBO        = &freeBO;
  ops.getBufferFd   = &getBufferFd;
  ops.logMsg        = &logMsg;

  args_from_host = (unsigned *)getKernelArg(boh, cu_idx);
  if (args_from_host == MAP_FAILED) {
      syslog(LOG_ERR, "Failed to map soft kernel args for %s_%d", name, cu_idx);
      freeBO(boh);
      dlclose(sk_handle);
      return;
  }

  // Prep FFI type for all kernel arguments
  ffi_cif cif;
  ffi_type *ffi_args[args.size()];
  void *values[args.size()];
  ffi_arg rc;
  void* ffi_arg_values[args.size()];
  
  // Buffer Objects
  unsigned int boHandles[args.size()];
  void* bos[args.size()];
  uint64_t boSize[args.size()];
  std::vector<int> bo_list;
  
  for(int i=0;i<args.size();i++) {
    ffi_args[i] = &args[i].ffitype;
  }
  
  if(ffi_prep_cif(&cif,FFI_DEFAULT_ABI, args.size(), &ffi_type_uint32,ffi_args) != FFI_OK) {
    syslog(LOG_ERR, "Cannot prep FFI arguments!");
    return;
  }
  
  while (1) {
    ret = waitNextCmd(cu_idx);

    if (ret) {
      /* We are told to exit the soft kernel loop */
      syslog(LOG_INFO, "Exit soft kernel %s\n", name);
      break;
    }

    syslog(LOG_INFO, "Got new kernel command!\n");
    
    /* Reg file indicates the kernel should not be running. */
    if (!(args_from_host[0] & 0x1))
      continue; //AP_START bit is not set; New Cmd is not available

    // New PS Kernel implementation
    // Check for call signature of only 2 arguments and 2nd argument has name of ops
    if((args.size()==2 && args[1].name.compare("ops")==0) || args.empty()) {
    } else {
      // FFI PS Kernel implementation
      // Map buffers used by kernel
      for(int i=0;i<args.size();i++) {
	if(args[i].type == xrt_core::pskernel::kernel_argument::argtype::global) {
	  uint64_t *buf_addr_ptr = (uint64_t*)(&args_from_host[args[i].offset/4]);
	  uint64_t buf_addr = reinterpret_cast<uint64_t>(*buf_addr_ptr);
	  uint64_t *buf_size_ptr = (uint64_t*)(&args_from_host[args[i].offset/4+2]);
	  uint64_t buf_size = reinterpret_cast<uint64_t>(*buf_size_ptr);
	  boSize[i] = buf_size;
	  
	  boHandles[i] = xclGetHostBO(devHdl,buf_addr,buf_size);
	  bos[i] = xclMapBO(devHdl,boHandles[i],true);
	  ffi_arg_values[i] = &bos[i];
	  bo_list.emplace_back(i);
	} else {
	  ffi_arg_values[i] = &args_from_host[args[i].offset/4];
	}
      }
    
    }

    // Original PS Kernel implementation
    // Check for call signature of only 2 arguments and 2nd argument has name os ops
    if((args.size()==2 && args[1].name.compare("ops")==0) || args.empty()) {
      kernel_return = old_kernel(&args_from_host[1],&ops);
    } else {
      ffi_call(&cif,FFI_FN(kernel), &kernel_return, ffi_arg_values);
    }
    args_from_host[1] = (uint32_t)kernel_return;

    // Unmap Buffers
    if((args.size()==2 && args[1].name.compare("ops")==0) || args.empty()) {
      // Nothing to be done here for original PS kernel implementation
    } else {
      for(auto i:bo_list) {
	munmap(bos[i],boSize[i]);
	xclFreeBO(devHdl,boHandles[i]);
      }
    }
  }

  dlclose(sk_handle);
  (void) destroySoftKernel(boh, args_from_host);
  xclClose(devHdl);
}

static inline void getSoftKernelPathName(uint32_t cu_idx, char *path)
{
  snprintf(path, XRT_MAX_PATH_LENGTH, "%s%s%d", SOFT_KERNEL_FILE_PATH,
          SOFT_KERNEL_FILE_NAME, cu_idx);
}

static inline void getSoftKernelPath(char *path)
{
  snprintf(path, XRT_MAX_PATH_LENGTH, "%s", SOFT_KERNEL_FILE_PATH);
}

/*
 * This function create a soft kernel file.
 * paddr  : The physical address of the soft kernel shared object.
 *          This image is DMAed from host to PS's memory. The XRT
 *          provides interface to create a BO handle for this
 *          memory and map it to process's memory space. So that
 *          we can create a file and copy the image to it.
 * size   : Size of the soft kenel image.
 * cuidx  : CU index. The file will be name bease on fixed path
 *          and name plus the CU index.
 */
static int createSoftKernelFile(xclDeviceHandle handle, uint32_t cuidx,
	int bohdl)
{
  unsigned int boHandle;
  FILE *fptr;
  void *buf;
  char path[XRT_MAX_PATH_LENGTH];
  int len, i;

  xclBOProperties prop;
  if (xclGetBOProperties(handle, bohdl, &prop)) {
    syslog(LOG_ERR, "Cannot get BO info.\n");
    xclFreeBO(handle, bohdl);
    return -1;
  }

  buf = xclMapBO(handle, bohdl, false);
  if (!buf) {
    syslog(LOG_ERR, "Cannot map softkernel BO.\n");
    xclFreeBO(handle, bohdl);
    return -1;
  }

  snprintf(path, XRT_MAX_PATH_LENGTH, "%s", SOFT_KERNEL_FILE_PATH);
  len = strlen(path);

  /* If not exist, create the path one by one. */
  for (i = 1; i < len; i++) {
    if (path[i] == '/') {
      path[i] = '\0';
      if (access(path, F_OK) != 0) {
        if (mkdir(path, 0744) != 0) {
          syslog(LOG_ERR, "Cannot create soft kernel file.\n");
          return -1;
        }
      }
      path[i] = '/';
    }
  }

  /* Create soft kernel file */
  getSoftKernelPathName(cuidx, path);

  fptr = fopen(path, "w+b");
  if (fptr == NULL) {
     syslog(LOG_ERR, "Cannot create file: %s\n", path);
     munmap(buf, prop.size);
     xclFreeBO(handle, bohdl);
     return -1;
  }

  /* copy the soft kernel to file */
  if (fwrite(buf, prop.size, 1, fptr) != 1) {
    syslog(LOG_ERR, "Fail to write to file %s.\n", path);
    fclose(fptr);
    munmap(buf, prop.size);
    xclFreeBO(handle, bohdl);
    return -1;
  }

  fclose(fptr);
  munmap(buf, prop.size);
  xclFreeBO(handle, bohdl);

  return 0;
}

#define STACKTRACE_DEPTH	(25)
static void stacktrace_logger(const int sig)
{
  const int stack_depth = STACKTRACE_DEPTH;
  syslog(LOG_ERR, "%s - got %d\n", __func__, sig);
  if (sig == SIGCHLD)
    return;
  void *array[stack_depth];
  int nSize = backtrace(array, stack_depth);
  char **symbols = backtrace_symbols(array, nSize);
  if (symbols) {
    for (int i = 0; i < nSize; i++)
      syslog(LOG_ERR, "%s\n", symbols[i]);
    free(symbols);
  }
}

/* Define a signal handler for the child to handle signals */
static void sigLog(const int sig)
{
  syslog(LOG_ERR, "%s - got %d\n", __func__, sig);
  stacktrace_logger(sig);
  exit(EXIT_FAILURE);
}

#define PNAME_LEN	(16)
void configSoftKernel(xclDeviceHandle handle, xclSKCmd *cmd)
{
  pid_t pid;
  uint32_t i;

  if (createSoftKernelFile(handle, cmd->start_cuidx, cmd->bohdl) != 0)
    return;

  for (i = cmd->start_cuidx; i < cmd->start_cuidx + cmd->cu_nums; i++) {
    /*
     * We create a process for each Compute Unit with same soft
     * kernel image.
     */
    pid = fork();
    if (pid > 0)
      signal(SIGCHLD,SIG_IGN);

    if (pid == 0) {
      char path[XRT_MAX_PATH_LENGTH];
      char proc_name[PNAME_LEN] = {};

      /* Install Signal Handler for the Child Processes/Soft-Kernels */
      struct sigaction act;
      act.sa_handler = sigLog;
      sigemptyset(&act.sa_mask);
      act.sa_flags = 0;
      sigaction(SIGHUP, &act, 0);
      sigaction(SIGINT, &act, 0);
      sigaction(SIGQUIT , &act, 0);
      sigaction(SIGILL, &act, 0);
      sigaction(SIGTRAP, &act, 0);
      sigaction(SIGABRT, &act, 0);
      sigaction(SIGBUS, &act, 0);
      sigaction(SIGFPE, &act, 0);
      sigaction(SIGKILL, &act, 0);
      sigaction(SIGUSR1, &act, 0);
      sigaction(SIGSEGV, &act, 0);
      sigaction(SIGUSR2, &act, 0);
      sigaction(SIGPIPE, &act, 0);
      sigaction(SIGALRM, &act, 0);
      sigaction(SIGTERM, &act, 0);

      (void)snprintf(proc_name, PNAME_LEN, "%s%d", cmd->krnl_name, i);
      if (prctl(PR_SET_NAME, (char *)proc_name) != 0) {
          syslog(LOG_ERR, "Unable to set process name to %s due to %s\n", proc_name, strerror(errno));
      }

      getSoftKernelPathName(cmd->start_cuidx, path);

      /* Start the soft kenel loop for each CU. */
      softKernelLoop(cmd->krnl_name, path, i);
      syslog(LOG_INFO, "Kernel %s was terminated\n", cmd->krnl_name);
      exit(EXIT_SUCCESS);
    }

    if (pid < 0)
      syslog(LOG_ERR, "Unable to create soft kernel process( %d)\n", i);
  }
}

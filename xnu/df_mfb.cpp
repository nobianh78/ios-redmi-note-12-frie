
/* fix-xcode16 P6: probe IOMobileFramebuffer for fire (appended to
 * IOPlatformExpert.cpp). Not a real display driver: the class is named
 * IOMobileFramebuffer so that IOServiceMatching("IOMobileFramebuffer") from
 * IOMobileFramebuffer.framework finds it. Every user-client call is logged
 * ("mfb sel ...") and answered with success and zeroed output, so the log shows
 * which selectors backboardd/QuartzCore need. clientMemoryForType hands out the
 * MT6768 framebuffer the df console draws into. */
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOWorkLoop.h>

extern "C" int proc_selfpid(void);
extern "C" volatile uint32_t *df_fbva;
extern "C" volatile uint32_t *df_wdt;
extern "C" int df_gui_active;
extern "C" vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr,
                                            vm_size_t size);
struct _vm_map;
typedef struct _vm_map *vm_map_t;
extern "C" vm_map_t get_task_map(task_t task);
extern "C" kern_return_t mach_vm_allocate(vm_map_t target,
                                          mach_vm_address_t *address,
                                          mach_vm_size_t size, int flags);
extern "C" mach_msg_return_t
mach_msg_send_from_kernel_proper(mach_msg_header_t *msg,
                                 mach_msg_size_t send_size);

#define DF_MFB_PHYS 0x7ce6a000ULL
#define DF_MFB_W 1080
#define DF_MFB_H 2400

static mach_vm_address_t g_user_fb_addr = 0;
static IOBufferMemoryDescriptor *g_sw_fb_md = NULL;
static uint32_t *g_sw_fb_kva = NULL;

static void df_flush_fb(void) {
  if (df_wdt) {
    df_wdt[0x08 / 4] = 0x1971; /* WDT_RST reload */
  }
  if (!df_fbva)
    return;
  for (uintptr_t a = (uintptr_t)df_fbva;
       a < (uintptr_t)df_fbva + (uintptr_t)DF_MFB_W * DF_MFB_H * 4; a += 64) {
    __asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
  }
  __asm__ volatile("dsb sy" ::: "memory");
}

static void df_clear_fb(void) {
  if (!df_fbva)
    return;
  for (size_t i = 0; i < (size_t)DF_MFB_W * DF_MFB_H; i++) {
    df_fbva[i] = 0xff000000;
  }
  df_flush_fb();
}

class IOMobileFramebuffer : public IOService {
  OSDeclareDefaultStructors(IOMobileFramebuffer);

public:
  virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
  virtual IOReturn newUserClient(task_t owningTask, void *securityID,
                                 UInt32 type,
                                 IOUserClient **handler) APPLE_KEXT_OVERRIDE;
};

struct DFAsyncSlot {
  bool active;
  uint32_t selector;
  OSAsyncReference64 ref;
};

class DFMobileFramebufferUC : public IOUserClient {
  OSDeclareDefaultStructors(DFMobileFramebufferUC);

  IOWorkLoop *fWL;
  IOTimerEventSource *fVsyncTimer;
  DFAsyncSlot fAsync[4];
  mach_port_t fNotifyPort;
  uint64_t fVsyncCount;
  static void vsyncTimerAction(OSObject *owner, IOTimerEventSource *sender);

public:
  virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
  virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
  virtual IOReturn registerNotificationPort(mach_port_t port, UInt32 type,
                                            io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
  virtual IOReturn externalMethod(uint32_t selector,
                                  IOExternalMethodArguments *args,
                                  IOExternalMethodDispatch *dispatch,
                                  OSObject *target,
                                  void *reference) APPLE_KEXT_OVERRIDE;
  virtual IOReturn
  clientMemoryForType(UInt32 type, IOOptionBits *options,
                      IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;
  virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
};

class IOAccelerator : public IOService {
  OSDeclareDefaultStructors(IOAccelerator);

public:
  virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};

class IOAcceleratorES : public IOAccelerator {
  OSDeclareDefaultStructors(IOAcceleratorES);

public:
  virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
  virtual OSObject *copyProperty(const char *aKey) const APPLE_KEXT_OVERRIDE;
  virtual IOReturn newUserClient(task_t owningTask, void *securityID,
                                 UInt32 type,
                                 IOUserClient **handler) APPLE_KEXT_OVERRIDE;
};

class DFIOAcceleratorUC : public IOUserClient {
  OSDeclareDefaultStructors(DFIOAcceleratorUC);

public:
  virtual IOReturn externalMethod(uint32_t selector,
                                  IOExternalMethodArguments *args,
                                  IOExternalMethodDispatch *dispatch,
                                  OSObject *target,
                                  void *reference) APPLE_KEXT_OVERRIDE;
  virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOMobileFramebuffer, IOService);
OSDefineMetaClassAndStructors(DFMobileFramebufferUC, IOUserClient);
OSDefineMetaClassAndStructors(IOAccelerator, IOService);
OSDefineMetaClassAndStructors(IOAcceleratorES, IOAccelerator);
OSDefineMetaClassAndStructors(DFIOAcceleratorUC, IOUserClient);

bool IOAccelerator::start(IOService *provider) {
  return IOService::start(provider);
}

bool IOAcceleratorES::start(IOService *provider) {
  if (!IOAccelerator::start(provider)) {
    return false;
  }
  setProperty("IOClass", "IOAcceleratorES");
  setProperty("IOMatchCategory", "IOAccelerator");
  setProperty("IOGLBundleName", "AppleMetalGLRenderer");
  setProperty("IOGLESBundleName", "AppleMetalGLRenderer");
  setProperty("IOGLESDefaultDiagnosticMode", (uint64_t)0, 32);
  setProperty("IOGPUVendorId", (uint64_t)0x106b, 32);
  setProperty("IOGPUDeviceId", (uint64_t)0x0001, 32);
  setProperty("IOUserClientClass", "DFIOAcceleratorUC");
  registerService();
  printf("ios: IOAcceleratorES registered\n");
  return true;
}

OSObject *IOAcceleratorES::copyProperty(const char *aKey) const {
  static unsigned acc_prop_cnt = 0;
  if (acc_prop_cnt++ < 40) {
    printf("acc: copyProperty(%s)\n", aKey ? aKey : "null");
  }
  return IOService::copyProperty(aKey);
}

IOReturn IOAcceleratorES::newUserClient(task_t owningTask, void *securityID,
                                        UInt32 type, IOUserClient **handler) {
  DFIOAcceleratorUC *uc = OSTypeAlloc(DFIOAcceleratorUC);
  if (!uc || !uc->initWithTask(owningTask, securityID, type) ||
      !uc->attach(this) || !uc->start(this)) {
    if (uc) {
      uc->detach(this);
      uc->release();
    }
    printf("acc: newUserClient failed\n");
    return kIOReturnNoMemory;
  }
  printf("acc: open type %u by pid %d\n", (unsigned)type, proc_selfpid());
  *handler = uc;
  return kIOReturnSuccess;
}

IOReturn DFIOAcceleratorUC::externalMethod(uint32_t selector,
                                           IOExternalMethodArguments *a,
                                           IOExternalMethodDispatch *dispatch,
                                           OSObject *target, void *reference) {
  printf("acc sel %u pid %d\n", selector, proc_selfpid());
  for (uint32_t i = 0; i < a->scalarOutputCount; i++) {
    a->scalarOutput[i] = 0;
  }
  if (a->structureOutput && a->structureOutputSize) {
    bzero(a->structureOutput, a->structureOutputSize);
  }
  return kIOReturnSuccess;
}

IOReturn DFIOAcceleratorUC::clientClose(void) {
  terminate();
  return kIOReturnSuccess;
}

static unsigned df_mfb_logs;

bool IOMobileFramebuffer::start(IOService *provider) {
  if (!IOService::start(provider)) {
    return false;
  }
  setProperty("DisplayWidth", DF_MFB_W, 32);
  setProperty("DisplayHeight", DF_MFB_H, 32);
  setProperty("IOMFBBrightnessLevel", 0x10000, 32);
  setProperty("external", false);
  setProperty("IOUserClientClass", "DFMobileFramebufferUC");
  setName("disp0");
  setProperty("primary-display", kOSBooleanTrue);
  setProperty("display-id", (uint64_t)0, 32);
  setProperty("backlight-control", kOSBooleanTrue);
  setProperty("NormalModeEnable", kOSBooleanTrue);
  setProperty("dfr-supported", kOSBooleanFalse);
  setProperty("IOMFBSupportsPBuffer", kOSBooleanTrue);
  setProperty("is-main-display", kOSBooleanTrue);
  setProperty("display-type", "LCD");
  registerService();
  printf("mfb: IOMobileFramebuffer %dx%d at 0x%llx registered\n", DF_MFB_W,
         DF_MFB_H, DF_MFB_PHYS);
  return true;
}

IOReturn IOMobileFramebuffer::newUserClient(task_t owningTask, void *securityID,
                                            UInt32 type,
                                            IOUserClient **handler) {
  DFMobileFramebufferUC *uc = OSTypeAlloc(DFMobileFramebufferUC);
  if (!uc || !uc->initWithTask(owningTask, securityID, type) ||
      !uc->attach(this) || !uc->start(this)) {
    if (uc) {
      uc->detach(this);
      uc->release();
    }
    printf("mfb: newUserClient type %u failed\n", (unsigned)type);
    return kIOReturnNoMemory;
  }
  printf("mfb: open type %u by pid %d\n", (unsigned)type, proc_selfpid());
  if (!df_gui_active) {
    df_gui_active = 1;
    df_clear_fb();
  }
  *handler = uc;
  return kIOReturnSuccess;
}

bool DFMobileFramebufferUC::start(IOService *provider) {
  if (!IOUserClient::start(provider)) {
    return false;
  }
  fWL = NULL;
  fVsyncTimer = NULL;
  fNotifyPort = MACH_PORT_NULL;
  fVsyncCount = 0;
  bzero(fAsync, sizeof(fAsync));
  fWL = IOWorkLoop::workLoop();
  if (fWL) {
    fVsyncTimer = IOTimerEventSource::timerEventSource(
        this, &DFMobileFramebufferUC::vsyncTimerAction);
    if (fVsyncTimer) {
      fWL->addEventSource(fVsyncTimer);
      fVsyncTimer->setTimeoutMS(33);
    }
  }
  return true;
}

void DFMobileFramebufferUC::stop(IOService *provider) {
  fNotifyPort = MACH_PORT_NULL;
  if (fVsyncTimer) {
    fVsyncTimer->cancelTimeout();
    if (fWL) {
      fWL->removeEventSource(fVsyncTimer);
    }
    fVsyncTimer->release();
    fVsyncTimer = NULL;
  }
  if (fWL) {
    fWL->release();
    fWL = NULL;
  }
  IOUserClient::stop(provider);
}

void DFMobileFramebufferUC::vsyncTimerAction(OSObject *owner,
                                             IOTimerEventSource *sender) {
  DFMobileFramebufferUC *uc = OSDynamicCast(DFMobileFramebufferUC, owner);
  if (!uc) {
    return;
  }
  if (df_wdt) {
    df_wdt[0x08 / 4] = 0x1971; /* WDT_RST reload */
  }
  uc->fVsyncCount++;
  uint64_t now = 0;
  clock_get_uptime(&now);
  for (int i = 0; i < 4; i++) {
    if (uc->fAsync[i].active) {
      io_user_reference_t args[2] = { (io_user_reference_t)now,
                                      (io_user_reference_t)uc->fVsyncCount };
      IOReturn r = sendAsyncResult64(uc->fAsync[i].ref, kIOReturnSuccess, args, 2);
      if (uc->fVsyncCount <= 5) {
        printf("mfb: vsync #%llu sel=%u sendAsync -> 0x%x\n",
               (unsigned long long)uc->fVsyncCount, uc->fAsync[i].selector, r);
      }
    }
  }
  if (uc->fNotifyPort != MACH_PORT_NULL && (uc->fVsyncCount % 3) == 0) {
    static uint32_t s_notify_cnt = 0;
    mach_msg_header_t msg;
    bzero(&msg, sizeof(msg));
    msg.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    msg.msgh_size = sizeof(msg);
    msg.msgh_remote_port = uc->fNotifyPort;
    msg.msgh_local_port = MACH_PORT_NULL;
    msg.msgh_voucher_port = 0;
    msg.msgh_id = 0;
    mach_msg_return_t mr = mach_msg_send_from_kernel_proper(&msg, sizeof(msg));
    if (++s_notify_cnt <= 5) {
      printf("mfb: notify type=3 #%u -> 0x%x\n", s_notify_cnt, (unsigned)mr);
    }
  }
  if (sender) {
    sender->setTimeoutMS(33);
  }
}

IOReturn DFMobileFramebufferUC::registerNotificationPort(
    mach_port_t port, UInt32 type, io_user_reference_t refCon) {
  printf("mfb: registerNotificationPort type=%u port=%p refCon=0x%llx pid=%d\n",
         (unsigned)type, (void *)port, (unsigned long long)refCon, proc_selfpid());
  if (type == 3 && port != MACH_PORT_NULL) {
    fNotifyPort = port;
  }
  return kIOReturnSuccess;
}

IOReturn DFMobileFramebufferUC::externalMethod(
    uint32_t selector, IOExternalMethodArguments *a,
    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) {
  if (df_mfb_logs++ < 400) {
    printf("mfb sel %u pid %d in %u/%u out %u/%u async=%d\n", selector,
           proc_selfpid(), a->scalarInputCount, a->structureInputSize,
           a->scalarOutputCount, a->structureOutputSize,
           a->asyncWakePort != MACH_PORT_NULL ? 1 : 0);
  }
  if (a->asyncWakePort != MACH_PORT_NULL && a->asyncReference) {
    printf("mfb: ASYNC sel %u pid %d refCnt=%u\n", selector, proc_selfpid(),
           a->asyncReferenceCount);
    int slot = -1;
    for (int i = 0; i < 4; i++) {
      if (fAsync[i].active && fAsync[i].selector == selector) {
        slot = i;
        break;
      }
      if (!fAsync[i].active && slot < 0) {
        slot = i;
      }
    }
    if (slot >= 0) {
      fAsync[slot].active = true;
      fAsync[slot].selector = selector;
      for (uint32_t i = 0; i < kOSAsyncRef64Count; i++) {
        fAsync[slot].ref[i] =
            (i < a->asyncReferenceCount) ? a->asyncReference[i] : 0;
      }
      fAsync[slot].ref[0] = (io_user_reference_t)(uintptr_t)a->asyncWakePort;
    }
  }
  for (uint32_t i = 0; i < a->scalarOutputCount; i++) {
    a->scalarOutput[i] = 0;
  }
  if (a->structureOutput && a->structureOutputSize) {
    bzero(a->structureOutput, a->structureOutputSize);
  }
  if (selector == 3 && a->scalarOutputCount >= 1) {
    /* GetLayerDefaultSurface: return surface_id = 1 */
    a->scalarOutput[0] = 1;
    printf("mfb: GetLayerDefaultSurface -> id 1\n");
    return kIOReturnSuccess;
  }
  if (selector == 4 && a->scalarOutputCount >= 1) {
    /* SwapBegin: return incremental swap token */
    static uint32_t s_swap_token = 1;
    a->scalarOutput[0] = ++s_swap_token;
  }
  if (selector == 8 && a->scalarOutputCount >= 2) { /* display size */
    a->scalarOutput[0] = DF_MFB_W;
    a->scalarOutput[1] = DF_MFB_H;
  }
  if (selector == 12 && a->scalarOutputCount >= 1) { /* power change */
    a->scalarOutput[0] = 1;
  }
  if (selector == 15 && a->scalarOutputCount >= 1) {
    a->scalarOutput[0] = 1;
  }
  if (selector == 18 && a->scalarOutputCount >= 1) { /* isMainDisplay */
    a->scalarOutput[0] = 1;
    printf("mfb: IsMainDisplay -> 1\n");
  }
  if (selector == 20 && a->scalarOutputCount >= 2) { /* canvas size */
    a->scalarOutput[0] = DF_MFB_W;
    a->scalarOutput[1] = DF_MFB_H;
  }
  if (selector == 28 && a->scalarOutputCount >= 1) { /* active flag */
    a->scalarOutput[0] = 1;
  }
  if (selector == 29 && a->structureOutput && a->structureOutputSize >= 8) {
    uint32_t *timing = (uint32_t *)a->structureOutput;
    timing[0] = 60; /* 60 Hz */
    timing[1] = 0;
  }
  if (selector == 57 && a->scalarOutputCount >= 1) { /* power state */
    a->scalarOutput[0] = 1;
  }
  if (selector == 22 || selector == 33) {
    if (a->scalarOutputCount >= 1) {
      a->scalarOutput[0] = 0;
    }
  }
  if (selector == 74) {
    /* Display Mode Information */
    if (a->scalarOutputCount >= 1) {
      a->scalarOutput[0] = 1;
    }
    if (a->structureOutput && a->structureOutputSize >= 24) {
      uint32_t *m = (uint32_t *)a->structureOutput;
      m[0] = DF_MFB_W;       /* 1080 */
      m[1] = DF_MFB_H;       /* 2400 */
      m[2] = 60;             /* 60 Hz */
      m[3] = DF_MFB_W * 4;   /* 4320 bytes per row */
      m[4] = 32;             /* 32 bpp */
      m[5] = 0x42475241;     /* 'BGRA' */
    }
    printf("mfb: sel 74 (display mode) -> 1080x2400@60\n");
  }
  if (selector == 79) {
    if (a->scalarOutputCount >= 1) {
      a->scalarOutput[0] = 1;
    }
    if (a->structureOutput && a->structureOutputSize >= 16) {
      uint32_t *m = (uint32_t *)a->structureOutput;
      m[0] = DF_MFB_W;
      m[1] = DF_MFB_H;
      m[2] = 60;
      m[3] = 0;
    }
    printf("mfb: sel 79 -> ok\n");
  }
  if (selector == 5) {
    /* SwapEnd */
    static uint32_t s_swap_cnt = 0;
    if (!df_fbva) {
      df_fbva = (volatile uint32_t *)ml_io_map_unmappable(
          DF_MFB_PHYS, (vm_size_t)DF_MFB_W * DF_MFB_H * 4);
    }
    s_swap_cnt++;
    uint32_t nonblack = 0;
    uint32_t nonzero = 0;
    if (g_sw_fb_kva && df_fbva) {
      size_t n = (size_t)DF_MFB_W * DF_MFB_H;
      for (size_t i = 0; i < n; i++) {
        uint32_t px = g_sw_fb_kva[i];
        df_fbva[i] = px | 0xff000000U;
        if (px != 0) {
          nonzero++;
          if ((px & 0x00ffffffU) != 0) {
            nonblack++;
          }
        }
      }
    }
    if (a->structureInput && a->structureInputSize >= 0x60) {
      uint8_t *p = (uint8_t *)a->structureInput;
      uint32_t s0 = *(uint32_t *)(p + 0x54);
      if (s_swap_cnt <= 20) {
        printf("mfb: SwapEnd #%u sid=%u nz=%u nb=%u [0]=0x%x [mid]=0x%x\n",
               s_swap_cnt, s0, nonzero, nonblack,
               g_sw_fb_kva ? g_sw_fb_kva[0] : 0,
               g_sw_fb_kva ? g_sw_fb_kva[(DF_MFB_W * DF_MFB_H) / 2] : 0);
      }
    }
    df_flush_fb();
  }
  if (selector == 6) {
    /* SwapWait */
    if (g_sw_fb_kva && df_fbva) {
      size_t n = (size_t)DF_MFB_W * DF_MFB_H;
      for (size_t i = 0; i < n; i++) {
        df_fbva[i] = g_sw_fb_kva[i] | 0xff000000U;
      }
    }
    df_flush_fb();
  }
  return kIOReturnSuccess;
}

IOReturn
DFMobileFramebufferUC::clientMemoryForType(UInt32 type, IOOptionBits *options,
                                           IOMemoryDescriptor **memory) {
  printf("mfb: clientMemoryForType %u\n", (unsigned)type);
  *memory = IOMemoryDescriptor::withPhysicalAddress(
      DF_MFB_PHYS, (IOByteCount)DF_MFB_W * DF_MFB_H * 4, kIODirectionInOut);
  return *memory ? kIOReturnSuccess : kIOReturnNoMemory;
}

IOReturn DFMobileFramebufferUC::clientClose(void) {
  terminate();
  return kIOReturnSuccess;
}

/* Probe IOCoreSurfaceRoot (IOSurface.framework's kernel service on iOS): logs
 * every selector
 * ("ios sel ...") and answers success with zeroed output. Not a working
 * IOSurface yet. */
class IOCoreSurfaceRoot : public IOService {
  OSDeclareDefaultStructors(IOCoreSurfaceRoot);

public:
  virtual IOReturn newUserClient(task_t owningTask, void *securityID,
                                 UInt32 type,
                                 IOUserClient **handler) APPLE_KEXT_OVERRIDE;
};

class DFCoreSurfaceUC : public IOUserClient {
  OSDeclareDefaultStructors(DFCoreSurfaceUC);
  task_t fTask;

  mach_vm_address_t fUserAddr;
  mach_vm_address_t fUserFbAddr;

public:
  virtual bool initWithTask(task_t owningTask, void *securityID,
                            UInt32 type) APPLE_KEXT_OVERRIDE;
  virtual IOReturn externalMethod(uint32_t selector,
                                  IOExternalMethodArguments *args,
                                  IOExternalMethodDispatch *dispatch,
                                  OSObject *target,
                                  void *reference) APPLE_KEXT_OVERRIDE;
  virtual IOReturn
  clientMemoryForType(UInt32 type, IOOptionBits *options,
                      IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;
  virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOCoreSurfaceRoot, IOService);
OSDefineMetaClassAndStructors(DFCoreSurfaceUC, IOUserClient);

static unsigned df_ios_logs;
static uint8_t s_iosurface_comm[16384] __attribute__((aligned(16384)));

bool DFCoreSurfaceUC::initWithTask(task_t owningTask, void *securityID,
                                   UInt32 type) {
  if (!IOUserClient::initWithTask(owningTask, securityID, type))
    return false;
  fTask = owningTask;
  fUserAddr = 0;
  fUserFbAddr = 0;
  return true;
}

IOReturn IOCoreSurfaceRoot::newUserClient(task_t owningTask, void *securityID,
                                          UInt32 type, IOUserClient **handler) {
  DFCoreSurfaceUC *uc = OSTypeAlloc(DFCoreSurfaceUC);
  if (!uc || !uc->initWithTask(owningTask, securityID, type) ||
      !uc->attach(this) || !uc->start(this)) {
    if (uc) {
      uc->detach(this);
      uc->release();
    }
    printf("ios: newUserClient type %u failed\n", (unsigned)type);
    return kIOReturnNoMemory;
  }
  printf("ios: open type %u by pid %d\n", (unsigned)type, proc_selfpid());
  *handler = uc;
  return kIOReturnSuccess;
}

IOReturn DFCoreSurfaceUC::externalMethod(uint32_t selector,
                                         IOExternalMethodArguments *a,
                                         IOExternalMethodDispatch *dispatch,
                                         OSObject *target, void *reference) {
  if (df_ios_logs++ < 400) {
    printf("ios sel %u pid %d in %u/%u out %u/%u\n", selector, proc_selfpid(),
           a->scalarInputCount, a->structureInputSize, a->scalarOutputCount,
           a->structureOutputSize);
  }
  for (uint32_t i = 0; i < a->scalarOutputCount; i++) {
    a->scalarOutput[i] = 0;
  }
  if (a->structureOutput && a->structureOutputSize) {
    bzero(a->structureOutput, a->structureOutputSize);
  }
  if (selector == 13 && a->structureOutput && a->structureOutputSize >= 24) {
    struct io_surface_limits {
      uint32_t align;
      uint32_t max_surface_count;
      uint32_t max_bytes_per_row;
      uint32_t max_width;
      uint32_t max_height;
      uint16_t reserved;
      uint16_t version;
    } *limits = (struct io_surface_limits *)a->structureOutput;
    limits->align = 64;
    limits->max_surface_count = 1024;
    limits->max_bytes_per_row = 16384;
    limits->max_width = 8192;
    limits->max_height = 8192;
    limits->reserved = 0;
    limits->version = 1;
    printf("ios: get_limits ok (ver=1)\n");
  }
  if ((selector == 0 || selector == 34 || a->structureOutputSize >= 0xa68) &&
      a->structureOutput && a->structureOutputSize >= 0xa68) {
    uint8_t *out = (uint8_t *)a->structureOutput;
    task_t t = fTask ? fTask : current_task();
    vm_map_t map = t ? get_task_map(t) : NULL;

    if (!fUserAddr) {
      *(uint32_t *)(s_iosurface_comm + 0x0c) = 1; /* seed at offset 0xc */
      *(uint32_t *)(s_iosurface_comm + 0x10) = 1;
      IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange(
          (mach_vm_address_t)s_iosurface_comm, sizeof(s_iosurface_comm),
          kIODirectionInOut, kernel_task);
      if (md) {
        IOMemoryMap *m =
            md->createMappingInTask(t ? t : current_task(), 0, kIOMapAnywhere);
        if (m) {
          fUserAddr = m->getAddress();
        }
        md->release();
      }
      if (!fUserAddr && map) {
        mach_vm_allocate(map, &fUserAddr, 16384, 1);
      }
    }

    /* Back IOSurface with normal cacheable RAM (1080x2400x4 = 10,368,000 bytes)
     * so CA::OGL::SWContext can use NEON SIMD writes without Device-nGnRE
     * alignment faults; SwapEnd blits scalar 32-bit pixels into df_fbva. */
    if (!g_sw_fb_md) {
      g_sw_fb_md = IOBufferMemoryDescriptor::inTaskWithOptions(
          kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared,
          (vm_size_t)DF_MFB_W * DF_MFB_H * 4, page_size);
      if (g_sw_fb_md) {
        g_sw_fb_kva = (uint32_t *)g_sw_fb_md->getBytesNoCopy();
        if (g_sw_fb_kva) {
          bzero(g_sw_fb_kva, (size_t)DF_MFB_W * DF_MFB_H * 4);
        }
        printf("ios: allocated SW RAM fb kva=%p\n", (void *)g_sw_fb_kva);
      }
    }
    if (!fUserFbAddr) {
      if (t && g_sw_fb_md) {
        IOMemoryMap *fb_map =
            g_sw_fb_md->createMappingInTask(t, 0, kIOMapAnywhere);
        if (fb_map) {
          fUserFbAddr = fb_map->getAddress();
        }
      }
      if (!fUserFbAddr && map) {
        mach_vm_allocate(map, &fUserFbAddr,
                         (mach_vm_size_t)DF_MFB_W * DF_MFB_H * 4, 1);
      }
    }

    mach_vm_address_t user_addr = fUserAddr;
    mach_vm_address_t user_fb_addr = fUserFbAddr;
    g_user_fb_addr = user_fb_addr;

    uint32_t target_sid = 1;
    if (a->scalarInputCount >= 1 && a->scalarInput[0] != 0) {
      target_sid = (uint32_t)a->scalarInput[0];
    }

    /* Fill IOSurface layout matching IOSurface.framework disassembly */
    *(uint64_t *)(out + 0x00) =
        (uint64_t)user_fb_addr; /* BaseAddress (returned by
                                   _IOSurfaceClientGetBaseAddressOfPlane) */
    *(uint64_t *)(out + 0x08) = (uint64_t)
        user_addr; /* mapped comm page (offset 0x70 in IOSurfaceClient) */
    *(uint64_t *)(out + 0x10) = 0;          /* detach/plane pointer */
    *(uint32_t *)(out + 0x18) = target_sid; /* surface_id */
    *(uint32_t *)(out + 0x1c) =
        DF_MFB_W * DF_MFB_H * 4;              /* alloc_size = 10,368,000 */
    *(uint32_t *)(out + 0x20) = DF_MFB_W;     /* width = 1080 */
    *(uint32_t *)(out + 0x24) = DF_MFB_H;     /* height = 2400 */
    *(uint32_t *)(out + 0x28) = DF_MFB_W * 4; /* bytes_per_row = 4320 */
    *(uint32_t *)(out + 0x2c) = 0;            /* plane 0 offset = 0 */
    *(uint32_t *)(out + 0x30) =
        DF_MFB_W * DF_MFB_H * 4;            /* plane 0 size = 10,368,000 */
    *(uint32_t *)(out + 0x34) = 0x42475241; /* pixel_format = 'BGRA' */
    *(uint32_t *)(out + 0x38) = 1;          /* plane_count = 1 */
    *(uint32_t *)(out + 0xdc) = 0;          /* plane 0 offset (alt) */
    *(uint32_t *)(out + 0x124) = 0;         /* plane 0 format flag */
    if (a->scalarOutputCount >= 1) {
      a->scalarOutput[0] = target_sid; /* surface_id */
    }
    printf("ios: sel %u -> id %u, fb=0x%llx, comm=0x%llx\n", selector,
           target_sid, (uint64_t)user_fb_addr, (uint64_t)user_addr);
  }
  if (selector == 9) {
    static uint32_t s_seed = 1;
    s_seed = (s_seed + 2) | 3;
    *(uint32_t *)(s_iosurface_comm + 0x0c) = s_seed;
    *(uint32_t *)(s_iosurface_comm + 0x10) = s_seed;
    if (a->structureOutput && a->structureOutputSize >= 4) {
      *(uint32_t *)a->structureOutput = s_seed;
    }
    printf("ios: sel 9 (set_value) -> seed=%u\n", s_seed);
  }
  if (selector == 27) {
    printf("ios: sel 27 (set_bulk_attachments)\n");
  }
  return kIOReturnSuccess;
}

IOReturn DFCoreSurfaceUC::clientMemoryForType(UInt32 type,
                                              IOOptionBits *options,
                                              IOMemoryDescriptor **memory) {
  printf("ios: clientMemoryForType %u\n", (unsigned)type);
  *memory = IOMemoryDescriptor::withPhysicalAddress(
      DF_MFB_PHYS, (IOByteCount)DF_MFB_W * DF_MFB_H * 4, kIODirectionInOut);
  return *memory ? kIOReturnSuccess : kIOReturnNoMemory;
}

IOReturn DFCoreSurfaceUC::clientClose(void) {
  terminate();
  return kIOReturnSuccess;
}

/* fix-xcode16 P6: log IOKit matching dictionaries looked up by backboardd
 * (which display does QuartzCore want?) */
extern "C" void df_match_log(const char *where, OSDictionary *m);
static unsigned df_ml;

extern "C" void df_match_log(const char *where, OSDictionary *m) {
  if (!m) {
    return;
  }
  if (current_task() == kernel_task) {
    return; /* early boot / kernel callers: no proc yet (zone_require_ro panic)
             */
  }
  if (kernproc == NULL) {
    return; /* IOKit reads the device tree before bsd_init: current_proc()
               itself panics then */
  }
  proc_t df_p = current_proc();
  if (df_p == NULL || df_p == kernproc) {
    return; /* before bsd_init kernproc is NULL: proc_best_name(NULL) panics in
               zone_require_ro */
  }
  const char *n = proc_best_name(df_p);
  if (!n || strcmp(n, "backboardd") != 0 || df_ml++ > 80) {
    return;
  }
  OSSerialize *s = OSSerialize::withCapacity(512);
  if (s && m->serialize(s)) {
    printf("MATCH %s %s\n", where, s->text());
  }
  if (s) {
    s->release();
  }
}

extern "C" void df_path_log(const char *path);
extern "C" void df_path_log(const char *path) {
  if (current_task() == kernel_task) {
    return; /* early boot / kernel callers: no proc yet (zone_require_ro panic)
             */
  }
  if (kernproc == NULL) {
    return; /* IOKit reads the device tree before bsd_init: current_proc()
               itself panics then */
  }
  proc_t df_p = current_proc();
  if (df_p == NULL || df_p == kernproc) {
    return; /* before bsd_init kernproc is NULL: proc_best_name(NULL) panics in
               zone_require_ro */
  }
  const char *n = proc_best_name(df_p);
  if (path && n && strcmp(n, "backboardd") == 0 && df_ml++ < 160) {
    printf("MATCH path %s\n", path);
  }
}

extern "C" void df_mfb_attach(IOService *provider) {
  IOCoreSurfaceRoot *sr = OSTypeAlloc(IOCoreSurfaceRoot);
  if (sr && sr->init() && sr->attach(provider) && sr->start(provider)) {
    sr->registerService();
    printf("ios: IOCoreSurfaceRoot registered\n");
  } else {
    printf("ios: IOCoreSurfaceRoot attach failed\n");
  }
  IOMobileFramebuffer *fb = OSTypeAlloc(IOMobileFramebuffer);
  if (fb && fb->init() && fb->attach(provider) && fb->start(provider)) {
    printf("mfb: IOMobileFramebuffer registered\n");
  } else {
    printf("mfb: attach failed\n");
    if (fb) {
      fb->release();
    }
  }
  IOAcceleratorES *acc = OSTypeAlloc(IOAcceleratorES);
  if (acc && acc->init() && acc->attach(provider) && acc->start(provider)) {
    printf("ios: IOAcceleratorES registered\n");
  } else {
    printf("ios: IOAcceleratorES attach failed\n");
    if (acc) {
      acc->release();
    }
  }
}

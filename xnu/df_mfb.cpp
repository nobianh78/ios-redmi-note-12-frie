
/* fix-xcode16 P6: probe IOMobileFramebuffer for fire (appended to
 * IOPlatformExpert.cpp). Not a real display driver: the class is named
 * IOMobileFramebuffer so that IOServiceMatching("IOMobileFramebuffer") from
 * IOMobileFramebuffer.framework finds it. Every user-client call is logged
 * ("mfb sel ...") and answered with success and zeroed output, so the log shows
 * which selectors backboardd/QuartzCore need. clientMemoryForType hands out the
 * MT6768 framebuffer the df console draws into. */
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOWorkLoop.h>
#include "df_apple_logo.h"

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
/* Physical LCD panel buffer is 1080x2400 (top of 1080x2460 display);
 * iOS renders natively at 1080x2400 @3x (= 360x800 pt, razor-sharp 1:1 pixels) with
 * stride 1088 px (4352 B = 68 * 64 B, natively 64-byte aligned for Mali-G52!). */
#define DF_LCD_W 1080
#define DF_LCD_H 2400
#define DF_MFB_W 1080
#define DF_MFB_H 2400
#define DF_MFB_STRIDE_PX 1088   /* 4352 B = 68 * 64 B: natively 64-byte aligned for Mali-G52! */
#define DF_L0_PHYS 0x7c430000ULL
#define DF_L0_STRIDE 4352
#define DF_L0_H 2460

extern "C" volatile int df_mali_auto_arm;

static mach_vm_address_t g_user_fb_addr = 0;
static IOBufferMemoryDescriptor *g_sw_fb_md = NULL;
static uint32_t *g_sw_fb_kva = NULL;

#define DF_IOS_MAX_SURFACES 64
#define DF_IOS_MAX_POOL_BYTES (128u * 1024u * 1024u)

struct DFIOSurfaceSlot {
  bool in_use;
  uint32_t width;
  uint32_t height;
  uint32_t bytes_per_row;
  uint32_t bytes_per_elem;
  uint32_t pixel_format;
  uint32_t alloc_size;
  uint32_t seed;
  int32_t refcnt;
  IOBufferMemoryDescriptor *md;
  uint32_t *kva;
};

static DFIOSurfaceSlot g_ios_surfaces[DF_IOS_MAX_SURFACES];
static IOLock *g_ios_lock = NULL;
static uint64_t g_ios_pool_bytes = 0;

static volatile uint32_t *df_l0va = NULL;
static volatile int g_boot_logo_done = 0;
static uint32_t g_boot_bar_ticks = 0;

/* Apple iOS 16 wallpaper (SydneyWallpaper.jpg), 540x1200 BGRA, made by tools/make_wallpaper.py.
 * PosterBoard needs Metal, so SpringBoard draws pure black where the wallpaper should be; SwapEnd
 * shows this image under every pure-black pixel instead. fix-xcode16.sh fills in the path. */
#define DF_WALL_W 544   /* 2176 B rows: 64-byte aligned for the Mali */
#define DF_WALL_H 1200
__asm__(".section __TEXT,__const\n"
        ".p2align 14\n"   /* page aligned: the Mali maps it with 4K PTEs from page-truncated PAs */
        ".globl _df_wall_px\n"
        "_df_wall_px:\n"
        ".incbin \"@DF_WALL_PATH@\"\n"
        ".text\n");
extern "C" const uint32_t df_wall_px[DF_WALL_W * DF_WALL_H];
/* xnu/df_mali.c: GPU compositor; returns 0 when the Mali drew the frame, -1 -> CPU path */
extern "C" int df_mali_compose(const uint32_t *swfb);

static void df_flush_fb(void) {
  if (df_wdt) {
    df_wdt[0x08 / 4] = 0x1971; /* WDT_RST reload */
  }
  if (!df_fbva)
    return;
  for (uintptr_t a = (uintptr_t)df_fbva;
       a < (uintptr_t)df_fbva + (uintptr_t)DF_LCD_W * DF_LCD_H * 4; a += 64) {
    __asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
  }
  __asm__ volatile("dsb sy" ::: "memory");
}

static void df_clear_l0(void) {
  size_t bytes = (size_t)DF_L0_STRIDE * DF_L0_H;
  if (!df_l0va) {
    df_l0va = (volatile uint32_t *)ml_io_map_unmappable(DF_L0_PHYS,
                                                        (vm_size_t)bytes);
  }
  if (df_l0va) {
    size_t words = bytes / 4;
    for (size_t i = 0; i < words; i++) {
      df_l0va[i] = 0xff000000U;
    }
    for (uintptr_t a = (uintptr_t)df_l0va; a < (uintptr_t)df_l0va + bytes;
         a += 64) {
      __asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
    }
  }
  /* Also clear bottom 60 rows (2400..2459) of OVL0 L3 (1080x2460) so no blue strip remains */
  size_t tail_bytes = (size_t)DF_LCD_W * (DF_L0_H - DF_LCD_H) * 4;
  static volatile uint32_t *df_l3_tail = NULL;
  if (!df_l3_tail) {
    df_l3_tail = (volatile uint32_t *)ml_io_map_unmappable(
        DF_MFB_PHYS + (vm_offset_t)DF_LCD_W * DF_LCD_H * 4,
        (vm_size_t)tail_bytes);
  }
  if (df_l3_tail) {
    size_t tw = tail_bytes / 4;
    for (size_t i = 0; i < tw; i++) {
      df_l3_tail[i] = 0xff000000U;
    }
    for (uintptr_t a = (uintptr_t)df_l3_tail;
         a < (uintptr_t)df_l3_tail + tail_bytes; a += 64) {
      __asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
    }
  }
  __asm__ volatile("dsb sy" ::: "memory");
  if (df_wdt) {
    df_wdt[0x08 / 4] = 0x1971;
  }
}

#define DF_BAR_X 330
#define DF_BAR_Y 1480
#define DF_BAR_W 420
#define DF_BAR_H 8

static void df_draw_boot_bar(uint32_t fill_w, uint32_t pulse_x) {
  if (!df_fbva || g_boot_logo_done)
    return;
  if (fill_w > DF_BAR_W)
    fill_w = DF_BAR_W;
  for (uint32_t dy = 0; dy < DF_BAR_H; dy++) {
    volatile uint32_t *row = &df_fbva[(DF_BAR_Y + dy) * DF_LCD_W + DF_BAR_X];
    int edge = (dy == 0 || dy == DF_BAR_H - 1);
    for (uint32_t dx = 0; dx < DF_BAR_W; dx++) {
      if (edge && (dx < 2 || dx >= DF_BAR_W - 2)) {
        row[dx] = 0xff000000U;
      } else if (dx < fill_w) {
        row[dx] = 0xffffffffU;
      } else if (dx >= pulse_x && dx < pulse_x + 36) {
        row[dx] = 0xff707070U;
      } else {
        row[dx] = 0xff2c2c2eU;
      }
    }
    for (uintptr_t a = (uintptr_t)row; a < (uintptr_t)(row + DF_BAR_W);
         a += 64) {
      __asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
    }
  }
  __asm__ volatile("dsb sy" ::: "memory");
}

static void df_draw_apple_logo(void) {
  if (!df_fbva)
    return;
  const uint32_t x0 = (DF_LCD_W - DF_LOGO_W) / 2;
  const uint32_t y0 = (DF_LCD_H - DF_LOGO_H) / 2 - 40;
  uint32_t idx = 0;
  const uint32_t total = (uint32_t)DF_LOGO_W * DF_LOGO_H;
  for (uint32_t k = 0; k + 1 < DF_LOGO_RLE_LEN && idx < total; k += 2) {
    uint32_t run = df_apple_logo_rle[k];
    uint32_t v = df_apple_logo_rle[k + 1];
    if (v == 0) {
      idx += run;
      continue;
    }
    uint32_t col = 0xff000000U | (v << 16) | (v << 8) | v;
    while (run-- && idx < total) {
      uint32_t lx = idx % DF_LOGO_W;
      uint32_t ly = idx / DF_LOGO_W;
      df_fbva[(y0 + ly) * DF_LCD_W + (x0 + lx)] = col;
      idx++;
    }
  }
  df_draw_boot_bar(12, 0);
}

static void df_clear_fb(void) {
  df_clear_l0();
  if (!df_fbva)
    return;
  for (size_t i = 0; i < (size_t)DF_LCD_W * DF_LCD_H; i++) {
    df_fbva[i] = 0xff000000;
  }
  if (!g_boot_logo_done) {
    df_draw_apple_logo();
  }
  df_flush_fb();
}

class IOMobileFramebuffer : public IOService {
  OSDeclareDefaultStructors(IOMobileFramebuffer);

  IOWorkLoop *fBootWL;
  IOTimerEventSource *fWdtTimer;
  static void wdtTimerAction(OSObject *owner, IOTimerEventSource *sender);

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

void IOMobileFramebuffer::wdtTimerAction(OSObject *owner,
                                         IOTimerEventSource *sender) {
  if (df_wdt) {
    df_wdt[0x08 / 4] = 0x1971; /* WDT_RST reload */
  }
  if (df_gui_active && !g_boot_logo_done && df_fbva) {
    g_boot_bar_ticks++;
    uint32_t fill = 12 + (g_boot_bar_ticks * (DF_BAR_W - 28)) / 360;
    if (fill > DF_BAR_W - 16)
      fill = DF_BAR_W - 16;
    uint32_t pulse = (g_boot_bar_ticks * 14) % (DF_BAR_W - 36);
    df_draw_boot_bar(fill, pulse);
  }
  if (sender) {
    sender->setTimeoutMS(500);
  }
}

bool IOMobileFramebuffer::start(IOService *provider) {
  if (!IOService::start(provider)) {
    return false;
  }
  fBootWL = IOWorkLoop::workLoop();
  fWdtTimer = NULL;
  if (fBootWL) {
    fWdtTimer = IOTimerEventSource::timerEventSource(
        this, &IOMobileFramebuffer::wdtTimerAction);
    if (fWdtTimer) {
      fBootWL->addEventSource(fWdtTimer);
      fWdtTimer->setTimeoutMS(500);
    }
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
  if (!g_sw_fb_md) {
    g_sw_fb_md = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared,
        (vm_size_t)DF_MFB_STRIDE_PX * DF_MFB_H * 4, page_size);
    if (g_sw_fb_md) {
      g_sw_fb_kva = (uint32_t *)g_sw_fb_md->getBytesNoCopy();
      if (g_sw_fb_kva) {
        bzero(g_sw_fb_kva, (size_t)DF_MFB_STRIDE_PX * DF_MFB_H * 4);
      }
      printf("mfb: pre-allocated SW RAM fb kva=%p\n", (void *)g_sw_fb_kva);
    }
  }
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
      fVsyncTimer->setTimeoutMS(50);
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
      if (uc->fVsyncCount <= 3) {
        printf("mfb: vsync #%llu sel=%u sendAsync -> 0x%x\n",
               (unsigned long long)uc->fVsyncCount, uc->fAsync[i].selector, r);
      }
    }
  }
  if (uc->fNotifyPort != MACH_PORT_NULL) {
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
    if (++s_notify_cnt <= 3) {
      printf("mfb: notify type=3 #%u -> 0x%x\n", s_notify_cnt, (unsigned)mr);
    }
  }
  if (sender) {
    sender->setTimeoutMS(g_boot_logo_done ? 16 : 50);
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
  if (df_mfb_logs++ < 60) {
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
      m[3] = DF_MFB_STRIDE_PX * 4;   /* 4352 bytes per row (64B aligned for the Mali) */
      m[4] = 32;             /* 32 bpp */
      m[5] = 0x42475241;     /* 'BGRA' */
    }
    printf("mfb: sel 74 (display mode) -> %dx%d@60\n", DF_MFB_W, DF_MFB_H);
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
          DF_MFB_PHYS, (vm_size_t)DF_LCD_W * DF_LCD_H * 4);
    }
    s_swap_cnt++;
    uint32_t s0 = 0;
    if (a->structureInput && a->structureInputSize >= 0x60) {
      s0 = *(uint32_t *)((uint8_t *)a->structureInput + 0x54);
    }
    const uint32_t *present_kva = g_sw_fb_kva;
    if (s0 >= 1 && s0 < DF_IOS_MAX_SURFACES && g_ios_surfaces[s0].in_use &&
        g_ios_surfaces[s0].kva &&
        g_ios_surfaces[s0].width == DF_MFB_W &&
        g_ios_surfaces[s0].height == DF_MFB_H &&
        g_ios_surfaces[s0].bytes_per_row == DF_MFB_STRIDE_PX * 4) {
      present_kva = g_ios_surfaces[s0].kva;
    }
    uint32_t nonblack = 0;
    int gpu_done = 0;
    uint32_t nonzero = 0;
    if (present_kva && df_fbva) {
      size_t n = (size_t)DF_MFB_W * DF_MFB_H;
      if (!g_boot_logo_done || s_swap_cnt <= 10) {
        for (size_t i = 0; i < n; i++) {
          uint32_t px = present_kva[i];
          if (px != 0) {
            nonzero++;
            if ((px & 0x00ffffffU) != 0) {
              nonblack++;
            }
          }
        }
        if (nonblack > 2000) {
          g_boot_logo_done = 1;
          if (!df_mali_auto_arm) {
            df_mali_auto_arm = 1; /* auto-probe & arm Mali-G52 GPU compositor */
          }
        }
      }
      if (g_boot_logo_done && df_mali_compose(present_kva) == 0) {
        gpu_done = 1;   /* Mali-G52 composited native 1080x2400 (stride 4352 B) straight into LCD */
      } else if (g_boot_logo_done) {
        /* CPU fallback before Mali auto-arms: native 1:1 1080x2400 + true premultiplied alpha */
        volatile uint32_t *dst = df_fbva;
        for (uint32_t y = 0; y < DF_LCD_H; y++) {
          const uint32_t *srow = present_kva + (size_t)y * DF_MFB_STRIDE_PX;
          const uint32_t *wrow = &df_wall_px[(y >> 1) * DF_WALL_W];
          for (uint32_t x = 0; x < DF_LCD_W; x++) {
            uint32_t px = srow[x];
            uint32_t a_ch = (px >> 24) & 0xffu;
            if (a_ch < 255u) {
              uint32_t w = wrow[x >> 1], k = 255u - a_ch;
              uint32_t r = ((px >> 16) & 0xffu) + ((((w >> 16) & 0xffu) * k) >> 8);
              uint32_t g = ((px >> 8) & 0xffu) + ((((w >> 8) & 0xffu) * k) >> 8);
              uint32_t b = (px & 0xffu) + (((w & 0xffu) * k) >> 8);
              if (r > 255u) r = 255u;
              if (g > 255u) g = 255u;
              if (b > 255u) b = 255u;
              px = (r << 16) | (g << 8) | b;
            }
            *dst++ = px | 0xff000000U;
          }
        }
      }
    }
    if (a->structureInput && a->structureInputSize >= 0x60) {
      static uint32_t s_last_nb = 0;
      static uint32_t s_nb_logs = 0;
      int diff = (int)nonblack - (int)s_last_nb;
      if (diff < 0) diff = -diff;
      if (s_swap_cnt <= 10 || (diff > 5000 && s_nb_logs++ < 30)) {
        s_last_nb = nonblack;
        printf("mfb: SwapEnd #%u sid=%u nz=%u nb=%u [0]=0x%x [mid]=0x%x\n",
               s_swap_cnt, s0, nonzero, nonblack,
               present_kva ? present_kva[0] : 0,
               present_kva ? present_kva[(DF_MFB_W * DF_MFB_H) / 2] : 0);
      }
    }
    if (g_boot_logo_done && !gpu_done) {
      df_flush_fb();
    }
  }
  return kIOReturnSuccess;
}

IOReturn
DFMobileFramebufferUC::clientMemoryForType(UInt32 type, IOOptionBits *options,
                                           IOMemoryDescriptor **memory) {
  printf("mfb: clientMemoryForType %u\n", (unsigned)type);
  *memory = IOMemoryDescriptor::withPhysicalAddress(
      DF_MFB_PHYS, (IOByteCount)DF_LCD_W * DF_LCD_H * 4, kIODirectionInOut);
  return *memory ? kIOReturnSuccess : kIOReturnNoMemory;
}

IOReturn DFMobileFramebufferUC::clientClose(void) {
  terminate();
  return kIOReturnSuccess;
}

/* Multi-surface IOCoreSurfaceRoot (IOSurface.framework's kernel service on iOS).
 * Slot 1 is the primary 1080x2400 display framebuffer (g_sw_fb_md).
 * Slots 2..63 allocate independent cacheable RAM buffers for offscreen groups,
 * backdrops, double-buffering, and App Switcher cards so backboardd never aliases
 * an offscreen render target onto the live display framebuffer. */
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

  IOMemoryMap *fCommMap;
  mach_vm_address_t fUserAddr;
  IOMemoryMap *fMaps[DF_IOS_MAX_SURFACES];
  mach_vm_address_t fMapAddr[DF_IOS_MAX_SURFACES];
  uint32_t fMapRef[DF_IOS_MAX_SURFACES];
  IOExternalTrap fTraps[6];

  IOReturn ensureCommMapped(void);
  IOReturn mapSlotLocked(uint32_t sid, mach_vm_address_t *out_va);
  void releaseSlotLocked(uint32_t sid);

public:
  virtual bool initWithTask(task_t owningTask, void *securityID,
                            UInt32 type) APPLE_KEXT_OVERRIDE;
  virtual IOReturn externalMethod(uint32_t selector,
                                  IOExternalMethodArguments *args,
                                  IOExternalMethodDispatch *dispatch,
                                  OSObject *target,
                                  void *reference) APPLE_KEXT_OVERRIDE;
  virtual IOExternalTrap *
  getTargetAndTrapForIndex(IOService **targetP,
                           UInt32 index) APPLE_KEXT_OVERRIDE;
  IOReturn trapReleaseNoop(void *p1, void *p2, void *p3, void *p4, void *p5,
                           void *p6);
  IOReturn trapReleaseSurface(void *p1, void *p2, void *p3, void *p4, void *p5,
                              void *p6);
  virtual IOReturn
  clientMemoryForType(UInt32 type, IOOptionBits *options,
                      IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;
  virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOCoreSurfaceRoot, IOService);
OSDefineMetaClassAndStructors(DFCoreSurfaceUC, IOUserClient);

static unsigned df_ios_logs;
static uint8_t s_iosurface_comm[16384] __attribute__((aligned(16384)));

static bool df_ios_key_eq(const uint8_t *s, uint32_t slen, const char *lit) {
  while (slen > 0 && s[slen - 1] == 0) {
    slen--;
  }
  size_t llen = strlen(lit);
  return slen == llen && memcmp(s, lit, llen) == 0;
}

static void df_ios_parse_props(const uint8_t *buf, size_t len, uint32_t *out_w,
                               uint32_t *out_h, uint32_t *out_bpr,
                               uint32_t *out_bpe, uint32_t *out_fmt,
                               uint32_t *out_alloc) {
  if (!buf || len < 8) {
    return;
  }
  uint32_t magic = 0;
  memcpy(&magic, buf, 4);
  if (magic != 0x000000d3U) {
    return;
  }
  size_t pos = 4;
  while (pos + 4 <= len) {
    uint32_t hdr = 0;
    memcpy(&hdr, buf + pos, 4);
    pos += 4;
    uint32_t type = hdr & 0x7f000000U;
    uint32_t slen = hdr & 0x00ffffffU;
    if (type == 0x04000000U) { /* kOSSerializeNumber */
      if (pos + 8 > len) {
        break;
      }
      pos += 8;
    } else if (type == 0x08000000U || type == 0x09000000U) { /* Symbol/String */
      size_t padded = ((size_t)slen + 3U) & ~3UL;
      if (padded > len - pos) {
        break;
      }
      const uint8_t *kptr = buf + pos;
      pos += padded;
      if (pos + 12 <= len) {
        uint32_t vhdr = 0;
        memcpy(&vhdr, buf + pos, 4);
        if ((vhdr & 0x7f000000U) == 0x04000000U) {
          uint64_t val = 0;
          memcpy(&val, buf + pos + 4, 8);
          pos += 12;
          if (df_ios_key_eq(kptr, slen, "IOSurfaceWidth")) {
            *out_w = (uint32_t)val;
          } else if (df_ios_key_eq(kptr, slen, "IOSurfaceHeight")) {
            *out_h = (uint32_t)val;
          } else if (df_ios_key_eq(kptr, slen, "IOSurfaceBytesPerRow")) {
            *out_bpr = (uint32_t)val;
          } else if (df_ios_key_eq(kptr, slen, "IOSurfaceBytesPerElement")) {
            *out_bpe = (uint32_t)val;
          } else if (df_ios_key_eq(kptr, slen, "IOSurfacePixelFormat")) {
            *out_fmt = (uint32_t)val;
          } else if (df_ios_key_eq(kptr, slen, "IOSurfaceAllocSize")) {
            *out_alloc = (uint32_t)val;
          }
        }
      }
    } else if (type == 0x0a000000U) { /* kOSSerializeData */
      size_t padded = ((size_t)slen + 3U) & ~3UL;
      if (padded > len - pos) {
        break;
      }
      pos += padded;
    }
  }
}

static bool df_ios_ensure_slot1_locked(void) {
  if (g_ios_surfaces[1].in_use && g_ios_surfaces[1].md) {
    return true;
  }
  uint32_t sz = (uint32_t)DF_MFB_STRIDE_PX * DF_MFB_H * 4;
  if (!g_sw_fb_md) {
    g_sw_fb_md = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared,
        (vm_size_t)sz, page_size);
    if (!g_sw_fb_md) {
      return false;
    }
    g_sw_fb_kva = (uint32_t *)g_sw_fb_md->getBytesNoCopy();
    if (g_sw_fb_kva) {
      bzero(g_sw_fb_kva, sz);
    }
    printf("ios: allocated SW RAM fb kva=%p\n", (void *)g_sw_fb_kva);
  }
  DFIOSurfaceSlot *s = &g_ios_surfaces[1];
  s->in_use = true;
  s->width = DF_MFB_W;
  s->height = DF_MFB_H;
  s->bytes_per_row = DF_MFB_STRIDE_PX * 4;
  s->bytes_per_elem = 4;
  s->pixel_format = 0x42475241; /* 'BGRA' */
  s->alloc_size = sz;
  s->seed = 1;
  s->refcnt = 1;
  s->md = g_sw_fb_md;
  s->kva = g_sw_fb_kva;
  *(uint32_t *)(s_iosurface_comm + 256 + 0x0c) = 1;
  *(uint32_t *)(s_iosurface_comm + 256 + 0x10) = 1;
  return true;
}

static void df_ios_fill_out(uint8_t *out, uint32_t sid,
                            const DFIOSurfaceSlot *s,
                            mach_vm_address_t user_fb_addr,
                            mach_vm_address_t user_comm_base) {
  /* IOSurfaceClient copies out+0x00.. to client+0x68..:
   *   out+0x00 -> client+0x68: BaseAddress
   *   out+0x08 -> client+0x70: mapped comm page (seed @ +0x0c / +0x10)
   *   out+0x18 -> client+0x80: surface_id
   *   out+0x1c -> client+0x84: alloc_size
   *   out+0x20 -> client+0x88: width
   *   out+0x24 -> client+0x8c: height
   *   out+0x28 -> client+0x90: bytes_per_row
   *   out+0x2c -> client+0x94: offset (0)
   *   out+0x30 -> client+0x98: pixel_format
   *   out+0x38 -> client+0xa0: plane_count (0 = single-plane)
   *   out+0x40 -> client+0xa8: bytes_per_element (uint16)
   *   out+0x42 -> client+0xaa: element_width (uint8)
   *   out+0x43 -> client+0xab: element_height (uint8) */
  *(uint64_t *)(out + 0x00) = (uint64_t)user_fb_addr;
  *(uint64_t *)(out + 0x08) = (uint64_t)(user_comm_base + (uint64_t)sid * 256U);
  *(uint64_t *)(out + 0x10) = 0;
  *(uint32_t *)(out + 0x18) = sid;
  *(uint32_t *)(out + 0x1c) = s->alloc_size;
  *(uint32_t *)(out + 0x20) = s->width;
  *(uint32_t *)(out + 0x24) = s->height;
  *(uint32_t *)(out + 0x28) = s->bytes_per_row;
  *(uint32_t *)(out + 0x2c) = 0;
  *(uint32_t *)(out + 0x30) = s->pixel_format;
  *(uint32_t *)(out + 0x34) = 0;
  *(uint32_t *)(out + 0x38) = 0;
  *(uint16_t *)(out + 0x40) = (uint16_t)s->bytes_per_elem;
  *(uint8_t *)(out + 0x42) = 1;
  *(uint8_t *)(out + 0x43) = 1;
  *(uint32_t *)(out + 0xdc) = 0;
  *(uint32_t *)(out + 0x124) = 0;
}

bool DFCoreSurfaceUC::initWithTask(task_t owningTask, void *securityID,
                                   UInt32 type) {
  if (!IOUserClient::initWithTask(owningTask, securityID, type))
    return false;
  fTask = owningTask;
  fCommMap = NULL;
  fUserAddr = 0;
  bzero(fMaps, sizeof(fMaps));
  bzero(fMapAddr, sizeof(fMapAddr));
  bzero(fMapRef, sizeof(fMapRef));
  bzero(fTraps, sizeof(fTraps));
  fTraps[4].object = this;
  fTraps[4].func =
      OSMemberFunctionCast(IOTrap, this, &DFCoreSurfaceUC::trapReleaseNoop);
  fTraps[5].object = this;
  fTraps[5].func =
      OSMemberFunctionCast(IOTrap, this, &DFCoreSurfaceUC::trapReleaseSurface);
  return true;
}

IOReturn DFCoreSurfaceUC::ensureCommMapped(void) {
  if (fUserAddr) {
    return kIOReturnSuccess;
  }
  task_t t = fTask ? fTask : current_task();
  IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange(
      (mach_vm_address_t)s_iosurface_comm, sizeof(s_iosurface_comm),
      kIODirectionInOut, kernel_task);
  if (md) {
    fCommMap = md->createMappingInTask(t, 0, kIOMapAnywhere);
    if (fCommMap) {
      fUserAddr = fCommMap->getAddress();
    }
    md->release();
  }
  if (!fUserAddr) {
    vm_map_t map = t ? get_task_map(t) : NULL;
    if (map) {
      mach_vm_allocate(map, &fUserAddr, sizeof(s_iosurface_comm), 1);
    }
  }
  return fUserAddr ? kIOReturnSuccess : kIOReturnNoMemory;
}

IOReturn DFCoreSurfaceUC::mapSlotLocked(uint32_t sid,
                                        mach_vm_address_t *out_va) {
  if (sid == 0 || sid >= DF_IOS_MAX_SURFACES || !g_ios_surfaces[sid].in_use ||
      !g_ios_surfaces[sid].md) {
    return kIOReturnNotFound;
  }
  if (!fMapAddr[sid]) {
    task_t t = fTask ? fTask : current_task();
    IOMemoryMap *m =
        g_ios_surfaces[sid].md->createMappingInTask(t, 0, kIOMapAnywhere);
    if (!m) {
      return kIOReturnNoMemory;
    }
    fMaps[sid] = m;
    fMapAddr[sid] = m->getAddress();
  }
  fMapRef[sid]++;
  *out_va = fMapAddr[sid];
  if (sid == 1) {
    g_user_fb_addr = fMapAddr[1];
  }
  return kIOReturnSuccess;
}

void DFCoreSurfaceUC::releaseSlotLocked(uint32_t sid) {
  if (sid < 2 || sid >= DF_IOS_MAX_SURFACES) {
    return; /* never tear down primary display surface sid=1 */
  }
  if (fMapRef[sid] > 0) {
    fMapRef[sid]--;
    if (fMapRef[sid] == 0) {
      if (fMaps[sid]) {
        fMaps[sid]->release();
        fMaps[sid] = NULL;
      }
      fMapAddr[sid] = 0;
    }
  }
  DFIOSurfaceSlot *s = &g_ios_surfaces[sid];
  if (s->in_use) {
    s->refcnt--;
    if (s->refcnt <= 0) {
      if (s->md) {
        s->md->release();
      }
      if (g_ios_pool_bytes >= s->alloc_size) {
        g_ios_pool_bytes -= s->alloc_size;
      } else {
        g_ios_pool_bytes = 0;
      }
      bzero(s, sizeof(*s));
    }
  }
}

IOExternalTrap *
DFCoreSurfaceUC::getTargetAndTrapForIndex(IOService **targetP, UInt32 index) {
  if (index == 4 || index == 5) {
    *targetP = this;
    return &fTraps[index];
  }
  return IOUserClient::getTargetAndTrapForIndex(targetP, index);
}

IOReturn DFCoreSurfaceUC::trapReleaseNoop(void *p1, void *p2, void *p3,
                                          void *p4, void *p5, void *p6) {
  (void)p1;
  (void)p2;
  (void)p3;
  (void)p4;
  (void)p5;
  (void)p6;
  return kIOReturnSuccess;
}

IOReturn DFCoreSurfaceUC::trapReleaseSurface(void *p1, void *p2, void *p3,
                                             void *p4, void *p5, void *p6) {
  (void)p2;
  (void)p3;
  (void)p4;
  (void)p5;
  (void)p6;
  uint32_t sid = (uint32_t)(uintptr_t)p1;
  if (g_ios_lock) {
    IOLockLock(g_ios_lock);
    releaseSlotLocked(sid);
    IOLockUnlock(g_ios_lock);
  }
  return kIOReturnSuccess;
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
  if (df_ios_logs++ < 40) {
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
  if ((selector == 0 || selector == 4 || selector == 34 ||
       a->structureOutputSize >= 0xa68) &&
      a->structureOutput && a->structureOutputSize >= 0xa68) {
    /* Only backboardd allocates/looks up IOSurfaces here; other processes
     * (SpringBoard, SBRendererService, Preferences) stay cleanly on CPU
     * CGImage / CA::Render::Shmem without needing cross-process Mach ports. */
    const char *pname = NULL;
    if (kernproc != NULL && current_task() != kernel_task) {
      proc_t curp = current_proc();
      if (curp != NULL && curp != kernproc) {
        pname = proc_best_name(curp);
      }
    }
    if (!pname || strcmp(pname, "backboardd") != 0) {
      static unsigned s_rej_logs = 0;
      if (s_rej_logs++ < 10) {
        printf("ios: reject sel %u from %s (pid %d)\n", selector,
               pname ? pname : "?", proc_selfpid());
      }
      return kIOReturnUnsupported;
    }
    if (!g_ios_lock) {
      g_ios_lock = IOLockAlloc();
    }
    IOLockLock(g_ios_lock);
    IOReturn kr = ensureCommMapped();
    if (kr != kIOReturnSuccess) {
      IOLockUnlock(g_ios_lock);
      return kr;
    }
    uint32_t target_sid = 0;
    if (selector == 0) {
      /* Create a new independent IOSurface */
      uint32_t req_w = 0, req_h = 0, req_bpr = 0, req_bpe = 0;
      uint32_t req_fmt = 0x42475241; /* 'BGRA' */
      uint32_t req_alloc = 0;
      if (a->structureInput && a->structureInputSize > 0) {
        df_ios_parse_props((const uint8_t *)a->structureInput,
                           a->structureInputSize, &req_w, &req_h, &req_bpr,
                           &req_bpe, &req_fmt, &req_alloc);
      }
      if (req_w == 0 || req_w > 8192) {
        req_w = DF_MFB_W;
      }
      if (req_h == 0 || req_h > 8192) {
        req_h = DF_MFB_H;
      }
      if (req_bpe == 0 || req_bpe > 16) {
        if (req_fmt == 0x4c303038U || req_fmt == 0x41303038U) { /* L008/A008 */
          req_bpe = 1;
        } else if (req_fmt == 0x4c353635U || req_fmt == 0x4c353535U) {
          req_bpe = 2;
        } else if (req_fmt == 0x62363461U || req_fmt == 0x52476841U) {
          req_bpe = 8;
        } else {
          req_bpe = 4;
        }
      }
      uint32_t min_bpr = req_w * req_bpe;
      if (req_bpr < min_bpr) {
        req_bpr = min_bpr;
      }
      req_bpr = (req_bpr + 63U) & ~63U; /* 64-byte row alignment */
      uint64_t calc_alloc = (uint64_t)req_bpr * (uint64_t)req_h;
      if (req_alloc < calc_alloc) {
        req_alloc = (uint32_t)calc_alloc;
      }
      if (req_alloc == 0 || req_alloc > (32u * 1024u * 1024u)) {
        IOLockUnlock(g_ios_lock);
        return kIOReturnNoMemory;
      }
      req_alloc = (uint32_t)(((uint64_t)req_alloc + page_size - 1U) &
                             ~((uint64_t)page_size - 1U));

      if (!g_ios_surfaces[1].in_use && req_w == DF_MFB_W && req_h == DF_MFB_H) {
        if (!df_ios_ensure_slot1_locked()) {
          IOLockUnlock(g_ios_lock);
          return kIOReturnNoMemory;
        }
        target_sid = 1;
      } else {
        for (uint32_t i = 2; i < DF_IOS_MAX_SURFACES; i++) {
          if (!g_ios_surfaces[i].in_use) {
            target_sid = i;
            break;
          }
        }
        if (target_sid == 0 ||
            g_ios_pool_bytes + req_alloc > DF_IOS_MAX_POOL_BYTES) {
          printf("ios: pool exhausted (sid=%u used=%llu req=%u)\n", target_sid,
                 (unsigned long long)g_ios_pool_bytes, req_alloc);
          IOLockUnlock(g_ios_lock);
          return kIOReturnNoMemory;
        }
        IOBufferMemoryDescriptor *md =
            IOBufferMemoryDescriptor::inTaskWithOptions(
                kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared,
                (vm_size_t)req_alloc, page_size);
        if (!md) {
          IOLockUnlock(g_ios_lock);
          return kIOReturnNoMemory;
        }
        uint32_t *kva = (uint32_t *)md->getBytesNoCopy();
        if (kva) {
          bzero(kva, req_alloc);
        }
        DFIOSurfaceSlot *s = &g_ios_surfaces[target_sid];
        s->in_use = true;
        s->width = req_w;
        s->height = req_h;
        s->bytes_per_row = req_bpr;
        s->bytes_per_elem = req_bpe;
        s->pixel_format = req_fmt;
        s->alloc_size = req_alloc;
        s->seed = 1;
        s->refcnt = 1;
        s->md = md;
        s->kva = kva;
        g_ios_pool_bytes += req_alloc;
        *(uint32_t *)(s_iosurface_comm + target_sid * 256 + 0x0c) = 1;
        *(uint32_t *)(s_iosurface_comm + target_sid * 256 + 0x10) = 1;
      }
    } else {
      /* Lookup existing surface by id (sel 4 or sel 34) */
      target_sid = (a->scalarInputCount >= 1 && a->scalarInput[0] != 0)
                       ? (uint32_t)a->scalarInput[0]
                       : 1;
      if (target_sid == 1 && !g_ios_surfaces[1].in_use) {
        if (!df_ios_ensure_slot1_locked()) {
          IOLockUnlock(g_ios_lock);
          return kIOReturnNoMemory;
        }
      }
      if (target_sid == 0 || target_sid >= DF_IOS_MAX_SURFACES ||
          !g_ios_surfaces[target_sid].in_use) {
        IOLockUnlock(g_ios_lock);
        return kIOReturnNotFound;
      }
      g_ios_surfaces[target_sid].refcnt++;
    }

    mach_vm_address_t user_fb_addr = 0;
    kr = mapSlotLocked(target_sid, &user_fb_addr);
    if (kr != kIOReturnSuccess) {
      if (target_sid >= 2) {
        releaseSlotLocked(target_sid);
      }
      IOLockUnlock(g_ios_lock);
      return kr;
    }
    DFIOSurfaceSlot *s = &g_ios_surfaces[target_sid];
    df_ios_fill_out((uint8_t *)a->structureOutput, target_sid, s, user_fb_addr,
                    fUserAddr);
    if (a->scalarOutputCount >= 1) {
      a->scalarOutput[0] = target_sid;
    }
    static unsigned s_create_logs = 0;
    if (s_create_logs++ < 40) {
      printf("ios: sel %u -> id %u (%ux%u bpr=%u bpe=%u fmt=0x%x sz=%u) "
             "fb=0x%llx\n",
             selector, target_sid, s->width, s->height, s->bytes_per_row,
             s->bytes_per_elem, s->pixel_format, s->alloc_size,
             (uint64_t)user_fb_addr);
    }
    IOLockUnlock(g_ios_lock);
  }
  if (selector == 9) {
    uint32_t sid = 1;
    if (a->structureInput && a->structureInputSize >= 4) {
      uint32_t in_sid = 0;
      memcpy(&in_sid, a->structureInput, 4);
      if (in_sid >= 1 && in_sid < DF_IOS_MAX_SURFACES) {
        sid = in_sid;
      }
    }
    uint32_t seed = (g_ios_surfaces[sid].seed + 2U) | 3U;
    g_ios_surfaces[sid].seed = seed;
    *(uint32_t *)(s_iosurface_comm + sid * 256 + 0x0c) = seed;
    *(uint32_t *)(s_iosurface_comm + sid * 256 + 0x10) = seed;
    if (a->structureOutput && a->structureOutputSize >= 4) {
      *(uint32_t *)a->structureOutput = seed;
    }
    if (df_ios_logs <= 40) {
      printf("ios: sel 9 (set_value) sid=%u -> seed=%u\n", sid, seed);
    }
  }
  if (selector == 27 && df_ios_logs <= 40) {
    printf("ios: sel 27 (set_bulk_attachments)\n");
  }
  return kIOReturnSuccess;
}

IOReturn DFCoreSurfaceUC::clientMemoryForType(UInt32 type,
                                              IOOptionBits *options,
                                              IOMemoryDescriptor **memory) {
  printf("ios: clientMemoryForType %u\n", (unsigned)type);
  *memory = IOMemoryDescriptor::withPhysicalAddress(
      DF_MFB_PHYS, (IOByteCount)DF_LCD_W * DF_LCD_H * 4, kIODirectionInOut);
  return *memory ? kIOReturnSuccess : kIOReturnNoMemory;
}

IOReturn DFCoreSurfaceUC::clientClose(void) {
  if (g_ios_lock) {
    IOLockLock(g_ios_lock);
    for (uint32_t i = 1; i < DF_IOS_MAX_SURFACES; i++) {
      while (fMapRef[i] > 0) {
        if (i == 1) {
          fMapRef[1] = 0;
          if (fMaps[1]) {
            fMaps[1]->release();
            fMaps[1] = NULL;
          }
          fMapAddr[1] = 0;
        } else {
          releaseSlotLocked(i);
        }
      }
    }
    if (fCommMap) {
      fCommMap->release();
      fCommMap = NULL;
    }
    fUserAddr = 0;
    IOLockUnlock(g_ios_lock);
  }
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

extern "C" void df_hid_attach(IOService *provider);

extern "C" void df_mfb_attach(IOService *provider) {
  if (!g_ios_lock) {
    g_ios_lock = IOLockAlloc();
  }
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
  df_hid_attach(provider);
}

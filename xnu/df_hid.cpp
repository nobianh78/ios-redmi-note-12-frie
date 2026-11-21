
/* DarwinFire IOHIDEventService + UserClient (appended to IOPlatformExpert.cpp
 * after df_mfb.cpp). Exposes an iOS 16 compatible multi-touch digitizer service
 * to backboardd's IOHIDEventServicePlugin.plugin via IOSharedDataQueue. */
#include <IOKit/IOLocks.h>
#include <IOKit/IOSharedDataQueue.h>

struct df_touch_pt {
  uint8_t id;         /* finger id 0..9 */
  uint8_t in_range;   /* 1 while present, 0 on lift */
  uint8_t is_touch;   /* 1 while touching, 0 on lift */
  uint8_t event_mask; /* 1=Range, 2=Touch, 4=Position */
  int32_t x_fixed;    /* 16.16 IOFixed normalized 0..65536 */
  int32_t y_fixed;    /* 16.16 IOFixed normalized 0..65536 */
};

/* Exact iOS 16.7.16 wire format verified against _IOHIDEventCreateWithBytes
 * (IOKit.framework @ 0x18e55107c) & _IOHIDEventCreateDigitizerFingerEvent. */
struct __attribute__((packed)) DFHIDQueueHeader {
  uint64_t timeStamp;       /* +0x00 */
  uint64_t senderID;        /* +0x08 */
  uint32_t options;         /* +0x10 */
  uint32_t attributeLength; /* +0x14 */
  uint32_t eventCount;      /* +0x18 */
};

struct __attribute__((packed)) DFHIDDigitizerEvent {
  uint32_t size;            /* +0x00 = 104 (0x68) */
  uint32_t type;            /* +0x04 = 11 (kIOHIDEventTypeDigitizer) */
  uint32_t options;         /* +0x08 */
  uint8_t depth;            /* +0x0c = 0 parent, 1 child */
  uint8_t reserved[3];      /* +0x0d */
  int32_t x;                /* +0x10 (16.16 IOFixed 0..65536) */
  int32_t y;                /* +0x14 (16.16 IOFixed 0..65536) */
  int32_t z;                /* +0x18 */
  uint32_t transducerIndex; /* +0x1c */
  uint32_t transducerType;  /* +0x20 = 3 Hand, 2 Finger */
  uint32_t identity;        /* +0x24 */
  uint32_t eventMask;       /* +0x28 */
  uint32_t childEventMask;  /* +0x2c */
  uint32_t buttonMask;      /* +0x30 */
  int32_t pressure;         /* +0x34 (0x10000 = 1.0) */
  int32_t auxPressure;      /* +0x38 */
  int32_t twist;            /* +0x3c */
  uint32_t orientationType; /* +0x40 = 2 (Quality) */
  int32_t quality;          /* +0x44 */
  int32_t density;          /* +0x48 */
  int32_t irregularity;     /* +0x4c */
  int32_t majorRadius;      /* +0x50 */
  int32_t minorRadius;      /* +0x54 */
  uint32_t pad[4];          /* +0x58..0x67 */
};

static_assert(sizeof(DFHIDQueueHeader) == 28, "DFHIDQueueHeader must be 28B");
static_assert(sizeof(DFHIDDigitizerEvent) == 104,
              "DFHIDDigitizerEvent must be 104B");

#define DF_HID_OPT_ABSOLUTE 0x00000001U
#define DF_HID_OPT_COLLECTION 0x00000002U
#define DF_HID_OPT_RANGE 0x00010000U
#define DF_HID_OPT_TOUCH 0x00020000U
#define DF_HID_OPT_DISP_INT 0x00080000U
#define DF_HID_QUEUE_SIZE 16384U
#define DF_HID_MAX_CLIENTS 4

class DFHIDEventServiceUC;

class IOHIDEventService : public IOService {
  OSDeclareDefaultStructors(IOHIDEventService);

public:
  virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
  virtual IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;
  virtual IOReturn newUserClient(task_t owningTask, void *securityID,
                                 UInt32 type,
                                 IOUserClient **handler) APPLE_KEXT_OVERRIDE;
};

class DFHIDEventServiceUC : public IOUserClient {
  OSDeclareDefaultStructors(DFHIDEventServiceUC);

  IOSharedDataQueue *fQueue;
  IOMemoryDescriptor *fQueueMD;
  bool fOpened;
  bool fPortSet;

public:
  virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
  virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
  virtual IOReturn
  registerNotificationPort(mach_port_t port, UInt32 type,
                           io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
  virtual IOReturn
  clientMemoryForType(UInt32 type, IOOptionBits *options,
                      IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;
  virtual IOReturn externalMethod(uint32_t selector,
                                  IOExternalMethodArguments *args,
                                  IOExternalMethodDispatch *dispatch,
                                  OSObject *target,
                                  void *reference) APPLE_KEXT_OVERRIDE;
  virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;

  bool enqueueRaw(void *data, uint32_t dataSize);
  bool isReady(void) const { return fQueue != NULL && fPortSet; }
};

OSDefineMetaClassAndStructors(IOHIDEventService, IOService);
OSDefineMetaClassAndStructors(DFHIDEventServiceUC, IOUserClient);

static IOLock *g_hid_lock = NULL;
static DFHIDEventServiceUC *g_hid_clients[DF_HID_MAX_CLIENTS];
static uint64_t g_hid_sender_id = 0;

bool IOHIDEventService::start(IOService *provider) {
  if (!IOService::start(provider)) {
    return false;
  }
  if (!g_hid_lock) {
    g_hid_lock = IOLockAlloc();
  }
  setName("AppleMultitouchSPI");
  setProperty("IOClass", "IOHIDEventService");
  setProperty("IOUserClientClass", "DFHIDEventServiceUC");
  setProperty("HIDServiceSupport", kOSBooleanTrue);
  setProperty("Built-In", kOSBooleanTrue);
  setProperty("DisplayIntegrated", kOSBooleanTrue);
  setProperty("PrimaryUsagePage", (uint64_t)0x0D, 32);
  setProperty("PrimaryUsage", (uint64_t)0x04, 32);
  setProperty("Transport", "SPI");
  setProperty("Product", "DarwinFire FT8720 Touch");
  setProperty("Manufacturer", "FocalTech");
  setProperty("VendorID", (uint64_t)0x05AC, 32);
  setProperty("ProductID", (uint64_t)0x8720, 32);
  setProperty("ReportInterval", (uint64_t)16667, 32);
  setProperty("QueueSize", (uint64_t)DF_HID_QUEUE_SIZE, 32);
  setProperty("SensorSurfaceWidth", (uint64_t)6800, 32);
  setProperty("SensorSurfaceHeight", (uint64_t)15100, 32);
  setProperty("LocationID", (uint64_t)0x100, 32);

  /* DeviceUsagePairs = [ { DeviceUsagePage: 0x0D, DeviceUsage: 0x04 } ] */
  OSArray *pairs = OSArray::withCapacity(1);
  OSDictionary *pair = OSDictionary::withCapacity(2);
  if (pairs && pair) {
    OSNumber *up = OSNumber::withNumber((uint64_t)0x0D, 32);
    OSNumber *u = OSNumber::withNumber((uint64_t)0x04, 32);
    if (up && u) {
      pair->setObject("DeviceUsagePage", up);
      pair->setObject("DeviceUsage", u);
      pairs->setObject(pair);
      setProperty("DeviceUsagePairs", pairs);
    }
    if (up)
      up->release();
    if (u)
      u->release();
  }
  if (pair)
    pair->release();
  if (pairs)
    pairs->release();

  /* SurfaceDimensions = { Width: 6800, Height: 15100 } (1/100 mm) */
  OSDictionary *sdim = OSDictionary::withCapacity(2);
  if (sdim) {
    OSNumber *sw = OSNumber::withNumber((uint64_t)6800, 32);
    OSNumber *sh = OSNumber::withNumber((uint64_t)15100, 32);
    if (sw && sh) {
      sdim->setObject("Width", sw);
      sdim->setObject("Height", sh);
      setProperty("SurfaceDimensions", sdim);
    }
    if (sw)
      sw->release();
    if (sh)
      sh->release();
    sdim->release();
  }

  registerService();
  g_hid_sender_id = getRegistryEntryID();
  printf("hid: IOHIDEventService registered (senderID=0x%llx)\n",
         (unsigned long long)g_hid_sender_id);
  return true;
}

IOReturn IOHIDEventService::setProperties(OSObject *properties) {
  static unsigned s_prop_logs = 0;
  if (s_prop_logs++ < 10) {
    printf("hid: setProperties pid=%d\n", proc_selfpid());
  }
  (void)properties;
  return kIOReturnSuccess;
}

IOReturn IOHIDEventService::newUserClient(task_t owningTask, void *securityID,
                                          UInt32 type, IOUserClient **handler) {
  DFHIDEventServiceUC *uc = OSTypeAlloc(DFHIDEventServiceUC);
  if (!uc || !uc->initWithTask(owningTask, securityID, type) ||
      !uc->attach(this) || !uc->start(this)) {
    if (uc) {
      uc->detach(this);
      uc->release();
    }
    printf("hid: newUserClient type=0x%x failed\n", (unsigned)type);
    return kIOReturnNoMemory;
  }
  printf("hid: newUserClient type=0x%x by pid=%d\n", (unsigned)type,
         proc_selfpid());
  *handler = uc;
  return kIOReturnSuccess;
}

bool DFHIDEventServiceUC::start(IOService *provider) {
  if (!IOUserClient::start(provider)) {
    return false;
  }
  fQueue = IOSharedDataQueue::withCapacity(DF_HID_QUEUE_SIZE);
  fQueueMD = fQueue ? fQueue->getMemoryDescriptor() : NULL;
  fOpened = false;
  fPortSet = false;
  if (!fQueue || !fQueueMD) {
    printf("hid: UC start failed to allocate IOSharedDataQueue\n");
    if (fQueueMD) {
      fQueueMD->release();
      fQueueMD = NULL;
    }
    if (fQueue) {
      fQueue->release();
      fQueue = NULL;
    }
    return false;
  }
  if (g_hid_lock) {
    IOLockLock(g_hid_lock);
    for (int i = 0; i < DF_HID_MAX_CLIENTS; i++) {
      if (!g_hid_clients[i]) {
        g_hid_clients[i] = this;
        break;
      }
    }
    IOLockUnlock(g_hid_lock);
  }
  return true;
}

void DFHIDEventServiceUC::stop(IOService *provider) {
  if (g_hid_lock) {
    IOLockLock(g_hid_lock);
    for (int i = 0; i < DF_HID_MAX_CLIENTS; i++) {
      if (g_hid_clients[i] == this) {
        g_hid_clients[i] = NULL;
      }
    }
    IOLockUnlock(g_hid_lock);
  }
  fOpened = false;
  fPortSet = false;
  if (fQueueMD) {
    fQueueMD->release();
    fQueueMD = NULL;
  }
  if (fQueue) {
    fQueue->release();
    fQueue = NULL;
  }
  IOUserClient::stop(provider);
}

IOReturn DFHIDEventServiceUC::registerNotificationPort(
    mach_port_t port, UInt32 type, io_user_reference_t refCon) {
  printf("hid: registerNotificationPort type=%u port=%p pid=%d\n",
         (unsigned)type, (void *)port, proc_selfpid());
  (void)refCon;
  if (g_hid_lock) {
    IOLockLock(g_hid_lock);
  }
  if (fQueue) {
    fQueue->setNotificationPort(port);
    fPortSet = (port != MACH_PORT_NULL);
  }
  if (g_hid_lock) {
    IOLockUnlock(g_hid_lock);
  }
  return kIOReturnSuccess;
}

IOReturn
DFHIDEventServiceUC::clientMemoryForType(UInt32 type, IOOptionBits *options,
                                         IOMemoryDescriptor **memory) {
  printf("hid: clientMemoryForType type=%u pid=%d\n", (unsigned)type,
         proc_selfpid());
  if (!fQueueMD) {
    return kIOReturnNoMemory;
  }
  fQueueMD->retain();
  if (options) {
    *options = 0;
  }
  *memory = fQueueMD;
  return kIOReturnSuccess;
}

IOReturn DFHIDEventServiceUC::externalMethod(uint32_t selector,
                                             IOExternalMethodArguments *a,
                                             IOExternalMethodDispatch *dispatch,
                                             OSObject *target,
                                             void *reference) {
  static unsigned s_hid_sel_logs = 0;
  if (s_hid_sel_logs++ < 30) {
    printf("hid: sel %u pid=%d in=%u/%u out=%u/%u\n", selector, proc_selfpid(),
           a->scalarInputCount, a->structureInputSize, a->scalarOutputCount,
           a->structureOutputSize);
  }
  (void)dispatch;
  (void)target;
  (void)reference;
  switch (selector) {
  case 0: /* kIOHIDEventServiceUserClientOpen */
    fOpened = true;
    return kIOReturnSuccess;
  case 1: /* kIOHIDEventServiceUserClientClose */
    fOpened = false;
    return kIOReturnSuccess;
  case 2: /* kIOHIDEventServiceUserClientCopyEvent */
  case 4: /* kIOHIDEventServiceUserClientCopyMatchingEvent */
    return kIOReturnUnsupported;
  case 3: /* kIOHIDEventServiceUserClientSetElementValue */
    return kIOReturnSuccess;
  default:
    return kIOReturnSuccess;
  }
}

IOReturn DFHIDEventServiceUC::clientClose(void) {
  terminate();
  return kIOReturnSuccess;
}

bool DFHIDEventServiceUC::enqueueRaw(void *data, uint32_t dataSize) {
  if (!fQueue || !fPortSet) {
    return false;
  }
  return fQueue->enqueue(data, dataSize) ? true : false;
}

extern "C" int df_hid_dispatch_touch(const struct df_touch_pt *pts, int count,
                                     uint32_t parent_mask, int any_range,
                                     int any_touch);
extern "C" int df_hid_dispatch_touch(const struct df_touch_pt *pts, int count,
                                     uint32_t parent_mask, int any_range,
                                     int any_touch) {
  if (!pts || count <= 0 || count > 10 || !g_hid_lock) {
    return 0;
  }
  uint8_t pkt[sizeof(DFHIDQueueHeader) + 11 * sizeof(DFHIDDigitizerEvent)];
  uint32_t total = (uint32_t)(sizeof(DFHIDQueueHeader) +
                              (size_t)(1 + count) * sizeof(DFHIDDigitizerEvent));
  bzero(pkt, total);

  uint64_t now = 0;
  clock_get_uptime(&now);

  DFHIDQueueHeader *hdr = (DFHIDQueueHeader *)pkt;
  hdr->timeStamp = now;
  hdr->senderID = g_hid_sender_id;
  hdr->options = 0;
  hdr->attributeLength = 0;
  hdr->eventCount = (uint32_t)(1 + count);

  DFHIDDigitizerEvent *ev = (DFHIDDigitizerEvent *)(pkt + sizeof(DFHIDQueueHeader));
  uint32_t child_mask = 0;
  for (int i = 0; i < count; i++) {
    child_mask |= pts[i].event_mask;
  }

  /* Parent Hand collection event (depth = 0) */
  ev[0].size = (uint32_t)sizeof(DFHIDDigitizerEvent);
  ev[0].type = 11; /* kIOHIDEventTypeDigitizer */
  ev[0].options = DF_HID_OPT_ABSOLUTE | DF_HID_OPT_COLLECTION | DF_HID_OPT_DISP_INT |
                  (any_range ? DF_HID_OPT_RANGE : 0) |
                  (any_touch ? DF_HID_OPT_TOUCH : 0);
  ev[0].depth = 0;
  ev[0].x = pts[0].x_fixed;
  ev[0].y = pts[0].y_fixed;
  ev[0].z = 0;
  ev[0].transducerIndex = 0;
  ev[0].transducerType = 3; /* kIOHIDDigitizerTransducerTypeHand */
  ev[0].identity = 0;
  ev[0].eventMask = parent_mask;
  ev[0].childEventMask = child_mask;
  ev[0].pressure = any_touch ? 0x10000 : 0;
  ev[0].orientationType = 2; /* Quality */
  ev[0].quality = 0x10000;
  ev[0].density = 0x10000;
  ev[0].irregularity = 0x10000;
  ev[0].majorRadius = 0x50000;
  ev[0].minorRadius = 0x50000;

  /* Child Finger events (depth = 1) */
  for (int i = 0; i < count; i++) {
    DFHIDDigitizerEvent *c = &ev[1 + i];
    c->size = (uint32_t)sizeof(DFHIDDigitizerEvent);
    c->type = 11;
    c->options = DF_HID_OPT_ABSOLUTE | DF_HID_OPT_DISP_INT |
                 (pts[i].in_range ? DF_HID_OPT_RANGE : 0) |
                 (pts[i].is_touch ? DF_HID_OPT_TOUCH : 0);
    c->depth = 1;
    c->x = pts[i].x_fixed;
    c->y = pts[i].y_fixed;
    c->z = 0;
    c->transducerIndex = (uint32_t)pts[i].id + 1;
    c->transducerType = 2; /* kIOHIDDigitizerTransducerTypeFinger */
    c->identity = (uint32_t)pts[i].id + 1;
    c->eventMask = pts[i].event_mask;
    c->childEventMask = 0;
    c->pressure = pts[i].is_touch ? 0x10000 : 0;
    c->orientationType = 2;
    c->quality = 0x10000;
    c->density = 0x10000;
    c->irregularity = 0x10000;
    c->majorRadius = 0x50000;
    c->minorRadius = 0x50000;
  }

  int sent = 0;
  IOLockLock(g_hid_lock);
  for (int i = 0; i < DF_HID_MAX_CLIENTS; i++) {
    if (g_hid_clients[i] && g_hid_clients[i]->isReady()) {
      if (g_hid_clients[i]->enqueueRaw(pkt, total)) {
        sent++;
      }
    }
  }
  IOLockUnlock(g_hid_lock);
  return sent;
}

extern "C" void df_hid_attach(IOService *provider);
extern "C" void df_hid_attach(IOService *provider) {
  IOHIDEventService *hid = OSTypeAlloc(IOHIDEventService);
  if (hid && hid->init() && hid->attach(provider) && hid->start(provider)) {
    /* start() already calls registerService() */
  } else {
    printf("hid: IOHIDEventService attach failed\n");
    if (hid) {
      hid->release();
    }
  }
}


/* fix-xcode16 P6: probe IOMobileFramebuffer for fire (appended to IOPlatformExpert.cpp).
 * Not a real display driver: the class is named IOMobileFramebuffer so that
 * IOServiceMatching("IOMobileFramebuffer") from IOMobileFramebuffer.framework finds it.
 * Every user-client call is logged ("mfb sel ...") and answered with success and zeroed
 * output, so the log shows which selectors backboardd/QuartzCore need.
 * clientMemoryForType hands out the MT6768 framebuffer the df console draws into. */
#include <IOKit/IOUserClient.h>
#include <IOKit/IOMemoryDescriptor.h>

extern "C" int proc_selfpid(void);

#define DF_MFB_PHYS  0x7ce6a000ULL
#define DF_MFB_W      1080
#define DF_MFB_H      2400

class IOMobileFramebuffer : public IOService
{
	OSDeclareDefaultStructors(IOMobileFramebuffer);
public:
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler) APPLE_KEXT_OVERRIDE;
};

class DFMobileFramebufferUC : public IOUserClient
{
	OSDeclareDefaultStructors(DFMobileFramebufferUC);
public:
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOMobileFramebuffer, IOService);
OSDefineMetaClassAndStructors(DFMobileFramebufferUC, IOUserClient);

static unsigned df_mfb_logs;

bool
IOMobileFramebuffer::start(IOService *provider)
{
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
	registerService();
	printf("mfb: IOMobileFramebuffer %dx%d at 0x%llx registered\n", DF_MFB_W, DF_MFB_H, DF_MFB_PHYS);
	return true;
}

IOReturn
IOMobileFramebuffer::newUserClient(task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler)
{
	DFMobileFramebufferUC *uc = OSTypeAlloc(DFMobileFramebufferUC);
	if (!uc || !uc->initWithTask(owningTask, securityID, type) || !uc->attach(this) || !uc->start(this)) {
		if (uc) {
			uc->detach(this);
			uc->release();
		}
		printf("mfb: newUserClient type %u failed\n", (unsigned)type);
		return kIOReturnNoMemory;
	}
	printf("mfb: open type %u by pid %d\n", (unsigned)type, proc_selfpid());
	*handler = uc;
	return kIOReturnSuccess;
}

IOReturn
DFMobileFramebufferUC::externalMethod(uint32_t selector, IOExternalMethodArguments *a,
    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference)
{
	if (df_mfb_logs++ < 400) {
		printf("mfb sel %u pid %d in %u/%u out %u/%u\n", selector, proc_selfpid(),
		    a->scalarInputCount, a->structureInputSize, a->scalarOutputCount, a->structureOutputSize);
	}
	for (uint32_t i = 0; i < a->scalarOutputCount; i++) {
		a->scalarOutput[i] = 0;
	}
	if (a->structureOutput && a->structureOutputSize) {
		bzero(a->structureOutput, a->structureOutputSize);
	}
	return kIOReturnSuccess;
}

IOReturn
DFMobileFramebufferUC::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory)
{
	printf("mfb: clientMemoryForType %u\n", (unsigned)type);
	*memory = IOMemoryDescriptor::withPhysicalAddress(DF_MFB_PHYS, (IOByteCount)DF_MFB_W * DF_MFB_H * 4, kIODirectionInOut);
	return *memory ? kIOReturnSuccess : kIOReturnNoMemory;
}

IOReturn
DFMobileFramebufferUC::clientClose(void)
{
	terminate();
	return kIOReturnSuccess;
}

extern "C" void
df_mfb_attach(IOService *provider)
{
	IOMobileFramebuffer *fb = OSTypeAlloc(IOMobileFramebuffer);
	if (fb && fb->init() && fb->attach(provider) && fb->start(provider)) {
		return;
	}
	printf("mfb: attach failed\n");
	if (fb) {
		fb->release();
	}
}

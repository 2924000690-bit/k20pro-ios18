/*
 * A minimal IO80211Interface for iOS 18's Wi-Fi side.
 *
 * wifid matches an IOKit service named IO80211Interface before it will call the
 * radio's ioctl interface anything but "no driver" ("PPM Error: No matching
 * driver found" in its own log), which in turn leaves Settings' Wi-Fi pane
 * greyed even though SIOCSA80211/SIOCGA80211 and SIOCGIFMEDIA already work.
 * This object publishes exactly that class name and the properties wifid and
 * IO80211.framework read; the actual requests keep going through the ioctl path.
 *
 * Off unless wifi_iokit=1, so a normal boot is untouched.
 */
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>

class IO80211Interface : public IOService {
	OSDeclareDefaultStructors(IO80211Interface)
public:
	bool start(IOService *provider) override;
};

OSDefineMetaClassAndStructors(IO80211Interface, IOService)

bool
IO80211Interface::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty("IO80211InterfaceRole", "Station");
	setProperty("IO80211InterfaceProductType", 0x10ULL, 32);
	setProperty("IO80211InterfaceRadioMODE", 0x1ULL, 32);
	setProperty("IO80211InterfaceCallStatus", 0x0ULL, 32);
	setProperty("IO80211InterfaceIPV", 0x1ULL, 32);
	setProperty("IOModel", "AppleWiFi");
	setProperty("IOClass", "IO80211Interface");
	registerService();
	return true;
}

static IO80211Interface *gIO80211Stub;

extern "C" void
hakios_io80211_stub_start(void)
{
	OSDictionary *match;
	IOService *parent;

	if (gIO80211Stub != NULL) {
		return;
	}
	match = IOService::serviceMatching("IOPlatformExpertDevice");
	if (match == NULL) {
		printf("HAKIOS IO80211: no matching dictionary\n");
		return;
	}
	parent = IOService::waitForMatchingService(match, 5000);
	match->release();
	if (parent == NULL) {
		printf("HAKIOS IO80211: no platform expert to attach to\n");
		return;
	}
	gIO80211Stub = new IO80211Interface;
	if (gIO80211Stub == NULL) {
		parent->release();
		return;
	}
	if (!gIO80211Stub->init()) {
		printf("HAKIOS IO80211: init failed\n");
		gIO80211Stub->release();
		gIO80211Stub = NULL;
		parent->release();
		return;
	}
	gIO80211Stub->attach(parent);
	gIO80211Stub->start(parent);
	printf("HAKIOS IO80211: IO80211Interface published (wifi_iokit=1)\n");
	parent->release();
}

/*
NXDK_GAMEPADS.C

The Xbox's controllers through nxdk's USB host stack (its OHCI driver and
XID class driver, nxdk_usb.lib), for xinput_xbox.c's XInput.

The stack starts with the constructors, before the game's main() takes its
memory (cache/physical_memory_map.c): it allocates its descriptor pool as
contiguous memory, which only the low 64 MB give and which the game state's
virtual memory would otherwise use up (port/xbox/README.md).

Each gamepad's interrupt endpoint is read continuously; a finished read
calls back from the stack's DPC with the controller's report, which is kept
for its port. The report after its two header bytes is the Xbox SDK's
XINPUT_GAMEPAD as it is (buttons, the eight analog buttons, the sticks), so
xinput_xbox.c hands it on unchanged. Plugging and unplugging is seen by
usbh_pooling_hubs, which the game's own polling (XGetDeviceChanges) calls,
and whose callbacks run on its thread.
*/

#include <string.h>
#include <usbh_lib.h>
#include <xid_driver.h>
#include <usb/libusbohci/inc/hub.h>
#include <xboxkrnl/xboxkrnl.h>

#include "nxdk_platform.h"

#define PORT_COUNT 4
#define REPORT_HEADER_SIZE 2

struct gamepad_port
{
	xid_dev_t *device;
	/* (written by the read's DPC) */
	unsigned char report[REPORT_HEADER_SIZE + XBOX_GAMEPAD_REPORT_SIZE];
	unsigned long reports;
	unsigned short rumble[2];
};

static struct gamepad_port ports[PORT_COUNT];
static int usb_started;

/* the front port a device is plugged into, 0-3 (the controller ports are
the root hub's 3, 4, 1 and 2, behind the internal hub of the consoles that
have one), or -1 */
static int device_port(xid_dev_t *device)
{
	int internal_hub = (XboxHardwareInfo.Flags & XBOX_HW_FLAG_INTERNAL_USB_HUB) != 0;
	UDEV_T *udev = device->iface->udev;

	while (udev)
	{
		UDEV_T *parent = udev->parent ? udev->parent->iface->udev : NULL;

		if (internal_hub ? (parent && !parent->parent) : !udev->parent)
		{
			switch (udev->port_num)
			{
			case 3: return 0;
			case 4: return 1;
			case 1: return 2;
			case 2: return 3;
			default: return -1;
			}
		}
		udev = parent;
	}
	return -1;
}

static void read_complete(UTR_T *utr)
{
	xid_dev_t *device = (xid_dev_t *)utr->context;
	struct gamepad_port *port;

	if (utr->status < 0 || !device || !device->user_data)
		return;
	port = (struct gamepad_port *)device->user_data;
	if (utr->xfer_len >= sizeof(port->report) && utr->buff[1] >= sizeof(port->report))
	{
		memcpy(port->report, utr->buff, sizeof(port->report));
		port->reports++;
	}
	utr->xfer_len = 0;
	utr->bIsTransferDone = 0;
	usbh_int_xfer(utr);
}

static void connected(xid_dev_t *device, int status)
{
	int index;

	(void)status;
	if (device->xid_desc.bType != XID_TYPE_GAMECONTROLLER)
		return;
	index = device_port(device);
	/* (a port not found, a hub's: the first free) */
	if (index < 0 || ports[index].device)
	{
		for (index = 0; index < PORT_COUNT && ports[index].device; index++)
			;
		if (index == PORT_COUNT)
		{
			platform_log("a fifth controller (%04x:%04x) is left out", device->idVendor, device->idProduct);
			return;
		}
	}
	memset(&ports[index], 0, sizeof(ports[index]));
	ports[index].device = device;
	device->user_data = &ports[index];
	platform_log("controller %04x:%04x in port %d", device->idVendor, device->idProduct, index + 1);
	usbh_xid_read(device, 0, read_complete);
}

static void disconnected(xid_dev_t *device, int status)
{
	struct gamepad_port *port = (struct gamepad_port *)device->user_data;

	(void)status;
	if (!port)
		return;
	device->user_data = NULL;
	port->device = NULL;
	platform_log("controller out of port %d", (int)(port - ports) + 1);
}

__attribute__((constructor)) static void gamepads_start(void)
{
	usbh_core_init();
	usbh_xid_init();
	usbh_install_xid_conn_callback(connected, disconnected);
	usb_started = 1;
}

unsigned long xbox_gamepads_poll(void)
{
	unsigned long connected_mask = 0;
	int index;

	if (!usb_started)
		return 0;
	usbh_pooling_hubs();
	for (index = 0; index < PORT_COUNT; index++)
	{
		if (ports[index].device)
		{
			connected_mask |= 1UL << index;
			/* (restarts a read a transfer error ended; one running is left be) */
			usbh_xid_read(ports[index].device, 0, read_complete);
		}
	}
	return connected_mask;
}

int xbox_gamepad_report(int index, unsigned char report[XBOX_GAMEPAD_REPORT_SIZE], unsigned long *reports)
{
	struct gamepad_port *port;
	KIRQL irql;

	if (index < 0 || index >= PORT_COUNT || !ports[index].device)
		return 0;
	port = &ports[index];
	/* (the read's DPC cannot change the report halfway through the copy) */
	irql = KeRaiseIrqlToDpcLevel();
	memcpy(report, port->report + REPORT_HEADER_SIZE, XBOX_GAMEPAD_REPORT_SIZE);
	*reports = port->reports;
	KfLowerIrql(irql);
	return 1;
}

void xbox_gamepad_rumble(int index, unsigned short left, unsigned short right)
{
	struct gamepad_port *port;

	if (index < 0 || index >= PORT_COUNT || !ports[index].device)
		return;
	port = &ports[index];
	/* (the game sets the motors every frame: only a change goes out) */
	if (port->rumble[0] == left && port->rumble[1] == right)
		return;
	if (usbh_xid_rumble(port->device, left, right) == USBH_OK)
	{
		port->rumble[0] = left;
		port->rumble[1] = right;
	}
}

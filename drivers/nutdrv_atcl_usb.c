/*
 * nutdrv_atcl_usb.c - driver for generic-brand "ATCL FOR UPS"
 *
 * Copyright (C) 2013 Charles Lepple <clepple+nut@gmail.com>
 *
 * Loosely based on richcomm_usb.c,
 * Copyright (C) 2007 Peter van Valderen <p.v.valderen@probu.nl>
 *                    Dirk Teurlings <dirk@upexia.nl>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include "main.h"
#include "usb-common.h"

/* driver version */
#define DRIVER_NAME	"'ATCL FOR UPS' USB driver"
#define DRIVER_VERSION	"0.02"

/* driver description structure */
upsdrv_info_t upsdrv_info = {
	DRIVER_NAME,
	DRIVER_VERSION,
	"Charles Lepple <clepple+nut@gmail.com>",
	DRV_EXPERIMENTAL,
	{ NULL }
};

#define STATUS_ENDPOINT (USB_ENDPOINT_IN | 1)
#define SHUTDOWN_ENDPOINT (USB_ENDPOINT_OUT | 2)
#define STATUS_PACKETSIZE 8
#define SHUTDOWN_PACKETSIZE 8

/* Probably can reduce this, since the pcap file shows mostly 1050-ish ms response times */
#define ATCL_USB_TIMEOUT USB_TIMEOUT

/* limit the amount of spew that goes in the syslog when we lose the UPS (from nut_usb.h) */
#define USB_ERR_LIMIT	10	/* start limiting after 10 in a row */
#define USB_ERR_RATE	10	/* then only print every 10th error */

static usb_device_id_t atcl_usb_id[] = {
	/* ATCL FOR UPS */
	{ USB_DEVICE(0x0001, 0x0000),  NULL /* TODO: match string descriptors, to avoid confusion with other 0001:0000 devices */ },

	/* end of list */
	{-1, -1, NULL}
};

static usb_dev_handle	*udev = NULL;
static USBDevice_t	usbdevice;
static unsigned int	comm_failures = 0;

static int device_match_func(USBDevice_t *device, void *privdata)
{
	switch (is_usb_device_supported(atcl_usb_id, device))
	{
	case SUPPORTED:
		return 1;

	case POSSIBLY_SUPPORTED:
	case NOT_SUPPORTED:
	default:
		return 0;
	}
}

static USBDeviceMatcher_t device_matcher = {
	&device_match_func,
	NULL,
	NULL
};

static int query_ups(char *reply)
{
	int	ret;

	ret = usb_interrupt_read(udev, STATUS_ENDPOINT, reply, STATUS_PACKETSIZE, ATCL_USB_TIMEOUT);

	if (ret <= 0) {
		upsdebugx(2, "status interrupt read: %s", ret ? usb_strerror() : "timeout");
		return ret;
	}

	upsdebug_hex(3, "read", reply, ret);
	return ret;
}

static void usb_comm_fail(const char *fmt, ...)
{
	int	ret;
	char	why[SMALLBUF];
	va_list	ap;

	/* this means we're probably here because select was interrupted */
	if (exit_flag != 0) {
		return;	 /* ignored, since we're about to exit anyway */
	}

	comm_failures++;

	if ((comm_failures == USB_ERR_LIMIT) || ((comm_failures % USB_ERR_RATE) == 0)) {
		upslogx(LOG_WARNING, "Warning: excessive comm failures, limiting error reporting");
	}

	/* once it's past the limit, only log once every USB_ERR_LIMIT calls */
	if ((comm_failures > USB_ERR_LIMIT) && ((comm_failures % USB_ERR_LIMIT) != 0)) {
		return;
	}

	/* generic message if the caller hasn't elaborated */
	if (!fmt) {
		upslogx(LOG_WARNING, "Communications with UPS lost - check cabling");
		return;
	}

	va_start(ap, fmt);
	ret = vsnprintf(why, sizeof(why), fmt, ap);
	va_end(ap);

	if ((ret < 1) || (ret >= (int) sizeof(why))) {
		upslogx(LOG_WARNING, "usb_comm_fail: vsnprintf needed more than %d bytes", (int)sizeof(why));
	}

	upslogx(LOG_WARNING, "Communications with UPS lost: %s", why);
}

static void usb_comm_good(void)
{
	if (comm_failures == 0) {
		return;
	}

	upslogx(LOG_NOTICE, "Communications with UPS re-established");	
	comm_failures = 0;
}

/*
 * Callback that is called by usb_device_open() that handles USB device
 * settings prior to accepting the devide. At the very least claim the
 * device here. Detaching the kernel driver will be handled by the
 * caller, don't do this here. Return < 0 on error, 0 or higher on
 * success.
 */
static int driver_callback(usb_dev_handle *handle, USBDevice_t *device)
{
	if (usb_set_configuration(handle, 1) < 0) {
		upsdebugx(5, "Can't set USB configuration");
		return -1;
	}

	if (usb_claim_interface(handle, 0) < 0) {
		upsdebugx(5, "Can't claim USB interface");
		return -1;
	}

	/* TODO: HID SET_IDLE to 0 */

	return 1;
}

static int usb_device_close(usb_dev_handle *handle)
{
	if (!handle) {
		return 0;
	}

	/* usb_release_interface() sometimes blocks and goes
	into uninterruptible sleep.  So don't do it. */
	/* usb_release_interface(handle, 0); */
	return usb_close(handle);
}

static int usb_device_open(usb_dev_handle **handlep, USBDevice_t *device, USBDeviceMatcher_t *matcher,
	int (*callback)(usb_dev_handle *handle, USBDevice_t *device))
{
	struct usb_bus	*bus;

	/* libusb base init */
	usb_init();
	usb_find_busses();
	usb_find_devices();

#ifndef __linux__ /* SUN_LIBUSB (confirmed to work on Solaris and FreeBSD) */
	/* Causes a double free corruption in linux if device is detached! */
	usb_device_close(*handlep);
#endif

	for (bus = usb_busses; bus; bus = bus->next) {

		struct usb_device	*dev;
		usb_dev_handle		*handle;

		for (dev = bus->devices; dev; dev = dev->next) {

			int	i, ret;
			USBDeviceMatcher_t	*m;

			upsdebugx(3, "Checking USB device [%04x:%04x] (%s/%s)", dev->descriptor.idVendor,
				dev->descriptor.idProduct, bus->dirname, dev->filename);
			
			/* supported vendors are now checked by the supplied matcher */

			/* open the device */
			*handlep = handle = usb_open(dev);
			if (!handle) {
				upsdebugx(4, "Failed to open USB device, skipping: %s", usb_strerror());
				continue;
			}

			/* collect the identifying information of this
			   device. Note that this is safe, because
			   there's no need to claim an interface for
			   this (and therefore we do not yet need to
			   detach any kernel drivers). */

			free(device->Vendor);
			free(device->Product);
			free(device->Serial);
			free(device->Bus);

			memset(device, 0, sizeof(*device));

			device->VendorID = dev->descriptor.idVendor;
			device->ProductID = dev->descriptor.idProduct;
			device->Bus = strdup(bus->dirname);
			
			if (dev->descriptor.iManufacturer) {
				char	buf[SMALLBUF];
				ret = usb_get_string_simple(handle, dev->descriptor.iManufacturer,
					buf, sizeof(buf));
				if (ret > 0) {
					device->Vendor = strdup(buf);
				}
			}

			if (dev->descriptor.iProduct) {
				char	buf[SMALLBUF];
				ret = usb_get_string_simple(handle, dev->descriptor.iProduct,
					buf, sizeof(buf));
				if (ret > 0) {
					device->Product = strdup(buf);
				}
			}

			if (dev->descriptor.iSerialNumber) {
				char	buf[SMALLBUF];
				ret = usb_get_string_simple(handle, dev->descriptor.iSerialNumber,
					buf, sizeof(buf));
				if (ret > 0) {
					device->Serial = strdup(buf);
				}
			}

			upsdebugx(4, "- VendorID     : %04x", device->VendorID);
			upsdebugx(4, "- ProductID    : %04x", device->ProductID);
			upsdebugx(4, "- Manufacturer : %s", device->Vendor ? device->Vendor : "unknown");
			upsdebugx(4, "- Product      : %s", device->Product ? device->Product : "unknown");
			upsdebugx(4, "- Serial Number: %s", device->Serial ? device->Serial : "unknown");
			upsdebugx(4, "- Bus          : %s", device->Bus ? device->Bus : "unknown");

			for (m = matcher; m; m = m->next) {
				
				switch (m->match_function(device, m->privdata))
				{
				case 0:
					upsdebugx(4, "Device does not match - skipping");
					goto next_device;
				case -1:
					fatal_with_errno(EXIT_FAILURE, "matcher");
					goto next_device;
				case -2:
					upsdebugx(4, "matcher: unspecified error");
					goto next_device;
				}
			}

			for (i = 0; i < 3; i++) {

				ret = callback(handle, device);
				if (ret >= 0) {
					upsdebugx(4, "USB device [%04x:%04x] opened", device->VendorID, device->ProductID);
					return ret;
				}
#ifdef HAVE_USB_DETACH_KERNEL_DRIVER_NP
				/* this method requires at least libusb 0.1.8:
				 * it forces device claiming by unbinding
				 * attached driver... From libhid */
				if (usb_detach_kernel_driver_np(handle, 0) < 0) {
					upsdebugx(1, "failed to detach kernel driver from USB device: %s", usb_strerror());
				} else {
					upsdebugx(4, "detached kernel driver from USB device...");
				}
#endif
			}

			fatalx(EXIT_FAILURE, "USB device [%04x:%04x] matches, but driver callback failed: %s",
				device->VendorID, device->ProductID, usb_strerror());

		next_device:
			usb_close(handle);
		}
	}

	*handlep = NULL;
	upsdebugx(4, "No matching USB device found");

	return -1;
}

/*
 * Initialise the UPS
 */
void upsdrv_initups(void)
{
	int	i;

	for (i = 0; usb_device_open(&udev, &usbdevice, &device_matcher, &driver_callback) < 0; i++) {

		if ((i < 3) && (sleep(5) == 0)) {
			usb_comm_fail("Can't open USB device, retrying ...");
			continue;
		}

		fatalx(EXIT_FAILURE,
			"Unable to find ATCL FOR UPS\n\n"

			"Things to try:\n"
			" - Connect UPS device to USB bus\n"
			" - Run this driver as another user (upsdrvctl -u or 'user=...' in ups.conf).\n"
			"   See upsdrvctl(8) and ups.conf(5).\n\n"

			"Fatal error: unusable configuration");
	}

}

void upsdrv_cleanup(void)
{
	usb_device_close(udev);

	free(usbdevice.Vendor);
	free(usbdevice.Product);
	free(usbdevice.Serial);
	free(usbdevice.Bus);
}

void upsdrv_initinfo(void)
{
	dstate_setinfo("ups.mfr", "%s", usbdevice.Vendor ? usbdevice.Vendor : "unknown");
	dstate_setinfo("ups.model", "%s", usbdevice.Product ? usbdevice.Product : "unknown");
	if(usbdevice.Serial && usbdevice.Product && strcmp(usbdevice.Serial, usbdevice.Product)) {
		/* Only set "ups.serial" if it isn't the same as "ups.model": */
		dstate_setinfo("ups.serial", "%s", usbdevice.Serial);
	}

	dstate_setinfo("ups.vendorid", "%04x", usbdevice.VendorID);
	dstate_setinfo("ups.productid", "%04x", usbdevice.ProductID);
}

void upsdrv_updateinfo(void)
{
	char	reply[STATUS_PACKETSIZE];
	int	ret;

	if (!udev) {
		ret = usb_device_open(&udev, &usbdevice, &device_matcher, &driver_callback);

		if (ret < 0) {
			return;
		}
	}

	ret = query_ups(reply);

	if (ret != STATUS_PACKETSIZE) {
		usb_comm_fail("Query to UPS failed");
		dstate_datastale();

		usb_device_close(udev);
		udev = NULL;

		return;
	}

	usb_comm_good();
	dstate_dataok();

	status_init();

	switch(reply[0]) {
		case 3:
			upsdebugx(2, "reply[0] = 0x%02x -> OL", reply[0]);
			status_set("OL");
			break;
		case 2:
			upsdebugx(2, "reply[0] = 0x%02x -> LB", reply[0]);
			status_set("LB");
			/* fall through */
		case 1:
			upsdebugx(2, "reply[0] = 0x%02x -> OB", reply[0]);
			status_set("OB");
			break;
		default:
			upslogx(LOG_ERR, "Unknown status: 0x%02x", reply[0]);
	}
	if(strnlen(reply + 1, 7) != 0) {
		upslogx(LOG_NOTICE, "Status bytes 1-7 are not all zero");
	}

	status_commit();
}

void upsdrv_shutdown(void)
{
	/*
	 * This packet seems to be what would shut down the UPS, but does not appear to work:
	 */
	const char	shutdown_packet[SHUTDOWN_PACKETSIZE] = { 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	int ret;

	ret = usb_interrupt_write(udev, SHUTDOWN_ENDPOINT, (char *)shutdown_packet, SHUTDOWN_PACKETSIZE, ATCL_USB_TIMEOUT);

	if (ret <= 0) {
		upsdebugx(LOG_NOTICE, "%s: first usb_interrupt_write() failed: %s", __func__, ret ? usb_strerror() : "timeout");
	}

	/* Totally guessing from the .pcap file here: */
	usleep(170*1000);

	ret = usb_interrupt_write(udev, SHUTDOWN_ENDPOINT, (char *)shutdown_packet, SHUTDOWN_PACKETSIZE, ATCL_USB_TIMEOUT);

	if (ret <= 0) {
		upsdebugx(LOG_ERR, "%s: second usb_interrupt_write() failed: %s", __func__, ret ? usb_strerror() : "timeout");
	}

}

void upsdrv_help(void)
{
}

void upsdrv_makevartable(void)
{
}

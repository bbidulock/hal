/***************************************************************************
 * CVSID: $Id$
 *
 * probe-input.c : Probe input devices
 *
 * Copyright (C) 2004 David Zeuthen, <david@fubar.dk>
 *
 * Licensed under the Academic Free License version 2.0
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 **************************************************************************/

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <linux/input.h>

#include "libhal/libhal.h"

#define test_bit(bit, array) (array[(bit) / 8] & (1 << ((bit) % 8)))

static void 
check_abs (int fd, LibHalContext *ctx, const char *udi)
{
	char bitmask[(ABS_MAX + 7) / 8];
	DBusError error;

	if (ioctl (fd, EVIOCGBIT(EV_ABS, sizeof (bitmask)), bitmask) < 0) {
		fprintf(stderr, "ioctl EVIOCGBIT failed\n");
		goto out;
	}

	if (!test_bit(ABS_X, bitmask) || !test_bit(ABS_Y, bitmask)) {
		fprintf (stderr, "missing x or y absolute axes\n");
		goto out;
	}

	dbus_error_init (&error);
	libhal_device_add_capability (ctx, udi, "input.tablet", &error);

out:
	;
}

static void 
check_key (int fd, LibHalContext *ctx, const char *udi)
{
	unsigned int i;
	char bitmask[(KEY_MAX + 7) / 8];
	int is_keyboard;
	DBusError error;

	if (ioctl (fd, EVIOCGBIT(EV_KEY, sizeof (bitmask)), bitmask) < 0) {
		fprintf(stderr, "ioctl EVIOCGBIT failed\n");
		goto out;
	}

	is_keyboard = FALSE;

	/* All keys that are not buttons are less than BTN_MISC */
	for (i = KEY_RESERVED + 1; i < BTN_MISC; i++) {
		if (test_bit (i, bitmask)) {
			is_keyboard = TRUE;
			break;
		}
	}

	if (is_keyboard) {
		dbus_error_init (&error);
		libhal_device_add_capability (ctx, udi, "input.keyboard", &error);
	}

out:
	;
}

static void 
check_rel (int fd, LibHalContext *ctx, const char *udi)
{
	char bitmask[(REL_MAX + 7) / 8];
	DBusError error;

	if (ioctl (fd, EVIOCGBIT(EV_REL, sizeof (bitmask)), bitmask) < 0) {
		fprintf(stderr, "ioctl EVIOCGBIT failed: %m\n");
		goto out;
	}

	if (!test_bit (REL_X, bitmask) || !test_bit (REL_Y, bitmask)) {
		fprintf (stderr, "missing x or y relative axes\n");
		goto out;
	}

	dbus_error_init (&error);
	libhal_device_add_capability (ctx, udi, "input.mouse", &error);

out:
	;
}

int 
main (int argc, char *argv[])
{
	int fd;
	int ret;
	char *udi;
	char *device_file;
	char *physical_device;
	LibHalContext *ctx = NULL;
	DBusError error;
	DBusConnection *conn;
	char name[128];
	struct input_id id;

	fd = -1;

	/* assume failure */
	ret = 1;

	udi = getenv ("UDI");
	if (udi == NULL)
		goto out;

	dbus_error_init (&error);
	if ((conn = dbus_bus_get (DBUS_BUS_SYSTEM, &error)) == NULL)
		goto out;

	if ((ctx = libhal_ctx_new ()) == NULL)
		goto out;
	if (!libhal_ctx_set_dbus_connection (ctx, conn))
		goto out;
	if (!libhal_ctx_init (ctx, &error))
		goto out;

	device_file = getenv ("HAL_PROP_INPUT_DEVICE");
	if (device_file == NULL)
		goto out;

	fprintf(stderr, "*** handling %s\n", device_file);


	fd = open (device_file, O_RDONLY);
	if (fd < 0)
		goto out;

	/* if we don't have a physical device then only accept input buses
	 * that we now aren't hotpluggable
	 */
	if (ioctl (fd, EVIOCGID, &id) < 0) {
		fprintf(stderr, "ioctl EVIOCGID failed\n");
		goto out;
	}
	physical_device = getenv ("HAL_PROP_INPUT_PHYSICAL_DEVICE");
	if (physical_device == NULL) {
		switch (id.bustype) {
		case 17: /* TODO: x86 legacy port; use symbol instead of hardcoded constant */
			break;

			/* TODO: ADB on Apple computers */
		default:
			goto out;
		}
	}

	/* only consider devices with the event interface */
	if (ioctl (fd, EVIOCGNAME(sizeof (name)), name) < 0) {
		fprintf(stderr, "ioctl EVIOCGNAME failed\n");
		goto out;
	}
	if (!libhal_device_set_property_string (ctx, udi, "info.product", name, &error))
		goto out;
	if (!libhal_device_set_property_string (ctx, udi, "input.product", name, &error))
		goto out;

	check_abs (fd, ctx, udi);
	check_rel (fd, ctx, udi);
	check_key (fd, ctx, udi);

	/* success */
	ret = 0;

out:
	if (fd >= 0)
		close (fd);

	if (ctx != NULL) {
		dbus_error_init (&error);
		libhal_ctx_shutdown (ctx, &error);
		libhal_ctx_free (ctx);
	}

	return ret;
}
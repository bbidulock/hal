/***************************************************************************
 * CVSID: $Id$
 *
 * addon-keyboard.c : Listen to key events and modify hal device objects
 *
 * Copyright (C) 2005 David Zeuthen, <david@fubar.dk>
 * Copyright (C) 2005 Ryan Lortie <desrt@desrt.ca>
 * Copyright (C) 2006 Matthew Garrett <mjg59@srcf.ucam.org>
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 **************************************************************************/

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <linux/input.h>

#include "libhal/libhal.h"

#include "../probing/shared.h"

static char *udi;

static char *key_name[KEY_MAX + 1] = {
	[0 ... KEY_MAX] = NULL,
	[KEY_STOP] = "stop",
	[KEY_AGAIN] = "again",
	[KEY_PROPS] = "props",
	[KEY_UNDO] = "undo",
	[KEY_FRONT] = "front",
	[KEY_COPY] = "copy",
	[KEY_OPEN] = "open",
	[KEY_PASTE] = "paste",
	[KEY_FIND] = "find",
	[KEY_CUT] = "cut",
	[KEY_HELP] = "help",
	[KEY_MENU] = "menu",
	[KEY_CALC] = "calc",
	[KEY_SETUP] = "setup",
	[KEY_SLEEP] = "sleep",
	[KEY_WAKEUP] = "wakeup",
	[KEY_FILE] = "file",
	[KEY_SENDFILE] = "sendfile",
	[KEY_DELETEFILE] = "deletefile",
	[KEY_XFER] = "xfer",
	[KEY_PROG1] = "prog1",
	[KEY_PROG2] = "prog2",
	[KEY_WWW] = "www",
	[KEY_MSDOS] = "msdos",
	[KEY_COFFEE] = "coffee",
	[KEY_DIRECTION] = "direction",
	[KEY_CYCLEWINDOWS] = "cyclewindows",
	[KEY_MAIL] = "mail",
	[KEY_BOOKMARKS] = "bookmarks",
	[KEY_COMPUTER] = "computer",
	[KEY_BACK] = "back",
	[KEY_FORWARD] = "forward",
	[KEY_CLOSECD] = "closecd",
	[KEY_EJECTCD] = "ejectcd",
	[KEY_EJECTCLOSECD] = "ejectclosecd",
	[KEY_NEXTSONG] = "nextsong",
	[KEY_PLAYPAUSE] = "playpause",
	[KEY_PREVIOUSSONG] = "previoussong",
	[KEY_STOPCD] = "stopcd",
	[KEY_RECORD] = "record",
	[KEY_REWIND] = "rewind",
	[KEY_PHONE] = "phone",
	[KEY_ISO] = "iso",
	[KEY_CONFIG] = "config",
	[KEY_HOMEPAGE] = "homepage",
	[KEY_REFRESH] = "refresh",
	[KEY_EXIT] = "exit",
	[KEY_MOVE] = "move",
	[KEY_EDIT] = "edit",
	[KEY_SCROLLUP] = "scrollup",
	[KEY_SCROLLDOWN] = "scrolldown",
	[KEY_KPLEFTPAREN] = "kpleftparen",
	[KEY_KPRIGHTPAREN] = "kprightparen",
	[KEY_F13] = "f13",
	[KEY_F14] = "f14",
	[KEY_F15] = "f15",
	[KEY_F16] = "f16",
	[KEY_F17] = "f17",
	[KEY_F18] = "f18",
	[KEY_F19] = "f19",
	[KEY_F20] = "f20",
	[KEY_F21] = "f21",
	[KEY_F22] = "f22",
	[KEY_F23] = "f23",
	[KEY_F24] = "f24",
	[KEY_PLAYCD] = "playcd",
	[KEY_PAUSECD] = "pausecd",
	[KEY_PROG3] = "prog3",
	[KEY_PROG4] = "prog4",
	[KEY_SUSPEND] = "hibernate",
	[KEY_CLOSE] = "close",
	[KEY_PLAY] = "play",
	[KEY_FASTFORWARD] = "fastforward",
	[KEY_BASSBOOST] = "bassboost",
	[KEY_PRINT] = "print",
	[KEY_HP] = "hp",
	[KEY_CAMERA] = "camera",
	[KEY_SOUND] = "sound",
	[KEY_QUESTION] = "question",
	[KEY_EMAIL] = "email",
	[KEY_CHAT] = "chat",
	[KEY_SEARCH] = "search",
	[KEY_CONNECT] = "connect",
	[KEY_FINANCE] = "finance",
	[KEY_SPORT] = "sport",
	[KEY_SHOP] = "shop",
	[KEY_ALTERASE] = "alterase",
	[KEY_CANCEL] = "cancel",
	[KEY_BRIGHTNESSDOWN] = "brightnessdown",
	[KEY_BRIGHTNESSUP] = "brightnessup",
	[KEY_MEDIA] = "media",
	[KEY_POWER] = "power"
};

static void
main_loop (LibHalContext *ctx, FILE* eventfp)
{
	DBusError error;
	struct input_event event;

	dbus_error_init (&error);

	while (fread (&event, sizeof(event), 1, eventfp)) {
		/* dbg ("event.code = %d (0x%02x)", event.code); */
		if (key_name[event.code] && event.value == 1) {
			libhal_device_emit_condition (ctx, udi, 
						      "ButtonPressed",
						      key_name[event.code],
						      &error);
		}
	}
	
	dbus_error_free (&error);
}

int
main (int argc, char **argv)
{
	LibHalContext *ctx = NULL;
	DBusError error;
	char *device_file;
	FILE *eventfp;

	if (getenv ("HALD_VERBOSE") != NULL)
		is_verbose = TRUE;

	dbus_error_init (&error);

	if ((udi = getenv ("UDI")) == NULL)
		goto out;
	
	if ((device_file = getenv ("HAL_PROP_INPUT_DEVICE")) == NULL)
		goto out;

	if ((ctx = libhal_ctx_init_direct (&error)) == NULL)
                goto out;

	eventfp = fopen(device_file, "r");	

	if (!eventfp)
		goto out;

	drop_privileges();

	while (1)
	{
		main_loop (ctx, eventfp);
		
		/* If main_loop exits sleep for 5s and try to reconnect (
		   again). */
		sleep (5);
	}

	return 0;

 out:
	if (ctx != NULL) {
                dbus_error_init (&error);
                libhal_ctx_shutdown (ctx, &error);
                libhal_ctx_free (ctx);
        }
	
	return 0;
}

/* vim:set sw=8 noet: */
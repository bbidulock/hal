/***************************************************************************
 * 
 *
 * addon-dell-backlight.cpp : Sets the backlight for Dell laptops using the libsmbios interface
 * 
 * Copyright (C) 2007 Erik Andrén <erik.andren@gmail.com>
 * Heavily based on the macbook addon and the dellLcdBrightness code in libsmbios. 
 * This program needs the dcdbas module to be loaded and libsmbios >= 0.12.1 installed
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 **************************************************************************/

#include <config.h>

#include <glib/gmain.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>

#include "libhal/libhal.h"
#include "../../logger.h"

#include "smbios/ISmi.h"
#include "smbios/IToken.h"

static LibHalContext *halctx = NULL;
static GMainLoop *main_loop;
static char *udi;
static DBusConnection *conn;
static const int DELL_LCD_BRIGHTNESS_TOKEN = 0x007d;

using namespace std;
using namespace smi;

typedef u32 (*readfn)(u32 location, u32 *minValue, u32 *maxValue);
typedef u32 (*writefn)(const std::string &password, u32 location, u32 value, u32 *minValue, u32 *maxValue);

static u32 
read_backlight(dbus_bool_t ACon)
{
	u8  location = 0;
	u32 minValue = 0;
	u32 maxValue = 0;
	u32 curValue;
	readfn readFunction;
	
	if (ACon) 
                readFunction = &smi::readACModeSetting;
    	else 
                readFunction = &smi::readBatteryModeSetting;
	
	smbios::TokenTableFactory *ttFactory = smbios::TokenTableFactory::getFactory() ;
        smbios::ITokenTable *tokenTable = ttFactory->getSingleton();
        smbios::IToken *token = &(*((*tokenTable)[ DELL_LCD_BRIGHTNESS_TOKEN ]));
        dynamic_cast< smbios::ISmiToken * >(token)->getSmiDetails( static_cast<u16*>(0), static_cast<u8*>(0), &location );

	try 
	{ 
        	curValue = readFunction(location, &minValue, &maxValue);
	}
	catch( const exception &e ) 
	{
        	HAL_ERROR(("Could not access the dcdbas kernel module. Please make sure it is loaded"));
		return 7;
    	}

	if(ACon) 
		HAL_DEBUG(("Reading %d from the AC backlight register", curValue));	
	else 
		HAL_DEBUG(("Reading %d from the BAT backlight register", curValue));

	return curValue;
}

static void 
write_backlight (u32 newBacklightValue, dbus_bool_t ACon) 
{	
	u8  location = 0;
	u32 minValue = 0;
	u32 maxValue = 0;	
	u32 curValue;
	writefn writeFunction;
	string password(""); //FIXME: Implement password support
	
	if (ACon) 
                writeFunction = &smi::writeACModeSetting;
    	else 
                writeFunction = &smi::writeBatteryModeSetting;

	smbios::TokenTableFactory *ttFactory = smbios::TokenTableFactory::getFactory();
        smbios::ITokenTable *tokenTable = ttFactory->getSingleton();
        smbios::IToken *token = &(*((*tokenTable)[ DELL_LCD_BRIGHTNESS_TOKEN ]));
        dynamic_cast< smbios::ISmiToken * >(token)->getSmiDetails( static_cast<u16*>(0), static_cast<u8*>(0), &location );

	try 
	{
		curValue = writeFunction(password, location, newBacklightValue, &minValue, &maxValue); 
	}
	catch( const exception &e )
    	{
        	HAL_ERROR(("Could not access the dcdbas kernel module. Please make sure it is loaded"));
		return;
    	}
	if(ACon)
		HAL_DEBUG(("Wrote %d to the AC backlight", curValue));
	else
		HAL_DEBUG(("Wrote %d to the BAT backlight", curValue));
}

static DBusHandlerResult
filter_function (DBusConnection *connection, DBusMessage *message, void *userdata)
{
	DBusError err;
	DBusMessage *reply = NULL;
	dbus_bool_t AC;
	dbus_error_init (&err);

	/* Mechanism to ensure that we always set the AC brightness when we are on AC-power etc. */
	AC = libhal_device_get_property_bool (halctx, 
					     "/org/freedesktop/Hal/devices/acpi_AC",
					     "ac_adapter.present",
					     &err);

	if (dbus_message_is_method_call (message, 
					 "org.freedesktop.Hal.Device.LaptopPanel", 
					 "SetBrightness")) {
		int brightness;
		
		HAL_DEBUG(("Received SetBrightness DBus call"));

		if (dbus_message_get_args (message, 
					   &err,
					   DBUS_TYPE_INT32, &brightness,
					   DBUS_TYPE_INVALID)) {
			if (brightness < 0 || brightness > 7) {
				reply = dbus_message_new_error (message,
								"org.freedesktop.Hal.Device.LaptopPanel.Invalid",
								"Brightness has to be between 0 and 7!");

			} else {
				int return_code;
				
				write_backlight (brightness, AC);

				reply = dbus_message_new_method_return (message);
				if (reply == NULL)
					goto error;

				return_code = 0;

				dbus_message_append_args (reply,
							  DBUS_TYPE_INT32, &return_code,
							  DBUS_TYPE_INVALID);
			}

			dbus_connection_send (connection, reply, NULL);
		}
		
	} else if (dbus_message_is_method_call (message, 
						"org.freedesktop.Hal.Device.LaptopPanel", 
						"GetBrightness")) {
		HAL_DEBUG(("Received GetBrightness DBUS call"));
		
		if (dbus_message_get_args (message, 
					   &err,
					   DBUS_TYPE_INVALID)) {
			int brightness = read_backlight(AC);
			if (brightness < 0)
				brightness = 0;
			else if (brightness > 7)
				brightness = 7;

			reply = dbus_message_new_method_return (message);
			if (reply == NULL)
				goto error;

			dbus_message_append_args (reply,
						  DBUS_TYPE_INT32, &brightness,
						  DBUS_TYPE_INVALID);
			dbus_connection_send (connection, reply, NULL);
		}
	}	
error:
	if (reply != NULL)
		dbus_message_unref (reply);
	
	return DBUS_HANDLER_RESULT_HANDLED;
}

int
main (int argc, char *argv[])
{
 	DBusError err;

	setup_logger ();

	udi = getenv ("UDI");

	HAL_DEBUG (("udi=%s", udi));
	if (udi == NULL) {
		HAL_ERROR (("No device specified"));
		return -2;
	}

	dbus_error_init (&err);
	if ((halctx = libhal_ctx_init_direct (&err)) == NULL) {
		HAL_ERROR (("Cannot connect to hald"));
		return -3;
	}

	dbus_error_init (&err);
	if (!libhal_device_addon_is_ready (halctx, udi, &err)) {
		return -4;
	}

	conn = libhal_ctx_get_dbus_connection (halctx);
	dbus_connection_setup_with_g_main (conn, NULL);

	dbus_connection_add_filter (conn, filter_function, NULL, NULL);

	/* this works because we hardcoded the udi's in the <spawn> in the fdi files */
	if (!libhal_device_claim_interface (halctx, 
					    "/org/freedesktop/Hal/devices/dell_lcd_panel", 
					    "org.freedesktop.Hal.Device.LaptopPanel", 
					    "    <method name=\"SetBrightness\">\n"
					    "      <arg name=\"brightness_value\" direction=\"in\" type=\"i\"/>\n"
					    "      <arg name=\"return_code\" direction=\"out\" type=\"i\"/>\n"
					    "    </method>\n"
					    "    <method name=\"GetBrightness\">\n"
					    "      <arg name=\"brightness_value\" direction=\"out\" type=\"i\"/>\n"
					    "    </method>\n",
					    &err)) {
		HAL_ERROR (("Cannot claim interface 'org.freedesktop.Hal.Device.LaptopPanel'"));
		return -4;
	}
	
	main_loop = g_main_loop_new (NULL, FALSE);
	g_main_loop_run (main_loop);
	return 0;
}


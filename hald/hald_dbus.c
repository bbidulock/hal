/***************************************************************************
 * CVSID: $Id$
 *
 * dbus.c : D-BUS interface of HAL daemon
 *
 * Copyright (C) 2003 David Zeuthen, <david@fubar.dk>
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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#include <dbus/dbus.h>
#include <dbus/dbus-glib-lowlevel.h>

#include "hald.h"
#include "hald_dbus.h"
#include "device.h"
#include "device_store.h"
#include "logger.h"
#include "osspec.h"

static DBusConnection *dbus_connection;

/**
 * @defgroup DaemonErrors Error conditions
 * @ingroup HalDaemon
 * @brief Various error messages the HAL daemon can raise
 * @{
 */

/** Raise the org.freedesktop.Hal.NoSuchDevice error
 *
 *  @param  connection          D-Bus connection
 *  @param  in_reply_to         message to report error on
 *  @param  udi                 Unique device id given
 */
static void
raise_no_such_device (DBusConnection * connection,
		      DBusMessage * in_reply_to, const char *udi)
{
	char buf[512];
	DBusMessage *reply;

	snprintf (buf, 511, "No device with id %s", udi);
	HAL_WARNING ((buf));
	reply = dbus_message_new_error (in_reply_to,
					"org.freedesktop.Hal.NoSuchDevice",
					buf);
	if (reply == NULL)
		DIE (("No memory"));
	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));
	dbus_message_unref (reply);
}

/** Raise the org.freedesktop.Hal.NoSuchProperty error
 *
 *  @param  connection          D-Bus connection
 *  @param  in_reply_to         message to report error on
 *  @param  device_id           Id of the device
 *  @param  key                 Key of the property that didn't exist
 */
static void
raise_no_such_property (DBusConnection * connection,
			DBusMessage * in_reply_to,
			const char *device_id, const char *key)
{
	char buf[512];
	DBusMessage *reply;

	snprintf (buf, 511, "No property %s on device with id %s", key,
		  device_id);
	HAL_WARNING ((buf));
	reply = dbus_message_new_error (in_reply_to,
					"org.freedesktop.Hal.NoSuchProperty",
					buf);
	if (reply == NULL)
		DIE (("No memory"));
	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));
	dbus_message_unref (reply);
}

/** Raise the org.freedesktop.Hal.TypeMismatch error
 *
 *  @param  connection          D-Bus connection
 *  @param  in_reply_to         message to report error on
 *  @param  device_id           Id of the device
 *  @param  key                 Key of the property
 */
static void
raise_property_type_error (DBusConnection * connection,
			   DBusMessage * in_reply_to,
			   const char *device_id, const char *key)
{
	char buf[512];
	DBusMessage *reply;

	snprintf (buf, 511,
		  "Type mismatch setting property %s on device with id %s",
		  key, device_id);
	HAL_WARNING ((buf));
	reply = dbus_message_new_error (in_reply_to,
					"org.freedesktop.Hal.TypeMismatch",
					buf);
	if (reply == NULL)
		DIE (("No memory"));
	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));
	dbus_message_unref (reply);
}

/** Raise the org.freedesktop.Hal.SyntaxError error
 *
 *  @param  connection          D-Bus connection
 *  @param  in_reply_to         message to report error on
 *  @param  method_name         Name of the method that was invoked with
 *                              the wrong signature
 */
static void
raise_syntax (DBusConnection * connection,
	      DBusMessage * in_reply_to, const char *method_name)
{
	char buf[512];
	DBusMessage *reply;

	snprintf (buf, 511,
		  "There is a syntax error in the invocation of "
		  "the method %s", method_name);
	HAL_WARNING ((buf));
	reply = dbus_message_new_error (in_reply_to,
					"org.freedesktop.Hal.SyntaxError",
					buf);
	if (reply == NULL)
		DIE (("No memory"));
	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));
	dbus_message_unref (reply);
}

/** Raise the org.freedesktop.Hal.DeviceNotLocked error
 *
 *  @param  connection          D-Bus connection
 *  @param  in_reply_to         message to report error on
 *  @param  device              device which isn't locked
 */
static void
raise_device_not_locked (DBusConnection *connection,
			 DBusMessage    *in_reply_to,
			 HalDevice      *device)
{
	char buf[512];
	DBusMessage *reply;

	snprintf (buf, 511, "The device %s is not locked",
		  hal_device_get_udi (device));
	HAL_WARNING ((buf));
	reply = dbus_message_new_error (in_reply_to,
					"org.freedesktop.Hal.DeviceNotLocked",
					buf);

	if (reply == NULL || !dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
}

/** Raise the org.freedesktop.Hal.DeviceAlreadyLocked error
 *
 *  @param  connection          D-Bus connection
 *  @param  in_reply_to         message to report error on
 *  @param  device              device which isn't locked
 */
static void
raise_device_already_locked (DBusConnection *connection,
			     DBusMessage    *in_reply_to,
			     HalDevice      *device)
{
	DBusMessage *reply;
	const char *reason;

	reason = hal_device_property_get_string (device, "info.locked.reason");
	HAL_WARNING (("Device %s is already locked: %s",
		      hal_device_get_udi (device), reason));


	reply = dbus_message_new_error (in_reply_to,
					"org.freedesktop.Hal.DeviceAlreadyLocked",
					
					reason);

	if (reply == NULL || !dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
}

/** Raise the org.freedesktop.Hal.PermissionDenied error
 *
 *  @param  connection          D-Bus connection
 *  @param  in_reply_to         message to report error on
 *  @param  message             what you're not allowed to do
 */
static void
raise_permission_denied (DBusConnection *connection,
			 DBusMessage    *in_reply_to,
			 const char     *reason)
{
	char buf[512];
	DBusMessage *reply;

	snprintf (buf, 511, "Permission denied: %s", reason);
	HAL_WARNING ((buf));
	reply = dbus_message_new_error (in_reply_to,
					"org.freedesktop.Hal.PermissionDenied",
					buf);

	if (reply == NULL || !dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
}

/** @} */

/**
 * @defgroup ManagerInterface D-BUS interface org.freedesktop.Hal.Manager
 * @ingroup HalDaemon
 * @brief D-BUS interface for querying device objects
 *
 * @{
 */

static gboolean
foreach_device_get_udi (HalDeviceStore *store, HalDevice *device,
			gpointer user_data)
{
	DBusMessageIter *iter = user_data;

	dbus_message_iter_append_string (iter, hal_device_get_udi (device));

	return TRUE;
}

/** Get all devices.
 *
 *  <pre>
 *  array{object_reference} Manager.GetAllDevices()
 *  </pre>
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @return                     What to do with the message
 */
DBusHandlerResult
manager_get_all_devices (DBusConnection * connection,
			 DBusMessage * message)
{
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter iter_array;

	HAL_TRACE (("entering"));

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	dbus_message_iter_init (reply, &iter);
	dbus_message_iter_append_array (&iter, &iter_array,
					DBUS_TYPE_STRING);

	hal_device_store_foreach (hald_get_gdl (),
				  foreach_device_get_udi,
				  &iter_array);

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);

	return DBUS_HANDLER_RESULT_HANDLED;
}

typedef struct {
	const char *key;
	const char *value;
	DBusMessageIter *iter;
} DeviceMatchInfo;

static gboolean
foreach_device_match_get_udi (HalDeviceStore *store, HalDevice *device,
			      gpointer user_data)
{
	DeviceMatchInfo *info = user_data;
	const char *dev_value;

	if (hal_device_property_get_type (device,
					  info->key) != DBUS_TYPE_STRING)
		return TRUE;

	dev_value = hal_device_property_get_string (device, info->key);

	if (dev_value != NULL && strcmp (dev_value, info->value) == 0) {
		dbus_message_iter_append_string (info->iter,
						 hal_device_get_udi (device));
	}

	return TRUE;
}

static gboolean
foreach_device_match_get_udi_tdl (HalDeviceStore *store, HalDevice *device,
				  gpointer user_data)
{
	DeviceMatchInfo *info = user_data;
	const char *dev_value;

	/* skip devices in the TDL that hasn't got a real UDI yet */
	if (strncmp (device->udi, "/org/freedesktop/Hal/devices/temp",
		     sizeof ("/org/freedesktop/Hal/devices/temp")) == 0)
		return TRUE;

	if (hal_device_property_get_type (device,
					  info->key) != DBUS_TYPE_STRING)
		return TRUE;

	dev_value = hal_device_property_get_string (device, info->key);

	if (dev_value != NULL && strcmp (dev_value, info->value) == 0) {
		dbus_message_iter_append_string (info->iter,
						 hal_device_get_udi (device));
	}

	return TRUE;
}

/** Find devices in the GDL where a single string property matches a given
 *  value. Also returns devices in the TDL that has a non-tmp UDI.
 *
 *  <pre>
 *  array{object_reference} Manager.FindDeviceStringMatch(string key,
 *                                                        string value)
 *  </pre>
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @return                     What to do with the message
 */
DBusHandlerResult
manager_find_device_string_match (DBusConnection * connection,
				  DBusMessage * message)
{
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter iter_array;
	DBusError error;
	const char *key;
	const char *value;
	DeviceMatchInfo info;

	HAL_TRACE (("entering"));

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &key,
				    DBUS_TYPE_STRING, &value,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message,
			      "Manager.FindDeviceStringMatch");
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	dbus_message_iter_init (reply, &iter);
	dbus_message_iter_append_array (&iter, &iter_array,
					DBUS_TYPE_STRING);

	info.key = key;
	info.value = value;
	info.iter = &iter_array;

	hal_device_store_foreach (hald_get_gdl (),
				  foreach_device_match_get_udi,
				  &info);

	/* Also returns devices in the TDL that has a non-tmp UDI */
	hal_device_store_foreach (hald_get_tdl (),
				  foreach_device_match_get_udi_tdl,
				  &info);

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);

	return DBUS_HANDLER_RESULT_HANDLED;
}

typedef struct {
	const char *capability;
	DBusMessageIter *iter;
} DeviceCapabilityInfo;

static gboolean
foreach_device_by_capability (HalDeviceStore *store, HalDevice *device,
			      gpointer user_data)
{
	DeviceCapabilityInfo *info = user_data;
	const char *caps;

	caps = hal_device_property_get_string (device, "info.capabilities");

	if (caps != NULL && strstr (caps, info->capability) != NULL) {
		dbus_message_iter_append_string (info->iter,
						 hal_device_get_udi (device));
	}

	return TRUE;
}

/** Find devices in the GDL with a given capability.
 *
 *  <pre>
 *  array{object_reference} Manager.FindDeviceByCapability(string capability)
 *  </pre>
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @return                     What to do with the message
 */
DBusHandlerResult
manager_find_device_by_capability (DBusConnection * connection,
				   DBusMessage * message)
{
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter iter_array;
	DBusError error;
	const char *capability;
	DeviceCapabilityInfo info;

	HAL_TRACE (("entering"));

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &capability,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message,
			      "Manager.FindDeviceByCapability");
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	dbus_message_iter_init (reply, &iter);
	dbus_message_iter_append_array (&iter, &iter_array,
					DBUS_TYPE_STRING);

	info.capability = capability;
	info.iter = &iter_array;

	hal_device_store_foreach (hald_get_gdl (),
				  foreach_device_by_capability,
				  &info);

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);

	return DBUS_HANDLER_RESULT_HANDLED;
}


/** Determine if a device exists.
 *
 *  <pre>
 *  bool Manager.DeviceExists(string udi)
 *  </pre>
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @return                     What to do with the message
 */
DBusHandlerResult
manager_device_exists (DBusConnection * connection, DBusMessage * message)
{
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusError error;
	HalDevice *d;
	const char *udi;

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &udi,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "Manager.DeviceExists");
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	HAL_TRACE (("entering, udi=%s", udi));

	d = hal_device_store_find (hald_get_gdl (), udi);

	if (d == NULL)
		d = hal_device_store_find (hald_get_tdl (), udi);

	reply = dbus_message_new_method_return (message);
	dbus_message_iter_init (reply, &iter);
	dbus_message_iter_append_boolean (&iter, d != NULL);

	if (reply == NULL)
		DIE (("No memory"));

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
	return DBUS_HANDLER_RESULT_HANDLED;
}

/** Send signal DeviceAdded(string udi) on the org.freedesktop.Hal.Manager
 *  interface on the object /org/freedesktop/Hal/Manager.
 *
 *  @param  device              The HalDevice added
 */
void
manager_send_signal_device_added (HalDevice *device)
{
	const char *udi = hal_device_get_udi (device);
	DBusMessage *message;
	DBusMessageIter iter;

	HAL_TRACE (("entering, udi=%s", udi));

	message = dbus_message_new_signal ("/org/freedesktop/Hal/Manager",
					   "org.freedesktop.Hal.Manager",
					   "DeviceAdded");

	dbus_message_iter_init (message, &iter);
	dbus_message_iter_append_string (&iter, udi);

	if (!dbus_connection_send (dbus_connection, message, NULL))
		DIE (("error broadcasting message"));

	dbus_message_unref (message);
}

/** Send signal DeviceRemoved(string udi) on the org.freedesktop.Hal.Manager
 *  interface on the object /org/freedesktop/Hal/Manager.
 *
 *  @param  device              The HalDevice removed
 */
void
manager_send_signal_device_removed (HalDevice *device)
{
	const char *udi = hal_device_get_udi (device);
	DBusMessage *message;
	DBusMessageIter iter;

	HAL_TRACE (("entering, udi=%s", udi));

	message = dbus_message_new_signal ("/org/freedesktop/Hal/Manager",
					   "org.freedesktop.Hal.Manager",
					   "DeviceRemoved");

	dbus_message_iter_init (message, &iter);
	dbus_message_iter_append_string (&iter, udi);

	if (!dbus_connection_send (dbus_connection, message, NULL))
		DIE (("error broadcasting message"));

	dbus_message_unref (message);
}

/** Send signal NewCapability(string udi, string capability) on the 
 *  org.freedesktop.Hal.Manager interface on the object 
 *  /org/freedesktop/Hal/Manager.
 *
 *  @param  udi                 Unique Device Id
 *  @param  capability          Capability
 */
void
manager_send_signal_new_capability (HalDevice *device,
				    const char *capability)
{
	const char *udi = hal_device_get_udi (device);
	DBusMessage *message;
	DBusMessageIter iter;

	HAL_TRACE (("entering, udi=%s, cap=%s", udi, capability));

	message = dbus_message_new_signal ("/org/freedesktop/Hal/Manager",
					   "org.freedesktop.Hal.Manager",
					   "NewCapability");

	dbus_message_iter_init (message, &iter);
	dbus_message_iter_append_string (&iter, udi);
	dbus_message_iter_append_string (&iter, capability);

	if (!dbus_connection_send (dbus_connection, message, NULL))
		DIE (("error broadcasting message"));

	dbus_message_unref (message);
}

/** @} */

/**
 * @defgroup DeviceInterface D-BUS interface org.freedesktop.Hal.Device
 * @ingroup HalDaemon
 * @brief D-BUS interface for generic device operations
 * @{
 */

static gboolean
foreach_property_append (HalDevice *device, HalProperty *p,
			 gpointer user_data)
{
	DBusMessageIter *iter = user_data;
	const char *key;
	int type;

	key = hal_property_get_key (p);
	type = hal_property_get_type (p);

	dbus_message_iter_append_dict_key (iter, key);

	switch (type) {
	case DBUS_TYPE_STRING:
		dbus_message_iter_append_string (iter,
						 hal_property_get_string (p));
		break;
	case DBUS_TYPE_INT32:
		dbus_message_iter_append_int32 (iter,
						hal_property_get_int (p));
		break;
	case DBUS_TYPE_UINT64:
		dbus_message_iter_append_uint64 (iter,
						hal_property_get_uint64 (p));
		break;
	case DBUS_TYPE_DOUBLE:
		dbus_message_iter_append_double (iter,
						 hal_property_get_double (p));
		break;
	case DBUS_TYPE_BOOLEAN:
		dbus_message_iter_append_boolean (iter,
						  hal_property_get_bool (p));
		break;
		
	default:
		HAL_WARNING (("Unknown property type %d", type));
		break;
	}

	return TRUE;
}
		
	
	
/** Get all properties on a device.
 *
 *  <pre>
 *  map{string, any} Device.GetAllProperties()
 *
 *    raises org.freedesktop.Hal.NoSuchDevice
 *  </pre>
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @return                     What to do with the message
 */
DBusHandlerResult
device_get_all_properties (DBusConnection * connection,
			   DBusMessage * message)
{
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter iter_dict;
	HalDevice *d;
	const char *udi;

	udi = dbus_message_get_path (message);

	HAL_TRACE (("entering, udi=%s", udi));

	d = hal_device_store_find (hald_get_gdl (), udi);
	if (d == NULL)
		d = hal_device_store_find (hald_get_tdl (), udi);

	if (d == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	dbus_message_iter_init (reply, &iter);
	dbus_message_iter_append_dict (&iter, &iter_dict);

	hal_device_property_foreach (d,
				     foreach_property_append,
				     &iter_dict);

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);

	return DBUS_HANDLER_RESULT_HANDLED;
}


/** Get a property on a device.
 *
 *  <pre>
 *  any Device.GetProperty(string key)
 *  string Device.GetPropertyString(string key)
 *  int Device.GetPropertyInteger(string key)
 *  bool Device.GetPropertyBoolean(string key)
 *  double Device.GetPropertyDouble(string key)
 *
 *    raises org.freedesktop.Hal.NoSuchDevice, 
 *           org.freedesktop.Hal.NoSuchProperty
 *  </pre>
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @return                     What to do with the message
 */
DBusHandlerResult
device_get_property (DBusConnection * connection, DBusMessage * message)
{
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusError error;
	HalDevice *d;
	const char *udi;
	char *key;
	int type;
	HalProperty *p;

	udi = dbus_message_get_path (message);

	HAL_TRACE (("entering, udi=%s", udi));

	d = hal_device_store_find (hald_get_gdl (), udi);
	if (d == NULL)
		d = hal_device_store_find (hald_get_tdl (), udi);

	if (d == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &key,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "GetProperty");
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	p = hal_device_property_find (d, key);
	if (p == NULL) {
		raise_no_such_property (connection, message, udi, key);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	dbus_message_iter_init (reply, &iter);

	type = hal_property_get_type (p);
	switch (type) {
	case DBUS_TYPE_STRING:
		dbus_message_iter_append_string (&iter,
						 hal_property_get_string (p));
		break;
	case DBUS_TYPE_INT32:
		dbus_message_iter_append_int32 (&iter,
						hal_property_get_int (p));
		break;
	case DBUS_TYPE_UINT64:
		dbus_message_iter_append_uint64 (&iter,
						hal_property_get_uint64 (p));
		break;
	case DBUS_TYPE_DOUBLE:
		dbus_message_iter_append_double (&iter,
						 hal_property_get_double (p));
		break;
	case DBUS_TYPE_BOOLEAN:
		dbus_message_iter_append_boolean (&iter,
						  hal_property_get_bool (p));
		break;

	default:
		HAL_WARNING (("Unknown property type %d", type));
		break;
	}

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);

	return DBUS_HANDLER_RESULT_HANDLED;
}


/** Get the type of a property on a device.
 *
 *  <pre>
 *  int Device.GetPropertyType(string key)
 *
 *    raises org.freedesktop.Hal.NoSuchDevice, 
 *           org.freedesktop.Hal.NoSuchProperty
 *  </pre>
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @return                     What to do with the message
 */
DBusHandlerResult
device_get_property_type (DBusConnection * connection,
			  DBusMessage * message)
{
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusError error;
	HalDevice *d;
	const char *udi;
	char *key;
	HalProperty *p;

	udi = dbus_message_get_path (message);

	HAL_TRACE (("entering, udi=%s", udi));

	d = hal_device_store_find (hald_get_gdl (), udi);
	if (d == NULL)
		d = hal_device_store_find (hald_get_tdl (), udi);

	if (d == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &key,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "GetPropertyType");
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	p = hal_device_property_find (d, key);
	if (p == NULL) {
		raise_no_such_property (connection, message, udi, key);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	dbus_message_iter_init (reply, &iter);
	dbus_message_iter_append_int32 (&iter,
					hal_property_get_type (p));

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);

	return DBUS_HANDLER_RESULT_HANDLED;
}


static dbus_bool_t 
sender_has_privileges (DBusConnection *connection, DBusMessage *message)
{
	DBusError error;
	unsigned long user_uid;
	const char *user_base_svc;

	user_base_svc = dbus_message_get_sender (message);
	if (user_base_svc == NULL) {
		HAL_WARNING (("Cannot determine base service of caller"));
		return FALSE;
	}

	HAL_DEBUG (("base_svc = %s", user_base_svc));

	dbus_error_init (&error);
	if ((user_uid = dbus_bus_get_unix_user (connection, user_base_svc, &error)) 
       == (unsigned long) -1) {
		HAL_WARNING (("Could not get uid for connection"));
		return FALSE;
	}

	HAL_INFO (("uid for caller is %ld", user_uid));

	if (user_uid != 0 && user_uid != geteuid()) {
		HAL_WARNING (("uid %d is doesn't have the right priviledges", user_uid));
		return FALSE;
	}

	return TRUE;
}

/** Set a property on a device.
 *
 *  <pre>
 *  void Device.SetProperty(string key, any value)
 *  void Device.SetPropertyString(string key, string value)
 *  void Device.SetPropertyInteger(string key, int value)
 *  void Device.SetPropertyBoolean(string key, bool value)
 *  void Device.SetPropertyDouble(string key, double value)
 *
 *    raises org.freedesktop.Hal.NoSuchDevice, 
 *           org.freedesktop.Hal.NoSuchProperty
 *           org.freedesktop.Hal.TypeMismatch
 *  </pre>
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @return                     What to do with the message
 */
DBusHandlerResult
device_set_property (DBusConnection * connection, DBusMessage * message)
{
	const char *udi;
	char *key;
	int type;
	dbus_bool_t rc;
	HalDevice *device;
	DBusMessageIter iter;
	DBusMessage *reply;

	HAL_TRACE (("entering"));

	udi = dbus_message_get_path (message);

	dbus_message_iter_init (message, &iter);
	type = dbus_message_iter_get_arg_type (&iter);
	if (type != DBUS_TYPE_STRING) {
		raise_syntax (connection, message, "SetProperty");
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	key = dbus_message_iter_get_string (&iter);

	if (!sender_has_privileges (connection, message)) {
		raise_permission_denied (connection, message, "SetProperty: not privileged");
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	HAL_DEBUG (("udi=%s, key=%s", udi, key));

	device = hal_device_store_find (hald_get_gdl (), udi);
	if (device == NULL)
		device = hal_device_store_find (hald_get_tdl (), udi);

	if (device == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	dbus_message_iter_next (&iter);

	/** @todo check permissions of the sender vs property to be modified */

	type = dbus_message_iter_get_arg_type (&iter);
	rc = FALSE;

	switch (type) {
	case DBUS_TYPE_STRING:
		rc = hal_device_property_set_string (device, key,
					     dbus_message_iter_get_string
					     (&iter));
		break;
	case DBUS_TYPE_INT32:
		rc = hal_device_property_set_int (device, key,
					  dbus_message_iter_get_int32
					  (&iter));
		break;
	case DBUS_TYPE_UINT64:
		rc = hal_device_property_set_uint64 (device, key,
					  dbus_message_iter_get_uint64
					  (&iter));
		break;
	case DBUS_TYPE_DOUBLE:
		rc = hal_device_property_set_double (device, key,
					     dbus_message_iter_get_double
					     (&iter));
		break;
	case DBUS_TYPE_BOOLEAN:
		rc = hal_device_property_set_bool (device, key,
					   dbus_message_iter_get_boolean
					   (&iter));
		break;

	default:
		HAL_WARNING (("Unsupported property type %d", type));
		break;
	}

	/* FIXME: temporary pstore test only */
	hal_device_property_set_attribute (device, key, PERSISTENCE, TRUE);
	HAL_WARNING (("FIXME: persistence set for all D-BUS props; "
		      "udi=%s, key=%s", udi, key));

	if (!rc) {
		raise_property_type_error (connection, message, udi, key);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
	return DBUS_HANDLER_RESULT_HANDLED;
}


/** Maximum string length for capabilities; quite a hack :-/ */
#define MAX_CAP_SIZE 2048

/** This function is used to modify the Capabilities property. The reason
 *  for having a dedicated function is that the HAL daemon will broadcast
 *  a signal on the Manager interface to tell applications that the device
 *  have got a new capability.
 *
 *  This is useful as capabilities can be merged after the device is created.
 *  One example of this is networking cards under Linux 2.6; the net.ethernet
 *  capability is not merged when the device is initially found by looking in 
 *  /sys/devices; it is merged when the /sys/classes tree is searched.
 *
 *  Note that the signal is emitted every time this method is invoked even
 *  though the capability already existed. This is useful in the above
 *  scenario when the PCI class says ethernet networking card but we yet
 *  don't have enough information to fill in the net.* and net.ethernet.*
 *  fields since this only happens when we visit the /sys/classes tree.
 *
 *  <pre>
 *  void Device.AddCapability(string capability)
 *
 *    raises org.freedesktop.Hal.NoSuchDevice, 
 *    raises org.freedesktop.Hal.PermissionDenied, 
 *  </pre>
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @return                     What to do with the message
 */
DBusHandlerResult
device_add_capability (DBusConnection * connection, DBusMessage * message)
{
	const char *udi;
	const char *capability;
	const char *caps;
	HalDevice *d;
	DBusMessage *reply;
	DBusError error;
	char buf[MAX_CAP_SIZE];

	HAL_TRACE (("entering"));

	if (!sender_has_privileges (connection, message)) {
		raise_permission_denied (connection, message, "AddCapability: not privileged");
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	udi = dbus_message_get_path (message);

	d = hal_device_store_find (hald_get_gdl (), udi);
	if (d == NULL)
		d = hal_device_store_find (hald_get_tdl (), udi);

	if (d == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &capability,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "AddCapability");
		return DBUS_HANDLER_RESULT_HANDLED;
	}


	caps = hal_device_property_get_string (d, "info.capabilities");
	if (caps == NULL) {
		hal_device_property_set_string (d, "info.capabilities",
					capability);
	} else {
		if (strstr (caps, capability) == NULL) {
			snprintf (buf, MAX_CAP_SIZE, "%s %s", caps,
				  capability);
			hal_device_property_set_string (d, "info.capabilities",
						buf);
		}
	}

	manager_send_signal_new_capability (d, capability);

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
	return DBUS_HANDLER_RESULT_HANDLED;
}



/** Remove a property on a device.
 *
 *  <pre>
 *  void Device.RemoveProperty(string key)
 *
 *    raises org.freedesktop.Hal.NoSuchDevice, 
 *           org.freedesktop.Hal.NoSuchProperty
 *           org.freedesktop.Hal.PermissionDenied
 *  </pre>
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @return                     What to do with the message
 */
DBusHandlerResult
device_remove_property (DBusConnection * connection, DBusMessage * message)
{
	const char *udi;
	char *key;
	HalDevice *d;
	DBusMessage *reply;
	DBusError error;

	HAL_TRACE (("entering"));

	udi = dbus_message_get_path (message);

	if (!sender_has_privileges (connection, message)) {
		raise_permission_denied (connection, message, "RemoveProperty: not privileged");
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	d = hal_device_store_find (hald_get_gdl (), udi);
	if (d == NULL)
		d = hal_device_store_find (hald_get_tdl (), udi);

	if (d == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &key,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "RemoveProperty");
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (!hal_device_property_remove (d, key)) {
		raise_no_such_property (connection, message, udi, key);
		return DBUS_HANDLER_RESULT_HANDLED;
	}


	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
	return DBUS_HANDLER_RESULT_HANDLED;
}


/** Determine if a property exists
 *
 *  <pre>
 *  bool Device.PropertyExists(string key)
 *
 *    raises org.freedesktop.Hal.NoSuchDevice, 
 *  </pre>
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @return                     What to do with the message
 */
DBusHandlerResult
device_property_exists (DBusConnection * connection, DBusMessage * message)
{
	const char *udi;
	char *key;
	HalDevice *d;
	DBusMessage *reply;
	DBusError error;
	DBusMessageIter iter;

	HAL_TRACE (("entering"));

	udi = dbus_message_get_path (message);

	d = hal_device_store_find (hald_get_gdl (), udi);
	if (d == NULL)
		d = hal_device_store_find (hald_get_tdl (), udi);

	if (d == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &key,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "RemoveProperty");
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	dbus_message_iter_init (reply, &iter);
	dbus_message_iter_append_boolean (&iter,
					  hal_device_has_property (d, key));

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
	return DBUS_HANDLER_RESULT_HANDLED;
}


/** Determine if a device has a capability
 *
 *  <pre>
 *  bool Device.QueryCapability(string capability_name)
 *
 *    raises org.freedesktop.Hal.NoSuchDevice, 
 *  </pre>
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @return                     What to do with the message
 */
DBusHandlerResult
device_query_capability (DBusConnection * connection,
			 DBusMessage * message)
{
	dbus_bool_t rc;
	const char *udi;
	const char *caps;
	char *capability;
	HalDevice *d;
	DBusMessage *reply;
	DBusError error;
	DBusMessageIter iter;

	HAL_TRACE (("entering"));

	udi = dbus_message_get_path (message);

	d = hal_device_store_find (hald_get_gdl (), udi);
	if (d == NULL)
		d = hal_device_store_find (hald_get_tdl (), udi);

	if (d == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &capability,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "QueryCapability");
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	rc = FALSE;
	caps = hal_device_property_get_string (d, "info.capabilities");
	if (caps != NULL) {
		char **capsv, **iter;

		capsv = g_strsplit (caps, " ", 0);
		for (iter = capsv; *iter != NULL; iter++) {
			if (strcmp (*iter, capability) == 0) {
				rc = TRUE;
				break;
			}
		}

		g_strfreev (capsv);
	}

	dbus_free (capability);

	dbus_message_iter_init (reply, &iter);
	dbus_message_iter_append_boolean (&iter, rc);

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
	return DBUS_HANDLER_RESULT_HANDLED;
}

static GHashTable *services_with_locks = NULL;

/** Grab an advisory lock on a device.
 *
 *  <pre>
 *  bool Device.Lock(string reason)
 *
 *    raises org.freedesktop.Hal.NoSuchDevice, 
 *           org.freedesktop.Hal.DeviceAlreadyLocked
 *  </pre>
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @return                     What to do with the message
 */
DBusHandlerResult
device_lock (DBusConnection * connection,
	     DBusMessage * message)
{
	const char *udi;
	HalDevice *d;
	DBusMessage *reply;
	dbus_bool_t already_locked;
	DBusError error;
	char *reason;
	const char *sender;

	HAL_TRACE (("entering"));

	udi = dbus_message_get_path (message);

	d = hal_device_store_find (hald_get_gdl (), udi);
	if (d == NULL)
		d = hal_device_store_find (hald_get_tdl (), udi);

	if (d == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	already_locked = hal_device_property_get_bool (d, "info.locked");

	if (already_locked) {
		raise_device_already_locked (connection, message, d);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &reason,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "Lock");
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	sender = dbus_message_get_sender (message);

	hal_device_property_set_bool (d, "info.locked", TRUE);
	hal_device_property_set_string (d, "info.locked.reason", reason);
	hal_device_property_set_string (d, "info.locked.dbus_service",
					sender);

	if (services_with_locks == NULL) {
		services_with_locks =
			g_hash_table_new_full (g_str_hash,
					       g_str_equal,
					       g_free,
					       g_object_unref);
	}

	g_hash_table_insert (services_with_locks, g_strdup (sender),
			     g_object_ref (d));

	dbus_free (reason);

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
	return DBUS_HANDLER_RESULT_HANDLED;
}

/** Release an advisory lock on a device.
 *
 *  <pre>
 *  bool Device.Unlock()
 *
 *    raises org.freedesktop.Hal.NoSuchDevice, 
 *           org.freedesktop.Hal.DeviceNotLocked,
 *           org.freedesktop.Hal.PermissionDenied
 *  </pre>
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @return                     What to do with the message
 */
DBusHandlerResult
device_unlock (DBusConnection * connection,
	       DBusMessage * message)
{
	dbus_bool_t rc;
	const char *udi;
	HalDevice *d;
	DBusMessage *reply;
	DBusError error;
	const char *sender;

	HAL_TRACE (("entering"));

	udi = dbus_message_get_path (message);

	d = hal_device_store_find (hald_get_gdl (), udi);
	if (d == NULL)
		d = hal_device_store_find (hald_get_tdl (), udi);

	if (d == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "Unlock");
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	rc = hal_device_property_get_bool (d, "info.locked");

	if (!rc) {
		raise_device_not_locked (connection, message, d);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	sender = dbus_message_get_sender (message);

	if (strcmp (sender, hal_device_property_get_string (
			    d, "info.locked.dbus_service")) != 0) {
		char *reason;

		reason = g_strdup_printf ("Service '%s' does not own the "
					  "lock on %s", sender,
					  hal_device_get_udi (d));

		raise_permission_denied (connection, message, reason);

		g_free (reason);

		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (g_hash_table_lookup (services_with_locks, sender))
		g_hash_table_remove (services_with_locks, sender);
	else {
		HAL_WARNING (("Service '%s' was not in the list of services "
			      "with locks!", sender));
	}

	hal_device_property_remove (d, "info.locked");
	hal_device_property_remove (d, "info.locked.reason");
	hal_device_property_remove (d, "info.locked.dbus_service");

	/* FIXME?  Pointless? */
	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
	return DBUS_HANDLER_RESULT_HANDLED;
}

/** Counter for atomic updating */
static int atomic_count = 0;

/** Number of updates pending */
static int num_pending_updates = 0;

/** Structure for queing updates */
typedef struct PendingUpdate_s {
	char *udi;                    /**< udi of device */
	char *key;                    /**< key of property; free when done */
	dbus_bool_t removed;          /**< true iff property was removed */
	dbus_bool_t added;            /**< true iff property was added */
	struct PendingUpdate_s *next; /**< next update or #NULL */
} PendingUpdate;

static PendingUpdate *pending_updates_head = NULL;

/** Begin an atomic update - this is useful for updating several properties
 *  in one go.
 *
 *  Note that an atomic update is recursive - use with caution!
 */
void
device_property_atomic_update_begin (void)
{
	atomic_count++;
}

/** End an atomic update.
 *
 *  Note that an atomic update is recursive - use with caution!
 */
void
device_property_atomic_update_end (void)
{
	PendingUpdate *pu_iter = NULL;
	PendingUpdate *pu_iter_next = NULL;
	PendingUpdate *pu_iter2 = NULL;

	--atomic_count;

	if (atomic_count < 0) {
		HAL_WARNING (("*** atomic_count = %d < 0 !!",
			      atomic_count));
		atomic_count = 0;
	}

	if (atomic_count == 0 && num_pending_updates > 0) {
		DBusMessage *message;
		DBusMessageIter iter;

		for (pu_iter = pending_updates_head;
		     pu_iter != NULL; pu_iter = pu_iter_next) {
			int num_updates_this;

			pu_iter_next = pu_iter->next;

			if (pu_iter->udi == NULL)
				goto have_processed;

			/* count number of updates for this device */
			num_updates_this = 0;
			for (pu_iter2 = pu_iter;
			     pu_iter2 != NULL; pu_iter2 = pu_iter2->next) {
				if (strcmp (pu_iter2->udi, pu_iter->udi) == 0)
					num_updates_this++;
			}

			/* prepare message */
			message = dbus_message_new_signal (
				pu_iter->udi,
				"org.freedesktop.Hal.Device",
				"PropertyModified");
			dbus_message_iter_init (message, &iter);
			dbus_message_iter_append_int32 (&iter,
							num_updates_this);
			for (pu_iter2 = pu_iter; pu_iter2 != NULL;
			     pu_iter2 = pu_iter2->next) {
				if (strcmp (pu_iter2->udi, pu_iter->udi) == 0) {
					dbus_message_iter_append_string
					    (&iter, pu_iter2->key);
					dbus_message_iter_append_boolean
					    (&iter, pu_iter2->removed);
					dbus_message_iter_append_boolean
					    (&iter, pu_iter2->added);

					/* signal this is already processed */
					if (pu_iter2 != pu_iter) {
						g_free (pu_iter2->udi);
						pu_iter2->udi = NULL;
					}
				}
			}


			if (!dbus_connection_send
			    (dbus_connection, message, NULL))
				DIE (("error broadcasting message"));

			dbus_message_unref (message);

		      have_processed:
			g_free (pu_iter->key);
			g_free (pu_iter);
		}		/* for all updates */

		num_pending_updates = 0;
		pending_updates_head = NULL;
	}
}



void
device_send_signal_property_modified (HalDevice *device, const char *key,
				      dbus_bool_t added, dbus_bool_t removed)
{
	const char *udi = hal_device_get_udi (device);
	DBusMessage *message;
	DBusMessageIter iter;

/*
    HAL_INFO(("Entering, udi=%s, key=%s, in_gdl=%s, removed=%s added=%s",
              device->udi, key, 
              in_gdl ? "true" : "false",
              removed ? "true" : "false",
              added ? "true" : "false"));
*/

	if (atomic_count > 0) {
		PendingUpdate *pu;

		pu = g_new0 (PendingUpdate, 1);
		pu->udi = g_strdup (udi);
		pu->key = g_strdup (key);
		pu->removed = removed;
		pu->added = added;
		pu->next = pending_updates_head;

		pending_updates_head = pu;
		num_pending_updates++;
	} else {
		message = dbus_message_new_signal (udi,
						  "org.freedesktop.Hal.Device",
						   "PropertyModified");

		dbus_message_iter_init (message, &iter);
		dbus_message_iter_append_int32 (&iter, 1);
		dbus_message_iter_append_string (&iter, key);
		dbus_message_iter_append_boolean (&iter, removed);
		dbus_message_iter_append_boolean (&iter, added);

		if (!dbus_connection_send (dbus_connection, message, NULL))
			DIE (("error broadcasting message"));

		dbus_message_unref (message);
	}
}

/** Emits a condition on a device; the device has to be in the GDL for
 *  this function to have effect.
 *
 *  Is intended for non-continuous events on the device like
 *  ProcesserOverheating, BlockDeviceGotDevice, e.g. conditions that
 *  are exceptional and may not be inferred by looking at properties
 *  (though some may).
 *
 *  This function accepts a number of parameters that are passed along
 *  in the D-BUS message. The recipient is supposed to extract the parameters
 *  himself, by looking at the HAL specification.
 *
 * @param  udi                  The UDI for this device
 * @param  condition_name       Name of condition
 * @param  first_arg_type       Type of the first argument
 * @param  ...                  value of first argument, list of additional
 *                              type-value pairs. Must be terminated with
 *                              DBUS_TYPE_INVALID
 */
void
device_send_signal_condition (HalDevice *device, const char *condition_name,
			      int first_arg_type, ...)
{
	const char *udi = hal_device_get_udi (device);
	DBusMessage *message;
	DBusMessageIter iter;
	va_list var_args;

	message = dbus_message_new_signal (udi,
					   "org.freedesktop.Hal.Device",
					   "Condition");
	dbus_message_iter_init (message, &iter);
	dbus_message_iter_append_string (&iter, condition_name);

	va_start (var_args, first_arg_type);
	dbus_message_append_args_valist (message, first_arg_type,
					 var_args);
	va_end (var_args);

	if (!dbus_connection_send (dbus_connection, message, NULL))
		DIE (("error broadcasting message"));

	dbus_message_unref (message);
}



static gboolean
reinit_dbus (gpointer user_data)
{
	if (hald_dbus_init ())
		return FALSE;
	else
		return TRUE;
}

static void
service_deleted (DBusMessage *message)
{
	char *service_name;
	HalDevice *d;

	if (!dbus_message_get_args (message, NULL,
				    DBUS_TYPE_STRING, &service_name,
				    DBUS_TYPE_INVALID)) {
		HAL_ERROR (("Invalid ServiceDeleted signal from bus!"));
		return;
	}

	d = g_hash_table_lookup (services_with_locks, service_name);

	if (d != NULL) {
		hal_device_property_remove (d, "info.locked");
		hal_device_property_remove (d, "info.locked.reason");
		hal_device_property_remove (d, "info.locked.dbus_service");

		g_hash_table_remove (services_with_locks, service_name);
	}

	dbus_free (service_name);
}

/** Message handler for method invocations. All invocations on any object
 *  or interface is routed through this function.
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @param  user_data           User data
 *  @return                     What to do with the message
 */
static DBusHandlerResult
filter_function (DBusConnection * connection,
		 DBusMessage * message, void *user_data)
{
/*
    HAL_INFO (("obj_path=%s interface=%s method=%s", 
	       dbus_message_get_path(message), 
	       dbus_message_get_interface(message),
	       dbus_message_get_member(message)));
*/

	if (dbus_message_is_signal (message,
				    DBUS_INTERFACE_ORG_FREEDESKTOP_LOCAL,
				    "Disconnected") &&
	    strcmp (dbus_message_get_path (message),
		    DBUS_PATH_ORG_FREEDESKTOP_LOCAL) == 0) {

		dbus_connection_unref (dbus_connection);
		g_timeout_add (3000, reinit_dbus, NULL);

	} else if (dbus_message_is_signal (message,
					   DBUS_INTERFACE_ORG_FREEDESKTOP_DBUS,
					   "ServiceDeleted")) {

		if (services_with_locks != NULL)
			service_deleted (message);
	} else if (dbus_message_is_method_call (message,
					 "org.freedesktop.Hal.Manager",
					 "GetAllDevices") &&
		   strcmp (dbus_message_get_path (message),
			   "/org/freedesktop/Hal/Manager") == 0) {
		return manager_get_all_devices (connection, message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Manager",
						"DeviceExists") &&
		   strcmp (dbus_message_get_path (message),
			   "/org/freedesktop/Hal/Manager") == 0) {
		return manager_device_exists (connection, message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Manager",
						"FindDeviceStringMatch") &&
		   strcmp (dbus_message_get_path (message),
			   "/org/freedesktop/Hal/Manager") == 0) {
		return manager_find_device_string_match (connection,
							 message);
	} else
	    if (dbus_message_is_method_call
		(message, "org.freedesktop.Hal.Manager",
		 "FindDeviceByCapability")
		&& strcmp (dbus_message_get_path (message),
			   "/org/freedesktop/Hal/Manager") == 0) {
		return manager_find_device_by_capability (connection,
							  message);
	}

	else if (dbus_message_is_method_call (message,
					      "org.freedesktop.Hal.Device",
					      "GetAllProperties")) {
		return device_get_all_properties (connection, message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"GetProperty")) {
		return device_get_property (connection, message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"GetPropertyString")) {
		return device_get_property (connection, message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"GetPropertyInteger")) {
		return device_get_property (connection, message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"GetPropertyBoolean")) {
		return device_get_property (connection, message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"GetPropertyDouble")) {
		return device_get_property (connection, message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"SetProperty")) {
		return device_set_property (connection, message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"SetPropertyString")) {
		return device_set_property (connection, message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"SetPropertyInteger")) {
		return device_set_property (connection, message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"SetPropertyBoolean")) {
		return device_set_property (connection, message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"SetPropertyDouble")) {
		return device_set_property (connection, message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"RemoveProperty")) {
		return device_remove_property (connection, message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"GetPropertyType")) {
		return device_get_property_type (connection, message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"PropertyExists")) {
		return device_property_exists (connection, message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"AddCapability")) {
		return device_add_capability (connection, message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"QueryCapability")) {
		return device_query_capability (connection, message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"Lock")) {
		return device_lock (connection, message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"Unlock")) {
		return device_unlock (connection, message);
	} else
		osspec_filter_function (connection, message, user_data);

	return DBUS_HANDLER_RESULT_HANDLED;
}

gboolean
hald_dbus_init (void)
{
	DBusError dbus_error;

	dbus_connection_set_change_sigpipe (TRUE);

	dbus_error_init (&dbus_error);
	dbus_connection = dbus_bus_get (DBUS_BUS_SYSTEM, &dbus_error);
	if (dbus_connection == NULL) {
		HAL_ERROR (("dbus_bus_get(): %s", dbus_error.message));
		return FALSE;
	}

	dbus_connection_setup_with_g_main (dbus_connection, NULL);
	dbus_connection_set_exit_on_disconnect (dbus_connection, FALSE);

	dbus_bus_acquire_service (dbus_connection, "org.freedesktop.Hal",
				  0, &dbus_error);
	if (dbus_error_is_set (&dbus_error)) {
		HAL_ERROR (("dbus_bus_acquire_service(): %s",
			    dbus_error.message));
		return FALSE;
	}

	dbus_connection_add_filter (dbus_connection, filter_function, NULL,
				    NULL);

	dbus_bus_add_match (dbus_connection,
			    "type='signal',"
			    "interface='"DBUS_INTERFACE_ORG_FREEDESKTOP_DBUS"',"
			    "sender='"DBUS_SERVICE_ORG_FREEDESKTOP_DBUS"',"
			    "member='ServiceDeleted'",
			    NULL);

	return TRUE;
}

/** @} */

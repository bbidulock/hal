/***************************************************************************
 *
 * device_pm.c - Various power management related utilities that need to use
 *               HalDevice. This is not suitable for use in addons and probers.
 *
 * Copyright (C) 2005-2007 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2005 Danny Kukawka <danny.kukawka@web.de>
 *
 * Licensed under the Academic Free License version 2.1
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

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <stdint.h>

#include <glib.h>

#include "logger.h"
#include "util_pm.h"
#include "device_pm.h"
#include "device_store.h"

/** 
 *  device_pm_abstract_props:
 *  @d:		Valid battery HalDevice
 *
 * Convert the buggy 'reporting' keys into 'charge_level' keys so stuff like
 * desktop power managers do not have to deal with odd quirks.
 */
void
device_pm_abstract_props (HalDevice *d)
{
	const char *reporting_unit;
	int reporting_current;
	int reporting_lastfull;
	int reporting_rate;
	int normalised_current;
	int normalised_lastfull;
	int normalised_rate;
	int design_voltage;
	int voltage;
	gboolean charging;
	gboolean discharging;

	/* get all the data we know */
	reporting_unit = hal_device_property_get_string (d,
					"battery.reporting.unit");
	reporting_current = hal_device_property_get_int (d,
					"battery.reporting.current");
	reporting_lastfull = hal_device_property_get_int (d,
					"battery.reporting.last_full");
	reporting_rate = hal_device_property_get_int (d,
					"battery.reporting.rate");

	/* ACPI gives out the special 'Ones' value for rate when it's unable
	 * to calculate the true rate. We should set the rate zero, and wait
	 * for the BIOS to stabilise. */
	if (reporting_rate == 0xffff) {
		reporting_rate = 0;
	}

	/* We are converting the unknown units into mWh because of ACPI's nature
	 * of not having a standard unit. */
	if (reporting_unit && strcmp (reporting_unit, "mAh") == 0) {
		/* convert mAh to mWh by multiplying by voltage.  due to the
		 * general wonkiness of ACPI implementations, this is a lot
		 * harder than it should have to be... */

		design_voltage = hal_device_property_get_int (d, "battery.voltage.design");
		voltage = hal_device_property_get_int (d, "battery.voltage.current");

		/* Just in case we don't get design voltage information, then
		 * this will pretend that we have 1V.  This degrades our
		 * ability to report accurate times on multi-battery systems
		 * but will always prevent negative charge levels and allow
		 * accurate reporting on single-battery systems. */
		if (design_voltage <= 0)
			design_voltage = 1000; /* mV */

		/* If the current voltage is unknown, smaller than 50% of design voltage (fd.o #8593) 
		 * or greater than design, then use design voltage. */
		if (voltage < (design_voltage/2)  || voltage > design_voltage) {
			HAL_DEBUG (("Current voltage is unknown, smaller than 50%% or greater than design"));
			voltage = design_voltage;
		}

		normalised_current = (reporting_current * voltage) / 1000;
		normalised_lastfull = (reporting_lastfull * voltage) / 1000;
		normalised_rate = (reporting_rate * voltage) / 1000;
	} else {
		/* handle as if mWh (which don't need conversion), which is
		 * the most common case. */
		normalised_current = reporting_current;
		normalised_lastfull = reporting_lastfull;
		normalised_rate = reporting_rate;
	}

	/*
	 * Check the normalised keys to see if positive.
	 */
	if (normalised_current < 0)
		normalised_current = 0;
	if (normalised_lastfull < 0)
		normalised_lastfull = 0;
	if (normalised_rate < 0)
		normalised_rate = 0;

	/* Some laptops report a rate even when not charging or discharging.
	 * If not charging and not discharging force rate to be zero. */
	charging = hal_device_property_get_bool (d, "battery.rechargeable.is_charging");
	discharging = hal_device_property_get_bool (d, "battery.rechargeable.is_discharging");

	if (!charging && !discharging)
		normalised_rate = 0;

	/* Some laptops report current charge much larger than
	 * full charge when at 100%.  Clamp back down to 100%. */
	if (normalised_current > normalised_lastfull)
	  	normalised_current = normalised_lastfull;

	hal_device_property_set_int (d, "battery.charge_level.current", normalised_current);
	hal_device_property_set_int (d, "battery.charge_level.last_full", normalised_lastfull);
	hal_device_property_set_int (d, "battery.charge_level.rate", normalised_rate);
}

/** 
 *  device_pm_calculate_percentage:
 *  @d:		Valid battery HalDevice
 *
 * Calculate the percentage from the current levels and the last full level
 * when the hardware has not given us a value.
 */
void
device_pm_calculate_percentage (HalDevice *d)
{
	int percentage;
	int current;
	int lastfull;

	/* default to fully charge to avoid triggering low power warnings on
	 * really broken batteries */
	percentage = 100;

	/* use the charge level compared to the last full amount */
	current = hal_device_property_get_int (d, "battery.charge_level.current");
	lastfull = hal_device_property_get_int (d, "battery.charge_level.last_full");

	/* make sure we have current */
	if (current <= 0) {
		HAL_WARNING (("battery.charge_level.current %i, returning -1!", current));
	} else if (lastfull <= 0) {
		HAL_WARNING (("battery.charge_level.lastfull %i, percentage returning -1!", lastfull));
	} else {
		percentage = ((double) current / (double) lastfull) * 100;
		/* Some bios's will report this out of range of 0..100, limit it here */
		if (percentage > 100)
			percentage = 100;
		else if (percentage < 0)
			percentage = 1;
	}
	hal_device_property_set_int (d, "battery.charge_level.percentage", percentage);
}

/** 
 *  device_pm_calculate_time:
 *  @d:		Valid battery HalDevice
 *
 * Calculate the time from the rate and the last full level
 * when the hardware has not given us a time value.
 */
void
device_pm_calculate_time (HalDevice *d)
{
	int time;

	time = util_compute_time_remaining (
		hal_device_get_udi (d), 
		hal_device_property_get_int (d, "battery.charge_level.rate"),
		hal_device_property_get_int (d, "battery.charge_level.current"),
		hal_device_property_get_int (d, "battery.charge_level.last_full"),
		hal_device_property_get_bool (d, "battery.rechargeable.is_discharging"),
		hal_device_property_get_bool (d, "battery.rechargeable.is_charging"),
		hal_device_property_get_bool (d, "battery.remaining_time.calculate_per_time"));

	/* zero time is unknown */
	if (time > 0)
		hal_device_property_set_int (d, "battery.remaining_time", time);
	else
		hal_device_property_remove (d, "battery.remaining_time");
}

/** 
 *  device_pm_remove_optional_props:
 *  @d:		Valid battery HalDevice
 *
 *  Removes all the optional hardware battery.* keys., i.e. the ones that are
 *  no longer valid when the battery cell is removed or changed.
 *  If the battery _device_ completely vanishes (e.g. in a docking bay) then
 *  the HalDevice should be completely removed from the device tree.
 *
 *  Note: Removing a key that doesn't exist is OK.
 */
void
device_pm_remove_optional_props (HalDevice *d)
{
	hal_device_property_remove (d, "battery.is_rechargeable");
	hal_device_property_remove (d, "battery.rechargeable.is_charging");
	hal_device_property_remove (d, "battery.rechargeable.is_discharging");
	hal_device_property_remove (d, "battery.vendor");
	hal_device_property_remove (d, "battery.model");
	hal_device_property_remove (d, "battery.serial");
	hal_device_property_remove (d, "battery.reporting.technology");
	hal_device_property_remove (d, "battery.technology");
	hal_device_property_remove (d, "battery.vendor");
	hal_device_property_remove (d, "battery.charge_level.unit");
	hal_device_property_remove (d, "battery.charge_level.current");
	hal_device_property_remove (d, "battery.charge_level.percentage");
	hal_device_property_remove (d, "battery.charge_level.last_full");
	hal_device_property_remove (d, "battery.charge_level.design");
	hal_device_property_remove (d, "battery.charge_level.capacity_state");
	hal_device_property_remove (d, "battery.charge_level.warning");
	hal_device_property_remove (d, "battery.charge_level.low");
	hal_device_property_remove (d, "battery.charge_level.granularity_1");
	hal_device_property_remove (d, "battery.charge_level.granularity_2");
	hal_device_property_remove (d, "battery.charge_level.rate");
	hal_device_property_remove (d, "battery.voltage.unit");
	hal_device_property_remove (d, "battery.voltage.design");
	hal_device_property_remove (d, "battery.voltage.current");
	hal_device_property_remove (d, "battery.alarm.unit");
	hal_device_property_remove (d, "battery.alarm.design");
	hal_device_property_remove (d, "battery.reporting.current");
	hal_device_property_remove (d, "battery.reporting.last_full");
	hal_device_property_remove (d, "battery.reporting.design");
	hal_device_property_remove (d, "battery.reporting.rate");
	hal_device_property_remove (d, "battery.reporting.warning");
	hal_device_property_remove (d, "battery.reporting.low");
	hal_device_property_remove (d, "battery.reporting.granularity_1");
	hal_device_property_remove (d, "battery.reporting.granularity_2");
	hal_device_property_remove (d, "battery.reporting.unit");
	hal_device_property_remove (d, "battery.remaining_time");
}

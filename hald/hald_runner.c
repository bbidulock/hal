/***************************************************************************
 * CVSID: $Id$
 *
 * hald_runner.c - Interface to the hal runner helper daemon
 *
 * Copyright (C) 2006 Sjoerd Simons, <sjoerd@luon.net>
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

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <sys/utsname.h>
#include <stdio.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <dbus/dbus-glib-lowlevel.h>
#include <string.h>

#include <sys/types.h>
#include <signal.h>

#include "hald.h"
#include "util.h"
#include "logger.h"
#include "hald_dbus.h"
#include "hald_runner.h"

#ifdef HAVE_CONKIT
#include "ck-tracker.h"
#endif

typedef struct {
	HalDevice *d;
	HalRunTerminatedCB cb;
	gpointer data1;
	gpointer data2;
} HelperData;

#define DBUS_SERVER_ADDRESS "unix:tmpdir=" HALD_SOCKET_DIR

static DBusConnection *runner_connection = NULL;
static HaldRunnerRunNotify method_run_notify = NULL;
static gpointer method_run_notify_userdata = NULL;

typedef struct {
	GPid pid;
	HalDevice *device;
	HalRunTerminatedCB cb;
	gpointer data1;
	gpointer data2;
} RunningProcess;

/* list of RunningProcess */
static GSList *running_processes = NULL;

static void
running_processes_remove_device (HalDevice * device)
{
	GSList *i;
	GSList *j;

	for (i = running_processes; i != NULL; i = j) {
		RunningProcess *rp;

		j = g_slist_next (i);
		rp = i->data;

		if (rp->device == device) {
			g_free (rp);
			running_processes =
			    g_slist_delete_link (running_processes, i);
		}

	}
}

void
runner_device_finalized (HalDevice * device)
{
	running_processes_remove_device (device);
}


static DBusHandlerResult
runner_server_message_handler (DBusConnection * connection,
			       DBusMessage * message, void *user_data)
{

	HAL_INFO (("runner_server_message_handler: destination=%s obj_path=%s interface=%s method=%s", dbus_message_get_destination (message), dbus_message_get_path (message), dbus_message_get_interface (message), dbus_message_get_member (message)));
	if (dbus_message_is_signal (message,
				    "org.freedesktop.HalRunner",
				    "StartedProcessExited")) {
		dbus_uint64_t dpid;
		DBusError error;
		dbus_error_init (&error);
		if (dbus_message_get_args (message, &error,
					   DBUS_TYPE_INT64, &dpid,
					   DBUS_TYPE_INVALID)) {
			GSList *i;
			GPid pid;

			pid = (GPid) dpid;

			HAL_INFO (("Previously started process with pid %d exited", pid));

			for (i = running_processes; i != NULL;
			     i = g_slist_next (i)) {
				RunningProcess *rp;

				rp = i->data;

				if (rp->pid == pid) {
					rp->cb (rp->device, 0, 0, NULL,
						rp->data1, rp->data2);
					g_free (rp);
					running_processes =
					    g_slist_delete_link
					    (running_processes, i);
					break;
				}
			}
		}
	} else if (dbus_message_is_signal (message,
					   DBUS_INTERFACE_LOCAL,
					   "Disconnected") &&
		   strcmp (dbus_message_get_path (message),
			   DBUS_PATH_LOCAL) == 0) {
		HAL_INFO (("runner process disconnected"));
		dbus_connection_unref (connection);
	}

	return DBUS_HANDLER_RESULT_HANDLED;
}

static void
runner_server_unregister_handler (DBusConnection * connection,
				  void *user_data)
{
	HAL_INFO (("========================================"));
	HAL_INFO (("runner_server_unregister_handler"));
	HAL_INFO (("========================================"));
}


static void
handle_connection (DBusServer * server,
		   DBusConnection * new_connection, void *data)
{

	if (runner_connection == NULL) {
		DBusObjectPathVTable vtable =
		    { &runner_server_unregister_handler,
			&runner_server_message_handler,
			NULL, NULL, NULL, NULL
		};

		runner_connection = new_connection;
		dbus_connection_ref (new_connection);
		dbus_connection_setup_with_g_main (new_connection, NULL);

		HAL_INFO (("runner connection is %p", new_connection));

		dbus_connection_register_fallback (new_connection,
						   "/org/freedesktop",
						   &vtable, NULL);

		/* dbus_server_unref(server); */

	}
}

static GPid runner_pid;
static DBusServer *runner_server = NULL;
static guint runner_watch;


static void
runner_died (GPid pid, gint status, gpointer data)
{
	g_spawn_close_pid (pid);
	DIE (("Runner died"));
}

void
hald_runner_stop_runner (void)
{
	if (runner_server != NULL) {
		DBusMessage *msg;

		/* Don't care about running processes anymore */

		HAL_INFO (("running_processes %p, num = %d",
			   running_processes,
			   g_slist_length (running_processes)));

		g_slist_foreach (running_processes, (GFunc) g_free, NULL);
		g_slist_free (running_processes);
		running_processes = NULL;

		HAL_INFO (("Killing runner with pid %d", runner_pid));

		g_source_remove (runner_watch);
		g_spawn_close_pid (runner_pid);

		msg =
		    dbus_message_new_method_call
		    ("org.freedesktop.HalRunner",
		     "/org/freedesktop/HalRunner",
		     "org.freedesktop.HalRunner", "Shutdown");
		if (msg == NULL)
			DIE (("No memory"));
		dbus_connection_send (runner_connection, msg, NULL);
		dbus_message_unref (msg);

		dbus_server_disconnect (runner_server);
		dbus_server_unref (runner_server);
		runner_server = NULL;

	}
}

gboolean
hald_runner_start_runner (void)
{
	DBusError err;
	GError *error = NULL;
	char *argv[] = { NULL, NULL };
	char *env[] = { NULL, NULL, NULL, NULL };
	const char *hald_runner_path;
	char *server_address;

	running_processes = NULL;

	dbus_error_init (&err);
	runner_server = dbus_server_listen (DBUS_SERVER_ADDRESS, &err);
	if (runner_server == NULL) {
		HAL_ERROR (("Cannot create D-BUS server for the runner"));
		goto error;
	}

	dbus_server_setup_with_g_main (runner_server, NULL);
	dbus_server_set_new_connection_function (runner_server,
						 handle_connection, NULL,
						 NULL);


	argv[0] = "hald-runner";
	server_address = dbus_server_get_address (runner_server);
	env[0] = g_strdup_printf ("HALD_RUNNER_DBUS_ADDRESS=%s",
				  server_address);
	dbus_free (server_address);
	hald_runner_path = g_getenv ("HALD_RUNNER_PATH");
	if (hald_runner_path != NULL) {
		env[1] =
		    g_strdup_printf ("PATH=%s:" PACKAGE_LIBEXEC_DIR ":"
				     PACKAGE_SCRIPT_DIR ":"
				     PACKAGE_BIN_DIR, hald_runner_path);
	} else {
		env[1] =
		    g_strdup_printf ("PATH=" PACKAGE_LIBEXEC_DIR ":"
				     PACKAGE_SCRIPT_DIR ":"
				     PACKAGE_BIN_DIR);
	}

	/*env[2] = "DBUS_VERBOSE=1"; */


	if (!g_spawn_async
	    (NULL, argv, env,
	     G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH, NULL, NULL,
	     &runner_pid, &error)) {
		HAL_ERROR (("Could not spawn runner : '%s'",
			    error->message));
		g_error_free (error);
		goto error;
	}
	g_free (env[0]);
	g_free (env[1]);

	HAL_INFO (("Runner has pid %d", runner_pid));

	runner_watch = g_child_watch_add (runner_pid, runner_died, NULL);
	while (runner_connection == NULL) {
		/* Wait for the runner */
		g_main_context_iteration (NULL, TRUE);
	}
	return TRUE;

      error:
	if (runner_server != NULL)
		dbus_server_unref (runner_server);
	return FALSE;
}

static void
add_property_to_msg (HalDevice * device,
		     const char *key, gpointer user_data)
{
	char *prop_upper, *value;
	char *c;
	gchar *env;
	DBusMessageIter *iter = (DBusMessageIter *) user_data;

	prop_upper = g_ascii_strup (key, -1);

	/* periods aren't valid in the environment, so replace them with
	 * underscores. */
	for (c = prop_upper; *c; c++) {
		if (*c == '.')
			*c = '_';
	}

	value = hal_device_property_to_string (device, key);
	env = g_strdup_printf ("HAL_PROP_%s=%s", prop_upper, value);
	dbus_message_iter_append_basic (iter, DBUS_TYPE_STRING, &env);

	g_free (env);
	g_free (value);
	g_free (prop_upper);
}

static void
add_env (DBusMessageIter * iter, const gchar * key, const gchar * value)
{
	gchar *env;
	env = g_strdup_printf ("%s=%s", key, value);
	dbus_message_iter_append_basic (iter, DBUS_TYPE_STRING, &env);
	g_free (env);
}

static void
add_basic_env (DBusMessageIter * iter, const gchar * udi)
{
	struct utsname un;
#ifdef HAVE_CONKIT
	CKTracker *ck_tracker;
#endif

	if (hald_is_verbose) {
		add_env (iter, "HALD_VERBOSE", "1");
	}
	if (hald_is_initialising) {
		add_env (iter, "HALD_STARTUP", "1");
	}
	if (hald_use_syslog) {
		add_env (iter, "HALD_USE_SYSLOG", "1");
	}
	add_env (iter, "UDI", udi);
	add_env (iter, "HALD_DIRECT_ADDR", hald_dbus_local_server_addr ());
#ifdef HAVE_POLKIT
	add_env (iter, "HAVE_POLKIT", "1");
#endif


#ifdef HAVE_CONKIT
	ck_tracker = hald_dbus_get_ck_tracker ();
	if (ck_tracker != NULL) {
		GSList *i;
		GSList *seats;
		GString *seats_string;
		char *s;
		char *p;

		seats_string = g_string_new ("");

		seats = ck_tracker_get_seats (ck_tracker);
		for (i = seats; i != NULL; i = g_slist_next (i)) {
			GSList *j;
			CKSeat *seat;
			GSList *sessions;
			const char *seat_id;
			GString *sessions_string;

			sessions_string = g_string_new ("");

			seat = i->data;

			/* use the basename as Id, e.g. Seat1 rather than /org/freedesktop/ConsoleKit/Seat1 */
			seat_id = ck_seat_get_id (seat);

			/* append to CK_SEATS */
			if (seats_string->len > 0) {
				g_string_append_c (seats_string, '\t');
			}
			g_string_append (seats_string, seat_id);

			sessions = ck_seat_get_sessions (seat);
			for (j = sessions; j != NULL; j = g_slist_next (j)) {
				CKSession *session;
				const char *session_id;

				session = j->data;
				/* basename again; e.g. Session1 rather than /org/freedesktop/ConsoleKit/Session1 */
				session_id = ck_session_get_id (session);

				if (sessions_string->len > 0) {
					g_string_append_c (sessions_string, '\t');
				}
				g_string_append (sessions_string, session_id);

				/* for each Session, export IS_ACTIVE and UID 
				 *
				 * CK_SESSION_IS_ACTIVE_Session2=true|false
				 * CK_SESSION_UID_Session2=501
				 * CK_SESSION_IS_LOCAL_Session2=true|false
				 * CK_SESSION_HOSTNAME_Session2=192.168.1.112
				 */
				s = g_strdup_printf ("CK_SESSION_IS_ACTIVE_%s", session_id);
				add_env (iter, s, ck_session_is_active (session) ? "true" : "false");
				g_free (s);
				s = g_strdup_printf ("CK_SESSION_UID_%s", session_id);
				p = g_strdup_printf ("%d", ck_session_get_user (session));
				add_env (iter, s, p);
				g_free (s);
				g_free (p);
				s = g_strdup_printf ("CK_SESSION_IS_LOCAL_%s", session_id);
				add_env (iter, s, ck_session_is_local (session) ? "true" : "false");
				g_free (s);
				s = g_strdup_printf ("CK_SESSION_HOSTNAME_%s", session_id);
				p = g_strdup_printf ("%s", ck_session_get_hostname (session));
				add_env (iter, s, p);
				g_free (s);
				g_free (p);
			}

			/* for each Seat, export sessions on each seat 
			 *
			 * CK_SEAT_Seat1=Session1 Session3 Session7
			 */
			s = g_strdup_printf ("CK_SEAT_%s", seat_id);
			add_env (iter, s, sessions_string->str);
			g_string_free (sessions_string, TRUE);
			g_free (s);

		}

		/* Export all detected seats 
		 *
		 * CK_SEATS=Seat1 Seat3 Seat4
		 */
		add_env (iter, "CK_SEATS", seats_string->str);
		g_string_free (seats_string, TRUE);
	}
#endif /* HAVE_CONKIT */

	if (uname (&un) == 0) {
		char *sysname;

		sysname = g_ascii_strdown (un.sysname, -1);
		add_env (iter, "HALD_UNAME_S", sysname);
		g_free (sysname);
	}
}

static void
add_extra_env (DBusMessageIter * iter, gchar ** env)
{
	int i;
	if (env != NULL)
		for (i = 0; env[i] != NULL; i++) {
			dbus_message_iter_append_basic (iter,
							DBUS_TYPE_STRING,
							&env[i]);
		}
}

static gboolean
add_command (DBusMessageIter * iter, const gchar * command_line)
{
	gint argc;
	gint x;
	char **argv;
	GError *err = NULL;
	DBusMessageIter array_iter;

	if (!g_shell_parse_argv (command_line, &argc, &argv, &err)) {
		HAL_ERROR (("Error parsing commandline '%s': %s",
			    command_line, err->message));
		g_error_free (err);
		return FALSE;
	}
	if (!dbus_message_iter_open_container (iter,
					       DBUS_TYPE_ARRAY,
					       DBUS_TYPE_STRING_AS_STRING,
					       &array_iter))
		DIE (("No memory"));
	for (x = 0; argv[x] != NULL; x++) {
		dbus_message_iter_append_basic (&array_iter,
						DBUS_TYPE_STRING,
						&argv[x]);
	}
	dbus_message_iter_close_container (iter, &array_iter);

	g_strfreev (argv);
	return TRUE;
}

static gboolean
add_first_part (DBusMessageIter * iter, HalDevice * device,
		const gchar * command_line, char **extra_env)
{
	DBusMessageIter array_iter;
	const char *udi;

	if (device != NULL)
		udi = hal_device_get_udi (device);
	else
		udi = "";

	dbus_message_iter_append_basic (iter, DBUS_TYPE_STRING, &udi);

	dbus_message_iter_open_container (iter,
					  DBUS_TYPE_ARRAY,
					  DBUS_TYPE_STRING_AS_STRING,
					  &array_iter);
	if (device != NULL)
		hal_device_property_foreach (device, add_property_to_msg,
					     &array_iter);
	add_basic_env (&array_iter, udi);
	add_extra_env (&array_iter, extra_env);
	dbus_message_iter_close_container (iter, &array_iter);

	if (!add_command (iter, command_line)) {
		return FALSE;
	}
	return TRUE;
}

/* Start a helper, returns true on a successfull start */
gboolean
hald_runner_start (HalDevice * device, const gchar * command_line,
		   char **extra_env, HalRunTerminatedCB cb, gpointer data1,
		   gpointer data2)
{
	DBusMessage *msg, *reply;
	DBusError err;
	DBusMessageIter iter;

	dbus_error_init (&err);
	msg = dbus_message_new_method_call ("org.freedesktop.HalRunner",
					    "/org/freedesktop/HalRunner",
					    "org.freedesktop.HalRunner",
					    "Start");
	if (msg == NULL)
		DIE (("No memory"));
	dbus_message_iter_init_append (msg, &iter);

	if (!add_first_part (&iter, device, command_line, extra_env))
		goto error;

	/* Wait for the reply, should be almost instantanious */
	reply =
	    dbus_connection_send_with_reply_and_block (runner_connection,
						       msg, -1, &err);
	if (reply) {
		gboolean ret =
		    (dbus_message_get_type (reply) ==
		     DBUS_MESSAGE_TYPE_METHOD_RETURN);

		if (ret) {
			dbus_int64_t pid_from_runner;
			if (dbus_message_get_args (reply, &err,
						   DBUS_TYPE_INT64,
						   &pid_from_runner,
						   DBUS_TYPE_INVALID)) {
				if (cb != NULL) {
					RunningProcess *rp;
					rp = g_new0 (RunningProcess, 1);
					rp->pid = (GPid) pid_from_runner;
					rp->cb = cb;
					rp->device = device;
					rp->data1 = data1;
					rp->data2 = data2;

					running_processes =
					    g_slist_prepend
					    (running_processes, rp);
					HAL_INFO (("running_processes %p, num = %d", running_processes, g_slist_length (running_processes)));
				}
			} else {
				HAL_ERROR (("Error extracting out_pid from runner's Start()"));
			}
		}

		dbus_message_unref (reply);
		dbus_message_unref (msg);
		return ret;
	}

      error:
	dbus_message_unref (msg);
	return FALSE;
}

static void
process_reply (DBusMessage *m, HelperData *hb)
{
	dbus_uint32_t exitt = HALD_RUN_SUCCESS;
	dbus_int32_t return_code = 0;
	GArray *error = NULL;
	DBusMessageIter iter;

	error = g_array_new (TRUE, FALSE, sizeof (char *));

	if (dbus_message_get_type (m) != DBUS_MESSAGE_TYPE_METHOD_RETURN)
		goto malformed;

	if (!dbus_message_iter_init (m, &iter) ||
	    dbus_message_iter_get_arg_type (&iter) != DBUS_TYPE_UINT32)
		goto malformed;
	dbus_message_iter_get_basic (&iter, &exitt);

	if (!dbus_message_iter_next (&iter) ||
	    dbus_message_iter_get_arg_type (&iter) != DBUS_TYPE_INT32)
		goto malformed;
	dbus_message_iter_get_basic (&iter, &return_code);

	while (dbus_message_iter_next (&iter) &&
	       dbus_message_iter_get_arg_type (&iter) ==
	       DBUS_TYPE_STRING) {
		const char *value;
		dbus_message_iter_get_basic (&iter, &value);
		g_array_append_vals (error, &value, 1);
	}

	if (hb->cb != NULL) {
		hb->cb (hb->d, exitt, return_code,
			(gchar **) error->data, hb->data1, hb->data2);
	}

	if (hb->d != NULL)
		g_object_unref (hb->d);

	dbus_message_unref (m);
	g_array_free (error, TRUE);

	g_free (hb);

	goto out;

      malformed:
	/* Send a Fail callback on malformed messages */
	HAL_ERROR (("Malformed or unexpected reply message"));
	if (hb->cb != NULL) {
		hb->cb (hb->d, HALD_RUN_FAILED, return_code, NULL, hb->data1,
			hb->data2);
	}

	if (hb->d != NULL)
		g_object_unref (hb->d);

	dbus_message_unref (m);
	g_array_free (error, TRUE);

	g_free (hb);

      out:
	if (method_run_notify)
		method_run_notify (method_run_notify_userdata);
}


static void
call_notify (DBusPendingCall * pending, void *user_data)
{
	HelperData *hb = (HelperData *) user_data;
	DBusMessage *m;

	m = dbus_pending_call_steal_reply (pending);
	process_reply (m, hb);
	dbus_pending_call_unref (pending);

}

/* Run a helper program using the commandline, with input as infomation on
 * stdin */
void
hald_runner_run_method (HalDevice * device,
			const gchar * command_line, char **extra_env,
			gchar * input, gboolean error_on_stderr,
			guint32 timeout,
			HalRunTerminatedCB cb,
			gpointer data1, gpointer data2)
{
	DBusMessage *msg;
	DBusMessageIter iter;
	DBusPendingCall *call;
	HelperData *hd = NULL;

	msg = dbus_message_new_method_call ("org.freedesktop.HalRunner",
					    "/org/freedesktop/HalRunner",
					    "org.freedesktop.HalRunner",
					    "Run");
	if (msg == NULL)
		DIE (("No memory"));
	dbus_message_iter_init_append (msg, &iter);

	if (!add_first_part (&iter, device, command_line, extra_env))
		goto error;

	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &input);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_BOOLEAN,
					&error_on_stderr);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_UINT32, &timeout);

	if (!dbus_connection_send_with_reply (runner_connection,
					      msg, &call, INT_MAX))
		DIE (("No memory"));

	/**
	 * The connection was disconnected as per D-Bus API
	 * This is an error condition and should not really happen
	 */
	if (call == NULL)
		goto error;

	hd = g_new0 (HelperData, 1);
	hd->d = device;
	hd->cb = cb;
	hd->data1 = data1;
	hd->data2 = data2;

	if (device != NULL)
		g_object_ref (device);

	dbus_pending_call_set_notify (call, call_notify, hd, NULL);
	dbus_message_unref (msg);
	return;
error:
	dbus_message_unref (msg);
	g_free (hd);
	cb (device, HALD_RUN_FAILED, 0, NULL, data1, data2);
}

void
hald_runner_run (HalDevice * device,
		 const gchar * command_line, char **extra_env,
		 guint timeout,
		 HalRunTerminatedCB cb, gpointer data1, gpointer data2)
{
	hald_runner_run_method (device, command_line, extra_env,
				"", FALSE, timeout, cb, data1, data2);
}

void
hald_runner_run_sync (HalDevice * device,
		      const gchar * command_line, char **extra_env,
		      guint timeout,
		      HalRunTerminatedCB cb, gpointer data1, gpointer data2)
{
	DBusMessage *msg;
	DBusMessage *reply;
	DBusMessageIter iter;
	HelperData *hd = NULL;
	const char *input = "";
	gboolean error_on_stderr = FALSE;
	DBusError error;

	msg = dbus_message_new_method_call ("org.freedesktop.HalRunner",
					    "/org/freedesktop/HalRunner",
					    "org.freedesktop.HalRunner",
					    "Run");
	if (msg == NULL)
		DIE (("No memory"));
	dbus_message_iter_init_append (msg, &iter);

	if (!add_first_part (&iter, device, command_line, extra_env))
		goto error;

	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &input);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_BOOLEAN, &error_on_stderr);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_UINT32, &timeout);

	dbus_error_init (&error);
	reply = dbus_connection_send_with_reply_and_block (runner_connection, msg, INT_MAX, &error);
	if (reply == NULL) {
		if (dbus_error_is_set (&error)) {
			HAL_ERROR (("Error running '%s': %s: %s", command_line, error.name, error.message));
		}
		goto error;
	}

	hd = g_new0 (HelperData, 1);
	hd->d = device;
	hd->cb = cb;
	hd->data1 = data1;
	hd->data2 = data2;

	/* this will free the HelperData and unref the reply (it's
	 * used also by the async version) 
	 */
	process_reply (reply, hd);

	dbus_message_unref (msg);
	return;

error:
	dbus_message_unref (msg);
	g_free (hd);
	cb (device, HALD_RUN_FAILED, 0, NULL, data1, data2);
}


void
hald_runner_kill_device (HalDevice * device)
{
	DBusMessage *msg, *reply;
	DBusError err;
	DBusMessageIter iter;
	const char *udi;

	running_processes_remove_device (device);

	msg = dbus_message_new_method_call ("org.freedesktop.HalRunner",
					    "/org/freedesktop/HalRunner",
					    "org.freedesktop.HalRunner",
					    "Kill");
	if (msg == NULL)
		DIE (("No memory"));
	dbus_message_iter_init_append (msg, &iter);
	udi = hal_device_get_udi (device);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &udi);

	/* Wait for the reply, should be almost instantanious */
	dbus_error_init (&err);
	reply =
	    dbus_connection_send_with_reply_and_block (runner_connection,
						       msg, -1, &err);
	if (reply) {
		dbus_message_unref (reply);
	}

	dbus_message_unref (msg);
}

void
hald_runner_kill_all (HalDevice * device)
{
	DBusMessage *msg, *reply;
	DBusError err;

	running_processes_remove_device (device);

	msg = dbus_message_new_method_call ("org.freedesktop.HalRunner",
					    "/org/freedesktop/HalRunner",
					    "org.freedesktop.HalRunner",
					    "KillAll");
	if (msg == NULL)
		DIE (("No memory"));

	/* Wait for the reply, should be almost instantanious */
	dbus_error_init (&err);
	reply =
	    dbus_connection_send_with_reply_and_block (runner_connection,
						       msg, -1, &err);
	if (reply) {
		dbus_message_unref (reply);
	}

	dbus_message_unref (msg);
}

void
hald_runner_set_method_run_notify (HaldRunnerRunNotify cb,
				   gpointer user_data)
{
	method_run_notify = cb;
	method_run_notify_userdata = user_data;
}

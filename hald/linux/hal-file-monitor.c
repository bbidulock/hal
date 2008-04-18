/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2007 William Jon McCann <mccann@jhu.edu>
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
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */

#ifdef HAVE_CONFIG
#  include "config.h"
#endif

#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#ifdef HAVE_SYS_INOTIFY_H
#include <sys/inotify.h>
#else
#include "inotify_local.h"
#endif

#include <glib.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <glib-object.h>

#include "../hal-file-monitor.h"

#define HAL_FILE_MONITOR_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), HAL_TYPE_FILE_MONITOR, HalFileMonitorPrivate))

typedef struct
{
        int     wd;
        char   *path;
        GSList *notifies;
} FileInotifyWatch;

typedef struct
{
        guint                   id;
        int                     mask;
        HalFileMonitorNotifyFunc notify_func;
        gpointer                user_data;
        FileInotifyWatch       *watch;
} FileMonitorNotify;

typedef struct
{
        FileInotifyWatch  *watch;
        HalFileMonitorEvent event;
        char              *path;
} FileMonitorEventInfo;

#define DEFAULT_NOTIFY_BUFLEN (32 * (sizeof (struct inotify_event) + 16))
#define MAX_NOTIFY_BUFLEN     (32 * DEFAULT_NOTIFY_BUFLEN)

struct HalFileMonitorPrivate
{
        guint       serial;

        gboolean    initialized_inotify;

        int         inotify_fd;
        guint       io_watch;

        GHashTable *wd_to_watch;
        GHashTable *path_to_watch;
        GHashTable *notifies;

        guint       buflen;
        guchar     *buffer;

        guint       events_idle_id;
        GQueue     *notify_events;
};

enum {
        PROP_0,
};

static void     hal_file_monitor_class_init  (HalFileMonitorClass *klass);
static void     hal_file_monitor_init        (HalFileMonitor      *file_monitor);
static void     hal_file_monitor_finalize    (GObject            *object);

G_DEFINE_TYPE (HalFileMonitor, hal_file_monitor, G_TYPE_OBJECT)

static gpointer monitor_object = NULL;

GQuark
hal_file_monitor_error_quark (void)
{
        static GQuark ret = 0;
        if (ret == 0) {
                ret = g_quark_from_static_string ("hal_file_monitor_error");
        }

        return ret;
}

/* most of this is adapted from libgnome-menu */

static int
our_event_mask_to_inotify_mask (int our_mask)
{
        int mask;

        mask = 0;

        if (our_mask & HAL_FILE_MONITOR_EVENT_ACCESS) {
                mask |= IN_ACCESS;
        }

        if (our_mask & HAL_FILE_MONITOR_EVENT_CREATE) {
                mask |= IN_CREATE | IN_MOVED_TO;
        }

        if (our_mask & HAL_FILE_MONITOR_EVENT_DELETE) {
                mask |= IN_DELETE | IN_DELETE_SELF | IN_MOVED_FROM | IN_MOVE_SELF;
        }

        if (our_mask & HAL_FILE_MONITOR_EVENT_CHANGE) {
                mask |= IN_MODIFY | IN_ATTRIB;
        }

        return mask;
}

static char *
imask_to_string (guint32 mask)
{
        GString *out;

        out = g_string_new (NULL);

        if (mask & IN_ACCESS) {
                g_string_append (out, "ACCESS ");
        }
        if (mask & IN_MODIFY) {
                g_string_append (out, "MODIFY ");
        }
        if (mask & IN_ATTRIB) {
                g_string_append (out, "ATTRIB ");
        }
        if (mask & IN_CLOSE_WRITE) {
                g_string_append (out, "CLOSE_WRITE ");
        }
        if (mask & IN_CLOSE_NOWRITE) {
                g_string_append (out, "CLOSE_NOWRITE ");
        }
        if (mask & IN_OPEN) {
                g_string_append (out, "OPEN ");
        }
        if (mask & IN_MOVED_FROM) {
                g_string_append (out, "MOVED_FROM ");
        }
        if (mask & IN_MOVED_TO) {
                g_string_append (out, "MOVED_TO ");
        }
        if (mask & IN_DELETE) {
                g_string_append (out, "DELETE ");
        }
        if (mask & IN_CREATE) {
                g_string_append (out, "CREATE ");
        }
        if (mask & IN_DELETE_SELF) {
                g_string_append (out, "DELETE_SELF ");
        }
        if (mask & IN_UNMOUNT) {
                g_string_append (out, "UNMOUNT ");
        }
        if (mask & IN_Q_OVERFLOW) {
                g_string_append (out, "Q_OVERFLOW ");
        }
        if (mask & IN_IGNORED) {
                g_string_append (out, "IGNORED ");
        }

        return g_string_free (out, FALSE);
}

static FileInotifyWatch *
file_monitor_add_watch_for_path (HalFileMonitor *monitor,
                                 const char    *path,
                                 int            mask)

{
        FileInotifyWatch *watch;
        int               wd;
        int               imask;
        char             *mask_str;

        imask = our_event_mask_to_inotify_mask (mask);

        mask_str = imask_to_string (imask);
        /*g_debug ("adding inotify watch %s", mask_str);*/
        g_free (mask_str);

        wd = inotify_add_watch (monitor->priv->inotify_fd, path, IN_MASK_ADD | imask);
        if (wd < 0) {
                /* FIXME: remove watch etc */
                return NULL;
        }

        watch = g_hash_table_lookup (monitor->priv->path_to_watch, path);
        if (watch == NULL) {
                watch = g_new0 (FileInotifyWatch, 1);

                watch->wd = wd;
                watch->path = g_strdup (path);

                g_hash_table_insert (monitor->priv->path_to_watch, watch->path, watch);
                g_hash_table_insert (monitor->priv->wd_to_watch, GINT_TO_POINTER (wd), watch);
        }

        return watch;
}

static void
monitor_release_watch (HalFileMonitor    *monitor,
                       FileInotifyWatch *watch)
{
        g_slist_free (watch->notifies);
        watch->notifies = NULL;

        g_free (watch->path);
        watch->path = NULL;

        inotify_rm_watch (monitor->priv->inotify_fd, watch->wd);
        watch->wd = -1;
}

static void
file_monitor_remove_watch (HalFileMonitor    *monitor,
                           FileInotifyWatch *watch)
{
        g_hash_table_remove (monitor->priv->path_to_watch,
                             watch->path);
        g_hash_table_remove (monitor->priv->wd_to_watch,
                             GINT_TO_POINTER (watch->wd));
        monitor_release_watch (monitor, watch);
}

static gboolean
remove_watch_foreach (const char       *path,
                      FileInotifyWatch *watch,
                      HalFileMonitor    *monitor)
{
        monitor_release_watch (monitor, watch);
        return TRUE;
}

static void
close_inotify (HalFileMonitor *monitor)
{
        if (! monitor->priv->initialized_inotify) {
                return;
        }

        monitor->priv->initialized_inotify = FALSE;

        g_hash_table_foreach_remove (monitor->priv->path_to_watch,
                                     (GHRFunc) remove_watch_foreach,
                                     monitor);
        monitor->priv->path_to_watch = NULL;

        if (monitor->priv->wd_to_watch != NULL) {
                g_hash_table_destroy (monitor->priv->wd_to_watch);
        }
        monitor->priv->wd_to_watch = NULL;

        g_free (monitor->priv->buffer);
        monitor->priv->buffer = NULL;
        monitor->priv->buflen = 0;

        if (monitor->priv->io_watch) {
                g_source_remove (monitor->priv->io_watch);
        }
        monitor->priv->io_watch = 0;

        if (monitor->priv->inotify_fd > 0) {
                close (monitor->priv->inotify_fd);
        }
        monitor->priv->inotify_fd = 0;
}

static gboolean
emit_events_in_idle (HalFileMonitor *monitor)
{
        FileMonitorEventInfo *event_info;

        monitor->priv->events_idle_id = 0;

        while ((event_info = g_queue_pop_head (monitor->priv->notify_events)) != NULL) {
                GSList           *l;
                FileInotifyWatch *watch;

                watch = event_info->watch;

                for (l = watch->notifies; l != NULL; l = l->next) {
                        FileMonitorNotify *notify;

                        notify = g_hash_table_lookup (monitor->priv->notifies,
                                                      GUINT_TO_POINTER (l->data));
                        if (notify == NULL) {
                                continue;
                        }

                        if (! (notify->mask & event_info->event)) {
                                continue;
                        }

                        if (notify->notify_func) {
                                notify->notify_func (monitor, event_info->event, event_info->path, notify->user_data);
                        }
                }

                g_free (event_info->path);
                event_info->path = NULL;

                event_info->event = HAL_FILE_MONITOR_EVENT_NONE;

                g_free (event_info);
        }

        return FALSE;
}

static void
file_monitor_queue_event (HalFileMonitor        *monitor,
                          FileMonitorEventInfo *event_info)
{
        g_queue_push_tail (monitor->priv->notify_events, event_info);

        if (monitor->priv->events_idle_id == 0) {
                monitor->priv->events_idle_id = g_idle_add ((GSourceFunc) emit_events_in_idle, monitor);
        }
}

static void
queue_watch_event (HalFileMonitor     *monitor,
                   FileInotifyWatch  *watch,
                   HalFileMonitorEvent event,
                   const char        *path)
{
        FileMonitorEventInfo *event_info;

        event_info = g_new0 (FileMonitorEventInfo, 1);

        event_info->watch   = watch;
        event_info->path    = g_strdup (path);
        event_info->event   = event;

        file_monitor_queue_event (monitor, event_info);
}

static void
handle_inotify_event (HalFileMonitor        *monitor,
                      FileInotifyWatch     *watch,
                      struct inotify_event *ievent)
{
        HalFileMonitorEvent  event;
        const char         *path;
        char               *freeme;
        char               *mask_str;

        freeme = NULL;

        if (ievent->len > 0) {
                path = freeme = g_build_filename (watch->path, ievent->name, NULL);
        } else {
                path = watch->path;
        }

        mask_str = imask_to_string (ievent->mask);
        /*g_debug ("handing inotify event %s for %s", mask_str, path);*/
        g_free (mask_str);

        event = HAL_FILE_MONITOR_EVENT_NONE;

        if (ievent->mask & (IN_CREATE | IN_MOVED_TO)) {
                event = HAL_FILE_MONITOR_EVENT_CREATE;
        } else if (ievent->mask & (IN_DELETE | IN_DELETE_SELF | IN_MOVED_FROM | IN_MOVE_SELF)) {
                event = HAL_FILE_MONITOR_EVENT_DELETE;
        } else if (ievent->mask & (IN_MODIFY | IN_ATTRIB)) {
                event = HAL_FILE_MONITOR_EVENT_CHANGE;
        } else if (ievent->mask & IN_ACCESS) {
                event = HAL_FILE_MONITOR_EVENT_ACCESS;
        }

        if (event != HAL_FILE_MONITOR_EVENT_NONE) {
                queue_watch_event (monitor, watch, event, path);
        }

        if (ievent->mask & IN_IGNORED) {
                file_monitor_remove_watch (monitor, watch);
        }
	
	g_free(freeme);
}

static gboolean
inotify_data_pending (GIOChannel    *source,
		      GIOCondition   condition,
                      HalFileMonitor *monitor)
{
        int len;
        int i;

        /*g_debug ("Inotify data pending");*/

        g_assert (monitor->priv->inotify_fd > 0);
        g_assert (monitor->priv->buffer != NULL);

        do {
                while ((len = read (monitor->priv->inotify_fd, monitor->priv->buffer, monitor->priv->buflen)) < 0 && errno == EINTR);

                if (len > 0) {
                        break;
                } else if (len < 0) {
                        g_warning ("Error reading inotify event: %s",
                                   g_strerror (errno));
                        goto error_cancel;
                }

                g_assert (len == 0);

                if ((monitor->priv->buflen << 1) > MAX_NOTIFY_BUFLEN) {
                        g_warning ("Error reading inotify event: Exceded maximum buffer size");
                        goto error_cancel;
                }

                /*g_debug ("Buffer size %u too small, trying again at %u\n",
                  monitor->priv->buflen, monitor->priv->buflen << 1);*/

                monitor->priv->buflen <<= 1;
                monitor->priv->buffer = g_realloc (monitor->priv->buffer, monitor->priv->buflen);
        } while (TRUE);

        /*g_debug ("Inotify buffer filled");*/

        i = 0;
        while (i < len) {
                struct inotify_event *ievent = (struct inotify_event *) &monitor->priv->buffer [i];
                FileInotifyWatch     *watch;

                /*g_debug ("Got event wd = %d, mask = 0x%x, cookie = %d, len = %d, name= %s\n",
                         ievent->wd,
                         ievent->mask,
                         ievent->cookie,
                         ievent->len,
                         ievent->len > 0 ? ievent->name : "<none>");*/

                watch = g_hash_table_lookup (monitor->priv->wd_to_watch,
                                             GINT_TO_POINTER (ievent->wd));
                if (watch != NULL) {
                        handle_inotify_event (monitor, watch, ievent);
                }

                i += sizeof (struct inotify_event) + ievent->len;
        }

        return TRUE;

 error_cancel:
        monitor->priv->io_watch = 0;

        close_inotify (monitor);

        return FALSE;
}

static FileMonitorNotify *
file_monitor_add_notify_for_path (HalFileMonitor          *monitor,
                                  const char             *path,
                                  int                     mask,
                                  HalFileMonitorNotifyFunc notify_func,
                                  gpointer                data)
{
        FileMonitorNotify *notify;
        FileInotifyWatch  *watch;

        notify = NULL;

        watch = file_monitor_add_watch_for_path (monitor, path, mask);
        if (watch != NULL) {
                notify = g_new0 (FileMonitorNotify, 1);
                notify->notify_func = notify_func;
                notify->user_data = data;
                notify->id = monitor->priv->serial++;
                notify->watch = watch;
                notify->mask = mask;

                /*g_debug ("Adding notify for %s mask:%d", path, mask);*/

                g_hash_table_insert (monitor->priv->notifies, GUINT_TO_POINTER (notify->id), notify);
                watch->notifies = g_slist_prepend (watch->notifies, GUINT_TO_POINTER (notify->id));
        }

        return notify;
}

static void
file_monitor_remove_notify (HalFileMonitor *monitor,
                            guint          id)
{
        FileMonitorNotify *notify;

        /*g_debug ("removing notify for %u", id);*/

        notify = g_hash_table_lookup (monitor->priv->notifies,
                                      GUINT_TO_POINTER (id));
        if (notify == NULL) {
                return;
        }

        g_hash_table_steal (monitor->priv->notifies,
                            GUINT_TO_POINTER (id));

        notify->watch->notifies = g_slist_remove (notify->watch->notifies, GUINT_TO_POINTER (id));

        if (g_slist_length (notify->watch->notifies) == 0) {
                file_monitor_remove_watch (monitor, notify->watch);
                g_free (notify->watch);
        }

        g_free (notify);
}

guint
hal_file_monitor_add_notify (HalFileMonitor          *monitor,
                            const char             *path,
                            int                     mask,
                            HalFileMonitorNotifyFunc notify_func,
                            gpointer                data)
{
        FileMonitorNotify *notify;

        if (! monitor->priv->initialized_inotify) {
                return 0;
        }

        notify = file_monitor_add_notify_for_path (monitor,
                                                   path,
                                                   mask,
                                                   notify_func,
                                                   data);
        if (notify == NULL) {
                g_warning ("Failed to add monitor on '%s': %s",
                           path,
                           g_strerror (errno));
                return 0;
        }

        return notify->id;
}

void
hal_file_monitor_remove_notify (HalFileMonitor *monitor,
                               guint          id)
{
        if (! monitor->priv->initialized_inotify) {
                return;
        }

        file_monitor_remove_notify (monitor, id);
}

static void
hal_file_monitor_class_init (HalFileMonitorClass *klass)
{
        GObjectClass   *object_class = G_OBJECT_CLASS (klass);

        object_class->finalize = hal_file_monitor_finalize;

        g_type_class_add_private (klass, sizeof (HalFileMonitorPrivate));
}


static void
setup_inotify (HalFileMonitor *monitor)
{
        GIOChannel *io_channel;
        int         fd;

        if (monitor->priv->initialized_inotify) {
                return;
        }

        if ((fd = inotify_init ()) < 0) {
                g_warning ("Failed to initialize inotify: %s",
                           g_strerror (errno));
                return;
        }

        monitor->priv->inotify_fd = fd;

        io_channel = g_io_channel_unix_new (fd);
        monitor->priv->io_watch = g_io_add_watch (io_channel,
                                                  G_IO_IN|G_IO_PRI,
                                                  (GIOFunc) inotify_data_pending,
                                                  monitor);
        g_io_channel_unref (io_channel);

        monitor->priv->buflen = DEFAULT_NOTIFY_BUFLEN;
        monitor->priv->buffer = g_malloc (DEFAULT_NOTIFY_BUFLEN);

        monitor->priv->notifies = g_hash_table_new (g_direct_hash,
                                                    g_direct_equal);

        monitor->priv->wd_to_watch = g_hash_table_new (g_direct_hash,
                                                       g_direct_equal);
        monitor->priv->path_to_watch = g_hash_table_new (g_str_hash,
                                                         g_str_equal);

        monitor->priv->initialized_inotify = TRUE;
}

static void
hal_file_monitor_init (HalFileMonitor *monitor)
{
        monitor->priv = HAL_FILE_MONITOR_GET_PRIVATE (monitor);

        monitor->priv->serial = 1;
        monitor->priv->notify_events = g_queue_new ();

        setup_inotify (monitor);
}

static void
hal_file_monitor_finalize (GObject *object)
{
        HalFileMonitor *monitor;

        g_return_if_fail (object != NULL);
        g_return_if_fail (HAL_IS_FILE_MONITOR (object));

        monitor = HAL_FILE_MONITOR (object);

        g_return_if_fail (monitor->priv != NULL);

        close_inotify (monitor);

        g_hash_table_destroy (monitor->priv->notifies);
        g_queue_free (monitor->priv->notify_events);

        G_OBJECT_CLASS (hal_file_monitor_parent_class)->finalize (object);
}

HalFileMonitor *
hal_file_monitor_new (void)
{
        if (monitor_object != NULL) {
                g_object_ref (monitor_object);
        } else {
                monitor_object = g_object_new (HAL_TYPE_FILE_MONITOR, NULL);

                g_object_add_weak_pointer (monitor_object,
                                           (gpointer *) &monitor_object);
        }

        return HAL_FILE_MONITOR (monitor_object);
}

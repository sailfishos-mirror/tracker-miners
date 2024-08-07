/*
 * Copyright (C) 2008, Nokia <ivan.frade@nokia.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#ifndef __TRACKERD_EXTRACT_H__
#define __TRACKERD_EXTRACT_H__

#include <gio/gio.h>
#include <libtracker-miners-common/tracker-common.h>
#include <libtracker-extract/tracker-extract.h>

#define TRACKER_EXTRACT_SERVICE        "org.freedesktop.Tracker3.Extract"
#define TRACKER_EXTRACT_PATH           "/org/freedesktop/Tracker3/Extract"
#define TRACKER_EXTRACT_INTERFACE      "org.freedesktop.Tracker3.Extract"

G_BEGIN_DECLS

#define TRACKER_EXTRACT_ERROR          (tracker_extract_error_quark ())
#define TRACKER_TYPE_EXTRACT           (tracker_extract_get_type ())
#define TRACKER_EXTRACT(object)        (G_TYPE_CHECK_INSTANCE_CAST ((object), TRACKER_TYPE_EXTRACT, TrackerExtract))
#define TRACKER_EXTRACT_CLASS(klass)   (G_TYPE_CHECK_CLASS_CAST ((klass), TRACKER_TYPE_EXTRACT, TrackerExtractClass))
#define TRACKER_IS_EXTRACT(object)     (G_TYPE_CHECK_INSTANCE_TYPE ((object), TRACKER_TYPE_EXTRACT))
#define TRACKER_IS_EXTRACT_CLASS(klass)(G_TYPE_CHECK_CLASS_TYPE ((klass), TRACKER_TYPE_EXTRACT))
#define TRACKER_EXTRACT_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS ((obj), TRACKER_TYPE_EXTRACT, TrackerExtractClass))

typedef struct TrackerExtract      TrackerExtract;
typedef struct TrackerExtractClass TrackerExtractClass;

typedef enum {
	TRACKER_EXTRACT_ERROR_NO_MIMETYPE,
	TRACKER_EXTRACT_ERROR_NO_EXTRACTOR,
	TRACKER_EXTRACT_ERROR_IO_ERROR,
} TrackerExtractError;

struct TrackerExtract {
	GObject parent;
};

struct TrackerExtractClass {
	GObjectClass parent;
};

GQuark          tracker_extract_error_quark             (void);
GType           tracker_extract_get_type                (void);
TrackerExtract *tracker_extract_new                     (gboolean                disable_shutdown,
                                                         const gchar            *force_module);

void            tracker_extract_file                    (TrackerExtract         *extract,
                                                         const gchar            *file,
                                                         const gchar            *content_id,
                                                         const gchar            *mimetype,
                                                         GCancellable           *cancellable,
                                                         GAsyncReadyCallback     cb,
                                                         gpointer                user_data);
TrackerExtractInfo *
                tracker_extract_file_finish             (TrackerExtract         *extract,
                                                         GAsyncResult           *res,
                                                         GError                **error);

void            tracker_extract_dbus_start              (TrackerExtract         *extract);
void            tracker_extract_dbus_stop               (TrackerExtract         *extract);

void            tracker_extract_set_max_text            (TrackerExtract *extract,
                                                         gint            max_text);

/* Not DBus API */
void            tracker_extract_get_metadata_by_cmdline (TrackerExtract             *object,
                                                         const gchar                *path,
                                                         const gchar                *mime,
                                                         TrackerSerializationFormat  output_format);

G_END_DECLS

#endif /* __TRACKERD_EXTRACT_H__ */

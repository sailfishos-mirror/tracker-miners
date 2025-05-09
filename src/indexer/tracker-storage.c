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

#include "config-miners.h"

#include <string.h>

#include <gio/gio.h>
#include <gio/gunixmounts.h>
#include <tracker-common.h>

#include "tracker-storage.h"

/**
 * SECTION:tracker-storage
 * @short_description: Removable storage and mount point convenience API
 * @include: libtracker-miner/tracker-miner.h
 *
 * This API is a convenience to to be able to keep track of volumes
 * which are mounted and also the type of removable media available.
 * The API is built upon the top of GIO's #GMount, #GDrive and #GVolume API.
 **/

#define TRACKER_STORAGE_GET_PRIVATE(obj) (G_TYPE_INSTANCE_GET_PRIVATE ((obj), TRACKER_TYPE_STORAGE, TrackerStoragePrivate))

typedef struct {
	GVolumeMonitor *volume_monitor;

	GNode *mounts;
	GHashTable *mounts_by_uuid;
	GHashTable *unmount_watchdogs;
} TrackerStoragePrivate;

typedef struct {
	gchar *mount_point;
	gchar *uuid;
	guint unmount_timer_id;
	guint removable : 1;
	guint optical : 1;
} MountInfo;

typedef struct {
	const gchar *path;
	GNode *node;
} TraverseData;

typedef struct {
	GSList *roots;
	TrackerStorageType type;
	gboolean exact_match;
} GetRoots;

typedef struct {
	TrackerStorage *storage;
	GMount *mount;
} UnmountCheckData;

static void     tracker_storage_finalize (GObject        *object);
static gboolean mount_info_free          (GNode          *node,
                                          gpointer        user_data);
static void     mount_node_free          (GNode          *node);
static gboolean mounts_setup             (TrackerStorage *storage);
static void     mount_added_cb           (GVolumeMonitor *monitor,
                                          GMount         *mount,
                                          gpointer        user_data);
static void     mount_removed_cb         (GVolumeMonitor *monitor,
                                          GMount         *mount,
                                          gpointer        user_data);
static void     mount_pre_removed_cb     (GVolumeMonitor *monitor,
                                          GMount         *mount,
                                          gpointer        user_data);

enum {
	MOUNT_POINT_ADDED,
	MOUNT_POINT_REMOVED,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = {0};

G_DEFINE_TYPE_WITH_PRIVATE (TrackerStorage, tracker_storage, G_TYPE_OBJECT);

static void
tracker_storage_class_init (TrackerStorageClass *klass)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS (klass);

	object_class->finalize     = tracker_storage_finalize;

	signals[MOUNT_POINT_ADDED] =
		g_signal_new ("mount-point-added",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              0,
		              NULL, NULL,
		              NULL,
		              G_TYPE_NONE,
		              5,
		              G_TYPE_STRING,
		              G_TYPE_STRING,
                              G_TYPE_STRING,
		              G_TYPE_BOOLEAN,
		              G_TYPE_BOOLEAN);

	signals[MOUNT_POINT_REMOVED] =
		g_signal_new ("mount-point-removed",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              0,
		              NULL, NULL,
		              NULL,
		              G_TYPE_NONE,
		              2,
		              G_TYPE_STRING,
		              G_TYPE_STRING);
}

static void
tracker_storage_init (TrackerStorage *storage)
{
	TrackerStoragePrivate *priv;

	priv = tracker_storage_get_instance_private (storage);

	priv->mounts = g_node_new (NULL);

	priv->mounts_by_uuid = g_hash_table_new_full (g_str_hash,
	                                              g_str_equal,
	                                              (GDestroyNotify) g_free,
	                                              NULL);
	priv->unmount_watchdogs = g_hash_table_new_full (NULL, NULL, NULL,
							 (GDestroyNotify) g_source_remove);

	priv->volume_monitor = g_volume_monitor_get ();

	/* Volume and property notification callbacks */
	g_signal_connect_object (priv->volume_monitor, "mount-removed",
	                         G_CALLBACK (mount_removed_cb), storage, 0);
	g_signal_connect_object (priv->volume_monitor, "mount-pre-unmount",
	                         G_CALLBACK (mount_pre_removed_cb), storage, 0);
	g_signal_connect_object (priv->volume_monitor, "mount-added",
	                         G_CALLBACK (mount_added_cb), storage, 0);

	TRACKER_NOTE (MONITORS, g_message ("Mount monitors set up for to watch for added, removed and pre-unmounts..."));

	/* Get all mounts and set them up */
	if (!mounts_setup (storage)) {
		return;
	}
}

static void
tracker_storage_finalize (GObject *object)
{
	TrackerStoragePrivate *priv;

	priv = tracker_storage_get_instance_private (TRACKER_STORAGE (object));

	g_hash_table_destroy (priv->unmount_watchdogs);

	if (priv->mounts_by_uuid) {
		g_hash_table_unref (priv->mounts_by_uuid);
	}

	if (priv->mounts) {
		mount_node_free (priv->mounts);
	}

	if (priv->volume_monitor) {
		g_object_unref (priv->volume_monitor);
	}

	(G_OBJECT_CLASS (tracker_storage_parent_class)->finalize) (object);
}

static void
mount_node_free (GNode *node)
{
	g_node_traverse (node,
	                 G_POST_ORDER,
	                 G_TRAVERSE_ALL,
	                 -1,
	                 mount_info_free,
	                 NULL);

	g_node_destroy (node);
}

static gboolean
mount_node_traverse_func (GNode    *node,
                          gpointer  user_data)
{
	TraverseData *data;
	MountInfo *info;

	if (!node->data) {
		/* Root node */
		return FALSE;
	}

	data = user_data;
	info = node->data;

	if (g_str_has_prefix (data->path, info->mount_point)) {
		data->node = node;
		return TRUE;
	}

	return FALSE;
}

static GNode *
mount_node_find (GNode       *root,
                 const gchar *path)
{
	TraverseData data = { path, NULL };

	g_node_traverse (root,
	                 G_POST_ORDER,
	                 G_TRAVERSE_ALL,
	                 -1,
	                 mount_node_traverse_func,
	                 &data);

	return data.node;
}

static gboolean
mount_info_free (GNode    *node,
                 gpointer  user_data)
{
	MountInfo *info;

	info = node->data;

	if (info) {
		g_free (info->mount_point);
		g_free (info->uuid);

		g_slice_free (MountInfo, info);
	}

	return FALSE;
}

static MountInfo *
mount_info_find (GNode       *root,
                 const gchar *path)
{
	GNode *node;

	node = mount_node_find (root, path);
	return (node) ? node->data : NULL;
}

static TrackerStorageType
mount_info_get_type (MountInfo *info)
{
	TrackerStorageType mount_type = 0;

	if (info->removable) {
		mount_type |= TRACKER_STORAGE_REMOVABLE;
	}

	if (info->optical) {
		mount_type |= TRACKER_STORAGE_OPTICAL;
	}

	return mount_type;
}

static gchar *
mount_point_normalize (const gchar *mount_point)
{
	gchar *mp;

	/* Normalize all mount points to have a / at the end */
	if (g_str_has_suffix (mount_point, G_DIR_SEPARATOR_S)) {
		mp = g_strdup (mount_point);
	} else {
		mp = g_strconcat (mount_point, G_DIR_SEPARATOR_S, NULL);
	}

	return mp;
}

static GNode *
mount_add_hierarchy (GNode       *root,
                     const gchar *uuid,
                     const gchar *mount_point,
                     gboolean     removable,
                     gboolean     optical)
{
	MountInfo *info;
	GNode *node;
	gchar *mp;

	mp = mount_point_normalize (mount_point);
	node = mount_node_find (root, mp);

	if (!node) {
		node = root;
	}

	info = g_slice_new (MountInfo);
	info->mount_point = mp;
	info->uuid = g_strdup (uuid);
	info->removable = removable;
	info->optical = optical;

	return g_node_append_data (node, info);
}

static void
mount_add_new (TrackerStorage *storage,
               const gchar    *uuid,
               const gchar    *mount_point,
               const gchar    *mount_name,
               gboolean        removable_device,
               gboolean        optical_disc)
{
	TrackerStoragePrivate *priv;
	GNode *node;

	priv = tracker_storage_get_instance_private (storage);

	node = mount_add_hierarchy (priv->mounts, uuid, mount_point, removable_device, optical_disc);
	g_hash_table_insert (priv->mounts_by_uuid, g_strdup (uuid), node);

	g_signal_emit (storage,
	               signals[MOUNT_POINT_ADDED],
	               0,
	               uuid,
	               mount_point,
                       mount_name,
	               removable_device,
	               optical_disc,
	               NULL);
}

static gchar *
mount_guess_content_type (GMount   *mount,
                          gboolean *is_optical,
                          gboolean *is_multimedia,
                          gboolean *is_blank)
{
	gchar *content_type = NULL;
	gchar **guess_type;

	*is_optical = FALSE;
	*is_multimedia = FALSE;
	*is_blank = FALSE;

	/* This function has 2 purposes:
	 *
	 * 1. Detect if we are using optical media
	 * 2. Detect if we are video or music, we can't index those types
	 *
	 * We try to determine the content type because we don't want
	 * to store Volume information in Tracker about DVDs and media
	 * which has no real data for us to mine.
	 *
	 * Generally, if is_multimedia is TRUE then we end up ignoring
	 * the media.
	 */
	guess_type = g_mount_guess_content_type_sync (mount, TRUE, NULL, NULL);

	if (guess_type) {
		gint i = 0;

		while (!content_type && guess_type[i]) {
			if (!g_strcmp0 (guess_type[i], "x-content/image-picturecd")) {
				/* Images */
				content_type = g_strdup (guess_type[i]);
			} else if (!g_strcmp0 (guess_type[i], "x-content/video-bluray") ||
			           !g_strcmp0 (guess_type[i], "x-content/video-dvd") ||
			           !g_strcmp0 (guess_type[i], "x-content/video-hddvd") ||
			           !g_strcmp0 (guess_type[i], "x-content/video-svcd") ||
			           !g_strcmp0 (guess_type[i], "x-content/video-vcd")) {
				/* Videos */
				*is_multimedia = TRUE;
				content_type = g_strdup (guess_type[i]);
			} else if (!g_strcmp0 (guess_type[i], "x-content/audio-cdda") ||
			           !g_strcmp0 (guess_type[i], "x-content/audio-dvd") ||
			           !g_strcmp0 (guess_type[i], "x-content/audio-player")) {
				/* Audios */
				*is_multimedia = TRUE;
				content_type = g_strdup (guess_type[i]);
			} else if (!g_strcmp0 (guess_type[i], "x-content/blank-bd") ||
			           !g_strcmp0 (guess_type[i], "x-content/blank-cd") ||
			           !g_strcmp0 (guess_type[i], "x-content/blank-dvd") ||
			           !g_strcmp0 (guess_type[i], "x-content/blank-hddvd")) {
				/* Blank */
				*is_blank = TRUE;
				content_type = g_strdup (guess_type[i]);
			} else if (!g_strcmp0 (guess_type[i], "x-content/software") ||
			           !g_strcmp0 (guess_type[i], "x-content/unix-software") ||
			           !g_strcmp0 (guess_type[i], "x-content/win32-software")) {
				/* NOTE: This one is a guess, can we
				 * have this content type on
				 * none-optical mount points?
				 */
				content_type = g_strdup (guess_type[i]);
			} else {
				/* else, keep on with the next guess, if any */
				i++;
			}
		}

		/* If we didn't have an exact match on possible guessed content types,
		 *  then use the first one returned (best guess always first) if any */
		if (!content_type && guess_type[0]) {
			content_type = g_strdup (guess_type[0]);
		}

		g_strfreev (guess_type);
	}

	if (content_type) {
		if (strstr (content_type, "vcd") ||
		    strstr (content_type, "cdda") ||
		    strstr (content_type, "dvd") ||
		    strstr (content_type, "bluray")) {
			*is_optical = TRUE;
		}
	} else {
		GUnixMountEntry *entry;
		gchar *mount_path;
		GFile *mount_root;

		/* No content type was guessed, try to find out
		 * at least whether it's an optical media or not
		 */
		mount_root = g_mount_get_root (mount);
		mount_path = g_file_get_path (mount_root);

		/* FIXME: Try to assume we have a unix mount :(
		 * EEK, once in a while, I have to write crack, oh well
		 */
		if (mount_path &&
		    (entry = g_unix_mount_at (mount_path, NULL)) != NULL) {
			const gchar *filesystem_type;
			gchar *device_path = NULL;
			GVolume *volume;

			volume = g_mount_get_volume (mount);
			filesystem_type = g_unix_mount_get_fs_type (entry);
			g_debug ("  Using filesystem type:'%s'",
			         filesystem_type);

			/* Volume may be NULL */
			if (volume) {
				device_path = g_volume_get_identifier (volume, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE);
				g_debug ("  Using device path:'%s'",
				         device_path);
				g_object_unref (volume);
			}

			/* NOTE: This code was taken from guess_mount_type()
			 * in GIO's gunixmounts.c and adapted purely for
			 * guessing optical media. We don't use the guessing
			 * code for other types such as MEMSTICKS, ZIPs,
			 * IPODs, etc.
			 *
			 * This code may need updating over time since it is
			 * very situational depending on how distributions
			 * mount their devices and how devices are named in
			 * /dev.
			 */
			if (strcmp (filesystem_type, "udf") == 0 ||
			    strcmp (filesystem_type, "iso9660") == 0 ||
			    strcmp (filesystem_type, "cd9660") == 0 ||
			    (device_path &&
			     (g_str_has_prefix (device_path, "/dev/cdrom") ||
			      g_str_has_prefix (device_path, "/dev/acd") ||
			      g_str_has_prefix (device_path, "/dev/cd")))) {
				*is_optical = TRUE;
			} else if (device_path &&
			           g_str_has_prefix (device_path, "/vol/")) {
				const gchar *name;

				name = mount_path + strlen ("/");

				if (g_str_has_prefix (name, "cdrom")) {
					*is_optical = TRUE;
				}
			} else {
				gchar *basename = g_path_get_basename (mount_path);

				if (g_str_has_prefix (basename, "cdr") ||
				    g_str_has_prefix (basename, "cdwriter") ||
				    g_str_has_prefix (basename, "burn") ||
				    g_str_has_prefix (basename, "dvdr")) {
					*is_optical = TRUE;
				}

				g_free (basename);
			}

			g_free (device_path);
			g_free (mount_path);
			g_unix_mount_free (entry);
		} else {
			g_debug ("  No GUnixMountEntry found, needed for detecting if optical media... :(");
			g_free (mount_path);
		}

		g_object_unref (mount_root);
	}

	return content_type;
}

static void
mount_add (TrackerStorage *storage,
           GMount         *mount)
{
	TrackerStoragePrivate *priv;
	GFile *root;
	GVolume *volume;
	gchar *mount_name, *mount_path, *uuid;
	gboolean is_optical = FALSE;
	gboolean is_removable = FALSE;

	/* Get mount name */
	mount_name = g_mount_get_name (mount);

	/* Get root path of the mount */
	root = g_mount_get_root (mount);
	if (!g_file_is_native (root)) {
		gchar *uri = g_file_get_uri (root);

		g_debug ("Ignoring mount '%s', URI '%s' is not native",
		         mount_name, uri);
		g_object_unref (root);
		g_free (mount_name);
		g_free (uri);
		return;
	}

	mount_path = g_file_get_path (root);
	g_debug ("Found '%s' mounted on path '%s'",
	         mount_name,
	         mount_path);

	/* Do not process shadowed mounts! */
	if (g_mount_is_shadowed (mount)) {
		g_debug ("  Skipping shadowed mount '%s'", mount_name);
		g_object_unref (root);
		g_free (mount_path);
		g_free (mount_name);
		return;
	}

	priv = tracker_storage_get_instance_private (storage);

	/* fstab partitions may not have corresponding
	 * GVolumes, so volume may be NULL */
	volume = g_mount_get_volume (mount);
	if (volume) {
		/* GMount with GVolume */

		/* Try to get UUID from the Volume.
		 * Note that g_volume_get_uuid() is NOT equivalent */
		uuid = g_volume_get_identifier (volume,
		                                G_VOLUME_IDENTIFIER_KIND_UUID);
		if (!uuid) {
			gchar *content_type;
			gboolean is_multimedia;
			gboolean is_blank;

			/* Optical discs usually won't have UUID in the GVolume */
			content_type = mount_guess_content_type (mount, &is_optical, &is_multimedia, &is_blank);
			is_removable = TRUE;

			/* We don't index content which is video, music or blank */
			if (!is_multimedia && !is_blank) {
				uuid = g_compute_checksum_for_string (G_CHECKSUM_MD5,
				                                      mount_name,
				                                      -1);
				g_debug ("  No UUID, generated:'%s' (based on mount name)", uuid);
				g_debug ("  Assuming GVolume has removable media, if wrong report a bug! "
				         "content type is '%s'",
				         content_type);
			} else {
				g_debug ("  Being ignored because mount with volume is music/video/blank "
				         "(content type:%s, optical:%s, multimedia:%s, blank:%s)",
				         content_type,
				         is_optical ? "yes" : "no",
				         is_multimedia ? "yes" : "no",
				         is_blank ? "yes" : "no");
			}

			g_free (content_type);
		} else {
			/* Any other removable media will have UUID in the
			 * GVolume. Note that this also may include some
			 * partitions in the machine which have GVolumes
			 * associated to the GMounts. We also check a drive
			 * exists to be sure the device is local. */
			GDrive *drive;

			drive = g_volume_get_drive (volume);

			if (drive) {
				/* We can't mount/unmount system volumes, so tag
				 * them as non removable. */
				is_removable = g_drive_is_media_removable (drive);
				g_debug ("  Found mount with volume and drive which %s be mounted: "
				         "Assuming it's %s removable, if wrong report a bug!",
				         is_removable ? "can" : "cannot",
				         is_removable ? "" : "not");
				g_object_unref (drive);
			} else {
				/* Note: not sure when this can happen... */
				g_debug ("  Mount with volume but no drive, "
				         "assuming not a removable device, "
				         "if wrong report a bug!");
				is_removable = FALSE;
			}
		}

		g_object_unref (volume);
	} else {
		/* GMount without GVolume.
		 * Note: Never found a case where this g_mount_get_uuid() returns
		 * non-NULL... :-) */
		uuid = g_mount_get_uuid (mount);
		if (!uuid) {
			if (mount_path) {
				gchar *content_type;
				gboolean is_multimedia;
				gboolean is_blank;

				content_type = mount_guess_content_type (mount, &is_optical, &is_multimedia, &is_blank);

				/* Note: for GMounts without GVolume, is_blank should NOT be considered,
				 * as it may give unwanted results... */
				if (!is_multimedia) {
					uuid = g_compute_checksum_for_string (G_CHECKSUM_MD5,
					                                      mount_path,
					                                      -1);
					g_debug ("  No UUID, generated:'%s' (based on mount path)", uuid);
				} else {
					g_debug ("  Being ignored because mount is music/video "
					         "(content type:%s, optical:%s, multimedia: yes)",
					         content_type,
					         is_optical ? "yes" : "no");
				}

				g_free (content_type);
			} else {
				g_debug ("  Being ignored because mount has no GVolume (i.e. not user mountable) "
				         "and has no mount root path available");
			}
		}
	}

	/* If we got something to be used as UUID, then add the mount
	 * to the TrackerStorage */
	if (uuid && mount_path && !g_hash_table_lookup (priv->mounts_by_uuid, uuid)) {
		g_debug ("  Adding mount point with UUID: '%s', removable: %s, optical: %s, path: '%s'",
		         uuid,
		         is_removable ? "yes" : "no",
		         is_optical ? "yes" : "no",
		         mount_path);
		mount_add_new (storage, uuid, mount_path, mount_name, is_removable, is_optical);
	} else {
		g_debug ("  Skipping mount point with UUID: '%s', path: '%s', already managed: '%s'",
		         uuid ? uuid : "none",
		         mount_path ? mount_path : "none",
		         (uuid && g_hash_table_lookup (priv->mounts_by_uuid, uuid)) ? "yes" : "no");
	}

	g_free (mount_name);
	g_free (mount_path);
	g_free (uuid);
	g_object_unref (root);
}

static gboolean
mounts_setup (TrackerStorage *storage)
{
	TrackerStoragePrivate *priv;
	GList *mounts, *lm;

	priv = tracker_storage_get_instance_private (storage);

	mounts = g_volume_monitor_get_mounts (priv->volume_monitor);

	if (!mounts) {
		g_debug ("No mounts found to iterate");
		return TRUE;
	}

	/* Iterate over all available mounts and add them.
	 * Note that GVolumeMonitor shows only those mounts which are
	 * actually mounted. */
	for (lm = mounts; lm; lm = g_list_next (lm)) {
		mount_add (storage, lm->data);
		g_object_unref (lm->data);
	}

	g_list_free (mounts);

	return TRUE;
}

static void
mount_added_cb (GVolumeMonitor *monitor,
                GMount         *mount,
                gpointer        user_data)
{
	mount_add (user_data, mount);
}

static void
mount_remove (TrackerStorage *storage,
              GMount         *mount)
{
	TrackerStoragePrivate *priv;
	MountInfo *info;
	GNode *node;
	GFile *file;
	gchar *name;
	gchar *mount_point;
	gchar *mp;

	priv = tracker_storage_get_instance_private (storage);

	file = g_mount_get_root (mount);
	mount_point = g_file_get_path (file);
	name = g_mount_get_name (mount);

	mp = mount_point_normalize (mount_point);
	node = mount_node_find (priv->mounts, mp);
	g_free (mp);

	if (node) {
		info = node->data;

		TRACKER_NOTE (MONITORS,
		              g_message ("Mount:'%s' with UUID:'%s' now unmounted from:'%s'",
		                         name,
		                         info->uuid,
		                         mount_point));

		g_signal_emit (storage, signals[MOUNT_POINT_REMOVED], 0, info->uuid, mount_point, NULL);

		g_hash_table_remove (priv->mounts_by_uuid, info->uuid);
		mount_node_free (node);
	} else {
		TRACKER_NOTE (MONITORS,
		              g_message ("Mount:'%s' now unmounted from:'%s' (was not tracked)",
		                         name,
		                         mount_point));
	}

	g_free (name);
	g_free (mount_point);
	g_object_unref (file);
}

static void
mount_removed_cb (GVolumeMonitor *monitor,
                  GMount         *mount,
                  gpointer        user_data)
{
	TrackerStorage *storage;
	TrackerStoragePrivate *priv;

	storage = user_data;
	priv = tracker_storage_get_instance_private (storage);

	mount_remove (storage, mount);

	/* Unmount suceeded, remove the pending check */
	g_hash_table_remove (priv->unmount_watchdogs, mount);
}

static gboolean
unmount_failed_cb (gpointer user_data)
{
	UnmountCheckData *data = user_data;
	TrackerStoragePrivate *priv;

	/* If this timeout gets to be executed, this is due
	 * to a pre-unmount signal with no corresponding
	 * unmount in a timely fashion, we assume this is
	 * due to an error, and add back the still mounted
	 * path.
	 */
	priv = tracker_storage_get_instance_private (data->storage);

	g_warning ("Unmount operation failed, adding back mount point...");

	mount_add (data->storage, data->mount);

	g_hash_table_remove (priv->unmount_watchdogs, data->mount);
	return FALSE;
}

static void
mount_pre_removed_cb (GVolumeMonitor *monitor,
                      GMount         *mount,
                      gpointer        user_data)
{
	TrackerStorage *storage;
	TrackerStoragePrivate *priv;
	UnmountCheckData *data;
	guint id;

	storage = user_data;
	priv = tracker_storage_get_instance_private (storage);

	mount_remove (storage, mount);

	/* Add check for failed unmounts */
	data = g_new (UnmountCheckData, 1);
	data->storage = storage;
	data->mount = mount;

	id = g_timeout_add_seconds_full (G_PRIORITY_DEFAULT_IDLE + 10, 3,
	                                 unmount_failed_cb,
	                                 data, (GDestroyNotify) g_free);
	g_hash_table_insert (priv->unmount_watchdogs, data->mount,
	                     GUINT_TO_POINTER (id));
}

/**
 * tracker_storage_new:
 *
 * Creates a new instance of #TrackerStorage.
 *
 * Returns: The newly created #TrackerStorage.
 *
 * Since: 0.8
 **/
TrackerStorage *
tracker_storage_new (void)
{
	return g_object_new (TRACKER_TYPE_STORAGE, NULL);
}

static void
get_mount_point_by_uuid_foreach (gpointer key,
                                 gpointer value,
                                 gpointer user_data)
{
	GetRoots *gr;
	GNode *node;
	MountInfo *info;
	TrackerStorageType mount_type;

	gr = user_data;
	node = value;
	info = node->data;
	mount_type = mount_info_get_type (info);

	/* is mount of the type we're looking for? */
	if ((gr->exact_match && mount_type == gr->type) ||
	    (!gr->exact_match && (mount_type & gr->type))) {
		gchar *normalized_mount_point;
		gint len;

		normalized_mount_point = g_strdup (info->mount_point);
		len = strlen (normalized_mount_point);

		/* Don't include trailing slashes */
		if (len > 2 && normalized_mount_point[len - 1] == G_DIR_SEPARATOR) {
			normalized_mount_point[len - 1] = '\0';
		}

		gr->roots = g_slist_prepend (gr->roots, normalized_mount_point);
	}
}

/**
 * tracker_storage_get_device_roots:
 * @storage: A #TrackerStorage
 * @type: A #TrackerStorageType
 * @exact_match: if all devices should exactly match the types
 *
 * Returns: (transfer full) (element-type utf8): a #GSList of strings
 * containing the root directories for devices with @type based on
 * @exact_match. Each element must be freed using g_free() and the
 * list itself through g_slist_free().
 *
 * Since: 0.8
 **/
GSList *
tracker_storage_get_device_roots (TrackerStorage     *storage,
                                  TrackerStorageType  type,
                                  gboolean            exact_match)
{
	TrackerStoragePrivate *priv;
	GetRoots gr;

	g_return_val_if_fail (TRACKER_IS_STORAGE (storage), NULL);

	priv = tracker_storage_get_instance_private (storage);

	gr.roots = NULL;
	gr.type = type;
	gr.exact_match = exact_match;

	g_hash_table_foreach (priv->mounts_by_uuid,
	                      get_mount_point_by_uuid_foreach,
	                      &gr);

	return gr.roots;
}

TrackerStorageType
tracker_storage_get_type_for_file (TrackerStorage *storage,
                                   GFile          *file)
{
	TrackerStoragePrivate *priv;
	g_autofree gchar *path = NULL;
	TrackerStorageType type = 0;
	MountInfo *info;

	g_return_val_if_fail (TRACKER_IS_STORAGE (storage), FALSE);

	path = g_file_get_path (file);
	if (!path)
		return type;

	/* Normalize all paths to have a / at the end */
	if (!g_str_has_suffix (path, G_DIR_SEPARATOR_S)) {
		gchar *norm_path;

		norm_path = g_strconcat (path, G_DIR_SEPARATOR_S, NULL);
		g_clear_pointer (&path, g_free);
		path = norm_path;
	}

	priv = tracker_storage_get_instance_private (storage);

	info = mount_info_find (priv->mounts, path);

	if (!info)
		return type;

	if (info->removable)
		type |= TRACKER_STORAGE_REMOVABLE;
	if (info->optical)
		type |= TRACKER_STORAGE_OPTICAL;

	return type;
}


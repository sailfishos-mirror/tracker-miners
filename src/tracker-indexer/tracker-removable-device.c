/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2008, Nokia
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 *
 * Author: Philip Van Hoof <philip@codeminded.be>
 */

/* When merging the decomposed branch to trunk: this filed used to be called
 * tracker-indexer/tracker-turtle.c. What has happened in trunk is that this
 * file got renamed to tracker-indexer/tracker-removable-device.c and that
 * we created a new file libtracker-data/tracker-turtle.c which contains some
 * of the functions that used to be available in this file. 
 *
 * The reason for that is that Ivan's backup support, which runs in trackerd,
 * needed access to the same Turtle related routines. So we moved it to one of
 * our internally shared libraries (libtracker-data got elected for this). 
 *
 * When merging the decomposed branch, simply pick this file over the file 
 * tracker-indexer/tracker-turtle.c (in the decomposed branch). */

#include "config.h"


#ifdef HAVE_RAPTOR

#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <gmodule.h>

#include <raptor.h>

#include <libtracker-data/tracker-data-query.h>
#include <libtracker-data/tracker-data-update.h>

#endif

#include <libtracker-data/tracker-turtle.h>

#include "tracker-removable-device.h"

#ifdef HAVE_RAPTOR
#include "tracker-module-metadata-private.h"

typedef struct {
	const gchar *ttl_file;
	gchar *last_subject;
	gchar *base;
	guint amount;
	TrackerIndexer *indexer;
	TrackerModuleMetadata *metadata;
	gchar *rdf_type;
} TurtleStorerInfo;

typedef enum {
	REMOVAL,
	REPLACE,
	MOVE
} StorerTask;

static void
commit_turtle_parse_info_storer (TurtleStorerInfo *info, gboolean may_flush, StorerTask task, gchar *destination)
{
	if (info->last_subject) {

		GHashTable *data;

		/* We have it as a URI, database api wants Paths. Update this when
		 * the database api becomes sane and uses URIs everywhere, the
		 * '+ 7' means that we cut away the 'file://' prefix. */

		if (info->rdf_type) {

		  /* We ignore records that didn't have an <rdf:type> 
		   * predicate. Those are just wrong anyway. */

		  switch (task) {
		    case REMOVAL:
			tracker_data_update_delete_service_by_path (info->last_subject + 7, 
								    info->rdf_type);
		    break;
		    case MOVE:
			data = tracker_module_metadata_get_hash_table (info->metadata);
			tracker_data_update_delete_service_by_path (info->last_subject + 7, 
								    info->rdf_type);
			tracker_data_update_replace_service (destination + 7, 
							     info->rdf_type, 
							     data);
			g_hash_table_destroy (data);
		    break;
		    default:
		    case REPLACE:
			data = tracker_module_metadata_get_hash_table (info->metadata);
			tracker_data_update_replace_service (info->last_subject + 7, 
							     info->rdf_type, 
							     data);
			g_hash_table_destroy (data);
		    break;
		  }
		}

		info->amount++;

		g_object_unref (info->metadata);

		g_free (info->last_subject);
		g_free (info->rdf_type);
		info->last_subject = NULL;
		info->metadata = NULL;
		info->rdf_type = NULL;
	}

	/* We commit per transaction of 100 here, and then we also iterate the
	 * mainloop so that the state-machine gets the opportunity to run for a
	 * moment */

	if (may_flush && info->amount > TRACKER_INDEXER_TRANSACTION_MAX) {
		tracker_indexer_transaction_commit (info->indexer);
		g_main_context_iteration (NULL, FALSE);
		tracker_indexer_transaction_open (info->indexer);
		info->amount = 0;
	}
}

static void
consume_triple_storer (void* user_data, const raptor_statement* triple) 
{
	TurtleStorerInfo *info = user_data;
	gchar            *subject;

	/* TODO: cope with multi-value values like User:Keywords */

	subject = (gchar *) raptor_uri_as_string ((raptor_uri *) triple->subject);

	if (!info->last_subject || g_strcmp0 (subject, info->last_subject) != 0) {

		/* Commit previous subject */
		commit_turtle_parse_info_storer (info, TRUE, REPLACE, NULL);

		/* Install next subject */
		info->last_subject = g_strdup (subject);
		info->metadata = tracker_module_metadata_new ();
	}

	if (triple->object_type == RAPTOR_IDENTIFIER_TYPE_LITERAL) {
		gchar *predicate;

		predicate = g_strdup ((const gchar *) raptor_uri_as_string ((raptor_uri *) triple->predicate));

		if (g_strcmp0 (predicate, "rdf:type") == 0) {
			g_free (info->rdf_type);

			/* TODO: ontology */
			/* Change this when Files and Emails becomes File and Email */

			info->rdf_type = g_strdup_printf ("%ss", (gchar *) triple->object);
		} else {
			tracker_module_metadata_add_string (info->metadata,
							    predicate,
							    triple->object);
		}

	} else if (triple->object_type == RAPTOR_IDENTIFIER_TYPE_RESOURCE) {
		gchar *key = g_strdup_printf ("file://%s/", info->base);

		if (g_strcmp0 (key, triple->object) == 0 && 
		    g_strcmp0 (key, triple->predicate) == 0) 
		   {
			/* <URI> <rdf:type> "Type" ;                    *
			 *       <> <>  -  is a removal of the resource */
			   
			/* We commit this subject as a removal, the last_subject
			 * field will be cleared for the next subject to be set
			 * ready first next process loop. */

			commit_turtle_parse_info_storer (info, FALSE, REMOVAL, NULL);
		   } 
		else 
		if (g_strcmp0 (key, triple->object) == 0 && 
		    g_strcmp0 (key, triple->predicate) != 0) 
		   {
			gchar        *predicate;

			/* <URI> <rdf:type> "Type" ;                             *
			 *       <Pfx:Predicate> <>  -  is a removal of the      * 
			 *                              resource's Pfx:Predicate */

			predicate = (gchar *) raptor_uri_as_string ((raptor_uri *) triple->predicate);

			/* We put NULL here, so that a null value goes into 
			 * SQLite. Perhaps we should change this? If so, Why? */

			tracker_module_metadata_add_string (info->metadata,
							    predicate,
							    NULL);

		   }
		else
		if (g_strcmp0 (key, triple->object) != 0 && 
		    g_strcmp0 (key, triple->predicate) == 0) 
		   {
			gchar        *object;
			/* <URI> <> <to-URI>  -  is a move of the subject */
			object = (gchar *) raptor_uri_as_string ((raptor_uri *) triple->object);
			commit_turtle_parse_info_storer (info, FALSE, MOVE, object);

		   }
		g_free (key);
	}

}

#endif /* HAVE_RAPTOR */

void
tracker_removable_device_optimize (TrackerIndexer *indexer, const gchar *mount_point)
{
	gchar *file = g_build_filename (mount_point, ".cache", 
					 "metadata", "metadata.ttl", NULL);

	if (g_file_test (file, G_FILE_TEST_EXISTS)) {
		tracker_turtle_optimize (file);
	}

	g_free (file);
}

void
tracker_removable_device_load (TrackerIndexer *indexer, const gchar *mount_point)
{
#ifdef HAVE_RAPTOR
	gchar           *file;

	file = g_build_filename (mount_point, ".cache", 
				 "metadata", "metadata.ttl", NULL);

	if (g_file_test (file, G_FILE_TEST_EXISTS)) {
		TurtleStorerInfo *info;
		gchar            *base_uri;

		info = g_slice_new0 (TurtleStorerInfo);

		info->ttl_file = file;
		info->indexer = g_object_ref (indexer);
		info->amount = 0;

		info->base = g_strdup (mount_point);

		/* We need to open the transaction, during the parsing will the
		 * transaction be committed and reopened */

		tracker_indexer_transaction_open (info->indexer);

		base_uri = g_strdup_printf ("file://%s/", mount_point);
		tracker_turtle_process (file, base_uri, consume_triple_storer, info);
		g_free (base_uri);

		/* Commit final subject (our loop doesn't handle the very last) 
		 * It can't be a REMOVAL nor MOVE as those happen immediately. */

		commit_turtle_parse_info_storer (info, FALSE, REPLACE, NULL);


		/* We will (always) be left in open state, so we commit the 
		 * last opened transaction */

		tracker_indexer_transaction_commit (info->indexer);

		g_free (info->base);
		g_object_unref (info->indexer);
		g_slice_free (TurtleStorerInfo, info);
	}

	g_free (file);

#endif /* HAVE_RAPTOR */

}

#ifdef HAVE_RAPTOR
typedef struct {
	raptor_serializer *serializer;
	gchar *about_uri;
} AddMetadataInfo;

static void
set_metadata (const gchar *key, const gchar *value, gpointer user_data)
{
	raptor_statement    *statement;
	AddMetadataInfo     *item = user_data;
	const gchar         *about_uri = item->about_uri;
	raptor_serializer   *serializer = item->serializer;

	statement = g_new0 (raptor_statement, 1);

	statement->subject = (void *) raptor_new_uri (about_uri);
	statement->subject_type = RAPTOR_IDENTIFIER_TYPE_RESOURCE;

	statement->predicate = (void *) raptor_new_uri (key);
	statement->predicate_type = RAPTOR_IDENTIFIER_TYPE_RESOURCE;

	statement->object = (unsigned char *) g_strdup (value);
	statement->object_type = RAPTOR_IDENTIFIER_TYPE_LITERAL;

	raptor_serialize_statement (serializer, 
				    statement);

	raptor_free_uri ((raptor_uri *) statement->subject);
	raptor_free_uri ((raptor_uri *) statement->predicate);
	g_free ((unsigned char *) statement->object);

	g_free (statement);
}
#endif


/* TODO URI branch: path -> uri */
#ifdef HAVE_RAPTOR
static void
foreach_in_metadata_set_metadata (TrackerField *field,
				  gpointer      value,
				  gpointer      user_data)
{
	if (!tracker_field_get_multiple_values (field)) {
		set_metadata (tracker_field_get_name (field), value, user_data);
	} else {
		GList *list;

		list = value;

		while (list) {
			set_metadata (tracker_field_get_name (field), list->data, user_data);
			list = list->next;
		}
	}

}
#endif

void
tracker_removable_device_add_metadata (TrackerIndexer        *indexer, 
				       const gchar           *mount_point, 
				       const gchar           *path,
				       const gchar           *rdf_type,
				       TrackerModuleMetadata *metadata)
{
#ifdef HAVE_RAPTOR
	AddMetadataInfo *info = g_slice_new (AddMetadataInfo);
	gchar           *file, *muri;
	FILE            *target_file;
	raptor_uri      *suri;

	file = g_build_filename (mount_point, ".cache", 
				 "metadata", NULL);
	g_mkdir_with_parents (file, 0700);
	g_free (file);

	file = g_build_filename (mount_point, ".cache", 
				 "metadata", "metadata.ttl", NULL);

	target_file = fopen (file, "a");
	/* Similar to a+ */
	if (!target_file) 
		target_file = fopen (file, "w");

	if (!target_file) {
		g_free (target_file);
		g_free (file);
		return;
	}

	info->serializer = raptor_new_serializer ("turtle");
	info->about_uri = g_strdup (path+strlen (mount_point)+1);

	raptor_serializer_set_feature (info->serializer, 
				       RAPTOR_FEATURE_WRITE_BASE_URI, 0);

	raptor_serializer_set_feature (info->serializer, 
				       RAPTOR_FEATURE_ASSUME_IS_RDF, 1);

	raptor_serializer_set_feature (info->serializer, 
				       RAPTOR_FEATURE_ALLOW_NON_NS_ATTRIBUTES, 1);

	muri = g_strdup_printf ("file://%s/base", mount_point);
	suri = raptor_new_uri (muri);
	g_free (muri);

	raptor_serialize_start_to_file_handle (info->serializer, 
					       suri, target_file);

	set_metadata ("rdf:type", rdf_type, info);

	tracker_module_metadata_foreach (metadata, 
					 foreach_in_metadata_set_metadata,
					 info);

	g_free (info->about_uri);
	raptor_serialize_end (info->serializer);
	raptor_free_serializer (info->serializer);
	fclose (target_file);
	raptor_free_uri (suri);

	g_slice_free (AddMetadataInfo, info);
#endif /* HAVE_RAPTOR */
}

/* TODO URI branch: path -> uri */

void
tracker_removable_device_add_removal (TrackerIndexer *indexer, 
				      const gchar *mount_point, 
				      const gchar *path,
				      const gchar *rdf_type)
{
#ifdef HAVE_RAPTOR
	gchar               *file, *about_uri;
	FILE                *target_file;
	raptor_uri          *suri;
	raptor_serializer   *serializer;
	AddMetadataInfo     *info;

	file = g_build_filename (mount_point, ".cache", 
				 "metadata", NULL);
	g_mkdir_with_parents (file, 0700);
	g_free (file);

	file = g_build_filename (mount_point, ".cache", 
				 "metadata", "metadata.ttl", NULL);

	target_file = fopen (file, "a");
	/* Similar to a+ */
	if (!target_file) 
		target_file = fopen (file, "w");

	if (!target_file) {
		g_free (target_file);
		g_free (file);
		return;
	}

	serializer = raptor_new_serializer ("turtle");
	about_uri = g_strdup (path+strlen (mount_point)+1);

	raptor_serializer_set_feature (serializer, 
				       RAPTOR_FEATURE_WRITE_BASE_URI, 0);

	raptor_serialize_start_to_file_handle (serializer, 
					       suri, target_file);

	info = g_slice_new (AddMetadataInfo);

	info->serializer = serializer;
	info->about_uri = about_uri;

	set_metadata ("rdf:type", rdf_type, info);
	set_metadata (NULL, NULL, info);

	g_slice_free (AddMetadataInfo, info);
	g_free (about_uri);
	raptor_serialize_end (serializer);
	raptor_free_serializer (serializer);
	fclose (target_file);

#endif /* HAVE_RAPTOR */
}

/* TODO URI branch: path -> uri */

void
tracker_removable_device_add_move (TrackerIndexer *indexer, 
				   const gchar *mount_point, 
				   const gchar *from_path, 
				   const gchar *to_path,
				   const gchar *rdf_type)
{
#ifdef HAVE_RAPTOR
	gchar               *file, *about_uri, *to_uri, *muri;
	FILE                *target_file;
	raptor_uri          *suri;
	raptor_statement    *statement;
	raptor_serializer   *serializer;
	AddMetadataInfo     *info;

	file = g_build_filename (mount_point, ".cache", 
				 "metadata", NULL);
	g_mkdir_with_parents (file, 0700);
	g_free (file);

	file = g_build_filename (mount_point, ".cache", 
				 "metadata", "metadata.ttl", NULL);

	target_file = fopen (file, "a");
	/* Similar to a+ */
	if (!target_file) 
		target_file = fopen (file, "w");

	if (!target_file) {
		g_free (target_file);
		g_free (file);
		return;
	}

	serializer = raptor_new_serializer ("turtle");

	raptor_serializer_set_feature (serializer, 
				       RAPTOR_FEATURE_WRITE_BASE_URI, 0);

	about_uri = g_strdup (from_path+strlen (mount_point)+1);
	to_uri = g_strdup (to_path+strlen (mount_point)+1);

	muri = g_strdup_printf ("file://%s/", mount_point);
	suri = raptor_new_uri (muri);
	g_free (muri);

	raptor_serialize_start_to_file_handle (serializer, 
					       suri, target_file);

	info = g_slice_new (AddMetadataInfo);

	info->serializer = serializer;
	info->about_uri = about_uri;

	set_metadata ("rdf:type", rdf_type, info);
	set_metadata (NULL, to_uri, info);

	g_slice_free (AddMetadataInfo, info);


	g_free (about_uri);
	g_free (to_uri);
	raptor_serialize_end (serializer);
	raptor_free_serializer (serializer);
	fclose (target_file);
	raptor_free_uri (suri);


#endif /* HAVE_RAPTOR */
}


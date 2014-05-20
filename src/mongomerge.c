/*
 * Copyright 2014 MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * This program will scan each BSON document contained in the provided files
 * and print metrics to STDOUT.
 */

#include <mongoc.h>
#include <stdio.h>
#include "mongomerge.h"

bson_t *
bson_new_from_iter_document (bson_iter_t *iter)
{
   uint32_t document_len;
   const uint8_t *document;
   BSON_ITER_HOLDS_DOCUMENT (iter) || DIE;
   bson_iter_document (iter, &document_len, &document);
   return bson_new_from_data (document, document_len);
}

bson_t *
bson_new_from_iter_array (bson_iter_t *iter)
{
   bson_t *bson;
   bson_iter_t iter_array;
   BSON_ITER_HOLDS_ARRAY (iter) || DIE;
   bson_iter_recurse (iter, &iter_array) || DIE;
   bson = bson_new ();
   while (bson_iter_next (&iter_array)) {
      bson_t *bson_sub = bson_new_from_iter_document (&iter_array);
      bson_append_document (bson, bson_iter_key (&iter_array), -1, bson_sub) || DIE;
      bson_destroy (bson_sub);
   }
   return bson;
}

void
bson_printf (const char *format,
             bson_t     *bson)
{
   char *str;
   str = bson_as_json (bson, NULL);
   printf (format, str);
   bson_free (str);
}

void
mongoc_cursor_dump (mongoc_cursor_t *cursor)
{
   const bson_t *doc;
   while (mongoc_cursor_next (cursor, &doc)) {
      char *str;
      str = bson_as_json (doc, NULL);
      printf ("%s\n", str);
      bson_free (str);
   }
}

void
mongoc_collection_dump (mongoc_collection_t *collection)
{
   bson_t bson = BSON_INITIALIZER;
   mongoc_cursor_t *cursor;
   cursor = mongoc_collection_find (collection, MONGOC_QUERY_NONE, 0, 0, 0, &bson, NULL, NULL);
   mongoc_cursor_dump (cursor);
   mongoc_cursor_destroy (cursor);
}

bool
mongoc_collection_remove_all (mongoc_collection_t *collection)
{
   bson_t bson = BSON_INITIALIZER;
   bool r;
   bson_error_t error;
   (r = mongoc_collection_delete (collection, MONGOC_DELETE_NONE, &bson, NULL, &error)) || WARN_ERROR;
   return (r);
}

mongoc_cursor_t *
mongoc_collection_aggregate_pipeline (mongoc_collection_t       *collection, /* IN */
                                      mongoc_query_flags_t       flags,      /* IN */
                                      const bson_t              *pipeline,   /* IN */
                                      const bson_t              *options,    /* IN */
                                      const mongoc_read_prefs_t *read_prefs) /* IN */
{
   bson_t *subpipeline;
   bson_iter_t iter;
   mongoc_cursor_t *cursor;

   bson_iter_init_find (&iter, pipeline, "pipeline") || DIE;
   subpipeline = bson_new_from_iter_array (&iter);
   cursor = mongoc_collection_aggregate (collection, flags, subpipeline, options, read_prefs);
   bson_destroy (subpipeline);
   return cursor;
}

bson_t *
child_by_merge_key(const char *parent_key, const char *child_name, const char *child_key)
{
   bson_t *bson;
   size_t dollar_child_key_size = strlen("$") + strlen(child_key);
   char *dollar_child_key = bson_malloc (dollar_child_key_size + 1);
   bson_snprintf (dollar_child_key, dollar_child_key_size, "$%s", child_key);
   bson = BCON_NEW (
      "pipeline", "[",
         "{",
            "$project", "{",
               "_id", BCON_INT32(0),
               "child_name", "{", "$literal", child_name, "}",
               "merge_id", dollar_child_key,
               parent_key, "$$ROOT",
            "}",
         "}",
      "]"
   );
   bson_free (dollar_child_key);
   return bson;
}

bson_t *
parent_child_merge_key(const char *parent_key, const char *child_name, const char *child_key)
{
   bson_t *bson;
   size_t parent_key_dot_child_key_size = strlen("$") + strlen(parent_key) + strlen(".") + strlen(child_key);
   char *parent_key_dot_child_key = bson_malloc (parent_key_dot_child_key_size + 1);
   bson_snprintf (parent_key_dot_child_key, parent_key_dot_child_key_size, "$%s.%s", parent_key, child_key);
   bson = BCON_NEW (
      "pipeline", "[",
         "{",
            "$project", "{",
              "_id", BCON_INT32(0),
              "child_name", "{", "$literal", child_name, "}",
              "merge_id", "{", "$ifNull", "[", parent_key_dot_child_key, parent_key, "]", "}",
              "parent_id", "$_id",
            "}",
         "}",
      "]"
   );
   bson_free (parent_key_dot_child_key);
   return bson;
}

bson_t *
merge_one_all(bson_t *accumulators, bson_t *projectors)
{
   bson_t *bson;
   bson = BCON_NEW (
      "pipeline", "[",
         "{", "$group", "{",
                 "_id", "{",
                    "child_name", "$child_name",
                    "merge_id", "$merge_id", "}",
                 "parent_id", "{",
                    "$push", "$parent_id", "}", "}", //.merge(accumulators)
          "}",
          "{", "$unwind", "$parent_id", "}",
          "{", "$group", "{",
                  "_id", "$parent_id", "}", //.merge(accumulators)
          "}",
          "{", "$project", "{",
                  "_id", BCON_INT32(0),
                  "parent_id", "$_id", "}", //.merge(projectors)
          "}",
      "]"
   );
   return bson;
}

bson_t *
copy_many_with_parent_id(const char *parent_key, const char *child_name, const char *child_key)
{
   return BCON_NEW (
      "pipeline", "[",
          "{", "$match", "{", child_key, "{", "$ne", BCON_NULL, "}", "}", "}",
          "{", "$project", "{",
                  "_id", BCON_INT32(0),
                  "parent_id", "$#{child_key}",
                  parent_key, "$$ROOT", "}", "}",
      "]"
   );
}

bson_t *
expand_spec(const char *parent_name, int merge_spec_count, char **merge_spec)
{
   bson_t *bson, bson_array;
   int i;
   printf("parent_name:\"%s\" merge_spec_count:%d\n", parent_name,merge_spec_count);

   bson = bson_new();
   bson_append_array_begin(bson, "merge_spec", -1, &bson_array);
   for (i = 0; i < merge_spec_count; i++) {
      char *s, *relation, *parent_key, *child_s, *child_name, *child_key, *colon;
      printf("merge_spec[%d]:\"%s\"\n", i, merge_spec[i]);
      s = bson_malloc(strlen(merge_spec[i]) + 1);
      strcpy(s, merge_spec[i]);
      parent_key = child_name = child_s = s;
      colon = strchr(s, ':');
      if (colon != NULL) {
         *colon = '\0';
         child_s = colon + 1;
      }
      if (*child_s != '[') {
         relation = "one";
         child_key = "_id";
      }
      else {
         char *terminator;
         child_s += 1;
         terminator = strchr(child_s, ']');
         (terminator != NULL && *(terminator + 1) == '\0') || DIE;
         *terminator = '\0';
         relation = "many";
         child_key = parent_name;
      }
      char *dot = strchr(child_s, '.');
      if (dot != NULL) {
         *dot = '\0';
         child_key = dot + 1;
      }
      if (*child_s != '\0') {
         child_name = child_s;
      }
      // check non-empty, legal chars
      printf("relation:\"%s\" parent_key:\"%s\" child_name:\"%s\" child_key:\"%s\"\n", relation, parent_key, child_name, child_key);
      BCON_APPEND(&bson_array, "0", "[", relation, parent_key, child_name, child_key, "]");
      bson_free (s);
   }
   bson_append_array_end(bson, &bson_array);
   return bson;
}

void
execute(const char *parent_name, int merge_spec_count, char **merge_spec)
{
   bson_t *bson;
   bson = expand_spec(parent_name, merge_spec_count, merge_spec);
   bson_destroy (bson);
}

#ifdef MAIN
int
main (int argc,
      char *argv[])
{
   const char *uristr = "mongodb://localhost/test";
   const char *database_name;
   mongoc_uri_t *uri;
   mongoc_client_t *client;
   mongoc_collection_t *collection;
   mongoc_cursor_t *cursor;
   const bson_t *doc;
   bson_t *query;
   char *str;

   mongoc_init ();

   uristr = getenv("MONGODB_URI");
   uri = mongoc_uri_new (uristr);
   client = mongoc_client_new (uristr);
   database_name = mongoc_uri_get_database (uri);
   collection = mongoc_client_get_collection (client, database_name, "test");
   query = bson_new ();
   cursor = mongoc_collection_find (collection, MONGOC_QUERY_NONE, 0, 0, 0, query, NULL, NULL);

   while (mongoc_cursor_next (cursor, &doc)) {
      str = bson_as_json (doc, NULL);
      printf ("%s\n", str);
      bson_free (str);
   }

   bson_destroy (query);
   mongoc_cursor_destroy (cursor);
   mongoc_collection_destroy (collection);
   mongoc_client_destroy (client);

   mongoc_cleanup ();

   return 0;
}
#endif

/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include "ot-builtins.h"
#include "ostree.h"

static char *opt_subject;
static char *opt_body;
static char *opt_branch;
static char *opt_statoverride_file;
static gboolean opt_link_checkout_speedup;
static gboolean opt_skip_if_unchanged;
static gboolean opt_tar_autocreate_parents;
static gboolean opt_no_xattrs;
static char **opt_trees;
static gint opt_owner_uid = -1;
static gint opt_owner_gid = -1;

static GOptionEntry options[] = {
  { "subject", 's', 0, G_OPTION_ARG_STRING, &opt_subject, "One line subject", "subject" },
  { "body", 'm', 0, G_OPTION_ARG_STRING, &opt_body, "Full description", "body" },
  { "branch", 'b', 0, G_OPTION_ARG_STRING, &opt_branch, "Branch", "branch" },
  { "tree", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_trees, "Overlay the given argument as a tree", "NAME" },
  { "owner-uid", 0, 0, G_OPTION_ARG_INT, &opt_owner_uid, "Set file ownership user id", "UID" },
  { "owner-gid", 0, 0, G_OPTION_ARG_INT, &opt_owner_gid, "Set file ownership group id", "GID" },
  { "no-xattrs", 0, 0, G_OPTION_ARG_NONE, &opt_no_xattrs, "Do not import extended attributes", NULL },
  { "link-checkout-speedup", 0, 0, G_OPTION_ARG_NONE, &opt_link_checkout_speedup, "Optimize for commits of trees composed of hardlinks into the repository", NULL },
  { "tar-autocreate-parents", 0, 0, G_OPTION_ARG_NONE, &opt_tar_autocreate_parents, "When loading tar archives, automatically create parent directories as needed", NULL },
  { "skip-if-unchanged", 0, 0, G_OPTION_ARG_NONE, &opt_skip_if_unchanged, "If the contents are unchanged from previous commit, do nothing", NULL },
  { "statoverride", 0, 0, G_OPTION_ARG_FILENAME, &opt_statoverride_file, "File containing list of modifications to make to permissions", "path" },
  { NULL }
};

static gboolean
parse_statoverride_file (GHashTable   **out_mode_add,
                         GCancellable  *cancellable,
                         GError        **error)
{
  gboolean ret = FALSE;
  gsize len;
  char **iter = NULL; /* nofree */
  gs_unref_hashtable GHashTable *ret_hash = NULL;
  gs_unref_object GFile *path = NULL;
  gs_free char *contents = NULL;
  char **lines = NULL;

  path = g_file_new_for_path (opt_statoverride_file);

  if (!g_file_load_contents (path, cancellable, &contents, &len, NULL,
                             error))
    goto out;
  
  ret_hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  lines = g_strsplit (contents, "\n", -1);

  for (iter = lines; iter && *iter; iter++)
    {
      const char *line = *iter;

      if (*line == '+')
        {
          const char *spc;
          guint mode_add;

          spc = strchr (line + 1, ' ');
          if (!spc)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Malformed statoverride file");
              goto out;
            }
          
          mode_add = (guint32)(gint32)g_ascii_strtod (line + 1, NULL);
          g_hash_table_insert (ret_hash,
                               g_strdup (spc + 1),
                               GUINT_TO_POINTER((gint32)mode_add));
        }
    }

  ret = TRUE;
  ot_transfer_out_value (out_mode_add, &ret_hash);
 out:
  g_strfreev (lines);
  return ret;
}

static OstreeRepoCommitFilterResult
commit_filter (OstreeRepo         *self,
               const char         *path,
               GFileInfo          *file_info,
               gpointer            user_data)
{
  GHashTable *mode_adds = user_data;
  gpointer value;

  if (opt_owner_uid >= 0)
    g_file_info_set_attribute_uint32 (file_info, "unix::uid", opt_owner_uid);
  if (opt_owner_gid >= 0)
    g_file_info_set_attribute_uint32 (file_info, "unix::gid", opt_owner_gid);

  if (mode_adds && g_hash_table_lookup_extended (mode_adds, path, NULL, &value))
    {
      guint current_mode = g_file_info_get_attribute_uint32 (file_info, "unix::mode");
      guint mode_add = GPOINTER_TO_UINT (value);
      g_file_info_set_attribute_uint32 (file_info, "unix::mode",
                                        current_mode | mode_add);
      g_hash_table_remove (mode_adds, path);
    }
  
  return OSTREE_REPO_COMMIT_FILTER_ALLOW;
}

gboolean
ostree_builtin_commit (int argc, char **argv, GFile *repo_path, GCancellable *cancellable, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  gboolean skip_commit = FALSE;
  gboolean in_transaction = FALSE;
  gs_unref_object OstreeRepo *repo = NULL;
  gs_unref_object GFile *arg = NULL;
  gs_free char *parent = NULL;
  gs_free char *commit_checksum = NULL;
  gs_unref_variant GVariant *parent_commit = NULL;
  gs_free char *contents_checksum = NULL;
  gs_unref_object OstreeMutableTree *mtree = NULL;
  gs_free char *tree_type = NULL;
  gs_unref_hashtable GHashTable *mode_adds = NULL;
  gs_unref_variant GVariant *parent_content_csum_v = NULL;
  gs_unref_variant GVariant *parent_metadata_csum_v = NULL;
  gs_free char *parent_content_checksum = NULL;
  gs_free char *parent_metadata_checksum = NULL;
  OstreeRepoCommitModifier *modifier = NULL;

  context = g_option_context_new ("[ARG] - Commit a new revision");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (opt_statoverride_file)
    {
      if (!parse_statoverride_file (&mode_adds, cancellable, error))
        goto out;
    }

  repo = ostree_repo_new (repo_path);
  if (!ostree_repo_check (repo, error))
    goto out;

  if (!opt_branch)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "A branch must be specified with --branch");
      goto out;
    }

  if (!opt_subject)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "A subject must be specified with --subject");
      goto out;
    }

  if (opt_owner_uid >= 0 || opt_owner_gid >= 0 || opt_statoverride_file != NULL
      || opt_no_xattrs)
    {
      modifier = ostree_repo_commit_modifier_new ();
      modifier->skip_xattrs = opt_no_xattrs;
      modifier->filter = commit_filter;
      modifier->user_data = mode_adds;
    }

  if (!ostree_repo_resolve_rev (repo, opt_branch, TRUE, &parent, error))
    goto out;

  if (opt_skip_if_unchanged && parent)
    {
      if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT,
                                     parent, &parent_commit, error))
        goto out;
    }

  if (!ostree_repo_prepare_transaction (repo, opt_link_checkout_speedup, NULL, cancellable, error))
    goto out;

  in_transaction = TRUE;

  mtree = ostree_mutable_tree_new ();

  if (argc == 1 && (opt_trees == NULL || opt_trees[0] == NULL))
    {
      char *current_dir = g_get_current_dir ();
      arg = g_file_new_for_path (current_dir);
      g_free (current_dir);

      if (!ostree_repo_stage_directory_to_mtree (repo, arg, mtree, modifier,
                                                 cancellable, error))
        goto out;
    }
  else
    {
      const char *const*tree_iter;
      const char *tree;
      const char *eq;

      for (tree_iter = (const char *const*)opt_trees; *tree_iter; tree_iter++)
        {
          tree = *tree_iter;

          eq = strchr (tree, '=');
          if (!eq)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Missing type in tree specification '%s'", tree);
              goto out;
            }
          g_free (tree_type);
          tree_type = g_strndup (tree, eq - tree);
          tree = eq + 1;

          g_clear_object (&arg);
          if (strcmp (tree_type, "dir") == 0)
            {
              arg = g_file_new_for_path (tree);
              if (!ostree_repo_stage_directory_to_mtree (repo, arg, mtree, modifier,
                                                         cancellable, error))
                goto out;
            }
          else if (strcmp (tree_type, "tar") == 0)
            {
              arg = g_file_new_for_path (tree);
              if (!ostree_repo_stage_archive_to_mtree (repo, arg, mtree, modifier,
                                                       opt_tar_autocreate_parents,
                                                       cancellable, error))
                goto out;
            }
          else if (strcmp (tree_type, "ref") == 0)
            {
              if (!ostree_repo_read_commit (repo, tree, &arg, cancellable, error))
                goto out;

              if (!ostree_repo_stage_directory_to_mtree (repo, arg, mtree, modifier,
                                                         cancellable, error))
                goto out;
            }
          else
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Invalid tree type specification '%s'", tree_type);
              goto out;
            }
        }
    }

  if (mode_adds && g_hash_table_size (mode_adds) > 0)
    {
      GHashTableIter hash_iter;
      gpointer key, value;

      g_hash_table_iter_init (&hash_iter, mode_adds);

      while (g_hash_table_iter_next (&hash_iter, &key, &value))
        {
          g_printerr ("Unmatched statoverride path: %s\n", (char*)key);
        }
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unmatched statoverride paths");
      goto out;
    }
          
  if (!ostree_repo_stage_mtree (repo, mtree, &contents_checksum, cancellable, error))
    goto out;

  if (opt_skip_if_unchanged && parent_commit)
    {
      g_variant_get_child (parent_commit, 6, "@ay", &parent_content_csum_v);
      g_variant_get_child (parent_commit, 7, "@ay", &parent_metadata_csum_v);

      parent_content_checksum = ostree_checksum_from_bytes_v (parent_content_csum_v);
      parent_metadata_checksum = ostree_checksum_from_bytes_v (parent_metadata_csum_v);

      if (strcmp (contents_checksum, parent_content_checksum) == 0
          && strcmp (ostree_mutable_tree_get_metadata_checksum (mtree),
                     parent_metadata_checksum) == 0)
        skip_commit = TRUE;
    }

  if (!skip_commit)
    {
      const char *root_metadata = ostree_mutable_tree_get_metadata_checksum (mtree);
      
      if (!root_metadata)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Can't commit an empty tree");
          goto out;
        }

      if (!ostree_repo_stage_commit (repo, opt_branch, parent, opt_subject, opt_body,
                                     contents_checksum, root_metadata,
                                     &commit_checksum, cancellable, error))
        goto out;

      if (!ostree_repo_commit_transaction (repo, cancellable, error))
        goto out;

      in_transaction = FALSE;
      
      if (!ostree_repo_write_ref (repo, NULL, opt_branch, commit_checksum, error))
        goto out;

      g_print ("%s\n", commit_checksum);
    }
  else
    {
      if (!ostree_repo_abort_transaction (repo, cancellable, error))
        goto out;

      in_transaction = FALSE;

      g_print ("%s\n", parent);
    }

  ret = TRUE;
 out:
  if (in_transaction)
    {
      (void) ostree_repo_abort_transaction (repo, cancellable, NULL);
    }
  if (context)
    g_option_context_free (context);
  if (modifier)
    ostree_repo_commit_modifier_unref (modifier);
  return ret;
}

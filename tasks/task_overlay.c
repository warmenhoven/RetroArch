/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>

#include <compat/strl.h>
#include <compat/posix_string.h>
#include <retro_miscellaneous.h>
#include <file/file_path.h>
#include <file/config_file.h>
#include <file/archive_file.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>
#include <formats/image.h>
#include <lrc_hash.h>

#include "tasks_internal.h"

#include "../gfx/video_driver.h"
#include "../input/input_driver.h"
#include "../input/input_overlay.h"
#include "../input/input_remapping.h"
#include "../verbosity.h"

/* Zip file magic bytes: "PK\x03\x04" */
#define ZIP_MAGIC_BYTE_0 0x50
#define ZIP_MAGIC_BYTE_1 0x4B
#define ZIP_MAGIC_BYTE_2 0x03
#define ZIP_MAGIC_BYTE_3 0x04

typedef struct overlay_loader overlay_loader_t;

struct overlay_loader
{
   config_file_t *conf;
   char *overlay_path;
   char *zip_path;       /* Path to zip file when loading from archive */
   char *zip_cfg_path;   /* Internal path to .cfg within zip */
   struct overlay *overlays;
   struct overlay *active;

   size_t resolve_pos;
   unsigned size;
   unsigned pos;
   unsigned pos_increment;

   enum overlay_status state;
   enum overlay_image_transfer_status loading_status;

   uint16_t overlay_types;

   uint8_t flags;
};

/**
 * overlay_file_is_zip:
 * @path: Path to file to check
 *
 * Check if file is a zip archive by reading magic bytes.
 * Does not rely on file extension.
 *
 * Returns: true if file is a zip archive, false otherwise.
 */
static bool overlay_file_is_zip(const char *path)
{
   RFILE *file;
   uint8_t magic[4];
   bool is_zip = false;

   if (string_is_empty(path))
      return false;

   file = filestream_open(path,
         RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);

   if (!file)
      return false;

   if (filestream_read(file, magic, 4) == 4)
   {
      is_zip = (magic[0] == ZIP_MAGIC_BYTE_0 &&
                magic[1] == ZIP_MAGIC_BYTE_1 &&
                magic[2] == ZIP_MAGIC_BYTE_2 &&
                magic[3] == ZIP_MAGIC_BYTE_3);
   }

   filestream_close(file);
   return is_zip;
}

/**
 * overlay_zip_read_file:
 * @zip_path: Path to zip archive
 * @internal_path: Path to file within archive
 * @buf: Output buffer (allocated by this function)
 * @len: Output length
 *
 * Read a file from within a zip archive into a buffer.
 *
 * Returns: true on success, false on failure.
 */
static bool overlay_zip_read_file(const char *zip_path,
      const char *internal_path, void **buf, int64_t *len)
{
   char full_path[PATH_MAX_LENGTH];

   if (string_is_empty(zip_path) || string_is_empty(internal_path))
      return false;

   /* Build archive#path format expected by file_archive_compressed_read */
   snprintf(full_path, sizeof(full_path), "%s#%s", zip_path, internal_path);

   return file_archive_compressed_read(full_path, buf, NULL, len) == 1;
}

/**
 * overlay_zip_find_cfg:
 * @zip_path: Path to zip archive
 * @cfg_path: Output buffer for config file path within archive
 * @cfg_path_size: Size of output buffer
 *
 * Find the first .cfg file within a zip archive.
 *
 * Returns: true if a .cfg file was found, false otherwise.
 */
static bool overlay_zip_find_cfg(const char *zip_path,
      char *cfg_path, size_t cfg_path_size)
{
   struct string_list *list;
   size_t i;
   bool found = false;

   if (string_is_empty(zip_path))
      return false;

   list = file_archive_get_file_list(zip_path, NULL);
   if (!list)
      return false;

   /* Find first .cfg file in archive */
   for (i = 0; i < list->size; i++)
   {
      const char *entry = list->elems[i].data;
      const char *ext   = path_get_extension(entry);

      if (ext && string_is_equal_noncase(ext, "cfg"))
      {
         strlcpy(cfg_path, entry, cfg_path_size);
         found = true;
         break;
      }
   }

   string_list_free(list);
   return found;
}

/**
 * overlay_zip_resolve_path:
 * @base_cfg_path: Path to .cfg file within archive (for relative resolution)
 * @relative_path: Relative path from .cfg file
 * @resolved: Output buffer for resolved path
 * @resolved_size: Size of output buffer
 *
 * Resolve a relative path within a zip archive.
 * Similar to fill_pathname_resolve_relative but for archive-internal paths.
 */
static void overlay_zip_resolve_path(const char *base_cfg_path,
      const char *relative_path, char *resolved, size_t resolved_size)
{
   char base_dir[PATH_MAX_LENGTH];

   if (string_is_empty(relative_path))
   {
      resolved[0] = '\0';
      return;
   }

   /* If relative_path starts with '/', treat as absolute within archive */
   if (relative_path[0] == '/')
   {
      strlcpy(resolved, relative_path + 1, resolved_size);
      return;
   }

   /* Get directory portion of base_cfg_path */
   strlcpy(base_dir, base_cfg_path, sizeof(base_dir));
   path_basedir(base_dir);

   /* Handle empty base dir (cfg at root of archive) */
   if (string_is_empty(base_dir) || string_is_equal(base_dir, "./"))
   {
      strlcpy(resolved, relative_path, resolved_size);
      return;
   }

   /* Concatenate base dir with relative path */
   fill_pathname_join(resolved, base_dir, relative_path, resolved_size);
}

/* Forward declaration for recursive include processing */
static char *overlay_zip_process_includes(const char *zip_path,
      const char *cfg_path, const char *content, unsigned depth);

/**
 * overlay_zip_load_config_recursive:
 * @zip_path: Path to zip archive
 * @cfg_path: Path to config file within archive
 * @depth: Current recursion depth for include processing
 *
 * Load a config file from within a zip archive and process any
 * #include directives by inlining the included content.
 *
 * Returns: Newly allocated string with config content, or NULL on failure.
 *          Caller must free the returned string.
 */
static char *overlay_zip_load_config(const char *zip_path,
      const char *cfg_path, unsigned depth)
{
   void *buf = NULL;
   int64_t len = 0;
   char *content;
   char *processed;

   if (depth > 16) /* MAX_INCLUDE_DEPTH */
   {
      RARCH_ERR("[Overlay] Include depth exceeded in zip overlay.\n");
      return NULL;
   }

   if (!overlay_zip_read_file(zip_path, cfg_path, &buf, &len))
      return NULL;

   if (!buf || len <= 0)
   {
      free(buf);
      return NULL;
   }

   /* Ensure null termination */
   content = (char*)malloc(len + 1);
   if (!content)
   {
      free(buf);
      return NULL;
   }
   memcpy(content, buf, len);
   content[len] = '\0';
   free(buf);

   /* Process #include directives */
   processed = overlay_zip_process_includes(zip_path, cfg_path, content, depth);
   free(content);

   return processed;
}

/**
 * overlay_zip_process_includes:
 * @zip_path: Path to zip archive
 * @cfg_path: Path to current config file within archive
 * @content: Config file content to process
 * @depth: Current recursion depth
 *
 * Process #include directives in config content by loading and inlining
 * the included files from within the zip archive.
 *
 * Returns: Newly allocated string with includes inlined, or NULL on failure.
 *          Caller must free the returned string.
 */
static char *overlay_zip_process_includes(const char *zip_path,
      const char *cfg_path, const char *content, unsigned depth)
{
   char *result;
   char *result_new;
   size_t result_size;
   size_t result_len;
   const char *line_start;
   const char *line_end;

   if (!content)
      return NULL;

   /* Start with buffer twice the size of content to allow for expansion */
   result_size = strlen(content) * 2 + 1;
   result = (char*)malloc(result_size);
   if (!result)
      return NULL;
   result[0] = '\0';
   result_len = 0;

   line_start = content;

   while (*line_start)
   {
      const char *include_start;
      size_t line_len;

      /* Find end of line */
      line_end = strchr(line_start, '\n');
      if (!line_end)
         line_end = line_start + strlen(line_start);

      line_len = line_end - line_start;

      /* Check for #include directive */
      include_start = strstr(line_start, "#include");
      if (include_start && include_start < line_end)
      {
         const char *quote_start;
         const char *quote_end;
         char include_path[PATH_MAX_LENGTH];
         char resolved_path[PATH_MAX_LENGTH];
         char *included_content;

         /* Find the quoted path: #include "path" */
         quote_start = strchr(include_start, '"');
         if (quote_start && quote_start < line_end)
         {
            quote_end = strchr(quote_start + 1, '"');
            if (quote_end && quote_end <= line_end)
            {
               size_t path_len = quote_end - quote_start - 1;
               if (path_len < sizeof(include_path))
               {
                  memcpy(include_path, quote_start + 1, path_len);
                  include_path[path_len] = '\0';

                  /* Resolve the include path relative to current cfg */
                  overlay_zip_resolve_path(cfg_path, include_path,
                        resolved_path, sizeof(resolved_path));

                  /* Recursively load the included file */
                  included_content = overlay_zip_load_config(
                        zip_path, resolved_path, depth + 1);

                  if (included_content)
                  {
                     size_t inc_len = strlen(included_content);

                     /* Ensure we have space */
                     while (result_len + inc_len + 2 >= result_size)
                     {
                        result_size *= 2;
                        result_new = (char*)realloc(result, result_size);
                        if (!result_new)
                        {
                           free(result);
                           free(included_content);
                           return NULL;
                        }
                        result = result_new;
                     }

                     /* Append included content */
                     memcpy(result + result_len, included_content, inc_len);
                     result_len += inc_len;
                     if (result[result_len - 1] != '\n')
                        result[result_len++] = '\n';
                     result[result_len] = '\0';

                     free(included_content);

                     /* Skip to next line */
                     line_start = (*line_end) ? line_end + 1 : line_end;
                     continue;
                  }
               }
            }
         }
      }

      /* Not an include line, copy it as-is */
      while (result_len + line_len + 2 >= result_size)
      {
         result_size *= 2;
         result_new = (char*)realloc(result, result_size);
         if (!result_new)
         {
            free(result);
            return NULL;
         }
         result = result_new;
      }

      memcpy(result + result_len, line_start, line_len);
      result_len += line_len;
      result[result_len++] = '\n';
      result[result_len] = '\0';

      /* Move to next line */
      line_start = (*line_end) ? line_end + 1 : line_end;
   }

   return result;
}

/**
 * overlay_zip_load_image:
 * @loader: Overlay loader context
 * @image_path: Relative path to image within archive
 * @img: Output texture_image structure
 *
 * Load an image from within a zip archive.
 *
 * Returns: true on success, false on failure.
 */
static bool overlay_zip_load_image(overlay_loader_t *loader,
      const char *image_path, struct texture_image *img)
{
   void *buf = NULL;
   int64_t len = 0;
   char resolved_path[PATH_MAX_LENGTH];
   enum image_type_enum img_type;
   bool result;

   if (!loader || string_is_empty(image_path))
      return false;

   /* Resolve path relative to config file location */
   overlay_zip_resolve_path(loader->zip_cfg_path, image_path,
         resolved_path, sizeof(resolved_path));

   /* Read image data from zip */
   if (!overlay_zip_read_file(loader->zip_path, resolved_path, &buf, &len))
   {
      RARCH_ERR("[Overlay] Failed to read image from zip: %s\n", resolved_path);
      return false;
   }

   if (!buf || len <= 0)
   {
      free(buf);
      return false;
   }

   /* Determine image type from path */
   img_type = image_texture_get_type(resolved_path);
   if (img_type == IMAGE_TYPE_NONE)
   {
      /* Try to detect from data or default to PNG */
      img_type = IMAGE_TYPE_PNG;
   }

   /* Load image from buffer */
   img->supports_rgba = (loader->flags & OVERLAY_LOADER_RGBA_SUPPORT) ? true : false;
   result = image_texture_load_buffer(img, img_type, buf, (size_t)len);

   free(buf);
   return result;
}

static void task_overlay_image_done(struct overlay *overlay)
{
   overlay->pos           = 0;
   /* Divide iteration steps by half of total descs if size is even,
    * otherwise default to 8 (arbitrary value for now to speed things up). */
   overlay->pos_increment = (overlay->size / 2) ? ((unsigned)(overlay->size / 2)) : 8;
}

static void task_overlay_load_desc_image(
      overlay_loader_t *loader,
      struct overlay_desc *desc,
      struct overlay *input_overlay,
      unsigned ol_idx, unsigned desc_idx)
{
   char overlay_desc_image_key[32];
   char image_path[PATH_MAX_LENGTH];
   config_file_t              *conf = loader->conf;

   overlay_desc_image_key[0]        = '\0';
   image_path[0]                    = '\0';

   snprintf(overlay_desc_image_key, sizeof(overlay_desc_image_key),
         "overlay%u_desc%u_overlay", ol_idx, desc_idx);

   if (config_get_path(conf, overlay_desc_image_key,
            image_path, sizeof(image_path)))
   {
      struct texture_image image_tex;
      bool loaded = false;

      memset(&image_tex, 0, sizeof(image_tex));

      if (loader->flags & OVERLAY_LOADER_FROM_ZIP)
      {
         /* Load image from zip archive */
         loaded = overlay_zip_load_image(loader, image_path, &image_tex);
      }
      else
      {
         /* Load image from filesystem */
         char path[PATH_MAX_LENGTH];
         fill_pathname_resolve_relative(path, loader->overlay_path,
               image_path, sizeof(path));

         image_tex.supports_rgba = (loader->flags & OVERLAY_LOADER_RGBA_SUPPORT) ? true : false;
         loaded = image_texture_load(&image_tex, path);
      }

      if (loaded)
      {
         input_overlay->load_images[input_overlay->load_images_size++] = image_tex;
         desc->image       = image_tex;
         desc->image_index = input_overlay->load_images_size - 1;
      }
   }

   input_overlay->pos ++;
}

static void task_overlay_redefine_eightway_direction(
      char *str, input_bits_t *data)
{
   unsigned bit;
   char *tok, *save = NULL;

   BIT256_CLEAR_ALL(*data);

   for (tok = strtok_r(str, "|", &save); tok;
         tok = strtok_r(NULL, "|", &save))
   {
      bit = input_config_translate_str_to_bind_id(tok);
      if (bit < RARCH_CUSTOM_BIND_LIST_END)
         BIT256_SET(*data, bit);
   }
}

static void task_overlay_desc_populate_eightway_config(
      overlay_loader_t *loader,
      struct overlay_desc *desc,
      unsigned ol_idx, unsigned desc_idx)
{
   size_t _len;
   input_driver_state_t *input_st = input_state_get_ptr();
   overlay_eightway_config_t *eightway;
   char conf_key[64];
   char *str;

   desc->eightway_config = (overlay_eightway_config_t *)
         calloc(1, sizeof(overlay_eightway_config_t));
   eightway              = desc->eightway_config;

   /* Populate default vals for the eightway type.
    */
   switch (desc->type)
   {
      case OVERLAY_TYPE_DPAD_AREA:
         BIT256_SET(eightway->up,    RETRO_DEVICE_ID_JOYPAD_UP);
         BIT256_SET(eightway->down,  RETRO_DEVICE_ID_JOYPAD_DOWN);
         BIT256_SET(eightway->left,  RETRO_DEVICE_ID_JOYPAD_LEFT);
         BIT256_SET(eightway->right, RETRO_DEVICE_ID_JOYPAD_RIGHT);

         eightway->slope_low  = &input_st->overlay_eightway_dpad_slopes[0];
         eightway->slope_high = &input_st->overlay_eightway_dpad_slopes[1];
         break;

      case OVERLAY_TYPE_ABXY_AREA:
         BIT256_SET(eightway->up,    RETRO_DEVICE_ID_JOYPAD_X);
         BIT256_SET(eightway->down,  RETRO_DEVICE_ID_JOYPAD_B);
         BIT256_SET(eightway->left,  RETRO_DEVICE_ID_JOYPAD_Y);
         BIT256_SET(eightway->right, RETRO_DEVICE_ID_JOYPAD_A);

         eightway->slope_low  = &input_st->overlay_eightway_abxy_slopes[0];
         eightway->slope_high = &input_st->overlay_eightway_abxy_slopes[1];
         break;

      default:
         free(eightway);
         desc->eightway_config = NULL;
         return;
   }

   _len = snprintf(conf_key, sizeof(conf_key), "overlay%u_desc%u", ol_idx, desc_idx);

   /* Redefine eightway vals if specified in conf
    */
   strlcpy(conf_key + _len, "_up", sizeof(conf_key) - _len);
   if (config_get_string(loader->conf, conf_key, &str))
   {
      task_overlay_redefine_eightway_direction(str, &eightway->up);
      free(str);
   }

   strlcpy(conf_key + _len, "_down", sizeof(conf_key) - _len);
   if (config_get_string(loader->conf, conf_key, &str))
   {
      task_overlay_redefine_eightway_direction(str, &eightway->down);
      free(str);
   }

   strlcpy(conf_key + _len, "_left", sizeof(conf_key) - _len);
   if (config_get_string(loader->conf, conf_key, &str))
   {
      task_overlay_redefine_eightway_direction(str, &eightway->left);
      free(str);
   }

   strlcpy(conf_key + _len, "_right", sizeof(conf_key) - _len);
   if (config_get_string(loader->conf, conf_key, &str))
   {
      task_overlay_redefine_eightway_direction(str, &eightway->right);
      free(str);
   }

   /* Prepopulate diagonals.
    */
   bits_or_bits(eightway->up_right.data, eightway->up.data,
         CUSTOM_BINDS_U32_COUNT);
   bits_or_bits(eightway->up_right.data, eightway->right.data,
         CUSTOM_BINDS_U32_COUNT);

   bits_or_bits(eightway->up_left.data, eightway->up.data,
         CUSTOM_BINDS_U32_COUNT);
   bits_or_bits(eightway->up_left.data, eightway->left.data,
         CUSTOM_BINDS_U32_COUNT);

   bits_or_bits(eightway->down_right.data, eightway->down.data,
         CUSTOM_BINDS_U32_COUNT);
   bits_or_bits(eightway->down_right.data, eightway->right.data,
         CUSTOM_BINDS_U32_COUNT);

   bits_or_bits(eightway->down_left.data, eightway->down.data,
         CUSTOM_BINDS_U32_COUNT);
   bits_or_bits(eightway->down_left.data, eightway->left.data,
         CUSTOM_BINDS_U32_COUNT);
}

static bool task_overlay_load_desc(
      overlay_loader_t *loader,
      struct overlay_desc *desc,
      struct overlay *input_overlay,
      unsigned ol_idx, unsigned desc_idx,
      unsigned width, unsigned height,
      bool normalized, float alpha_mod, float range_mod)
{
   size_t _len;
   float width_mod, height_mod;
   char conf_key[64];
   char overlay_desc_key[32];
   char overlay_key[64];
   char overlay[256];
   char *tok, *save                     = NULL;
   unsigned list_size                   = 0;
   char *elem0                          = NULL;
   char *elem1                          = NULL;
   char *elem2                          = NULL;
   char *elem3                          = NULL;
   char *elem4                          = NULL;
   char *elem5                          = NULL;
   char *overlay_cpy                    = NULL;
   float tmp_float                      = 0.0f;
   bool tmp_bool                        = false;
   bool ret                             = true;
   bool by_pixel                        = false;
   char *key                            = NULL;
   const char *x                        = NULL;
   const char *y                        = NULL;
   const char *box                      = NULL;
   config_file_t *conf                  = loader->conf;

   overlay_desc_key[0]                  =
      overlay_key[0]                    =
      conf_key[0]                       =
      overlay[0]                        = '\0';

   snprintf(overlay_desc_key, sizeof(overlay_desc_key),
         "overlay%u_desc%u", ol_idx, desc_idx);

   _len = strlcpy(overlay_key, overlay_desc_key, sizeof(overlay_key));
   strlcpy(overlay_key + _len, "_normalized", sizeof(overlay_key) - _len);
   if (config_get_bool(conf, overlay_key, &tmp_bool))
      normalized = tmp_bool;

   by_pixel = !normalized;

   if (by_pixel && (width == 0 || height == 0))
   {
      RARCH_ERR("[Overlay] Base overlay is not set and not using normalized coordinates.\n");
      ret = false;
      goto end;
   }

   if (!config_get_array(conf, overlay_desc_key, overlay, sizeof(overlay)))
   {
      RARCH_ERR("[Overlay] Didn't find key: %s.\n", overlay_desc_key);
      ret = false;
      goto end;
   }

   overlay_cpy = strdup(overlay);
   if ((tok = strtok_r(overlay_cpy, ", ", &save)))
   {
      elem0 = strdup(tok);
      list_size++;
   }
   if ((tok = strtok_r(NULL, ", ", &save)))
   {
      elem1 = strdup(tok);
      list_size++;
   }
   if ((tok = strtok_r(NULL, ", ", &save)))
   {
      elem2 = strdup(tok);
      list_size++;
   }
   if ((tok = strtok_r(NULL, ", ", &save))) /* box */
   {
      elem3 = strdup(tok);
      list_size++;
   }
   if ((tok = strtok_r(NULL, ", ", &save)))
   {
      elem4 = strdup(tok);
      list_size++;
   }
   if ((tok = strtok_r(NULL, ", ", &save)))
   {
      elem5 = strdup(tok);
      list_size++;
   }
   free(overlay_cpy);

   if (list_size < 6)
   {
      RARCH_ERR("[Overlay] Overlay desc is invalid. Requires at least 6 tokens.\n");
      ret = false;
      goto end;
   }

   key                 = elem0;
   x                   = elem1;
   y                   = elem2;
   box                 = elem3;

   desc->retro_key_idx = 0;
   BIT256_CLEAR_ALL(desc->button_mask);

   if (string_is_equal(key, "analog_left"))
      desc->type          = OVERLAY_TYPE_ANALOG_LEFT;
   else if (string_is_equal(key, "analog_right"))
      desc->type          = OVERLAY_TYPE_ANALOG_RIGHT;
   else if (string_is_equal(key, "dpad_area"))
      desc->type          = OVERLAY_TYPE_DPAD_AREA;
   else if (string_is_equal(key, "abxy_area"))
      desc->type          = OVERLAY_TYPE_ABXY_AREA;
   else if (strstr(key, "retrok_") == key)
   {
      desc->type          = OVERLAY_TYPE_KEYBOARD;
      desc->retro_key_idx = input_config_translate_str_to_rk(key + 7, strlen(key + 7));
   }
   else
   {
      char      *save = NULL;
      const char *tmp = strtok_r(key, "|", &save);

      desc->type = OVERLAY_TYPE_BUTTONS;

      for (; tmp; tmp = strtok_r(NULL, "|", &save))
      {
         if (!string_is_equal(tmp, "nul"))
            BIT256_SET(desc->button_mask, input_config_translate_str_to_bind_id(tmp));
      }

      if (BIT256_GET(desc->button_mask, RARCH_OVERLAY_NEXT))
      {
         strlcpy(overlay_key + _len, "_next_target",
               sizeof(overlay_key) - _len);
         config_get_array(conf, overlay_key,
               desc->next_index_name, sizeof(desc->next_index_name));
      }
      else if (BIT256_GET(desc->button_mask, RARCH_OSK))
         BIT16_SET(loader->overlay_types, OVERLAY_TYPE_OSK_TOGGLE);
   }

   BIT16_SET(loader->overlay_types, desc->type);

   width_mod  = 1.0f;
   height_mod = 1.0f;

   if (by_pixel)
   {
      width_mod  /= width;
      height_mod /= height;
   }

   desc->x       = (float)strtod(x, NULL) * width_mod;
   desc->y       = (float)strtod(y, NULL) * height_mod;
   desc->x_shift = desc->x;
   desc->y_shift = desc->y;

   if (string_is_equal(box, "radial"))
      desc->hitbox = OVERLAY_HITBOX_RADIAL;
   else if (string_is_equal(box, "rect"))
      desc->hitbox = OVERLAY_HITBOX_RECT;
   else
   {
      RARCH_ERR("[Overlay] Hitbox type (%s) is invalid. Use \"radial\" or \"rect\".\n", box);
      ret = false;
      goto end;
   }

   switch (desc->type)
   {
      case OVERLAY_TYPE_ANALOG_LEFT:
      case OVERLAY_TYPE_ANALOG_RIGHT:
         if (desc->hitbox != OVERLAY_HITBOX_RADIAL)
         {
            RARCH_ERR("[Overlay] Analog hitbox type must be \"radial\".\n");
            ret = false;
            goto end;
         }

         strlcpy(overlay_key + _len, "_saturate_pct",
               sizeof(overlay_key) - _len);
         if (config_get_float(conf, overlay_key,
                  &tmp_float))
            desc->analog_saturate_pct = tmp_float;
         else
            desc->analog_saturate_pct = 1.0f;
         break;
      case OVERLAY_TYPE_DPAD_AREA:
      case OVERLAY_TYPE_ABXY_AREA:
         task_overlay_desc_populate_eightway_config(
               loader, desc, ol_idx, desc_idx);
         break;
      default:
         /* OVERLAY_TYPE_BUTTONS  - unhandled */
         /* OVERLAY_TYPE_KEYBOARD - unhandled */
         break;
   }

   desc->range_x = (float)strtod(elem4, NULL) * width_mod;
   desc->range_y = (float)strtod(elem5, NULL) * height_mod;

   _len = strlcpy(conf_key, overlay_desc_key, sizeof(conf_key));

   strlcpy(conf_key + _len, "_reach_x",   sizeof(conf_key) - _len);
   desc->reach_right = 1.0f;
   desc->reach_left  = 1.0f;
   if (config_get_float(conf, conf_key, &tmp_float))
   {
      desc->reach_right = tmp_float;
      desc->reach_left  = tmp_float;
   }

   strlcpy(conf_key + _len, "_reach_y",   sizeof(conf_key) - _len);
   desc->reach_up   = 1.0f;
   desc->reach_down = 1.0f;
   if (config_get_float(conf, conf_key, &tmp_float))
   {
      desc->reach_up   = tmp_float;
      desc->reach_down = tmp_float;
   }

   strlcpy(conf_key + _len, "_movable",   sizeof(conf_key) - _len);
   desc->flags    &= ~OVERLAY_DESC_MOVABLE;
   desc->delta_x   = 0.0f;
   desc->delta_y   = 0.0f;
   if (config_get_bool(conf, conf_key, &tmp_bool)
         && tmp_bool)
      desc->flags |= OVERLAY_DESC_MOVABLE;

   strlcpy(conf_key + _len, "_reach_up", sizeof(conf_key) - _len);
   if (config_get_float(conf, conf_key, &tmp_float))
      desc->reach_up = tmp_float;

   strlcpy(conf_key + _len, "_alpha_mod",   sizeof(conf_key) - _len);
   desc->alpha_mod = alpha_mod;
   if (config_get_float(conf, conf_key, &tmp_float))
         desc->alpha_mod = tmp_float;

   strlcpy(conf_key + _len, "_range_mod",   sizeof(conf_key) - _len);
   desc->range_mod = range_mod;
   if (config_get_float(conf, conf_key, &tmp_float))
      desc->range_mod = tmp_float;

   strlcpy(conf_key + _len, "_exclusive",   sizeof(conf_key) - _len);
   desc->flags &= ~OVERLAY_DESC_EXCLUSIVE;
   if (config_get_bool(conf, conf_key, &tmp_bool)
         && tmp_bool)
      desc->flags |= OVERLAY_DESC_EXCLUSIVE;

   strlcpy(conf_key + _len, "_reach_down",   sizeof(conf_key) - _len);
   if (config_get_float(conf, conf_key, &tmp_float))
      desc->reach_down = tmp_float;

   strlcpy(conf_key + _len, "_reach_left",   sizeof(conf_key) - _len);
   if (config_get_float(conf, conf_key, &tmp_float))
      desc->reach_left = tmp_float;

   strlcpy(conf_key + _len, "_reach_right",   sizeof(conf_key) - _len);
   if (config_get_float(conf, conf_key, &tmp_float))
      desc->reach_right = tmp_float;

   strlcpy(conf_key + _len, "_range_mod_exclusive", sizeof(conf_key) - _len);
   desc->flags &= ~OVERLAY_DESC_RANGE_MOD_EXCLUSIVE;
   if (config_get_bool(conf, conf_key, &tmp_bool)
         && tmp_bool)
      desc->flags |= OVERLAY_DESC_RANGE_MOD_EXCLUSIVE;

   if (     (desc->reach_left == 0.0f && desc->reach_right == 0.0f)
         || (desc->reach_up   == 0.0f && desc->reach_down  == 0.0f))
      desc->hitbox = OVERLAY_HITBOX_NONE;

   desc->mod_x   = desc->x - desc->range_x;
   desc->mod_w   = 2.0f * desc->range_x;
   desc->mod_y   = desc->y - desc->range_y;
   desc->mod_h   = 2.0f * desc->range_y;

   input_overlay->pos ++;

end:
   if (elem0)
      free(elem0);
   if (elem1)
      free(elem1);
   if (elem2)
      free(elem2);
   if (elem3)
      free(elem3);
   if (elem4)
      free(elem4);
   if (elem5)
      free(elem5);
   return ret;
}

static ssize_t task_overlay_find_index(const struct overlay *ol,
      const char *name, size_t len)
{
   size_t i;
   if (!ol)
      return -1;
   for (i = 0; i < len; i++)
   {
      if (string_is_equal(ol[i].name, name))
         return i;
   }
   return -1;
}

static bool task_overlay_resolve_targets(struct overlay *ol,
      size_t idx, size_t len)
{
   unsigned i;
   struct overlay *current = (struct overlay*)&ol[idx];

   for (i = 0; i < current->size; i++)
   {
      struct overlay_desc *desc = (struct overlay_desc*)&current->descs[i];
      const char *next          = desc->next_index_name;
      ssize_t         next_idx  = (idx + 1) % len;

      if (!string_is_empty(next))
      {
         next_idx = task_overlay_find_index(ol, next, len);

         if (next_idx < 0)
         {
            RARCH_ERR("[Overlay] Couldn't find overlay called: \"%s\".\n",
                  next);
            return false;
         }
      }

      desc->next_index = (unsigned)next_idx;
   }

   return true;
}

static void task_overlay_resolve_iterate(retro_task_t *task)
{
   overlay_loader_t *loader  = (overlay_loader_t*)task->state;
   bool             not_done = loader->resolve_pos < loader->size;

   if (!not_done)
   {
      loader->state = OVERLAY_STATUS_DEFERRED_DONE;
      return;
   }

   if (!task_overlay_resolve_targets(loader->overlays,
            loader->resolve_pos, loader->size))
   {
      RARCH_ERR("[Overlay] Failed to resolve next targets.\n");
      task_set_flags(task, RETRO_TASK_FLG_CANCELLED, true);
      loader->state   = OVERLAY_STATUS_DEFERRED_ERROR;
      return;
   }

   if (loader->resolve_pos == 0)
      loader->active = &loader->overlays[0];

   loader->resolve_pos += 1;
}

static void task_overlay_deferred_loading(retro_task_t *task)
{
   size_t i                  = 0;
   overlay_loader_t *loader  = (overlay_loader_t*)task->state;
   struct overlay *overlay   = &loader->overlays[loader->pos];
   bool not_done             = loader->pos < loader->size;

   if (!not_done)
   {
      loader->state = OVERLAY_STATUS_DEFERRED_LOADING_RESOLVE;
      return;
   }

   switch (loader->loading_status)
   {
      case OVERLAY_IMAGE_TRANSFER_NONE:
      case OVERLAY_IMAGE_TRANSFER_BUSY:
         loader->loading_status = OVERLAY_IMAGE_TRANSFER_DONE;
	 /* fall-through */
      case OVERLAY_IMAGE_TRANSFER_DONE:
         task_overlay_image_done(&loader->overlays[loader->pos]);
         loader->loading_status = OVERLAY_IMAGE_TRANSFER_DESC_IMAGE_ITERATE;
         loader->overlays[loader->pos].pos = 0;
         break;
      case OVERLAY_IMAGE_TRANSFER_DESC_IMAGE_ITERATE:
         for (i = 0; i < overlay->pos_increment; i++)
         {
            if (overlay->pos < overlay->size)
            {
               task_overlay_load_desc_image(loader,
                     &overlay->descs[overlay->pos], overlay,
                     loader->pos, (unsigned)overlay->pos);
            }
            else
            {
               overlay->pos       = 0;
               loader->loading_status = OVERLAY_IMAGE_TRANSFER_DESC_ITERATE;
               break;
            }

         }
         break;
      case OVERLAY_IMAGE_TRANSFER_DESC_ITERATE:
         for (i = 0; i < overlay->pos_increment; i++)
         {
            if (overlay->pos < overlay->size)
            {
               if (!task_overlay_load_desc(loader,
                        &overlay->descs[overlay->pos], overlay,
                        loader->pos, (unsigned)overlay->pos,
                        overlay->image.width, overlay->image.height,
                        overlay->config.normalized,
                        overlay->config.alpha_mod, overlay->config.range_mod))
               {
                  RARCH_ERR("[Overlay] Failed to load overlay descs for overlay #%u.\n",
                        (unsigned)overlay->pos);
                  task_set_flags(task, RETRO_TASK_FLG_CANCELLED, true);
                  loader->state   = OVERLAY_STATUS_DEFERRED_ERROR;
                  break;
               }
            }
            else
            {
               overlay->pos       = 0;
               loader->loading_status = OVERLAY_IMAGE_TRANSFER_DESC_DONE;
               break;
            }
         }
         break;
      case OVERLAY_IMAGE_TRANSFER_DESC_DONE:
         if (loader->pos == 0)
            task_overlay_resolve_iterate(task);

         loader->pos += 1;
         loader->loading_status = OVERLAY_IMAGE_TRANSFER_NONE;
         break;
      case OVERLAY_IMAGE_TRANSFER_ERROR:
         task_set_flags(task, RETRO_TASK_FLG_CANCELLED, true);
         loader->state   = OVERLAY_STATUS_DEFERRED_ERROR;
         break;
   }
}

static void task_overlay_deferred_load(retro_task_t *task)
{
   unsigned i;
   overlay_loader_t *loader  = (overlay_loader_t*)task->state;
   config_file_t       *conf = loader->conf;

   for (i = 0; i < loader->pos_increment; i++, loader->pos++)
   {
      size_t _len;
      char conf_key[32];
      char tmp_str[PATH_MAX_LENGTH];
      float tmp_float                   = 0.0;
      bool tmp_bool                     = false;
      struct texture_image *texture_img = NULL;
      struct overlay_desc *overlay_desc = NULL;
      struct overlay          *overlay  = NULL;
      bool                     to_cont  = loader->pos < loader->size;

      if (!to_cont)
      {
         loader->pos   = 0;
         loader->state = OVERLAY_STATUS_DEFERRED_LOADING;
         break;
      }

      tmp_str[0] = '\0';

      overlay = &loader->overlays[loader->pos];

      _len = snprintf(conf_key, sizeof(conf_key), "overlay%u", loader->pos);

      strlcpy(conf_key + _len, "_rect", sizeof(conf_key) - _len);
      strlcpy(overlay->config.rect.key, conf_key,
            sizeof(overlay->config.rect.key));

      strlcpy(conf_key + _len, "_name", sizeof(conf_key) - _len);
      strlcpy(overlay->config.names.key, conf_key,
            sizeof(overlay->config.names.key));

      strlcpy(conf_key + _len, "_descs", sizeof(conf_key) - _len);
      strlcpy(overlay->config.descs.key, conf_key,
            sizeof(overlay->config.descs.key));

      strlcpy(conf_key + _len, "_overlay", sizeof(conf_key) - _len);
      strlcpy(overlay->config.paths.key, conf_key,
            sizeof(overlay->config.paths.key));

      if (!config_get_uint(conf, overlay->config.descs.key,
               &overlay->config.descs.size))
      {
         RARCH_ERR("[Overlay] Failed to read number of descs from config key: %s.\n",
               overlay->config.descs.key);
         goto error;
      }

      overlay_desc = (struct overlay_desc*)
         calloc(overlay->config.descs.size, sizeof(*overlay->descs));

      if (!overlay_desc)
      {
         RARCH_ERR("[Overlay] Failed to allocate descs.\n");
         goto error;
      }

      overlay->descs = overlay_desc;
      overlay->size  = overlay->config.descs.size;

      strlcpy(conf_key + _len, "_alpha_mod", sizeof(conf_key) - _len);
      if (config_get_float(conf, conf_key, &tmp_float))
         overlay->config.alpha_mod = tmp_float;
      else
         overlay->config.alpha_mod = 1.0f;

      strlcpy(conf_key + _len, "_range_mod", sizeof(conf_key) - _len);
      if (config_get_float(conf, conf_key, &tmp_float))
         overlay->config.range_mod = tmp_float;
      else
         overlay->config.range_mod = 1.0f;

      strlcpy(conf_key + _len, "_normalized", sizeof(conf_key) - _len);
      if (config_get_bool(conf, conf_key, &tmp_bool)
            && tmp_bool)
         overlay->config.normalized = tmp_bool;
      else
         overlay->config.normalized = false;

      strlcpy(conf_key + _len, "_full_screen", sizeof(conf_key) - _len);
      if (config_get_bool(conf, conf_key, &tmp_bool)
            && tmp_bool)
         overlay->flags |=  OVERLAY_FULL_SCREEN;
      else
         overlay->flags &= ~OVERLAY_FULL_SCREEN;

      /* Precache load image array for simplicity. */
      texture_img = (struct texture_image*)
         calloc(1 + overlay->size, sizeof(struct texture_image));

      if (!texture_img)
      {
         RARCH_ERR("[Overlay] Failed to allocate load_images.\n");
         goto error;
      }

      overlay->load_images = texture_img;

      if (config_get_path(conf, overlay->config.paths.key,
               tmp_str, sizeof(tmp_str)))
         strlcpy(overlay->config.paths.path,
               tmp_str, sizeof(overlay->config.paths.path));

      if (!string_is_empty(overlay->config.paths.path))
      {
         struct texture_image image_tex;

         memset(&image_tex, 0, sizeof(image_tex));

         if (loader->flags & OVERLAY_LOADER_FROM_ZIP)
         {
            /* Load image from zip archive */
            if (!overlay_zip_load_image(loader,
                     overlay->config.paths.path, &image_tex))
            {
               RARCH_ERR("[Overlay] Failed to load image from zip: \"%s\".\n",
                     overlay->config.paths.path);
               loader->loading_status = OVERLAY_IMAGE_TRANSFER_ERROR;
               goto error;
            }
         }
         else
         {
            /* Load image from filesystem */
            char overlay_resolved_path[PATH_MAX_LENGTH];

            overlay_resolved_path[0] = '\0';

            fill_pathname_resolve_relative(overlay_resolved_path,
                  loader->overlay_path,
                  overlay->config.paths.path, sizeof(overlay_resolved_path));

            image_tex.supports_rgba =
                  (loader->flags & OVERLAY_LOADER_RGBA_SUPPORT) ? true : false;

            if (!image_texture_load(&image_tex, overlay_resolved_path))
            {
               RARCH_ERR("[Overlay] Failed to load image: \"%s\".\n",
                     overlay_resolved_path);
               loader->loading_status = OVERLAY_IMAGE_TRANSFER_ERROR;
               goto error;
            }
         }

         overlay->load_images[overlay->load_images_size++] = image_tex;
         overlay->image = image_tex;
      }

      config_get_array(conf, overlay->config.names.key,
            overlay->name, sizeof(overlay->name));

      /* Attempt to determine native aspect ratio */
      strlcpy(conf_key + _len, "_aspect_ratio", sizeof(conf_key) - _len);
      if (config_get_float(conf, conf_key, &tmp_float))
         overlay->aspect_ratio = tmp_float;
      else
         overlay->aspect_ratio = 0.0f;

      if (overlay->aspect_ratio <= 0.0f)
      {
         /* No ratio has been set - assume 16:9
          * (or 16:9 rotated) */

         /* Check whether overlay name indicates a
          * portrait layout */
         if (strstr(overlay->name, "portrait"))
            overlay->aspect_ratio = 0.5625f;    /* 1 / (16/9) */
         else
            overlay->aspect_ratio = 1.7777778f; /* 16/9 */
      }

      /* By default, we stretch the overlay out in full. */
      overlay->x = overlay->y = 0.0f;
      overlay->w = overlay->h = 1.0f;

      if (config_get_array(conf, overlay->config.rect.key,
               overlay->config.rect.array, sizeof(overlay->config.rect.array)))
      {
         char *tok, *save         = NULL;
         char *elem0              = NULL;
         char *elem1              = NULL;
         char *elem2              = NULL;
         char *elem3              = NULL;
         unsigned list_size       = 0;
         char *cfg_rect_array_cpy = strdup(overlay->config.rect.array);

         if ((tok = strtok_r(cfg_rect_array_cpy, ", ", &save)))
         {
            elem0 = strdup(tok);
            list_size++;
         }
         if ((tok = strtok_r(NULL, ", ", &save)))
         {
            elem1 = strdup(tok);
            list_size++;
         }
         if ((tok = strtok_r(NULL, ", ", &save)))
         {
            elem2 = strdup(tok);
            list_size++;
         }
         if ((tok = strtok_r(NULL, ", ", &save)))
         {
            elem3 = strdup(tok);
            list_size++;
         }
         free(cfg_rect_array_cpy);

         if (list_size < 4)
         {
            RARCH_ERR("[Overlay] Failed to split rect \"%s\" into at least four tokens.\n",
                  overlay->config.rect.array);
            free(elem0);
            free(elem1);
            free(elem2);
            free(elem3);
            goto error;
         }

         overlay->x = (float)strtod(elem0, NULL);
         overlay->y = (float)strtod(elem1, NULL);
         overlay->w = (float)strtod(elem2, NULL);
         overlay->h = (float)strtod(elem3, NULL);
         free(elem0);
         free(elem1);
         free(elem2);
         free(elem3);
      }

      /* Assume for now that scaling center is in the middle.
       * TODO: Make this configurable. */
      overlay->flags      &= ~OVERLAY_BLOCK_SCALE;
      overlay->center_x    = overlay->x + 0.5f * overlay->w;
      overlay->center_y    = overlay->y + 0.5f * overlay->h;

      /* Parse viewport override (optional) */
      strlcpy(conf_key + _len, "_viewport", sizeof(conf_key) - _len);
      RARCH_LOG("[Overlay] Checking for viewport key: %s\n", conf_key);
      if (config_get_array(conf, conf_key, tmp_str, sizeof(tmp_str)))
      {
         char *tok, *save      = NULL;
         char *elem0           = NULL;
         char *elem1           = NULL;
         char *elem2           = NULL;
         char *elem3           = NULL;
         unsigned list_size    = 0;
         char *cfg_vp_cpy      = strdup(tmp_str);
         RARCH_LOG("[Overlay] Found viewport value: %s\n", tmp_str);

         if ((tok = strtok_r(cfg_vp_cpy, ", ", &save)))
         {
            elem0 = strdup(tok);
            list_size++;
         }
         if ((tok = strtok_r(NULL, ", ", &save)))
         {
            elem1 = strdup(tok);
            list_size++;
         }
         if ((tok = strtok_r(NULL, ", ", &save)))
         {
            elem2 = strdup(tok);
            list_size++;
         }
         if ((tok = strtok_r(NULL, ", ", &save)))
         {
            elem3 = strdup(tok);
            list_size++;
         }
         free(cfg_vp_cpy);

         if (list_size >= 4)
         {
            overlay->viewport.x  = (float)strtod(elem0, NULL);
            overlay->viewport.y  = (float)strtod(elem1, NULL);
            overlay->viewport.w  = (float)strtod(elem2, NULL);
            overlay->viewport.h  = (float)strtod(elem3, NULL);
            overlay->flags      |= OVERLAY_HAS_VIEWPORT;
            RARCH_LOG("[Overlay] Parsed viewport: x=%.3f y=%.3f w=%.3f h=%.3f\n",
                  overlay->viewport.x, overlay->viewport.y,
                  overlay->viewport.w, overlay->viewport.h);
         }
         else
            RARCH_WARN("[Overlay] viewport \"%s\" requires four tokens.\n", tmp_str);

         free(elem0);
         free(elem1);
         free(elem2);
         free(elem3);
      }

      /* Parse viewport_fill option (optional, default false) */
      strlcpy(conf_key + _len, "_viewport_fill", sizeof(conf_key) - _len);
      if (config_get_bool(conf, conf_key, &tmp_bool) && tmp_bool)
         overlay->flags |= OVERLAY_VIEWPORT_FILL;
      else
         overlay->flags &= ~OVERLAY_VIEWPORT_FILL;

      /* Check whether x/y separation are force disabled
       * for this overlay */
      strlcpy(conf_key + _len, "_block_x_separation", sizeof(conf_key) - _len);
      if (config_get_bool(conf, conf_key, &tmp_bool)
            && tmp_bool)
         overlay->flags |=  OVERLAY_BLOCK_X_SEPARATION;
      else
         overlay->flags &= ~OVERLAY_BLOCK_X_SEPARATION;

      strlcpy(conf_key + _len, "_block_y_separation", sizeof(conf_key) - _len);
      if (config_get_bool(conf, conf_key, &tmp_bool)
            && tmp_bool)
         overlay->flags |=  OVERLAY_BLOCK_Y_SEPARATION;
      else
         overlay->flags &= ~OVERLAY_BLOCK_Y_SEPARATION;

      /* Check whether x/y separation are enabled
       * for this overlay in auto-scale mode */
      strlcpy(conf_key + _len, "_auto_x_separation", sizeof(conf_key) - _len);
      overlay->flags    |=  OVERLAY_AUTO_X_SEPARATION;
      if (config_get_bool(conf, conf_key, &tmp_bool))
      {
         if (!tmp_bool)
            overlay->flags &= ~OVERLAY_AUTO_X_SEPARATION;
      }
      else
      {
         if (overlay->flags & OVERLAY_BLOCK_X_SEPARATION
               || overlay->image.width != 0)
            overlay->flags &= ~OVERLAY_AUTO_X_SEPARATION;
      }

      strlcpy(conf_key + _len, "_auto_y_separation", sizeof(conf_key) - _len);
      if (config_get_bool(conf, conf_key, &tmp_bool)
            && tmp_bool)
         overlay->flags |=  OVERLAY_AUTO_Y_SEPARATION;
      else
         overlay->flags &= ~OVERLAY_AUTO_Y_SEPARATION;
   }

   return;

error:
   if (task)
      task_set_flags(task, RETRO_TASK_FLG_CANCELLED, true);
   loader->pos     = 0;
   loader->state   = OVERLAY_STATUS_DEFERRED_ERROR;
}

static void task_overlay_free(retro_task_t *task)
{
   unsigned i;
   overlay_loader_t *loader  = (overlay_loader_t*)task->state;
   struct overlay *overlay   = &loader->overlays[loader->pos];
   uint8_t flg               = task_get_flags(task);

   if ((flg & RETRO_TASK_FLG_CANCELLED) > 0)
   {
      if (loader->overlay_path)
         free(loader->overlay_path);

      for (i = 0; i < overlay->load_images_size; i++)
      {
         struct texture_image *ti = &overlay->load_images[i];
         image_texture_free(ti);
      }

      for (i = 0; i < loader->size; i++)
         input_overlay_free_overlay(&loader->overlays[i]);

      free(loader->overlays);
   }

   if (loader->conf)
      config_file_free(loader->conf);

   if (loader->zip_path)
      free(loader->zip_path);

   if (loader->zip_cfg_path)
      free(loader->zip_cfg_path);

   free(loader);
}

static void task_overlay_handler(retro_task_t *task)
{
   uint8_t flg;
   overlay_loader_t *loader  = (overlay_loader_t*)task->state;

   switch (loader->state)
   {
      case OVERLAY_STATUS_DEFERRED_LOADING:
         task_overlay_deferred_loading(task);
         break;
      case OVERLAY_STATUS_DEFERRED_LOAD:
         task_overlay_deferred_load(task);
         break;
      case OVERLAY_STATUS_DEFERRED_LOADING_RESOLVE:
         task_overlay_resolve_iterate(task);
         break;
      case OVERLAY_STATUS_DEFERRED_ERROR:
         task_set_flags(task, RETRO_TASK_FLG_CANCELLED, true);
         break;
      case OVERLAY_STATUS_DEFERRED_DONE:
      default:
      case OVERLAY_STATUS_NONE:
         task_set_flags(task, RETRO_TASK_FLG_FINISHED, true);
         break;
   }

   flg = task_get_flags(task);

   if (       ((flg & RETRO_TASK_FLG_FINISHED)  > 0)
         && (!((flg & RETRO_TASK_FLG_CANCELLED) > 0)))
   {
      overlay_task_data_t *data = (overlay_task_data_t*)
         calloc(1, sizeof(*data));

      data->overlays                    = loader->overlays;
      data->active                      = loader->active;
      data->size                        = loader->size;
      data->flags                       = loader->flags;
      data->overlay_types               = loader->overlay_types;
      data->overlay_path                = loader->overlay_path;

      task_set_data(task, data);
   }
}

static bool task_overlay_finder(retro_task_t *task, void *user_data)
{
   overlay_loader_t *loader = NULL;
   if (!task || (task->handler != task_overlay_handler) || !user_data)
      return false;
   if (!(loader = (overlay_loader_t*)task->state))
      return false;
   return string_is_equal(loader->overlay_path, (const char*)user_data);
}

bool task_push_overlay_load_default(
      retro_task_callback_t cb,
      const char *overlay_path,
      bool is_osk,
      void *user_data)
{
   task_finder_data_t find_data;
   retro_task_t *t          = NULL;
   config_file_t *conf      = NULL;
   overlay_loader_t *loader = NULL;
   bool is_zip              = false;

   if (string_is_empty(overlay_path))
      return false;

   /* Prevent overlay from being loaded if it already is being loaded */
   find_data.func           = task_overlay_finder;
   find_data.userdata       = (void*)overlay_path;

   if (task_queue_find(&find_data))
      return false;

   loader                   = (overlay_loader_t*)calloc(1, sizeof(*loader));

   if (!loader)
      return false;

   /* Check if this is a zip archive (by magic bytes, not extension) */
   is_zip = overlay_file_is_zip(overlay_path);

   if (is_zip)
   {
      char cfg_path[PATH_MAX_LENGTH];
      char *config_str = NULL;

      /* Find the .cfg file within the zip */
      if (!overlay_zip_find_cfg(overlay_path, cfg_path, sizeof(cfg_path)))
      {
         RARCH_ERR("[Overlay] No .cfg file found in zip archive: %s\n",
               overlay_path);
         free(loader);
         return false;
      }

      RARCH_LOG("[Overlay] Loading overlay from zip: %s (config: %s)\n",
            overlay_path, cfg_path);

      /* Load config with #include processing */
      config_str = overlay_zip_load_config(overlay_path, cfg_path, 0);
      if (!config_str)
      {
         RARCH_ERR("[Overlay] Failed to load config from zip: %s#%s\n",
               overlay_path, cfg_path);
         free(loader);
         return false;
      }

      /* Parse the config string */
      conf = config_file_new_from_string(config_str, overlay_path);
      free(config_str);

      if (!conf)
      {
         RARCH_ERR("[Overlay] Failed to parse config from zip: %s\n",
               overlay_path);
         free(loader);
         return false;
      }

      /* Store zip-specific info */
      loader->zip_path     = strdup(overlay_path);
      loader->zip_cfg_path = strdup(cfg_path);
      loader->flags       |= OVERLAY_LOADER_FROM_ZIP;
   }
   else
   {
      /* Standard filesystem loading */
      if (!(conf = config_file_new_from_path_to_string(overlay_path)))
      {
         free(loader);
         return false;
      }
   }

   if (!config_get_uint(conf, "overlays", &loader->size))
   {
      /* Error - overlays variable not defined in config. */
      config_file_free(conf);
      if (loader->zip_path)
         free(loader->zip_path);
      if (loader->zip_cfg_path)
         free(loader->zip_cfg_path);
      free(loader);
      return false;
   }

   loader->overlays         = (struct overlay*)
      calloc(loader->size, sizeof(*loader->overlays));

   if (!loader->overlays)
   {
      config_file_free(conf);
      if (loader->zip_path)
         free(loader->zip_path);
      if (loader->zip_cfg_path)
         free(loader->zip_cfg_path);
      free(loader);
      return false;
   }

   loader->conf             = conf;
   loader->state            = OVERLAY_STATUS_DEFERRED_LOAD;
   loader->pos_increment    = (loader->size / 4) ? (loader->size / 4) : 4;

   if (is_osk)
      loader->flags        |= OVERLAY_LOADER_IS_OSK;
#ifdef RARCH_INTERNAL
   if (video_driver_supports_rgba())
      loader->flags        |= OVERLAY_LOADER_RGBA_SUPPORT;
#endif

   t                        = task_init();

   if (!t)
   {
      config_file_free(conf);
      free(loader->overlays);
      if (loader->zip_path)
         free(loader->zip_path);
      if (loader->zip_cfg_path)
         free(loader->zip_cfg_path);
      free(loader);
      return false;
   }

   loader->overlay_path     = strdup(overlay_path);

   t->handler               = task_overlay_handler;
   t->cleanup               = task_overlay_free;
   t->state                 = loader;
   t->callback              = cb;
   t->user_data             = user_data;

   task_queue_push(t);

   return true;
}

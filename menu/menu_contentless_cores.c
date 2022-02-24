/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2011-2020 - Daniel De Matteis
 *  Copyright (C) 2019-2022 - James Leaver
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

#include <stddef.h>
#include <compat/strcasestr.h>
#include <compat/strl.h>
#include <array/rhmap.h>
#include <file/file_path.h>
#include <string/stdstring.h>

#include "menu_driver.h"
#include "menu_displaylist.h"
#include "../retroarch.h"
#include "../core_info.h"
#include "../configuration.h"

#define CONTENTLESS_CORE_ICON_DEFAULT "default.png"

typedef struct
{
   uintptr_t **system;
   uintptr_t fallback;
} contentless_core_icons_t;

typedef struct
{
   contentless_core_icons_t *icons;
   bool icons_enabled;
} contentless_cores_state_t;

static contentless_cores_state_t *contentless_cores_state = NULL;

static void contentless_cores_unload_icons(contentless_cores_state_t *state)
{
   size_t i, cap;

   if (!state || !state->icons)
      return;

   if (state->icons->fallback)
      video_driver_texture_unload(&state->icons->fallback);

   for (i = 0, cap = RHMAP_CAP(state->icons->system); i != cap; i++)
   {
      if (RHMAP_KEY(state->icons->system, i))
      {
         uintptr_t *icon = state->icons->system[i];

         if (!icon)
            continue;

         video_driver_texture_unload(icon);
         free(icon);
      }
   }

   RHMAP_FREE(state->icons->system);
   free(state->icons);
   state->icons = NULL;
}

static void contentless_cores_load_icons(contentless_cores_state_t *state)
{
   bool rgba_supported              = video_driver_supports_rgba();
   core_info_list_t *core_info_list = NULL;
   char icon_directory[PATH_MAX_LENGTH];
   char icon_path[PATH_MAX_LENGTH];
   size_t i;

   icon_directory[0] = '\0';
   icon_path[0]      = '\0';

   if (!state)
      return;

   /* Unload any existing icons */
   contentless_cores_unload_icons(state);

   if (!state->icons_enabled)
      return;

   /* Create new icon container */
   state->icons = (contentless_core_icons_t*)calloc(
         1, sizeof(*state->icons));

   /* Get icon directory */
   fill_pathname_application_special(icon_directory,
         sizeof(icon_directory),
         APPLICATION_SPECIAL_DIRECTORY_ASSETS_SYSICONS);

   if (string_is_empty(icon_directory))
      return;

   /* Load fallback icon */
   fill_pathname_join(icon_path, icon_directory,
         CONTENTLESS_CORE_ICON_DEFAULT, sizeof(icon_path));

   if (path_is_valid(icon_path))
   {
      struct texture_image ti = {0};
      ti.supports_rgba        = rgba_supported;

      if (image_texture_load(&ti, icon_path))
      {
         if (ti.pixels)
            video_driver_texture_load(&ti,
                  TEXTURE_FILTER_MIPMAP_LINEAR,
                  &state->icons->fallback);

         image_texture_free(&ti);
      }
   }

   /* Get icons for all contentless cores */
   core_info_get_list(&core_info_list);

   if (!core_info_list)
      return;

   for (i = 0; i < core_info_list->count; i++)
   {
      core_info_t *core_info = core_info_get(core_info_list, i);

      /* Icon name is the first entry in the core
       * info database list */
      if (core_info &&
          core_info->supports_no_game &&
          core_info->databases_list &&
          (core_info->databases_list->size > 0))
      {
         const char *icon_name   =
               core_info->databases_list->elems[0].data;
         struct texture_image ti = {0};
         ti.supports_rgba        = rgba_supported;

         fill_pathname_join(icon_path, icon_directory,
               icon_name, sizeof(icon_path));
         strlcat(icon_path, ".png", sizeof(icon_path));

         if (!path_is_valid(icon_path))
            continue;

         if (image_texture_load(&ti, icon_path))
         {
            if (ti.pixels)
            {
               uintptr_t *icon = (uintptr_t*)calloc(1, sizeof(*icon));

               video_driver_texture_load(&ti,
                     TEXTURE_FILTER_MIPMAP_LINEAR,
                     icon);

               /* Add icon to hash map */
               RHMAP_SET_STR(state->icons->system, core_info->core_file_id.str, icon);
            }

            image_texture_free(&ti);
         }
      }
   }
}

uintptr_t menu_contentless_cores_get_entry_icon(const char *core_id)
{
   contentless_cores_state_t *state = contentless_cores_state;
   uintptr_t *icon                  = NULL;

   if (!state ||
       !state->icons_enabled ||
       !state->icons ||
       string_is_empty(core_id))
      return 0;

   icon = RHMAP_GET_STR(state->icons->system, core_id);

   if (icon)
      return *icon;

   return state->icons->fallback;
}

void menu_contentless_cores_context_init(void)
{
   if (!contentless_cores_state)
      return;

   contentless_cores_load_icons(contentless_cores_state);
}

void menu_contentless_cores_context_deinit(void)
{
   if (!contentless_cores_state)
      return;

   contentless_cores_unload_icons(contentless_cores_state);
}

void menu_contentless_cores_free(void)
{
   if (!contentless_cores_state)
      return;

   contentless_cores_unload_icons(contentless_cores_state);
   free(contentless_cores_state);
   contentless_cores_state = NULL;
}

unsigned menu_displaylist_contentless_cores(file_list_t *list, settings_t *settings)
{
   unsigned count                   = 0;
   enum menu_contentless_cores_display_type
         core_display_type          = (enum menu_contentless_cores_display_type)
               settings->uints.menu_content_show_contentless_cores;
   core_info_list_t *core_info_list = NULL;

   /* Get core list */
   core_info_get_list(&core_info_list);

   if (core_info_list)
   {
      size_t menu_index = 0;
      size_t i;

      /* Sort cores alphabetically */
      core_info_qsort(core_info_list, CORE_INFO_LIST_SORT_DISPLAY_NAME);

      /* Loop through cores */
      for (i = 0; i < core_info_list->count; i++)
      {
         core_info_t *core_info = core_info_get(core_info_list, i);
         bool core_valid = false;

         if (core_info)
         {
            switch (core_display_type)
            {
               case MENU_CONTENTLESS_CORES_DISPLAY_ALL:
                  core_valid = core_info->supports_no_game;
                  break;
               case MENU_CONTENTLESS_CORES_DISPLAY_SINGLE_PURPOSE:
                  core_valid = core_info->supports_no_game &&
                        core_info->single_purpose;
                  break;
               default:
                  break;
            }

            if (core_valid &&
                menu_entries_append_enum(list,
                     core_info->path,
                     core_info->core_file_id.str,
                     MENU_ENUM_LABEL_CONTENTLESS_CORE,
                     MENU_SETTING_ACTION_CONTENTLESS_CORE_RUN,
                     0, 0))
            {
               file_list_set_alt_at_offset(
                     list, menu_index, core_info->display_name);

               menu_index++;
               count++;
            }
         }
      }
   }

   /* Initialise icons, if required */
   if (!contentless_cores_state && (count > 0))
   {
      contentless_cores_state = (contentless_cores_state_t*)calloc(1,
            sizeof(*contentless_cores_state));

      /* Disable icons when using menu drivers without
       * icon support */
      contentless_cores_state->icons_enabled =
            !string_is_equal(menu_driver_ident(), "rgui");

      contentless_cores_load_icons(contentless_cores_state);
   }

   if ((count == 0) &&
       menu_entries_append_enum(list,
            msg_hash_to_str(MENU_ENUM_LABEL_VALUE_NO_CORES_AVAILABLE),
            msg_hash_to_str(MENU_ENUM_LABEL_NO_CORES_AVAILABLE),
            MENU_ENUM_LABEL_NO_CORES_AVAILABLE,
            0, 0, 0))
      count++;

   return count;
}
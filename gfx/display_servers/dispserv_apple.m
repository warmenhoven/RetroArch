/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *  Copyright (C) 2016-2019 - Brad Parker
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
#include "../video_display_server.h"
#include "../video_driver.h"
#include "../../ui/drivers/cocoa/apple_platform.h"
#include "../../ui/drivers/cocoa/cocoa_common.h"

#if OSX
#import <AppKit/AppKit.h>
#endif

#if OSX
static bool apple_display_server_set_window_opacity(void *data, unsigned opacity)
{
   settings_t *settings      = config_get_ptr();
   bool windowed_full        = settings->bools.video_windowed_fullscreen;
   NSWindow *window          = ((RetroArch_OSX*)[[NSApplication sharedApplication] delegate]).window;
   if (windowed_full || !window.keyWindow)
      return false;
   window.alphaValue = (CGFloat)opacity / (CGFloat)100.0f;
   return true;
}

static bool apple_display_server_set_window_progress(void *data, int progress, bool finished)
{
#if 0
   static NSProgressIndicator *indicator;
   static dispatch_once_t once;
   dispatch_once(&once, ^{
      indicator = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(0, 0, 100, 20)];
      indicator.indeterminate = NO;
      indicator.minValue = 0;
      indicator.maxValue = 100;
      indicator.doubleValue = 0;

      // Create a custom view for the dock tile
      NSView *dockTileView = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 100, 20)];
      [dockTileView addSubview:indicator];
      [indicator setTranslatesAutoresizingMaskIntoConstraints:NO];

      [NSLayoutConstraint activateConstraints:@[
            [indicator.centerXAnchor constraintEqualToAnchor:dockTileView.centerXAnchor],
            [indicator.centerYAnchor constraintEqualToAnchor:dockTileView.centerYAnchor]
            ]];

      // Set the custom view as the dock tile content view
         [[NSApp dockTile] setContentView:dockTileView];
   });
   if (finished)
      indicator.doubleValue = (double)100.0;
   else
      indicator.doubleValue = (double)progress;
   [[NSApp dockTile] display];
#endif
   return true;
}

static bool apple_display_server_set_window_decorations(void *data, bool on)
{
   settings_t *settings      = config_get_ptr();
   bool windowed_full        = settings->bools.video_windowed_fullscreen;
   NSWindow *window          = ((RetroArch_OSX*)[[NSApplication sharedApplication] delegate]).window;
   if (windowed_full)
      return false;
   if (on)
      window.styleMask |= NSWindowStyleMaskTitled;
   else
      window.styleMask &= ~NSWindowStyleMaskTitled;
   return true;
}
#endif

static bool apple_display_server_set_resolution(void *data,
      unsigned width, unsigned height, int int_hz, float hz,
      int center, int monitor_index, int xoffset, int padjust)
{
   return false;
}

static void *apple_display_server_get_resolution_list(
      void *data, unsigned *len)
{
   unsigned j                        = 0;
   struct video_display_config *conf = NULL;
   *len = 3;

   if (!(conf = (struct video_display_config*)
      calloc(*len, sizeof(struct video_display_config))))
      return NULL;

   for (; j < *len; j++)
   {
      conf[j].width       = 3840;
      conf[j].height      = 2160;
      conf[j].bpp         = 32;
      conf[j].refreshrate = (j == 0) ? 60 : (j == 1 ? 70 : 120);
      conf[j].refreshrate_float = (j == 0) ? 60.0f : (j == 1 ? 70.0f : 120.0f);
      conf[j].interlaced  = false;
      conf[j].dblscan     = false;
      conf[j].idx         = j;
      conf[j].current     = (j == 0);
   }
   return conf;
}

#if IOS
static void apple_display_server_set_screen_orientation(void *data,
      enum rotation rotation)
{
}
#endif

const video_display_server_t dispserv_apple = {
   NULL, /* init */
   NULL, /* destroy */
#if OSX
   apple_display_server_set_window_opacity,
   apple_display_server_set_window_progress,
   apple_display_server_set_window_decorations,
#else
   NULL, /* set_window_opacity */
   NULL, /* set_window_progress */
   NULL, /* set_window_decorations */
#endif
   apple_display_server_set_resolution,
   apple_display_server_get_resolution_list,
   NULL, /* get_output_options */
#if IOS
   apple_display_server_set_screen_orientation,
#else
   NULL, /* set_screen_orientation */
#endif
   NULL, /* get_screen_orientation */
   NULL, /* get_flags */
   "apple"
};

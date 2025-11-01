//
//  Use this file to import your target's public headers that you would like to expose to Swift.
//

#ifndef HAVE_COCOATOUCH
#define HAVE_COCOATOUCH
#endif

#include "libretro-common/include/libretro.h"
#include "../../input/drivers_keyboard/keyboard_event_apple.h"
#include "../../input/input_keymaps.h"
#include "../../paths.h"

#import "../ui/drivers/cocoa/cocoa_common.h"
#import "../../ui/drivers/cocoa/RetroArchPlaylistManager.h"
#import "../ui/drivers/cocoa/apple_platform.h"

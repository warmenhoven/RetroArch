//
//  RetroArchPlaylistManager.m
//  RetroArch
//
//  Unified playlist management for iOS and tvOS
//

#import "RetroArchPlaylistManager.h"
#include "../../../playlist.h"
#include "../../../paths.h"
#include "../../../retroarch.h"
#include "../../../runloop.h"
#include "../../../verbosity.h"
#include "../../../file_path_special.h"
#include "../../../libretro-common/include/lists/dir_list.h"
#include "../../../libretro-common/include/file/file_path.h"
#include "../../../libretro-common/include/string/stdstring.h"

// Block type for enumerating playlist entries
NS_ASSUME_NONNULL_BEGIN
typedef void (^PlaylistEntryBlock)(const struct playlist_entry *entry, NSString *playlistName, uint32_t index);
NS_ASSUME_NONNULL_END

@implementation RetroArchPlaylistGame
@end

@implementation RetroArchPlaylistManager

+ (NSArray<NSString *> * _Nonnull)getCommonPlaylistNames
{
    NSMutableArray<NSString *> *playlistNames = [[NSMutableArray alloc] init];

    // Get RetroArch's playlist directory
    char playlist_dir[PATH_MAX_LENGTH];
    settings_t *settings = config_get_ptr();

    if (!settings) {
        RARCH_LOG("RetroArch not initialized yet, cannot access playlists\n");
        return [playlistNames copy];
    }

    fill_pathname_join_special(playlist_dir,
                               settings->paths.directory_playlist,
                               "",
                               sizeof(playlist_dir));

    // Use dir_list to discover actual playlist files (like menu system does)
    struct string_list str_list = {0};
    if (!dir_list_initialize(&str_list, playlist_dir, NULL, true,
                             settings->bools.show_hidden_files, true, false)) {
        RARCH_LOG("Could not scan playlist directory: %s\n", playlist_dir);
        return [playlistNames copy];
    }

    // Sort playlists (same as menu system)
    dir_list_sort_ignore_ext(&str_list, true);

    // Process each file, filtering for valid playlists
    for (size_t i = 0; i < str_list.size; i++) {
        const char *path = str_list.elems[i].data;
        const char *playlist_file = path_basename(path);

        if (string_is_empty(playlist_file))
            continue;

        // Only include .lpl files (same logic as menu_displaylist_parse_playlists)
        if (!string_is_equal_noncase(path_get_extension(playlist_file), "lpl"))
            continue;

        // Exclude history and favorites files (same logic as menu system)
        if (string_ends_with_size(path, "_history.lpl",
                                  strlen(path), STRLEN_CONST("_history.lpl")) ||
            string_is_equal(playlist_file, FILE_PATH_CONTENT_FAVORITES))
            continue;

        // Add valid playlist to our list
        [playlistNames addObject:[NSString stringWithUTF8String:playlist_file]];
    }

    dir_list_deinitialize(&str_list);

    RARCH_LOG("App Intents: Discovered %lu playlists in %s\n",
              (unsigned long)playlistNames.count, playlist_dir);

    return [playlistNames copy];
}

+ (void)enumerateAllPlaylistEntries:(PlaylistEntryBlock _Nonnull)block
{
    // Get RetroArch's playlist directory
    char playlist_dir[PATH_MAX_LENGTH];
    settings_t *settings = config_get_ptr();

    if (!settings) {
        RARCH_LOG("RetroArch not initialized yet, cannot enumerate playlists\n");
        return;
    }

    fill_pathname_join_special(playlist_dir,
                               settings->paths.directory_playlist,
                               "",
                               sizeof(playlist_dir));

    NSArray<NSString *> *playlistNames = [self getCommonPlaylistNames];

    for (NSString *playlistName in playlistNames) {
        char playlist_path[PATH_MAX_LENGTH];
        fill_pathname_join_special(playlist_path,
                                   playlist_dir,
                                   [playlistName UTF8String],
                                   sizeof(playlist_path));

        // Try to load the playlist
        playlist_config_t config;
        config.capacity = COLLECTION_SIZE;
        config.old_format = false;
        config.compress = false;
        config.fuzzy_archive_match = false;
        config.autofix_paths = false;
        strlcpy(config.path, playlist_path, sizeof(config.path));
        strlcpy(config.base_content_directory, "", sizeof(config.base_content_directory));

        playlist_t *playlist = playlist_init(&config);
        if (!playlist)
            continue;

        uint32_t playlist_size = playlist_get_size(playlist);

        // Enumerate all entries in this playlist
        for (uint32_t i = 0; i < playlist_size; i++) {
            const struct playlist_entry *entry = NULL;
            playlist_get_index(playlist, i, &entry);

            if (entry && entry->path && entry->label) {
                block(entry, playlistName, i);
            }
        }

        playlist_free(playlist);
    }
}

+ (NSArray<RetroArchPlaylistGame *> * _Nonnull)getAllGames
{
    NSMutableArray<RetroArchPlaylistGame *> *games = [[NSMutableArray alloc] init];

    // Check if RetroArch is properly initialized
    runloop_state_t *runloop_st = runloop_state_get_ptr();
    if (!runloop_st || !(runloop_st->flags & RUNLOOP_FLAG_IS_INITED)) {
        RARCH_LOG("RetroArch not fully initialized, cannot access playlists\n");
        return [games copy];
    }

    // Double-check that config is available
    settings_t *settings = config_get_ptr();
    if (!settings) {
        RARCH_LOG("RetroArch configuration not available, cannot access playlists\n");
        return [games copy];
    }

    [self enumerateAllPlaylistEntries:^(const struct playlist_entry *entry, NSString *playlistName, uint32_t index) {
        RetroArchPlaylistGame *game = [[RetroArchPlaylistGame alloc] init];

        // Create a unique ID from path and playlist
        game.gameId = [NSString stringWithFormat:@"%@:%@", playlistName, @(index)];
        game.title = [NSString stringWithUTF8String:entry->label];
        game.fullPath = [NSString stringWithUTF8String:entry->path];

        // Extract filename from path
        const char *filename = path_basename(entry->path);
        game.filename = [NSString stringWithUTF8String:filename];

        if (entry->core_path)
            game.corePath = [NSString stringWithUTF8String:entry->core_path];
        if (entry->core_name)
            game.coreName = [NSString stringWithUTF8String:entry->core_name];

        [games addObject:game];
    }];

    RARCH_LOG("App Intents: Found %lu games across all playlists\n", (unsigned long)games.count);
    return [games copy];
}

+ (nullable RetroArchPlaylistGame *)findGameByFilename:(NSString * _Nonnull)filename
{
    // Check if RetroArch is properly initialized
    runloop_state_t *runloop_st = runloop_state_get_ptr();
    if (!runloop_st || !(runloop_st->flags & RUNLOOP_FLAG_IS_INITED)) {
        RARCH_LOG("RetroArch not fully initialized, cannot find games\n");
        return nil;
    }

    NSArray<RetroArchPlaylistGame *> *allGames = [self getAllGames];

    for (RetroArchPlaylistGame *game in allGames) {
        if ([game.filename isEqualToString:filename]) {
            return game;
        }
    }

    return nil;
}

@end

//
//  GameEntity.swift
//  RetroArch
//
//  App Intents support for game launching
//

import Foundation
import AppIntents

@available(iOS 16.0, *)
struct GameEntity: AppEntity, Identifiable {
   static var typeDisplayRepresentation = TypeDisplayRepresentation(name: "Game")
   static let defaultQuery = GameEntityQuery()

   var id: String
   var displayRepresentation: DisplayRepresentation
   var filename: String
   var core: String?
   var coreName: String?
   var systemName: String?

   init(id: String, title: String, filename: String, core: String? = nil, coreName: String? = nil, systemName: String? = nil) {
      self.id = id
      self.filename = filename
      self.core = core
      self.coreName = coreName
      self.systemName = systemName

      // Create display representation with system context for disambiguation
      let subtitle: String
      if let systemName = systemName {
         subtitle = systemName
      } else if let coreName = coreName {
         subtitle = coreName
      } else {
         subtitle = "Unknown System"
      }

      // Create synonyms for better Siri recognition
      var synonyms: [String] = []

      // Add the base title
      synonyms.append(title)

      // Remove common ROM suffixes for better matching
      let cleanTitle = title.replacingOccurrences(of: " (USA)", with: "", options: .caseInsensitive)
         .replacingOccurrences(of: " (Europe)", with: "", options: .caseInsensitive)
         .replacingOccurrences(of: " (Japan)", with: "", options: .caseInsensitive)
         .replacingOccurrences(of: " (World)", with: "", options: .caseInsensitive)
         .replacingOccurrences(of: " [!]", with: "", options: .caseInsensitive)
         .trimmingCharacters(in: .whitespaces)

      if cleanTitle != title {
         synonyms.append(cleanTitle)
      }

      // Add version without punctuation
      let noPunctuationTitle = cleanTitle.replacingOccurrences(of: ":", with: "")
         .replacingOccurrences(of: "-", with: " ")
         .replacingOccurrences(of: "_", with: " ")
         .replacingOccurrences(of: "  ", with: " ")
         .trimmingCharacters(in: .whitespaces)

      if noPunctuationTitle != cleanTitle {
         synonyms.append(noPunctuationTitle)
      }

      self.displayRepresentation = DisplayRepresentation(
         title: "\(title)",
         subtitle: LocalizedStringResource(stringLiteral: subtitle),
         synonyms: synonyms.map { LocalizedStringResource(stringLiteral: $0) }
      )
   }
}

@available(iOS 16.0, *)
struct GameEntityQuery: EntityQuery, EntityStringQuery {
   func entities(for identifiers: [GameEntity.ID]) async throws -> [GameEntity] {
      // Get all available games and filter by the requested identifiers
      let allGames = await getAllGames()
      return allGames.filter { identifiers.contains($0.id) }
   }

   func suggestedEntities() async throws -> [GameEntity] {
      // Return recently played or favorite games for quick access
      let allGames = await getAllGames()
      // For now, return first 10 games - could be enhanced to show recent/favorites
      return Array(allGames.prefix(10))
   }

   // This method enables Siri to match spoken text to game entities
   func entities(matching string: String) async throws -> [GameEntity] {
      print("🎮 [DEBUG] EntityStringQuery.entities(matching:) called with: '\(string)'")
      let allGames = await getAllGames()
      let searchString = string.lowercased()

      print("🎮 [DEBUG] Siri searching for: '\(string)' in \(allGames.count) games")

      // Search for games that match the spoken text
      return allGames.filter { game in
         let title = String(localized: game.displayRepresentation.title).lowercased()
         // Check for exact match first
         if title == searchString {
            return true
         }
         // Check if title contains the search string
         if title.contains(searchString) {
            return true
         }
         // Check for fuzzy matching - remove common words and punctuation
         let cleanTitle = title.replacingOccurrences(of: " (usa)", with: "")
            .replacingOccurrences(of: " (europe)", with: "")
            .replacingOccurrences(of: " (japan)", with: "")
            .replacingOccurrences(of: " (world)", with: "")
            .replacingOccurrences(of: "[!]", with: "")
            .replacingOccurrences(of: "(rev", with: "")
            .replacingOccurrences(of: ")", with: "")
            .replacingOccurrences(of: "(", with: "")
            .replacingOccurrences(of: ":", with: "")
            .replacingOccurrences(of: "-", with: " ")
            .replacingOccurrences(of: "_", with: " ")

         return cleanTitle.contains(searchString) || searchString.contains(cleanTitle)
      }.sorted { game1, game2 in
         // Prioritize exact matches
         let title1 = String(localized: game1.displayRepresentation.title).lowercased()
         let title2 = String(localized: game2.displayRepresentation.title).lowercased()

         if title1 == searchString && title2 != searchString {
            return true
         }
         if title2 == searchString && title1 != searchString {
            return false
         }

         // Then prioritize matches that start with the search string
         if title1.hasPrefix(searchString) && !title2.hasPrefix(searchString) {
            return true
         }
         if title2.hasPrefix(searchString) && !title1.hasPrefix(searchString) {
            return false
         }

         // Finally sort alphabetically
         return title1 < title2
      }
   }

   private func getAllGames() async -> [GameEntity] {
      print("🎮 [DEBUG] getAllGames() called")
      // This will call into the C/Objective-C RetroArch playlist code
      let games = getAllGamesForAppIntents()
      print("🎮 [DEBUG] Found \(games.count) total games in playlists")
      if games.count > 0 {
         print("🎮 [DEBUG] First few games: \(games.prefix(3).map { String(localized: $0.displayRepresentation.title) })")
         return games
      } else {
         print("🎮 [DEBUG] No games found - RetroArch may not be initialized or no playlists available")
         print("🎮 [DEBUG] Returning fallback test games for App Intents debugging")
         return getFallbackGames()
      }
   }

   private func getFallbackGames() -> [GameEntity] {
      // Fallback games for testing when RetroArch isn't initialized
      return [
         GameEntity(
            id: "test-super-mario-world",
            title: "Super Mario World",
            filename: "Super Mario World (USA).sfc",
            core: nil,
            coreName: "Snes9x",
            systemName: "SNES"
         ),
         GameEntity(
            id: "test-super-mario-bros",
            title: "Super Mario Bros",
            filename: "Super Mario Bros (USA).nes",
            core: nil,
            coreName: "QuickNES",
            systemName: "NES"
         ),
         GameEntity(
            id: "test-zelda",
            title: "The Legend of Zelda",
            filename: "Legend of Zelda, The (USA).nes",
            core: nil,
            coreName: "QuickNES",
            systemName: "NES"
         )
      ]
   }

   private func getAllGamesForAppIntents() -> [GameEntity] {
      let objcGames = RetroArchPlaylistManager.getAllGames()

      // Return empty array if no games found (RetroArch not initialized)
      guard !objcGames.isEmpty else {
         return []
      }

      // Create a map to track duplicate game titles for smart disambiguation
      var gamesByTitle: [String: [RetroArchPlaylistGame]] = [:]
      for game in objcGames {
         let cleanTitle = game.title.lowercased().trimmingCharacters(in: .whitespacesAndNewlines)
         if gamesByTitle[cleanTitle] == nil {
            gamesByTitle[cleanTitle] = []
         }
         gamesByTitle[cleanTitle]?.append(game)
      }

      return objcGames.map { objcGame in
         let systemName = extractSystemName(from: objcGame.gameId)
         let cleanTitle = objcGame.title.lowercased().trimmingCharacters(in: .whitespacesAndNewlines)

         // Create unique ID that includes system for disambiguation when there are duplicates
         let uniqueId: String
         if let duplicates = gamesByTitle[cleanTitle], duplicates.count > 1 {
            uniqueId = "\(objcGame.title) (\(systemName))"
         } else {
            uniqueId = objcGame.title
         }

         return GameEntity(
            id: uniqueId,
            title: objcGame.title,
            filename: objcGame.filename,
            core: objcGame.corePath,
            coreName: objcGame.coreName,
            systemName: systemName
         )
      }
   }

   // Extract a user-friendly system name from the playlist game ID
   private func extractSystemName(from gameId: String) -> String {
      // gameId format is "playlistName:index"
      let components = gameId.components(separatedBy: ":")
      guard let playlistName = components.first else { return "Unknown" }

      // Convert playlist filename to user-friendly system name
      let systemName = playlistName
         .replacingOccurrences(of: ".lpl", with: "")
         .replacingOccurrences(of: "Nintendo - ", with: "")
         .replacingOccurrences(of: "Sega - ", with: "")
         .replacingOccurrences(of: "Sony - ", with: "")
         .replacingOccurrences(of: "Microsoft - ", with: "")
         .replacingOccurrences(of: "Atari - ", with: "")
         .replacingOccurrences(of: "SNK - ", with: "")
         .replacingOccurrences(of: "Arcade - ", with: "")
         .replacingOccurrences(of: " - ", with: " ")

      // Handle common abbreviations
      let abbreviations: [String: String] = [
         "Game Boy": "Game Boy",
         "Game Boy Color": "Game Boy Color",
         "Game Boy Advance": "Game Boy Advance",
         "Nintendo Entertainment System": "NES",
         "Super Nintendo Entertainment System": "SNES",
         "Nintendo 64": "Nintendo 64",
         "GameCube": "GameCube",
         "Master System Mark III": "Master System",
         "Mega Drive Genesis": "Genesis",
         "Saturn": "Saturn",
         "Dreamcast": "Dreamcast",
         "PlayStation": "PlayStation",
         "PlayStation Portable": "PSP",
         "Xbox": "Xbox"
      ]

      return abbreviations[systemName] ?? systemName
   }
}

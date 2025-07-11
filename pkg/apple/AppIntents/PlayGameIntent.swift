//
//  PlayGameIntent.swift
//  RetroArch
//
//  App Intent for launching games via Siri/Shortcuts
//

import Foundation
import AppIntents
import UIKit

@available(iOS 16.0, *)
struct PlayGameIntent: AppIntent {
   static var title: LocalizedStringResource = "Play Game"
   static var description = IntentDescription("Launch a game in RetroArch")
   static var openAppWhenRun = true

   @Parameter(title: "Game", description: "The game to launch")
   var game: GameEntity

   @MainActor
   func perform() async throws -> some IntentResult {
      // Get the app delegate and call the game launch method directly
      guard let appDelegate = UIApplication.shared.delegate as? RetroArch_iOS else {
         throw PlayGameIntentError.gameNotFound
      }

      // Launch the game directly using the existing method
      let success = appDelegate.launchGame(byFilename: game.filename)

      if success {
         return .result()
      } else {
         throw PlayGameIntentError.gameNotFound
      }
   }
}

@available(iOS 16.0, *)
enum PlayGameIntentError: Swift.Error, LocalizedError {
   case gameNotFound

   var errorDescription: String? {
      switch self {
      case .gameNotFound:
         return "Game could not be found or launched"
      }
   }
}

//
//  RetroArchAppShortcuts.swift
//  RetroArch
//
//  App Shortcuts provider and bridge for RetroArch App Intents
//

import Foundation
import AppIntents

@available(iOS 16.0, *)
@objc(RetroArchAppShortcuts)
public final class RetroArchAppShortcuts: NSObject, AppShortcutsProvider {
   @AppShortcutsBuilder
   public static var appShortcuts: [AppShortcut] {
      AppShortcut(
         intent: PlayGameIntent(),
         phrases: [
            "Play \(\.$game) in \(.applicationName)"
         ],
         shortTitle: "Play Game",
         systemImageName: "gamecontroller"
      )
   }

   @objc public static func updateAppShortcuts() {
      Task {
         RetroArchAppShortcuts.updateAppShortcutParameters()
      }
   }
}

import UIKit

/// The app's UIKit delegate, for what SwiftUI's App cannot express.
final class HengeAppDelegate: NSObject, UIApplicationDelegate {
    /// Before iOS 27 the external display scene connects through Info.plist's
    /// UIWindowSceneSessionRoleExternalDisplayNonInteractive entry (ExternalDisplaySceneDelegate).
    /// With the SwiftUI lifecycle UIKit asks here for the application role only, never for that
    /// one, with or without the plist entry; this records what it is asked so a device log shows it.
    func application(_ application: UIApplication, configurationForConnecting session: UISceneSession,
                     options: UIScene.ConnectionOptions) -> UISceneConfiguration {
        let config = UISceneConfiguration(name: nil, sessionRole: session.role)
        HengeLog.log("display", "scene configuration asked for role \(session.role.rawValue)")
        #if HENGE_LEGACY
        // The typed role arrives with iOS 16; on 14 and 15 it is the string.
        let external = UISceneSession.Role(rawValue: "UIWindowSceneSessionRoleExternalDisplayNonInteractive")
        #else
        let external: UISceneSession.Role = .windowExternalDisplayNonInteractive
        #endif
        if session.role == external, LegacyExternalDisplay.offeredToAppDelegate {
            config.delegateClass = ExternalDisplaySceneDelegate.self
        }
        return config
    }
}

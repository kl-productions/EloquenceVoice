import Foundation

/// The settings file in the extension's own container. VoiceOver and the app both
/// load the same extension, so both see this one file. No file protection, so
/// VoiceOver can still read it while the phone is locked.
enum SettingsStore {
    private static var url: URL {
        let directory = (try? FileManager.default.url(for: .documentDirectory, in: .userDomainMask,
                                                      appropriateFor: nil, create: true))
            ?? FileManager.default.temporaryDirectory
        return directory.appendingPathComponent("settings.json")
    }

    static func load() -> SynthSettings {
        SynthSettings.decode(try? Data(contentsOf: url)) ?? SynthSettings()
    }

    static func save(_ settings: SynthSettings) {
        try? settings.encoded().write(to: url, options: [.atomic, .noFileProtection])
    }
}

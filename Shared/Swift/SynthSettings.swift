import Foundation

/// Per-voice changes. `nil` keeps the preset's own value.
struct VoiceTuning: Codable, Equatable {
    var speed: Int?             // 0...250
    var pitch: Int?             // 0...100
    var pitchFluctuation: Int?  // 0...100
    var headSize: Int?          // 0...100
    var roughness: Int?         // 0...100
    var breathiness: Int?       // 0...100
    var volume: Int?            // 0...100
}

/// Shared between the app and the extension through the App Group.
struct SynthSettings: Codable, Equatable {
    var sampleRate = 22050
    var useSSMLReader = true
    var tunings: [String: VoiceTuning] = [:]

    private static let key = "synthSettings"

    private static var store: UserDefaults {
        if let group = Bundle.main.object(forInfoDictionaryKey: "OEVVAppGroup") as? String,
           !group.isEmpty,
           let shared = UserDefaults(suiteName: group) {
            return shared
        }
        return .standard
    }

    static func load() -> SynthSettings {
        guard let data = store.data(forKey: key),
              let settings = try? JSONDecoder().decode(SynthSettings.self, from: data)
        else { return SynthSettings() }
        return settings
    }

    func save() {
        if let data = try? JSONEncoder().encode(self) {
            Self.store.set(data, forKey: Self.key)
        }
    }
}

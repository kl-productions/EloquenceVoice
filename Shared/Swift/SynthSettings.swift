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

/// The engine's settings. The extension keeps them in its own container; the app
/// reads and writes them through the audio unit's message channel, so no App Group
/// is needed and they work with a free Apple ID.
struct SynthSettings: Codable, Equatable {
    var sampleRate = 22050
    var useSSMLReader = true
    var tunings: [String: VoiceTuning] = [:]

    func encoded() -> Data {
        (try? JSONEncoder().encode(self)) ?? Data()
    }

    static func decode(_ data: Data?) -> SynthSettings? {
        guard let data else { return nil }
        return try? JSONDecoder().decode(SynthSettings.self, from: data)
    }
}

import AVFoundation

struct EVVLanguage: Identifiable, Hashable {
    let id: UInt32
    let tag: String
    let locale: String
    let name: String
}

struct EVVPreset: Identifiable, Hashable {
    let id: Int          // the engine's preset voice number, 1...8
    let name: String
    let gender: AVSpeechSynthesisVoiceGender
    let age: Int
}

enum VoiceCatalog {
    /// Japanese is left out: its text is Shift-JIS or EUC-JP, never UTF-8, so it would be mis-spoken.
    static let knownLanguages: [EVVLanguage] = [
        .init(id: 0x0001_0000, tag: "enus", locale: "en-US", name: "American English"),
        .init(id: 0x0001_0001, tag: "engb", locale: "en-GB", name: "British English"),
        .init(id: 0x0002_0000, tag: "eses", locale: "es-ES", name: "Castilian Spanish"),
        .init(id: 0x0002_0001, tag: "esus", locale: "es-MX", name: "Mexican Spanish"),
        .init(id: 0x0003_0000, tag: "frfr", locale: "fr-FR", name: "French"),
        .init(id: 0x0003_0001, tag: "frca", locale: "fr-CA", name: "Canadian French"),
        .init(id: 0x0004_0000, tag: "dede", locale: "de-DE", name: "German"),
        .init(id: 0x0005_0000, tag: "itit", locale: "it-IT", name: "Italian"),
        .init(id: 0x0011_0000, tag: "plpl", locale: "pl-PL", name: "Polish"),
    ]

    static let presets: [EVVPreset] = [
        .init(id: 1, name: "Reed", gender: .male, age: 40),
        .init(id: 2, name: "Shelley", gender: .female, age: 40),
        .init(id: 3, name: "Sandy", gender: .female, age: 10),
        .init(id: 4, name: "Rocko", gender: .male, age: 40),
        .init(id: 5, name: "Glen", gender: .male, age: 40),
        .init(id: 6, name: "Flo", gender: .female, age: 40),
        .init(id: 7, name: "Grandma", gender: .female, age: 75),
        .init(id: 8, name: "Grandpa", gender: .male, age: 75),
    ]

    static var languages: [EVVLanguage] {
        knownLanguages.filter { BuiltLanguages.tags.contains($0.tag) }
    }

    static func identifier(_ language: EVVLanguage, _ preset: EVVPreset) -> String {
        "org.openevv.\(language.tag).\(preset.name.lowercased())"
    }

    static func displayName(_ preset: EVVPreset) -> String {
        "\(preset.name) (OpenEVV)"
    }

    static func resolve(_ identifier: String) -> (EVVLanguage, EVVPreset)? {
        let parts = identifier.split(separator: ".")
        guard parts.count >= 2,
              let language = languages.first(where: { $0.tag == parts[parts.count - 2] }),
              let preset = presets.first(where: { $0.name.lowercased() == parts[parts.count - 1] })
        else { return nil }
        return (language, preset)
    }

    static func providerVoices() -> [AVSpeechSynthesisProviderVoice] {
        languages.flatMap { language in
            presets.map { preset in
                let voice = AVSpeechSynthesisProviderVoice(
                    name: displayName(preset),
                    identifier: identifier(language, preset),
                    primaryLanguages: [language.locale],
                    supportedLanguages: [language.locale])
                voice.gender = preset.gender
                voice.age = preset.age
                voice.version = "1"
                return voice
            }
        }
    }
}

import AudioToolbox

/// What the app and the extension say to each other through the audio unit's
/// message channel. Every message is a dictionary with a "request" key.
enum EngineChannel {
    static let name = "eloquence"

    /// Answers engineStarted (Bool), message (String), sampleRate (Int), voiceCount (Int).
    static let status = "status"
    /// Answers settings (Data, JSON of SynthSettings).
    static let getSettings = "getSettings"
    /// Takes settings (Data); answers ok (Bool).
    static let setSettings = "setSettings"

    /// The audio component the extension's Info.plist declares: type ausp, subtype
    /// elqn, manufacturer OEVV.
    static let component = AudioComponentDescription(
        componentType: kAudioUnitType_SpeechSynthesizer,
        componentSubType: 0x656C_716E,
        componentManufacturer: 0x4F45_5656,
        componentFlags: AudioComponentFlags([.sandboxSafe, .isV3AudioUnit]).rawValue,
        componentFlagsMask: AudioComponentFlags([.sandboxSafe, .isV3AudioUnit]).rawValue)
}

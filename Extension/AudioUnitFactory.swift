import CoreAudioKit

/// The extension's entry point, named in Info.plist. iOS asks it for the audio unit.
public class AudioUnitFactory: NSObject, AUAudioUnitFactory {
    private var audioUnit: AUAudioUnit?

    public func beginRequest(with context: NSExtensionContext) {}

    @objc
    public func createAudioUnit(with componentDescription: AudioComponentDescription) throws -> AUAudioUnit {
        let unit = try EloquenceAudioUnit(componentDescription: componentDescription, options: [])
        audioUnit = unit
        return unit
    }
}

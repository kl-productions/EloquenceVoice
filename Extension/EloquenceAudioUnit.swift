import AVFoundation

/// The speech synthesizer iOS loads when VoiceOver, Spoken Content or the app uses one of our voices.
public final class EloquenceAudioUnit: AVSpeechSynthesisProviderAudioUnit {
    private let host: OpaquePointer?
    private let sampleRate: Int
    private let startupMessage: String
    private let outputBus: AUAudioUnitBus
    private var busArray: AUAudioUnitBusArray!
    private let scratch = RenderScratch()

    @objc
    public override init(componentDescription: AudioComponentDescription,
                         options: AudioComponentInstantiationOptions = []) throws {
        // Fall back to the engine's native rate if the chosen one is refused.
        var rate = SettingsStore.load().sampleRate
        var created = elq_host_create(Int32(rate))
        var message = "The engine started at \(rate) Hz."
        if created == nil && rate != 11025 {
            rate = 11025
            created = elq_host_create(Int32(rate))
            message = "The chosen sample rate was refused, so the engine started at 11,025 Hz."
        }
        if created == nil {
            message = "The engine did not start: it found no language or could not make a voice."
        }
        host = created
        sampleRate = rate
        startupMessage = message

        // The unit is made even when the engine failed, so the app can still
        // connect and say why instead of failing silently.
        guard let format = AVAudioFormat(standardFormatWithSampleRate: Double(rate), channels: 1) else {
            throw NSError(domain: NSOSStatusErrorDomain, code: Int(kAudioUnitErr_FormatNotSupported))
        }
        outputBus = try AUAudioUnitBus(format: format)
        try super.init(componentDescription: componentDescription, options: options)
        busArray = AUAudioUnitBusArray(audioUnit: self, busType: .output, busses: [outputBus])
    }

    deinit {
        if let host {
            elq_host_destroy(host)
        }
        scratch.release()
    }

    public override var outputBusses: AUAudioUnitBusArray { busArray }

    public override var speechVoices: [AVSpeechSynthesisProviderVoice] {
        get { host == nil ? [] : VoiceCatalog.providerVoices() }
        set { }
    }

    public override func allocateRenderResources() throws {
        try super.allocateRenderResources()
        scratch.reserve(frames: Int(maximumFramesToRender))
    }

    public override func synthesizeSpeechRequest(_ request: AVSpeechSynthesisProviderRequest) {
        guard let host, let resolved = VoiceCatalog.resolve(request.voice.identifier) else { return }
        let (language, preset) = resolved
        let settings = SettingsStore.load()
        let tuning = settings.tunings[request.voice.identifier] ?? VoiceTuning()
        var voice = ElqVoiceSettings(
            preset: Int32(preset.id),
            speed: Int32(tuning.speed ?? -1),
            pitch: Int32(tuning.pitch ?? -1),
            pitchFluctuation: Int32(tuning.pitchFluctuation ?? -1),
            headSize: Int32(tuning.headSize ?? -1),
            roughness: Int32(tuning.roughness ?? -1),
            breathiness: Int32(tuning.breathiness ?? -1),
            volume: Int32(tuning.volume ?? -1),
            useSSMLReader: settings.useSSMLReader ? 1 : 0)
        request.ssmlRepresentation.withCString { document in
            _ = elq_host_speak(host, language.id, document, &voice)
        }
    }

    public override func cancelSpeechRequest() {
        if let host {
            elq_host_cancel(host)
        }
    }

    public override var internalRenderBlock: AUInternalRenderBlock {
        let host = self.host
        let scratch = self.scratch
        return { actionFlags, _, frameCount, _, outputData, _, _ in
            let buffers = UnsafeMutableAudioBufferListPointer(outputData)
            guard buffers.count > 0 else { return kAudioUnitErr_NoConnection }

            var frames = Int(frameCount)
            if buffers[0].mData == nil {
                guard let pointer = scratch.pointer else { return kAudioUnitErr_TooManyFramesToProcess }
                frames = min(frames, scratch.capacity)
                buffers[0].mData = UnsafeMutableRawPointer(pointer)
            }
            let samples = buffers[0].mData!.assumingMemoryBound(to: Float32.self)

            var finished: Int32 = 1
            var written = 0
            if let host {
                written = elq_host_render(host, samples, frames, &finished)
            }
            if written < frames {
                (samples + written).update(repeating: 0, count: frames - written)
            }
            buffers[0].mDataByteSize = UInt32(frames * MemoryLayout<Float32>.size)
            if finished != 0 {
                actionFlags.pointee = .offlineUnitRenderAction_Complete
            }
            return noErr
        }
    }

    public override func messageChannel(for channelName: String) -> AUMessageChannel {
        EngineMessageChannel(unit: self)
    }

    fileprivate func answer(_ message: [AnyHashable: Any]) -> [AnyHashable: Any] {
        switch message["request"] as? String {
        case EngineChannel.status:
            return [
                "engineStarted": host != nil,
                "message": startupMessage,
                "sampleRate": sampleRate,
                "voiceCount": speechVoices.count,
            ]
        case EngineChannel.getSettings:
            return ["settings": SettingsStore.load().encoded()]
        case EngineChannel.setSettings:
            guard let settings = SynthSettings.decode(message["settings"] as? Data) else { return ["ok": false] }
            SettingsStore.save(settings)
            return ["ok": true]
        default:
            return [:]
        }
    }
}

/// How the app reaches the extension: engine status and settings.
final class EngineMessageChannel: NSObject, AUMessageChannel {
    private weak var unit: EloquenceAudioUnit?
    var callHostBlock: CallHostBlock?

    init(unit: EloquenceAudioUnit) {
        self.unit = unit
    }

    func callAudioUnit(_ message: [AnyHashable: Any]) -> [AnyHashable: Any] {
        unit?.answer(message) ?? [:]
    }
}

/// A buffer for hosts that ask the unit to supply its own output memory.
final class RenderScratch {
    private(set) var pointer: UnsafeMutablePointer<Float32>?
    private(set) var capacity = 0

    func reserve(frames: Int) {
        let wanted = max(frames, 4096)
        guard wanted > capacity else { return }
        release()
        pointer = .allocate(capacity: wanted)
        pointer?.initialize(repeating: 0, count: wanted)
        capacity = wanted
    }

    func release() {
        pointer?.deallocate()
        pointer = nil
        capacity = 0
    }
}

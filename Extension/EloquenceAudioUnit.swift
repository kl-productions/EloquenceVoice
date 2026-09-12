import AVFoundation

/// The speech synthesizer iOS loads when VoiceOver or Spoken Content uses one of our voices.
public final class EloquenceAudioUnit: AVSpeechSynthesisProviderAudioUnit {
    private let host: OpaquePointer
    private let outputBus: AUAudioUnitBus
    private var busArray: AUAudioUnitBusArray!
    private let scratch = RenderScratch()

    public override init(componentDescription: AudioComponentDescription,
                         options: AudioComponentInstantiationOptions = []) throws {
        // Fall back to the engine's native rate if the chosen one is refused.
        var sampleRate = SynthSettings.load().sampleRate
        var created = elq_host_create(Int32(sampleRate))
        if created == nil && sampleRate != 11025 {
            sampleRate = 11025
            created = elq_host_create(Int32(sampleRate))
        }
        guard let host = created,
              let format = AVAudioFormat(standardFormatWithSampleRate: Double(sampleRate),
                                         channels: 1) else {
            throw NSError(domain: NSOSStatusErrorDomain, code: Int(kAudioUnitErr_FailedInitialization))
        }
        self.host = host
        outputBus = try AUAudioUnitBus(format: format)
        try super.init(componentDescription: componentDescription, options: options)
        busArray = AUAudioUnitBusArray(audioUnit: self, busType: .output, busses: [outputBus])
    }

    deinit {
        elq_host_destroy(host)
        scratch.release()
    }

    public override var outputBusses: AUAudioUnitBusArray { busArray }

    public override var speechVoices: [AVSpeechSynthesisProviderVoice] {
        get { VoiceCatalog.providerVoices() }
        set { }
    }

    public override func allocateRenderResources() throws {
        try super.allocateRenderResources()
        scratch.reserve(frames: Int(maximumFramesToRender))
    }

    public override func synthesizeSpeechRequest(_ request: AVSpeechSynthesisProviderRequest) {
        guard let resolved = VoiceCatalog.resolve(request.voice.identifier) else { return }
        let (language, preset) = resolved
        let settings = SynthSettings.load()
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
        elq_host_cancel(host)
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

            var finished: Int32 = 0
            let written = elq_host_render(host, samples, frames, &finished)
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

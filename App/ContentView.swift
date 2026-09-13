import AVFoundation
import SwiftUI

/// The app's side of the loaded voice extension: audio for the Speak button, engine
/// status, and the settings the extension keeps.
final class EngineLink: ObservableObject {
    let unit: AVAudioUnit
    private let channel: AUMessageChannel
    private let engine = AVAudioEngine()
    private var pendingSave: DispatchWorkItem?

    @Published var settings = SynthSettings()
    @Published private(set) var engineStarted = false
    @Published private(set) var statusMessage = ""
    @Published private(set) var voiceCount = 0
    @Published private(set) var sampleRate = 0
    @Published private(set) var audioError: String?

    init(unit: AVAudioUnit) {
        self.unit = unit
        channel = unit.auAudioUnit.messageChannel(for: EngineChannel.name)
        refreshStatus()
        if let saved = SynthSettings.decode(call([ "request": EngineChannel.getSettings ])["settings"] as? Data) {
            settings = saved
        }
        engine.attach(unit)
        engine.connect(unit, to: engine.mainMixerNode, format: unit.outputFormat(forBus: 0))
        engine.prepare()
    }

    private func call(_ message: [AnyHashable: Any]) -> [AnyHashable: Any] {
        channel.callAudioUnit?(message) ?? [:]
    }

    func refreshStatus() {
        let answer = call(["request": EngineChannel.status])
        guard !answer.isEmpty else {
            engineStarted = false
            statusMessage = "The voice extension loaded but did not answer."
            return
        }
        engineStarted = answer["engineStarted"] as? Bool ?? false
        statusMessage = answer["message"] as? String ?? ""
        voiceCount = answer["voiceCount"] as? Int ?? 0
        sampleRate = answer["sampleRate"] as? Int ?? 0
    }

    func startAudio() {
        do {
            let session = AVAudioSession.sharedInstance()
            try session.setCategory(.playback, mode: .spokenAudio, options: [.duckOthers])
            try session.setActive(true)
            if !engine.isRunning {
                try engine.start()
            }
            audioError = nil
        } catch {
            audioError = "Audio could not start: \(error.localizedDescription)"
        }
    }

    func stopAudio() {
        engine.stop()
        try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
    }

    /// Sends text straight to the loaded extension, the way VoiceOver would.
    func speak(_ text: String, voiceIdentifier: String) {
        startAudio()
        let escaped = text
            .replacingOccurrences(of: "&", with: "&amp;")
            .replacingOccurrences(of: "<", with: "&lt;")
            .replacingOccurrences(of: ">", with: "&gt;")
        let request = AVSpeechSynthesisProviderRequest(
            ssmlRepresentation: "<speak>\(escaped)</speak>",
            voice: AVSpeechSynthesisProviderVoice(name: "", identifier: voiceIdentifier,
                                                  primaryLanguages: [], supportedLanguages: []))
        unit.auAudioUnit.perform(#selector(AVSpeechSynthesisProviderAudioUnit.synthesizeSpeechRequest(_:)),
                                 with: request)
    }

    /// Saves shortly after the last change, so dragging a slider does not send a message per step.
    func scheduleSave() {
        pendingSave?.cancel()
        let snapshot = settings
        let work = DispatchWorkItem { [weak self] in
            _ = self?.call(["request": EngineChannel.setSettings, "settings": snapshot.encoded()])
        }
        pendingSave = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.4, execute: work)
    }
}

struct ContentView: View {
    @StateObject private var link: EngineLink
    @State private var sample = "Hello. This is Eloquence, speaking on an iPhone."
    @State private var voiceID: String

    init(audioUnit: AVAudioUnit) {
        _link = StateObject(wrappedValue: EngineLink(unit: audioUnit))
        let first = VoiceCatalog.languages.first.map { VoiceCatalog.identifier($0, VoiceCatalog.presets[0]) }
        _voiceID = State(initialValue: first ?? "")
    }

    var body: some View {
        List {
            Section {
                Picker("Voice", selection: $voiceID) {
                    ForEach(VoiceCatalog.languages) { language in
                        ForEach(VoiceCatalog.presets) { preset in
                            Text(VoiceCatalog.languages.count > 1 ? "\(preset.name), \(language.name)" : preset.name)
                                .tag(VoiceCatalog.identifier(language, preset))
                        }
                    }
                }
                TextField("Text to speak", text: $sample, axis: .vertical)
                Button("Speak") {
                    link.speak(sample, voiceIdentifier: voiceID)
                }
                if let error = link.audioError {
                    Text(error)
                }
            } header: {
                Text("Try it")
            }

            Section {
                Text("Open Settings, Accessibility, VoiceOver, Speech, Voice. Pick a language, then choose a voice marked OpenEVV.")
                Button("Refresh VoiceOver's voice list") {
                    AVSpeechSynthesisProviderVoice.updateSpeechVoices()
                }
            } header: {
                Text("Use it with VoiceOver")
            }

            Section {
                Picker("Sample rate", selection: $link.settings.sampleRate) {
                    Text("11,025 Hz (classic)").tag(11025)
                    Text("22,050 Hz").tag(22050)
                    Text("44,100 Hz").tag(44100)
                }
                Toggle("Use the SSML reader", isOn: $link.settings.useSSMLReader)
            } header: {
                Text("Engine")
            } footer: {
                Text("A sample rate change applies after the app and VoiceOver are restarted. Turn off the SSML reader if some text is skipped or read strangely.")
            }

            ForEach(VoiceCatalog.languages) { language in
                Section {
                    ForEach(VoiceCatalog.presets) { preset in
                        let id = VoiceCatalog.identifier(language, preset)
                        NavigationLink(preset.name) {
                            VoiceDetailView(
                                title: preset.name,
                                voiceIdentifier: id,
                                tuning: Binding(
                                    get: { link.settings.tunings[id] ?? VoiceTuning() },
                                    set: { link.settings.tunings[id] = $0 }),
                                link: link)
                        }
                    }
                } header: {
                    Text("\(language.name) voices")
                }
            }

            Section {
                LabeledContent("Engine", value: link.engineStarted ? "Running" : "Not running")
                Text(link.statusMessage)
                LabeledContent("Voices offered to iOS", value: "\(link.voiceCount)")
                LabeledContent("Sample rate", value: "\(link.sampleRate) Hz")
                Button("Check again") {
                    link.refreshStatus()
                }
            } header: {
                Text("Status")
            }
        }
        .navigationTitle("Eloquence Voice")
        .onChange(of: link.settings) { _ in
            link.scheduleSave()
        }
        .onAppear {
            link.startAudio()
            AVSpeechSynthesisProviderVoice.updateSpeechVoices()
        }
    }
}

struct VoiceDetailView: View {
    let title: String
    let voiceIdentifier: String
    @Binding var tuning: VoiceTuning
    @ObservedObject var link: EngineLink
    @State private var sample = "Hello. This is Eloquence, speaking on an iPhone."

    var body: some View {
        Form {
            Section {
                TextField("Text to speak", text: $sample, axis: .vertical)
                Button("Speak") {
                    link.speak(sample, voiceIdentifier: voiceIdentifier)
                }
            } header: {
                Text("Preview")
            }
            Section {
                TuningRow(title: "Speed", range: 0...250, value: $tuning.speed)
                TuningRow(title: "Pitch", range: 0...100, value: $tuning.pitch)
                TuningRow(title: "Pitch variation", range: 0...100, value: $tuning.pitchFluctuation)
                TuningRow(title: "Head size", range: 0...100, value: $tuning.headSize)
                TuningRow(title: "Roughness", range: 0...100, value: $tuning.roughness)
                TuningRow(title: "Breathiness", range: 0...100, value: $tuning.breathiness)
                TuningRow(title: "Volume", range: 0...100, value: $tuning.volume)
            } header: {
                Text("Voice")
            } footer: {
                Text("Settings left off keep the voice's own value. Speed climbs steeply: 50 is normal, 100 is about three times as fast, and 250 is about twenty-four times. VoiceOver's rate still applies on top.")
            }
        }
        .navigationTitle(title)
    }
}

struct TuningRow: View {
    let title: String
    let range: ClosedRange<Int>
    @Binding var value: Int?

    var body: some View {
        Toggle("Custom \(title.lowercased())", isOn: Binding(
            get: { value != nil },
            // 50 is where the presets sit, so switching this on changes nothing yet.
            set: { value = $0 ? 50 : nil }))
        if let current = value {
            Slider(value: Binding(get: { Double(current) }, set: { value = Int($0.rounded()) }),
                   in: Double(range.lowerBound)...Double(range.upperBound),
                   step: 1) {
                Text(title)
            }
            .accessibilityValue("\(current)")
        }
    }
}

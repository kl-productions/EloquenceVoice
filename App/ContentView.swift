import AVFoundation
import SwiftUI

final class Previewer: ObservableObject {
    private let synthesizer = AVSpeechSynthesizer()

    func speak(_ text: String, voiceIdentifier: String) -> Bool {
        guard let voice = AVSpeechSynthesisVoice(identifier: voiceIdentifier) else { return false }
        synthesizer.stopSpeaking(at: .immediate)
        let utterance = AVSpeechUtterance(string: text)
        utterance.voice = voice
        synthesizer.speak(utterance)
        return true
    }
}

struct ContentView: View {
    @State private var settings = SynthSettings.load()
    @StateObject private var previewer = Previewer()

    var body: some View {
        NavigationStack {
            List {
                Section("Set up") {
                    Text("Open Settings, Accessibility, VoiceOver, Speech, Voice. Pick a language, then choose a voice marked OpenEVV.")
                    Button("Refresh voice list") {
                        AVSpeechSynthesisProviderVoice.updateSpeechVoices()
                    }
                }

                Section {
                    Picker("Sample rate", selection: $settings.sampleRate) {
                        Text("11,025 Hz (classic)").tag(11025)
                        Text("22,050 Hz").tag(22050)
                        Text("44,100 Hz").tag(44100)
                    }
                    Toggle("Use the SSML reader", isOn: $settings.useSSMLReader)
                } header: {
                    Text("Engine")
                } footer: {
                    Text("A sample rate change applies after VoiceOver is turned off and on again. Turn off the SSML reader if some text is skipped or read strangely.")
                }

                ForEach(VoiceCatalog.languages) { language in
                    Section(language.name) {
                        ForEach(VoiceCatalog.presets) { preset in
                            let id = VoiceCatalog.identifier(language, preset)
                            NavigationLink(preset.name) {
                                VoiceDetailView(
                                    title: preset.name,
                                    voiceIdentifier: id,
                                    tuning: Binding(
                                        get: { settings.tunings[id] ?? VoiceTuning() },
                                        set: { settings.tunings[id] = $0 }),
                                    previewer: previewer)
                            }
                        }
                    }
                }
            }
            .navigationTitle("Eloquence Voice")
        }
        .onChange(of: settings) { _, newValue in
            newValue.save()
        }
    }
}

struct VoiceDetailView: View {
    let title: String
    let voiceIdentifier: String
    @Binding var tuning: VoiceTuning
    @ObservedObject var previewer: Previewer
    @State private var sample = "Hello. This is Eloquence, speaking on an iPhone."
    @State private var previewFailed = false

    var body: some View {
        Form {
            Section("Preview") {
                TextField("Text to speak", text: $sample, axis: .vertical)
                Button("Speak") {
                    previewFailed = !previewer.speak(sample, voiceIdentifier: voiceIdentifier)
                }
                if previewFailed {
                    Text("iOS has not loaded this voice yet. Tap Refresh voice list on the main screen, wait a moment, and try again.")
                }
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

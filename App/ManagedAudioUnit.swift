import AVFAudio
import Combine

/// Loads the voice extension the way VoiceOver does, so the app can speak through it
/// directly and say plainly when iOS cannot find or start it.
final class ManagedAudioUnit: ObservableObject {
    enum State {
        case loading
        case failed(String)
        case ready(AVAudioUnit)
    }

    @Published private(set) var state: State = .loading
    @Published private(set) var componentsFound = 0

    init() {
        connect()
    }

    func connect() {
        state = .loading
        Task {
            var lastError = "iOS did not find the Eloquence voice extension inside this app."
            for attempt in 0..<5 {
                let found = AVAudioUnitComponentManager.shared().components(matching: EngineChannel.component)
                DispatchQueue.main.async { self.componentsFound = found.count }
                for component in found {
                    do {
                        let unit = try await AVAudioUnit.instantiate(
                            with: component.audioComponentDescription, options: [.loadOutOfProcess])
                        DispatchQueue.main.async { self.state = .ready(unit) }
                        return
                    } catch {
                        let nsError = error as NSError
                        lastError = "The voice extension was found but failed to load: "
                            + "\(nsError.localizedDescription) (\(nsError.domain) \(nsError.code))."
                    }
                }
                if attempt < 4 {
                    try? await Task.sleep(nanoseconds: 1_000_000_000)
                }
            }
            DispatchQueue.main.async { self.state = .failed(lastError) }
        }
    }
}

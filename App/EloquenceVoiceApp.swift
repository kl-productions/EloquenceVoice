import SwiftUI

@main
struct EloquenceVoiceApp: App {
    @StateObject private var audioUnit = ManagedAudioUnit()

    var body: some Scene {
        WindowGroup {
            NavigationStack {
                switch audioUnit.state {
                case .loading:
                    HStack(spacing: 8) {
                        ProgressView()
                        Text("Starting the Eloquence engine…")
                    }
                    .padding()
                case .failed(let message):
                    StartupFailureView(message: message,
                                       componentsFound: audioUnit.componentsFound,
                                       retry: { audioUnit.connect() })
                case .ready(let unit):
                    ContentView(audioUnit: unit)
                }
            }
        }
    }
}

struct StartupFailureView: View {
    let message: String
    let componentsFound: Int
    let retry: () -> Void

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                Text("The voice could not start")
                    .font(.title2)
                    .accessibilityAddTraits(.isHeader)
                Text(message)
                Text(componentsFound == 0
                     ? "iOS reports no Eloquence voice extension in this app. If you installed it with Sideloadly, check that app extensions are not being removed, then install it again."
                     : "iOS found the voice extension \(componentsFound) time(s), so the extension is installed but did not start.")
                Button("Try again", action: retry)
                    .buttonStyle(.bordered)
            }
            .padding()
        }
        .navigationTitle("Eloquence Voice")
    }
}

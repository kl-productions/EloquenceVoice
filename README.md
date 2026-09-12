# Eloquence Voice for iOS

An iPhone app that adds the classic IBM ViaVoice / ETI Eloquence voices (Reed, Shelley, Sandy, Rocko, Glen, Flo, Grandma, Grandpa) as system voices you can pick in VoiceOver and Spoken Content.

It uses [OpenEVV](https://github.com/mudb0y/openevv), a rebuild of the original engine in portable C. Its audio matches IBM's own engine sample for sample. Apple's built-in Eloquence voices are a separate implementation. This one is the original engine's behavior, and you control it.

## Read this first

- **You need iOS 17 or later.** Apple added support for voices from other apps in iOS 17, not iOS 16. iOS 16 only let you use Apple's own voices.
- **You don't need a Mac.** GitHub can build the app on its own Mac computers for free. You then install it from Windows with Sideloadly. Both paths are covered below.
- **Personal use only.** The engine code is MIT licensed, but the language data comes from IBM's original files and still belongs to IBM. OpenEVV's authors say they can't license it to anyone. Don't publish this on the App Store or share the built app.
- **This has not been run on a real iPhone yet.** It was written without a Mac, so the first build may turn up compile errors. See "If something goes wrong" below.

## What's better than the built-in voices

- It's the original engine's rules and sound.
- Each of the 8 voices has its own settings: speed (up to 250), pitch, pitch variation, head size, roughness, breathiness, and volume.
- You can choose the sample rate: 11,025 Hz is the classic sound, and 22,050 or 44,100 Hz are upsampled without changing the voice.
- Supported languages: US and UK English, Castilian and Mexican Spanish, French, Canadian French, German, Italian, and Polish (Polish is unfinished).
- It includes IBM's own SSML reader, which handles numbers, dates, currency, and pronunciations the way Eloquence always has. You can switch it off.

## Path A: no Mac (GitHub + Windows)

1. Create a free GitHub account, then create a new **private** repository.
2. Upload everything in this folder to it, including the `.github` folder.
3. In the repository, open **Actions**, choose **ios**, then **Run workflow**. Leave the languages as `enus` or add more, separated by spaces, for example `enus engb dede`. Each extra language adds build time. English only takes roughly 15–30 minutes.
4. When the run finishes, open it and download **EloquenceVoice-unsigned-ipa**. Unzip it to get `EloquenceVoice-unsigned.ipa`.
5. On Windows, install **iTunes** (the version from apple.com, not the Microsoft Store) and **Sideloadly** (sideloadly.io).
6. Plug in your iPhone, open Sideloadly, drag in the `.ipa`, enter your Apple ID, and click Start.
7. On the iPhone, open Settings › General › VPN & Device Management and trust your Apple ID. On iOS 16 and later, also turn on Settings › Privacy & Security › Developer Mode.

With a free Apple ID, the app stops opening after 7 days, and you reinstall it with Sideloadly. A paid developer account ($99/year) lasts a year. It also makes the shared settings between the app and the voice more reliable, because free accounts sometimes can't use App Groups.

## Path B: with a Mac

```bash
brew install make xcodegen
OPENEVV_LANGS="enus" bash scripts/build-openevv.sh
xcodegen generate
open EloquenceVoice.xcodeproj
```

In `project.yml`, change `BUNDLE_ROOT` to something unique, like `com.yourname.eloquencevoice`. Then pick your team under Signing for both targets and run it on your iPhone.

## Turning it on

1. Open **Eloquence Voice** once and tap **Refresh voice list**.
2. Go to Settings › Accessibility › VoiceOver › Speech › Voice, pick your language, and choose a voice marked **(OpenEVV)**.
3. For Speak Selection and Speak Screen, go to Settings › Accessibility › Spoken Content › Voices.

You can add it to the VoiceOver rotor under Settings › Accessibility › VoiceOver › Speech › Add New Language.

## If something goes wrong

- **The build fails in "Build the engine".** Apple's compiler is stricter than the Linux one OpenEVV is tested with. Copy the first `error:` lines from the log and bring them back here. They're usually small fixes.
- **The voices don't show up in Settings.** Open the app, tap Refresh voice list, wait a minute, then restart the iPhone.
- **Some text is skipped or sounds odd.** Turn off **Use the SSML reader** in the app.
- **Voice settings don't change anything.** Your Apple ID may not support App Groups. The voice still works, just with its default settings.

## How it works

- `Vendor/OpenEVV.xcframework`: the engine, built for iPhone by `scripts/build-openevv.sh`.
- `Shared/Engine/elq_bridge.c`: starts the engine, passes text in, and hands samples to iOS through a buffer that is safe for real-time audio.
- `Extension/EloquenceAudioUnit.swift`: the `AVSpeechSynthesisProviderAudioUnit` that iOS loads when a voice speaks.
- `App/`: the settings and preview app.
- `tests/bridge_test.c`: drives the bridge the way iOS does and checks 16 behaviors, including SSML rate, pitch, and volume, cancelling, long documents, and sample rates. GitHub runs it on a Mac before building the app, and you can download what it recorded as **bridge-test-recordings**.
- `samples/`: recordings made by that test on Windows, so you can hear the engine before installing anything.

## Not done yet

- **Some SSML.** IBM's reader doesn't understand `say-as` currency or spelling out characters. When a document contains them, those tags are dropped, and the rest of the document still gets its rate, pitch, and volume. Only US English has been tested with the reader. If another language's document is rejected, all tags are removed. Speech still works, but VoiceOver's rate won't apply to it.

- **Word highlighting.** Word and sentence markers aren't reported to iOS yet.
- **Japanese.** It builds, but it isn't offered, because its text isn't UTF-8.
- **Stopping mid-sentence.** The engine always finishes the sentence it's working on in the background, so stopping takes about 30 ms. You won't hear it, because the leftover audio is thrown away.

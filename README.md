## Safex setup

From the repository root, use Bun to download the native dependencies and set
`SAFEX_ROOT` for the current operating system and install the CLI from its root:

```bash
bun run scripts/setup-safex.ts && bun link
```

On macOS, the command adds `SAFEX_ROOT` to `~/.zshrc` (or `~/.bash_profile`).
On Linux, it uses `~/.zshrc`, `~/.bashrc`, or `~/.profile` according to the
active shell. On Windows, it persists the variable with `setx`. Open a new
terminal after it finishes. The command supports macOS, Linux, and Windows.

## Safex CLI

Quick run:
```sh
safex create [name] -p [package]
cd [name]
safex run dev
safex android run
safex ios run
```

Build commands:
```sh
safex android build apk debug
safex android build aab release --keystore ./release.keystore --key-alias upload --sign-pass '<password>'
safex ios build --team ABCDE12345
safex ios build export
```

Initialize and tool the project:

```sh
# init
safex init -p [package] -n [name]
safex ios init -p [package] -n [name]
safex android init -p [package] -n [name]
safex mobile init -p [package] -n [name]
# create mobile icons
safex ios icon -p ./icon
safex android icon -p ./icon
safex mobile icon -p ./icon
```

`safex init` creates `native/CMakeLists.txt`. The generated CMake project uses
the `SAFEX_ROOT` environment variable for Safex native sources and third-party
dependencies. Platform templates are copied into `native/android` and
`native/ios`; existing directories are never overwritten. The CLI supplies
`SAFEX_ROOT` when it builds or runs a platform project.

`safex run dev` configures `native/CMakeLists.txt` in `build`, builds `sdl3js`
in parallel, then runs `./build/sdl3js`.

`safex mobile init` copies both `android/` and `ios/` templates into the
current directory. To generate launcher icons from a 1024×1024 PNG or JPEG,
use `safex mobile icon [-p <icon-path>]`; without `-p`, it uses `icon.png`,
`icon.jpg`, or `icon.jpeg` in the current directory, ignoring filename case.
Generated files are written beneath `native/`. Use `safex android icon` or
`safex ios icon` to update only one platform.

## Android

Android requires JDK 17 or newer, Android SDK 37, Build Tools 37.0.0, NDK
`28.2.13676358`, and CMake `3.31.6`. The checked-in Gradle wrapper downloads
Gradle automatically.

### Command line

On macOS, use the JBR (JDK) and Android SDK installed by Android Studio. In
Android Studio, open **Preferences** → **Appearance & Behavior** → **System
Settings** → **Android SDK**. Copy **Android SDK Location** and, in the **SDK
Tools** tab, install **Android SDK Command-line Tools (latest)**.

Add the following to `~/.zshrc`, replacing the SDK path if Android Studio shows
a different location:

```bash
export JAVA_HOME="/Applications/Android Studio.app/Contents/jbr/Contents/Home"
export ANDROID_HOME="$HOME/Library/Android/sdk"
export ANDROID_SDK_ROOT="$ANDROID_HOME"
export PATH="$JAVA_HOME/bin:$ANDROID_HOME/platform-tools:$ANDROID_HOME/cmdline-tools/latest/bin:$PATH"
```

Reload the shell configuration and verify that the Android Studio tools are
available:

```bash
source ~/.zshrc
java -version
sdkmanager --version
```

Then install the SDK components and point Gradle at the SDK:

```bash
sdkmanager --licenses
sdkmanager \
  "platform-tools" \
  "platforms;android-37.0" \
  "build-tools;37.0.0" \
  "ndk;28.2.13676358" \
  "cmake;3.31.6"
printf 'sdk.dir=%s\n' "$ANDROID_HOME" > android/local.properties
```

Then build with the wrapper:

```bash
# Debug APK, signed with the Android debug key
./scripts/package-android.sh apk debug

# Release APK
./scripts/package-android.sh apk release

# Release Android App Bundle for Google Play
./scripts/package-android.sh aab release
```

The packages are written under `android/app/build/outputs/`.

From a Safex game project, the CLI builds an APK or AAB directly. For a signed
release package, provide the keystore, alias, and either one shared signing
password (`--sign-pass`) or separate `--store-password` and `--key-password`
arguments. Passwords are passed to Gradle as environment properties and are not
written to disk:

```bash
safex android build aab release \
  --keystore ./release.keystore \
  --key-alias upload \
  --store-password '<keystore-password>' \
  --key-password '<key-password>'
```

### Android Studio

1. Run `./scripts/bootstrap-mobile-deps.sh` once.
2. In Android Studio, choose **Open** and select `native/android/`.
3. Set the Gradle JDK to JDK 17 or newer, install the SDK/NDK/CMake versions
   listed above in **Settings > Android SDK**, then sync the project.
4. Select the `app` run configuration and a device or emulator, then click
   **Run**. Use **Build > Build Bundle(s) / APK(s)** to create a distributable.

For a signed release package, copy `android/keystore.properties.example` to
`android/keystore.properties`, create an upload keystore, and fill in its four
values:

```bash
keytool -genkeypair -v \
  -keystore android/release.keystore \
  -alias upload -keyalg RSA -keysize 2048 -validity 10000
```

The manifest requests only network access and OpenGL ES 2. Add camera,
microphone, notifications, or other permissions to
`android/app/src/main/AndroidManifest.xml` only when the game uses them.

## iOS

iOS builds require macOS with Xcode installed. A free Apple account is enough
for a personal device; an Apple Developer Program team is required for release
distribution.

### Command line

Generate the Xcode project and run on an available iPhone Simulator:

```bash
./scripts/generate-ios-project.sh
./scripts/run-ios.sh
```

Set `IOS_SIMULATOR_UDID` to select a simulator, or set
`IOS_CONFIGURATION=Release` for a release simulator build:

```bash
IOS_SIMULATOR_UDID=<simulator-udid> ./scripts/run-ios.sh
```

To archive and export an IPA, set the Apple team ID used for signing:

```bash
DEVELOPMENT_TEAM=ABCDE12345 ./scripts/package-ios.sh
```

The archive and exported IPA are written below `ios/build/`. The default
`ios/ExportOptions.plist` targets App Store Connect; change its `method` to
`ad-hoc` or `development` when appropriate.

Build an IPA from a Safex game project with:

```bash
safex ios build --team ABCDE12345
```

iOS signing uses the signing certificate and provisioning profile installed in
the macOS keychain; Xcode does not accept a certificate password as an IPA
build argument.

### Xcode

1. Run `./scripts/bootstrap-mobile-deps.sh` once, then generate the project:

   ```bash
   ./scripts/generate-ios-project.sh
   open ios/xcode/SDL3Game.xcodeproj
   ```

2. In Xcode, select the `sdl3js` scheme and an iPhone Simulator or connected
   device, then click **Run**.
3. For a physical device or archive, select the project target, choose your
   team under **Signing & Capabilities**, and use **Product > Archive**. In the
   Organizer, choose **Distribute App** to export or upload the IPA.

Regenerate the project after changing CMake configuration. Add privacy usage
descriptions such as `NSCameraUsageDescription` to `ios/Info.plist.in` before
adding the matching native capability.

### Install on macOS (Homebrew)

```bash
sh scripts/setup-mac.sh
```

### Install on Windows (vcpkg)

```powershell
./scripts/setup-win.bat
```

## Native builds

Run the commands in this section from the `native/` directory. Native builds
compile SDL3, SDL3_image, FreeType, Hermes, and Box2D from source, so fetch
those dependencies before the first Android or iOS build:

```bash
./scripts/bootstrap-native-deps.sh
```

### Build
- `cmake -S . -B build`
- `cmake -S . -B build -DENABLE_BOX2D=OFF`: Build without the native Box2D module:
- `cmake --build build --parallel && ./build/sdl3js`: Run

Build the game's web assets before packaging if `dist/` is missing or stale.

### App icons

Replace `scripts/icon.png` with a square 1024px PNG, then generate the iOS and
Android launcher icons with Bun:

```bash
bun run icons
```

## Android

Android requires JDK 17 or newer, Android SDK 37, Build Tools 37.0.0, NDK
`28.2.13676358`, and CMake `3.31.6`. The checked-in Gradle wrapper downloads
Gradle automatically.

### Command line

Install JDK 17+ and the Android command-line tools. On macOS with Homebrew:

```bash
brew install openjdk@17
brew install --cask android-commandlinetools
```

Set `JAVA_HOME` to JDK 17+ and `ANDROID_HOME` (or `ANDROID_SDK_ROOT`) to the
Android SDK location. Then install the SDK components and point Gradle at the
SDK:

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

# Safex CLI

Install the CLI globally with npm, then run it from a game project directory:

```sh
npm install --global safex
```

For a local checkout of this repository, install the CLI from its root instead:

```sh
npm install --global .
```

Then initialize and run the project:

```sh
safex init
safex run dev
safex mobile init
safex mobile icon -p ./icon
safex android init
safex android run
# or
safex ios init
safex ios run
```

`safex init` creates `native/CMakeLists.txt`. The generated CMake project uses
the `SAFEX_ROOT` environment variable for Safex native sources and third-party
dependencies. Platform templates are copied into `native/android` and
`native/ios`; existing directories are never overwritten. The CLI supplies
`SAFEX_ROOT` when it builds or runs a platform project.

`safex run dev` configures `native/CMakeLists.txt` in `build`, builds `sdl3js`
in parallel, then runs `./build/sdl3js`.

`safex mobile init` copies both `android/` and `ios/` templates into the
current directory. To generate launcher icons from a 1024×1024 PNG, use
`safex mobile icon [-p <icon-path>]`; the path defaults to `./icon`. Use
`safex android icon` or `safex ios icon` to update only one platform.

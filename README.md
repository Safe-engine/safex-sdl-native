### Install on macOS (Homebrew)

```bash
brew install sdl3 sdl3_image freetype box2d quickjs-ng
```

### Install on Windows (vcpkg)

```powershell
vcpkg install sdl3 sdl3-image freetype box2d quickjs-ng --triplet x64-windows
# Then pass: -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
```

### Build
- `cmake -S . -B build`
- `cmake -S . -B build -DENABLE_BOX2D=OFF`: Build without the native Box2D module:
- `cmake --build build --parallel && ./build/sdl3js`: Run

### Resolution policy

`Engine.start()` accepts an optional fourth argument that controls how the
logical game size is presented inside the actual window or browser viewport:

```ts
Engine.start('My Game', 720, 1280, 'letterbox')
```

Available policies are `letterbox` (default), `overscan`, `stretch`, and
`integer-scale`. The active `Engine.viewport` exposes the resulting rendered
rectangle, X/Y scale, and safe area.

### Mobile prerequisites

Mobile builds compile SDL3, SDL3_image, FreeType, and QuickJS from source:

```bash
bun run mobile:deps
bun run mobile:assets
```

Android requires Android Studio or command-line SDK tools with SDK 37,
NDK `28.2.13676358`, CMake `3.31.6`, JDK 17+, and Gradle 9.4.1. iOS
requires Xcode and an Apple Developer account for device/release signing.

### Android APK and AAB

On macOS, install all Android command-line dependencies and generate the
Gradle wrapper with:

```bash
bun run android:setup
```

The setup command uses Homebrew to install JDK 17 and Android command-line
tools, then installs the required SDK, build-tools, NDK, and CMake packages.

Open `android/` in Android Studio, or build from the repository root:

```bash
# Debug APK (signed with the Android debug key)
./scripts/package-android.sh apk debug

# Release APK or Play Store bundle
bun run android:apk
bun run android:aab
```

Release output is written below `android/app/build/outputs/`. For signed release
packages, copy `android/keystore.properties.example` to
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

### iOS Xcode and IPA

Generate the Xcode project:

```bash
bun run ios:project
open ios/xcode/SDL3Game.xcodeproj
```

Build, install, and launch the app in an available iPhone Simulator:

```bash
bun run ios:run
```

Set `IOS_SIMULATOR_UDID` to target a specific simulator, or set
`IOS_CONFIGURATION=Release` to run a release build.

Set your Apple team and export a release IPA:

```bash
export DEVELOPMENT_TEAM=ABCDE12345
bun run ios:ipa
```

The archive and IPA are written below `ios/build/`. The default
`ios/ExportOptions.plist` targets App Store Connect; change `method` to
`ad-hoc` or `development` when appropriate. Add privacy usage descriptions
such as `NSCameraUsageDescription` to `ios/Info.plist.in` before adding the
matching native capability.

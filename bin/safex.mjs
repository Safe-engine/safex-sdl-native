#!/usr/bin/env node

import { cpSync, existsSync, mkdirSync, readdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';
import { cwd } from 'node:process';

const safexRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const projectRoot = process.cwd();
const nativeRoot = join(projectRoot, 'native');
const defaultPackageName = 'com.safex.jssdl';
const defaultAppName = 'JS SDL';

function fail(message) {
    console.error(`safex: ${message}`);
    process.exit(1);
}

function run(command, args, options = {}) {
    const result = spawnSync(command, args, {
        cwd: options.cwd ?? projectRoot,
        env: { ...process.env, SAFEX_ROOT: safexRoot, ...options.env },
        stdio: 'inherit',
    });

    if (result.error) {
        fail(`could not run ${command}: ${result.error.message}`);
    }
    if (result.status !== 0) {
        process.exit(result.status ?? 1);
    }
}

function initNative() {
    if (existsSync(nativeRoot)) {
        fail('native/ already exists; refusing to overwrite it.');
    }

    mkdirSync(nativeRoot);
    const cmake = readFileSync(join(safexRoot, 'CMakeLists.txt'), 'utf8')
        .replaceAll('CMAKE_CURRENT_SOURCE_DIR}/third_party', 'SAFEX_ROOT}/third_party')
        .replaceAll('add_subdirectory(third_party/SDL EXCLUDE_FROM_ALL)', 'add_subdirectory("${SAFEX_ROOT}/third_party/SDL" "${CMAKE_CURRENT_BINARY_DIR}/third_party/SDL" EXCLUDE_FROM_ALL)')
        .replaceAll('add_subdirectory(third_party/SDL_image EXCLUDE_FROM_ALL)', 'add_subdirectory("${SAFEX_ROOT}/third_party/SDL_image" "${CMAKE_CURRENT_BINARY_DIR}/third_party/SDL_image" EXCLUDE_FROM_ALL)')
        .replaceAll('add_subdirectory(third_party/freetype EXCLUDE_FROM_ALL)', 'add_subdirectory("${SAFEX_ROOT}/third_party/freetype" "${CMAKE_CURRENT_BINARY_DIR}/third_party/freetype" EXCLUDE_FROM_ALL)')
        .replaceAll('add_subdirectory(third_party/quickjs EXCLUDE_FROM_ALL)', 'add_subdirectory("${SAFEX_ROOT}/third_party/quickjs" "${CMAKE_CURRENT_BINARY_DIR}/third_party/quickjs" EXCLUDE_FROM_ALL)')
        .replaceAll('add_subdirectory(third_party/box2d EXCLUDE_FROM_ALL)', 'add_subdirectory("${SAFEX_ROOT}/third_party/box2d" "${CMAKE_CURRENT_BINARY_DIR}/third_party/box2d" EXCLUDE_FROM_ALL)')
        .replaceAll('    src/main.c', '    ${SAFEX_ROOT}/src/main.c')
        .replaceAll('    src/js_sdl3.c', '    ${SAFEX_ROOT}/src/js_sdl3.c')
        .replaceAll('list(APPEND JS_SDL_SOURCES src/js_box2d.c)', 'list(APPEND JS_SDL_SOURCES ${SAFEX_ROOT}/src/js_box2d.c)')
        .replaceAll('        tests/native_binding_tests.c', '        ${SAFEX_ROOT}/tests/native_binding_tests.c')
        .replaceAll('        src/js_sdl3.c', '        ${SAFEX_ROOT}/src/js_sdl3.c')
        .replaceAll('list(APPEND JS_SDL_TEST_SOURCES src/js_box2d.c)', 'list(APPEND JS_SDL_TEST_SOURCES ${SAFEX_ROOT}/src/js_box2d.c)')
        .replaceAll('target_include_directories(js_sdl3_native_tests PRIVATE src)', 'target_include_directories(js_sdl3_native_tests PRIVATE ${SAFEX_ROOT}/src)');
    const configuredCmake = [
        'set(SAFEX_ROOT "$ENV{SAFEX_ROOT}")',
        'if(NOT SAFEX_ROOT)',
        '    message(FATAL_ERROR "SAFEX_ROOT is required. Run CMake through the safex CLI.")',
        'endif()',
        '',
        cmake,
    ].join('\n');

    writeFileSync(join(nativeRoot, 'CMakeLists.txt'), configuredCmake);
    cpSync(join(safexRoot, '.gitignore'), join(nativeRoot, '.gitignore'));
    cpSync(join(safexRoot, 'vite.config.ts'), join(nativeRoot, 'vite.config.ts'));
    console.log('Created native template files. Run: safex <android|ios> init');
}

function initNativeIfMissing() {
    if (!existsSync(nativeRoot)) initNative();
}

function initOptions(options) {
    const usage = 'Usage: safex [ios|android|mobile] init [-p <package-name>] [-n <app-name>]';
    const values = new Map();
    for (let index = 0; index < options.length; index += 2) {
        const option = options[index];
        const value = options[index + 1];
        if (!['-p', '-n'].includes(option) || !value || values.has(option)) fail(usage);
        values.set(option, value);
    }

    const packageName = values.get('-p');
    const appName = values.get('-n');
    if (packageName && !/^[A-Za-z_][A-Za-z0-9_]*(\.[A-Za-z_][A-Za-z0-9_]*)+$/.test(packageName)) {
        fail('package name must be a dot-separated identifier, such as com.example.game.');
    }
    if (appName && /["\\\r\n<>&$;]/.test(appName)) fail('app name contains unsupported characters.');
    return { packageName, appName };
}

function createOptions(appName, options) {
    const usage = 'Usage: safex create <app-name> [-p <package-name>]';
    if (!appName || !/^[A-Za-z0-9][A-Za-z0-9._-]*$/.test(appName)
        || (options.length !== 0 && (options.length !== 2 || options[0] !== '-p'))) fail(usage);
    return { appName, packageName: initOptions(options).packageName };
}

async function createProject(appName, options) {
    const { packageName } = createOptions(appName, options);
    const workspacePath = join(projectRoot, appName);
    if (existsSync(workspacePath)) fail(`${appName}/ already exists; refusing to overwrite it.`);
    const { initProject, installDependencies, syncResConst } = await import('./project.mjs');

    await initProject(workspacePath);
    await installDependencies(workspacePath);
    await syncResConst(workspacePath);

    if (packageName) run(process.argv[1], ['init', '-p', packageName, '-n', appName], { cwd: workspacePath });
}

function replaceFile(path, pattern, replacement) {
    writeFileSync(path, readFileSync(path, 'utf8').replace(pattern, replacement));
}

function configureNative(options) {
    const cmakePath = join(nativeRoot, 'CMakeLists.txt');
    let cmake = readFileSync(cmakePath, 'utf8')
        .replace('    MACOSX_BUNDLE_BUNDLE_NAME "JS SDL"', '    MACOSX_BUNDLE_BUNDLE_NAME "${JS_SDL_APP_NAME}"')
        .replace('    MACOSX_BUNDLE_GUI_IDENTIFIER "com.safeengine.jssdl"', '    MACOSX_BUNDLE_GUI_IDENTIFIER "${JS_SDL_PACKAGE_NAME}"')
        .replace('    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.safeengine.jssdl"', '    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${JS_SDL_PACKAGE_NAME}"');
    if (!cmake.includes('set(JS_SDL_APP_NAME ')) {
        cmake = cmake.replace('if(ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "iOS")', [
            'set(JS_SDL_APP_NAME "JS SDL" CACHE STRING "Application name")',
            'set(JS_SDL_PACKAGE_NAME "com.safeengine.jssdl" CACHE STRING "iOS bundle identifier")',
            '',
            'if(ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "iOS")',
        ].join('\n'));
    }
    if (options.packageName) cmake = cmake.replace(/set\(JS_SDL_PACKAGE_NAME "[^"]*" CACHE STRING "iOS bundle identifier"\)/,
        `set(JS_SDL_PACKAGE_NAME "${options.packageName}" CACHE STRING "iOS bundle identifier")`);
    if (options.appName) cmake = cmake.replace(/set\(JS_SDL_APP_NAME "[^"]*" CACHE STRING "Application name"\)/,
        `set(JS_SDL_APP_NAME "${options.appName}" CACHE STRING "Application name")`);
    writeFileSync(cmakePath, cmake);

    const plistPath = join(nativeRoot, 'ios', 'Info.plist.in');
    if (existsSync(plistPath)) {
        replaceFile(plistPath, /(<key>CFBundleDisplayName<\/key>\s*<string>)[^<]*(<\/string>)/,
            '$1@JS_SDL_APP_NAME@$2');
    }
}

function configureAndroid(options) {
    const androidRoot = join(nativeRoot, 'android');
    if (!existsSync(androidRoot)) return;
    if (options.packageName) {
        const gradlePath = join(androidRoot, 'app', 'build.gradle');
        replaceFile(gradlePath, /namespace "[^"]+"/, `namespace "${options.packageName}"`);
        replaceFile(gradlePath, /applicationId "[^"]+"/, `applicationId "${options.packageName}"`);
        const activityPath = join(androidRoot, 'app', 'src', 'main', 'java', 'com', 'safeengine', 'jssdl', 'MainActivity.java');
        if (existsSync(activityPath)) replaceFile(activityPath, /^package [^;]+;/m, `package ${options.packageName};`);
        const proguardPath = join(androidRoot, 'app', 'proguard-rules.pro');
        replaceFile(proguardPath, /-keep class [^\s]+\.\*\* \{ \*; \}/, `-keep class ${options.packageName}.** { *; }`);
    }
    if (options.appName) {
        replaceFile(join(androidRoot, 'app', 'src', 'main', 'res', 'values', 'strings.xml'),
            /<string name="app_name">[^<]*<\/string>/, `<string name="app_name">${options.appName}</string>`);
    }
}

function configurePlatforms(options, platforms) {
    configureNative(options);
    if (platforms.includes('android')) configureAndroid(options);
}

function initPlatform(platform, options = {}) {
    initNativeIfMissing();
    const source = join(safexRoot, platform);
    const destination = join(nativeRoot, platform);
    if (!existsSync(destination)) {
        cpSync(source, destination, { recursive: true, preserveTimestamps: true });
        console.log(`Created native/${platform}/`);
    }
    configurePlatforms(options, [platform]);
}

function initPlatformIfMissing(platform) {
    if (!existsSync(join(nativeRoot, platform))) initPlatform(platform);
}

function initMobile(options = {}) {
    initNativeIfMissing();
    const platforms = ['android', 'ios'];
    for (const platform of platforms) {
        initPlatform(platform, options);
    }
}

function iconPath(options) {
    if (options.length === 0) {
        const files = readdirSync(projectRoot, { withFileTypes: true });
        for (const name of ['icon.png', 'icon.jpg', 'icon.jpeg']) {
            const icon = files.find((file) => file.isFile() && file.name.toLowerCase() === name);
            if (icon) return resolve(projectRoot, icon.name);
        }
        return resolve(projectRoot, 'icon');
    }
    if (options.length === 2 && options[0] === '-p') return resolve(projectRoot, options[1]);
    fail('icon expects an optional path: safex <mobile|android|ios> icon [-p <icon-path>]');
}

function generateIcons(platforms, options) {
    run('bun', [
        join(safexRoot, 'scripts', 'generate-icons.ts'),
        '--source', iconPath(options),
        '--platform', platforms,
        '--output', nativeRoot,
    ]);
}

function buildGame() {
    run('bun', ['run', 'vite', 'build', '--config', 'native/vite.config.ts']);
}

function androidGradleRoot() {
    const roots = [join(nativeRoot, 'android'), join(nativeRoot, 'android', 'android')];
    const root = roots.find((candidate) => existsSync(join(candidate, 'gradlew')));
    if (!root) {
        fail('Android Gradle wrapper was not found under native/android/. Run safex android init in a project without an existing native/android/ directory.');
    }
    return root;
}

function syncResources() {
    run('bun', ['run', 'sync-res']);
}

function runDev() {
    initNativeIfMissing();
    syncResources();
    run('cmake', ['-S', '.', '-B', 'build'], { cwd: 'native' });
    run('cmake', ['--build', 'build', '--parallel'], { cwd: 'native' });
    run('bun', ['run', 'vite', 'build', '--watch', '--config', 'native/vite.config.ts']);
}

function runAndroid() {
    initPlatformIfMissing('android');
    syncResources();
    buildGame();
    run('./gradlew', ['--no-daemon', 'installDebug'], { cwd: androidGradleRoot() });
    run('adb', ['shell', 'monkey', '-p', `${androidPackageName()}.debug`, '1']);
}

function androidPackageName() {
    const gradle = readFileSync(join(androidGradleRoot(), 'app', 'build.gradle'), 'utf8');
    return gradle.match(/applicationId "([^"]+)"/)?.[1] ?? defaultPackageName;
}

function runIos() {
    initPlatformIfMissing('ios');
    syncResources();
    buildGame();
    const deviceJson = spawnSync('xcrun', ['simctl', 'list', 'devices', 'available', '-j'], {
        encoding: 'utf8',
    });
    if (deviceJson.status !== 0) {
        fail('could not find an available iPhone Simulator.');
    }
    const devices = Object.values(JSON.parse(deviceJson.stdout).devices).flat();
    const device = devices.find(({ name, state, isAvailable }) =>
        isAvailable !== false && name.startsWith('iPhone ') && state === 'Booted')
        ?? devices.find(({ name, isAvailable }) => isAvailable !== false && name.startsWith('iPhone '));
    if (!device) {
        fail('no available iPhone Simulator was found. Install a Simulator runtime in Xcode.');
    }

    if (device.state !== 'Booted') run('xcrun', ['simctl', 'boot', device.udid]);
    run('xcrun', ['simctl', 'bootstatus', device.udid, '-b']);
    run('open', ['-a', 'Simulator']);

    const iosRoot = join(nativeRoot, 'ios');
    const buildRoot = join(iosRoot, 'simulator');
    const derivedData = join(iosRoot, 'build', 'simulator-derived');
    run('cmake', [
        '-S', nativeRoot, '-B', buildRoot, '-G', 'Xcode',
        '-DCMAKE_SYSTEM_NAME=iOS', '-DCMAKE_OSX_SYSROOT=iphonesimulator',
        `-DCMAKE_OSX_ARCHITECTURES=${process.arch === 'arm64' ? 'arm64' : 'x86_64'}`,
        '-DCMAKE_OSX_DEPLOYMENT_TARGET=15.0', '-DCMAKE_XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH=YES',
        ...iosCmakeSettings(),
    ]);
    run('xcodebuild', [
        '-project', join(buildRoot, 'SDL3Game.xcodeproj'), '-scheme', 'sdl3js',
        '-configuration', 'Debug', '-destination', `id=${device.udid}`,
        '-derivedDataPath', derivedData, '-quiet', 'CODE_SIGNING_ALLOWED=NO', 'build',
    ]);

    const app = join(buildRoot, 'Debug-iphonesimulator', 'sdl3js.app');
    run('xcrun', ['simctl', 'install', device.udid, app]);
    const packageName = nativeSetting('JS_SDL_PACKAGE_NAME', defaultPackageName);
    run('xcrun', ['simctl', 'launch', '--terminate-running-process', device.udid, packageName]);
    console.log(`Launched ${packageName} on ${device.name}.`);
}

function nativeSetting(name, fallback) {
    const cmake = readFileSync(join(nativeRoot, 'CMakeLists.txt'), 'utf8');
    return cmake.match(new RegExp(`set\\(${name} "([^"]*)"`))?.[1] ?? fallback;
}

function iosCmakeSettings() {
    return [
        `-DJS_SDL_APP_NAME=${nativeSetting('JS_SDL_APP_NAME', defaultAppName)}`,
        `-DJS_SDL_PACKAGE_NAME=${nativeSetting('JS_SDL_PACKAGE_NAME', defaultPackageName)}`,
    ];
}

function optionValues(options, usage) {
    const values = new Map();
    for (let index = 0; index < options.length; index += 2) {
        const option = options[index];
        const value = options[index + 1];
        if (!option?.startsWith('--') || value === undefined || values.has(option)) fail(usage);
        values.set(option, value);
    }
    return values;
}

function buildAndroid(options) {
    const usage = 'Usage: safex android build [apk|aab] [debug|release] [--keystore <path> --key-alias <alias> (--sign-pass <password> | --store-password <password> --key-password <password>)]';
    let format = 'aab';
    let variant = 'release';
    const positional = [];
    let flagIndex = options.findIndex((option) => option.startsWith('--'));
    if (flagIndex === -1) flagIndex = options.length;
    positional.push(...options.slice(0, flagIndex));
    if (positional.length > 2) fail(usage);
    [format = format, variant = variant] = positional;
    if (!['apk', 'aab'].includes(format) || !['debug', 'release'].includes(variant)) fail(usage);

    const signing = optionValues(options.slice(flagIndex), usage);
    const allowed = new Set(['--keystore', '--key-alias', '--sign-pass', '--store-password', '--key-password']);
    if ([...signing.keys()].some((option) => !allowed.has(option))) fail(usage);
    if (signing.has('--sign-pass') && (signing.has('--store-password') || signing.has('--key-password'))) fail(usage);

    const signingValues = signing.size === 0 ? undefined : {
        keystore: signing.get('--keystore'),
        keyAlias: signing.get('--key-alias'),
        storePassword: signing.get('--store-password') ?? signing.get('--sign-pass'),
        keyPassword: signing.get('--key-password') ?? signing.get('--sign-pass'),
    };
    if (signingValues && Object.values(signingValues).some((value) => !value)) fail(usage);

    initPlatformIfMissing('android');
    syncResources();
    buildGame();
    const task = `${format === 'apk' ? 'assemble' : 'bundle'}${variant[0].toUpperCase()}${variant.slice(1)}`;
    const env = signingValues ? {
        ORG_GRADLE_PROJECT_safexStoreFile: resolve(projectRoot, signingValues.keystore),
        ORG_GRADLE_PROJECT_safexStorePassword: signingValues.storePassword,
        ORG_GRADLE_PROJECT_safexKeyAlias: signingValues.keyAlias,
        ORG_GRADLE_PROJECT_safexKeyPassword: signingValues.keyPassword,
    } : undefined;
    run('./gradlew', ['--no-daemon', task], { cwd: androidGradleRoot(), env });
    console.log(`Android ${format.toUpperCase()}: native/android/app/build/outputs/`);
}

function buildIos(options) {
    if (options.length === 1 && options[0] === 'export') {
        exportIosProject();
        return;
    }
    const usage = 'Usage: safex ios build --team <Apple Developer team ID>';
    const signing = optionValues(options, usage);
    if (signing.size !== 1 || !signing.has('--team')) fail(usage);

    initPlatformIfMissing('ios');
    syncResources();
    buildGame();
    const iosRoot = join(nativeRoot, 'ios');
    const buildRoot = join(iosRoot, 'xcode');
    const archive = join(iosRoot, 'build', 'JSSDL.xcarchive');
    const exportDir = join(iosRoot, 'build', 'export');
    const team = signing.get('--team');
    run('cmake', [
        '-S', nativeRoot, '-B', buildRoot, '-G', 'Xcode',
        '-DCMAKE_SYSTEM_NAME=iOS', '-DCMAKE_OSX_ARCHITECTURES=arm64',
        '-DCMAKE_OSX_DEPLOYMENT_TARGET=15.0', '-DCMAKE_XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH=NO',
        `-DJS_SDL_DEVELOPMENT_TEAM=${team}`, ...iosCmakeSettings(),
    ]);
    run('xcodebuild', [
        '-project', join(buildRoot, 'SDL3Game.xcodeproj'), '-scheme', 'sdl3js',
        '-configuration', 'Release', '-destination', 'generic/platform=iOS',
        '-archivePath', archive, `DEVELOPMENT_TEAM=${team}`, '-allowProvisioningUpdates', 'archive',
    ]);
    run('xcodebuild', [
        '-exportArchive', '-archivePath', archive, '-exportPath', exportDir,
        '-exportOptionsPlist', join(iosRoot, 'ExportOptions.plist'), '-allowProvisioningUpdates',
    ]);
    console.log(`iOS IPA: ${join('native', 'ios', 'build', 'export')}`);
}

function exportIosProject() {
    initPlatformIfMissing('ios');
    syncResources();
    buildGame();
    const buildRoot = join(nativeRoot, 'ios', 'xcode');
    run('cmake', [
        '-S', nativeRoot, '-B', buildRoot, '-G', 'Xcode',
        '-DCMAKE_SYSTEM_NAME=iOS', '-DCMAKE_OSX_ARCHITECTURES=arm64',
        '-DCMAKE_OSX_DEPLOYMENT_TARGET=15.0', '-DCMAKE_XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH=NO',
        ...iosCmakeSettings(),
    ]);
    console.log(`iOS Xcode project: ${join('native', 'ios', 'xcode', 'SDL3Game.xcodeproj')}`);
}

const [platform, action, ...options] = process.argv.slice(2);
if (platform === 'create') await createProject(action, options);
else if (platform === 'init') {
    const init = initOptions(action === undefined ? [] : [action, ...options]);
    initNativeIfMissing();
    configurePlatforms(init, ['android', 'ios'].filter((name) => existsSync(join(nativeRoot, name))));
}
else if ((platform === 'android' || platform === 'ios') && action === 'init') initPlatform(platform, initOptions(options));
else if (platform === 'mobile' && action === 'init') initMobile(initOptions(options));
else if (platform === 'mobile' && action === 'icon') generateIcons('all', options);
else if ((platform === 'android' || platform === 'ios') && action === 'icon') generateIcons(platform, options);
else if (platform === 'android' && action === 'run') runAndroid();
else if (platform === 'ios' && action === 'run') runIos();
else if (platform === 'android' && action === 'build') buildAndroid(options);
else if (platform === 'ios' && action === 'build') buildIos(options);
else if (platform === 'run' && action === 'dev') runDev();
else {
    console.log('Usage: safex create <app-name> [-p <package-name>] | safex init [-p <package-name>] [-n <app-name>] | safex run dev | safex <mobile|android|ios> init [-p <package-name>] [-n <app-name>] | safex <mobile|android|ios> icon [-p <icon-path>] | safex android <run|build> | safex ios <run|build>');
    process.exitCode = 1;
}

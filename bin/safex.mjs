#!/usr/bin/env node

import { cpSync, existsSync, mkdirSync, readdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';
import { cwd } from 'node:process';

const safexRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const projectRoot = process.cwd();
const nativeRoot = join(projectRoot, 'native');

function fail(message) {
    console.error(`safex: ${message}`);
    process.exit(1);
}

function run(command, args, options = {}) {
    const result = spawnSync(command, args, {
        cwd: options.cwd ?? projectRoot,
        env: { ...process.env, SAFEX_ROOT: safexRoot },
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

function initPlatform(platform) {
    initNativeIfMissing();
    const source = join(safexRoot, platform);
    const destination = join(nativeRoot, platform);
    if (existsSync(destination)) {
        fail(`native/${platform}/ already exists; refusing to overwrite it.`);
    }

    cpSync(source, destination, { recursive: true, preserveTimestamps: true });
    console.log(`Created native/${platform}/`);
}

function initPlatformIfMissing(platform) {
    if (!existsSync(join(nativeRoot, platform))) initPlatform(platform);
}

function initMobile() {
    initNativeIfMissing();
    const platforms = ['android', 'ios'];
    const existing = platforms.find((platform) => existsSync(join(projectRoot, platform)));
    if (existing) {
        fail(`${existing}/ already exists; refusing to overwrite it.`);
    }

    for (const platform of platforms) {
        cpSync(join(safexRoot, platform), join(projectRoot, platform), {
            recursive: true,
            preserveTimestamps: true,
        });
    }
    console.log('Created android/ and ios/.');
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
    run('bun', ['run', 'build']);
}

function runDev() {
    initNativeIfMissing();
    run('cmake', ['-S', '.', '-B', 'build'], { cwd: 'native' });
    run('cmake', ['--build', 'build', '--parallel'], { cwd: 'native' });
    run('bun', ['run', 'vite', 'build', '--watch', '--config', 'native/vite.config.ts']);
}

function runAndroid() {
    initPlatformIfMissing('android');

    buildGame();
    run('./gradlew', ['--no-daemon', 'installDebug'], { cwd: join(nativeRoot, 'android') });
    run('adb', ['shell', 'monkey', '-p', 'com.safeengine.jssdl.debug', '1']);
}

function runIos() {
    initPlatformIfMissing('ios');

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

    const iosRoot = join(nativeRoot, 'ios');
    const buildRoot = join(iosRoot, 'simulator');
    const derivedData = join(iosRoot, 'build', 'simulator-derived');
    run('cmake', [
        '-S', nativeRoot, '-B', buildRoot, '-G', 'Xcode',
        '-DCMAKE_SYSTEM_NAME=iOS', '-DCMAKE_OSX_SYSROOT=iphonesimulator',
        `-DCMAKE_OSX_ARCHITECTURES=${process.arch === 'arm64' ? 'arm64' : 'x86_64'}`,
        '-DCMAKE_OSX_DEPLOYMENT_TARGET=15.0', '-DCMAKE_XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH=YES',
    ]);
    run('xcodebuild', [
        '-project', join(buildRoot, 'SDL3Game.xcodeproj'), '-scheme', 'sdl3js',
        '-configuration', 'Debug', '-destination', `id=${device.udid}`,
        '-derivedDataPath', derivedData, '-quiet', 'CODE_SIGNING_ALLOWED=NO', 'build',
    ]);

    const app = join(buildRoot, 'Debug-iphonesimulator', 'sdl3js.app');
    run('xcrun', ['simctl', 'install', device.udid, app]);
    run('xcrun', ['simctl', 'launch', '--terminate-running-process', device.udid, 'com.safeengine.jssdl']);
}

const [platform, action, ...options] = process.argv.slice(2);
if (platform === 'init' && !action) initNative();
else if ((platform === 'android' || platform === 'ios') && action === 'init') initPlatform(platform);
else if (platform === 'mobile' && action === 'init' && options.length === 0) initMobile();
else if (platform === 'mobile' && action === 'icon') generateIcons('all', options);
else if ((platform === 'android' || platform === 'ios') && action === 'icon') generateIcons(platform, options);
else if (platform === 'android' && action === 'run') runAndroid();
else if (platform === 'ios' && action === 'run') runIos();
else if (platform === 'run' && action === 'dev') runDev();
else {
    console.log('Usage: safex init | safex run dev | safex mobile init | safex <mobile|android|ios> icon [-p <icon-path>] | safex <android|ios> <init|run>');
    process.exitCode = 1;
}

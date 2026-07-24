import { mkdir } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';

const nativeDir = resolve(import.meta.dir, '..');
const source = resolve(import.meta.dir, 'icon.png');

const icons = [
  { path: 'ios/Assets.xcassets/AppIcon.appiconset/AppIcon-1024.png', size: 1024 },
  { path: 'android/app/src/main/res/mipmap-mdpi/ic_launcher.png', size: 48 },
  { path: 'android/app/src/main/res/mipmap-hdpi/ic_launcher.png', size: 72 },
  { path: 'android/app/src/main/res/mipmap-xhdpi/ic_launcher.png', size: 96 },
  { path: 'android/app/src/main/res/mipmap-xxhdpi/ic_launcher.png', size: 144 },
  { path: 'android/app/src/main/res/mipmap-xxxhdpi/ic_launcher.png', size: 192 },
] as const;

const metadata = await Bun.file(source).image().metadata();
if (metadata.format !== 'png' || metadata.width !== 1024 || metadata.height !== 1024) {
  throw new Error(
    `Expected ${source} to be a 1024x1024 PNG; received ${metadata.width}x${metadata.height} ${metadata.format}.`,
  );
}

await Promise.all(
  icons.flatMap(({ path, size }) => {
    const output = resolve(nativeDir, path);
    return [
      mkdir(dirname(output), { recursive: true }).then(() =>
        Bun.file(source).image().resize(size, size).png().write(output),
      ),
      ...(path.includes('ic_launcher.png')
        ? [
          mkdir(dirname(output), { recursive: true }).then(() =>
            Bun.file(source)
              .image()
              .resize(size, size)
              .png()
              .write(output.replace('ic_launcher.png', 'ic_launcher_round.png')),
          ),
        ]
        : []),
    ];
  }),
);

console.log(`Generated 1 iOS and 10 Android app icons from ${source}.`);

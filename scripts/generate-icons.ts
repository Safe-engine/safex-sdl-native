import { existsSync } from 'node:fs';
import { mkdir } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';

const arguments_ = Bun.argv.slice(2);
const sourceIndex = arguments_.indexOf('--source');
const platformIndex = arguments_.indexOf('--platform');
const source = resolve(sourceIndex === -1 ? 'icon' : arguments_[sourceIndex + 1]);
const platform = platformIndex === -1 ? 'all' : arguments_[platformIndex + 1];

if (sourceIndex !== -1 && !arguments_[sourceIndex + 1]) {
  throw new Error('Missing value for --source.');
}
if (!['all', 'android', 'ios'].includes(platform)) {
  throw new Error('Expected --platform to be android, ios, or all.');
}
if (!existsSync(source)) {
  throw new Error(`Icon file does not exist: ${source}`);
}

const icons = [
  { path: 'ios/Assets.xcassets/AppIcon.appiconset/AppIcon-1024.png', size: 1024 },
  { path: 'android/app/src/main/res/mipmap-mdpi/ic_launcher.png', size: 48 },
  { path: 'android/app/src/main/res/mipmap-hdpi/ic_launcher.png', size: 72 },
  { path: 'android/app/src/main/res/mipmap-xhdpi/ic_launcher.png', size: 96 },
  { path: 'android/app/src/main/res/mipmap-xxhdpi/ic_launcher.png', size: 144 },
  { path: 'android/app/src/main/res/mipmap-xxxhdpi/ic_launcher.png', size: 192 },
] as const;

const selectedIcons = icons.filter(({ path }) => platform === 'all' || path.startsWith(`${platform}/`));
const metadata = await Bun.file(source).image().metadata();
if (metadata.format !== 'png' || metadata.width !== 1024 || metadata.height !== 1024) {
  throw new Error(
    `Expected ${source} to be a 1024x1024 PNG; received ${metadata.width}x${metadata.height} ${metadata.format}.`,
  );
}

await Promise.all(
  selectedIcons.flatMap(({ path, size }) => {
    const output = resolve(path);
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

const iosCount = selectedIcons.filter(({ path }) => path.startsWith('ios/')).length;
const androidCount = selectedIcons.filter(({ path }) => path.startsWith('android/')).length * 2;
console.log(`Generated ${iosCount} iOS and ${androidCount} Android app icons from ${source}.`);

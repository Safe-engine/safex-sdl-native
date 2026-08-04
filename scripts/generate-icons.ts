import { existsSync } from 'node:fs';
import { mkdir, rm } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';

const arguments_ = Bun.argv.slice(2);
const sourceIndex = arguments_.indexOf('--source');
const platformIndex = arguments_.indexOf('--platform');
const outputIndex = arguments_.indexOf('--output');
const sourcePath = sourceIndex === -1 ? 'icon' : arguments_[sourceIndex + 1];
const platform = platformIndex === -1 ? 'all' : arguments_[platformIndex + 1];
const outputPath = outputIndex === -1 ? '.' : arguments_[outputIndex + 1];

if (!sourcePath) {
  throw new Error('Missing value for --source.');
}
if (!outputPath) {
  throw new Error('Missing value for --output.');
}
if (!['all', 'android', 'ios'].includes(platform)) {
  throw new Error('Expected --platform to be android, ios, or all.');
}
const source = resolve(sourcePath);
const outputRoot = resolve(outputPath);
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
  { path: 'android/app/src/main/res/drawable/launch_icon.png', size: 192 },
] as const;

const selectedIcons = icons.filter(({ path }) => platform === 'all' || path.startsWith(`${platform}/`));
const adaptiveIcons = [
  'android/app/src/main/res/mipmap-anydpi/ic_launcher.xml',
  'android/app/src/main/res/mipmap-anydpi/ic_launcher_round.xml',
  'android/app/src/main/res/drawable/launch_icon.xml',
];
const metadata = await Bun.file(source).image().metadata();
if (!['png', 'jpeg'].includes(metadata.format) || metadata.width !== 1024 || metadata.height !== 1024) {
  throw new Error(
    `Expected ${source} to be a 1024x1024 PNG or JPEG; received ${metadata.width}x${metadata.height} ${metadata.format}.`,
  );
}

await Promise.all(
  [
    ...selectedIcons.flatMap(({ path, size }) => {
    const output = resolve(outputRoot, path);
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
    ...(platform === 'ios' ? [] : adaptiveIcons.map((path) => rm(resolve(outputRoot, path), { force: true }))),
  ],
);

const iosCount = selectedIcons.filter(({ path }) => path.startsWith('ios/')).length;
const androidCount = selectedIcons
  .filter(({ path }) => path.startsWith('android/'))
  .reduce((count, { path }) => count + (path.includes('ic_launcher.png') ? 2 : 1), 0);
console.log(`Generated ${iosCount} iOS and ${androidCount} Android app icons from ${source}.`);

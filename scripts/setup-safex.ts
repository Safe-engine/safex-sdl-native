import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { mkdir } from "node:fs/promises";

const root = dirname(dirname(fileURLToPath(import.meta.url)));
const thirdParty = join(root, "third_party");

async function run(command: string[], cwd?: string) {
  const process = Bun.spawn(command, {
    cwd,
    stdout: "inherit",
    stderr: "inherit",
  });
  const exitCode = await process.exited;

  if (exitCode !== 0) {
    throw new Error(`Command failed (${exitCode}): ${command.join(" ")}`);
  }
}

async function cloneDependency(directory: string, repository: string, revision: string) {
  const destination = join(thirdParty, directory);

  if (await Bun.file(join(destination, ".git")).exists()) {
    console.log(`${directory} already exists`);
    return;
  }

  await run([
    "git",
    "clone",
    "--depth",
    "1",
    "--recursive",
    "--branch",
    revision,
    repository,
    destination,
  ]);
}

async function bootstrapNativeDependencies() {
  await mkdir(thirdParty, { recursive: true });
  await cloneDependency("SDL", "https://github.com/libsdl-org/SDL.git", process.env.SDL_REVISION ?? "main");
  await cloneDependency("SDL_image", "https://github.com/libsdl-org/SDL_image.git", process.env.SDL_IMAGE_REVISION ?? "main");
  await cloneDependency("freetype", "https://github.com/freetype/freetype.git", process.env.FREETYPE_REVISION ?? "master");
  await cloneDependency("quickjs", "https://github.com/quickjs-ng/quickjs.git", process.env.QUICKJS_REVISION ?? "master");
  await cloneDependency("box2d", "https://github.com/erincatto/box2d.git", process.env.BOX2D_REVISION ?? "main");
  console.log("Native dependencies are ready in third_party/.");
}

async function configureUnixEnvironment() {
  if (process.env.SAFEX_ROOT) {
    console.log(`SAFEX_ROOT is already set to: ${process.env.SAFEX_ROOT}`);
    return;
  }

  const home = process.env.HOME;
  if (!home) {
    throw new Error("HOME is required to configure SAFEX_ROOT.");
  }

  const shell = process.env.SHELL ?? "";
  const profileName = process.platform === "darwin"
    ? shell === "/bin/zsh" ? ".zshrc" : ".bash_profile"
    : shell.endsWith("/zsh") ? ".zshrc" : shell.endsWith("/bash") ? ".bashrc" : ".profile";
  const profile = join(home, profileName);
  const setting = `export SAFEX_ROOT=\"${root}\"`;
  const existing = (await Bun.file(profile).exists()) ? await Bun.file(profile).text() : "";

  if (!existing.split(/\r?\n/).includes(setting)) {
    await Bun.write(profile, `${existing}${existing.endsWith("\n") || !existing ? "" : "\n"}\n${setting}\n`);
  }

  console.log(`Added SAFEX_ROOT to ${profile}. Open a new terminal or run: . ${profile}`);
}

async function configureWindowsEnvironment() {
  await run(["setx", "SAFEX_ROOT", root]);
  console.log("Added SAFEX_ROOT. Restart your command-line application.");
}

if (process.platform === "darwin" || process.platform === "linux") {
  await bootstrapNativeDependencies();
  await configureUnixEnvironment();
} else if (process.platform === "win32") {
  await bootstrapNativeDependencies();
  await configureWindowsEnvironment();
} else {
  throw new Error("setup-safex.ts supports macOS, Linux, and Windows only.");
}

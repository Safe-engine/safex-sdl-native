import { mkdirSync, writeFileSync } from 'node:fs';
import { execSync } from 'node:child_process';
import path from 'node:path';

const baseUrl = 'https://raw.githubusercontent.com/Safe-engine/safex-cocos-starter/refs/heads/main/';
const repositoryTreeUrl = 'https://api.github.com/repos/Safe-engine/safex-cocos-starter/git/trees/';

function getRemoteUrl(filePath) {
  return baseUrl.replace('heads/main/', `heads/sdl-vite/`) + filePath;
}

async function getProjectFiles() {
  const url = `${repositoryTreeUrl}sdl-vite?recursive=1`;
  const res = await fetch(url);
  if (!res.ok) {
    throw Error(`Failed to fetch template files: ${res.statusText}`);
  }
  const response = await res.json();
  return response.tree.filter((item) => item.type === 'blob' && item.path !== 'package.json');
}

async function downloadProjectFile(workspacePath, fileName) {
  const url = getRemoteUrl(fileName);
  const res = await fetch(url);
  if (!res.ok) {
    throw Error(`Failed to fetch: ${fileName}`);
  }
  const arrayBuffer = await res.arrayBuffer();
  const buffer = Buffer.from(arrayBuffer);
  const filePath = path.join(workspacePath, fileName);
  mkdirSync(path.dirname(filePath), { recursive: true });
  writeFileSync(filePath, buffer);
}

export async function initProject(workspacePath) {
  const url = getRemoteUrl('package.json');
  const res = await fetch(url);
  if (!res.ok) {
    throw Error(`Failed to fetch: ${res.statusText}`);
  }
  const response = await res.json();
  mkdirSync(workspacePath, { recursive: true });
  const filePath = path.join(workspacePath, 'package.json');
  response.name = path.basename(workspacePath);
  response.version = '1.0.1';
  response.description = 'game with safex';
  delete response.workspaces;
  // console.log('initProject', filePath, response);
  writeFileSync(filePath, JSON.stringify(response, null, 2).replace(/workspace:*/g, '*'));

  const projectFiles = await getProjectFiles();
  for (let index = 0; index < projectFiles.length; index++) {
    const fileName = projectFiles[index].path;
    if (fileName) {
      await downloadProjectFile(workspacePath, fileName);
    }
  }
}
export function installDependencies(rootFolder) {
  const cwd = rootFolder || process.cwd();
  console.log(`Running: bun install in ${cwd}`);
  execSync('bun install', { cwd, stdio: 'inherit' });
}
export function syncResConst(rootFolder) {
  const cwd = rootFolder || process.cwd();
  console.log(`Running: bun run sync-res in ${cwd}`);
  execSync('bun run sync-res', { cwd, stdio: 'inherit' });
}

// @ts-nocheck
import { spawn } from 'child_process'
import path from 'path'
import { defineConfig } from 'vite'
import { safexTransform } from 'vite-plugin-safex-transform'

function runSdl3jsPlugin() {
  let child: ReturnType<typeof spawn> | null = null
  let isWatch = false

  const killChild = () => {
    if (child) {
      child.kill()
      child = null
    }
  }

  const exitWithChild = (code: number | null, signal: NodeJS.Signals | null) => {
    if (code && code !== 0) {
      console.error(`[run-sdl3js] sdl3js exited with code ${code}${signal ? ` (${signal})` : ''}`)
    }
    process.exit(code ?? 0)
  }

  process.on('exit', killChild)
  process.on('SIGINT', () => {
    killChild()
    process.exit()
  })
  process.on('SIGTERM', () => {
    killChild()
    process.exit()
  })

  return {
    name: 'run-sdl3js',
    configResolved(config: any) {
      isWatch = !!config.build.watch
    },
    writeBundle() {
      if (!isWatch) return

      if (child) {
        console.log('[run-sdl3js] Stopping previous sdl3js process...')
        killChild()
      }

      console.log('[run-sdl3js] Starting ./build/sdl3js...')
      const nextChild = spawn(path.resolve(__dirname, './build/sdl3js'), [], { stdio: 'inherit' })
      child = nextChild

      nextChild.on('error', (err) => {
        console.error('[run-sdl3js] Failed to start sdl3js:', err)
      })
      nextChild.on('exit', (code, signal) => {
        if (child !== nextChild) return

        child = null
        exitWithChild(code, signal)
      })
    },
  }
}

// Vite lib-mode produces a single ESM bundle compatible with QuickJS-NG.
// Native modules are external — resolved at runtime by the C host.
export default defineConfig({
  resolve: {
    alias: {
      // Use the engine's JavaScript fallback when the native Box2D module is
      // intentionally disabled in CMake.
      box2d: '@safe-engine/sdl/lib/physics/box2d.js',
    },
  },
  build: {
    lib: {
      entry: path.resolve(__dirname, '../src/main.ts'),
      formats: ['es'],
      fileName: () => 'main.js',
    },
    outDir: path.resolve(__dirname, 'dist'),
    target: 'esnext',
    minify: false,
    rollupOptions: {
      external: ['sdl3'],
    },
  },
  plugins: [runSdl3jsPlugin(), safexTransform()],
})

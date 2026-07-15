import sharp from 'sharp'
import { promises as fs } from 'fs'
import { execFile } from 'child_process'
import { promisify } from 'util'
import { fileURLToPath } from 'url'
import { dirname, join } from 'path'

const __dirname = dirname(fileURLToPath(import.meta.url))
const root = join(__dirname, '..')
const buildDir = join(root, 'build')
const execFileAsync = promisify(execFile)

const srcSvg = await fs.readFile(join(buildDir, 'icon.svg'))

// Keep PNG fallbacks for Windows and generic Electron use.
const png1024 = join(buildDir, 'icon.png')
await sharp(srcSvg, { density: 512 })
  .resize(1024, 1024)
  .png({ compressionLevel: 9 })
  .toFile(png1024)
console.log('generated', png1024)

// Also produce common sizes for Windows tray / taskbar fallback
for (const size of [256, 512]) {
  const out = join(buildDir, `icon-${size}.png`)
  await sharp(srcSvg, { density: 512 })
    .resize(size, size)
    .png({ compressionLevel: 9 })
    .toFile(out)
  console.log('generated', out)
}

// .icns is only consumable by (and only buildable on) macOS: `iconutil` ships
// with macOS and does not exist on Windows/Linux. Guard it so `npm run icons`
// works cross-platform — the Windows build uses build/icon.png (see the "win"
// icon in package.json), so skipping .icns here is harmless off macOS.
if (process.platform === 'darwin') {
  const iconsetDir = join(buildDir, 'icon.iconset')
  await fs.rm(iconsetDir, { recursive: true, force: true })
  await fs.mkdir(iconsetDir, { recursive: true })

  const macIconSizes = [
    ['icon_16x16.png', 16],
    ['icon_16x16@2x.png', 32],
    ['icon_32x32.png', 32],
    ['icon_32x32@2x.png', 64],
    ['icon_128x128.png', 128],
    ['icon_128x128@2x.png', 256],
    ['icon_256x256.png', 256],
    ['icon_256x256@2x.png', 512],
    ['icon_512x512.png', 512],
    ['icon_512x512@2x.png', 1024]
  ]

  for (const [name, size] of macIconSizes) {
    await sharp(srcSvg, { density: 512 })
      .resize(size, size)
      .png({ compressionLevel: 9 })
      .toFile(join(iconsetDir, name))
  }

  await execFileAsync('iconutil', ['-c', 'icns', iconsetDir, '-o', join(buildDir, 'icon.icns')])
  await fs.rm(iconsetDir, { recursive: true, force: true })
  console.log('generated', join(buildDir, 'icon.icns'))
} else {
  console.log('skipped icon.icns (macOS-only; iconutil unavailable on this platform)')
}

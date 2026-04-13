import sharp from 'sharp'
import { promises as fs } from 'fs'
import { fileURLToPath } from 'url'
import { dirname, join } from 'path'

const __dirname = dirname(fileURLToPath(import.meta.url))
const root = join(__dirname, '..')
const buildDir = join(root, 'build')

const srcSvg = await fs.readFile(join(buildDir, 'icon.svg'))

// electron-builder will auto-derive platform-specific icons from a 512+ PNG,
// but generating a 1024x1024 PNG gives sharp source for both icns and ico.
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

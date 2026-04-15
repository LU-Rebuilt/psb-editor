# psb-editor

Qt6/OpenGL editor for LEGO Universe ForkParticle .psb particle effects.

Part of the [LU-Rebuilt](https://github.com/LU-Rebuilt) project.

## Usage

```
psb_editor [file.psb|file.txt]
```

Supports both single PSB emitter files and multi-emitter effect .txt files.

### Features

**Simulation (matches LU client 1:1):**
- Emission rate with fractional accumulator and max_particles cap (FUN_010d2f90)
- Per-particle randomized sizes: lerp(variation, base, random) for birth/mid/death (FUN_01097c60)
- Lifetime: lerp(life_min, life_var, random) with age counting up (FUN_01097c60)
- Velocity: lerp(vel_min, vel_var, random) distributed within emission volume (FUN_01097c60)
- 3-phase color interpolation: initial -> transitional 1 -> transitional 2 -> final (FUN_0109bb30)
- 2-phase size interpolation at scale_ratio threshold (FUN_0109bb30)
- Gravity: velocity.y += gravity * dt (FUN_01093fc0)
- Drag: velocity += velocity * drag * dt (FUN_01093fc0)
- Skip drag/forces when flags & 0x100 (FUN_0109bb30)
- Spin direction from flags: 0x200000 = always positive, 0x400000 = always negative
- Particle mode decode from PSB flags (FUN_010cdbf0)
- Emission volumes: point, box, sphere, cone, cylinder

**Rendering:**
- Particle mode rendering: billboard (modes 0-4), velocity-aligned streak (modes 2, 5)
- 6 facing modes from effect file: camera billboard, world X/Y/Z, emitter-relative, radial
- Texture blending: additive, screen, multiply, subtract, alpha (from *EBLENDMODE)
- Textured billboarded quads with DDS texture loading (DXT1/3/5 software decode)
- Sprite atlas UV rect support for texture page sub-regions
- Trail rendering with camera-facing ribbon strips and alpha fade
- Distance culling (DIST/DMIN from effect file)
- Distance sorting (DS flag: back-to-front particle order)
- Scale by emitter (SE flag: particle size * emitter scale)

**Effect file support:**
- All tags: EMITTERNAME, TRANSFORM, FACING, ROT, TRAIL, TIME, DS, SE, MT, DIST, DMIN, PRIO, LOOP
- Per-emitter texture editing (Ctrl+T with emitter selection)
- Auto-frame camera to fit all emitters on load

**Editor:**
- Property tree with all PSB fields using official ForkParticle names from MediaFX RE
- Decoded particle mode display (Billboard, Velocity Streak, 3D Model, etc.)
- Flag annotations (skip drag, spin direction overrides)
- Color swatches with color picker (double-click)
- Undo/redo for all edits
- Emitter motion preview (orbit + bob) to test ROT/MT/Trail flags

### Camera Controls

| Input | Action |
|-------|--------|
| Left drag | Orbit |
| Middle drag | Pan |
| Right drag | Zoom |
| Scroll wheel | Zoom (proportional) |

### Keyboard Shortcuts

| Key | Action |
|-----|--------|
| Ctrl+O | Open PSB/Effect file |
| Ctrl+S | Save |
| Ctrl+Shift+S | Save As |
| Ctrl+T | Edit Textures |
| Ctrl+R | Restart (preserves camera) |
| Space | Play/Pause |
| Ctrl+Z | Undo |
| Ctrl+Shift+Z | Redo |

### Toolbar

- **Speed** — Playback speed multiplier (0.1x - 10x)
- **Emission** — Emission rate multiplier (0.1x - 20x)
- **Force Loop** — Continuously re-emit particles
- **Grid** — Show/hide ground grid and axes
- **Emitter Motion** — Preview particles on a moving emitter (orbit + bob)

**Setup:** Set the client root to the game's install directory (containing `res/`) so the editor can find texture pages in `res/forkp/textures/dds/`. Auto-detected when opening files from within a client directory.

## Building

```bash
cmake -B build
cmake --build build -j$(nproc)
```

Requires Qt6 (Widgets, OpenGLWidgets) and OpenGL.

## Acknowledgments

Reverse-engineered from:
- **legouniverse.exe** — Complete particle simulation pipeline
- **mediafx.exe** — Official ForkParticle field names and UI labels
- **RenderParticleDLL.dll** — Particle rendering modes (billboard, velocity streak)
- **GeomObjectDLL.dll** — 3D model particle rendering (.PAX format)

## License

[GNU Affero General Public License v3.0](https://www.gnu.org/licenses/agpl-3.0.html) (AGPLv3)

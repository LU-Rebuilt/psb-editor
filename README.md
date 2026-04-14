# psb-editor

Qt6/OpenGL editor for LEGO Universe ForkParticle .psb particle effects.

> **Status:** Work in progress. The editor can load and preview particle effects but may not be fully functional for all PSB files. Contributions and testing welcome.

> **Note:** This project was developed with significant AI assistance (Claude by Anthropic). All code has been reviewed and validated by the project maintainer, but AI-generated code may contain subtle issues. Contributions and reviews are welcome.

Part of the [LU-Rebuilt](https://github.com/LU-Rebuilt) project.

## Usage

```
psb_editor [file.psb]
```

**Features:**
- Real-time particle simulation using PSB emitter parameters (colors, velocities, sizes, gravity, spin)
- Textured billboarded quads with DDS texture loading from client resources
- Sprite atlas UV rect extraction for texture page sub-regions
- Color lifecycle: start, middle, end color lerp
- Blend mode support: additive, screen, multiply, alpha blend
- Property tree showing all PSB fields organized by section (colors with swatches, timing, velocity, size, rotation, textures with UV rects)
- Adjustable playback speed slider (0.1x - 10x)
- Adjustable emission rate slider (0.1x - 20x)
- Looping playback (burst and continuous emitters)
- Orbit camera with mouse drag and scroll zoom

**Keyboard shortcuts:**
- `Ctrl+O` — Open file
- `Space` — Play/Pause
- `Ctrl+R` — Restart emitter
- `Ctrl+Shift+R` — Set client root

**Setup:** Set the client root to the game's install directory (containing `res/`) so the editor can find texture pages in `res/forkp/textures/dds/`. The editor auto-detects the root when opening PSB files from within a client directory.

## Building

```bash
cmake -B build
cmake --build build -j$(nproc)
```

For local development:

```bash
cmake -B build -DFETCHCONTENT_SOURCE_DIR_LU_ASSETS=/path/to/local/lu-assets \
               -DFETCHCONTENT_SOURCE_DIR_TOOL_COMMON=/path/to/local/tool-common
```

## Acknowledgments

Format parsers built from:
- **Ghidra reverse engineering** of the original LEGO Universe client binary — PSB binary format, ForkParticle emitter fields

## License

[GNU Affero General Public License v3.0](https://www.gnu.org/licenses/agpl-3.0.html) (AGPLv3)


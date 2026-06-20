![irongingot](images/title.png)

<div align="center">
  <img src="https://img.shields.io/github/stars/TheShovel/irongingot?style=flat-square&logo=github" alt="Stars">&nbsp;&nbsp;
  <img src="https://img.shields.io/github/languages/top/TheShovel/irongingot?style=flat-square&logo=c&label=language" alt="Language">&nbsp;&nbsp;
  <img src="https://img.shields.io/github/license/TheShovel/irongingot?style=flat-square" alt="License">&nbsp;&nbsp;
  <img src="https://img.shields.io/github/last-commit/TheShovel/irongingot?style=flat-square&logo=git" alt="Last Commit">
</div>

**irongingot** is a fork of [bareiron](https://github.com/p2r3/bareiron) - a minimalist Minecraft server for low-spec hardware. This fork keeps the low memory usage and adds some much-needed features.

Runs on as low as **~7MB of RAM** !!!

> [!NOTE]
> Unlike the original bareiron, **ESP32 is not supported** in this fork.

![irongingot screenshot](images/Screenshot_20260402_002901.png)

- **Minecraft version:** `1.21.8`
- **Protocol version:** `772`
- **Base project:** bareiron by p2r3

> [!WARNING]
> Only vanilla clients are supported. Fabric and other mod loaders may have issues.

## What's New

Compared with the original bareiron, **irongingot** targets modern vanilla Minecraft and is much more feature-complete: it keeps the low-memory design, but adds configurable gameplay, richer terrain, structures, dimensions, mobs, inventories, and desktop/server-focused builds instead of ESP32 support.

- **More dimensions** - Nether and End support with portal travel, dimension-aware chunks, spawn fixes, and void death handling
- **More structures** - Villages, mineshafts, dungeons, strongholds, and Nether structure support
- **More mobs and entities** - Villagers, piglins, endermen, fish, arrows, and ender pearls
- **More interactions** - Chests, buckets, lava/water flow, flint and steel, farming, smelting, ores, bows, and creative inventory support
- **Doors and stairs** - They work now
- **Trees and vegetation** - Biome-appropriate trees, flowers, and grass generate in the world
- **Better terrain** - Improved world generation with better caves, mountains, ore distribution, and dimension-specific terrain
- **Config file** - Change settings in `server.conf` instead of recompiling
- **Terminal UI** - Server status and logs are shown in a terminal interface
- **Multithreaded chunk gen** - Chunk generation runs in worker threads
- **Musl libc support** - Build with `--musl` for ~7MB RAM usage, plus ARM64 musl cross-build support with Zig
- **Performance fixes** - Various optimizations for chunk streaming, fluid updates, packet handling, and CPU usage

## Quick Start

You can download pre-built binaries from the [Releases page](https://github.com/TheShovel/irongingot/releases), or compile the server yourself. See the **Compilation** section below for instructions.

## Compilation

`./rebuild.sh` does everything for you from a clean clone to a built binary in one command:

```sh
./rebuild.sh
```

It downloads and verifies the MC 1.21.8 `server.jar`, runs the vanilla data generator, extracts village structure NBT, regenerates all generated source files (`include/registries.h`, `src/registries.c`, `include/generated_village_templates.h`, `src/generated_village_templates.c`), then compiles the server.

Prerequisites for `./rebuild.sh`: Java 21+ JDK, Node.js, and curl. On Windows, use **WSL** (Windows Subsystem for Linux) to run the rebuild script.

### Dependencies

For the full release build on Debian/Ubuntu, install:

```sh
sudo apt install build-essential zlib1g-dev libcurl4-openssl-dev musl-tools mingw-w64 zip
```

For the full release build on Arch Linux, install:

```sh
sudo pacman -S --needed base-devel zlib curl musl mingw-w64-gcc zip
```

To also build the Linux ARM64 musl package, install [Zig](https://ziglang.org/download/) and make sure `zig` is on your `PATH`.

On Arch Linux, Zig can be installed with:

```sh
sudo pacman -S --needed zig
```

| Build target | Debian/Ubuntu packages | Arch Linux packages |
|--------------|------------------------|---------------------|
| **Linux x86_64 glibc** | `gcc`, `zlib1g-dev`, `libcurl4-openssl-dev` | `base-devel`, `zlib`, `curl` |
| **Linux x86_64 musl** | `musl-tools` | `musl` |
| **Linux ARM64 musl** | `zig` (cross-compiles with bundled zlib) | `zig` (cross-compiles with bundled zlib) |
| **Windows x86_64 cross-compile** | `mingw-w64` (uses bundled zlib) | `mingw-w64-gcc` (uses bundled zlib) |
| **Windows native/MSYS2** | `mingw-w64-x86_64-gcc`, `mingw-w64-x86_64-zlib` | N/A |

### Build Commands

Before building, generate `include/registries.h` as described above, or use `./rebuild.sh` to regenerate everything and build in one step.

- **Full data/codegen rebuild + local binary:** `./rebuild.sh`
- **All release packages:** `./build_all.sh`
  - writes packages to `build/`
  - builds `irongingot-v<VERSION>-linux-glibc.zip`
  - builds `irongingot-v<VERSION>-linux-musl.zip` when `musl-gcc` is available
  - builds `irongingot-v<VERSION>-linux-arm64-musl.zip` when `zig` is available
  - builds `irongingot-v<VERSION>-windows.zip` when a MinGW cross-compiler is available
- **Set release version:** `VERSION=1.2.3 ./build_all.sh`
- **Linux x86_64 glibc only:** `./build.sh` — dynamically linked, ~30MB RAM usage
- **Linux x86_64 musl only:** `./build.sh --musl` — statically linked, ~7MB RAM usage
- **Windows native:** MSYS2 MINGW64 shell, install `mingw-w64-x86_64-gcc`, run `./build.sh`
- **Windows 32-bit:** MSYS2 MINGW64 shell, install `mingw-w64-cross-gcc`, run `./build.sh --9x`

> [!TIP]
> The musl builds are **strongly recommended** for production use. They are fully static and are intended to provide the lower memory usage described above, including the ARM64 package.

## Development

See [DEVELOPER.md](./DEVELOPER.md#3-architecture-overview) for the full architecture diagram, threading model, packet flow, and chunk pipeline details.

## Configuration

Edit `server.conf` to customize your server:

```ini
port = 25565
max_players = 16
gamemode = 0
view_distance = 10
world_seed = 0xA103DE6C
infinite_block_changes = true
motd = A irongingot server
brand = irongingot
```

Key options include:

- **Performance:** `chunk_cache_size`, `tick_interval`, `broadcast_all_movement`
- **Features:** `allow_chests`, `do_fluid_flow`, `enable_flight`, `allow_doors`
- **World:** `view_distance`, `world_seed`, `infinite_block_changes`

## Non-Volatile Storage

World data auto-saves to `world.json`.

## License

GPL-3.0 License - see [LICENSE](LICENSE).

## Credits

- Original [bareiron](https://github.com/p2r3/bareiron) by p2r3
- [cubiomes](https://github.com/Cubitect/cubiomes) for biome generation
- [Cosmopolitan Libc](https://github.com/jart/cosmopolitan) for cross-platform binaries
- [Alexballistic](https://www.youtube.com/@alexBallistic) for the server icon/logo

## AI Contribution

Approximately **46%** of this project was made with AI assistance.
Based on the currently tracked text files, that is about **29,637 AI-assisted lines** out of **64,436 total lines**. The chart below omits files with fewer than 60 estimated AI-assisted lines, because it would look ugly, and why make a pie chart if it looks ugly. This was calculated using Opencode history.

```mermaid
pie showData
    title Estimated AI-assisted lines by file
    "DEVELOPER.md (319 est. AI lines)" : 319
    "LICENSE (310 est. AI lines)" : 310
    "LLM_REPO_DOCS.md (87 est. AI lines)" : 87
    "MISSING_FEATURES.md (77 est. AI lines)" : 77
    "README.md (72 est. AI lines)" : 72
    "build_all.sh (86 est. AI lines)" : 86
    "build_registries.js (598 est. AI lines)" : 598
    "build_village_templates.js (328 est. AI lines)" : 328
    "decode_tags.py (311 est. AI lines)" : 311
    "include/globals.h (299 est. AI lines)" : 299
    "include/registries.h.bak (837 est. AI lines)" : 837
    "include/special_block.h (79 est. AI lines)" : 79
    "index.html (200 est. AI lines)" : 200
    "rebuild.sh (74 est. AI lines)" : 74
    "src/async.c (94 est. AI lines)" : 94
    "src/chunk_generator.c (244 est. AI lines)" : 244
    "src/config.c (178 est. AI lines)" : 178
    "src/crafting.c (612 est. AI lines)" : 612
    "src/creative_mode.c (362 est. AI lines)" : 362
    "src/globals.c (63 est. AI lines)" : 63
    "src/main.c (691 est. AI lines)" : 691
    "src/mojang.c (178 est. AI lines)" : 178
    "src/packets.c (2638 est. AI lines)" : 2638
    "src/procedures.c (4453 est. AI lines)" : 4453
    "src/registries.c.bak (237 est. AI lines)" : 237
    "src/serialize.c (351 est. AI lines)" : 351
    "src/special_block.c (277 est. AI lines)" : 277
    "src/structures.c (66 est. AI lines)" : 66
    "src/terminal_ui.c (274 est. AI lines)" : 274
    "src/tools.c (607 est. AI lines)" : 607
    "src/worldgen.c (1928 est. AI lines)" : 1928
    "third_party/cjson/cJSON.c (1475 est. AI lines)" : 1475
    "third_party/cjson/cJSON.h (92 est. AI lines)" : 92
    "third_party/zlib/adler32.c (75 est. AI lines)" : 75
    "third_party/zlib/crc32.c (483 est. AI lines)" : 483
    "third_party/zlib/crc32.h (4345 est. AI lines)" : 4345
    "third_party/zlib/deflate.c (984 est. AI lines)" : 984
    "third_party/zlib/deflate.h (173 est. AI lines)" : 173
    "third_party/zlib/gzguts.h (99 est. AI lines)" : 99
    "third_party/zlib/gzlib.c (268 est. AI lines)" : 268
    "third_party/zlib/gzread.c (277 est. AI lines)" : 277
    "third_party/zlib/gzwrite.c (290 est. AI lines)" : 290
    "third_party/zlib/infback.c (289 est. AI lines)" : 289
    "third_party/zlib/inffast.c (147 est. AI lines)" : 147
    "third_party/zlib/inflate.c (702 est. AI lines)" : 702
    "third_party/zlib/inftrees.c (138 est. AI lines)" : 138
    "third_party/zlib/trees.c (514 est. AI lines)" : 514
    "third_party/zlib/zconf.h (250 est. AI lines)" : 250
    "third_party/zlib/zlib.h (891 est. AI lines)" : 891
    "third_party/zlib/zutil.c (138 est. AI lines)" : 138
    "third_party/zlib/zutil.h (117 est. AI lines)" : 117
```

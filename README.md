# irongingot

**irongingot** is a fork of [bareiron](https://github.com/p2r3/bareiron) - a minimalist Minecraft server for low-spec hardware and embedded systems. This fork keeps the low memory usage and adds some much-needed features.

Runs on as low as **5MB of RAM**!!!

![irongingot screenshot](images/Screenshot_20260402_002901.png)

- **Minecraft version:** `1.21.8`
- **Protocol version:** `772`
- **Base project:** bareiron by p2r3

> [!WARNING]
> Only vanilla clients are supported. Fabric and other mod loaders may have issues.

## What's New

- **Doors and stairs** - They work now
- **Trees and vegetation** - Biome-appropriate trees, flowers, and grass generate in the world
- **Better terrain** - Improved world generation with better caves, mountains, and ore distribution
- **Config file** - Change settings in `server.conf` instead of recompiling
- **Multithreaded chunk gen** - Chunk generation runs on a separate thread
- **Infinite block changes** - Optional unlimited building with dynamic memory allocation
- **Performance fixes** - Various optimizations for chunk streaming and packet handling

## Quick Start

You'll need to compile the server yourself. See the **Compilation** section below for instructions.

### Configuration

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

## Compilation

Before compiling, you'll need to dump registry data from a vanilla Minecraft server. On Linux, this can be done automatically using the `extract_registries.sh` script. Otherwise, the manual process is as follows: create a folder called `notchian` here, and put a Minecraft server JAR in it. Then, follow [this guide](https://minecraft.wiki/w/Minecraft_Wiki:Projects/wiki.vg_merge/Data_Generators) to dump all of the registries (use the _second_ command with the `--all` flag). Finally, run `build_registries.js` with either [bun](https://bun.sh/), [node](https://nodejs.org/en/download), or [deno](https://docs.deno.com/runtime/getting_started/installation/).

### Build Commands

- **Linux:** `gcc` + `./build.sh`
- **Windows (native):** MSYS2 MINGW64 shell, install `mingw-w64-x86_64-gcc`, run `./build.sh`
- **Windows (32-bit):** MSYS2 MINGW64 shell, install `mingw-w64-cross-gcc`, run `./build.sh --9x`
- **ESP32:** PlatformIO with ESP-IDF framework, clone this repo on top

## Configuration

Most settings are in `server.conf`. Key options:

- **Performance:** `chunk_cache_size`, `tick_interval`, `broadcast_all_movement`
- **Features:** `allow_chests`, `do_fluid_flow`, `enable_flight`, `allow_doors`
- **World:** `view_distance`, `world_seed`, `infinite_block_changes`

For embedded-specific options, check `include/globals.h`.

## Non-Volatile Storage

**PC:** World data auto-saves to `world.bin`.

**ESP32:** Set up LittleFS in PlatformIO and set `sync_world_to_disk = true` in `server.conf`.

## License

GPL-3.0 License - see [LICENSE](LICENSE).

## Acknowledgments

- Original [bareiron](https://github.com/p2r3/bareiron) by p2r3
- [cubiomes](https://github.com/Cubitect/cubiomes) for biome generation
- [Cosmopolitan Libc](https://github.com/jart/cosmopolitan) for cross-platform binaries

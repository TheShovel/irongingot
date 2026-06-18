# irongingot — LLM context. READ FIRST. UPDATE ON MAJOR CHANGES.

MC 1.21.8 (proto 772) server in C. Fork of bareiron. ~7MB RAM (musl static). Linux/Windows. No ESP32.

## Files

| File | Role |
|------|------|
| `src/main.c` | Entry, epoll loop, packet dispatch switch, server tick, chunk streamer, signals |
| `src/packets.c` | All MC protocol handlers (~170+ funcs) |
| `src/procedures.c` | Game logic: block changes, mob AI, combat, fluids, crafting, tick |
| `src/worldgen.c` | Terrain gen: Overworld/Nether/End, ores, caves, biomes, structures |
| `src/chunk_generator.c` | Chunk cache (LRU, 20 sections×4096 `uint16_t`), parallel gen via thread pool |
| `src/structures.c` | Tree/sapling placement |
| `src/config.c` | `server.conf` parser → `ServerConfig config` global |
| `src/serialize.c` | `world.json` save/load |
| `src/tools.c` | Network I/O (`send_all`/`recv_all`), packet framing, `fast_rand()`, favicon loader |
| `src/varnum.c` | VarInt/VarLong read/write |
| `src/crafting.c` | Crafting + furnace recipe lookup |
| `src/creative_mode.c` | Creative inventory UI (scroll list, item pick) |
| `src/special_block.c` | State hash table for doors/trapdoors/stairs/chests/fences/beds/furnaces/barrels/ender_chests/ladders |
| `src/mojang.c` | Skin fetch via Mojang API (libcurl, optional `MOJANG_SKIN_LOOKUP_AVAILABLE`) |
| `src/thread_utils.c` | pthread create with controlled stack sizes (128K default, 512K chunk, 256K pool) |
| `src/async.c` | Per-client async packet sender threads (priority queue) |
| `src/registries.c` | **Generated** — block palette, state tables, registry binary blobs |
| `src/generated_village_templates.c` | **Generated** — village building NBT compiled to C |
| `src/globals.c` | Global state init |
| `src/terminal_ui.c` | Terminal status panel + log |
| `src/noise/perlin.c` | Java-compatible Perlin noise (JavaRandom, OctavePerlinNoiseSampler) |
| `src/cubiomes/` | Git submodule — biome generation, structure finders |
| `third_party/cjson/` | Vendored cJSON (world.json) |
| `third_party/zlib/` | Vendored zlib (packet compression, bundled for musl/Windows) |
| `include/*.h` | Headers mirror src files |
| `server.conf` | Runtime config (INI-style) |
| `world.json` | Auto-saved world state |
| `build.sh` | Single binary build (gcc or musl-gcc) |
| `build_all.sh` | Full release packaging (glibc + musl + ARM64 + Windows .zips) |
| `rebuild.sh` | **One-command rebuild:** downloads server.jar, dumps registries, generates code, compiles |
| `build_registries.js` | Generate `registries.h` + `registries.c` from vanilla data |
| `build_village_templates.js` | Generate village templates from vanilla NBT |
| `extract_registries.sh` | Automates dumping from vanilla server.jar |
| `images/` | Screenshot, logo, background, title banner |
| `index.html` | Landing page |
| `MISSING_FEATURES.md` | Priority-ordered unimplemented features |
| `CHANGELOG.md` | Release history |

## Architecture

### Threads
| Thread | Work |
|--------|------|
| **Main** | epoll accept/read, packet dispatch, game tick (20 TPS), chunk streaming |
| **Async senders** (1/client) | Drain send queue per client (mutex + condvar) |
| **Thread pool** (N CPU cores) | Parallel chunk generation |
| **Mojang API** (optional) | Periodic skin fetch |

### Packet flow
```
recv → readVarInt(pid) → handlePacket() switch
  → cs_*() (modify state) → sc_*() (queue response) → async sender → send_all()
```

## Chunk Pipeline

Chunks are generated **synchronously on demand** by the chunk streamer thread (~1.7ms/chunk, 7× faster than original). A tiny LRU cache (24 entries, ~4MB) serves as scratch buffer between generation and network serialization — no async queue needed. Background worker threads opportunistically pre-generate chunks in a 3×3 area around each player.

Chunk flow: `streamChunksForPlayer()` → cache miss → `generate_chunk_data()` (sync, ~1.7ms) → serialize → `sc_chunkDataAndUpdateLight()` → send queue. View distance from `config.view_distance` (server.conf, clamped 2-32). Streamer sends up to 10 chunks per 20ms cycle.

## Key Datatypes

- **`PlayerData`** (globals.h): pos, yaw/pitch, health/hunger/sat, hotbar+36+armor+offhand+crafting inventory (uint16_t item IDs), ender chest, flags (sprint/sneak/fly/etc), dimension, spawn, open merchant, XP, portal coords
- **`CachedChunkData`** (chunk_generator.h): 20 sections × 4096 `uint16_t` blocks, biomes[20], LRU via access_count, lock-free `generating` flag
- **`MobData`** (globals.h): type, pos, move_delta, timers, profession, dimension. Max `MAX_MOBS`
- **`SpecialBlockEntry`** (special_block.h): hash table (MAX_SPECIAL_BLOCKS=8192), keyed by pos, value = `uint16_t` state bitfield
- **`BlockChange`** (globals.h): player edits {x,y,z,block,dimension}
- **`ProjectileData`** (globals.h): arrows + ender pearls {pos, vel, owner, damage, stuck}
- **`FluidUpdateEntry`** (globals.h): ring buffer for deferred water/lava flow

## Protocol

State machine: `Handshake → Status|Login → Configuration → Play`

Compression enabled after login (threshold from config). Packet framing: VarInt length + VarInt packet ID + payload.

## Block System

~337 block types as `uint16_t` IDs (`B_*` defines in registries.h). Interactive blocks (door/trapdoor/stair/fence/chest/furnace/barrel/bed/ender_chest) store state in `special_block.c` hash table as packed `uint16_t` bitfields.

## World Gen

Perlin noise (Java-compatible) for terrain height, detail, mountains. Cubiomes for biome assignment. Ores by Y-level + biome (grid-based: 4×4×4 precomputed density grid replaces per-block noise + 6-neighbor spread). Caves: 4×4×4 grid replaces per-block noise. Combined ~7× chunk gen speedup vs original. Structures: villages (5 styles×13 professions from NBT templates), dungeons (mossy+cobble, chests, spawners), mineshafts, strongholds (silverfish, portal), nether fortresses, trees (8 species).

Worldgen benchmark (bench_worldgen):

=== Before ===
y=[0,64):  ~2800-3000 us/section  (caves + ores + structures)
y=[64,80):  ~1059 us/section
y=80+:     ~310 us/section
Full chunk: 11.93 ms, 596 us/section

=== After (all optimizations) ===
y=[0,64):  ~900-980 us/section  (3.1× faster)
y=[64,80):  ~365 us/section  (2.9× faster)
y=80+:     ~280 us/section  (1.1× faster)
Full chunk: 1.80 ms, 90 us/section  (6.6× faster)

### Perf optimizations
- **Cave/ore density grids**: 4×4×4 precomputed in `buildChunkSection` → 128 `octave_sample()` calls instead of 8192 per section. Trilinear interpolation removes square artifacts.
- **Structure cell caches**: Per-section TLS caches for dungeon/stronghold cells → 1 `splitmix64` instead of 4096
- **y-range guards**: Skip function calls in `getTerrainAtFromCache` when y outside structure range
- **Lookup tables**: Biome height/scale via `biome_base[]`/`biome_scale[]` arrays replacing if-else chain
- **Minimal ore noise**: Single `octave_sample()` per block + switch-based neighbor check instead of 7 calls
- **Micro-optimizations**: Shift/mask ops in hot loop, trilerp grid lookup with bit ops, redundant dz check removed
- **Village footprint gate**: `getVillageBlockAt` skips template binary-search + terrain-fill for houses whose xz footprint the query block misses (~80% of village lookups).

## Game Tick (procedures.c:handleServerTick)
1. Weather → 2. Fluid queue → 3. Mob AI (move to nearest player, anger timers, sounds). Villagers beyond `MOB_DESPAWN_DISTANCE` (256) skip AI (frozen, not despawned). Mob AI optimized: same-block collision skip (~95% ticks skip bounding-box cascade), shared direction pre-compute for hostile AI (single sqrt/atan2 per mob vs per-branch duplication), mob-grid skip when stationary. → 4. Projectiles → 5. XP orbs → 6. Block tick (leaf decay, crops/wheat growth, cactus, fire) → 7. Player tick (fall dmg, suffocate, drown, hunger, regen, portal cooldown) → 8. Mob spawning → 9. Player list broadcast

Wheat growth uses a dedicated tracking list (`wheat_coords[]`/`wheat_count` in `special_block.c`) to avoid scanning the hash table. Both the hash table and the wheat tracking list grow dynamically — no fixed limits. All special blocks clean up their hash entries when broken to prevent table leaks.

## Inventory

46 internal slots: hotbar(9) + main(27) + armor(4) + offhand(1) + crafting(4) + result(1). Window types: player inventory, chest (27/54), crafting table (41), merchant (3). Slot mapping via `serverSlotToClientSlot()`/`clientSlotToServerSlot()`.

## Village House Rotation (worldgen.c)

Houses are placed at fixed offsets from the village center. They rotate so each front door faces the village center; all houses use the center biome style (prevents mixed taiga/birch edge houses). Helpers: `houseRotation()`, `rotLocal()`, type-aware `rotBlockDir()` (doors/stairs/trapdoors/beds/chests/horiz/fences only). Generated templates preserve stair shape + slab type.

## State Encoding (special_block.h)

Single `uint16_t` bits per block type:
- **Doors:** bit0=open, bit1=hinge, bits2-3=direction
- **Trapdoors:** bit0=open, bit1=half, bits2-3=direction
- **Stairs:** bits0-1=half, bits2-3=direction, bits4-6=shape
- **Slabs:** bits0-1=type (generated-template packing only)
- **Furnaces:** bits0-1=direction, bit2=lit
- **Chests/ender_chests:** bits0-1=direction
- **Barrels:** bits0-2=direction, bit3=open
- **Fences/glass panes:** bits0-3=N/E/S/W connections
- **Beds:** bits0-1=direction, bit2=head, bit3=occupied
- **Horizontal-facing (wall_torch/lectern/ladder):** bits0-1=direction (north/south/west/east)

## Config (server.conf → `ServerConfig config`)

Network: port, max_players, compression_threshold, network_timeout, mojang_api_timeout_ms
Game: gamemode(0-3), difficulty(0-3), view_distance, world_seed, rng_seed, mob_* params
Worldgen: chunk_size, terrain_base_height, cave_base_depth, biome_size
Perf: chunk_cache_size, max_block_changes, infinite_block_changes, tick_interval, disk_sync_interval
Features: sync_world_to_disk, do_fluid_flow, allow_chests, allow_doors, enable_flight, enable_commands, fetch_skins_from_mojang, safe_area_radius
Debug: log_unknown_packets, log_length_discrepancy, log_chunk_generation

## world.json format

`format_version`, `block_changes[]` (player edits), `players[]` (full state per player), `special_blocks[]` (interactive block states), `mobs[]` (persistent mobs). Auto-saves on `disk_sync_interval`.

## Build

| Target | Compiler | Libs |
|--------|----------|------|
| Linux glibc | gcc | system zlib + libcurl (opt) |
| Linux musl | musl-gcc | bundled zlib |
| Linux ARM64 musl | zig cc | bundled zlib |
| Windows | mingw-w64-gcc | bundled zlib + ws2_32 |

`./build.sh` (single), `./build_all.sh` (all targets + .zips).
**`./rebuild.sh`** — all-in-one: downloads server.jar → dumps registries → generates code → compiles.
Prereq: `include/registries.h` generated from vanilla server.jar via `extract_registries.sh` or `rebuild.sh` (needs Java 21+ and Node/Bun/Deno).

## Thread Safety

- **Main thread only:** special_blocks hash table, mob/player/projectile arrays, block_changes (mutex for snapshot copy).
- **Chunk cache:** mutex lock. `generating` flag is lock-free.
- **Fluid queue:** lock-free ring buffer with `volatile` head/tail.
- **Send queue:** per-client mutex + condvar.
- **Packet buffers:** TLS per thread.

## Build RAM targets
- glibc: ~30MB
- musl static: ~7MB
- ARM64 musl: ~7MB
- Windows: ~30MB

## Key missing features (MISSING_FEATURES.md)
Ghast/Blaze/MagmaCube/Shulker mobs, brewing/potions, enchanting/anvil, cow/pig/sheep/chicken breeding, item entities, redstone, online mode. /spawn command added. /commands fixed (Chat Command 0x09 handler).

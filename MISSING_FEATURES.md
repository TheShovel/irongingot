# Missing Features

This document lists features missing from irongingot compared to vanilla Minecraft 1.21.8, organized by priority and recommended implementation order.

---

## Priority Legend

- **Priority: HIGH** - Core gameplay features, essential for vanilla experience
- **Priority: MEDIUM** - Important features that enhance gameplay but aren't critical
- **Priority: LOW** - Nice-to-have features, aesthetic or advanced mechanics

---

## HIGH PRIORITY - Essential Core Gameplay

These features are fundamental to a complete Minecraft experience and should be implemented first.

### 1. Basic Missing Mobs (Nether/End completeness)

- **Ghast** (High) - Key Nether mob for atmosphere
- **Blaze** (High) - Required for blaze rods and Nether fortresses
- **Magma Cube** (High) - Common Nether mob
- **Shulker** (High) - Required for End content

---

## MEDIUM PRIORITY - Important Gameplay Enhancement

These features significantly improve gameplay but are not strictly required.

### 2. Brewing & Potions System

- **Brewing Stand** (Medium) - Core potion brewing block
- **Potions System** (Medium) - All potion effects (speed, strength, healing, etc.)
- **Tipped Arrows** (Low) - Advanced arrow variants

### 3. Enchanting System

- **Enchantment Tables** (Medium) - Core enchantment mechanics
- **Anvil UI** (Medium) - Repair and combine items
- **Smithing Table** (Low) - Trim armor (cosmetic)
- **Recipe Book** (Low) - Needs correct packet IDs for protocol 772. `0x6C` is `set_titles_animation` in this version.

### 4. Missing Mobs - Hostile

- **Zombie Villager** (Medium) - Village defense, cure gameplay
- **Husk** (Low) - Desert variant of zombie
- **Drowned** (Medium) - Underwater zombie variant
- **Wither Skeleton** (Medium) - Needed for Wither boss
- **Bogged** (Low) - Nether skeleton variant
- **Warden** (Low) - Deep dark mob
- **Breeze** (Low) - New 1.21 mob

### 5. Missing Mobs - Passive

- **Cow/Pig/Sheep/Chicken** (Medium) - Full passive mob behavior (breeding, drops)
- **Rabbit** (Low) - Small passive mob
- **Fox** (Low) - Woodland passive mob
- **Wolf** (Medium) - Taming mechanic

### 6. Missing Structures

- **Buried Treasure** (Medium) - Ocean explorer map content
- **Ruins (Surface)** (Low) - World generation variety
- **Igloo** (Low) - Small structure
- **Pillager Outpost** (Medium) - Badlands structure with loot

---

## LOW PRIORITY - Nice to Have & Advanced Features

These are advanced features or aesthetic improvements.

### 7. Redstone Components (Complex systems)

- **Redstone Dust** (Low) - Wiring (complex continuous updates)
- **Lever/Button/Pressure Plate** (Low) - Basic activation
- **Piston/Sticky Piston** (Low) - Block moving
- **Dispenser/Dropper** (Low) - Item dispensing
- **Redstone Torch** (Low) - Basic redstone
- **Powered/Detector Rail** (Low) - Minecart systems
- **Hopper** (Low) - Item transport
- **Comparator/Repeater/Observer** (Low) - Advanced redstone
- **Jukebox/Note Block** (Low) - Music/sound
- **Target Block** (Low) - New redstone component

### 8. Utility Blocks

- **Sign** (Low) - Text signs
- **Painting** (Low) - Decorative
- **Item Frame** (Low) - Item display
- **Flower Pot** (Low) - Plant decoration
- **Cauldron** (Low) - Water/mud mechanics

### 9. Utility Entities

- ~~**Item Entity** (Low) - Items on ground (may impact performance)~~ - ✅ Items drop with physics, pickup, and despawn
- **Minecart** (Low) - Minecart system
- **Boat** (Low) - Water transport
- **Armor Stand** (Low) - Decorative entity
- **Snowball/Egg** (Low) - Throwable items
- **Fireball/Shulker Bullet** (Low) - Projectile variety
- **Trident** (Low) - Complex projectile with mechanics
- **Fishing Bobber** (Low) - Fishing mechanic

### 10. Advanced Commands

- ~~`/gamerule` (Low) - Game rule modification~~ - ✅ 6 rules: doMobSpawning, keepInventory, doImmediateRespawn, naturalRegeneration, doDaylightCycle, doWeatherCycle
- ~~`/difficulty` (Low) - Difficulty setting~~ - ✅ peaceful|easy|normal|hard
- `/ban`, `/kick`, `/whitelist` (Low) - Admin tools (requires auth system)
- `/warp`, `/home`, `/spawn` (Low) - Teleportation commands

### 11. World Generation Enhancements

- **River Biomes** (Low) - River generation
- **Biome-specific Vegetation** (Low) - More tree/flower variety
- **Underground Lakes** (Low) - Cave decorations
- **Deepslate Layer** (Low) - Post-1.18 terrain

### 12. Combat Features

- **Shield Blocking** (Low) - Blocking mechanic

### 13. Visual/Aesthetic

- **Potion Particles** (Low) - Status effect particles
- **Mob Effect Particles** (Low) - Damage/interaction particles
- **Hurt Overlay** (Low) - Red screen on damage
- **Portal Particles** (Low) - Nether portal effect

### 14. Technical Features

- **Online Mode** (Low) - Mojang authentication (requires networking work)
- **Resource Pack** (Low) - Custom textures
- **Data Packs** (Low) - Custom content loading

---

## Implementation Order Summary

**Phase 1 (High Priority):**
1. Nether/End mobs (Blaze, Ghast, Magma Cube, Shulker)

**Phase 2 (Medium Priority):**
2. Brewing & Potions
3. Enchanting + Anvil
4. Missing hostile mobs (Wither Skeleton, Drowned, Zombie Villager, etc.)
5. Missing passive mobs (Cow, Pig, Sheep, Chicken, Wolf, etc.)
6. Missing structures (Buried Treasure, Pillager Outpost, etc.)

**Phase 3 (Low Priority):**
7. Redstone components
8. Utility blocks and entities
9. Advanced commands
10. World generation improvements
11. Shield blocking
12. Visual effects and particles
13. Technical features

---

## Notes

- Irongingot is designed as a minimalist server targeting low-spec hardware (~7MB RAM)
- Item Entities are implemented with physics, pickup, and 5-minute despawn
- Complex systems (Redstone, minecarts) require significant networking and continuous updates
- Consider memory/performance impact when implementing cosmetic features

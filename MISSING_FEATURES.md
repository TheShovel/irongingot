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

### 2. Basic Missing Structures (Dimension completion)

- ~~**Nether Fortresses** (High)~~ - ✅ Classic Nether structure with loot
- ~~**Village Variants** (High)~~ - ✅ Full village generation with roads, wells, bells

### 3. Villager Trading

- ~~**Trading Menu** (High)~~ - ✅ Villager trades for emeralds
- ~~**Job Sites** (Medium)~~ - ✅ Workstation-based villager professions

### 4. Essential Containers

- ~~**Ender Chest** (High)~~ - ✅ Cross-dimension storage
- ~~**Barrel** (Medium)~~ - ✅ Simple storage alternative to chests

### 5. Basic Commands

- ~~`/gamemode` (High)~~ - ✅ Runtime game mode switching for admins
- ~~`/tp` (High)~~ - ✅ Teleport command for server management
- ~~`/give` (High)~~ - ✅ Essential admin command

---

## MEDIUM PRIORITY - Important Gameplay Enhancement

These features significantly improve gameplay but are not strictly required.

### 6. Brewing & Potions System

- **Brewing Stand** (Medium) - Core potion brewing block
- **Potions System** (Medium) - All potion effects (speed, strength, healing, etc.)
- **Tipped Arrows** (Low) - Advanced arrow variants

### 7. Enchanting System

- **Enchantment Tables** (Medium) - Core enchantment mechanics
- **Anvil UI** (Medium) - Repair and combine items
- **Smithing Table** (Low) - Trim armor (cosmetic)

### 8. Missing Mobs - Hostile

- **Zombie Villager** (Medium) - Village defense, cure gameplay
- **Husk** (Low) - Desert variant of zombie
- **Drowned** (Medium) - Underwater zombie variant
- **Wither Skeleton** (Medium) - Needed for Wither boss
- **Bogged** (Low) - Nether skeleton variant
- **Warden** (Low) - Deep dark mob
- **Breeze** (Low) - New 1.21 mob

### 9. Missing Mobs - Passive

- **Cow/Pig/Sheep/Chicken** (Medium) - Full passive mob behavior (breeding, drops)
- **Rabbit** (Low) - Small passive mob
- **Fox** (Low) - Woodland passive mob
- **Wolf** (Medium) - Taming mechanic

### 10. Missing Structures

- **Buried Treasure** (Medium) - Ocean explorer map content
- **Ruins (Surface)** (Low) - World generation variety
- **Igloo** (Low) - Small structure
- **Pillager Outpost** (Medium) - Badlands structure with loot

### 11. Missing Commands

- ~~`/time` (Medium)~~ - ✅ Time control for admins
- ~~`/weather` (Medium)~~ - ✅ Weather control
- ~~`/kill` (Medium)~~ - ✅ Kill command
- ~~`/heal` (Low)~~ - ✅ Heal command
- ~~`/feed` (Low)~~ - ✅ Feed command

### 12. Player Mechanics

- ~~**Sleeping** (Medium)~~ - ✅ Skip night with beds
- ~~**Spawn Point Setting** (Medium)~~ - ✅ Beds set respawn point
- ~~**Armor Protection** (Medium)~~ - ✅ Armor damage reduction

---

## LOW PRIORITY - Nice to Have & Advanced Features

These are advanced features or aesthetic improvements.

### 13. Redstone Components (Complex systems)

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

### 14. Utility Blocks

- **Sign** (Low) - Text signs
- **Painting** (Low) - Decorative
- **Item Frame** (Low) - Item display
- **Flower Pot** (Low) - Plant decoration
- **Cauldron** (Low) - Water/mud mechanics
- **Composter** (Low) - Bone meal crafting
- **Bell** (Low) - Village feature
- **Lantern/Campfire** (Low) - Light sources

### 15. Utility Entities

- **Item Entity** (Low) - Items on ground (may impact performance)
- **Minecart** (Low) - Minecart system
- **Boat** (Low) - Water transport
- **Armor Stand** (Low) - Decorative entity
- **Snowball/Egg** (Low) - Throwable items
- **Fireball/Shulker Bullet** (Low) - Projectile variety
- **Trident** (Low) - Complex projectile with mechanics
- **Fishing Bobber** (Low) - Fishing mechanic

### 16. Advanced Commands

- `/gamerule` (Low) - Game rule modification
- `/difficulty` (Low) - Difficulty setting
- `/ban`, `/kick`, `/whitelist` (Low) - Admin tools (requires auth system)
- `/warp`, `/home`, `/spawn` (Low) - Teleportation commands

### 17. World Generation Enhancements

- **Ocean Variants** (Low) - Full ocean biomes (requires rewrite)
- **River Biomes** (Low) - River generation
- **Biome-specific Vegetation** (Low) - More tree/flower variety
- **Underground Lakes** (Low) - Cave decorations
- **Deepslate Layer** (Low) - Post-1.18 terrain

### 18. Combat Features

- **Critical Hits** (Low) - Damage multiplier
- **Shield Blocking** (Low) - Blocking mechanic

### 19. Visual/Aesthetic

- **Potion Particles** (Low) - Status effect particles
- **Mob Effect Particles** (Low) - Damage/interaction particles
- **Hurt Overlay** (Low) - Red screen on damage
- **Portal Particles** (Low) - Nether portal effect

### 20. Technical Features

- **Online Mode** (Low) - Mojang authentication (requires networking work)
- **Resource Pack** (Low) - Custom textures
- **Plugin Channel API** (Low) - Mod integration
- **Data Packs** (Low) - Custom content loading

---

## Implementation Order Summary

**Phase 1 (High Priority):**
1. Nether Fortresses + Blaze/Ghast mobs
2. Villager Trading / Job Sites
3. ~~End Chest + Barrel~~ ✅ Completed
4. ~~Basic admin commands (`/gamemode`, `/tp`, `/give`)~~ ✅ Completed
5. ~~Sleeping + Spawn Point mechanics~~ ✅ Completed

**Phase 2 (Medium Priority):**
6. Brewing & Potions
7. Enchanting + Anvil
8. Missing mobs (Wither Skeleton, Drowned, Wolf, etc.)
9. ~~Armor protection~~ ✅ Completed

**Phase 3 (Low Priority):**
10. Redstone components
11. Utility blocks and entities
12. Advanced commands
13. Structure improvements (ocean monuments, mansions)
14. Visual effects and particles

---

## Notes

- Irongingot is designed as a minimalist server targeting low-spec hardware (~7MB RAM)
- Some limitations (like Item Entities) may be intentionally omitted for performance
- Complex systems (Redstone, minecarts) require significant networking and continuous updates
- Consider memory/performance impact when implementing cosmetic features

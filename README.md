# PvP Life (Comprehensive Autonomous World PvP & Duel System)

![PvP Life Banner](assets/banner.png)

Repository/module name: `mod-playerbots-pvp-life`  
Created by **iCore**. Incorporates 100% of capabilities from `mod-playerbots-world-pvp`.

An AzerothCore + mod-playerbots module focused on making the open world feel alive through persistent, configurable bot PvP, world skirmishes, reactive raid defenses, and capital gate duel rituals.

**Single Installed Module Requirement:** Users install only `mod-playerbots-pvp-life` alongside AzerothCore and `mod-playerbots`. `mod-playerbots-world-pvp` is completely unnecessary and should not be installed or enabled.

---

### Key Subsystems & Features

- **Persistent World PvP & Frontlines:** Keeps outdoor PvP skirmishes actively running across 35+ configured hotspots throughout Eastern Kingdoms, Kalimdor, Outland, and Northrend.
- **Reactive Raid Defense:** Automatically detects qualifying outdoor player-led invasion raids (20+ members, PvP-flagged leader), staging an opposing faction defense force 120 yards away in 9 formation lanes at 4-yard spacing before advancing 25 yards to engage.
- **Capital Gate Duel Arenas:** Near-continuous bot-vs-bot duel activity outside Stormwind and Orgrimmar gates. Both `mod-playerbots-pvp-life` and `mod-playerbots-city-life` run gate duels concurrently without interference.
- **Bot-to-Player Duel Challenges:** Bots in duel arenas challenge nearby real players with configurable level-difference checks and strict anti-spam cooldowns.
- **For the Horde & For the Alliance Campaigns:** Faction pressure pushes towards enemy capitals with organic defender yells and world chat reactions.
- **Mutual Anti-Stealing Reservation Barrier:** Standalone-safe, thread-safe shared reservation registry (`PlayerbotsLife::ReservationRegistry`) ensuring bots borrowed by City Life are never stolen or commanded by PvP Life, and vice versa.
- **Conservative Balanced Population Budgets:** Default borrowing budget of 10% / 120 bots max (configured for a 1,000-bot server) guarantees that ≥ 80% (800+ bots) of random bots remain completely free to roam, quest, level, and run dungeons.
- **Zero Bot Tampering Invariant:** Bots are borrowed strictly AS-IS from the natural Playerbot pool with zero artificial level boosts, zero talent changes, and zero stat inflation.
- **Zero GM Spam Protection:** Game Masters and administrators are strictly exempt from unsolicited duel challenges, yells, or world announcements.
- **Silent-by-Default Chatter & Announcements:** All automated bot chatter, yells, and server-wide announcements default to OFF (`0`) in config for a clean, natural player atmosphere.
- **Instant Disengagement on Invite or Logout:** Combatants and duelers immediately drop PvP Life activities if invited to a player group or logged out.
- **Shared Duel Staging Lattice (20yd Spacing):** Prevents duel area collisions outside Stormwind and Orgrimmar gates when running concurrently with `mod-playerbots-city-life`.
- **Asynchronous Non-Blocking Audit Logger:** Dedicated worker thread writing to `Logs/pvplife_audit.log` tracking `MOVE`, `DUEL`, `START`, `MEMBER`, `DUEL_QUEUE`, `END`, `RETURN`, `REACTIVE_START`, and `CONFIG` events with zero world-thread blocking I/O.
- **100% Backward-Compatible `.wpvp` GM Command Aliases:** All 14 `.wpvp` commands from the legacy world PvP module work natively as aliases within PvP Life.

---

## Requirements

- [`mod-playerbots/azerothcore-wotlk`, Playerbot branch](https://github.com/mod-playerbots/azerothcore-wotlk/tree/Playerbot)
- [`mod-playerbots/mod-playerbots`, master branch](https://github.com/mod-playerbots/mod-playerbots)
- C++17-capable build environment (used by AzerothCore).
- MySQL database (AzerothCore baseline).

---

## Installation & Setup

1. Place only `mod-playerbots-pvp-life` into your `modules/` folder:
   ```text
   azerothcore-wotlk/modules/mod-playerbots-pvp-life
   ```
2. Apply the schema and seed data to your **WORLD** database:
   ```bash
   mysql -u root -p acore_world < data/sql/manual/world_pvp_life.sql
   ```
   *(If upgrading from customized legacy `playerbot_world_pvp_hotspot` data, run `data/sql/manual/import_legacy_world_pvp_hotspots.sql`).*
3. Re-run CMake and rebuild the server. Current AzerothCore automatically discovers `mod-playerbots-pvp-life` and its `.conf.dist`.
4. Copy `conf/mod_playerbots_pvp_life.conf.dist` to `mod_playerbots_pvp_life.conf` and adjust settings as desired.
5. Start worldserver and look for `>> Loaded mod-playerbots-pvp-life` in your startup log.

---

## Coexistence with `mod-playerbots-city-life`

`mod-playerbots-pvp-life` and `mod-playerbots-city-life` are architected for seamless, zero-conflict simultaneous execution:
- **Zero Feature Suppression:** Neither module disables any feature for the other.
- **Concurrent Capital Gate Duels:** Both modules host dueling outside city gates simultaneously. Each module pairs only its own reserved bots.
- **Natural Hostility:** Standard core PvP hostility rules apply everywhere.
- **Candidate Scanning Isolation:** During bot recruitment, each module verifies `!PlayerbotsLife::IsReserved(guid)` before claiming participants.
- **Thread-Safe Shared Reservations:** `src/LifeBotReservation.h` provides atomic module flags and mutex-guarded ownership checks that resolve to the same process-wide singleton whether compiled together or standalone.

---

## GM Commands Reference

Both `.pvplife` and `.wpvp` roots are fully supported:

### Status & Diagnostics
- `.pvplife status` (or `.wpvp status`): Overview of active events, queues, budgets, and participant counts.
- `.pvplife scan [minLvl] [maxLvl]` (or `.wpvp scan`): Scans online bot population, faction distribution, and hotspot readiness.
- `.pvplife compat` (or `.wpvp compat`): Shows detected active Life modules and reservation statistics.
- `.pvplife reservations`: Breakdown of reserved bots by owning module (`CityLife`, `PvPLife`, `Artisans`).
- `.pvplife reload`: Reloads configuration, zone cache, and chat tables.

### Reactive Defense Management
- `.pvplife response status`: Shows pending raid responses, targets, and staging countdowns.
- `.pvplife response stop <id|all>`: Cancels pending reactive defenses.

### Hotspot & Event Management
- `.pvplife start <zoneName>` (or `.wpvp start <spot>`): Force-starts a configured hotspot.
- `.pvplife stop <id|all>` (or `.wpvp stop <id|all>`): Stops one or all active events and returns bots.
- `.pvplife zone list` (or `.wpvp spot list`): Lists all configured hotspots.
- `.pvplife zone create <name> <type> <attacker> <defender> <minLvl> <maxLvl>` (or `.wpvp spot create`): Creates a hotspot at current GM position.
- `.pvplife zone rally <name>` (or `.wpvp spot rally`): Saves current GM position as attacker rally point.
- `.pvplife zone target <name>` (or `.wpvp spot target`): Saves current GM position as defender target point.
- `.pvplife zone counts <name> <attMin> <attMax> <defMin> <defMax>` (or `.wpvp spot counts`): Tunes participant counts.
- `.pvplife zone duration <name> <minM> <maxM>` (or `.wpvp spot duration`): Tunes event duration.
- `.pvplife zone enable <name> <0|1>` (or `.wpvp spot enable`): Enables or disables a hotspot.

---

## Credits & Attribution

- **PvP Life Core & Architecture:** Created by **iCore**.
- **World PvP Hotspot Seeds & Reactive Defense Logic:** Ported and consolidated from `mod-playerbots-world-pvp`.
- **Cross-Module Reservation Protocol:** Designed for the Playerbots Life ecosystem (`mod-playerbots-city-life`, `mod-playerbots-pvp-life`, `mod-playerbots-artisans`).

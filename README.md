# PvP Life (Comprehensive Autonomous World PvP & Duel System)

![PvP Life Banner](assets/banner.png)

Repository/module name: `mod-hearthside-pvp`, a fork of `mod-playerbots-pvp-life` (config keys, conf file name,
GM commands and log tags keep the upstream names).  
Created by **iCore**. Incorporates 100% of capabilities from `mod-playerbots-world-pvp`.

An AzerothCore + mod-playerbots module focused on making the open world feel alive through persistent, configurable bot PvP, world skirmishes, reactive raid defenses, and capital gate duel rituals.

**Single Installed Module Requirement:** Users install only this module alongside AzerothCore and `mod-playerbots`. `mod-playerbots-world-pvp` is completely unnecessary and should not be installed or enabled.

---

### Key Subsystems & Features

- **Persistent World PvP & Frontlines:** Keeps outdoor PvP skirmishes actively running across 35+ configured hotspots throughout Eastern Kingdoms, Kalimdor, Outland, and Northrend.
- **Reactive Raid Defense:** Automatically detects qualifying outdoor player-led invasion raids (20+ members, PvP-flagged leader, outside their own faction's territory and outside Wintergrasp while its battle is on), staging an opposing faction defense force 120 yards away in rows of 9 lanes at 4-yard spacing before it advances on the raid leader.
- **Capital Gate Duel Arenas:** Near-continuous bot-vs-bot duel activity outside Stormwind and Orgrimmar gates. This module and `mod-hearthside-city` (City Life) run gate duels concurrently without interference.
- **Bot-to-Player Duel Challenges:** Bots in duel arenas challenge nearby real players of their own faction with configurable level-difference checks and strict anti-spam cooldowns (shared with City Life). Players who are dead, mounted, on a taxi or vehicle, AFK, DND, trading or in combat are left alone.
- **For the Horde & For the Alliance Campaigns:** Faction pressure pushes towards enemy capitals with organic defender yells and world chat reactions.
- **Mutual Anti-Stealing Reservation Barrier:** Standalone-safe, thread-safe shared reservation registry (`PlayerbotsLife::ReservationRegistry`) ensuring bots borrowed by City Life are never stolen or commanded by PvP Life, and vice versa.
- **Conservative Balanced Population Budgets:** Default borrowing budget of 10% / 120 bots max (configured for a 1,000-bot server) guarantees that ≥ 80% (800+ bots) of random bots remain completely free to roam, quest, level, and run dungeons.
- **Zero Bot Tampering Invariant:** Bots are borrowed strictly AS-IS from the natural Playerbot pool with zero artificial level boosts, zero talent changes, and zero stat inflation.
- **Zero GM Spam Protection:** Game Masters and administrators are strictly exempt from unsolicited duel challenges, yells, or world announcements.
- **Silent-by-Default Chatter & Announcements:** All automated bot chatter, yells, and server-wide announcements default to OFF (`0`) in config for a clean, natural player atmosphere.
- **Instant Disengagement on Invite or Logout:** Combatants and duelers immediately drop PvP Life activities if invited to a player group or logged out.
- **Shared Duel Staging Lattice (20yd Spacing):** Prevents duel area collisions outside Stormwind and Orgrimmar gates when running concurrently with `mod-hearthside-city`.
- **Asynchronous Non-Blocking Audit Logger:** Dedicated worker thread writing to `Logs/pvplife_audit.log` tracking `CONFIG`, `START`, `END`, `MOVE`, `DUEL_QUEUE`, `DUEL`, `REACTIVE_START` and `ZONE_EDIT` (GM zone edits) records with zero world-thread blocking I/O.
- **Backward-Compatible `.wpvp` GM Command Alias:** `.wpvp` accepts every `.pvplife` subcommand, with `spot` as the legacy name for `zone`.

---

## Requirements

- [`mod-playerbots/azerothcore-wotlk`, Playerbot branch](https://github.com/mod-playerbots/azerothcore-wotlk/tree/Playerbot)
- [`mod-playerbots/mod-playerbots`, master branch](https://github.com/mod-playerbots/mod-playerbots)
- C++17-capable build environment (used by AzerothCore).
- MySQL database (AzerothCore baseline).

---

## Installation & Setup

1. Place `mod-hearthside-pvp` into your `modules/` folder:
   ```text
   azerothcore-wotlk/modules/mod-hearthside-pvp
   ```
2. Nothing to apply by hand: AzerothCore's updater applies `data/sql/db-world/base/pvp_life.sql`
   to the **WORLD** database on the next worldserver start (`Updates.EnableDatabases` must include
   the world database). The file is idempotent and keeps hotspots you have edited or disabled.
   *(If upgrading from customized legacy `playerbot_world_pvp_hotspot` data, run `data/sql/manual/import_legacy_world_pvp_hotspots.sql` after that first start: legacy rows overwrite the seeded hotspots of the same name.)*
3. Re-run CMake and rebuild the server. Current AzerothCore automatically discovers the module and its `.conf.dist`.
4. Copy `conf/mod_playerbots_pvp_life.conf.dist` to `mod_playerbots_pvp_life.conf` and adjust settings as desired.
5. Start worldserver and look for `>> Loaded mod-playerbots-pvp-life` in your startup log.

---

## Coexistence with `mod-hearthside-city` (City Life)

PvP Life and City Life are architected for seamless, zero-conflict simultaneous execution:
- **Zero Feature Suppression:** Neither module disables any feature for the other.
- **Concurrent Capital Gate Duels:** Both modules host dueling outside city gates simultaneously. Each module pairs only its own reserved bots.
- **Natural Hostility:** Standard core PvP hostility rules apply everywhere.
- **Candidate Scanning Isolation:** During bot recruitment, each module verifies `!PlayerbotsLife::IsReserved(guid)` before claiming participants.
- **Thread-Safe Shared Reservations:** `src/LifeBotReservation.h` provides atomic module flags and mutex-guarded ownership checks backed by one registry per process. Check `.pvplife compat`: every Life module must report the same protocol version and registry address (relevant when building with `MODULES=dynamic`).

---

## GM Commands Reference

Both `.pvplife` (GM level) and `.wpvp` (administrator level) roots take every subcommand below; under `.wpvp`, `spot` stands for `zone`.

### Status & Diagnostics
- `.pvplife status` (or `.wpvp status`): Overview of active events, queues, budgets, and participant counts.
- `.pvplife scan [minLvl] [maxLvl]` (or `.wpvp scan`): Scans online bot population, faction distribution, and hotspot readiness.
- `.pvplife compat` (or `.wpvp compat`): Shows detected active Life modules and reservation statistics.
- `.pvplife reservations`: Breakdown of reserved bots by owning module (`CityLife`, `PvPLife`, `Artisans`).
- `.pvplife reload`: Re-applies the loaded configuration, re-checks the tables and re-reads zones and chat lines. To pick up edits to the conf file, use `.reload config` (which does all of this too).
- `.pvplife debug [status|validate|bot <guid|name>]`: Internal counters, a table check, or one bot's state.

### Reactive Defense Management
- `.pvplife response status`: Shows pending raid responses, targets, and staging countdowns.
- `.pvplife response stop <id|all>`: Cancels pending reactive defenses.

### Hotspot & Event Management
- `.pvplife start <zoneName>` (or `.wpvp start <spot>`): Force-starts a configured hotspot.
- `.pvplife stop <id|all>` (or `.wpvp stop <id|all>`): Stops one or all active events and returns bots. A stopped duel zone restarts on its own while `PvPLife.Duel.AlwaysActive = 1`; disable the zone to keep it off.
- `.pvplife zone list` (or `.wpvp spot list`): Lists all configured hotspots.
- `.pvplife zone create <name> <type> <attacker> <defender> <minLvl> <maxLvl>`: Creates a hotspot at current GM position, disabled until you set rally and target and enable it. `<type>`: `skirmish`, `duel`, `forthehorde`, `forthealliance`; teams: `alliance`, `horde`, `any`.
- `.wpvp spot create <name> <attacker> <defender> <minLvl> <maxLvl>`: The legacy form: the type is `duel` when both teams are the same faction, else `skirmish`.
- `.pvplife zone rally <name>` (or `.wpvp spot rally`): Saves current GM position as attacker rally point.
- `.pvplife zone target <name>` (or `.wpvp spot target`): Saves current GM position as defender target point.
- `.pvplife zone counts <name> <attMin> <attMax> <defMin> <defMax>` (or `.wpvp spot counts`): Tunes participant counts.
- `.pvplife zone duration <name> <minM> <maxM>` (or `.wpvp spot duration`): Tunes event duration.
- `.pvplife zone weight <name> <weight>` / `zone cooldown <name> <seconds>`: Selection weight (0 = never picked automatically) and restart cooldown.
- `.pvplife zone challenge <name> <0|1>` / `zone chat <name> <0|1>`: Real-player duel challenges and defender chatter for that zone.
- `.pvplife zone enable <name> <0|1>` (or `.wpvp spot enable`): Enables or disables a hotspot.

A `PvPLife.Zone.<name>.*` key in the conf overrides the stored enabled flag and population, GM edits included; the commands say so when one is set.

---

## Credits & Attribution

- **PvP Life Core & Architecture:** Created by **iCore**.
- **World PvP Hotspot Seeds & Reactive Defense Logic:** Ported and consolidated from `mod-playerbots-world-pvp`.
- **Cross-Module Reservation Protocol:** Designed for the Playerbots Life ecosystem (City Life, PvP Life, `mod-playerbots-artisans`).

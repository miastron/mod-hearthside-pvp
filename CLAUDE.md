# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

An AzerothCore (WotLK 3.3.5a, `mod-playerbots/azerothcore-wotlk` Playerbot branch) worldserver
module that borrows idle playerbots for world PvP skirmishes, capital-gate duels, faction campaigns
and reactive raid defense. It is a fork of upstream `mod-playerbots-pvp-life` (by iCore), being
rebranded as `mod-hearthside-pvp`. The rename is partial and deliberate so far:

- The folder name drives AzerothCore's generated loader symbol, so the entry point in
  `src/playerbots_pvp_life_loader.cpp` must stay `Addmod_hearthside_pvpScripts()`.
- Source files, config keys (`PvPLife.*`), the conf file name (`mod_playerbots_pvp_life.conf.dist`),
  log tags (`[PvPLife]`), GM commands and the `>> Loaded mod-playerbots-pvp-life` startup line all
  keep the upstream name. Don't rename them piecemeal.

## Build and test

No standalone build, no tests, no lint config. `CMakeLists.txt` is a comment: AzerothCore
auto-discovers `src/` and `conf/*.conf.dist` when the repo sits at
`azerothcore-wotlk/modules/mod-hearthside-pvp`. Compiling needs AzerothCore's headers and deps, so it
happens inside a full core build, not here. Verify runtime behavior in-game with `.pvplife status`,
`.pvplife scan`, `.pvplife reservations`, and `Logs/pvplife_audit.log`.

## Database

World DB only. `data/sql/db-world/base/pvp_life.sql` is applied by AzerothCore's updater and
re-applied whenever its hash changes, so it must stay idempotent: `CREATE TABLE IF NOT EXISTS`, and
`INSERT IGNORE` keyed on `uq_pvp_life_zone_name` so operator-edited hotspots survive. Edit that file
in place rather than adding dated update files. `data/sql/manual/` holds only the one-off legacy
`playerbot_world_pvp_hotspot` import.

Tables: `pvp_life_zone` (hotspots; `activity_type` 0=Skirmish 1=Duel 2=ForTheHorde 3=ForTheAlliance,
teams 0=Alliance 1=Horde 2=Any) and `pvp_life_chat`. `Manager::VerifyDatabase()` disables the module
if either is missing. Runtime zone edits from GM commands go through async queries
(`QueueZoneMutation`, `_queryProcessor`/`_transactionProcessor`); don't add blocking DB calls on the
world thread.

## Architecture

- `src/mod_playerbots_pvp_life.cpp`: script registration only. A WorldScript forwards config
  load/update/shutdown to the singleton; PlayerScript (logout) and GroupScript (a real player joins a
  bot's group) trigger instant disengagement; CommandScript routes `.pvplife` and `.wpvp` (legacy
  aliases from `mod-playerbots-world-pvp`).
- `src/PvPLifeMgr.{h,cpp}`: `PvPLife::Manager` (`sPvPLifeMgr`) holds nearly all logic and state in
  one ~4k-line file. `Manager::Update` runs every world tick: async callbacks, admission grants,
  then per-tick work queues (`_moveQueue`, `_duelQueue`, `_engagementQueue`, `_chatQueue`) and,
  every `_tickSeconds`, the management pass (start/expire events, reactive raid detection, duel
  maintenance, real-player challenges). Bots are released through a phased
  `_releasingParticipants` queue that runs even while disabled or reloading, restoring captured AI
  state (`AIStateJournal`). `_maxChangesPerTick` throttles how many bots change state per tick.
- `src/PvPLifeTypes.h`: the plain data structs (`Zone`, `ActiveEvent`, `Participant`, queue steps)
  and enums.
- `src/LifeBotReservation.h`: header-only, process-wide registry shared with sibling "Life" modules
  (`mod-hearthside-city`, `mod-playerbots-artisans`). It provides lease tokens (generation-checked,
  Provisional → Active → Releasing → Released), duel staging slots on a shared 20yd lattice, the
  real-player challenge cooldowns, and admission requests. Every queued step carries a `LeaseToken`
  and re-validates it before acting (`CanExecuteStep`, `IsOwnedActor`), because the other module may
  have claimed the bot in between. **This file must stay byte-identical to the copy in
  `../mod-hearthside-city/src/`** (mod-playerbots-artisans includes City's copy directly). Bump
  `LIFE_BOT_RESERVATION_PROTOCOL_VERSION` on any change: `AddPvPLifeScripts()` calls
  `CheckProtocolVersion()`, which logs an error when another Life module registered a different
  version. Change both repos together.
- `src/PvPLifeAudit.h`: async audit logger (`sPvPLifeAudit`) with its own worker thread writing
  `Logs/pvplife_audit.log`; world-thread code only enqueues.
- `src/PvPLifeDuelSelection.h`, `src/PvPLifeDuelRequestPolicy.h`: small pure-logic helpers for duel
  pairing and request rules.
- `../mod-hearthside-city/src/LifeMotion.h` (City owns it; `PvPLifeMgr.h` includes it by relative
  path and `#error`s without it, so this module now needs City checked out beside it):
  `LifeMotion::Launch` is the validated jump/curve move under duel footwork (`FootworkStep`,
  `FootworkBound` is its `Bound`), and the antic `Routine`s are the idle duel-ground antics
  (`UpdateAntics` every ~100 ms, `PvPLife.Antics.*`, `.pvplife debug antic <name> <antic>`). Change
  it in the City repo; it is not copied here.

## Cross-module integration

- **Bot detection:** this core has no `WorldSession::IsBot()`. Use `PvPLife::IsBotSession(player)`
  (account-based), not `IsRealPlayer()` alone: mod-playerbots attaches its AI after
  `OnPlayerLogin`, so a logging-in bot otherwise reads as human.
- **Archetype filter:** if `hs_archetype.h` from `../mod-hearthside-chat` is on the include path,
  `PVPLIFE_HAS_ARCHETYPES` is defined and `PvPLife.Bots.Archetypes` restricts which bots may be
  borrowed (`Manager::FitsArchetype`). Without that module every bot fits; keep the code compiling
  both ways.
- Bots are borrowed as-is (no level, talent or gear changes), GMs are never challenged or yelled at,
  and all chatter and announcements default to off in config. Preserve these invariants.

## Config

`conf/mod_playerbots_pvp_life.conf.dist` documents every option. A new option needs: a documented
entry there, a member in `PvPLifeMgr.h`, a read in `Manager::LoadConfig()`, and (if relevant) a field
in `PrintStatus`. Several bot-account defaults are imported from mod-playerbots' config
(`ImportPlayerbotDefaults`) when `PvPLife.Bots.UsePlayerbotConfig` is on.

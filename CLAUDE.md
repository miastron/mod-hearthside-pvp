# mod-hearthside-pvp

AzerothCore (WotLK 3.3.5a) module that borrows idle playerbots for world PvP skirmishes,
capital-gate duels, faction campaigns and reactive raid defense. Fork of upstream
`mod-playerbots-pvp-life` (iCore).

## The Hearthside collection

| Module | Role |
|---|---|
| `mod-hearthside-life` | Coordinator: the shared `Life*.h` headers and their state. City and PvP require it. |
| `mod-hearthside-chat` | All bot speech. Talks to Life through `hs_life.*`; never included by the others. |
| `mod-hearthside-city` | Bots as city residents (fork of `mod-playerbots-city-life`). |
| `mod-hearthside-pvp` | Bots in world PvP and duels (this repo). |

## Build

No standalone build, no tests. Compiles into AzerothCore's `modules` target (auto-discovered;
`CMakeLists.txt` is only a comment); building, restarting and MySQL happen on the test realm (see
`../mod-hearthside-chat/CLAUDE.md`). Label any runtime claim you could not exercise as unverified.
In game: `.pvplife status|scan|reservations` and `Logs/pvplife_audit.log`.

- The loader `Addmod_hearthside_pvpScripts()` (`src/playerbots_pvp_life_loader.cpp`) is named after
  the folder; renaming one means renaming the other.
- A new `.cpp` forces a CMake re-run and a full rebuild of `modules`; a new header does not.
- Everything else keeps upstream names on purpose (`PvPLife.*` keys,
  `mod_playerbots_pvp_life.conf.dist`, `[PvPLife]`, `.pvplife`, the `>> Loaded` line). Don't rename
  piecemeal.

## Shared Life headers

They are `../mod-hearthside-life`'s, included as `../../mod-hearthside-life/src/<header>`;
`PvPLifeMgr.h` `#error`s without that module. There is no copy here. Change them in Life and build
City and PvP. PvP uses: `LifeBotReservation.h` (leases, duel staging slots, challenge cooldowns),
`LifeMotion.h` (duel footwork, antics), `LifeMedic.h`, `LifeBots.h` (bot filter, budget, strategy
journal, `HoldLevels`), `LifePresence.h`, `LifeArrival.h`, `LifeAudit.h`.

## Architecture

- `src/mod_playerbots_pvp_life.cpp`: registration only. Logout and a real player joining a bot's
  group disengage at once; `.pvplife` and legacy `.wpvp` commands.
- `src/PvPLifeMgr.{h,cpp}`: `PvPLife::Manager` (`sPvPLifeMgr`) holds nearly all logic. `Update`
  runs async callbacks, admission grants and the work queues (`_moveQueue`, `_duelQueue`,
  `_engagementQueue`) each tick, and the management pass every `_tickSeconds`.
- **Leases:** every queued step carries a `LeaseToken` and re-validates it (`CanExecuteStep`,
  `IsOwnedActor`): another module may have claimed the bot in between.
- **Release** is phased (`_releasingParticipants`, runs even while disabled) and restores what the
  module changed (`Participant::Journal`); a bot a real player can see walks off.
- Events are sized from the zone row; nearby players only affect start priority. A watched duel
  ground winds down one bot at a time (`PvPLife.Duel.WindDown.*`).
- `PvPLifeTypes.h` (structs, enums), `PvPLifeDuelSelection.h`/`PvPLifeDuelRequestPolicy.h` (pure
  duel-pairing logic). Duel tactics notes: `docs/duel-tactics-3.3.5a.md`.

## Cross-module rules

- No `WorldSession::IsBot()`: use `PvPLife::IsBotSession(player)`. `IsRealPlayer()` alone is wrong
  at login, since mod-playerbots attaches its AI after `OnPlayerLogin`.
- `PvPLife.Bots.Archetypes` filters through Life's `LifeArchetypes` (every bot fits without a
  provider). PvP never includes Chat's headers.
- A deploying raid defense publishes `LifeEvents::RaidDefenseStarted`.
- A bot Chat marks busy (`Manager::Talking`) is kept out of duel and challenge picks, antics and the
  linger release until the scene ends.
- Invariants: bots borrowed as-is (no level, talent or gear changes), GMs never challenged,
  announcements off by default, **no bot speech** (`ApplyPvpStrategies` adds `-chat,-emote`; speech
  is Chat's).

## Database

World DB only. `data/sql/db-world/base/pvp_life.sql` is re-applied whenever its hash changes, so
keep it idempotent (`CREATE TABLE IF NOT EXISTS`, `INSERT IGNORE` on `uq_pvp_life_zone_name`) and
edit it in place rather than adding update files. `pvp_life_zone` holds hotspots
(`activity_type` 0=Skirmish 1=Duel 2=ForTheHorde 3=ForTheAlliance; teams 0=Alliance 1=Horde
2=Any; `bot_chat` is an unread leftover). GM zone edits are async (`QueueZoneMutation`); no
blocking DB calls on the world thread.

## Config

`conf/mod_playerbots_pvp_life.conf.dist` documents every option. A new one needs an entry there, a
member in `PvPLifeMgr.h`, a read in `Manager::LoadConfig()` and, if relevant, a line in
`PrintStatus`. Keys shared in meaning with City keep the same name after the prefix (`Bots.*`,
`Population.BudgetMode`, `Antics.*`, `DuelMedics.*`, `Audit.*`).

/*
 * PvP Life
 * Copyright (C) 2026 iCore
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PVP_LIFE_MGR_H
#define PVP_LIFE_MGR_H

#include "PvPLifeTypes.h"
// The shared Life headers (movement, medics, bot identity, presence, audit) and their one
// registry are mod-hearthside-life's, so this module needs it checked out beside it.
#if __has_include("../../mod-hearthside-life/src/LifeMotion.h")
#include "../../mod-hearthside-life/src/LifeMotion.h"
#else
#error "mod-hearthside-pvp requires mod-hearthside-life beside it in modules/"
#endif
#include "../../mod-hearthside-life/src/LifeMedic.h"
#include "../../mod-hearthside-life/src/LifeBots.h"
#include "../../mod-hearthside-life/src/LifePresence.h"
#include "../../mod-hearthside-life/src/LifeAudit.h"
#include "AsyncCallbackProcessor.h"
#include "Position.h"
#include "QueryCallback.h"
#include "Transaction.h"
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class ChatHandler;
class Player;
class Unit;
class Field;

namespace PvPLife
{
    // LifeBots::IsBotSession / IsHumanPlayer with this module's bot-account filter
    // (PvPLife.Bots.AccountPrefix/AccountMin/AccountMax/AccountFilterMode).
    bool IsBotSession(Player const* player);
    bool IsHumanPlayer(Player const* player);

    // PvPLife.Audit.*: Logs/pvplife_audit.log by default.
    inline LifeAudit::Logger& AuditLogger()
    {
        static LifeAudit::Logger logger;
        return logger;
    }

    class Manager
    {
    public:
        static Manager& Instance();

        void LoadConfig();
        void Update(uint32 diff);
        void Shutdown();
        bool HandleCommand(ChatHandler* handler, std::string const& text);
        bool HandleWpvpCommand(ChatHandler* handler, std::string const& text);

        // Public Hook & Participant Interface
        bool IsParticipant(uint32 guidLow) const;
        // returnHome: the bot walks off (watched) or goes back where it was borrowed from, as at an
        // event's end; otherwise it is let go where it stands.
        void DisengageParticipant(uint32 guidLow, bool returnHome = false);
        void OnPlayerLogout(Player* player);
        void OnAddGroupMember(uint32 guidLow);
        // PlayerScript/GroupScript hooks can fire on map-update threads (a bot's own AI logs it out
        // or accepts an invite from inside Player::Update), concurrently with other hooks that read
        // the participant list. They queue the GUID here; Update() disengages it on the world thread.
        void QueueDisengage(uint32 guidLow);
        bool IsConfiguredBotAccount(uint32 accountId) const;
        LifeBots::AccountFilter const& Accounts() const { return _accounts; }
        // PlayerScript::OnPlayerDuelEnd, any duel won anywhere (map-update threads): queued for the
        // duel medics, handled on the world thread.
        void QueueDuelEnd(Player* winner, Player* loser);

    private:
        void ProcessQueuedDisengages();
        std::mutex _queuedDisengageMutex;
        std::vector<uint32> _queuedDisengages;
        Manager() = default;

        bool VerifyDatabase();

        std::vector<Zone> LoadZones(bool readyOnly = false, int typeFilter = -1);
        bool LoadZoneByName(std::string name, Zone& out);
        void LoadZoneConfig();
        bool QueueZoneMutation(std::string const& sql);
        void RefreshAccountCache();
        void ApplyZoneConfig(Zone& zone) const;
        Zone ReadZone(Field* fields);
        std::string ZoneSelectSql(std::string const& suffix = "") const;

        std::vector<BotCandidate> LoadCandidates(TeamSide team, uint8 minLevel, uint8 maxLevel,
            std::unordered_set<uint32> const& excluded, uint32 deficit = 0);
        // Offline bots between minLevel and maxLevel; the snapshot behind it is fetched
        // asynchronously, so the first call for a level range returns nothing.
        std::vector<BotCandidate> LoadOfflineCandidates(TeamSide team, uint8 minLevel, uint8 maxLevel,
            std::unordered_set<uint32> const& excluded);
        static uint16 OfflineCacheKey(uint8 minLevel, uint8 maxLevel) { return uint16(uint16(minLevel) << 8 | maxLevel); }
        // True if this bot's mod-hearthside-chat archetype is on PvPLife.Bots.Archetypes
        // (or that list is empty, or the chat module is not in the build).
        bool FitsArchetype(uint32 guidLow) const { return _archetypeFilter.Fits(guidLow); }
        Player* FindPlayer(uint32 guidLow) const;
        TeamSide TeamForRace(uint8 race) const;
        bool IsSafeBot(Player* player, bool allowPvPLifeReservation = false) const;
        uint8 EffectiveMinLevel(uint8 minLevel, ActivityType type = ActivityType::Skirmish) const;

        bool StartZone(Zone const& zone, ChatHandler* handler = nullptr, bool forced = false);
        void EndEvent(ActiveEvent& event, std::string const& reason);
        void ExpireEvents();
        void MaintainPersistentWorldPvp();
        void MaintainPersistentDuelZones();
        void TryStartRandomSkirmish();
        void TryStartFactionCampaign();

        // Reactive Raid Response
        void TryQueueReactiveRaidResponse();
        void StartPendingRaidResponses();
        bool CancelPendingRaidResponse(uint64 responseId, std::string const& reason);

        // Movement, combat engagement and duels
        void ScheduleMove(uint64 eventId, PlayerbotsLife::LeaseToken const& token, uint32 guidLow,
            std::string const& name, uint32 mapId, float x, float y, float z, float o, uint32 delaySeconds, bool teleport);
        // False if the step was not queued (a bot already has a step, or the pair limit is reached).
        bool ScheduleDuel(uint64 eventId, uint64 pairId, PlayerbotsLife::LeaseToken const& challengerToken,
            PlayerbotsLife::LeaseToken const& targetToken, uint32 challengerGuid, uint32 targetGuid,
            std::string const& challengerName, std::string const& targetName, uint32 delaySeconds,
            bool realPlayerTarget = false, uint32 challengeClaimExpiry = 0);
        // Erases matching duel steps, handing back the shared real-player challenge claim of any
        // real-player step whose challenge was never sent.
        template <class Pred> void EraseDuelSteps(Pred pred);
        // A real player this bot may challenge right now (S5 checks, GM, level, range).
        bool IsChallengeableRealPlayer(Player* target, Player* bot) const;
        void ScheduleEngagement(uint64 eventId, PlayerbotsLife::LeaseToken const& botToken,
            uint32 botGuid, uint32 targetGuid, uint32 delaySeconds);
        void ScheduleEventChat(ActiveEvent const& event);

        bool CanExecuteStep(uint64 eventId, uint64 generation, uint32 guidLow, bool allowPreparing = false) const;
        bool IsHumanControlled(Player* player) const;
        bool IsOwnedActor(Player* player, Participant const& participant, uint64 eventId) const;
        void ProcessAdmissionGrants();
        void QueueParticipantRelease(Participant const& participant, bool returnHome);
        void RememberDuelIdentity(uint64 eventId, Player* challenger, Player* target);
        // LifeBots::Restore, unless a real player's group or a battleground has taken the bot over
        // (mod-playerbots reset its strategies itself then).
        void RestoreAIState(Player* player, LifeBots::StrategyJournal const& journal) const;
        void CloseNativeDuel(Player* player, Participant const& participant) const;

        static bool TryParseUInt32(std::string const& str, uint32& outVal);
        static bool TryParseUInt64(std::string const& str, uint64& outVal);

        void ProcessMoveQueue();
        void ProcessDuelQueue();
        void ProcessEngagementQueue();
        void ProcessChatQueue();
        void ProcessRealPlayerChallenges();
        void MaintainDuelParticipants();
        // Role matchups for bot duels (PvPLife.Duel.Matchup.*): DPS vs DPS always, a healer rarely,
        // since healer duels drag on until someone gives up. Rolled per candidate pair.
        enum class DuelRole : uint8 { Dps = 0, Tank = 1, Healer = 2 };
        static DuelRole RoleOf(Player* player); // null (offline candidate) counts as DPS
        bool RollMatchup(Player* a, Player* b) const;
        void MaintainBotDuels();
        // Player-like movement for leased bots fighting a player, by class and spec
        // (PvPLifeFootwork.h, PvPLife.Footwork.*); playerbots' own combat AI only closes to range or flees.
        // Radius around X/Y and HomeRadius around the bot's own staging spot; 0 = unbounded.
        using FootworkBound = LifeMotion::Bound;
        void UpdateFootwork();
        // Any of this realm's own bots fighting a player in a battleground, not only leased ones.
        void UpdateBattlegroundFootwork(uint64 now);
        // The per-bot interval, the state gates and the trace around Footwork.
        void RunFootwork(Player* bot, Player* opponent, FootworkBound const& bound, char const* context, uint64 now);
        // What it did, for PvPLife.Footwork.Trace: an action, a spell name, or why it held.
        char const* Footwork(Player* bot, Player* opponent, FootworkBound const& bound);
        // One footwork move: a smooth curve (or a jump) that keeps facing `face`.
        bool FootworkStep(Player* bot, Unit* face, float angle, float distance, Position const& from, bool jump,
            FootworkBound const& bound);
        void ProcessFootworkChecks();
        // Idle antics for duel-ground bots between bouts and a mid-air dismount for riders coming in
        // (LifeMotion.h, PvPLife.Antics.*). Run every ~100 ms from Update.
        void UpdateAntics();
        // mod-playerbots' AcceptDuelAction resets a bot's strategies to its random-bot defaults the
        // moment it accepts, so during the countdown "new rpg" walked it off and "mount" rode it away
        // until the challenge broke off. Puts the duel set straight back (every ~100 ms from Update).
        void KeepDuelStrategies();
        // The 3-second duel countdown, used the way players use it: a ranged duelist backs off to its
        // range, a warrior out to Charge range, and each casts its stealth or self-buffs
        // (FootworkProfile::Prep, PvPLife.Footwork.DuelPrep). Every ~100 ms from Update.
        void UpdateDuelPrep();
        // Where a duelist fights inside: the duel flag's circle and its pair's staging area.
        FootworkBound DuelBound(Player* bot, Player* opponent, Participant const& participant, ActiveEvent const& event) const;
        struct DuelPrepState
        {
            uint64 CastAtMs = 0;
            uint64 MoveAtMs = 0;
            uint64 FaceAtMs = 0; // a bot that turned and ran turns back to its opponent on arrival
            uint8 Casts = 0;
            bool Moved = false;
        };
        std::unordered_map<uint32, DuelPrepState> _duelPrep;
        // Starts `kind` on an idle duelist now; with None, rolls PvPLife.Antics.ChancePerMinute and picks one.
        bool TryStartAntic(Player* bot, uint32 guidLow, LifeMotion::Antic kind = LifeMotion::Antic::None);
        void StopAntic(uint32 guidLow);
        bool AnticRunning(uint32 guidLow) const;
        // Healer duelists who now and then heal the duelists of a duel that just finished in front of
        // them (LifeMedic.h, PvPLife.DuelMedics.*). Every ~100 ms from Update.
        void UpdateDuelMedics();
        // Between bouts an idle duelist now and then strolls to another spot in its home area or turns
        // to watch a fight, its partner, a real player or nothing much (PvPLife.Duel.IdleFidget.*), so
        // the pairs do not stand frozen face to face. True if it did something.
        bool IdleFidget(Player* bot, Participant const& participant, ActiveEvent const& event,
            std::vector<Player*> const& fighting);
        // A fidget stroll still under way.
        bool FidgetWalking(uint32 guidLow, Player* bot) const;
        // A deployed, leased member of a prepared duel-zone event, or null.
        Participant const* FindDuelGroundParticipant(uint32 guidLow) const;
        struct FootworkCheck
        {
            uint64 DueMs = 0;
            float X = 0.0f, Y = 0.0f;
            bool Jump = false;
        };
        std::unordered_map<uint32, FootworkCheck> _footworkChecks;
        // Keeps mod-playerbots' level brackets from re-leveling a bot while it is leased
        // (LifeBots::HoldLevels), and drops per-bot state of bots no longer leased.
        void KeepLeasedLevelsFixed();
        void ProcessReleasingParticipants();
        void ProcessPreparingEvents();
        void DeployEventMembers(ActiveEvent& event, uint32& changesCount, uint32 maxChanges);

        // With a journal the changes are recorded for the restore (at deployment); the re-applications
        // after mod-playerbots reset a bot's strategies need none, as the restore compares states.
        void ApplyPvpStrategies(Player* player, bool duelMode, LifeBots::StrategyJournal* journal = nullptr) const;
        // With PvPLife.Movement.DropOffArrival a "teleport" is an arrival: a bot within walking range walks
        // in; anyone else is dropped off out of sight on a clear path and walks or rides the rest.
        void MoveBot(Player* player, uint32 mapId, float x, float y, float z, float o, bool teleport);
        void WalkBot(Player* player, uint32 mapId, float x, float y, float z);
        void ProcessPendingWalks();
        bool IsLeasedParticipant(uint32 guidLow) const;
        // A real player within sight of this bot (LifePresence::Watched): ending an event then walks
        // it off (LifeArrival::WalkOff) instead of a teleport.
        bool RealPlayerWatching(Player* bot) const;
        // mod-playerbots' BotActiveAlone verdict: new duels, engagements and footwork wait for it.
        bool BotActive(Player* bot) const;
        void EngageTarget(Player* bot, Player* target);
        float Jitter() const;
        uint32 GetLiveWorldBotCount() const;
        uint32 CalculatePopulationBudget() const;
        // budgetShort: set when the shared budget, not the zone row, limited the count.
        void CalculatePopulation(Zone const& zone, uint32& attackerCount, uint32& defenderCount,
            bool* budgetShort = nullptr) const;
        // mod-playerbots' BotActiveAlone wake rule for this spot: a real player on its map in the same
        // zone (ForceWhenInZone), anywhere on the map (ForceWhenInMap) or within ForceWhenInRadius.
        // Bots there are awake, so spots near a player get the budget first.
        bool PlayerPresent(Zone const& zone) const;

        // Duel-ground wind-down: at the timer a watched ground lets one idle bot go every
        // IntervalMin-Max seconds; the partner it leaves lingers LingerMin-Max seconds and then
        // goes too, until nobody is left or MaxSeconds pass.
        bool CanWindDown(ActiveEvent const& event) const;
        void StartWindDown(ActiveEvent& event, uint32 now);
        void UpdateWindDown(ActiveEvent& event, uint32 now);
        // Lets go of a member mod-playerbots took into a battleground or dungeon (its partner starts
        // lingering, as if it had walked off) and of each lingering member whose time is up.
        void ReleaseLeavers(ActiveEvent& event, uint32 now);
        // The partner `departingGuid` leaves behind (same staging pair) starts lingering.
        void StartLinger(ActiveEvent& event, Participant const& departing, uint32 now);
        // Not dueling, about to duel, fighting, moving off or mid-antic: free to leave now.
        bool DuelIdle(Player* bot, uint32 guidLow) const;
        // A duelist waiting long without a duel rolls once for boredom (more antics).
        void RollBoredom(uint32 guidLow);

        void AuditLog(std::string const& tag, std::string const& text) const;
        std::string RandomChatLine(ActivityType type, ChatChannel channel, TeamSide speakerTeam);
        void Announce(std::string const& text) const;
        void PrintStatus(ChatHandler* handler) const;
        void PrintScan(ChatHandler* handler, uint8 minLevel, uint8 maxLevel);
        void PrintHelp(ChatHandler* handler, bool isWpvp = false) const;
        void PrintCompat(ChatHandler* handler) const;
        void PrintReservations(ChatHandler* handler) const;
        void PrintDebugStatus(ChatHandler* handler) const;
        bool HandleZoneCommand(ChatHandler* handler, std::vector<std::string> const& args);
        bool HandleSpotCommand(ChatHandler* handler, std::vector<std::string> const& args);
        bool HandleResponseCommand(ChatHandler* handler, std::vector<std::string> const& args);
        bool HandleDebugCommand(ChatHandler* handler, std::vector<std::string> const& args);
        void StopEventById(uint64 id, ChatHandler* handler);

        // Master & timing
        bool _enable = true;
        bool _databaseReady = false;
        bool _debug = false;
        bool _announce = false;
        uint32 _startupDelaySeconds = 90;
        uint32 _tickSeconds = 20;
        uint32 _timerMs = 0;
        uint32 _startupElapsedMs = 0;
        uint32 _maxActiveEvents = 4;

        // Bot selection & filtering
        bool _requireBotEligibility = true;
        bool _usePlayerbotConfig = true;
        std::string _botAccountPrefix = "auto";
        uint32 _botAccountMin = 0;
        uint32 _botAccountMax = 0;
        bool _accountsRequireBoth = false; // PvPLife.Bots.AccountFilterMode = all
        LifeBots::AccountFilter _accounts;
        uint32 _botQueryLimit = 500;
        LifeBots::ArchetypeFilter _archetypeFilter; // PvPLife.Bots.Archetypes
        uint32 _minOnlineRequired = 3;
        uint8 _globalMinLevel = 20;
        bool _skipGroupedBots = true;
        bool _respectPlayerbotActivity = false;
        bool _allowPartialTeams = true;
        uint32 _minimumBotsPerSide = 1;
        uint32 _maxBotsPerSide = 100;
        bool _returnBots = true;

        // Movement & combat
        bool _teleportOnArrival = true;
        bool _useMovePoint = true;
        bool _hardTeleportToTarget = false;
        bool _dropOffArrival = true;
        uint32 _positionJitter = 10;
        uint32 _arrivalStaggerMin = 2;
        uint32 _arrivalStaggerMax = 12;
        uint32 _moveAfterArrivalMin = 6;
        uint32 _moveAfterArrivalMax = 20;
        bool _strategiesEnable = true;
        bool _forcePvpFlag = true;
        bool _forceAttackOnEngage = true;
        uint32 _engageAfterMoveSeconds = 4;
        uint32 _engageRetrySeconds = 5;
        uint32 _engageMaxRetries = 12;

        std::string _pvpCombatStrategies = "+pvp,+boost,+dps debuff,-passive,-stay";
        std::string _pvpNonCombatStrategies = "+pvp,+boost,-passive,-stay";
        std::string _duelNonCombatStrategies = "+duel,+pvp,+boost,+stay,-follow,-passive,-grind";

        // Persistent World PvP
        bool _alwaysActiveWorldPvp = true;
        uint32 _minActiveSkirmishes = 1;
        uint32 _maxActiveSkirmishes = 3;
        uint32 _randomSkirmishChance = 35;

        // Duel zones
        bool _duelEnable = true;
        bool _duelAlwaysActive = true;
        uint32 _duelSpellId = 7266;
        uint32 _duelPairLimit = 12;
        uint32 _duelPairDelayMin = 5;
        uint32 _duelPairDelayMax = 18;
        uint32 _duelLeashRadius = 40;
        uint32 _duelGuardIntervalMs = 1000;
        uint32 _duelGuardTimerMs = 0;
        bool _challengeRealPlayers = true;
        uint32 _realPlayerChallengeChance = 25;
        uint32 _realPlayerScanRadius = 55;
        uint32 _realPlayerMaxLevelDifference = 5;
        uint32 _realPlayerCooldownSeconds = 900;
        uint32 _botChallengeCooldownSeconds = 180;
        uint32 _botDuelCooldownSeconds = 45;
        uint32 _botDuelMaxLevelDifference = 2;
        // Wind-down (PvPLife.Duel.WindDown.*): a watched duel ground empties one bot at a time.
        bool _windDownEnable = true;
        uint32 _windDownIntervalMin = 30;
        uint32 _windDownIntervalMax = 150;
        uint32 _windDownLingerMin = 60;
        uint32 _windDownLingerMax = 300;
        uint32 _windDownMaxSeconds = 900;
        // A duel ground that emptied stays quiet this long (PvPLife.Duel.QuietMin/MaxSeconds).
        uint32 _duelQuietMin = 30;
        uint32 _duelQuietMax = 1200;
        std::unordered_map<uint32, uint32> _duelQuietUntil; // zone id -> game time
        // Bored duelists (PvPLife.Duel.Bored.*) do antics this many times as often. Value 0 = rolled
        // and not bored; no entry = not rolled yet.
        uint32 _boredChance = 35;
        uint32 _boredAfterSeconds = 240;
        uint32 _boredMultiplierMin = 2;
        uint32 _boredMultiplierMax = 4;
        std::unordered_map<uint32, uint32> _boredom;
        std::unordered_map<uint32, uint32> _lastDuelAt; // bot guid -> game time its last duel began
        // While a spot near a real player is waiting for budget, spots nobody is near do not start.
        uint32 _presenceWaitingUntil = 0;
        uint32 _duelMatchup[3][3] = { { 100, 70, 10 }, { 70, 25, 0 }, { 10, 0, 5 } }; // [DuelRole][DuelRole], %
        bool _footworkDuels = true;
        bool _footworkWorldPvp = true;
        bool _footworkBattlegrounds = false;
        bool _footworkTrace = false;
        bool _footworkDuelPrep = true;
        uint32 _footworkIntervalMinMs = 1000;
        uint32 _footworkIntervalMaxMs = 2500;
        uint32 _footworkJumpChance = 35;
        std::unordered_map<uint32, uint64> _footworkNextMs;
        LifeMotion::AnticConfig _antics;
        std::unordered_map<uint32, LifeMotion::Routine> _anticRoutines; // idle entries keep the cooldown
        std::unordered_set<uint32> _hopDismountRolled; // riders already rolled for a mid-air dismount
        uint32 _anticTimerMs = 0;
        LifeMedic::Config _medics;
        std::mutex _duelEndMutex;
        std::vector<LifeMedic::DuelEnd> _duelEnds;
        std::vector<LifeMedic::PendingHeal> _pendingHeals;
        bool _idleFidgetEnable = true;
        uint32 _idleFidgetMinSeconds = 6;
        uint32 _idleFidgetMaxSeconds = 20;
        struct FidgetState
        {
            uint64 NextMs = 0;
            uint64 WalkUntilMs = 0;
        };
        std::unordered_map<uint32, FidgetState> _idleFidget;

        // Campaigns & chatter
        bool _forTheHordeEnable = true;
        bool _forTheAllianceEnable = true;
        uint32 _factionCampaignChance = 12;
        uint32 _factionCampaignGlobalCooldown = 3600;
        uint32 _lastFactionCampaign = 0;
        bool _botChatEnable = false;
        uint32 _botChatYellChance = 0;
        uint32 _botChatWorldChance = 0;

        // Reactive Raid Response
        bool _reactiveEnable = true;
        bool _reactiveAllianceEnable = true;
        bool _reactiveHordeEnable = true;
        uint32 _reactiveMinGroupSize = 20;
        uint32 _reactiveMaxResponders = 25;
        ResponderLevelMode _reactiveLevelMode = ResponderLevelMode::Raid;
        uint8 _reactiveFixedLevel = 60;
        uint8 _reactiveMaxLevelDifference = 5;
        bool _reactiveLoginOfflineBots = false;
        uint32 _reactiveLoginDelaySeconds = 20;
        uint32 _reactiveLoginTimeoutSeconds = 60;
        uint32 _reactiveMaxConcurrentLogins = 4;
        uint32 _reactiveCooldownSeconds = 1800;
        uint32 _reactiveDurationMinutes = 20;
        float _reactiveRallyDistance = 120.0f;
        float _reactiveLaneSpacing = 4.0f;
        uint32 _reactiveFormationColumns = 9;
        uint32 _lastReactiveResponse = 0;
        uint64 _nextResponseId = 1;
        std::vector<PendingRaidResponse> _pendingResponses;

        // Population Budget
        bool _populationBudgetEnable = true;
        bool _budgetPercentage = true; // PvPLife.Population.BudgetMode: percentage, or count (fixed)
        uint32 _fixedCount = 100;
        uint32 _borrowPercent = 10;
        uint32 _maxTotal = 120;
        uint32 _maxChangesPerTick = 3;
        uint32 _changesThisTick = 0;
        // Releases have their own allowance, refilled every second, so a large event ending cannot
        // starve deployments or run past the release deadline (ProcessReleasingParticipants).
        uint32 _releasesThisSecond = 0;
        uint32 _releaseSecond = 0;
        bool _proportionalZoneScaling = true;

        // Audit log
        bool _auditEnable = true;
        std::string _auditFilePath = "Logs/pvplife_audit.log";

        // Active state
        uint64 _nextEventId = 1;
        uint64 _nextPairId = 1;
        uint64 _nextAssignmentId = 1;
        std::vector<ActiveEvent> _activeEvents;
        std::vector<MoveStep> _moveQueue;
        // A bot just dropped off out of sight, waiting for its teleport to land before it walks in.
        struct PendingWalk
        {
            uint32 MapId = 0;
            float X = 0.0f, Y = 0.0f, Z = 0.0f;
            uint64 DueMs = 0;
            bool MountTried = false;
        };
        std::unordered_map<uint32, PendingWalk> _pendingWalks;
        std::unordered_set<uint32> _dismountOnArrival;
        // Where an idle duel bot that wandered off is walking back to, inside its home area.
        std::unordered_map<uint32, Position> _idleReturnPoint;
        std::vector<DuelStep> _duelQueue;
        std::vector<EngagementStep> _engagementQueue;
        std::vector<ChatStep> _chatQueue;
        std::unordered_map<uint32, uint32> _playerChallengeCooldown;
        std::unordered_map<uint32, uint32> _botChallengeCooldown;
        std::unordered_map<uint32, uint32> _botDuelCooldown;
        mutable uint32 _lastCensusTimeMs = 0;
        mutable uint32 _cachedLiveBotCount = 0;
        QueryCallbackProcessor _queryProcessor;
        AsyncCallbackProcessor<TransactionCallback> _transactionProcessor;
        uint32 _pendingZoneWrites = 0;
        uint64 _dataGeneration = 0;
        uint64 _zoneSnapshotGeneration = 0;
        std::vector<Zone> _zoneCache;
        struct CachedChatLine
        {
            ActivityType Type;
            ChatChannel Channel;
            TeamSide Team;
            std::string Text;
            uint32 Weight;
        };
        std::vector<CachedChatLine> _chatCache;
        std::unordered_map<uint16, std::vector<BotCandidate>> _offlineCandidateCache; // keyed by OfflineCacheKey
        std::unordered_map<uint16, uint32> _offlineCandidateRefresh;
        std::unordered_set<uint16> _offlineCandidateQueries;
        uint32 _lastCooldownPruneTime = 0;
        void PruneExpiredCooldowns();

        // The real players online (LifePresence::Snapshot), refreshed at most every 3s.
        mutable LifePresence::Snapshot _presence;
        void UpdateRealPlayersSnapshot() const;

        enum class ReleasePhase : uint8
        {
            PendingCombatExit = 0,
            RestoreWhenSafe   = 1,
            Released          = 2
        };

        struct ReleasingParticipant
        {
            Participant Part;
            uint32 EnqueuedAt = 0;
            uint32 Deadline = 0;
            uint32 Attempts = 0;
            ReleasePhase Phase = ReleasePhase::PendingCombatExit;
            bool ReturnHome = false;
        };
        std::vector<ReleasingParticipant> _releasingParticipants;
        // The release path past the deadline, at logout and at shutdown: closes our duel, stops
        // combat and restores AI state; teleports home only when allowTeleport and the bot can go.
        void ForceReleaseParticipant(ReleasingParticipant const& releasing, Player* player, bool allowTeleport);

        struct ZoneConfig
        {
            bool Enabled = true;
            uint32 MinPopulation = 2;
            uint32 MaxPopulation = 2;
        };

        std::unordered_map<std::string, ZoneConfig> _zoneConfig;
        ZoneConfig BuildZoneConfig(Zone const& zone) const;
    };
}

#define sPvPLifeMgr PvPLife::Manager::Instance()
#define sPvPLifeAudit PvPLife::AuditLogger()

#endif

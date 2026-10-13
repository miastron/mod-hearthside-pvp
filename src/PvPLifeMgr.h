/*
 * PvP Life
 * Copyright (C) 2026 iCore
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PVP_LIFE_MGR_H
#define PVP_LIFE_MGR_H

#include "PvPLifeTypes.h"
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
class Field;

namespace PvPLife
{
    // A playerbot's session, by account, whether or not mod-playerbots has
    // attached its AI yet: it attaches after OnPlayerLogin, so ::IsRealPlayer()
    // alone reads every bot logging in as a human. Stands in for the
    // WorldSession::IsBot() this module was written against, which this core
    // does not have.
    bool IsBotSession(Player const* player);

    // A person playing through the client: has a session, is not a bot account and has no
    // PlayerbotAI attached. A player-owned alt bot is thus neither borrowable nor human.
    bool IsHumanPlayer(Player const* player);

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
        void DisengageParticipant(uint32 guidLow);
        void OnPlayerLogout(Player* player);
        void OnAddGroupMember(uint32 guidLow);
        // PlayerScript/GroupScript hooks can fire on map-update threads (a bot's own AI logs it out
        // or accepts an invite from inside Player::Update), concurrently with other hooks that read
        // the participant list. They queue the GUID here; Update() disengages it on the world thread.
        void QueueDisengage(uint32 guidLow);
        bool IsConfiguredBotAccount(uint32 accountId) const;

    private:
        void ProcessQueuedDisengages();
        std::mutex _queuedDisengageMutex;
        std::vector<uint32> _queuedDisengages;
        Manager() = default;

        bool VerifyDatabase();
        void ImportPlayerbotDefaults();

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
        bool FitsArchetype(uint32 guidLow);
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
        void CaptureAIState(Player* player, AIStateJournal& journal) const;
        void RestoreAIState(Player* player, AIStateJournal const& journal) const;
        void CloseNativeDuel(Player* player, Participant const& participant) const;

        static bool TryParseUInt32(std::string const& str, uint32& outVal);
        static bool TryParseUInt64(std::string const& str, uint64& outVal);

        void ProcessMoveQueue();
        void ProcessDuelQueue();
        void ProcessEngagementQueue();
        void ProcessChatQueue();
        void ProcessRealPlayerChallenges();
        void MaintainDuelParticipants();
        void MaintainBotDuels();
        // Player-like movement for leased bots fighting a player, by class and spec
        // (PvPLifeFootwork.h, PvPLife.Footwork.*); playerbots' own combat AI only closes to range or flees.
        struct FootworkBound
        {
            float X = 0.0f, Y = 0.0f;
            float Radius = 0.0f; // 0 = unbounded
            float HomeX = 0.0f, HomeY = 0.0f;
            float HomeRadius = 0.0f; // the bot's own staging spot; 0 = none
        };
        void UpdateFootwork();
        // Any of this realm's own bots fighting a player in a battleground, not only leased ones.
        void UpdateBattlegroundFootwork(uint64 now);
        // The per-bot interval, the state gates and the trace around Footwork.
        void RunFootwork(Player* bot, Player* opponent, FootworkBound const& bound, char const* context, uint64 now);
        // What it did, for PvPLife.Footwork.Trace: an action, a spell name, or why it held.
        char const* Footwork(Player* bot, Player* opponent, FootworkBound const& bound);
        bool FootworkStep(Player* bot, float angle, float distance, Position const& from, bool jump,
            FootworkBound const& bound) const;
        // Keeps mod-playerbots' level brackets from re-leveling a bot while it is leased.
        void KeepLeasedLevelsFixed();
        void ProcessReleasingParticipants();
        void ProcessPreparingEvents();
        void DeployEventMembers(ActiveEvent& event, uint32& changesCount, uint32 maxChanges);

        void ApplyPvpStrategies(Player* player, bool duelMode) const;
        // With PvPLife.Movement.DropOffArrival a "teleport" is an arrival: a bot within walking range walks
        // in; anyone else is dropped off out of sight on a clear path and walks or rides the rest.
        void MoveBot(Player* player, uint32 mapId, float x, float y, float z, float o, bool teleport);
        void WalkBot(Player* player, uint32 mapId, float x, float y, float z);
        bool FindDropOff(Player* bot, uint32 mapId, float x, float y, float z, Position& dropOff) const;
        void ProcessPendingWalks();
        bool IsLeasedParticipant(uint32 guidLow) const;
        void EngageTarget(Player* bot, Player* target);
        float Jitter() const;
        uint32 GetLiveWorldBotCount() const;
        uint32 CalculatePopulationBudget() const;
        void CalculatePopulation(Zone const& zone, uint32& attackerCount, uint32& defenderCount) const;

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
        AccountFilterMode _accountFilterMode = AccountFilterMode::LegacyAny;
        uint32 _botQueryLimit = 500;
        std::string _archetypes;           // PvPLife.Bots.Archetypes, comma-separated mod-hearthside-chat enum names
        uint32 _archetypeMask = 0;         // parsed in LoadConfig; 0 with a non-empty list = no bot fits
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
        bool _footworkDuels = true;
        bool _footworkWorldPvp = true;
        bool _footworkBattlegrounds = false;
        bool _footworkTrace = false;
        uint32 _footworkIntervalMinMs = 1000;
        uint32 _footworkIntervalMaxMs = 2500;
        uint32 _footworkJumpChance = 35;
        std::unordered_map<uint32, uint64> _footworkNextMs;
        std::unordered_set<std::string> _levelExcludeNamesAdded;

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
        BudgetMode _budgetMode = BudgetMode::Percentage;
        uint32 _fixedCount = 100;
        uint32 _borrowPercent = 10;
        uint32 _maxTotal = 120;
        DemandMode _demandMode = DemandMode::Hybrid;
        uint32 _baselinePercent = 20;
        uint32 _burstGraceSeconds = 180;
        uint32 _maxChangesPerTick = 3;
        uint32 _changesThisTick = 0;
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
        mutable std::unordered_map<uint32, bool> _botAccountCache;
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
        mutable std::unordered_map<uint32, uint32> _zoneLastRealPlayerTime;
        uint32 _lastCooldownPruneTime = 0;
        void PruneExpiredCooldowns();

        struct RealPlayerSnapshotEntry
        {
            uint32 GuidLow = 0;
            uint32 MapId = 0;
            uint32 ZoneId = 0;
            uint32 AreaId = 0;
            TeamSide Team = TeamSide::Any;
            uint8 Level = 0;
            float X = 0.0f;
            float Y = 0.0f;
            float Z = 0.0f;
            float O = 0.0f;
            bool IsAlive = true;
            bool IsPvP = false;
            bool InCombat = false;
            bool HasDuel = false;
            bool IsGameMaster = false;
            uint32 Security = 0;
            bool IsGroupLeader = false;
            uint32 GroupMembersCount = 0;
        };
        mutable std::vector<RealPlayerSnapshotEntry> _cachedRealPlayers;
        mutable uint32 _lastRealPlayerCensusTimeMs = 0;
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

#endif

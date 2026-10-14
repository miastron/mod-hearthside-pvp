/*
 * PvP Life
 * Copyright (C) 2026 iCore
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PvPLifeMgr.h"
#include "PvPLifeDuelSelection.h"
#include "PvPLifeDuelRequestPolicy.h"
#include "PvPLifeFootwork.h"
#include "PvPLifeAudit.h"
#include "LifeBotReservation.h"

#include "AiFactory.h"
#include "BattlefieldMgr.h"
#include "Battleground.h"
#include "BattlegroundEY.h"
#include "BattlegroundWS.h"
#include "CellImpl.h"
#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "LastMovementValue.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "MoveSplineInit.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "Opcodes.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PositionValue.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "SharedDefines.h"
#include "SocialMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"


// Archetype filter: mod-hearthside-chat's GUID-fixed personality decides which
// bots this module may borrow (PvPLife.Bots.Archetypes). Without that module in the
// build every archetype fits.
#if __has_include("hs_archetype.h")
#include "hs_archetype.h"
#define PVPLIFE_HAS_ARCHETYPES 1
#endif

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <set>
#include <sstream>
#include <shared_mutex>

namespace
{
    inline bool IsLifeBotReserved(uint32 guidLow)
    {
        return PlayerbotsLife::IsReserved(guidLow);
    }

    std::string Lower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    std::vector<std::string> Tokenize(std::string const& text)
    {
        std::istringstream stream(text);
        std::vector<std::string> out;
        std::string token;
        while (stream >> token)
            out.push_back(token);
        return out;
    }

    uint32 GameTimeSeconds()
    {
        return static_cast<uint32>(GameTime::GetGameTime().count());
    }

    uint32 GameTimeMS()
    {
        return static_cast<uint32>(GameTime::GetGameTimeMS().count());
    }

    char const* TypeName(PvPLife::ActivityType type)
    {
        switch (type)
        {
            case PvPLife::ActivityType::Skirmish: return "Skirmish";
            case PvPLife::ActivityType::Duel: return "Duel";
            case PvPLife::ActivityType::ForTheHorde: return "ForTheHorde";
            case PvPLife::ActivityType::ForTheAlliance: return "ForTheAlliance";
        }
        return "Unknown";
    }

    char const* TeamName(PvPLife::TeamSide team)
    {
        switch (team)
        {
            case PvPLife::TeamSide::Alliance: return "Alliance";
            case PvPLife::TeamSide::Horde: return "Horde";
            default: return "Any";
        }
    }

    bool ParseTeam(std::string token, PvPLife::TeamSide& out)
    {
        token = Lower(token);
        if (token == "a" || token == "ally" || token == "alliance")
        {
            out = PvPLife::TeamSide::Alliance;
            return true;
        }
        if (token == "h" || token == "horde")
        {
            out = PvPLife::TeamSide::Horde;
            return true;
        }
        if (token == "any" || token == "both")
        {
            out = PvPLife::TeamSide::Any;
            return true;
        }
        return false;
    }

    bool ParseType(std::string token, PvPLife::ActivityType& out)
    {
        token = Lower(token);
        if (token == "skirmish" || token == "world" || token == "worldpvp")
        {
            out = PvPLife::ActivityType::Skirmish;
            return true;
        }
        if (token == "duel")
        {
            out = PvPLife::ActivityType::Duel;
            return true;
        }
        if (token == "forthehorde" || token == "fth")
        {
            out = PvPLife::ActivityType::ForTheHorde;
            return true;
        }
        if (token == "forthealliance" || token == "fta" || token == "fortheally")
        {
            out = PvPLife::ActivityType::ForTheAlliance;
            return true;
        }
        return false;
    }

    // Configured cooldowns are clamped to this, so "now + cooldown" cannot wrap.
    constexpr uint32 MaxCooldownSeconds = 7 * 24 * 3600;

    // How long a released bot may wait for the throttled restore path before the terminal path
    // takes it. Kept well under PlayerbotsLife::RELEASING_LEASE_TTL_SEC, after which the shared
    // scavenger drops the lease and another Life module may claim the bot.
    constexpr uint32 ReleaseDeadlineSeconds = 120;
    static_assert(ReleaseDeadlineSeconds * 2 <= PlayerbotsLife::RELEASING_LEASE_TTL_SEC,
        "the release deadline must stay well under the shared Releasing lease TTL");

    // Seconds since a stored timestamp; a timestamp in the future (clock stepped back, restored
    // database) reads as "just now" instead of underflowing into "long ago".
    uint32 SecondsSince(uint32 now, uint32 then)
    {
        return now >= then ? now - then : 0;
    }

    // While the Wintergrasp battle is in war time PvP Life starts nothing in its zone.
    bool IsWintergraspWarZone(uint32 zoneId)
    {
        if (!zoneId)
            return false;
        Battlefield* wintergrasp = sBattlefieldMgr->GetBattlefieldByBattleId(BATTLEFIELD_BATTLEID_WG);
        return wintergrasp && wintergrasp->IsWarTime() && wintergrasp->GetZoneId() == zoneId;
    }

    // Ground height near (x, y), searching down from a little above z; z itself when the base map
    // is not loaded or no ground is found.
    float GroundZ(uint32 mapId, float x, float y, float z)
    {
        Map const* map = sMapMgr->FindBaseMap(mapId);
        if (!map)
            return z;
        float ground = map->GetHeight(PHASEMASK_NORMAL, x, y, z + 5.0f);
        return ground > INVALID_HEIGHT ? ground : z;
    }

    // Cancels a duel that is still challenged or counting down (one that never reached combat).
    void CancelPendingNativeDuel(Player* player)
    {
        if (!player || !player->duel || !player->GetSession() ||
            player->duel->State == DUEL_STATE_IN_PROGRESS || player->duel->State == DUEL_STATE_COMPLETED)
            return;
        WorldPacket packet(CMSG_DUEL_CANCELLED, 8);
        packet << player->GetGuidValue(PLAYER_DUEL_ARBITER);
        player->GetSession()->HandleDuelCancelledOpcode(packet);
    }

    // The "+name"/"-name" changes that turn one strategy set into another.
    std::vector<std::string> StrategyDelta(std::vector<std::string> const& before, std::vector<std::string> const& after)
    {
        std::vector<std::string> changes;
        for (std::string const& name : after)
            if (std::find(before.begin(), before.end(), name) == before.end())
                changes.push_back("+" + name);
        for (std::string const& name : before)
            if (std::find(after.begin(), after.end(), name) == after.end())
                changes.push_back("-" + name);
        return changes;
    }
}

namespace PvPLife
{
    bool IsBotSession(Player const* player)
    {
        if (!player || !player->GetSession())
            return false;
        uint32 accountId = player->GetSession()->GetAccountId();
        return accountId && (sPlayerbotAIConfig.IsInRandomAccountList(accountId) ||
                             sRandomPlayerbotMgr.IsAddclassBot(player->GetGUID().GetCounter()) ||
                             sPvPLifeMgr.IsConfiguredBotAccount(accountId));
    }

    bool IsHumanPlayer(Player const* player)
    {
        return player && player->GetSession() && !IsBotSession(player) &&
            !GET_PLAYERBOT_AI(const_cast<Player*>(player));
    }

    template <class Pred>
    void Manager::EraseDuelSteps(Pred pred)
    {
        // partition, not remove_if: the erased steps are read below, and remove_if leaves the tail
        // in a moved-from state.
        auto removed = std::stable_partition(_duelQueue.begin(), _duelQueue.end(),
            [&pred](DuelStep const& step) { return !pred(step); });
        for (auto it = removed; it != _duelQueue.end(); ++it)
            if (it->ChallengeClaimExpiry && !it->RequestSent)
                PlayerbotsLife::ReleaseRealPlayerChallengeClaim(it->TargetGuidLow, it->ChallengeClaimExpiry);
        _duelQueue.erase(removed, _duelQueue.end());
    }

    Manager& Manager::Instance()
    {
        static Manager instance;
        return instance;
    }

    void Manager::AuditLog(std::string const& tag, std::string const& text) const
    {
        if (_auditEnable)
            sPvPLifeAudit.Log(tag, text);
    }

    void Manager::Shutdown()
    {
        ++_dataGeneration;
        PlayerbotsLife::CancelAdmissionRequest(0, PlayerbotsLife::ReservationOwner::PvPLife);
        CancelPendingRaidResponse(0, "shutdown");
        for (auto const& [guid, routine] : _anticRoutines)
            LifeMotion::Halt(FindPlayer(guid), routine);
        _anticRoutines.clear();
        for (ActiveEvent& e : _activeEvents)
            EndEvent(e, "shutdown");
        _activeEvents.clear();
        _moveQueue.clear();
        EraseDuelSteps([](DuelStep const&) { return true; });
        _engagementQueue.clear();
        _chatQueue.clear();

        // Undo what can be undone in place; nobody is teleported while the world shuts down.
        for (ReleasingParticipant const& rp : _releasingParticipants)
            if (PlayerbotsLife::ValidateLeaseState(rp.Part.Token, PlayerbotsLife::LeaseState::Releasing))
                ForceReleaseParticipant(rp, FindPlayer(rp.Part.Bot.GuidLow), false);
        _releasingParticipants.clear();

        // Backstop for anything the bookkeeping above no longer knows about.
        PlayerbotsLife::ReleaseAllForOwner(PlayerbotsLife::ReservationOwner::PvPLife);

        sPvPLifeAudit.Shutdown();
    }

    void Manager::LoadConfig()
    {
        for (ActiveEvent& event : _activeEvents)
            if (!event.FullyPrepared)
                EndEvent(event, "config-reload");
        CancelPendingRaidResponse(0, "config-reload");
        // Whether the module was running before this load; false on the first load at startup.
        bool wasRunning = _enable && _databaseReady;
        ++_dataGeneration;
        _lastCensusTimeMs = 0;
        _enable = sConfigMgr->GetOption<bool>("PvPLife.Enable", true);

        _debug = sConfigMgr->GetOption<bool>("PvPLife.Debug", false);
        _announce = sConfigMgr->GetOption<bool>("PvPLife.Announce", false);
        _startupDelaySeconds = std::min<uint32>(3600, sConfigMgr->GetOption<uint32>("PvPLife.StartupDelaySeconds", 90));
        _tickSeconds = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PvPLife.TickSeconds", 20), 5, 3600);
        _maxActiveEvents = sConfigMgr->GetOption<uint32>("PvPLife.Events.MaxActive", 4);

        // 2. Bot Selection & Filtering
        _requireBotEligibility = sConfigMgr->GetOption<bool>("PvPLife.Bots.RequireBotEligibility", true);
        _usePlayerbotConfig = sConfigMgr->GetOption<bool>("PvPLife.Bots.UsePlayerbotConfig", true);
        _botAccountPrefix = sConfigMgr->GetOption<std::string>("PvPLife.Bots.AccountPrefix", "auto");
        _botAccountMin = sConfigMgr->GetOption<uint32>("PvPLife.Bots.AccountMin", 0);
        _botAccountMax = sConfigMgr->GetOption<uint32>("PvPLife.Bots.AccountMax", 0);
        std::string filterModeStr = Lower(sConfigMgr->GetOption<std::string>("PvPLife.Bots.AccountFilterMode", "legacy_any"));
        _accountFilterMode = (filterModeStr == "all") ? AccountFilterMode::All : AccountFilterMode::LegacyAny;

        _botQueryLimit = sConfigMgr->GetOption<uint32>("PvPLife.Bots.QueryLimit", 500);
        if (_botQueryLimit)
            _botQueryLimit = std::clamp<uint32>(_botQueryLimit, 50, 5000);
        _archetypes = sConfigMgr->GetOption<std::string>("PvPLife.Bots.Archetypes",
            "PVP_SERIOUS,PVP_CASUAL,TROLL_MILD,TROLL_AGGRESSIVE");
        _minOnlineRequired = sConfigMgr->GetOption<uint32>("PvPLife.Bots.MinOnlineRequired", 3);
        _globalMinLevel = static_cast<uint8>(std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PvPLife.Bots.GlobalMinLevel", 20), 1, 80));

        _skipGroupedBots = sConfigMgr->GetOption<bool>("PvPLife.Bots.SkipGrouped", true);
        _respectPlayerbotActivity = sConfigMgr->GetOption<bool>("PvPLife.Bots.RespectPlayerbotActivity", false);
        _allowPartialTeams = sConfigMgr->GetOption<bool>("PvPLife.Bots.AllowPartialTeams", true);
        _minimumBotsPerSide = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PvPLife.Bots.MinimumPerSide", 1), 1, 10);
        _maxBotsPerSide = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PvPLife.Bots.MaxPerSide", 100), 1, 200);
        _minimumBotsPerSide = std::min(_minimumBotsPerSide, _maxBotsPerSide);
        _returnBots = sConfigMgr->GetOption<bool>("PvPLife.Bots.ReturnAfterActivity", true);

        // 3. Movement & Combat
        _teleportOnArrival = sConfigMgr->GetOption<bool>("PvPLife.Movement.TeleportOnArrival", true);
        _useMovePoint = sConfigMgr->GetOption<bool>("PvPLife.Movement.UseMovePoint", true);
        _hardTeleportToTarget = sConfigMgr->GetOption<bool>("PvPLife.Movement.HardTeleportToTarget", false);
        _dropOffArrival = sConfigMgr->GetOption<bool>("PvPLife.Movement.DropOffArrival", true);
        _positionJitter = sConfigMgr->GetOption<uint32>("PvPLife.Movement.PositionJitter", 10);
        _arrivalStaggerMin = sConfigMgr->GetOption<uint32>("PvPLife.Movement.ArrivalStaggerMinSeconds", 2);
        _arrivalStaggerMax = sConfigMgr->GetOption<uint32>("PvPLife.Movement.ArrivalStaggerMaxSeconds", 12);
        _moveAfterArrivalMin = sConfigMgr->GetOption<uint32>("PvPLife.Movement.MoveAfterArrivalMinSeconds", 6);
        _moveAfterArrivalMax = sConfigMgr->GetOption<uint32>("PvPLife.Movement.MoveAfterArrivalMaxSeconds", 20);

        _strategiesEnable = sConfigMgr->GetOption<bool>("PvPLife.Strategies.Enable", true);
        _forcePvpFlag = sConfigMgr->GetOption<bool>("PvPLife.Combat.ForcePvpFlag", true);
        _forceAttackOnEngage = sConfigMgr->GetOption<bool>("PvPLife.Combat.ForceAttackOnEngage", true);
        _engageAfterMoveSeconds = sConfigMgr->GetOption<uint32>("PvPLife.Combat.EngageAfterMoveSeconds", 4);
        _engageRetrySeconds = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("PvPLife.Combat.EngageRetrySeconds", 5));
        _engageMaxRetries = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PvPLife.Combat.EngageMaxRetries", 12), 1, 50);

        _pvpCombatStrategies = sConfigMgr->GetOption<std::string>("PvPLife.Strategies.Combat", "+pvp,+boost,+dps debuff,-passive,-stay");
        _pvpNonCombatStrategies = sConfigMgr->GetOption<std::string>("PvPLife.Strategies.NonCombat", "+pvp,+boost,-passive,-stay,-new rpg");
        _duelNonCombatStrategies = sConfigMgr->GetOption<std::string>("PvPLife.Strategies.DuelNonCombat", "+duel,+pvp,+boost,+stay,-follow,-passive,-grind,-new rpg");

        // 4. Persistent World PvP
        _alwaysActiveWorldPvp = sConfigMgr->GetOption<bool>("PvPLife.World.AlwaysActive", true);
        _minActiveSkirmishes = sConfigMgr->GetOption<uint32>("PvPLife.World.MinActiveHotspots", 1);
        _maxActiveSkirmishes = std::max(_minActiveSkirmishes, sConfigMgr->GetOption<uint32>("PvPLife.World.MaxActiveHotspots", 3));
        _randomSkirmishChance = std::min<uint32>(100, sConfigMgr->GetOption<uint32>("PvPLife.World.RandomStartChance", 35));

        // 5. Duel Zones
        _duelEnable = sConfigMgr->GetOption<bool>("PvPLife.Duel.Enable", true);
        _duelAlwaysActive = sConfigMgr->GetOption<bool>("PvPLife.Duel.AlwaysActive", true);
        _duelSpellId = sConfigMgr->GetOption<uint32>("PvPLife.Duel.SpellId", 7266);
        _duelPairLimit = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PvPLife.Duel.PairLimit", 12), 1, 40);
        _duelPairDelayMin = sConfigMgr->GetOption<uint32>("PvPLife.Duel.PairDelayMinSeconds", 5);
        _duelPairDelayMax = sConfigMgr->GetOption<uint32>("PvPLife.Duel.PairDelayMaxSeconds", 18);
        _duelLeashRadius = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PvPLife.Duel.LeashRadius", 40), 10, 1000);
        _duelGuardIntervalMs = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PvPLife.Duel.GuardIntervalMs", 1000), 250, 5000);
        _footworkDuels = sConfigMgr->GetOption<bool>("PvPLife.Footwork.Duels", true);
        _footworkWorldPvp = sConfigMgr->GetOption<bool>("PvPLife.Footwork.WorldPvP", true);
        _footworkBattlegrounds = sConfigMgr->GetOption<bool>("PvPLife.Footwork.Battlegrounds", false);
        _footworkIntervalMinMs = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PvPLife.Footwork.IntervalMinMs", 1000), 500, 30000);
        _footworkIntervalMaxMs = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PvPLife.Footwork.IntervalMaxMs", 2500), _footworkIntervalMinMs, 60000);
        _footworkJumpChance = std::min<uint32>(100, sConfigMgr->GetOption<uint32>("PvPLife.Footwork.JumpChance", 35));
        _footworkTrace = sConfigMgr->GetOption<bool>("PvPLife.Footwork.Trace", false);
        _idleFidgetEnable = sConfigMgr->GetOption<bool>("PvPLife.Duel.IdleFidget.Enable", true);
        _idleFidgetMinSeconds = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PvPLife.Duel.IdleFidget.MinSeconds", 6), 2, 600);
        _idleFidgetMaxSeconds = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PvPLife.Duel.IdleFidget.MaxSeconds", 20), _idleFidgetMinSeconds, 600);
        {
            // Duel grounds pack staging spots 20 yards apart: keep the hopping tighter than City's.
            // Their idle spells between duels are short (about a minute), so the odds per minute are higher.
            LifeMotion::AnticConfig defaults;
            defaults.Radius = 6.0f;
            defaults.ChancePerMinute = 30;
            _antics = LifeMotion::LoadAnticConfig("PvPLife.Antics", defaults);
        }
        _medics = LifeMedic::LoadConfig("PvPLife.DuelMedics");
        _challengeRealPlayers = sConfigMgr->GetOption<bool>("PvPLife.Duel.ChallengeRealPlayers", true);
        _realPlayerChallengeChance = std::min<uint32>(100, sConfigMgr->GetOption<uint32>("PvPLife.Duel.PlayerChallengeChance", 25));
        _realPlayerScanRadius = std::min<uint32>(1000, sConfigMgr->GetOption<uint32>("PvPLife.Duel.PlayerScanRadius", 55));
        _realPlayerMaxLevelDifference = std::min<uint32>(80, sConfigMgr->GetOption<uint32>("PvPLife.Duel.MaxLevelDifference", 5));
        // Cooldowns are clamped to a week so "now + cooldown" can never wrap.
        _realPlayerCooldownSeconds = std::min<uint32>(MaxCooldownSeconds, sConfigMgr->GetOption<uint32>("PvPLife.Duel.PlayerChallengeCooldownSeconds", 900));
        _botChallengeCooldownSeconds = std::min<uint32>(MaxCooldownSeconds, sConfigMgr->GetOption<uint32>("PvPLife.Duel.BotChallengeCooldownSeconds", 180));
        _botDuelCooldownSeconds = std::min<uint32>(MaxCooldownSeconds, sConfigMgr->GetOption<uint32>("PvPLife.Duel.BotVsBotCooldownSeconds", 45));
        _botDuelMaxLevelDifference = std::min<uint32>(80, sConfigMgr->GetOption<uint32>("PvPLife.Duel.BotVsBotMaxLevelDifference", 2));

        // 6. Campaigns & Chatter
        _forTheHordeEnable = sConfigMgr->GetOption<bool>("PvPLife.ForTheHorde.Enable", true);
        _forTheAllianceEnable = sConfigMgr->GetOption<bool>("PvPLife.ForTheAlliance.Enable", true);
        _factionCampaignChance = std::min<uint32>(100, sConfigMgr->GetOption<uint32>("PvPLife.FactionCampaign.ChancePerTick", 12));
        _factionCampaignGlobalCooldown = std::min<uint32>(MaxCooldownSeconds, sConfigMgr->GetOption<uint32>("PvPLife.FactionCampaign.GlobalCooldownSeconds", 3600));

        _botChatEnable = sConfigMgr->GetOption<bool>("PvPLife.BotChat.Enable", false);
        _botChatYellChance = std::min<uint32>(100, sConfigMgr->GetOption<uint32>("PvPLife.BotChat.YellChance", 0));
        _botChatWorldChance = std::min<uint32>(100, sConfigMgr->GetOption<uint32>("PvPLife.BotChat.WorldChance", 0));

        // 7. Reactive Raid Response
        _reactiveEnable = sConfigMgr->GetOption<bool>("PvPLife.ReactiveRaidResponse.Enable", true);
        _reactiveAllianceEnable = sConfigMgr->GetOption<bool>("PvPLife.ReactiveRaidResponse.AllianceEnable", true);
        _reactiveHordeEnable = sConfigMgr->GetOption<bool>("PvPLife.ReactiveRaidResponse.HordeEnable", true);
        _reactiveMinGroupSize = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PvPLife.ReactiveRaidResponse.MinGroupSize", 20), 2, 40);
        _reactiveMaxResponders = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PvPLife.ReactiveRaidResponse.MaxResponders", 25), 1, 40);
        std::string lvlModeStr = Lower(sConfigMgr->GetOption<std::string>("PvPLife.ReactiveRaidResponse.LevelMode", "raid"));
        _reactiveLevelMode = (lvlModeStr == "fixed") ? ResponderLevelMode::Fixed : ResponderLevelMode::Raid;
        _reactiveFixedLevel = static_cast<uint8>(std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PvPLife.ReactiveRaidResponse.FixedLevel", 60), 1, 80));
        _reactiveMaxLevelDifference = static_cast<uint8>(std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PvPLife.ReactiveRaidResponse.MaxLevelDifference", 5), 0, 30));

        _reactiveLoginOfflineBots = sConfigMgr->GetOption<bool>("PvPLife.ReactiveRaidResponse.LoginOfflineBots", false);
        _reactiveLoginDelaySeconds = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("PvPLife.ReactiveRaidResponse.LoginDelaySeconds", 20));
        _reactiveLoginTimeoutSeconds = std::max<uint32>(_reactiveLoginDelaySeconds + 5, sConfigMgr->GetOption<uint32>("PvPLife.ReactiveRaidResponse.LoginTimeoutSeconds", 60));
        _reactiveMaxConcurrentLogins = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PvPLife.ReactiveRaidResponse.MaxConcurrentLogins", 4), 1, 20);
        _reactiveCooldownSeconds = std::min<uint32>(MaxCooldownSeconds, sConfigMgr->GetOption<uint32>("PvPLife.ReactiveRaidResponse.CooldownSeconds", 1800));
        _reactiveDurationMinutes = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PvPLife.ReactiveRaidResponse.DurationMinutes", 20), 1, 1440);
        _reactiveRallyDistance = std::max<float>(20.0f, sConfigMgr->GetOption<float>("PvPLife.ReactiveRaidResponse.RallyDistance", 120.0f));
        _reactiveLaneSpacing = std::max<float>(1.0f, sConfigMgr->GetOption<float>("PvPLife.ReactiveRaidResponse.LaneSpacing", 4.0f));
        _reactiveFormationColumns = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PvPLife.ReactiveRaidResponse.FormationColumns", 9), 1, 15);

        // 8. Population Borrowing Budget
        _populationBudgetEnable = sConfigMgr->GetOption<bool>("PvPLife.Population.Enable", true);
        std::string budgetModeStr = Lower(sConfigMgr->GetOption<std::string>("PvPLife.Population.BudgetMode", "percentage"));
        _budgetMode = (budgetModeStr == "fixed") ? BudgetMode::Fixed : BudgetMode::Percentage;
        _fixedCount = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PvPLife.Population.FixedCount", 100), 2, 500);
        _borrowPercent = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PvPLife.Population.BorrowPercent", 10), 1, 100);
        _maxTotal = sConfigMgr->GetOption<uint32>("PvPLife.Population.MaxTotal", 120);
        if (_maxTotal > 1000)
            _maxTotal = 1000;
        std::string demandModeStr = Lower(sConfigMgr->GetOption<std::string>("PvPLife.Population.DemandMode", "hybrid"));
        if (demandModeStr == "fixed")
            _demandMode = DemandMode::Fixed;
        else if (demandModeStr == "dynamic")
            _demandMode = DemandMode::Dynamic;
        else
            _demandMode = DemandMode::Hybrid;
        _baselinePercent = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PvPLife.Population.BaselinePercent", 20), 1, 100);
        _burstGraceSeconds = sConfigMgr->GetOption<uint32>("PvPLife.Population.BurstGraceSeconds", 180);
        _maxChangesPerTick = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PvPLife.Population.MaxChangesPerTick", 3), 1, 20);
        _proportionalZoneScaling = sConfigMgr->GetOption<bool>("PvPLife.Population.ProportionalZoneScaling", true);

        // 9. Audit Logging
        _auditEnable = sConfigMgr->GetOption<bool>("PvPLife.Audit.Enable", true);
        _auditFilePath = sConfigMgr->GetOption<std::string>("PvPLife.Audit.FilePath", "Logs/pvplife_audit.log");
        sPvPLifeAudit.Configure(_auditEnable, _auditFilePath);

        // Sanity adjustments
        if (_arrivalStaggerMax < _arrivalStaggerMin)
            _arrivalStaggerMax = _arrivalStaggerMin;
        if (_moveAfterArrivalMax < _moveAfterArrivalMin)
            _moveAfterArrivalMax = _moveAfterArrivalMin;
        if (_duelPairDelayMax < _duelPairDelayMin)
            _duelPairDelayMax = _duelPairDelayMin;

        ImportPlayerbotDefaults();
        _startupDelaySeconds = std::min<uint32>(3600, _startupDelaySeconds);

        // Archetype filter, parsed on every load so a bad value is reported every time. A list
        // that names no known archetype fails closed: no bot fits until it is fixed.
#ifdef PVPLIFE_HAS_ARCHETYPES
        _archetypeMask = 0;
        if (!_archetypes.empty())
        {
            std::vector<std::string> unknown;
            _archetypeMask = Hs_ArchetypeMaskForNames(_archetypes, unknown);
            for (std::string const& name : unknown)
                LOG_ERROR("module", "[PvPLife] PvPLife.Bots.Archetypes: unknown archetype '{}', ignored.", name);
            if (!_archetypeMask)
                LOG_ERROR("module", "[PvPLife] PvPLife.Bots.Archetypes '{}' names no known archetype: no bot will be borrowed until it is fixed or emptied.", _archetypes);
        }
#else
        if (!_archetypes.empty())
            LOG_WARN("module", "[PvPLife] PvPLife.Bots.Archetypes is set but mod-hearthside-chat (hs_archetype.h) is not in this build; the filter is ignored.");
#endif

        _botAccountCache.clear();
        RefreshAccountCache();
        _offlineCandidateCache.clear();
        _offlineCandidateRefresh.clear();
        _offlineCandidateQueries.clear();
        if (!_botChatEnable)
            _chatQueue.clear();
        if (_botChatYellChance == 0)
            _chatQueue.erase(std::remove_if(_chatQueue.begin(), _chatQueue.end(),
                [](ChatStep const& s) { return s.Channel == ChatChannel::Yell; }), _chatQueue.end());
        if (_botChatWorldChance == 0)
            _chatQueue.erase(std::remove_if(_chatQueue.begin(), _chatQueue.end(),
                [](ChatStep const& s) { return s.Channel == ChatChannel::World; }), _chatQueue.end());
        for (ActiveEvent& event : _activeEvents)
            if ((event.EventZone.Type == ActivityType::Duel && !_duelEnable) ||
                (event.EventZone.Type == ActivityType::ForTheHorde && !_forTheHordeEnable) ||
                (event.EventZone.Type == ActivityType::ForTheAlliance && !_forTheAllianceEnable))
                EndEvent(event, "activity-disabled");
        // Re-apply the per-zone conf overrides to the zones already cached, so the DB values never
        // stand in for them while the new zone snapshot is being fetched.
        _zoneConfig.clear();
        for (Zone const& zone : _zoneCache)
            _zoneConfig[zone.Name] = BuildZoneConfig(zone);
        // _startupElapsedMs is left alone: the startup delay runs once, not after every reload.
        _timerMs = 0;
        _duelGuardTimerMs = 0;

        if (!_requireBotEligibility)
        {
            LOG_ERROR("module", "[PvPLife] SAFETY CHECK FAILED: PvPLife.Bots.RequireBotEligibility must be enabled.");
            _enable = false;
        }

        _databaseReady = VerifyDatabase();
        if (!_databaseReady)
            _enable = false;
        if (_databaseReady)
            LoadZoneConfig();

        if (wasRunning && !_enable)
        {
            // Stopped by this load: release every bot through the normal restore path (which keeps
            // running while disabled) and drop everything still waiting.
            for (ActiveEvent& e : _activeEvents)
                EndEvent(e, "disabled");
            _activeEvents.clear();
            CancelPendingRaidResponse(0, "disabled");
            PlayerbotsLife::CancelAdmissionRequest(0, PlayerbotsLife::ReservationOwner::PvPLife);
            _moveQueue.clear();
            EraseDuelSteps([](DuelStep const&) { return true; });
            _engagementQueue.clear();
            _chatQueue.clear();
        }

        if (_enable)
            PlayerbotsLife::RegisterActiveModule(PlayerbotsLife::MODULE_PVP_LIFE);
        else
            PlayerbotsLife::UnregisterActiveModule(PlayerbotsLife::MODULE_PVP_LIFE);

        LOG_INFO("module", "[PvPLife] Master: enable={} DB={} alwaysWorld={} duel={} reactive={} budget={}({}%) "
            "prefix='{}' globalMinLvl={}",
            _enable ? 1 : 0, _databaseReady ? 1 : 0, _alwaysActiveWorldPvp ? 1 : 0, _duelEnable ? 1 : 0,
            _reactiveEnable ? 1 : 0, _populationBudgetEnable ? 1 : 0, _borrowPercent,
            _botAccountPrefix, _globalMinLevel);

        AuditLog("CONFIG", "enabled=" + std::to_string(_enable ? 1 : 0) + " prefix=" + _botAccountPrefix +
            " globalMin=" + std::to_string(_globalMinLevel) + " budgetPercent=" + std::to_string(_borrowPercent) +
            " reactive=" + std::to_string(_reactiveEnable ? 1 : 0));
    }

    void Manager::ImportPlayerbotDefaults()
    {
        std::string lowered = Lower(_botAccountPrefix);
        bool autoPrefix = lowered.empty() || lowered == "auto" || lowered == "playerbot" || lowered == "playerbots";
        if (!_usePlayerbotConfig)
        {
            // Nothing to import "auto" from: match no prefix rather than accounts literally named
            // "auto...". Random bots still qualify through mod-playerbots' own account list.
            if (autoPrefix)
                _botAccountPrefix.clear();
            return;
        }

        std::string configuredPrefix = sConfigMgr->GetOption<std::string>("Playerbots.RandomBotAccountPrefix",
            sConfigMgr->GetOption<std::string>("AiPlayerbot.RandomBotAccountPrefix", "rndbot", false), false);
        uint32 loginDelay = sConfigMgr->GetOption<uint32>("Playerbots.DisabledWithoutRealPlayerLoginDelay",
            sConfigMgr->GetOption<uint32>("AiPlayerbot.DisabledWithoutRealPlayerLoginDelay", 30, false), false);
        if (autoPrefix)
            _botAccountPrefix = configuredPrefix;
        _startupDelaySeconds = std::max(_startupDelaySeconds, loginDelay);
    }

    bool Manager::VerifyDatabase()
    {
        // '_' is a LIKE wildcard, hence the escapes.
        if (!WorldDatabase.Query("SHOW TABLES LIKE 'pvp\\_life\\_zone'"))
        {
            LOG_ERROR("module", "[PvPLife] Missing WORLD table pvp_life_zone. AzerothCore's updater creates it from "
                "data/sql/db-world/base/pvp_life.sql: make sure Updates.EnableDatabases includes the world database.");
            return false;
        }
        if (!WorldDatabase.Query("SHOW TABLES LIKE 'pvp\\_life\\_chat'"))
        {
            LOG_ERROR("module", "[PvPLife] Missing WORLD table pvp_life_chat. AzerothCore's updater creates it from "
                "data/sql/db-world/base/pvp_life.sql: make sure Updates.EnableDatabases includes the world database.");
            return false;
        }

        QueryResult columnsResult = WorldDatabase.Query("SHOW COLUMNS FROM pvp_life_zone");
        if (!columnsResult)
        {
            LOG_ERROR("module", "[PvPLife] Failed to inspect columns of pvp_life_zone.");
            return false;
        }

        std::unordered_set<std::string> columns;
        do
        {
            Field* fields = columnsResult->Fetch();
            columns.insert(Lower(fields[0].Get<std::string>()));
        } while (columnsResult->NextRow());

        static const std::vector<std::string> requiredCols = {
            "id", "name", "enabled", "activity_type", "attacker_team", "defender_team",
            "min_level", "max_level", "map_id", "rally_x", "rally_y", "rally_z", "rally_o",
            "target_x", "target_y", "target_z", "target_o", "attackers_min", "attackers_max",
            "defenders_min", "defenders_max", "duration_min", "duration_max", "weight",
            "cooldown_seconds", "last_start", "challenge_players", "bot_chat"
        };

        for (std::string const& col : requiredCols)
        {
            if (!columns.count(col))
            {
                LOG_ERROR("module", "[PvPLife] Table pvp_life_zone is missing required column '{}'. Re-create it from data/sql/db-world/base/pvp_life.sql.", col);
                return false;
            }
        }

        return true;
    }

    std::string Manager::ZoneSelectSql(std::string const& suffix) const
    {
        return "SELECT id,name,enabled,activity_type,attacker_team,defender_team,min_level,max_level,map_id,"
               "rally_x,rally_y,rally_z,rally_o,target_x,target_y,target_z,target_o,"
               "attackers_min,attackers_max,defenders_min,defenders_max,duration_min,duration_max,weight,"
               "cooldown_seconds,last_start,challenge_players,bot_chat FROM pvp_life_zone " + suffix;
    }

    Zone Manager::ReadZone(Field* f)
    {
        Zone z;
        z.Id = f[0].Get<uint32>();
        z.Name = f[1].Get<std::string>();
        z.Enabled = f[2].Get<uint8>() != 0;
        uint8 type = f[3].Get<uint8>();
        uint8 attackerTeam = f[4].Get<uint8>();
        uint8 defenderTeam = f[5].Get<uint8>();
        z.Type = static_cast<ActivityType>(std::min<uint8>(type, uint8(ActivityType::ForTheAlliance)));
        z.AttackerTeam = static_cast<TeamSide>(std::min<uint8>(attackerTeam, uint8(TeamSide::Any)));
        z.DefenderTeam = static_cast<TeamSide>(std::min<uint8>(defenderTeam, uint8(TeamSide::Any)));
        z.MinLevel = f[6].Get<uint8>();
        z.MaxLevel = f[7].Get<uint8>();
        z.MapId = f[8].Get<uint32>();
        z.RallyX = f[9].Get<float>(); z.RallyY = f[10].Get<float>(); z.RallyZ = f[11].Get<float>(); z.RallyO = f[12].Get<float>();
        z.TargetX = f[13].Get<float>(); z.TargetY = f[14].Get<float>(); z.TargetZ = f[15].Get<float>(); z.TargetO = f[16].Get<float>();
        z.AttackersMin = f[17].Get<uint32>(); z.AttackersMax = f[18].Get<uint32>();
        z.DefendersMin = f[19].Get<uint32>(); z.DefendersMax = f[20].Get<uint32>();
        z.PopulationMin = z.AttackersMin + z.DefendersMin;
        z.PopulationMax = z.AttackersMax + z.DefendersMax;
        z.DurationMin = f[21].Get<uint32>(); z.DurationMax = f[22].Get<uint32>();
        z.Weight = f[23].Get<uint32>(); z.CooldownSeconds = f[24].Get<uint32>(); z.LastStart = f[25].Get<uint32>();
        z.ChallengePlayers = f[26].Get<uint8>() != 0;
        z.BotChat = f[27].Get<uint8>() != 0;
        // An unknown map id would assert in MapMgr::CreateBaseMap, so check it before GetZoneId.
        bool validMap = sMapStore.LookupEntry(z.MapId) != nullptr;
        if (!validMap || type > uint8(ActivityType::ForTheAlliance) || attackerTeam > uint8(TeamSide::Any) ||
            defenderTeam > uint8(TeamSide::Any))
        {
            LOG_ERROR("module", "[PvPLife] Zone '{}' has an invalid activity_type ({}), team ({}/{}) or map_id ({}); it is treated as disabled.",
                z.Name, type, attackerTeam, defenderTeam, z.MapId);
            z.Enabled = false;
        }
        if (validMap)
            z.ZoneId = sMapMgr->GetZoneId(PHASEMASK_NORMAL, z.MapId, z.RallyX, z.RallyY, z.RallyZ);
        return z;
    }

    Manager::ZoneConfig Manager::BuildZoneConfig(Zone const& zone) const
    {
        // A PvPLife.Zone.<name>.* key in the conf overrides the database row, GM edits included.
        ZoneConfig config;
        uint32 minimumPopulation = _minimumBotsPerSide * 2;
        uint32 maximumPopulation = _maxBotsPerSide * 2;
        std::string prefix = "PvPLife.Zone." + zone.Name;
        config.Enabled = sConfigMgr->GetOption<bool>(prefix + ".Enable", zone.Enabled, false);
        config.MinPopulation = std::clamp<uint32>(
            sConfigMgr->GetOption<uint32>(prefix + ".MinPopulation", zone.PopulationMin, false),
            minimumPopulation, maximumPopulation);
        config.MaxPopulation = std::clamp<uint32>(
            sConfigMgr->GetOption<uint32>(prefix + ".MaxPopulation", zone.PopulationMax, false),
            config.MinPopulation, maximumPopulation);
        return config;
    }

    void Manager::LoadZoneConfig()
    {
        uint64 generation = _dataGeneration;
        uint64 snapshot = ++_zoneSnapshotGeneration;
        _queryProcessor.AddCallback(WorldDatabase.AsyncQuery(ZoneSelectSql("ORDER BY id")).WithCallback(
            [this, generation, snapshot](QueryResult result)
            {
                if (generation != _dataGeneration || snapshot != _zoneSnapshotGeneration)
                    return;
                if (!result)
                {
                    // An empty table and a failed query look the same here; keep what is cached
                    // rather than stop every zone over what may be a transient failure.
                    LOG_WARN("module", "[PvPLife] Zone snapshot is empty or unavailable; keeping the {} cached zones.",
                        _zoneCache.size());
                    return;
                }
                _zoneConfig.clear();
                _zoneCache.clear();
                do
                {
                    Zone zone = ReadZone(result->Fetch());
                    _zoneConfig[zone.Name] = BuildZoneConfig(zone);
                    _zoneCache.push_back(zone);
                } while (result->NextRow());
            }));
        _queryProcessor.AddCallback(WorldDatabase.AsyncQuery(
            "SELECT activity_type,channel,speaker_team,text,weight FROM pvp_life_chat WHERE enabled=1 ORDER BY id").WithCallback(
            [this, generation, snapshot](QueryResult result)
            {
                if (generation != _dataGeneration || snapshot != _zoneSnapshotGeneration)
                    return;
                std::vector<CachedChatLine> lines;
                if (result)
                    do
                    {
                        Field* fields = result->Fetch();
                        lines.push_back({ static_cast<ActivityType>(fields[0].Get<uint8>()),
                            static_cast<ChatChannel>(fields[1].Get<uint8>()),
                            static_cast<TeamSide>(fields[2].Get<uint8>()), fields[3].Get<std::string>(),
                            std::max<uint32>(1, fields[4].Get<uint32>()) });
                    } while (result->NextRow());
                _chatCache = std::move(lines);
            }));
    }

    void Manager::ApplyZoneConfig(Zone& zone) const
    {
        auto it = _zoneConfig.find(zone.Name);
        if (it != _zoneConfig.end())
        {
            zone.Enabled = it->second.Enabled;
            zone.PopulationMin = it->second.MinPopulation;
            zone.PopulationMax = it->second.MaxPopulation;
        }
    }

    std::vector<Zone> Manager::LoadZones(bool readyOnly, int typeFilter)
    {
        std::vector<Zone> zones;
        uint32 now = GameTimeSeconds();
        for (Zone zone : _zoneCache)
        {
            ApplyZoneConfig(zone);
            if (!zone.Enabled || !zone.Weight || (typeFilter >= 0 && int(zone.Type) != typeFilter) ||
                (readyOnly && zone.LastStart && SecondsSince(now, zone.LastStart) < zone.CooldownSeconds))
                continue;
            zones.push_back(zone);
        }
        std::sort(zones.begin(), zones.end(), [](Zone const& a, Zone const& b)
        {
            return a.Weight != b.Weight ? a.Weight > b.Weight : a.Id < b.Id;
        });
        return zones;
    }

    bool Manager::LoadZoneByName(std::string name, Zone& out)
    {
        name = Lower(name);
        auto zone = std::find_if(_zoneCache.begin(), _zoneCache.end(),
            [&](Zone const& current) { return Lower(current.Name) == name; });
        if (zone == _zoneCache.end())
            return false;
        out = *zone;
        ApplyZoneConfig(out);
        return true;
    }

    TeamSide Manager::TeamForRace(uint8 race) const
    {
        switch (race)
        {
            case RACE_HUMAN:
            case RACE_DWARF:
            case RACE_NIGHTELF:
            case RACE_GNOME:
            case RACE_DRAENEI:
                return TeamSide::Alliance;
            case RACE_ORC:
            case RACE_UNDEAD_PLAYER:
            case RACE_TAUREN:
            case RACE_TROLL:
            case RACE_BLOODELF:
                return TeamSide::Horde;
            default:
                return TeamSide::Any;
        }
    }

    Player* Manager::FindPlayer(uint32 guidLow) const
    {
        return ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(guidLow));
    }

    void Manager::RefreshAccountCache()
    {
        std::string prefix = Lower(_botAccountPrefix);
        LoginDatabase.EscapeString(prefix);
        std::string sql = Acore::StringFormat(
            "SELECT id,username FROM account WHERE ({} > 0 AND id BETWEEN {} AND {}) OR "
            "({} > 0 AND LEFT(LOWER(username),{})='{}')",
            _botAccountMax, _botAccountMin, _botAccountMax, _botAccountPrefix.size(), _botAccountPrefix.size(), prefix);
        uint64 generation = _dataGeneration;
        _queryProcessor.AddCallback(LoginDatabase.AsyncQuery(sql).WithCallback([this, generation](QueryResult result)
        {
            if (generation != _dataGeneration || !result)
                return;
            do
            {
                uint32 id = result->Fetch()[0].Get<uint32>();
                bool range = _botAccountMax > 0 && id >= _botAccountMin && id <= _botAccountMax;
                bool prefixMatch = !_botAccountPrefix.empty() &&
                    Lower(result->Fetch()[1].Get<std::string>()).rfind(Lower(_botAccountPrefix), 0) == 0;
                bool all = _accountFilterMode == AccountFilterMode::All &&
                    _botAccountMax > 0 && !_botAccountPrefix.empty();
                _botAccountCache[id] = all ? range && prefixMatch : range || prefixMatch;
            } while (result->NextRow());
        }));
    }

    bool Manager::IsConfiguredBotAccount(uint32 accountId) const
    {
        auto cached = _botAccountCache.find(accountId);
        if (cached != _botAccountCache.end())
            return cached->second;
        bool range = accountId && _botAccountMax > 0 && _botAccountMax >= _botAccountMin &&
            accountId >= _botAccountMin && accountId <= _botAccountMax;
        return range && !(_accountFilterMode == AccountFilterMode::All && !_botAccountPrefix.empty());
    }

    bool Manager::IsSafeBot(Player* player, bool allowPvPLifeReservation) const
    {
        if (!player || !player->GetSession() || !player->IsInWorld() || player->IsBeingTeleported())
            return false;
        if (!sRandomPlayerbotMgr.IsRandomBot(player) &&
            !IsConfiguredBotAccount(player->GetSession()->GetAccountId()))
            return false;

        // Reserved by anyone blocks; with allowPvPLifeReservation, our own reservation does not.
        uint32 guid = player->GetGUID().GetCounter();
        if (IsLifeBotReserved(guid) &&
            !(allowPvPLifeReservation && PlayerbotsLife::IsReservedBy(guid, PlayerbotsLife::ReservationOwner::PvPLife)))
            return false;

        PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
        if (!ai || ::IsRealPlayer(player) || ai->HasGameClientMaster())
            return false;
        if (_respectPlayerbotActivity && !ai->AllowActivity(ALL_ACTIVITY))
            return false;
        if (!player->IsAlive() || player->IsInCombat() || player->IsInFlight() || player->duel)
            return false;
        if (player->InBattleground() || player->InBattlegroundQueue())
            return false;
        if (player->GetMap() && player->GetMap()->Instanceable())
            return false;
        if (_skipGroupedBots && player->GetGroup())
            return false;
        return true;
    }

    bool Manager::IsParticipant(uint32 guidLow) const
    {
        for (ActiveEvent const& e : _activeEvents)
            for (Participant const& p : e.Members)
                if (p.Bot.GuidLow == guidLow && PlayerbotsLife::ValidateLease(p.Token))
                    return true;

        for (PendingRaidResponse const& r : _pendingResponses)
            for (BotCandidate const& b : r.Bots)
                if (b.GuidLow == guidLow && PlayerbotsLife::ValidateLease(b.Token))
                    return true;

        for (ReleasingParticipant const& rp : _releasingParticipants)
            if (rp.Part.Bot.GuidLow == guidLow && PlayerbotsLife::ValidateLeaseState(rp.Part.Token, PlayerbotsLife::LeaseState::Releasing))
                return true;

        return false;
    }

    void Manager::OnPlayerLogout(Player* player)
    {
        if (!player)
            return;
        uint32 guidLow = player->GetGUID().GetCounter();
        // Undo our changes while the bot is still here: the release queue only sees it next tick,
        // when it is gone.
        for (ActiveEvent const& event : _activeEvents)
            for (Participant const& part : event.Members)
                if (part.Bot.GuidLow == guidLow && part.Deployed && PlayerbotsLife::ValidateLease(part.Token))
                    RestoreAIState(player, part.Journal);
        for (ReleasingParticipant const& releasing : _releasingParticipants)
            if (releasing.Part.Bot.GuidLow == guidLow &&
                PlayerbotsLife::ValidateLeaseState(releasing.Part.Token, PlayerbotsLife::LeaseState::Releasing))
                RestoreAIState(player, releasing.Part.Journal);
        // Not gated on IsParticipant: that misses a member still waiting for admission (no lease
        // yet), and DisengageParticipant does nothing for a bot that is not a member.
        QueueDisengage(guidLow);
    }

    void Manager::OnAddGroupMember(uint32 guidLow)
    {
        QueueDisengage(guidLow);
    }

    void Manager::QueueDisengage(uint32 guidLow)
    {
        std::lock_guard<std::mutex> lock(_queuedDisengageMutex);
        _queuedDisengages.push_back(guidLow);
    }

    void Manager::ProcessQueuedDisengages()
    {
        std::vector<uint32> queued;
        {
            std::lock_guard<std::mutex> lock(_queuedDisengageMutex);
            queued.swap(_queuedDisengages);
        }
        for (uint32 guidLow : queued)
            DisengageParticipant(guidLow);
    }

    void Manager::CaptureAIState(Player* player, AIStateJournal& journal) const
    {
        if (!player || journal.Captured)
            return;

        journal.OriginalPvPFlag = player->IsPvP();
        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(player))
        {
            journal.CombatStrategies = ai->GetStrategies(BOT_STATE_COMBAT);
            journal.NonCombatStrategies = ai->GetStrategies(BOT_STATE_NON_COMBAT);
            PositionMap& positions = ai->GetAiObjectContext()->GetValue<PositionMap&>("position")->Get();
            auto stay = positions.find("stay");
            journal.HadStayPosition = stay != positions.end() && stay->second.isSet();
            if (journal.HadStayPosition)
            {
                journal.StayX = stay->second.x;
                journal.StayY = stay->second.y;
                journal.StayZ = stay->second.z;
                journal.StayMapId = stay->second.mapId;
            }
        }
        journal.Captured = true;
    }

    void Manager::RestoreAIState(Player* player, AIStateJournal const& journal) const
    {
        if (!player || !journal.Captured || IsHumanControlled(player))
            return;

        // A real player's group takes the bot over: mod-playerbots' AcceptInvitationAction resets
        // its strategies when it joins, so there is nothing of ours left to undo.
        if (player->GetGroup())
        {
            for (GroupReference const* ref = player->GetGroup()->GetFirstMember(); ref != nullptr; ref = ref->next())
            {
                if (IsHumanPlayer(ref->GetSource()) || ::IsSelfBot(ref->GetSource()))
                    return;
            }
        }

        if (player->IsPvP() == journal.AppliedPvPFlag && player->IsPvP() != journal.OriginalPvPFlag)
            player->SetPvP(journal.OriginalPvPFlag);

        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(player))
        {
            // Reverse exactly the changes we made; anything else that changed meanwhile stays.
            for (std::string const& change : journal.AppliedCombatChanges)
                ai->ChangeStrategy((change[0] == '+' ? "-" : "+") + change.substr(1), BOT_STATE_COMBAT);
            for (std::string const& change : journal.AppliedNonCombatChanges)
                ai->ChangeStrategy((change[0] == '+' ? "-" : "+") + change.substr(1), BOT_STATE_NON_COMBAT);

            PositionMap& positions = ai->GetAiObjectContext()->GetValue<PositionMap&>("position")->Get();
            if (journal.HadStayPosition)
                positions["stay"].Set(journal.StayX, journal.StayY, journal.StayZ, journal.StayMapId);
            else if (auto stay = positions.find("stay"); stay != positions.end())
                stay->second.Reset();
        }
    }

    void Manager::CloseNativeDuel(Player* player, Participant const& part) const
    {
        if (!player || !player->duel || !part.DuelArbiter || !player->duel->Opponent ||
            player->GetGuidValue(PLAYER_DUEL_ARBITER).GetRawValue() != part.DuelArbiter ||
            player->duel->Opponent->GetGUID().GetCounter() != part.DuelOpponentGuid)
            return;
        WorldPacket packet(CMSG_DUEL_CANCELLED, 8);
        packet << player->GetGuidValue(PLAYER_DUEL_ARBITER);
        if (player->GetSession())
            player->GetSession()->HandleDuelCancelledOpcode(packet);
    }

    void Manager::RememberDuelIdentity(uint64 eventId, Player* challenger, Player* target)
    {
        if (!challenger || !target || !challenger->duel || !target->duel ||
            challenger->duel->Opponent != target || target->duel->Opponent != challenger ||
            challenger->GetGuidValue(PLAYER_DUEL_ARBITER) != target->GetGuidValue(PLAYER_DUEL_ARBITER))
            return;
        for (ActiveEvent& event : _activeEvents)
            if (event.EventId == eventId && !event.Closing)
                for (Participant& part : event.Members)
                {
                    Player* player = part.Bot.GuidLow == challenger->GetGUID().GetCounter() ? challenger :
                        (part.Bot.GuidLow == target->GetGUID().GetCounter() ? target : nullptr);
                    if (player && PlayerbotsLife::ValidateActiveLease(part.Token))
                    {
                        part.DuelArbiter = player->GetGuidValue(PLAYER_DUEL_ARBITER).GetRawValue();
                        part.DuelOpponentGuid = player->duel->Opponent->GetGUID().GetCounter();
                    }
                }
    }

    bool Manager::IsHumanControlled(Player* player) const
    {
        if (!player || !player->GetSession())
            return true;
        PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
        if (!ai || ::IsRealPlayer(player) || ::IsSelfBot(player) || ai->HasGameClientMaster())
            return true;
        if (Group* group = player->GetGroup())
            for (GroupReference const* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->GetSource(); member && (::IsRealPlayer(member) || ::IsSelfBot(member)))
                    return true;
        return false;
    }

    bool Manager::IsOwnedActor(Player* player, Participant const& part, uint64 eventId) const
    {
        if (!player || part.Token.GuidLow != part.Bot.GuidLow || part.Token.ActivityId != eventId ||
            part.Token.Owner != PlayerbotsLife::ReservationOwner::PvPLife ||
            !PlayerbotsLife::ValidateActiveLease(part.Token) || IsHumanControlled(player))
            return false;
        return IsSafeBot(player, true);
    }

    bool Manager::IsChallengeableRealPlayer(Player* target, Player* bot) const
    {
        if (!target || !bot || !IsHumanPlayer(target) || !target->IsInWorld() || target->IsGameMaster() ||
            target->GetSession()->GetSecurity() > SEC_PLAYER)
            return false;
        if (!target->IsAlive() || target->IsMounted() || target->GetVehicle() || target->IsInFlight() ||
            target->IsBeingTeleported() || target->isAFK() || target->isDND() || target->GetTradeData() ||
            target->duel || target->IsInCombat())
            return false;
        if (target->GetMap() != bot->GetMap() || target->GetTeamId() != bot->GetTeamId())
            return false;
        uint32 levelDiff = std::abs(static_cast<int32>(bot->GetLevel()) - static_cast<int32>(target->GetLevel()));
        return levelDiff <= _realPlayerMaxLevelDifference &&
            bot->GetExactDist2d(target) <= static_cast<float>(_realPlayerScanRadius);
    }

    bool Manager::CanExecuteStep(uint64 eventId, uint64 generation, uint32 guidLow, bool allowPreparing) const
    {
        auto event = std::find_if(_activeEvents.begin(), _activeEvents.end(),
            [eventId](ActiveEvent const& current) { return current.EventId == eventId; });
        uint32 now = GameTimeSeconds();
        if (event == _activeEvents.end() || event->Closing || event->EndsAt <= now ||
            (!allowPreparing && !event->FullyPrepared) || event->AdmissionRequestId != 0)
            return false;
        auto part = std::find_if(event->Members.begin(), event->Members.end(),
            [guidLow](Participant const& current) { return current.Bot.GuidLow == guidLow; });
        if (part == event->Members.end() || !part->Deployed || part->Token.Generation != generation ||
            part->Token.GuidLow != guidLow || part->Token.ActivityId != eventId ||
            part->Token.Owner != PlayerbotsLife::ReservationOwner::PvPLife ||
            !PlayerbotsLife::ValidateActiveLease(part->Token))
            return false;
        Player* player = FindPlayer(guidLow);
        return player && player->IsInWorld() && player->IsAlive() && !IsHumanControlled(player) &&
            !player->IsInFlight() && !player->InBattleground() && !player->InBattlegroundQueue() &&
            player->GetMap() && !player->GetMap()->Instanceable();
    }

    void Manager::DisengageParticipant(uint32 guidLow)
    {
        for (ActiveEvent& event : _activeEvents)
        {
            auto it = std::find_if(event.Members.begin(), event.Members.end(),
                [guidLow](Participant const& part) { return part.Bot.GuidLow == guidLow; });
            // A closing event's members are already on their way out through the release queue.
            if (it == event.Members.end() || event.Closing)
                continue;
            // A member still waiting for admission (or one whose lease was lost) has no lease to
            // release: drop it all the same. ProcessAdmissionGrants hands back any token the
            // grant later brings for it.
            bool leased = PlayerbotsLife::ValidateLease(it->Token);
            Participant departing = *it;
            event.Members.erase(it);
            if (leased)
                QueueParticipantRelease(departing, false);
            event.DuelPairs.erase(std::remove_if(event.DuelPairs.begin(), event.DuelPairs.end(),
                [guidLow](DuelPair const& pair) { return pair.ChallengerGuidLow == guidLow || pair.TargetGuidLow == guidLow; }),
                event.DuelPairs.end());
            // The staging slot belongs to the pair: free it once neither duelist is left.
            if (departing.PairId && std::none_of(event.Members.begin(), event.Members.end(),
                [&departing](Participant const& part) { return part.PairId == departing.PairId; }))
                PlayerbotsLife::ReleaseDuelStagingSlotByPair(departing.PairId, PlayerbotsLife::ReservationOwner::PvPLife);
            break;
        }
        for (PendingRaidResponse& pending : _pendingResponses)
        {
            auto it = std::find_if(pending.Bots.begin(), pending.Bots.end(),
                [guidLow](BotCandidate const& bot) { return bot.GuidLow == guidLow; });
            if (it != pending.Bots.end() && PlayerbotsLife::ValidateLease(it->Token))
            {
                PlayerbotsLife::ReleaseLeaseExact(it->Token);
                pending.Bots.erase(it);
            }
        }
        _moveQueue.erase(std::remove_if(_moveQueue.begin(), _moveQueue.end(),
            [guidLow](MoveStep const& step) { return step.GuidLow == guidLow; }), _moveQueue.end());
        EraseDuelSteps([guidLow](DuelStep const& step) { return step.ChallengerGuidLow == guidLow || step.TargetGuidLow == guidLow; });
        _engagementQueue.erase(std::remove_if(_engagementQueue.begin(), _engagementQueue.end(),
            [guidLow](EngagementStep const& step) { return step.BotGuidLow == guidLow || step.TargetGuidLow == guidLow; }), _engagementQueue.end());
        _chatQueue.erase(std::remove_if(_chatQueue.begin(), _chatQueue.end(),
            [guidLow](ChatStep const& step) { return step.SpeakerGuidLow == guidLow; }), _chatQueue.end());
    }

    uint8 Manager::EffectiveMinLevel(uint8 minLevel, ActivityType type) const
    {
        if (type == ActivityType::Duel)
            return static_cast<uint8>(std::clamp<uint32>(minLevel, 1, 80));
        uint32 effective = std::max<uint32>(minLevel, _globalMinLevel);
        return static_cast<uint8>(std::min<uint32>(effective, 80));
    }

    bool Manager::FitsArchetype(uint32 guidLow)
    {
#ifdef PVPLIFE_HAS_ARCHETYPES
        if (_archetypes.empty())
            return true;
        if (!_archetypeMask)
            return false; // a list naming no known archetype fails closed (LoadConfig logs it)
        uint64 raw = ObjectGuid::Create<HighGuid::Player>(guidLow).GetRawValue();
        uint32 archetype = static_cast<uint32>(Hs_ArchetypeForBot(raw));
        return archetype < 32 && (_archetypeMask & (1u << archetype)) != 0;
#else
        (void)guidLow;
        return true;
#endif
    }

    std::vector<BotCandidate> Manager::LoadCandidates(TeamSide team, uint8 minLevel, uint8 maxLevel,
        std::unordered_set<uint32> const& excluded, uint32 deficit)
    {
        std::vector<BotCandidate> candidates;
        if (maxLevel < minLevel)
            return candidates;

        std::vector<uint32> onlineGuids;
        {
            std::shared_lock<std::shared_mutex> playerLock(*HashMapHolder<Player>::GetLock());
            HashMapHolder<Player>::MapType const& players = ObjectAccessor::GetPlayers();
            onlineGuids.reserve(players.size());
            for (auto const& pair : players)
                if (pair.second)
                    onlineGuids.push_back(pair.second->GetGUID().GetCounter());
        }

        if (onlineGuids.empty())
            return candidates;

        // Smart Hybrid Dynamic QueryLimit:
        // 1. Calculate dynamic census ceiling
        uint32 dynamicCeiling = _botQueryLimit;
        if (dynamicCeiling == 0)
        {
            uint32 liveBots = static_cast<uint32>(onlineGuids.size());
            dynamicCeiling = std::clamp<uint32>(liveBots / 2, 100, 1500);
        }

        // 2. Demand-driven early exit target
        uint32 reqDeficit = (deficit > 0) ? deficit : 10;
        uint32 candidateTarget = std::min<uint32>(dynamicCeiling, std::max<uint32>(20, reqDeficit * 5));

        // 3. Rotational sampling across online bots
        size_t n = onlineGuids.size();
        size_t offset = urand(0, static_cast<uint32>(n - 1));

        for (size_t i = 0; i < n; ++i)
        {
            uint32 guidLow = onlineGuids[(offset + i) % n];
            Player* player = FindPlayer(guidLow);
            if (!player || !player->GetSession() || player->GetLevel() < minLevel || player->GetLevel() > maxLevel)
                continue;

            BotCandidate b;
            b.GuidLow = guidLow;
            b.AccountId = player->GetSession()->GetAccountId();
            b.Name = player->GetName();
            b.Level = player->GetLevel();
            b.Race = player->getRace();
            b.Class = player->getClass();
            b.Team = TeamForRace(b.Race);
            b.Online = true;

            if (excluded.find(b.GuidLow) != excluded.end() || IsParticipant(b.GuidLow))
                continue;
            if (PlayerbotsLife::IsReserved(b.GuidLow))
                continue;
            if (team != TeamSide::Any && b.Team != team)
                continue;
            if (!IsSafeBot(player) || !FitsArchetype(b.GuidLow))
                continue;
            candidates.push_back(b);

            if (candidates.size() >= candidateTarget)
                break;
        }

        for (size_t i = candidates.size(); i > 1; --i)
            std::swap(candidates[i - 1], candidates[urand(0, static_cast<uint32>(i - 1))]);

        return candidates;
    }

    std::vector<BotCandidate> Manager::LoadOfflineCandidates(TeamSide team, uint8 minLevel, uint8 maxLevel,
        std::unordered_set<uint32> const& excluded)
    {
        uint32 now = GameTimeSeconds();
        uint16 key = OfflineCacheKey(minLevel, maxLevel);
        auto refreshed = _offlineCandidateRefresh.find(key);
        if (!_offlineCandidateQueries.count(key) &&
            (refreshed == _offlineCandidateRefresh.end() || SecondsSince(now, refreshed->second) >= 30))
        {
            _offlineCandidateQueries.insert(key);
            uint64 generation = _dataGeneration;
            uint32 ceiling = _botQueryLimit ? _botQueryLimit : 100;
            std::string sql = Acore::StringFormat(
                "SELECT guid,account,name,level,race,class FROM characters "
                "WHERE online=0 AND level BETWEEN {} AND {} ORDER BY RAND() LIMIT {}", minLevel, maxLevel, ceiling);
            _queryProcessor.AddCallback(CharacterDatabase.AsyncQuery(sql).WithCallback(
                [this, generation, key](QueryResult result)
                {
                    if (generation != _dataGeneration)
                        return;
                    _offlineCandidateQueries.erase(key);
                    std::vector<BotCandidate> candidates;
                    if (result)
                        do
                        {
                            Field* fields = result->Fetch();
                            BotCandidate bot;
                            bot.GuidLow = fields[0].Get<uint32>();
                            bot.AccountId = fields[1].Get<uint32>();
                            bot.Name = fields[2].Get<std::string>();
                            bot.Level = fields[3].Get<uint8>();
                            bot.Race = fields[4].Get<uint8>();
                            bot.Class = fields[5].Get<uint8>();
                            bot.Team = TeamForRace(bot.Race);
                            bot.Online = false;
                            candidates.push_back(std::move(bot));
                        } while (result->NextRow());
                    _offlineCandidateCache[key] = std::move(candidates);
                    _offlineCandidateRefresh[key] = GameTimeSeconds();
                }));
        }
        std::vector<BotCandidate> candidates;
        auto cached = _offlineCandidateCache.find(key);
        if (cached == _offlineCandidateCache.end())
            return candidates;
        for (BotCandidate const& bot : cached->second)
            if (!FindPlayer(bot.GuidLow) && !excluded.count(bot.GuidLow) && !IsParticipant(bot.GuidLow) &&
                !PlayerbotsLife::IsReserved(bot.GuidLow) && (team == TeamSide::Any || bot.Team == team) &&
                IsConfiguredBotAccount(bot.AccountId) && FitsArchetype(bot.GuidLow))
                candidates.push_back(bot);
        return candidates;
    }

    float Manager::Jitter() const
    {
        if (!_positionJitter)
            return 0.0f;
        return static_cast<float>(irand(-static_cast<int32>(_positionJitter), static_cast<int32>(_positionJitter)));
    }

    uint32 Manager::GetLiveWorldBotCount() const
    {
        uint32 now = GameTimeMS();
        if (_lastCensusTimeMs && now - _lastCensusTimeMs < 5000)
            return _cachedLiveBotCount;
        uint32 count = 0;
        std::shared_lock<std::shared_mutex> playerLock(*HashMapHolder<Player>::GetLock());
        for (auto const& entry : ObjectAccessor::GetPlayers())
        {
            Player* player = entry.second;
            if (!player || !player->GetSession() || !player->IsInWorld())
                continue;
            if (IsHumanControlled(player) || !player->IsAlive() || player->InBattleground() ||
                player->InBattlegroundQueue() || !player->GetMap() || player->GetMap()->Instanceable())
                continue;
            if (sRandomPlayerbotMgr.IsRandomBot(player) ||
                IsConfiguredBotAccount(player->GetSession()->GetAccountId()))
                ++count;
        }
        _lastCensusTimeMs = now;
        _cachedLiveBotCount = count;
        return count;
    }

    void Manager::UpdateRealPlayersSnapshot() const
    {
        uint32 now = GameTimeMS();
        if (_lastRealPlayerCensusTimeMs && now - _lastRealPlayerCensusTimeMs < 3000)
            return;

        std::vector<RealPlayerSnapshotEntry> snapshot;
        uint32 nowSec = GameTimeSeconds();
        {
            std::shared_lock<std::shared_mutex> playerLock(*HashMapHolder<Player>::GetLock());
            for (auto const& entry : ObjectAccessor::GetPlayers())
            {
                Player* player = entry.second;
                if (!player || !player->IsInWorld() || !IsHumanPlayer(player))
                    continue;
                // Real-player presence per zone for Population.DemandMode; GMs do not count.
                if (!player->IsGameMaster())
                    _zoneLastRealPlayerTime[player->GetZoneId()] = nowSec;

                RealPlayerSnapshotEntry rpe;
                rpe.GuidLow = player->GetGUID().GetCounter();
                rpe.MapId = player->GetMapId();
                rpe.ZoneId = player->GetZoneId();
                rpe.AreaId = player->GetAreaId();
                rpe.Team = player->GetTeamId() == TEAM_ALLIANCE ? TeamSide::Alliance : TeamSide::Horde;
                rpe.Level = player->GetLevel();
                rpe.X = player->GetPositionX();
                rpe.Y = player->GetPositionY();
                rpe.Z = player->GetPositionZ();
                rpe.O = player->GetOrientation();
                rpe.IsAlive = player->IsAlive();
                rpe.IsPvP = player->IsPvP();
                rpe.InCombat = player->IsInCombat();
                rpe.HasDuel = (player->duel != nullptr);
                rpe.IsGameMaster = player->IsGameMaster();
                rpe.Security = player->GetSession()->GetSecurity();

                Group* group = player->GetGroup();
                rpe.IsGroupLeader = (group && group->IsLeader(player->GetGUID()));
                rpe.GroupMembersCount = group ? group->GetMembersCount() : 0;

                snapshot.push_back(std::move(rpe));
            }
        }
        _lastRealPlayerCensusTimeMs = now;
        _cachedRealPlayers = std::move(snapshot);
    }

    void Manager::PruneExpiredCooldowns()
    {
        uint32 now = GameTimeSeconds();
        auto pruneMap = [now](std::unordered_map<uint32, uint32>& map, size_t maxCap)
        {
            for (auto it = map.begin(); it != map.end();)
            {
                if (now >= it->second)
                    it = map.erase(it);
                else
                    ++it;
            }
            if (map.size() > maxCap)
            {
                std::vector<std::pair<uint32, uint32>> entries(map.begin(), map.end());
                std::sort(entries.begin(), entries.end(), [](auto const& a, auto const& b) { return a.second < b.second; });
                size_t toRemove = map.size() - maxCap;
                for (size_t i = 0; i < toRemove; ++i)
                    map.erase(entries[i].first);
            }
        };

        pruneMap(_playerChallengeCooldown, 512);
        pruneMap(_botChallengeCooldown, 512);
        pruneMap(_botDuelCooldown, 512);
    }

    uint32 Manager::CalculatePopulationBudget() const
    {
        if (!_populationBudgetEnable)
            return _maxTotal > 0 ? _maxTotal : 500;

        uint32 eligiblePool = GetLiveWorldBotCount();
        if (eligiblePool == 0)
            return 0;

        uint32 budget = _fixedCount;
        if (_budgetMode == BudgetMode::Percentage)
            budget = (eligiblePool * _borrowPercent) / 100;

        uint32 ceiling = (_maxTotal > 0) ? _maxTotal : eligiblePool;
        uint32 upper = std::min<uint32>(ceiling, eligiblePool);
        if (upper == 0)
            return 0;

        return std::clamp<uint32>(budget, 1, upper);
    }

    void Manager::CalculatePopulation(Zone const& zone, uint32& attackerCount, uint32& defenderCount) const
    {
        uint32 minimumPopulation = std::clamp<uint32>(zone.PopulationMin, 1, _maxBotsPerSide * 2);
        uint32 maximumPopulation = std::clamp(zone.PopulationMax, minimumPopulation, _maxBotsPerSide * 2);

        if (_proportionalZoneScaling)
        {
            uint32 liveBots = GetLiveWorldBotCount();
            float scale = (liveBots > 0) ? (static_cast<float>(liveBots) / 1000.0f) : 1.0f;
            scale = std::clamp(scale, 0.5f, 2.5f);
            minimumPopulation = std::max<uint32>(2, static_cast<uint32>(std::lround(minimumPopulation * scale)));
            maximumPopulation = std::max<uint32>(minimumPopulation, static_cast<uint32>(std::lround(maximumPopulation * scale)));
        }

        minimumPopulation = std::min(minimumPopulation, _maxBotsPerSide * 2);
        maximumPopulation = std::clamp(maximumPopulation, minimumPopulation, _maxBotsPerSide * 2);
        uint32 totalPopulation = urand(minimumPopulation, maximumPopulation);

        // DemandMode execution: Fixed, Dynamic, Hybrid
        uint32 zoneId = zone.ZoneId ? zone.ZoneId : sMapMgr->GetZoneId(PHASEMASK_NORMAL, zone.MapId, zone.RallyX, zone.RallyY, zone.RallyZ);
        if (_demandMode != DemandMode::Fixed && zoneId != 0)
        {
            UpdateRealPlayersSnapshot(); // refreshes the per-zone presence (throttled to every 3s)
            uint32 nowSec = GameTimeSeconds();
            bool hasRealPlayerPresence = false;
            auto it = _zoneLastRealPlayerTime.find(zoneId);
            if (it != _zoneLastRealPlayerTime.end() && SecondsSince(nowSec, it->second) <= _burstGraceSeconds)
                hasRealPlayerPresence = true;

            if (_demandMode == DemandMode::Dynamic && !hasRealPlayerPresence)
            {
                attackerCount = 0;
                defenderCount = 0;
                return;
            }
            else if (_demandMode == DemandMode::Hybrid && !hasRealPlayerPresence)
            {
                float baselineFactor = static_cast<float>(_baselinePercent) / 100.0f;
                totalPopulation = std::max<uint32>(2, static_cast<uint32>(std::lround(totalPopulation * baselineFactor)));
            }
        }

        uint32 budget = CalculatePopulationBudget();
        uint32 currentActive = PlayerbotsLife::GetCooperationSnapshot().PvPLifeCount;

        uint32 availableBudget = (budget > currentActive) ? (budget - currentActive) : 0;
        if (availableBudget < 2)
        {
            attackerCount = 0;
            defenderCount = 0;
            return;
        }

        if (totalPopulation > availableBudget)
            totalPopulation = availableBudget;
        if (totalPopulation < 2)
        {
            attackerCount = 0;
            defenderCount = 0;
            return;
        }

        // Duels come in pairs: an even total split evenly, so no bot is borrowed without a partner.
        if (zone.Type == ActivityType::Duel)
        {
            attackerCount = defenderCount = std::min(totalPopulation / 2, _maxBotsPerSide);
            return;
        }

        uint64 attackerWeight = static_cast<uint64>(zone.AttackersMin) + zone.AttackersMax;
        uint64 defenderWeight = static_cast<uint64>(zone.DefendersMin) + zone.DefendersMax;
        uint64 totalWeight = attackerWeight + defenderWeight;
        if (!totalWeight)
        {
            attackerWeight = 1;
            totalWeight = 2;
        }

        attackerCount = static_cast<uint32>(
            (static_cast<uint64>(totalPopulation) * attackerWeight + totalWeight / 2) / totalWeight);
        attackerCount = std::clamp<uint32>(attackerCount, 1, totalPopulation - 1);
        defenderCount = totalPopulation - attackerCount;

        if (attackerCount > _maxBotsPerSide)
        {
            attackerCount = _maxBotsPerSide;
            defenderCount = totalPopulation - attackerCount;
        }
        if (defenderCount > _maxBotsPerSide)
        {
            defenderCount = _maxBotsPerSide;
            attackerCount = totalPopulation - defenderCount;
        }
    }

    void Manager::ApplyPvpStrategies(Player* player, bool duelMode) const
    {
        if (!player)
            return;
        if (_forcePvpFlag)
            player->SetPvP(true);
        if (_strategiesEnable)
        {
            if (PlayerbotAI* ai = GET_PLAYERBOT_AI(player))
            {
                std::string combatStrats = _pvpCombatStrategies;
                std::string nonCombatStrats = duelMode ? _duelNonCombatStrategies : _pvpNonCombatStrategies;
                // Whichever wandering strategy mod-playerbots gave the bot (new rpg, or rpg when
                // AutoDoQuests is on and the new one is off) walks it out of the event, whatever the
                // configured sets say.
                nonCombatStrats += ",-new rpg,-rpg,-travel";
                if (!_botChatEnable || _botChatYellChance == 0)
                {
                    combatStrats += ",-chat,-emote";
                    nonCombatStrats += ",-chat,-emote";
                }
                ai->ChangeStrategy(combatStrats, BOT_STATE_COMBAT);
                ai->ChangeStrategy(nonCombatStrats, BOT_STATE_NON_COMBAT);
                if (duelMode)
                    // -mount: mod-playerbots remounts an idle bot, and the 3-second duel countdown is
                    // idle, so a duelist dismounted for the challenge got back on just to be pulled off
                    // again when the duel began (realm 2026-10-13).
                    ai->ChangeStrategy("+stay,-follow,-passive,-grind,-mount", BOT_STATE_NON_COMBAT);
            }
        }
    }

    void Manager::ScheduleMove(uint64 eventId, PlayerbotsLife::LeaseToken const& token, uint32 guidLow,
        std::string const& name, uint32 mapId, float x, float y, float z, float o, uint32 delaySeconds, bool teleport)
    {
        MoveStep step;
        step.EventId = eventId;
        step.Generation = token.Generation;
        step.Token = token;
        step.GuidLow = guidLow;
        step.Name = name;
        step.ExecuteAt = GameTimeSeconds() + delaySeconds;
        step.MapId = mapId;
        step.X = x; step.Y = y; step.Z = z; step.O = o;
        step.Teleport = teleport;
        _moveQueue.push_back(step);
        StopAntic(guidLow);
    }

    bool Manager::ScheduleDuel(uint64 eventId, uint64 pairId, PlayerbotsLife::LeaseToken const& challengerToken,
        PlayerbotsLife::LeaseToken const& targetToken, uint32 challengerGuid, uint32 targetGuid,
        std::string const& challengerName, std::string const& targetName, uint32 delaySeconds,
        bool realPlayerTarget, uint32 challengeClaimExpiry)
    {
        if (!challengerGuid || !targetGuid || challengerGuid == targetGuid)
            return false;
        std::set<std::pair<uint32, uint32>> busyPairs;
        for (DuelStep const& queued : _duelQueue)
        {
            if (queued.ChallengerGuidLow == challengerGuid || queued.TargetGuidLow == challengerGuid ||
                queued.ChallengerGuidLow == targetGuid || queued.TargetGuidLow == targetGuid)
                return false;
            if (queued.EventId == eventId)
                busyPairs.emplace(std::min(queued.ChallengerGuidLow, queued.TargetGuidLow),
                    std::max(queued.ChallengerGuidLow, queued.TargetGuidLow));
        }
        for (ActiveEvent const& event : _activeEvents)
            if (event.EventId == eventId)
                for (Participant const& participant : event.Members)
                    if (Player* player = FindPlayer(participant.Bot.GuidLow); player && player->duel && player->duel->Opponent)
                    {
                        uint32 a = player->GetGUID().GetCounter();
                        uint32 b = player->duel->Opponent->GetGUID().GetCounter();
                        busyPairs.emplace(std::min(a, b), std::max(a, b));
                    }
        if (busyPairs.size() >= _duelPairLimit)
            return false;

        DuelStep step;
        step.EventId = eventId;
        step.Generation = challengerToken.Generation;
        step.PairId = pairId;
        step.ChallengerToken = challengerToken;
        step.TargetToken = targetToken;
        step.ChallengerGuidLow = challengerGuid;
        step.TargetGuidLow = targetGuid;
        step.ChallengerName = challengerName;
        step.TargetName = targetName;
        step.ExecuteAt = GameTimeSeconds() + delaySeconds;
        step.TargetIsRealPlayer = realPlayerTarget;
        step.ChallengeClaimExpiry = challengeClaimExpiry;
        _duelQueue.push_back(step);
        StopAntic(challengerGuid);
        StopAntic(targetGuid);
        return true;
    }

    void Manager::ScheduleEngagement(uint64 eventId, PlayerbotsLife::LeaseToken const& botToken,
        uint32 botGuid, uint32 targetGuid, uint32 delaySeconds)
    {
        EngagementStep step;
        step.EventId = eventId;
        step.Generation = botToken.Generation;
        step.BotToken = botToken;
        step.BotGuidLow = botGuid;
        step.TargetGuidLow = targetGuid;
        step.ExecuteAt = GameTimeSeconds() + delaySeconds;
        step.RetryCount = 0;
        step.MaxRetries = static_cast<uint8>(_engageMaxRetries);
        _engagementQueue.push_back(step);
    }

    namespace
    {
        // A bot this close walks in; farther (or on another map) it is dropped off first. Below
        // mod-playerbots' ReactDistance (150) so its own "return to stay position" keeps walking it.
        constexpr float WalkInRange = 140.0f;
        // Drop-off ring around the destination: past what anyone standing there notices, inside WalkInRange.
        constexpr float DropOffMinDistance = 70.0f;
        constexpr float DropOffMaxDistance = 120.0f;
        constexpr uint32 DropOffAttempts = 24;
        // Riding is worth a mount cast past this.
        constexpr float MountUpDistance = 45.0f;
        // Between duels a bot idles anywhere within this of its staging spot.
        constexpr float DuelHomeAreaRadius = 6.0f;
        // Mid-duel footwork never carries a bot farther than this from its staging spot.
        constexpr float DuelFootworkHomeRadius = 25.0f;

        uint64 NowMs()
        {
            return static_cast<uint64>(GameTime::GetGameTimeMS().count());
        }
    }

    void Manager::MoveBot(Player* player, uint32 mapId, float x, float y, float z, float o, bool teleport)
    {
        if (!player || player->IsBeingTeleported())
            return;
        // A failed height probe (INVALID_HEIGHT) or a bad zone row would teleport the bot into the void.
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(o) || z <= INVALID_HEIGHT)
        {
            AuditLog("MOVE", "player=" + player->GetName() + " rejected invalid destination map=" + std::to_string(mapId));
            return;
        }

        bool sameMap = player->GetMapId() == mapId;
        if (_dropOffArrival && player->IsAlive() && (teleport || !sameMap || !_useMovePoint))
        {
            if (sameMap && player->GetExactDist2d(x, y) <= WalkInRange)
            {
                WalkBot(player, mapId, x, y, z);
                return;
            }
            Position dropOff;
            if (FindDropOff(player, mapId, x, y, z, dropOff))
            {
                if (player->IsInCombat())
                    player->CombatStop(true);
                player->TeleportTo(mapId, dropOff.GetPositionX(), dropOff.GetPositionY(), dropOff.GetPositionZ(),
                    dropOff.GetOrientation());
                PendingWalk walk;
                walk.MapId = mapId;
                walk.X = x; walk.Y = y; walk.Z = z;
                walk.DueMs = NowMs() + 2500;
                _pendingWalks[player->GetGUID().GetCounter()] = walk;
                AuditLog("DROPOFF", "player=" + player->GetName() + " map=" + std::to_string(mapId) +
                    " x=" + std::to_string(dropOff.GetPositionX()) + " y=" + std::to_string(dropOff.GetPositionY()) +
                    " walk=" + std::to_string(dropOff.GetExactDist2d(x, y)));
                return;
            }
            AuditLog("DROPOFF", "player=" + player->GetName() + " none found, teleporting map=" + std::to_string(mapId));
        }

        if (teleport || !sameMap || !_useMovePoint)
        {
            if (player->IsInCombat())
                player->CombatStop(true);
            player->TeleportTo(mapId, x, y, z, o);
        }
        else
            player->GetMotionMaster()->MovePoint(0, x, y, z);
    }

    void Manager::WalkBot(Player* player, uint32 mapId, float x, float y, float z)
    {
        if (!player || !player->IsAlive() || player->GetMapId() != mapId)
            return;
        // The duel "stay" strategy halts movement it did not start unless a stay position is set, and
        // walks the bot back to that position itself (pathfinding) when it is.
        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(player))
            if (ai->HasStrategy("stay", BOT_STATE_NON_COMBAT))
                ai->GetAiObjectContext()->GetValue<PositionMap&>("position")->Get()["stay"].Set(x, y, z, mapId);
        // Already on its way: the leash re-checks every GuardIntervalMs and must not restart the walk.
        if (player->isMoving() && player->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE)
            return;
        player->GetMotionMaster()->MovePoint(0, x, y, z);
    }

    bool Manager::FindDropOff(Player* bot, uint32 mapId, float tx, float ty, float tz, Position& dropOff) const
    {
        Map* map = sMapMgr->FindBaseNonInstanceMap(mapId);
        if (!map || map->Instanceable())
            return false;

        // Paths, heights and creatures are only known where a grid is loaded, so measure from someone
        // already standing near the destination. Nobody there means nobody to see an arrival either.
        Player* anchor = nullptr;
        std::vector<Player*> watchers;
        for (auto const& ref : map->GetPlayers())
        {
            Player* p = ref.GetSource();
            if (!p || p == bot || !p->IsInWorld() || p->IsBeingTeleported())
                continue;
            float d = p->GetExactDist2d(tx, ty);
            if (d > 250.0f)
                continue;
            if (IsHumanPlayer(p))
                watchers.push_back(p);
            if (d < 60.0f && p->IsAlive() && !p->IsFlying() && (!anchor || d < anchor->GetExactDist2d(tx, ty)))
                anchor = p;
        }
        if (!anchor)
            return false;

        // Hostile creatures that would pull the bot on its way in.
        std::list<Unit*> units;
        float searchRadius = anchor->GetExactDist2d(tx, ty) + DropOffMaxDistance + 30.0f;
        Acore::AnyUnitInObjectRangeCheck check(anchor, searchRadius);
        Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(anchor, units, check);
        Cell::VisitObjects(anchor, searcher, searchRadius);
        std::vector<Position> hostiles;
        for (Unit* unit : units)
        {
            Creature* creature = unit->ToCreature();
            if (!creature || creature->IsCritter() || creature->IsTotem() || creature->IsPet() ||
                creature->GetReactState() != REACT_AGGRESSIVE || !creature->IsHostileTo(bot) ||
                creature->GetLevel() + 10 < bot->GetLevel())
                continue;
            hostiles.push_back(creature->GetPosition());
        }
        auto nearHostile = [&](float x, float y, float radius)
        {
            return std::any_of(hostiles.begin(), hostiles.end(),
                [&](Position const& h) { return h.GetExactDist2d(x, y) < radius; });
        };

        for (uint32 attempt = 0; attempt < DropOffAttempts; ++attempt)
        {
            float angle = frand(0.0f, 2.0f * float(M_PI));
            float distance = frand(DropOffMinDistance, DropOffMaxDistance);
            float cx = tx + std::cos(angle) * distance;
            float cy = ty + std::sin(angle) * distance;
            float cz = map->GetHeight(anchor->GetPhaseMask(), cx, cy, tz + 30.0f, true, 80.0f);
            if (cz <= INVALID_HEIGHT + 1.0f || std::fabs(cz - tz) > 35.0f)
                continue;
            if (map->IsInWater(anchor->GetPhaseMask(), cx, cy, cz, bot->GetCollisionHeight()))
                continue;

            // Out of sight: nobody close, and nobody in range with a clear line to it.
            bool seen = std::any_of(watchers.begin(), watchers.end(), [&](Player* w)
            {
                float d = w->GetExactDist(cx, cy, cz);
                return d < 45.0f || (d < 110.0f && w->IsWithinLOS(cx, cy, cz + 2.0f));
            });
            if (seen || nearHostile(cx, cy, 30.0f))
                continue;

            // A plain walkable route: no partial path, no long detour around a cliff, no ledge drops,
            // no hostile camp beside the road.
            PathGenerator path(anchor);
            if (!path.CalculatePath(cx, cy, cz, tx, ty, tz, false) || path.GetPathType() != PATHFIND_NORMAL)
                continue;
            Movement::PointsArray const& points = path.GetPath();
            if (points.size() < 2)
                continue;
            float length = 0.0f;
            bool clear = true;
            for (size_t i = 1; i < points.size() && clear; ++i)
            {
                G3D::Vector3 const& a = points[i - 1];
                G3D::Vector3 const& b = points[i];
                length += (b - a).length();
                if (std::fabs(b.z - a.z) > 4.5f || nearHostile(b.x, b.y, 20.0f))
                    clear = false;
            }
            if (!clear || length > distance * 1.6f + 15.0f)
                continue;

            dropOff.Relocate(cx, cy, cz + 0.5f, std::atan2(ty - cy, tx - cx));
            return true;
        }
        return false;
    }

    bool Manager::BotActive(Player* bot) const
    {
        // mod-playerbots' own answer (Playerbots.BotActiveAlone and its force rules: a real player in
        // range or in the zone, combat, a battleground, the rotating share, SmartScale). An event
        // keeps its bots either way; an inactive one just stands where it is until it wakes.
        PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
        return ai && ai->AllowActivity();
    }

    bool Manager::RealPlayerWatching(Player* bot) const
    {
        constexpr float WatchRange = 150.0f;
        for (auto const& ref : bot->GetMap()->GetPlayers())
            if (Player* p = ref.GetSource(); p && p != bot && p->IsInWorld() && IsHumanPlayer(p) &&
                p->GetExactDist2d(bot) <= WatchRange)
                return true;
        return false;
    }

    bool Manager::IsLeasedParticipant(uint32 guidLow) const
    {
        for (ActiveEvent const& event : _activeEvents)
            if (!event.Closing)
                for (Participant const& part : event.Members)
                    if (part.Bot.GuidLow == guidLow)
                        return PlayerbotsLife::ValidateActiveLease(part.Token);
        return false;
    }

    void Manager::ProcessPendingWalks()
    {
        if (_pendingWalks.empty())
            return;
        uint64 now = NowMs();
        for (auto it = _pendingWalks.begin(); it != _pendingWalks.end();)
        {
            PendingWalk& walk = it->second;
            Player* bot = FindPlayer(it->first);
            if (!bot || bot->IsBeingTeleported() || !bot->IsInWorld())
            {
                // A cross-map teleport takes a moment; give up on one that never lands.
                if (now > walk.DueMs + 20000)
                    it = _pendingWalks.erase(it);
                else
                    ++it;
                continue;
            }
            if (now < walk.DueMs)
            {
                ++it;
                continue;
            }
            if (bot->GetMapId() != walk.MapId || !bot->IsAlive() || IsHumanControlled(bot) || !IsLeasedParticipant(it->first))
            {
                it = _pendingWalks.erase(it);
                continue;
            }
            // Ride the rest of the way. The mount action picks a flying mount where the zone allows one.
            if (!walk.MountTried && !bot->IsMounted() && bot->GetExactDist2d(walk.X, walk.Y) > MountUpDistance)
            {
                walk.MountTried = true;
                if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot))
                {
                    bool hadMount = ai->HasStrategy("mount", BOT_STATE_NON_COMBAT);
                    if (!hadMount)
                        ai->ChangeStrategy("+mount", BOT_STATE_NON_COMBAT);
                    ai->DoSpecificAction("check mount state", Event(), true);
                    if (!hadMount)
                        ai->ChangeStrategy("-mount", BOT_STATE_NON_COMBAT);
                }
                if (bot->IsNonMeleeSpellCast(false))
                {
                    walk.DueMs = now + 1800; // let the mount cast finish before moving
                    _dismountOnArrival.insert(it->first);
                    ++it;
                    continue;
                }
            }
            if (bot->IsMounted())
                _dismountOnArrival.insert(it->first);
            WalkBot(bot, walk.MapId, walk.X, walk.Y, walk.Z);
            it = _pendingWalks.erase(it);
        }
    }

    void Manager::UpdateFootwork()
    {
        if (!_footworkDuels && !_footworkWorldPvp && !_footworkBattlegrounds)
            return;
        ProcessFootworkChecks();
        if (_footworkNextMs.size() > 4096)
            _footworkNextMs.clear();
        uint64 now = NowMs();
        for (ActiveEvent const& event : _activeEvents)
        {
            bool duelEvent = event.EventZone.Type == ActivityType::Duel;
            if (event.Closing || (duelEvent ? !_footworkDuels : !_footworkWorldPvp))
                continue;
            for (Participant const& participant : event.Members)
            {
                if (!participant.Deployed)
                    continue;
                Player* bot = FindPlayer(participant.Bot.GuidLow);
                if (!bot || !bot->IsInWorld() || IsHumanControlled(bot))
                    continue;

                // Who it is fighting, and the ground it must stay on.
                Player* opponent = nullptr;
                FootworkBound bound;
                if (duelEvent)
                {
                    if (!bot->duel || bot->duel->State != DUEL_STATE_IN_PROGRESS)
                        continue;
                    opponent = bot->duel->Opponent;
                    // Out of bounds is measured from the duel flag: past 50 yards a 10-second countdown
                    // starts, back inside 40 cancels it (Player::CheckDuelDistance).
                    GameObject* flag = bot->GetMap()->GetGameObject(bot->GetGuidValue(PLAYER_DUEL_ARBITER));
                    bound.X = flag ? flag->GetPositionX() : participant.ActivityX;
                    bound.Y = flag ? flag->GetPositionY() : participant.ActivityY;
                    bound.Radius = flag ? 38.0f : float(_duelLeashRadius) - 2.0f;
                    // The flag spawns between the duelists wherever they stand, so a 38-yard
                    // allowance around it lets every duel start a little farther off than the
                    // last (realm 2026-10-12: pairs drifting onto the road). Anchor to the pair's
                    // staging spots as well: the midpoint of both duelists' own spots. Not each
                    // bot's own spot -- re-paired bots come from different lanes, and two home
                    // pulls 36 yards apart dragged a warrior and a hunter apart for a whole duel
                    // (realm 2026-10-13, Gracie and Jarenn).
                    bound.HomeX = participant.ActivityX;
                    bound.HomeY = participant.ActivityY;
                    for (Participant const& other : event.Members)
                        if (other.Bot.GuidLow == opponent->GetGUID().GetCounter())
                        {
                            bound.HomeX = (participant.ActivityX + other.ActivityX) * 0.5f;
                            bound.HomeY = (participant.ActivityY + other.ActivityY) * 0.5f;
                            break;
                        }
                    bound.HomeRadius = DuelFootworkHomeRadius;
                }
                else
                {
                    if (!bot->IsInCombat())
                        continue;
                    Unit* victim = bot->GetVictim();
                    opponent = victim ? victim->ToPlayer() : nullptr;
                    if (opponent && bot->GetExactDist2d(opponent) > 40.0f)
                        opponent = nullptr;
                }
                if (!opponent || !opponent->IsInWorld() || opponent->GetMap() != bot->GetMap())
                    continue;

                RunFootwork(bot, opponent, bound, duelEvent ? "duel" : "wpvp", now);
            }
        }
        if (_footworkBattlegrounds)
            UpdateBattlegroundFootwork(now);
    }

    void Manager::UpdateBattlegroundFootwork(uint64 now)
    {
        for (auto const& [guid, bot] : ObjectAccessor::GetPlayers())
        {
            if (!bot || !bot->IsInWorld() || !bot->InBattleground() || bot->InArena() || !bot->IsInCombat())
                continue;
            Battleground* bg = bot->GetBattleground();
            if (!bg || bg->GetStatus() != STATUS_IN_PROGRESS)
                continue;
            // Only bots running on their own AI. Unlike the leased events, a real player in the
            // same raid does not count: every battleground raid has one.
            PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
            if (!ai || ::IsRealPlayer(bot) || ::IsSelfBot(bot) || ai->HasGameClientMaster())
                continue;
            // The objective always wins: never touch a flag carrier, and never a bot on its way
            // somewhere (mod-playerbots' battleground tactics walking it to a base, a flag, an escort).
            if (bot->HasAura(BG_WS_SPELL_WARSONG_FLAG) || bot->HasAura(BG_WS_SPELL_SILVERWING_FLAG) ||
                bot->HasAura(BG_EY_NETHERSTORM_FLAG_SPELL))
                continue;
            Unit* victim = bot->GetVictim();
            Player* opponent = victim ? victim->ToPlayer() : nullptr;
            if (!opponent || !opponent->IsInWorld() || opponent->GetMap() != bot->GetMap() ||
                bot->GetExactDist2d(opponent) > 30.0f)
                continue;
            float dx, dy, dz;
            if (bot->isMoving() && bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE &&
                bot->GetMotionMaster()->GetDestination(dx, dy, dz) &&
                opponent->GetExactDist2d(dx, dy) > 15.0f && bot->GetExactDist2d(dx, dy) > 15.0f)
                continue;
            // No arena of its own to stay in: keep each move close to where the fight is.
            FootworkBound bound;
            bound.X = bot->GetPositionX();
            bound.Y = bot->GetPositionY();
            bound.Radius = 12.0f;
            RunFootwork(bot, opponent, bound, "bg", now);
        }
    }

    void Manager::RunFootwork(Player* bot, Player* opponent, FootworkBound const& bound, char const* context, uint64 now)
    {
        if (!BotActive(bot))
            return;
        uint64& next = _footworkNextMs[bot->GetGUID().GetCounter()];
        if (next > now)
            return;
        next = now + urand(_footworkIntervalMinMs, _footworkIntervalMaxMs);
        // Never cut a cast short, and leave control effects and mounts alone.
        char const* outcome = nullptr;
        if (!bot->IsAlive() || !opponent->IsAlive())
            outcome = "skip:dead";
        else if (bot->IsNonMeleeSpellCast(false) || bot->HasUnitState(UNIT_STATE_CASTING))
            outcome = "skip:casting";
        else if (bot->IsInFlight())
            outcome = "skip:flight";
        else if (bot->IsMounted())
        {
            // Fighting a player while mounted: get off (the duel-start dismount misses a bot
            // remounted by its own AI, and world PvP has no start to hook).
            bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
            outcome = "dismount";
        }
        else if (bot->IsFalling() || bot->IsInWater() || bot->HasUnitState(UNIT_STATE_JUMPING | UNIT_STATE_CHARGING))
            outcome = "skip:airborne";
        else if (bot->HasUnitState(UNIT_STATE_ROOT | UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING))
            outcome = "skip:controlled";
        else
            outcome = Footwork(bot, opponent, bound);
        if (_footworkTrace)
            AuditLog("FOOTWORK", "bot=" + bot->GetName() + " class=" + std::to_string(bot->getClass()) +
                " tab=" + std::to_string(AiFactory::GetPlayerSpecTab(bot)) +
                " dist=" + std::to_string(int(bot->GetExactDist2d(opponent))) +
                " moving=" + std::to_string(bot->isMoving() ? 1 : 0) +
                " mm=" + std::to_string(int(bot->GetMotionMaster()->GetCurrentMovementGeneratorType())) +
                " " + context + " " + outcome);
    }

    namespace
    {
        // The first spell on the list the bot can cast at this moment (known, off cooldown, in range,
        // resources), cast. nullptr when none could be.
        char const* CastFirst(PlayerbotAI* ai, Player* bot, Player* opponent, FootworkSpell const (&spells)[MaxFootworkSpells])
        {
            for (FootworkSpell const& spell : spells)
            {
                if (!spell.Name)
                    break;
                Unit* target = spell.SelfCast ? static_cast<Unit*>(bot) : static_cast<Unit*>(opponent);
                if (ai->CanCastSpell(spell.Name, target) && ai->CastSpell(spell.Name, target))
                    return spell.Name;
            }
            return nullptr;
        }

        // Rooted, frozen, stunned, feared, disoriented or slowed: the window a caster moves in.
        bool IsControlled(Player* player)
        {
            return player->HasUnitState(UNIT_STATE_ROOT | UNIT_STATE_STUNNED | UNIT_STATE_FLEEING | UNIT_STATE_CONFUSED) ||
                player->HasRootAura() || player->isFrozen() || player->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED);
        }
    }

    bool Manager::FootworkStep(Player* bot, Unit* face, float angle, float distance, Position const& from, bool jump,
        FootworkBound const& bound)
    {
        // One continuous curve around `from` (the opponent, for every move but the edge return) or a
        // jump, still facing the opponent and backpedalling when giving ground (LifeMotion::Launch).
        LifeMotion::Move move;
        move.From = from;
        move.Angle = angle;
        move.Distance = distance;
        move.Jump = jump;
        move.Face = face;
        LifeMotion::Landing landing;
        if (!LifeMotion::Launch(bot, move, bound, &landing))
            return false;

        // Measure whether anything still redirects it (PvPLife.Footwork.Trace).
        if (_footworkTrace)
        {
            FootworkCheck check;
            check.DueMs = NowMs() + uint64(std::clamp(landing.DurationMs / 2, 250, 1500));
            check.X = landing.X;
            check.Y = landing.Y;
            check.Jump = landing.Jumped;
            _footworkChecks[bot->GetGUID().GetCounter()] = check;
        }
        return true;
    }

    Participant const* Manager::FindDuelGroundParticipant(uint32 guidLow) const
    {
        for (ActiveEvent const& event : _activeEvents)
        {
            if (event.Closing || !event.FullyPrepared || event.EventZone.Type != ActivityType::Duel)
                continue;
            for (Participant const& participant : event.Members)
                if (participant.Bot.GuidLow == guidLow)
                    return participant.Deployed && PlayerbotsLife::ValidateActiveLease(participant.Token) ? &participant : nullptr;
        }
        return nullptr;
    }

    bool Manager::AnticRunning(uint32 guidLow) const
    {
        auto it = _anticRoutines.find(guidLow);
        return it != _anticRoutines.end() && LifeMotion::Running(it->second);
    }

    void Manager::StopAntic(uint32 guidLow)
    {
        auto it = _anticRoutines.find(guidLow);
        if (it != _anticRoutines.end() && LifeMotion::Running(it->second))
            LifeMotion::End(FindPlayer(guidLow), it->second, uint32(NowMs()), _antics);
    }

    bool Manager::TryStartAntic(Player* bot, uint32 guidLow, LifeMotion::Antic kind)
    {
        uint32 now = uint32(NowMs());
        LifeMotion::Routine& routine = _anticRoutines[guidLow];
        if (kind == LifeMotion::Antic::None)
        {
            if (!LifeMotion::Ready(routine, now) || !LifeMotion::RollAntic(_antics, _duelGuardIntervalMs))
                return false;
            kind = LifeMotion::PickAntic(_antics);
        }
        else if (LifeMotion::Running(routine))
            LifeMotion::End(bot, routine, now, _antics);
        if (!LifeMotion::Begin(bot, routine, kind, now, _antics))
            return false;
        if (_debug)
            AuditLog("ANTIC", "bot=" + bot->GetName() + " antic=" + LifeMotion::AnticName(kind));
        return true;
    }

    bool Manager::FidgetWalking(uint32 guidLow, Player* bot) const
    {
        auto it = _idleFidget.find(guidLow);
        return it != _idleFidget.end() && NowMs() < it->second.WalkUntilMs && bot->isMoving();
    }

    bool Manager::IdleFidget(Player* bot, Participant const& participant, ActiveEvent const& event,
        std::vector<Player*> const& fighting)
    {
        if (!_idleFidgetEnable)
            return false;
        uint64 now = NowMs();
        FidgetState& state = _idleFidget[participant.Bot.GuidLow];
        uint64 interval = uint64(urand(_idleFidgetMinSeconds, _idleFidgetMaxSeconds)) * 1000;
        if (!state.NextMs)
        {
            // First sight of this bot: start its clock at a random point, so pairs do not move in step.
            state.NextMs = now + urand(0, uint32(interval));
            return false;
        }
        if (now < state.NextMs)
            return false;
        state.NextMs = now + interval;

        uint32 roll = urand(1, 100);
        if (roll <= 45)
        {
            // A stroll to somewhere else in the home area, at walking pace.
            for (int attempt = 0; attempt < 4; ++attempt)
            {
                float angle = frand(0.0f, 2.0f * float(M_PI));
                float radius = frand(0.0f, DuelHomeAreaRadius);
                float x = participant.ActivityX + std::cos(angle) * radius;
                float y = participant.ActivityY + std::sin(angle) * radius;
                if (bot->GetExactDist2d(x, y) < 1.5f)
                    continue;
                LifeMotion::Move move;
                move.From = bot->GetPosition();
                move.Angle = std::atan2(y - bot->GetPositionY(), x - bot->GetPositionX());
                move.Distance = bot->GetExactDist2d(x, y);
                move.Walk = true;
                move.MoveStay = true;
                LifeMotion::Bound bound;
                bound.HomeX = participant.ActivityX;
                bound.HomeY = participant.ActivityY;
                bound.HomeRadius = DuelHomeAreaRadius + 1.0f;
                LifeMotion::Landing landing;
                if (LifeMotion::Launch(bot, move, bound, &landing))
                {
                    state.WalkUntilMs = now + uint64(std::max(landing.DurationMs, 0)) + 500;
                    return true;
                }
            }
        }

        // A turn: to watch a duel nearby, to the partner, to a real player close by, or anywhere.
        WorldObject* face = nullptr;
        if (roll <= 75 && !fighting.empty())
        {
            float best = 30.0f;
            for (Player* duelist : fighting)
                if (duelist != bot && bot->IsWithinDistInMap(duelist, best))
                {
                    best = bot->GetDistance(duelist);
                    face = duelist;
                }
        }
        if (!face && roll <= 85)
            for (Participant const& other : event.Members)
                if (other.PairId == participant.PairId && other.Bot.GuidLow != participant.Bot.GuidLow)
                    if (Player* partner = FindPlayer(other.Bot.GuidLow); partner && bot->IsWithinDistInMap(partner, 30.0f))
                        face = partner;
        if (!face && roll <= 92)
            for (auto const& ref : bot->GetMap()->GetPlayers())
                if (Player* p = ref.GetSource(); p && p != bot && IsHumanPlayer(p) && bot->IsWithinDistInMap(p, 20.0f))
                {
                    face = p;
                    break;
                }
        if (face)
            bot->SetFacingToObject(face);
        else
            bot->SetFacingTo(frand(0.0f, 2.0f * float(M_PI)));
        return true;
    }

    void Manager::QueueDuelEnd(Player* winner, Player* loser)
    {
        if (!_medics.Enable || !winner || !loser)
            return;
        std::lock_guard<std::mutex> lock(_duelEndMutex);
        if (_duelEnds.size() < 64)
            _duelEnds.push_back(LifeMedic::MakeDuelEnd(winner, loser));
    }

    void Manager::UpdateDuelMedics()
    {
        std::vector<LifeMedic::DuelEnd> ends;
        {
            std::lock_guard<std::mutex> lock(_duelEndMutex);
            ends.swap(_duelEnds);
        }
        uint32 now = uint32(NowMs());
        for (LifeMedic::DuelEnd const& end : ends)
        {
            // Idle duelists of this map's duel grounds: not dueling, not queued for one, not mid-antic.
            std::vector<Player*> candidates;
            for (ActiveEvent const& event : _activeEvents)
            {
                if (event.Closing || !event.FullyPrepared || event.EventZone.Type != ActivityType::Duel ||
                    event.EventZone.MapId != end.MapId)
                    continue;
                for (Participant const& participant : event.Members)
                {
                    uint32 guid = participant.Bot.GuidLow;
                    Player* bot = FindPlayer(guid);
                    if (!bot || !participant.Deployed || !PlayerbotsLife::ValidateActiveLease(participant.Token) ||
                        IsHumanControlled(bot) || !BotActive(bot) || AnticRunning(guid) ||
                        std::any_of(_duelQueue.begin(), _duelQueue.end(), [guid](DuelStep const& step)
                            { return step.ChallengerGuidLow == guid || step.TargetGuidLow == guid; }))
                        continue;
                    candidates.push_back(bot);
                }
            }
            for (LifeMedic::PendingHeal const& heal : LifeMedic::Plan(candidates, end, _medics, now))
                _pendingHeals.push_back(heal);
        }

        for (auto it = _pendingHeals.begin(); it != _pendingHeals.end();)
        {
            if (!LifeMotion::Reached(now, it->DueMs))
            {
                ++it;
                continue;
            }
            Player* healer = FindPlayer(it->HealerGuid);
            Player* target = FindPlayer(it->TargetGuid);
            if (healer && FindDuelGroundParticipant(it->HealerGuid) && !IsHumanControlled(healer) &&
                LifeMedic::Heal(healer, target) && _footworkTrace)
                AuditLog("MEDIC", "healer=" + healer->GetName() + " target=" + target->GetName());
            it = _pendingHeals.erase(it);
        }
    }

    void Manager::KeepDuelStrategies()
    {
        for (ActiveEvent const& event : _activeEvents)
        {
            if (event.Closing || event.EventZone.Type != ActivityType::Duel)
                continue;
            for (Participant const& participant : event.Members)
            {
                Player* bot = FindPlayer(participant.Bot.GuidLow);
                if (!bot || !bot->duel || !participant.Deployed || IsHumanControlled(bot) ||
                    !PlayerbotsLife::ValidateActiveLease(participant.Token))
                    continue;
                PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
                if (!ai || (ai->HasStrategy("stay", BOT_STATE_NON_COMBAT) &&
                    !ai->HasStrategy("new rpg", BOT_STATE_NON_COMBAT) && !ai->HasStrategy("mount", BOT_STATE_NON_COMBAT)))
                    continue;
                ApplyPvpStrategies(bot, true);
                // Whatever the defaults already started: stand still for the countdown.
                if (bot->duel->State == DUEL_STATE_COUNTDOWN)
                {
                    if (bot->IsMounted() || bot->HasMountedAura())
                    {
                        bot->Dismount();
                        bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
                    }
                    if (bot->isMoving())
                    {
                        bot->StopMoving();
                        bot->GetMotionMaster()->Clear();
                    }
                }
                if (_footworkTrace)
                    AuditLog("DUELSTRAT", "bot=" + bot->GetName() + " restored after mod-playerbots' duel-accept reset");
            }
        }
    }

    void Manager::UpdateAntics()
    {
        uint32 now = uint32(NowMs());

        // Riders coming in to a duel ground: some hop and get off in mid-air instead of stopping first.
        if (_antics.Enable && _antics.HopDismountChance)
        {
            for (uint32 guid : _dismountOnArrival)
            {
                if (_hopDismountRolled.count(guid))
                    continue;
                Player* bot = FindPlayer(guid);
                Participant const* participant = FindDuelGroundParticipant(guid);
                if (!bot || !participant || !bot->IsMounted() || !bot->isMoving() || IsHumanControlled(bot) ||
                    bot->GetDistance2d(participant->ActivityX, participant->ActivityY) > 9.0f)
                    continue;
                _hopDismountRolled.insert(guid);
                if (urand(1, 100) <= _antics.HopDismountChance)
                    LifeMotion::BeginHopDismount(bot, _anticRoutines[guid], participant->ActivityX, participant->ActivityY, now);
            }
        }

        for (auto& [guid, routine] : _anticRoutines)
        {
            if (!LifeMotion::Running(routine))
                continue;
            Player* bot = FindPlayer(guid);
            Participant const* participant = FindDuelGroundParticipant(guid);
            bool going = bot && participant && !IsHumanControlled(bot) &&
                (routine.Kind == LifeMotion::Antic::HopDismount || BotActive(bot));
            if (going)
            {
                // Stay clear of the neighbouring staging spots (20 yards apart) and their duels.
                LifeMotion::Bound home;
                home.HomeX = participant->ActivityX;
                home.HomeY = participant->ActivityY;
                home.HomeRadius = DuelHomeAreaRadius + 4.0f;
                going = LifeMotion::Advance(bot, routine, now, _antics, home);
            }
            if (!going)
            {
                if (routine.Kind == LifeMotion::Antic::HopDismount)
                    _dismountOnArrival.erase(guid);
                LifeMotion::End(bot, routine, now, _antics);
            }
        }
    }

    void Manager::ProcessFootworkChecks()
    {
        if (_footworkChecks.empty())
            return;
        uint64 now = NowMs();
        for (auto it = _footworkChecks.begin(); it != _footworkChecks.end();)
        {
            if (it->second.DueMs > now)
            {
                ++it;
                continue;
            }
            if (Player* bot = FindPlayer(it->first); bot && bot->IsInWorld())
            {
                // onpath: the spline footwork launched is still the one running.
                bool running = !bot->movespline->Finalized();
                G3D::Vector3 dest = bot->movespline->FinalDestination();
                bool onPath = running && std::hypot(dest.x - it->second.X, dest.y - it->second.Y) < 1.5f;
                AuditLog("FOOTCHECK", "bot=" + bot->GetName() + " jump=" + std::to_string(it->second.Jump ? 1 : 0) +
                    " onpath=" + std::to_string(onPath ? 1 : 0) + " running=" + std::to_string(running ? 1 : 0) +
                    " mm=" + std::to_string(int(bot->GetMotionMaster()->GetCurrentMovementGeneratorType())));
            }
            it = _footworkChecks.erase(it);
        }
    }

    char const* Manager::Footwork(Player* bot, Player* opponent, FootworkBound const& bound)
    {
        PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
        if (!ai)
            return "no-ai";
        FootworkProfile const& profile = FootworkProfileFor(bot->getClass(), AiFactory::GetPlayerSpecTab(bot));
        float distance = bot->GetExactDist2d(opponent);
        float away = opponent->GetAngle(bot); // from the opponent toward this bot
        float side = urand(0, 1) ? 1.0f : -1.0f;
        bool jump = urand(1, 100) <= _footworkJumpChance;
        bool controlled = IsControlled(opponent);
        Position const& them = opponent->GetPosition();

        // Back inside the duel area before anything else. A caster walks it, a melee may jump it.
        // The pull home is for footwork that wandered, never a reason to stop fighting: a bot already
        // within reach of its opponent, or one whose opponent stands nearer home than it does (so the
        // chase itself heads home), is left to fight (realm 2026-10-13: a warrior spent a whole duel
        // walking home while the hunter's pet chewed on it).
        bool chaseHeadsHome = bound.HomeRadius > 0.0f &&
            opponent->GetExactDist2d(bound.HomeX, bound.HomeY) + 5.0f < bot->GetExactDist2d(bound.HomeX, bound.HomeY);
        bool inReach = bot->IsWithinMeleeRange(opponent);
        bool offHome = bound.HomeRadius > 0.0f && !inReach && !chaseHeadsHome &&
            bot->GetExactDist2d(bound.HomeX, bound.HomeY) > bound.HomeRadius - 3.0f;
        bool offEdge = bound.Radius > 0.0f && bot->GetExactDist2d(bound.X, bound.Y) > bound.Radius - 4.0f &&
            opponent->GetExactDist2d(bound.X, bound.Y) + 5.0f >= bot->GetExactDist2d(bound.X, bound.Y);
        if (offHome || offEdge)
        {
            Position centre = offHome ? Position(bound.HomeX, bound.HomeY, bot->GetPositionZ())
                                      : Position(bound.X, bound.Y, bot->GetPositionZ());
            return FootworkStep(bot, opponent, bot->GetAngle(&centre) + frand(-0.3f, 0.3f), frand(6.0f, 9.0f), bot->GetPosition(), jump && !profile.HardCaster, FootworkBound())
                ? "edge:back-in" : "edge:blocked";
        }

        if (profile.MinRange > 0.0f)
        {
            float tooClose = profile.MinRange * 0.5f;
            float mid = (profile.MinRange + profile.MaxRange) * 0.5f;
            if (distance < tooClose)
            {
                // Distance only once the opponent is held: running from an uncontrolled melee just
                // trades casting for a chase neither side wins.
                if (controlled)
                {
                    return FootworkStep(bot, opponent, away + frand(-0.4f, 0.4f), std::min(mid, distance + 9.0f), them, jump, bound)
                        ? "close:back-off" : "close:back-off-blocked";
                }
                if (char const* cast = CastFirst(ai, bot, opponent, profile.Controls))
                    return cast;
                for (FootworkSpell const& escape : profile.Escapes)
                {
                    if (!escape.Name)
                        break;
                    if (!ai->CanCastSpell(escape.Name, bot))
                        continue;
                    // Blink carries the caster forward, Disengage leaps it backward.
                    if (std::string(escape.Name) == "disengage")
                        bot->SetFacingToObject(opponent);
                    else
                        bot->SetFacingTo(away);
                    if (ai->CastSpell(escape.Name, bot))
                        return escape.Name;
                }
                if (!profile.HardCaster)
                    return FootworkStep(bot, opponent, away + side * frand(0.8f, 1.4f), distance + frand(3.0f, 5.0f), them, jump, bound)
                        ? "close:sidestep" : "close:sidestep-blocked";
                return "close:stand"; // a caster with nothing up stands and casts
            }
            // In its band: a spec that fights with instants keeps moving, a caster keeps casting.
            if (distance > profile.MaxRange)
                return "far:ai"; // out of range: the combat AI closes in
            if (profile.HardCaster)
            {
                // Between casts a real caster shifts a few yards or hops, then casts again: worth a
                // moment's delay on the next cast, never a run (the anti-dance rule above stands).
                if (urand(1, 100) > 35)
                    return "band:cast";
                return FootworkStep(bot, opponent, away + side * frand(2.0f, 4.0f) / distance,
                    std::clamp(distance + frand(-1.5f, 1.5f), profile.MinRange, profile.MaxRange), them, jump, bound)
                    ? "band:shift" : "band:shift-blocked";
            }
            return FootworkStep(bot, opponent, away + side * frand(4.0f, 7.0f) / distance,
                std::clamp(distance + frand(-2.0f, 2.0f), profile.MinRange, profile.MaxRange), them, jump, bound)
                ? "band:strafe" : "band:strafe-blocked";
        }

        // Melee.
        if (distance > 8.0f && distance <= 25.0f)
        {
            if (char const* cast = CastFirst(ai, bot, opponent, profile.Closers))
                return cast;
            if (!jump || !bot->isMoving())
                return "gap:ai-chase";
            return FootworkStep(bot, opponent, away, std::max(distance - frand(5.0f, 7.0f), 2.0f), them, true, bound) // leap while closing
                ? "gap:leap" : "gap:leap-blocked";
        }
        if (distance > 7.0f)
            return "far:ai"; // the combat AI's own chase covers it
        if (profile.WantsBehind && opponent->HasInArc(float(M_PI), bot))
        {
            return FootworkStep(bot, opponent, opponent->GetOrientation() + float(M_PI) + frand(-0.5f, 0.5f), frand(1.5f, 2.5f), them, jump, bound)
                ? "melee:behind" : "melee:behind-blocked";
        }
        if (urand(1, 100) > 80)
            return "melee:hold";
        return FootworkStep(bot, opponent, away + side * frand(0.6f, 1.3f), frand(1.5f, 3.0f), them, jump, bound) // circle
            ? "melee:circle" : "melee:circle-blocked";
    }

    void Manager::KeepLeasedLevelsFixed()
    {
        // mod-playerbots' level brackets re-level a random bot in place (realm 2026-10-12: a leased level 9
        // dinged to 63 mid-duel-event). Its exclude lists are by name and also written by mod-hearthside-chat,
        // so only names added here are ever taken back out.
        std::unordered_set<std::string> leased;
        std::unordered_set<uint32> leasedGuids;
        for (ActiveEvent const& event : _activeEvents)
            for (Participant const& participant : event.Members)
                if (participant.Deployed && PlayerbotsLife::ValidateActiveLease(participant.Token))
                {
                    leased.insert(participant.Bot.Name);
                    leasedGuids.insert(participant.Bot.GuidLow);
                }
        for (auto it = _dismountOnArrival.begin(); it != _dismountOnArrival.end();)
            it = leasedGuids.count(*it) ? std::next(it) : _dismountOnArrival.erase(it);
        for (auto it = _idleReturnPoint.begin(); it != _idleReturnPoint.end();)
            it = leasedGuids.count(it->first) ? std::next(it) : _idleReturnPoint.erase(it);
        for (auto it = _anticRoutines.begin(); it != _anticRoutines.end();)
        {
            if (leasedGuids.count(it->first))
                ++it;
            else
            {
                LifeMotion::Halt(FindPlayer(it->first), it->second);
                it = _anticRoutines.erase(it);
            }
        }
        for (auto it = _idleFidget.begin(); it != _idleFidget.end();)
            it = leasedGuids.count(it->first) ? std::next(it) : _idleFidget.erase(it);
        for (auto it = _hopDismountRolled.begin(); it != _hopDismountRolled.end();)
            it = _dismountOnArrival.count(*it) ? std::next(it) : _hopDismountRolled.erase(it);

        auto& brackets = sPlayerbotAIConfig.LevelBracketsExcludeNames;
        auto& resets = sPlayerbotAIConfig.ResetBotLevelExcludeNames;
        for (std::string const& name : leased)
        {
            bool added = false;
            for (auto* list : { &brackets, &resets })
                if (std::find(list->begin(), list->end(), name) == list->end())
                {
                    list->push_back(name);
                    added = true;
                }
            if (added)
                _levelExcludeNamesAdded.insert(name);
        }
        for (auto it = _levelExcludeNamesAdded.begin(); it != _levelExcludeNamesAdded.end();)
        {
            if (leased.count(*it))
            {
                ++it;
                continue;
            }
            for (auto* list : { &brackets, &resets })
                list->erase(std::remove(list->begin(), list->end(), *it), list->end());
            it = _levelExcludeNamesAdded.erase(it);
        }
    }

    void Manager::EngageTarget(Player* bot, Player* target)
    {
        if (!bot || !target || !bot->IsAlive() || !target->IsAlive())
            return;
        if (bot->GetMapId() != target->GetMapId())
            return;

        if (!bot->IsValidAttackTarget(target))
            return;

        AreaTableEntry const* botArea = sAreaTableStore.LookupEntry(bot->GetAreaId());
        AreaTableEntry const* targetArea = sAreaTableStore.LookupEntry(target->GetAreaId());
        if ((botArea && (botArea->flags & AREA_FLAG_SANCTUARY)) || (targetArea && (targetArea->flags & AREA_FLAG_SANCTUARY)))
            return;

        ApplyPvpStrategies(bot, false);

        bot->SetSelection(target->GetGUID());
        bot->Attack(target, true);
    }

    void Manager::DeployEventMembers(ActiveEvent& event, uint32& changesCount, uint32 maxChanges)
    {
        if (event.Closing || event.AdmissionRequestId != 0 || event.FullyPrepared)
            return;
        uint32 now = GameTimeSeconds();
        std::vector<uint32> dropped;
        std::vector<uint32> defenderGuids;
        for (Participant const& member : event.Members)
            if (!member.Attacker)
                defenderGuids.push_back(member.Bot.GuidLow);
        auto deploy = [&](Participant& part, bool duelMode, uint32 index)
        {
            if (part.Deployed)
                return true;
            if (changesCount >= maxChanges)
                return false;
            Player* player = FindPlayer(part.Bot.GuidLow);
            if (!IsOwnedActor(player, part, event.EventId))
            {
                // Wait out combat, a duel, a teleport or a flight. Anything else (logged out, dead, lease
                // lost, grouped with a player) is not going to clear up: let that bot go rather
                // than hold the whole event until its prepare deadline.
                if (!player || !(player->IsInCombat() || player->IsBeingTeleported() || player->IsInFlight() || player->duel))
                    dropped.push_back(part.Bot.GuidLow);
                return false;
            }
            part.OriginalMap = player->GetMapId();
            part.OriginalX = player->GetPositionX();
            part.OriginalY = player->GetPositionY();
            part.OriginalZ = player->GetPositionZ();
            part.OriginalO = player->GetOrientation();
            CaptureAIState(player, part.Journal);
            ApplyPvpStrategies(player, duelMode);
            if (PlayerbotAI* ai = GET_PLAYERBOT_AI(player))
            {
                part.Journal.AppliedCombatChanges = StrategyDelta(part.Journal.CombatStrategies, ai->GetStrategies(BOT_STATE_COMBAT));
                part.Journal.AppliedNonCombatChanges = StrategyDelta(part.Journal.NonCombatStrategies, ai->GetStrategies(BOT_STATE_NON_COMBAT));
            }
            part.Journal.AppliedPvPFlag = player->IsPvP();
            if (sRandomPlayerbotMgr.IsRandomBot(player))
                sRandomPlayerbotMgr.ScheduleTeleport(part.Bot.GuidLow, event.EndsAt - now + 60);
            uint32 arrival = urand(_arrivalStaggerMin, _arrivalStaggerMax) + index;
            part.Deployed = true;
            ++changesCount;
            ScheduleMove(event.EventId, part.Token, part.Bot.GuidLow, part.Bot.Name, event.EventZone.MapId,
                part.ActivityX, part.ActivityY, part.ActivityZ, part.ActivityO, arrival, _teleportOnArrival);
            if (!duelMode && part.Attacker)
            {
                uint32 moveDelay = arrival + urand(_moveAfterArrivalMin, _moveAfterArrivalMax);
                ScheduleMove(event.EventId, part.Token, part.Bot.GuidLow, part.Bot.Name, event.EventZone.MapId,
                    event.EventZone.TargetX + Jitter(), event.EventZone.TargetY + Jitter(),
                    event.EventZone.TargetZ, event.EventZone.TargetO, moveDelay, _hardTeleportToTarget);
                // Spread the attackers over the defenders instead of sending all at the first one.
                uint32 targetGuid = event.TriggerGuidLow;
                if (event.Origin != EventOrigin::ReactiveRaidResponse && !defenderGuids.empty())
                    targetGuid = defenderGuids[index % defenderGuids.size()];
                if (_forceAttackOnEngage && targetGuid)
                    ScheduleEngagement(event.EventId, part.Token, part.Bot.GuidLow, targetGuid,
                        moveDelay + _engageAfterMoveSeconds);
            }
            return true;
        };
        if (event.EventZone.Type == ActivityType::Duel)
        {
            for (uint32 index = 0; index < event.DuelPairs.size(); ++index)
            {
                DuelPair& pair = event.DuelPairs[index];
                if (pair.Deployed)
                    continue;
                auto a = std::find_if(event.Members.begin(), event.Members.end(),
                    [&](Participant const& part) { return part.Bot.GuidLow == pair.ChallengerGuidLow; });
                auto b = std::find_if(event.Members.begin(), event.Members.end(),
                    [&](Participant const& part) { return part.Bot.GuidLow == pair.TargetGuidLow; });
                if (a == event.Members.end() || b == event.Members.end())
                    continue;
                bool firstReady = deploy(*a, true, index);
                bool secondReady = deploy(*b, true, index);
                if (!firstReady || !secondReady)
                    continue;
                // Room for the walk in from a drop-off, then each pair on its own clock: a shared
                // formula started the whole first round at once (realm 2026-10-13).
                uint32 delay = _arrivalStaggerMax + 20 + urand(5, 40);
                if (ScheduleDuel(event.EventId, pair.PairId, a->Token, b->Token,
                    a->Bot.GuidLow, b->Bot.GuidLow, a->Bot.Name, b->Bot.Name, delay))
                    pair.Deployed = true;
            }
        }
        else
            for (uint32 index = 0; index < event.Members.size(); ++index)
                deploy(event.Members[index], false, index);

        for (uint32 guid : dropped)
            DisengageParticipant(guid);
        // A duelist whose partner left before deploying has no pair to duel in: let it go too.
        if (event.EventZone.Type == ActivityType::Duel)
        {
            std::vector<uint32> orphans;
            for (Participant const& part : event.Members)
                if (!part.Deployed && std::none_of(event.DuelPairs.begin(), event.DuelPairs.end(), [&part](DuelPair const& pair)
                    { return pair.ChallengerGuidLow == part.Bot.GuidLow || pair.TargetGuidLow == part.Bot.GuidLow; }))
                    orphans.push_back(part.Bot.GuidLow);
            for (uint32 guid : orphans)
                DisengageParticipant(guid);
        }
        uint32 remainingAttackers = static_cast<uint32>(std::count_if(event.Members.begin(), event.Members.end(),
            [](Participant const& part) { return part.Attacker; }));
        uint32 remainingDefenders = static_cast<uint32>(event.Members.size()) - remainingAttackers;
        if (remainingAttackers < event.RequiredAttackers || remainingDefenders < event.RequiredDefenders ||
            remainingAttackers + remainingDefenders < std::max<uint32>(1, event.MinimumTotal))
        {
            EndEvent(event, "insufficient-members");
            return;
        }

        uint32 attackers = static_cast<uint32>(std::count_if(event.Members.begin(), event.Members.end(),
            [](Participant const& part) { return part.Deployed && part.Attacker; }));
        uint32 defenders = static_cast<uint32>(std::count_if(event.Members.begin(), event.Members.end(),
            [](Participant const& part) { return part.Deployed && !part.Attacker; }));
        if (attackers < event.RequiredAttackers || defenders < event.RequiredDefenders ||
            attackers + defenders < event.MinimumTotal)
            return;
        if (std::all_of(event.Members.begin(), event.Members.end(),
            [](Participant const& part) { return part.Deployed; }))
        {
            event.FullyPrepared = true;
            event.StartedAt = now;
            event.EndsAt = now + event.DurationSeconds;
            if (event.EventZone.BotChat && _botChatEnable)
                ScheduleEventChat(event);
            // Announced once the bots are in place, not when admission was only requested.
            if (event.Origin == EventOrigin::Zone)
                Announce("[World PvP] " + std::string(TypeName(event.EventZone.Type)) + " at " + event.EventZone.Name);
        }
    }

    void Manager::ProcessAdmissionGrants()
    {
        uint32 now = GameTimeSeconds();
        for (ActiveEvent& event : _activeEvents)
        {
            if (!event.AdmissionRequestId || event.Closing)
                continue;
            if (event.PrepareDeadline <= now)
            {
                EndEvent(event, "admission-timeout");
                continue;
            }
            std::vector<PlayerbotsLife::LeaseToken> tokens;
            if (!PlayerbotsLife::TakeAdmissionGrant(event.AdmissionRequestId, PlayerbotsLife::ReservationOwner::PvPLife, tokens))
                continue;
            event.AdmissionRequestId = 0;
            // Tokens for members that left while waiting go straight back.
            for (auto const& token : tokens)
                if (std::none_of(event.Members.begin(), event.Members.end(),
                    [&token](Participant const& part) { return part.Bot.GuidLow == token.GuidLow; }))
                    PlayerbotsLife::ReleaseLeaseExact(token);
            // A member without a usable lease (the grant may hand back fewer tokens than asked for)
            // is dropped; DeployEventMembers ends the event if the minimums are no longer met.
            std::unordered_set<uint32> missing;
            for (Participant& part : event.Members)
            {
                auto token = std::find_if(tokens.begin(), tokens.end(),
                    [&](PlayerbotsLife::LeaseToken const& lease) { return lease.GuidLow == part.Bot.GuidLow; });
                if (token == tokens.end())
                {
                    missing.insert(part.Bot.GuidLow);
                    continue;
                }
                part.Token = *token;
                if (!PlayerbotsLife::PromoteLease(part.Token))
                {
                    PlayerbotsLife::ReleaseLeaseExact(part.Token);
                    missing.insert(part.Bot.GuidLow);
                }
            }
            event.Members.erase(std::remove_if(event.Members.begin(), event.Members.end(),
                [&missing](Participant const& part) { return missing.count(part.Bot.GuidLow) != 0; }), event.Members.end());
            event.DuelPairs.erase(std::remove_if(event.DuelPairs.begin(), event.DuelPairs.end(), [&missing](DuelPair const& pair)
                { return missing.count(pair.ChallengerGuidLow) != 0 || missing.count(pair.TargetGuidLow) != 0; }), event.DuelPairs.end());
            if (event.Members.empty() ||
                PlayerbotsLife::GetCooperationSnapshot().PvPLifeCount > CalculatePopulationBudget())
                EndEvent(event, "invalid-admission");
        }
        for (PendingRaidResponse& pending : _pendingResponses)
        {
            if (!pending.AdmissionRequestId || pending.Cancelled)
                continue;
            std::vector<PlayerbotsLife::LeaseToken> tokens;
            if (!PlayerbotsLife::TakeAdmissionGrant(pending.AdmissionRequestId, PlayerbotsLife::ReservationOwner::PvPLife, tokens))
                continue;
            pending.AdmissionRequestId = 0;
            for (auto const& token : tokens)
                if (std::none_of(pending.Bots.begin(), pending.Bots.end(),
                    [&token](BotCandidate const& bot) { return bot.GuidLow == token.GuidLow; }))
                    PlayerbotsLife::ReleaseLeaseExact(token);
            // Promote now: a provisional lease is scavenged after 30s, and the response only starts
            // LoginDelaySeconds later. Bots without a usable lease are dropped from the response.
            for (BotCandidate& bot : pending.Bots)
            {
                auto token = std::find_if(tokens.begin(), tokens.end(),
                    [&](PlayerbotsLife::LeaseToken const& lease) { return lease.GuidLow == bot.GuidLow; });
                bot.Token = token != tokens.end() ? *token : PlayerbotsLife::LeaseToken();
                if (token != tokens.end() && !PlayerbotsLife::PromoteLease(bot.Token))
                {
                    PlayerbotsLife::ReleaseLeaseExact(bot.Token);
                    bot.Token = PlayerbotsLife::LeaseToken();
                }
            }
            pending.Bots.erase(std::remove_if(pending.Bots.begin(), pending.Bots.end(),
                [](BotCandidate const& bot) { return bot.Token.GuidLow == 0; }), pending.Bots.end());
            pending.LeasesGranted = !pending.Bots.empty();
            if (!pending.LeasesGranted || pending.Deadline <= now)
            {
                pending.Cancelled = true;
                for (BotCandidate const& bot : pending.Bots)
                    PlayerbotsLife::ReleaseLeaseExact(bot.Token);
                continue;
            }
            for (uint32 guid : pending.PendingLoginGuids)
            {
                auto bot = std::find_if(pending.Bots.begin(), pending.Bots.end(),
                    [guid](BotCandidate const& candidate) { return candidate.GuidLow == guid; });
                if (bot != pending.Bots.end() && PlayerbotsLife::ValidateActiveLease(bot->Token))
                    sRandomPlayerbotMgr.AddPlayerBot(ObjectGuid::Create<HighGuid::Player>(guid), 0);
            }
        }
    }

    void Manager::ProcessPreparingEvents()
    {
        uint32 now = GameTimeSeconds();
        for (ActiveEvent& event : _activeEvents)
        {
            if (event.Closing || event.FullyPrepared || event.AdmissionRequestId != 0)
                continue;
            if (event.PrepareDeadline <= now)
            {
                // Members still undeployed at the deadline (stuck in combat, a duel or a flight) are
                // let go; if the ones that did deploy still meet the event's minimums, it starts
                // without them instead of being called off.
                uint32 attackers = static_cast<uint32>(std::count_if(event.Members.begin(), event.Members.end(),
                    [](Participant const& part) { return part.Deployed && part.Attacker; }));
                uint32 defenders = static_cast<uint32>(std::count_if(event.Members.begin(), event.Members.end(),
                    [](Participant const& part) { return part.Deployed && !part.Attacker; }));
                if (attackers < event.RequiredAttackers || defenders < event.RequiredDefenders ||
                    attackers + defenders < std::max<uint32>(1, event.MinimumTotal))
                {
                    EndEvent(event, "prep-timeout");
                    continue;
                }
                std::vector<uint32> late;
                for (Participant const& part : event.Members)
                    if (!part.Deployed)
                        late.push_back(part.Bot.GuidLow);
                for (uint32 guid : late)
                    DisengageParticipant(guid);
                LOG_INFO("module", "[PvPLife] Event #{} {} starting without {} undeployed member(s) (attackers={}, defenders={})",
                    event.EventId, event.EventZone.Name, late.size(), attackers, defenders);
                AuditLog("PARTIAL_START", "event=" + std::to_string(event.EventId) + " zone=" + event.EventZone.Name +
                    " dropped=" + std::to_string(late.size()));
            }
            // With every remaining member deployed, this starts the event.
            if (_changesThisTick < _maxChangesPerTick || event.PrepareDeadline <= now)
                DeployEventMembers(event, _changesThisTick, _maxChangesPerTick);
        }
        _activeEvents.erase(std::remove_if(_activeEvents.begin(), _activeEvents.end(),
            [](ActiveEvent const& event) { return event.Closing; }), _activeEvents.end());
    }

    bool Manager::StartZone(Zone const& zone, ChatHandler* handler, bool forced)
    {
        if (!_enable || !_databaseReady)
        {
            if (handler)
                handler->PSendSysMessage("PvP Life: The module is disabled; nothing was started.");
            return false;
        }
        if (!zone.Enabled && !forced)
            return false;
        if (zone.Type == ActivityType::Duel && !_duelEnable && !forced)
            return false;
        if (zone.Type == ActivityType::ForTheHorde && !_forTheHordeEnable && !forced)
            return false;
        if (zone.Type == ActivityType::ForTheAlliance && !_forTheAllianceEnable && !forced)
            return false;
        if (!sMapStore.LookupEntry(zone.MapId))
        {
            if (handler)
                handler->PSendSysMessage("PvP Life: {} has an unknown map id {}.", zone.Name, zone.MapId);
            return false;
        }
        if (IsWintergraspWarZone(zone.ZoneId))
        {
            if (handler)
                handler->PSendSysMessage("PvP Life: {} lies in Wintergrasp, where the battle is on.", zone.Name);
            return false;
        }

        if (_maxActiveEvents > 0 && _activeEvents.size() >= _maxActiveEvents && !forced)
            return false;

        if (!forced && _maxChangesPerTick > 0 && _changesThisTick >= _maxChangesPerTick)
            return false;

        for (ActiveEvent const& e : _activeEvents)
            if (e.EventZone.Id == zone.Id)
            {
                if (handler)
                    handler->PSendSysMessage("PvP Life: {} is already running as #{}.", zone.Name, e.EventId);
                return false;
            }

        // A skirmish between "any" and "any" would draw both sides from one pool and pit bots of
        // the same faction against each other: pick opposing factions for this run instead.
        Zone eventZone = zone;
        if (eventZone.Type != ActivityType::Duel && eventZone.AttackerTeam == TeamSide::Any &&
            eventZone.DefenderTeam == TeamSide::Any)
        {
            bool hordeAttacks = urand(0, 1) == 1;
            eventZone.AttackerTeam = hordeAttacks ? TeamSide::Horde : TeamSide::Alliance;
            eventZone.DefenderTeam = hordeAttacks ? TeamSide::Alliance : TeamSide::Horde;
        }

        std::unordered_set<uint32> excluded;
        for (ActiveEvent const& e : _activeEvents)
            for (Participant const& p : e.Members)
                excluded.insert(p.Bot.GuidLow);
        for (PendingRaidResponse const& r : _pendingResponses)
            for (BotCandidate const& b : r.Bots)
                excluded.insert(b.GuidLow);
        for (ReleasingParticipant const& rp : _releasingParticipants)
            excluded.insert(rp.Part.Bot.GuidLow);

        uint32 attackerNeed = 0;
        uint32 defenderNeed = 0;
        CalculatePopulation(eventZone, attackerNeed, defenderNeed);
        uint8 effMinLevel = EffectiveMinLevel(eventZone.MinLevel, eventZone.Type);
        std::vector<BotCandidate> attackerPool =
            LoadCandidates(eventZone.AttackerTeam, effMinLevel, eventZone.MaxLevel, excluded, attackerNeed);
        std::vector<BotCandidate> defenderPool =
            LoadCandidates(eventZone.DefenderTeam, effMinLevel, eventZone.MaxLevel, excluded, defenderNeed);
        std::unordered_set<uint32> attackerIds;
        std::unordered_set<uint32> defenderIds;
        for (BotCandidate const& bot : attackerPool)
            attackerIds.insert(bot.GuidLow);
        for (BotCandidate const& bot : defenderPool)
            defenderIds.insert(bot.GuidLow);

        std::vector<BotCandidate> attackers;
        std::vector<BotCandidate> defenders;
        if (eventZone.Type == ActivityType::Duel)
        {
            // Same level about a third of the time, otherwise one or two apart.
            uint32 maxGap = std::min<uint32>(2, _botDuelMaxLevelDifference);
            auto pairs = SelectDiverseDuelPairs(attackerPool, defenderPool,
                std::min(attackerNeed, defenderNeed), _botDuelMaxLevelDifference,
                [maxGap]() { return unsigned(urand(0, maxGap)); });
            attackers = std::move(pairs.first);
            defenders = std::move(pairs.second);
        }
        else
        {
            std::vector<BotCandidate> shared;
            for (BotCandidate const& bot : attackerPool)
            {
                if (defenderIds.count(bot.GuidLow))
                    shared.push_back(bot);
                else if (attackers.size() < attackerNeed)
                    attackers.push_back(bot);
            }
            for (BotCandidate const& bot : defenderPool)
                if (!attackerIds.count(bot.GuidLow) && defenders.size() < defenderNeed)
                    defenders.push_back(bot);

            for (BotCandidate const& bot : shared)
            {
                bool attackerNeedsBot = attackers.size() < attackerNeed;
                bool defenderNeedsBot = defenders.size() < defenderNeed;
                if (!attackerNeedsBot && !defenderNeedsBot)
                    break;

                if (attackerNeedsBot && (!defenderNeedsBot || attackers.size() <= defenders.size()))
                    attackers.push_back(bot);
                else
                    defenders.push_back(bot);
            }
        }

        uint32 requiredAttackers = _allowPartialTeams ? _minimumBotsPerSide : attackerNeed;
        uint32 requiredDefenders = _allowPartialTeams ? _minimumBotsPerSide : defenderNeed;
        // Duels come in pairs: without partial teams a duel zone needs as many full pairs as the
        // smaller side asked for (capped by the pair limit), not each side's own count.
        if (eventZone.Type == ActivityType::Duel && !_allowPartialTeams)
            requiredAttackers = requiredDefenders = std::min({ attackerNeed, defenderNeed, _duelPairLimit });
        // One pair is a whole duel event, which MinOnlineRequired (default 3) would turn away.
        uint32 requiredTotal = eventZone.Type == ActivityType::Duel ? 2 : _minOnlineRequired;
        uint32 now = GameTimeSeconds();

        if (attackers.size() < requiredAttackers || defenders.size() < requiredDefenders ||
            (attackers.size() + defenders.size()) < requiredTotal)
        {
            if (_debug)
                LOG_INFO("module", "[PvPLife] '{}' lacks bots: attackers {}/{} defenders {}/{} total {} (min required {})",
                    zone.Name, attackers.size(), attackerNeed, defenders.size(), defenderNeed,
                    attackers.size() + defenders.size(), requiredTotal);
            if (handler)
                handler->PSendSysMessage("PvP Life: Insufficient eligible bots for {} (attackers {}/{}, defenders {}/{}).",
                    zone.Name, attackers.size(), attackerNeed, defenders.size(), defenderNeed);
            return false;
        }

        ActiveEvent event;
        event.EventId = _nextEventId++;
        event.EventZone = eventZone;
        event.Origin = EventOrigin::Zone;
        event.StartedAt = now;
        uint32 dmin = std::max<uint32>(1, zone.DurationMin);
        uint32 dmax = std::max(dmin, zone.DurationMax);
        event.DurationSeconds = urand(dmin, dmax) * MINUTE;
        event.RequiredAttackers = requiredAttackers;
        event.RequiredDefenders = requiredDefenders;
        event.MinimumTotal = requiredTotal;
        uint32 plannedMembers = static_cast<uint32>(attackers.size() + defenders.size());
        uint32 queuedWork = plannedMembers + static_cast<uint32>(_releasingParticipants.size());
        for (ActiveEvent const& queued : _activeEvents)
            if (!queued.Closing)
                queuedWork += static_cast<uint32>(std::count_if(queued.Members.begin(), queued.Members.end(),
                    [](Participant const& part) { return !part.Deployed; }));
        uint32 batches = (queuedWork + _maxChangesPerTick - 1) / _maxChangesPerTick;
        event.PrepareDeadline = now + std::max<uint32>(60, 2 * _tickSeconds * batches + _arrivalStaggerMax);
        event.EndsAt = event.PrepareDeadline + event.DurationSeconds;

        // Nothing is leased or queued before admission; only claimed staging slots need undoing.
        auto rollback = [&]()
        {
            PlayerbotsLife::ReleaseDuelStagingSlotByActivity(event.EventId, PlayerbotsLife::ReservationOwner::PvPLife);
            event.Members.clear();
            event.DuelPairs.clear();
        };

        if (eventZone.Type == ActivityType::Duel)
        {
            uint32 numPairs = static_cast<uint32>(std::min(attackers.size(), defenders.size()));
            numPairs = std::min(numPairs, _duelPairLimit);
            // The registry clamps the duration below 2^31 ms; compute it without uint32 overflow.
            uint32 eventDurationMs = static_cast<uint32>(std::min<uint64>(
                uint64(event.EndsAt - now) * IN_MILLISECONDS, 0x7FFFFFFFu));

            for (uint32 i = 0; i < numPairs; ++i)
            {
                BotCandidate const& candA = attackers[i];
                BotCandidate const& candB = defenders[i];

                Player* playerA = FindPlayer(candA.GuidLow);
                Player* playerB = FindPlayer(candB.GuidLow);
                if (!IsSafeBot(playerA) || !IsSafeBot(playerB))
                    continue;

                Participant partA;
                Participant partB;
                uint64 pairId = _nextPairId++;

                // Duel pair staging: allocate a single DuelStagingSlot per pair (PV-17)
                uint32 duelJitter = std::min<uint32>(_positionJitter, 1);
                float laneOffset = (float(i) - float(numPairs - 1) * 0.5f) * 8.0f;
                float sx = (zone.RallyX + zone.TargetX) * 0.5f +
                    static_cast<float>(irand(-static_cast<int32>(duelJitter), static_cast<int32>(duelJitter)));
                float sy = (zone.RallyY + zone.TargetY) * 0.5f + laneOffset +
                    static_cast<float>(irand(-static_cast<int32>(duelJitter), static_cast<int32>(duelJitter)));
                float sz = (zone.RallyZ + zone.TargetZ) * 0.5f;

                PlayerbotsLife::DuelStagingSlot slot;
                uint32 slotId = 0;
                if (!PlayerbotsLife::ClaimDuelStagingSlot(zone.MapId, sx, sy, sz, 30.0f, event.EventId,
                    PlayerbotsLife::ReservationOwner::PvPLife, GameTimeMS(), eventDurationMs, slot, pairId))
                    continue; // Defer the pair: never deploy at an unreserved fallback centre.

                slotId = slot.SlotId;
                sx = slot.CenterX;
                sy = slot.CenterY;
                // A slot displaced along the lattice keeps the requested height; snap to the ground.
                sz = GroundZ(zone.MapId, sx, sy, slot.CenterZ);

                // Each pair faces its own way (60 degrees apart, plus a little noise) and stands a
                // slightly different distance apart, instead of every pair lined up on RallyO.
                float baseAngle = Position::NormalizeOrientation(zone.RallyO + float(i) * float(M_PI) / 3.0f +
                    frand(-0.35f, 0.35f));
                float offsetDist = 2.0f + frand(0.0f, 2.5f); // 4 to 9 yards apart
                float ax = sx - std::cos(baseAngle) * offsetDist;
                float ay = sy - std::sin(baseAngle) * offsetDist;
                float az = GroundZ(zone.MapId, ax, ay, sz);
                float ao = baseAngle;

                float bx = sx + std::cos(baseAngle) * offsetDist;
                float by = sy + std::sin(baseAngle) * offsetDist;
                float bz = GroundZ(zone.MapId, bx, by, sz);
                float bo = Position::NormalizeOrientation(baseAngle + 3.14159265f);

                // Setup Part A
                partA.Bot = candA;
                partA.Attacker = true;
                partA.OriginalMap = playerA->GetMapId();
                partA.OriginalX = playerA->GetPositionX();
                partA.OriginalY = playerA->GetPositionY();
                partA.OriginalZ = playerA->GetPositionZ();
                partA.OriginalO = playerA->GetOrientation();
                partA.ActivityX = ax; partA.ActivityY = ay; partA.ActivityZ = az; partA.ActivityO = ao;
                partA.PairId = pairId;
                partA.DuelStagingSlotId = slotId;
                partA.Deployed = false;

                // Setup Part B
                partB.Bot = candB;
                partB.Attacker = false;
                partB.OriginalMap = playerB->GetMapId();
                partB.OriginalX = playerB->GetPositionX();
                partB.OriginalY = playerB->GetPositionY();
                partB.OriginalZ = playerB->GetPositionZ();
                partB.OriginalO = playerB->GetOrientation();
                partB.ActivityX = bx; partB.ActivityY = by; partB.ActivityZ = bz; partB.ActivityO = bo;
                partB.PairId = pairId;
                partB.DuelStagingSlotId = slotId;
                partB.Deployed = false;

                event.Members.push_back(partA);
                event.Members.push_back(partB);

                DuelPair dp;
                dp.PairId = pairId;
                dp.EventId = event.EventId;
                dp.ChallengerGuidLow = candA.GuidLow;
                dp.TargetGuidLow = candB.GuidLow;
                dp.StagingSlotId = slotId;
                dp.State = DuelPairState::Approaching;
                dp.Deployed = false;
                event.DuelPairs.push_back(dp);
            }
            AuditLog("DUEL_QUEUE", "event=" + std::to_string(event.EventId) + " spot=" + zone.Name + " pairs=" + std::to_string(event.DuelPairs.size()));
        }
        else
        {
            auto add = [&](BotCandidate const& bot, bool attacker)
            {
                Player* player = FindPlayer(bot.GuidLow);
                if (!IsSafeBot(player))
                    return;

                Participant part;
                part.Bot = bot;
                part.Attacker = attacker;
                part.OriginalMap = player->GetMapId();
                part.OriginalX = player->GetPositionX();
                part.OriginalY = player->GetPositionY();
                part.OriginalZ = player->GetPositionZ();
                part.OriginalO = player->GetOrientation();

                float sx = (attacker ? zone.RallyX : zone.TargetX) + Jitter();
                float sy = (attacker ? zone.RallyY : zone.TargetY) + Jitter();
                float sz = attacker ? zone.RallyZ : zone.TargetZ;
                float so = attacker ? zone.RallyO : zone.TargetO;

                part.ActivityX = sx;
                part.ActivityY = sy;
                part.ActivityZ = sz;
                part.ActivityO = so;
                part.Deployed = false;
                event.Members.push_back(part);
            };

            for (BotCandidate const& b : attackers)
                add(b, true);
            for (BotCandidate const& b : defenders)
                add(b, false);
        }

        uint32 actualAttackers = 0;
        uint32 actualDefenders = 0;
        for (Participant const& p : event.Members)
        {
            if (p.Attacker)
                ++actualAttackers;
            else
                ++actualDefenders;
        }

        if (actualAttackers < requiredAttackers || actualDefenders < requiredDefenders ||
            (actualAttackers + actualDefenders) < requiredTotal)
        {
            if (_debug)
                LOG_INFO("module", "[PvPLife] '{}' aborted: usable attackers {}/{} defenders {}/{} total {} (min required {})",
                    zone.Name, actualAttackers, requiredAttackers, actualDefenders, requiredDefenders,
                    actualAttackers + actualDefenders, requiredTotal);
            if (handler)
                handler->PSendSysMessage("PvP Life: Insufficient usable bots for {} (attackers {}/{}, defenders {}/{}).",
                    zone.Name, actualAttackers, requiredAttackers, actualDefenders, requiredDefenders);
            rollback();
            return false;
        }

        std::vector<uint32> admissionGuids;
        for (Participant const& part : event.Members)
            admissionGuids.push_back(part.Bot.GuidLow);
        event.AdmissionRequestId = PlayerbotsLife::SubmitAdmissionRequest(
            PlayerbotsLife::ReservationOwner::PvPLife, event.EventId, admissionGuids,
            CalculatePopulationBudget(), now, event.PrepareDeadline);
        if (!event.AdmissionRequestId)
        {
            if (handler)
                handler->PSendSysMessage("PvP Life: Admission for {} was refused (budget or request queue full).", zone.Name);
            rollback();
            return false;
        }
        _activeEvents.push_back(event);
        WorldDatabase.Execute("UPDATE pvp_life_zone SET last_start={} WHERE id={}", now, zone.Id);
        for (Zone& cached : _zoneCache)
            if (cached.Id == zone.Id)
                cached.LastStart = now;

        std::string label = std::string(TypeName(zone.Type)) + " at " + zone.Name;
        LOG_INFO("module", "[PvPLife] START #{} {} attackers={} defenders={} duration={}m",
            event.EventId, label, actualAttackers, actualDefenders, event.DurationSeconds / MINUTE);

        AuditLog("START", "event=" + std::to_string(event.EventId) + " spot=" + zone.Name +
            " mode=" + (zone.Type == ActivityType::Duel ? "duel" : "world_pvp") +
            " attackerTeam=" + TeamName(eventZone.AttackerTeam) + " defenderTeam=" + TeamName(eventZone.DefenderTeam) +
            " attackers=" + std::to_string(actualAttackers) + " defenders=" + std::to_string(actualDefenders) +
            " durationSeconds=" + std::to_string(event.DurationSeconds));

        if (handler)
            handler->PSendSysMessage("PvP Life: #{} {} queued for admission (bots {} + {}).",
                event.EventId, label, actualAttackers, actualDefenders);
        return true;
    }

    void Manager::QueueParticipantRelease(Participant const& part, bool returnHome)
    {
        if (!PlayerbotsLife::BeginRelease(part.Token))
            return; // A stale record cannot clean another generation's actor.
        if (!part.Deployed || !part.Journal.Captured)
        {
            PlayerbotsLife::ReleaseLeaseExact(part.Token);
            return;
        }
        if (std::any_of(_releasingParticipants.begin(), _releasingParticipants.end(),
            [&](ReleasingParticipant const& current) { return current.Part.Token.Generation == part.Token.Generation; }))
            return;

        // Clear any queued bot-directed steps for this departing participant immediately (PVP-01)
        uint32 guid = part.Bot.GuidLow;
        StopAntic(guid);
        _moveQueue.erase(std::remove_if(_moveQueue.begin(), _moveQueue.end(),
            [guid](MoveStep const& s) { return s.GuidLow == guid; }), _moveQueue.end());
        EraseDuelSteps([guid](DuelStep const& s) { return s.ChallengerGuidLow == guid || s.TargetGuidLow == guid; });
        _engagementQueue.erase(std::remove_if(_engagementQueue.begin(), _engagementQueue.end(),
            [guid](EngagementStep const& s) { return s.BotGuidLow == guid || s.TargetGuidLow == guid; }), _engagementQueue.end());
        _chatQueue.erase(std::remove_if(_chatQueue.begin(), _chatQueue.end(),
            [guid](ChatStep const& s) { return s.SpeakerGuidLow == guid; }), _chatQueue.end());

        ReleasingParticipant releasing;
        releasing.Part = part;
        releasing.ReturnHome = returnHome;
        releasing.EnqueuedAt = GameTimeSeconds();
        // Releases are throttled by MaxChangesPerTick; whatever is left at the deadline takes the
        // terminal path. It must stay well under the shared registry's RELEASING_LEASE_TTL_SEC.
        releasing.Deadline = releasing.EnqueuedAt + ReleaseDeadlineSeconds;
        releasing.Phase = ReleasePhase::PendingCombatExit;
        releasing.Attempts = 0;
        _releasingParticipants.push_back(releasing);
    }

    void Manager::EndEvent(ActiveEvent& event, std::string const& reason)
    {
        if (event.Closing)
            return;
        event.Closing = true; // Terminal before callbacks, queue purges, or cleanup.
        if (event.AdmissionRequestId != 0)
            PlayerbotsLife::CancelAdmissionRequest(event.AdmissionRequestId, PlayerbotsLife::ReservationOwner::PvPLife);
        event.AdmissionRequestId = 0;
        uint64 id = event.EventId;
        _moveQueue.erase(std::remove_if(_moveQueue.begin(), _moveQueue.end(),
            [id](MoveStep const& step) { return step.EventId == id; }), _moveQueue.end());
        EraseDuelSteps([id](DuelStep const& step) { return step.EventId == id; });
        _engagementQueue.erase(std::remove_if(_engagementQueue.begin(), _engagementQueue.end(),
            [id](EngagementStep const& step) { return step.EventId == id; }), _engagementQueue.end());
        _chatQueue.erase(std::remove_if(_chatQueue.begin(), _chatQueue.end(),
            [id](ChatStep const& step) { return step.EventId == id; }), _chatQueue.end());
        for (Participant const& part : event.Members)
            QueueParticipantRelease(part, _returnBots);
        // Every staging slot this event claimed carries its id; the duelists are leaving.
        PlayerbotsLife::ReleaseDuelStagingSlotByActivity(id, PlayerbotsLife::ReservationOwner::PvPLife);
        LOG_INFO("module", "[PvPLife] END #{} {} reason={}", id, event.EventZone.Name, reason);
        AuditLog("END", "event=" + std::to_string(id) + " reason=" + reason);
    }

    void Manager::ForceReleaseParticipant(ReleasingParticipant const& releasing, Player* player, bool allowTeleport)
    {
        Participant const& part = releasing.Part;
        if (player && player->GetSession() && !IsHumanControlled(player))
        {
            CloseNativeDuel(player, part);
            if (player->IsInCombat())
                player->CombatStop(true);
            RestoreAIState(player, part.Journal);
            if (allowTeleport)
            {
                if (sRandomPlayerbotMgr.IsRandomBot(player))
                    sRandomPlayerbotMgr.ScheduleTeleport(part.Bot.GuidLow);
                if (releasing.ReturnHome && player->IsInWorld() && !player->IsBeingTeleported() && player->IsAlive())
                    player->TeleportTo(part.OriginalMap, part.OriginalX, part.OriginalY, part.OriginalZ, part.OriginalO);
            }
        }
        PlayerbotsLife::ReleaseLeaseExact(part.Token);
    }

    void Manager::ProcessReleasingParticipants()
    {
        auto it = _releasingParticipants.begin();
        uint32 now = GameTimeSeconds();
        while (it != _releasingParticipants.end())
        {
            Participant const& part = it->Part;
            Player* player = FindPlayer(part.Bot.GuidLow);
            if (!PlayerbotsLife::ValidateLeaseState(part.Token, PlayerbotsLife::LeaseState::Releasing))
            {
                // The lease is gone (scavenged or purged). Still undo our changes when the bot is
                // here, not player-controlled, and no Life module has claimed it since.
                if (player && player->GetSession() && !IsHumanControlled(player) &&
                    !PlayerbotsLife::IsReserved(part.Bot.GuidLow))
                    RestoreAIState(player, part.Journal);
                it = _releasingParticipants.erase(it);
                continue;
            }
            if (!player || !player->GetSession() || IsHumanControlled(player))
            {
                PlayerbotsLife::ReleaseLeaseExact(part.Token);
                it = _releasingParticipants.erase(it);
                continue;
            }

            // Deadline reached (throttled out, stuck in combat or a transfer): the terminal path
            // still restores AI state and sends the bot home.
            if (now >= it->Deadline)
            {
                LOG_WARN("module", "[PvPLife] Release deadline reached for actor {} (phase={}, attempts={}); releasing it now.",
                    part.Bot.GuidLow, static_cast<uint32>(it->Phase), it->Attempts);
                ForceReleaseParticipant(*it, player, true);
                it = _releasingParticipants.erase(it);
                continue;
            }

            if (!player->IsInWorld() || player->IsBeingTeleported())
            {
                ++it->Attempts;
                ++it;
                continue;
            }
            if (_changesThisTick >= _maxChangesPerTick)
            {
                ++it;
                continue;
            }

            CloseNativeDuel(player, part);

            if (it->Phase == ReleasePhase::PendingCombatExit)
            {
                if (player->IsInCombat())
                {
                    ++it->Attempts;
                    ++it;
                    continue;
                }
                it->Phase = ReleasePhase::RestoreWhenSafe;
            }

            if (it->Phase == ReleasePhase::RestoreWhenSafe)
            {
                RestoreAIState(player, part.Journal);
                if (sRandomPlayerbotMgr.IsRandomBot(player))
                    sRandomPlayerbotMgr.ScheduleTeleport(part.Bot.GuidLow);
                if (it->ReturnHome && player->IsAlive() && !player->IsInCombat())
                {
                    if (RealPlayerWatching(player))
                    {
                        // Seen: walk off and carry on as an ordinary bot here, and hold mod-playerbots'
                        // own random teleport off for a while too.
                        if (sRandomPlayerbotMgr.IsRandomBot(player))
                            sRandomPlayerbotMgr.ScheduleTeleport(part.Bot.GuidLow, urand(1200, 2400));
                        float angle = frand(0.0f, 2.0f * float(M_PI));
                        float x = player->GetPositionX() + std::cos(angle) * frand(40.0f, 80.0f);
                        float y = player->GetPositionY() + std::sin(angle) * frand(40.0f, 80.0f);
                        float z = player->GetPositionZ();
                        if (player->GetMap()->CanReachPositionAndGetValidCoords(player, x, y, z))
                            player->GetMotionMaster()->MovePoint(0, x, y, z);
                    }
                    else
                        player->TeleportTo(part.OriginalMap, part.OriginalX, part.OriginalY, part.OriginalZ, part.OriginalO);
                }
                ++_changesThisTick;
                it->Phase = ReleasePhase::Released;
                PlayerbotsLife::ReleaseLeaseExact(part.Token);
                it = _releasingParticipants.erase(it);
            }
        }
    }

    void Manager::ExpireEvents()
    {
        uint32 now = GameTimeSeconds();
        for (ActiveEvent& event : _activeEvents)
        {
            if (event.Closing)
                continue;
            if (event.EndsAt <= now)
            {
                // Let the duels under way finish (up to two minutes) rather than cut them off.
                if (event.EventZone.Type == ActivityType::Duel && now < event.EndsAt + 120 &&
                    std::any_of(event.Members.begin(), event.Members.end(), [this](Participant const& part)
                    {
                        Player* player = FindPlayer(part.Bot.GuidLow);
                        return player && player->duel && player->duel->State == DUEL_STATE_IN_PROGRESS;
                    }))
                    continue;
                EndEvent(event, "timer");
            }
            else if (event.FullyPrepared && event.EventZone.Type == ActivityType::Duel)
            {
                // A bot mid-teleport is briefly out of the world but still taking part.
                uint32 alive = 0;
                for (Participant const& part : event.Members)
                    if (Player* player = FindPlayer(part.Bot.GuidLow);
                        player && (player->IsInWorld() || player->IsBeingTeleported()) && player->IsAlive())
                        ++alive;
                if (alive < 2)
                    EndEvent(event, "duel-population");
            }
        }
        _activeEvents.erase(std::remove_if(_activeEvents.begin(), _activeEvents.end(),
            [](ActiveEvent const& event) { return event.Closing; }), _activeEvents.end());
    }

    void Manager::ProcessMoveQueue()
    {
        uint32 now = GameTimeSeconds();
        std::vector<MoveStep> keep;
        for (MoveStep const& step : _moveQueue)
        {
            if (step.ExecuteAt > now)
            {
                keep.push_back(step);
                continue;
            }
            auto event = std::find_if(_activeEvents.begin(), _activeEvents.end(),
                [&](ActiveEvent const& current) { return current.EventId == step.EventId; });
            if (event == _activeEvents.end() || event->Closing || event->EndsAt <= now ||
                !PlayerbotsLife::ValidateActiveLease(step.Token))
                continue;
            Player* p = FindPlayer(step.GuidLow);
            if (!p || p->IsBeingTeleported())
            {
                if (step.RetryCount < 4 && (p || step.RetryCount < 2))
                {
                    MoveStep retry = step;
                    retry.ExecuteAt = now + 2;
                    ++retry.RetryCount;
                    keep.push_back(retry);
                }
                continue;
            }
            if (!CanExecuteStep(step.EventId, step.Generation, step.GuidLow, true))
                continue;
            MoveBot(p, step.MapId, step.X, step.Y, step.Z, step.O, step.Teleport);
            AuditLog("MOVE", "event=" + std::to_string(step.EventId) + " player=" + step.Name +
                " guid=" + std::to_string(step.GuidLow) + " map=" + std::to_string(step.MapId) +
                " x=" + std::to_string(step.X) + " y=" + std::to_string(step.Y) + " z=" + std::to_string(step.Z) +
                " mode=" + (step.Teleport ? (_dropOffArrival ? "arrive" : "teleport") : "movepoint"));
        }
        _moveQueue.swap(keep);
    }

    void Manager::ProcessEngagementQueue()
    {
        if (_engagementQueue.empty())
            return;

        uint32 now = GameTimeSeconds();
        std::vector<EngagementStep> keep;
        for (EngagementStep const& step : _engagementQueue)
        {
            auto preparing = std::find_if(_activeEvents.begin(), _activeEvents.end(),
                [&](ActiveEvent const& event) { return event.EventId == step.EventId; });
            if (preparing != _activeEvents.end() && !preparing->Closing && !preparing->FullyPrepared &&
                preparing->PrepareDeadline > now)
            {
                keep.push_back(step);
                continue;
            }
            if (step.ExecuteAt > now)
            {
                keep.push_back(step);
                continue;
            }

            if (!CanExecuteStep(step.EventId, step.Generation, step.BotGuidLow))
                continue;

            Player* bot = FindPlayer(step.BotGuidLow);
            Player* target = FindPlayer(step.TargetGuidLow);
            if (!bot || !target || !bot->IsAlive() || !target->IsAlive())
                continue;

            if (!BotActive(bot))
            {
                EngagementStep wait = step;
                wait.ExecuteAt = now + _engageRetrySeconds;
                keep.push_back(wait);
                continue;
            }

            EngageTarget(bot, target);

            if (!bot->IsInCombat() && step.RetryCount < step.MaxRetries)
            {
                EngagementStep retry = step;
                retry.ExecuteAt = now + _engageRetrySeconds;
                ++retry.RetryCount;
                keep.push_back(retry);
            }
        }
        _engagementQueue.swap(keep);
    }

    void Manager::ProcessDuelQueue()
    {
        uint32 now = GameTimeSeconds();
        std::vector<DuelStep> keep;
        for (DuelStep const& step : _duelQueue)
        {
            auto preparing = std::find_if(_activeEvents.begin(), _activeEvents.end(),
                [&](ActiveEvent const& event) { return event.EventId == step.EventId; });
            if (preparing != _activeEvents.end() && !preparing->Closing && !preparing->FullyPrepared &&
                preparing->PrepareDeadline > now)
            {
                keep.push_back(step);
                continue;
            }
            if (step.ExecuteAt > now)
            {
                keep.push_back(step);
                continue;
            }

            if (!CanExecuteStep(step.EventId, step.Generation, step.ChallengerGuidLow))
                continue;
            if (!step.TargetIsRealPlayer && !CanExecuteStep(step.EventId, step.TargetToken.Generation, step.TargetGuidLow))
                continue;

            auto event = std::find_if(_activeEvents.begin(), _activeEvents.end(), [&](ActiveEvent const& active)
            {
                return active.EventId == step.EventId && active.EventZone.Type == ActivityType::Duel && active.EndsAt > now;
            });
            if (event == _activeEvents.end())
                continue;
            Player* challenger = FindPlayer(step.ChallengerGuidLow);
            Player* target = FindPlayer(step.TargetGuidLow);
            if (!challenger || !target || challenger == target || !challenger->GetSession() || !target->GetSession())
                continue;
            if (!BotActive(challenger) || (!step.TargetIsRealPlayer && !BotActive(target)))
            {
                DuelStep wait = step;
                wait.ExecuteAt = now + 5;
                keep.push_back(wait);
                continue;
            }
            // Same account test IsSafeBot used when the bots were admitted.
            auto borrowable = [this](Player* bot)
            {
                return GET_PLAYERBOT_AI(bot) && (sRandomPlayerbotMgr.IsRandomBot(bot) ||
                    IsConfiguredBotAccount(bot->GetSession()->GetAccountId()));
            };
            if (!borrowable(challenger))
                continue;
            // A real target is checked again right before the challenge goes out: it may have
            // turned GM, mounted, gone AFK or walked off since it was picked.
            if (step.TargetIsRealPlayer ? (!IsHumanPlayer(target) ||
                    (!step.RequestSent && !IsChallengeableRealPlayer(target, challenger))) :
                !borrowable(target))
                continue;

            auto fail = [&](char const* reason, uint32 result = uint32(SPELL_CAST_OK))
            {
                uint32 until = now + std::max<uint32>(DuelFailureBackoff, _botDuelCooldownSeconds);
                _botDuelCooldown[step.ChallengerGuidLow] = std::max(_botDuelCooldown[step.ChallengerGuidLow], until);
                if (!step.TargetIsRealPlayer)
                    _botDuelCooldown[step.TargetGuidLow] = std::max(_botDuelCooldown[step.TargetGuidLow], until);
                LOG_WARN("module", "[PvPLife] Duel request deferred: {} -> {}; reason={} spell_result={} spell={} "
                    "area={}/{} dist={} phase={}/{} backoff={}s",
                    challenger->GetName(), target->GetName(), reason, result, _duelSpellId,
                    challenger->GetAreaId(), target->GetAreaId(), challenger->GetExactDist(target),
                    challenger->GetPhaseMask(), target->GetPhaseMask(), until - now);
            };
            if (!challenger->IsInWorld() || !target->IsInWorld() || !challenger->IsAlive() || !target->IsAlive() ||
                challenger->IsBeingTeleported() || target->IsBeingTeleported() ||
                challenger->IsInFlight() || target->IsInFlight())
                continue;
            if (challenger->GetMap() != target->GetMap() || !challenger->InSamePhase(target))
            {
                fail("Map or phase mismatch", SPELL_FAILED_BAD_TARGETS);
                continue;
            }

            bool matching = challenger->duel && target->duel &&
                challenger->duel->Opponent == target && target->duel->Opponent == challenger &&
                challenger->duel->Initiator == challenger && target->duel->Initiator == challenger &&
                challenger->GetGuidValue(PLAYER_DUEL_ARBITER) &&
                challenger->GetGuidValue(PLAYER_DUEL_ARBITER) == target->GetGuidValue(PLAYER_DUEL_ARBITER);
            bool started = matching &&
                (challenger->duel->State == DUEL_STATE_COUNTDOWN || challenger->duel->State == DUEL_STATE_IN_PROGRESS) &&
                (target->duel->State == DUEL_STATE_COUNTDOWN || target->duel->State == DUEL_STATE_IN_PROGRESS);
            DuelRequestAction action = EvaluateDuelRequest(step.RequestSent, challenger->duel || target->duel,
                matching, step.TargetIsRealPlayer, started, now, step.RequestDeadline);
            if (action == DuelRequestAction::Busy)
                continue;
            if (action == DuelRequestAction::Wait)
            {
                DuelStep waiting = step;
                waiting.ExecuteAt = now + 1;
                keep.push_back(waiting);
                continue;
            }
            if (action == DuelRequestAction::TimedOut)
            {
                fail("Duel state not created after cast");
                continue;
            }
            if (action == DuelRequestAction::AcceptBot || action == DuelRequestAction::Confirmed)
            {
                if (action == DuelRequestAction::AcceptBot)
                {
                    // Both failures below leave a duel we created behind: cancel it, or the pair
                    // reads as busy until the duel object goes away on its own.
                    if (target->duel->State != DUEL_STATE_CHALLENGED || challenger->duel->State != DUEL_STATE_CHALLENGED)
                    {
                        fail("Duel accept state mismatch");
                        CancelPendingNativeDuel(challenger);
                        continue;
                    }
                    WorldPacket packet(CMSG_DUEL_ACCEPTED, 8);
                    packet << target->GetGuidValue(PLAYER_DUEL_ARBITER);
                    target->GetSession()->HandleDuelAcceptedOpcode(packet);
                    if (!challenger->duel || !target->duel ||
                        challenger->duel->State != DUEL_STATE_COUNTDOWN || target->duel->State != DUEL_STATE_COUNTDOWN)
                    {
                        fail("Countdown not verified after duel acceptance");
                        CancelPendingNativeDuel(challenger);
                        continue;
                    }
                }
                RememberDuelIdentity(step.EventId, challenger, target);
                if (step.TargetIsRealPlayer)
                {
                    _botChallengeCooldown[step.ChallengerGuidLow] = now + _botChallengeCooldownSeconds;
                    _playerChallengeCooldown[step.TargetGuidLow] = now + _realPlayerCooldownSeconds;
                    PlayerbotsLife::RecordRealPlayerChallenge(step.TargetGuidLow, now, _realPlayerCooldownSeconds);
                }
                else
                {
                    _botDuelCooldown[step.ChallengerGuidLow] = now + _botDuelCooldownSeconds;
                    _botDuelCooldown[step.TargetGuidLow] = now + _botDuelCooldownSeconds;
                }
                for (auto& pair : event->DuelPairs)
                {
                    if (pair.PairId == step.PairId ||
                        (pair.ChallengerGuidLow == step.ChallengerGuidLow && pair.TargetGuidLow == step.TargetGuidLow))
                    {
                        pair.State = DuelPairState::Active;
                        break;
                    }
                }
                LOG_INFO("module", "[PvPLife] Duel state confirmed: {} -> {}; state={}",
                    challenger->GetName(), target->GetName(), step.TargetIsRealPlayer ? "waiting/accepted by player" : "countdown/in-progress");
                AuditLog("DUEL", "event=" + std::to_string(step.EventId) + " challenger=" + challenger->GetName() +
                    " target=" + target->GetName() + " spell=" + std::to_string(_duelSpellId));
                continue;
            }

            if (challenger->IsInCombat() || target->IsInCombat() || challenger->IsPetInCombat() || target->IsPetInCombat())
            {
                fail("Self or pet in combat", SPELL_FAILED_TARGET_AFFECTING_COMBAT);
                continue;
            }
            SpellInfo const* spell = sSpellMgr->GetSpellInfo(_duelSpellId);
            if (!spell || !spell->HasEffect(SPELL_EFFECT_DUEL))
            {
                fail("Spell lacks duel effect", SPELL_FAILED_SPELL_UNAVAILABLE);
                continue;
            }
            AreaTableEntry const* casterArea = sAreaTableStore.LookupEntry(challenger->GetAreaId());
            AreaTableEntry const* targetArea = sAreaTableStore.LookupEntry(target->GetAreaId());
            if ((casterArea && !(casterArea->flags & AREA_FLAG_ALLOW_DUELS)) ||
                (targetArea && !(targetArea->flags & AREA_FLAG_ALLOW_DUELS)))
            {
                fail("Duels forbidden in area", SPELL_FAILED_NO_DUELING);
                continue;
            }
            if (!target->GetSocial() || target->GetSocial()->HasIgnore(challenger->GetGUID()))
            {
                fail("Social info not ready or target ignored", SPELL_FAILED_BAD_TARGETS);
                continue;
            }
            SpellCastResult targetResult = spell->CheckTarget(challenger, target, false);
            if (targetResult != SPELL_CAST_OK)
            {
                fail("Target requirements not met", targetResult);
                continue;
            }
            float approachDistance = DuelApproachDistance(challenger->GetSpellMaxRangeForTarget(target, spell));
            if (challenger->GetExactDist(target) > approachDistance || !challenger->IsWithinLOSInMap(target))
            {
                DuelStep retry = step;
                if (retry.RetryCount++ < DuelApproachRetries)
                {
                    // Come in a little off the straight line, so challengers do not all walk the same path.
                    float x, y, z;
                    target->GetNearPoint(challenger, x, y, z, challenger->GetCombatReach(),
                        std::min(3.0f, approachDistance * 0.5f), target->GetAngle(challenger) + frand(-0.45f, 0.45f));
                    if (PlayerbotAI* ai = GET_PLAYERBOT_AI(challenger))
                    {
                        PositionMap& positions = ai->GetAiObjectContext()->GetValue<PositionMap&>("position")->Get();
                        positions["stay"].Set(x, y, z, challenger->GetMapId());
                    }
                    challenger->GetMotionMaster()->MovePoint(0, x, y, z);
                    retry.ExecuteAt = now + 3;
                    keep.push_back(retry);
                }
                else
                    fail("Approach or line-of-sight timeout", SPELL_FAILED_OUT_OF_RANGE);
                continue;
            }
            challenger->SetFacingToObject(target);
            if (!step.TargetIsRealPlayer)
            {
                target->SetFacingToObject(challenger);
                challenger->SetHealth(challenger->GetMaxHealth());
                target->SetHealth(target->GetMaxHealth());
                // Refill mana/energy; rage and runic power start a duel empty, not full.
                for (Player* duelist : { challenger, target })
                {
                    Powers power = duelist->getPowerType();
                    bool startsEmpty = power == POWER_RAGE || power == POWER_RUNIC_POWER;
                    duelist->SetPower(power, startsEmpty ? 0 : duelist->GetMaxPower(power));
                }
            }
            // Nobody duels from the saddle: a bot that rode in (or that mod-playerbots mounted while it
            // idled) would otherwise start mounted, and mounted it can neither cast nor swing
            // (realm 2026-10-12 trace: 79% of footwork checks found the duelist still mounted).
            for (Player* duelist : { challenger, target })
                if (!step.TargetIsRealPlayer || duelist == challenger)
                    duelist->RemoveAurasByType(SPELL_AURA_MOUNTED);
            if (!step.TargetIsRealPlayer && roll_chance_i(40))
                challenger->HandleEmoteCommand(EMOTE_ONESHOT_BOW);
            SpellCastResult result = challenger->CastSpell(target, _duelSpellId, true);
            if (result != SPELL_CAST_OK)
            {
                fail("Duel spell cast failed", result);
                continue;
            }
            RememberDuelIdentity(step.EventId, challenger, target);
            DuelStep waiting = step;
            waiting.RequestSent = true;
            waiting.RequestDeadline = now + DuelRequestTimeout;
            waiting.ExecuteAt = now + 1;
            keep.push_back(waiting);
        }
        // A real-player step dropped before its challenge went out hands its shared claim back,
        // so the player is not locked out of both modules' challenges for nothing.
        for (DuelStep const& step : _duelQueue)
            if (step.ChallengeClaimExpiry && !step.RequestSent &&
                std::none_of(keep.begin(), keep.end(), [&step](DuelStep const& kept)
                    { return kept.ChallengerGuidLow == step.ChallengerGuidLow && kept.TargetGuidLow == step.TargetGuidLow; }))
                PlayerbotsLife::ReleaseRealPlayerChallengeClaim(step.TargetGuidLow, step.ChallengeClaimExpiry);
        _duelQueue.swap(keep);
    }

    std::string Manager::RandomChatLine(ActivityType type, ChatChannel channel, TeamSide speakerTeam)
    {
        uint64 total = 0;
        for (CachedChatLine const& line : _chatCache)
            if (line.Type == type && line.Channel == channel &&
                (line.Team == speakerTeam || line.Team == TeamSide::Any))
                total += line.Weight;
        if (!total)
            return {};
        uint64 roll = static_cast<uint64>(frand(0.0f, 1.0f) * total);
        roll = std::min(roll, total - 1);
        for (CachedChatLine const& line : _chatCache)
            if (line.Type == type && line.Channel == channel &&
                (line.Team == speakerTeam || line.Team == TeamSide::Any))
            {
                if (roll < line.Weight)
                    return line.Text;
                roll -= line.Weight;
            }
        return {};
    }

    void Manager::ScheduleEventChat(ActiveEvent const& event)
    {
        if (!_botChatEnable || !event.EventZone.BotChat)
            return;
        if (_botChatYellChance == 0 && _botChatWorldChance == 0)
            return;
        if (event.EventZone.Type != ActivityType::ForTheHorde && event.EventZone.Type != ActivityType::ForTheAlliance)
            return;

        std::vector<Participant const*> defenders;
        for (Participant const& p : event.Members)
            if (!p.Attacker)
                defenders.push_back(&p);
        if (defenders.empty())
            return;

        TeamSide team = event.EventZone.DefenderTeam;
        uint32 base = _arrivalStaggerMax + 8;
        if (_botChatYellChance > 0 && urand(1, 100) <= _botChatYellChance)
        {
            std::string line = RandomChatLine(event.EventZone.Type, ChatChannel::Yell, team);
            if (!line.empty())
            {
                Participant const* speaker = defenders[urand(0, static_cast<uint32>(defenders.size() - 1))];
                ChatStep step;
                step.EventId = event.EventId;
                step.Generation = speaker->Token.Generation;
                step.Token = speaker->Token;
                step.SpeakerGuidLow = speaker->Bot.GuidLow;
                step.ExecuteAt = GameTimeSeconds() + base + urand(1, 8);
                step.Channel = ChatChannel::Yell;
                step.Text = std::move(line);
                _chatQueue.push_back(step);
            }
        }
        if (_botChatWorldChance > 0 && urand(1, 100) <= _botChatWorldChance)
        {
            std::string line = RandomChatLine(event.EventZone.Type, ChatChannel::World, team);
            if (!line.empty())
            {
                Participant const* speaker = defenders[urand(0, static_cast<uint32>(defenders.size() - 1))];
                ChatStep step;
                step.EventId = event.EventId;
                step.Generation = speaker->Token.Generation;
                step.Token = speaker->Token;
                step.SpeakerGuidLow = speaker->Bot.GuidLow;
                step.ExecuteAt = GameTimeSeconds() + base + urand(8, 25);
                step.Channel = ChatChannel::World;
                step.Text = std::move(line);
                _chatQueue.push_back(step);
            }
        }
    }

    void Manager::ProcessChatQueue()
    {
        if (!_botChatEnable)
        {
            if (!_chatQueue.empty())
                _chatQueue.clear();
            return;
        }

        uint32 now = GameTimeSeconds();
        std::vector<ChatStep> keep;
        for (ChatStep const& step : _chatQueue)
        {
            if (step.Channel == ChatChannel::Yell && _botChatYellChance == 0)
                continue;
            if (step.Channel == ChatChannel::World && _botChatWorldChance == 0)
                continue;

            auto preparing = std::find_if(_activeEvents.begin(), _activeEvents.end(),
                [&](ActiveEvent const& event) { return event.EventId == step.EventId; });
            if (preparing != _activeEvents.end() && !preparing->Closing && !preparing->FullyPrepared &&
                preparing->PrepareDeadline > now)
            {
                keep.push_back(step);
                continue;
            }
            if (step.ExecuteAt > now)
            {
                keep.push_back(step);
                continue;
            }
            if (!CanExecuteStep(step.EventId, step.Generation, step.SpeakerGuidLow))
                continue;
            Player* speaker = FindPlayer(step.SpeakerGuidLow);
            if (!speaker)
                continue;
            PlayerbotAI* ai = GET_PLAYERBOT_AI(speaker);
            if (!ai)
                continue;
            if (step.Channel == ChatChannel::Yell)
            {
                if (_botChatYellChance > 0)
                    ai->Yell(step.Text);
            }
            else
            {
                if (_botChatWorldChance > 0)
                    ai->SayToWorld(step.Text);
            }
        }
        _chatQueue.swap(keep);
    }

    void Manager::MaintainBotDuels()
    {
        if (!_duelEnable)
            return;

        uint32 now = GameTimeSeconds();
        std::unordered_set<uint32> queued;
        for (DuelStep const& step : _duelQueue)
        {
            queued.insert(step.ChallengerGuidLow);
            queued.insert(step.TargetGuidLow);
        }

        for (ActiveEvent const& event : _activeEvents)
        {
            if (event.Closing || !event.FullyPrepared || event.EventZone.Type != ActivityType::Duel ||
                event.EndsAt <= now)
                continue;

            std::vector<uint32> available;
            for (Participant const& participant : event.Members)
            {
                if (!participant.Deployed || !PlayerbotsLife::ValidateActiveLease(participant.Token))
                    continue;

                uint32 guidLow = participant.Bot.GuidLow;
                if (queued.count(guidLow))
                    continue;

                Player* bot = FindPlayer(guidLow);
                if (IsHumanControlled(bot))
                    continue;
                if (!bot || !bot->IsAlive() || bot->IsBeingTeleported() || bot->duel || bot->IsInCombat() ||
                    bot->IsInFlight() || bot->InBattleground())
                    continue;

                ApplyPvpStrategies(bot, true);
                // Same leash centre as MaintainDuelParticipants (the bot's own staging point), so
                // the two passes never teleport a bot back and forth.
                if (bot->GetMapId() != event.EventZone.MapId ||
                    bot->GetDistance2d(participant.ActivityX, participant.ActivityY) > _duelLeashRadius)
                {
                    MoveBot(bot, event.EventZone.MapId, participant.ActivityX, participant.ActivityY,
                        participant.ActivityZ, participant.ActivityO, true);
                    continue;
                }

                if (!BotActive(bot))
                    continue;
                // Mid-antic: let it finish first (a duel would cut it off a few seconds in).
                if (AnticRunning(guidLow))
                    continue;
                auto duelCd = _botDuelCooldown.find(guidLow);
                if (duelCd != _botDuelCooldown.end() && duelCd->second > now)
                    continue;
                auto playerCd = _botChallengeCooldown.find(guidLow);
                if (playerCd != _botChallengeCooldown.end() && playerCd->second > now)
                    continue;

                available.push_back(guidLow);
            }

            std::unordered_set<uint32> busy;
            for (Participant const& participant : event.Members)
                if (Player* member = FindPlayer(participant.Bot.GuidLow); member && (member->duel || queued.count(participant.Bot.GuidLow)))
                    busy.insert(participant.Bot.GuidLow);
            uint32 scheduled = (static_cast<uint32>(busy.size()) + 1) / 2;
            while (available.size() >= 2 && scheduled < _duelPairLimit)
            {
                uint32 firstIndex = urand(0, static_cast<uint32>(available.size() - 1));
                uint32 first = available[firstIndex];
                Player* a = FindPlayer(first);
                if (!a)
                {
                    available.erase(available.begin() + firstIndex);
                    continue;
                }

                // Partners come from nearby: one of the two closest, so a pair does not cross the
                // whole duel ground to meet, but the same two neighbours do not always pair either.
                std::vector<std::pair<float, uint32>> compatibleIndexes;
                for (uint32 i = 0; i < available.size(); ++i)
                {
                    if (i == firstIndex)
                        continue;
                    Player* candidate = FindPlayer(available[i]);
                    if (!candidate || a->GetTeamId() != candidate->GetTeamId())
                        continue;
                    float distance = a->GetDistance2d(candidate);
                    if (distance > _duelLeashRadius * 2.0f)
                        continue;
                    uint32 levelDiff = std::abs(static_cast<int32>(a->GetLevel()) - static_cast<int32>(candidate->GetLevel()));
                    if (levelDiff <= _botDuelMaxLevelDifference)
                        compatibleIndexes.emplace_back(distance, i);
                }

                if (compatibleIndexes.empty())
                {
                    available.erase(available.begin() + firstIndex);
                    continue;
                }

                std::sort(compatibleIndexes.begin(), compatibleIndexes.end());
                uint32 pickLimit = std::min<uint32>(static_cast<uint32>(compatibleIndexes.size()), 2);
                uint32 secondIndex = compatibleIndexes[urand(0, pickLimit - 1)].second;
                uint32 second = available[secondIndex];
                Player* b = FindPlayer(second);

                if (secondIndex > firstIndex)
                {
                    available.erase(available.begin() + secondIndex);
                    available.erase(available.begin() + firstIndex);
                }
                else
                {
                    available.erase(available.begin() + firstIndex);
                    available.erase(available.begin() + secondIndex);
                }

                if (b)
                {
                    PlayerbotsLife::LeaseToken tokenA, tokenB;
                    for (Participant const& part : event.Members)
                    {
                        if (part.Bot.GuidLow == first)
                            tokenA = part.Token;
                        else if (part.Bot.GuidLow == second)
                            tokenB = part.Token;
                    }
                    ScheduleDuel(event.EventId, _nextPairId++, tokenA, tokenB, first, second, a->GetName(), b->GetName(), urand(_duelPairDelayMin, _duelPairDelayMax));
                    queued.insert(first);
                    queued.insert(second);
                    ++scheduled;
                }
            }
        }
    }

    void Manager::MaintainDuelParticipants()
    {
        for (ActiveEvent const& event : _activeEvents)
        {
            if (event.Closing || !event.FullyPrepared || event.EventZone.Type != ActivityType::Duel)
                continue;

            // Duels being fought right now, for the idle bots nearby to watch.
            std::vector<Player*> fighting;
            for (Participant const& participant : event.Members)
                if (Player* duelist = FindPlayer(participant.Bot.GuidLow);
                    duelist && duelist->duel && duelist->duel->State == DUEL_STATE_IN_PROGRESS)
                    fighting.push_back(duelist);

            for (Participant const& participant : event.Members)
            {
                if (!participant.Deployed || !PlayerbotsLife::ValidateActiveLease(participant.Token))
                    continue;

                Player* bot = FindPlayer(participant.Bot.GuidLow);
                if (IsHumanControlled(bot))
                    continue;
                if (!bot || !bot->IsAlive() || bot->IsBeingTeleported() || bot->IsInFlight() || bot->InBattleground())
                    continue;

                if (bot->duel || bot->IsInCombat())
                    continue;

                if (std::any_of(_duelQueue.begin(), _duelQueue.end(), [&](DuelStep const& step)
                    { return step.EventId == event.EventId && (step.ChallengerGuidLow == participant.Bot.GuidLow || step.TargetGuidLow == participant.Bot.GuidLow); }))
                    continue;

                if (std::any_of(_moveQueue.begin(), _moveQueue.end(), [&](MoveStep const& step)
                    { return step.EventId == event.EventId && step.GuidLow == participant.Bot.GuidLow; }))
                    continue;

                if (bot->GetMapId() != event.EventZone.MapId ||
                    bot->GetDistance2d(participant.ActivityX, participant.ActivityY) > _duelLeashRadius)
                {
                    MoveBot(bot, event.EventZone.MapId, participant.ActivityX, participant.ActivityY,
                        participant.ActivityZ, participant.ActivityO, true);
                    continue;
                }

                // An antic or a fidget stroll under way moves the bot itself; leave it be until it ends.
                if (AnticRunning(participant.Bot.GuidLow) || FidgetWalking(participant.Bot.GuidLow, bot))
                    continue;

                if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot))
                {
                    if (!ai->HasStrategy("duel", BOT_STATE_NON_COMBAT) ||
                        !ai->HasStrategy("stay", BOT_STATE_NON_COMBAT))
                        ApplyPvpStrategies(bot, true);

                    // Home is an area, not a point: a bot inside it stays where it is, and one that
                    // has wandered out walks back to a random spot inside it, the way a player
                    // drifts back to roughly where they were rather than onto the same square.
                    PositionMap& positions = ai->GetAiObjectContext()->GetValue<PositionMap&>("position")->Get();
                    uint32 guidLow = participant.Bot.GuidLow;
                    if (bot->GetDistance2d(participant.ActivityX, participant.ActivityY) <= DuelHomeAreaRadius + 2.0f)
                    {
                        _idleReturnPoint.erase(guidLow);
                        positions["stay"].Set(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                            event.EventZone.MapId);
                    }
                    else
                    {
                        auto point = _idleReturnPoint.find(guidLow);
                        if (point == _idleReturnPoint.end())
                        {
                            float angle = frand(0.0f, 2.0f * float(M_PI));
                            float radius = frand(0.0f, DuelHomeAreaRadius);
                            float x = participant.ActivityX + std::cos(angle) * radius;
                            float y = participant.ActivityY + std::sin(angle) * radius;
                            float z = GroundZ(event.EventZone.MapId, x, y, participant.ActivityZ);
                            point = _idleReturnPoint.emplace(guidLow, Position(x, y, z)).first;
                        }
                        positions["stay"].Set(point->second.GetPositionX(), point->second.GetPositionY(),
                            point->second.GetPositionZ(), event.EventZone.MapId);
                    }
                }

                // Rode in from a drop-off: get off at the spot.
                if (_dismountOnArrival.count(participant.Bot.GuidLow) &&
                    bot->GetDistance2d(participant.ActivityX, participant.ActivityY) <= 6.0f)
                {
                    bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
                    _dismountOnArrival.erase(participant.Bot.GuidLow);
                }

                if (bot->isMoving() && bot->GetDistance2d(participant.ActivityX, participant.ActivityY) <= 3.0f)
                {
                    bot->StopMoving();
                    bot->GetMotionMaster()->Clear();
                    bot->ClearUnitState(UNIT_STATE_CHASE);
                    bot->ClearUnitState(UNIT_STATE_FOLLOW);
                }

                // Between bouts, now and then an antic: a dance, a strafe, a few hops (PvPLife.Antics.*).
                if (!bot->isMoving() && BotActive(bot) && TryStartAntic(bot, participant.Bot.GuidLow))
                    continue;

                if (!bot->isMoving() && BotActive(bot) && IdleFidget(bot, participant, event, fighting))
                    continue;

                // Watching a duel nearby: now and then applaud, cheer, roar or flex. About 4% a
                // second, whatever the guard interval.
                if (!bot->isMoving() && !fighting.empty() && BotActive(bot) &&
                    frand(0.0f, 100.0f) < 4.0f * _duelGuardIntervalMs / 1000.0f &&
                    std::any_of(fighting.begin(), fighting.end(), [bot](Player* duelist)
                        { return bot->IsWithinDistInMap(duelist, 20.0f); }))
                {
                    static uint32 const spectateEmotes[] =
                        { EMOTE_ONESHOT_APPLAUD, EMOTE_ONESHOT_CHEER, EMOTE_ONESHOT_ROAR, EMOTE_ONESHOT_FLEX };
                    bot->HandleEmoteCommand(spectateEmotes[urand(0, 3)]);
                }
            }
        }
    }

    void Manager::ProcessRealPlayerChallenges()
    {
        if (!_duelEnable || !_challengeRealPlayers)
            return;

        uint32 now = GameTimeSeconds();
        UpdateRealPlayersSnapshot();

        for (ActiveEvent const& event : _activeEvents)
        {
            if (event.Closing || !event.FullyPrepared || event.EventZone.Type != ActivityType::Duel || !event.EventZone.ChallengePlayers)
                continue;

            struct RealPlayerTarget
            {
                uint32 GuidLow = 0;
                TeamSide Team = TeamSide::Any;
                uint8 Level = 0;
                float X = 0.0f;
                float Y = 0.0f;
            };

            std::vector<RealPlayerTarget> potentialTargets;
            for (RealPlayerSnapshotEntry const& player : _cachedRealPlayers)
            {
                if (!player.IsAlive || player.IsGameMaster || player.HasDuel || player.InCombat)
                    continue;
                if (player.Security > SEC_PLAYER)
                    continue;
                if (player.MapId != event.EventZone.MapId)
                    continue;
                uint32 playerGuid = player.GuidLow;
                if (!PlayerbotsLife::CanChallengeRealPlayer(playerGuid, now, _realPlayerCooldownSeconds))
                    continue;
                auto playerCd = _playerChallengeCooldown.find(playerGuid);
                if (playerCd != _playerChallengeCooldown.end() && playerCd->second > now)
                    continue;

                RealPlayerTarget rpt;
                rpt.GuidLow = playerGuid;
                rpt.Team = player.Team;
                rpt.Level = player.Level;
                rpt.X = player.X;
                rpt.Y = player.Y;
                potentialTargets.push_back(rpt);
            }

            if (potentialTargets.empty())
                continue;

            for (Participant const& participant : event.Members)
            {
                if (!participant.Deployed || !PlayerbotsLife::ValidateActiveLease(participant.Token))
                    continue;

                Player* bot = FindPlayer(participant.Bot.GuidLow);
                if (IsHumanControlled(bot))
                    continue;
                if (!bot || !bot->IsInWorld() || !bot->IsAlive() || bot->duel || bot->IsInCombat() || bot->IsBeingTeleported())
                    continue;
                auto botCd = _botChallengeCooldown.find(participant.Bot.GuidLow);
                if (botCd != _botChallengeCooldown.end() && botCd->second > now)
                    continue;
                auto duelCd = _botDuelCooldown.find(participant.Bot.GuidLow);
                if (duelCd != _botDuelCooldown.end() && duelCd->second > now)
                    continue;
                if (urand(1, 100) > _realPlayerChallengeChance)
                    continue;

                std::vector<uint32> targets;
                TeamSide botTeam = bot->GetTeamId() == TEAM_ALLIANCE ? TeamSide::Alliance : TeamSide::Horde;
                float scanRadius = static_cast<float>(_realPlayerScanRadius);
                float scanRadiusSq = scanRadius * scanRadius;
                float botX = bot->GetPositionX();
                float botY = bot->GetPositionY();

                for (RealPlayerTarget const& pt : potentialTargets)
                {
                    if (pt.Team != botTeam)
                        continue;
                    float dx = botX - pt.X;
                    float dy = botY - pt.Y;
                    if ((dx * dx + dy * dy) > scanRadiusSq)
                        continue;
                    uint32 diff = std::abs(static_cast<int32>(bot->GetLevel()) - static_cast<int32>(pt.Level));
                    if (diff > _realPlayerMaxLevelDifference)
                        continue;
                    targets.push_back(pt.GuidLow);
                }

                if (targets.empty())
                    continue;
                uint32 targetGuid = targets[urand(0, static_cast<uint32>(targets.size() - 1))];

                Player* targetPlayer = FindPlayer(targetGuid);
                if (!IsChallengeableRealPlayer(targetPlayer, bot))
                    continue;

                // Claim in the shared registry (City Life honours the same cooldown). Every path
                // that does not end in a challenge sent hands the claim back.
                uint32 claimExpiry = now + _realPlayerCooldownSeconds;
                if (!PlayerbotsLife::TryClaimRealPlayerChallenge(targetGuid, now, _realPlayerCooldownSeconds))
                    continue;

                PlayerbotsLife::LeaseToken playerToken;
                if (!ScheduleDuel(event.EventId, 0, participant.Token, playerToken, bot->GetGUID().GetCounter(), targetGuid,
                    bot->GetName(), targetPlayer->GetName(), urand(1, 4), true, claimExpiry))
                {
                    PlayerbotsLife::ReleaseRealPlayerChallengeClaim(targetGuid, claimExpiry);
                    continue;
                }
                // The player's own cooldown is set when the challenge is confirmed (ProcessDuelQueue).
                _botChallengeCooldown[bot->GetGUID().GetCounter()] = now + _botChallengeCooldownSeconds;
            }
        }
    }

    void Manager::MaintainPersistentWorldPvp()
    {
        if (!_alwaysActiveWorldPvp)
            return;
        if (_maxActiveEvents > 0 && _activeEvents.size() >= _maxActiveEvents)
            return;

        uint32 active = 0;
        std::unordered_set<uint32> activeIds;
        for (ActiveEvent const& e : _activeEvents)
        {
            // Reactive defenses are typed Skirmish but are not hotspots: they do not count here.
            if (e.EventZone.Type == ActivityType::Skirmish && e.Origin == EventOrigin::Zone)
                ++active;
            activeIds.insert(e.EventZone.Id);
        }
        if (active >= _minActiveSkirmishes)
            return;

        std::vector<Zone> zones = LoadZones(true, static_cast<int>(ActivityType::Skirmish));
        zones.erase(std::remove_if(zones.begin(), zones.end(), [&](Zone const& z) { return activeIds.count(z.Id) != 0; }), zones.end());

        while (!zones.empty() && active < _minActiveSkirmishes && active < _maxActiveSkirmishes)
        {
            if (_maxActiveEvents > 0 && _activeEvents.size() >= _maxActiveEvents)
                break;

            uint32 totalWeight = 0;
            for (Zone const& z : zones)
                totalWeight += std::max<uint32>(1, z.Weight);

            uint32 roll = urand(1, std::max<uint32>(1, totalWeight));
            size_t chosen = 0;
            uint32 cursor = 0;
            for (size_t i = 0; i < zones.size(); ++i)
            {
                cursor += std::max<uint32>(1, zones[i].Weight);
                if (roll <= cursor)
                {
                    chosen = i;
                    break;
                }
            }

            Zone zone = zones[chosen];
            zones.erase(zones.begin() + chosen);
            if (StartZone(zone))
            {
                ++active;
                activeIds.insert(zone.Id);
            }
        }
    }

    void Manager::MaintainPersistentDuelZones()
    {
        if (!_duelEnable || !_duelAlwaysActive)
            return;
        if (_maxActiveEvents > 0 && _activeEvents.size() >= _maxActiveEvents)
            return;

        std::unordered_set<uint32> activeIds;
        for (ActiveEvent const& e : _activeEvents)
            if (e.EventZone.Type == ActivityType::Duel)
                activeIds.insert(e.EventZone.Id);

        std::vector<Zone> zones = LoadZones(false, static_cast<int>(ActivityType::Duel));
        for (Zone const& z : zones)
        {
            if (_maxActiveEvents > 0 && _activeEvents.size() >= _maxActiveEvents)
                break;
            if (activeIds.find(z.Id) == activeIds.end())
                StartZone(z);
        }
    }

    void Manager::TryStartRandomSkirmish()
    {
        if (_maxActiveEvents > 0 && _activeEvents.size() >= _maxActiveEvents)
            return;

        uint32 active = 0;
        std::unordered_set<uint32> activeIds;
        for (ActiveEvent const& e : _activeEvents)
        {
            if (e.EventZone.Type == ActivityType::Skirmish && e.Origin == EventOrigin::Zone)
                ++active;
            activeIds.insert(e.EventZone.Id);
        }
        if (active >= _maxActiveSkirmishes || urand(1, 100) > _randomSkirmishChance)
            return;

        std::vector<Zone> zones = LoadZones(true, static_cast<int>(ActivityType::Skirmish));
        zones.erase(std::remove_if(zones.begin(), zones.end(), [&](Zone const& z) { return activeIds.count(z.Id) != 0; }), zones.end());
        if (zones.empty())
            return;

        uint32 total = 0;
        for (Zone const& z : zones) total += z.Weight;
        uint32 roll = urand(1, std::max<uint32>(1, total));
        uint32 cursor = 0;
        for (Zone const& z : zones)
        {
            cursor += z.Weight;
            if (roll <= cursor)
            {
                StartZone(z);
                return;
            }
        }
    }

    void Manager::TryStartFactionCampaign()
    {
        if ((!_forTheHordeEnable && !_forTheAllianceEnable) || urand(1, 100) > _factionCampaignChance)
            return;
        if (_maxActiveEvents > 0 && _activeEvents.size() >= _maxActiveEvents)
            return;

        uint32 now = GameTimeSeconds();
        if (_lastFactionCampaign && SecondsSince(now, _lastFactionCampaign) < _factionCampaignGlobalCooldown)
            return;

        std::vector<Zone> zones;
        if (_forTheHordeEnable)
        {
            std::vector<Zone> fth = LoadZones(true, static_cast<int>(ActivityType::ForTheHorde));
            zones.insert(zones.end(), fth.begin(), fth.end());
        }
        if (_forTheAllianceEnable)
        {
            std::vector<Zone> fta = LoadZones(true, static_cast<int>(ActivityType::ForTheAlliance));
            zones.insert(zones.end(), fta.begin(), fta.end());
        }
        if (zones.empty())
            return;

        while (!zones.empty())
        {
            uint32 totalWeight = 0;
            for (Zone const& z : zones)
                totalWeight += std::max<uint32>(1, z.Weight);

            uint32 roll = urand(1, std::max<uint32>(1, totalWeight));
            size_t chosen = 0;
            uint32 cursor = 0;
            for (size_t i = 0; i < zones.size(); ++i)
            {
                cursor += std::max<uint32>(1, zones[i].Weight);
                if (roll <= cursor)
                {
                    chosen = i;
                    break;
                }
            }

            Zone zone = zones[chosen];
            zones.erase(zones.begin() + chosen);
            if (StartZone(zone))
            {
                _lastFactionCampaign = now;
                return;
            }
        }
    }

    // Reactive Raid Response
    void Manager::TryQueueReactiveRaidResponse()
    {
        if (!_reactiveEnable || !_pendingResponses.empty())
            return;
        if (_maxActiveEvents > 0 && _activeEvents.size() >= _maxActiveEvents)
            return;

        uint32 now = GameTimeSeconds();
        if (_lastReactiveResponse && SecondsSince(now, _lastReactiveResponse) < _reactiveCooldownSeconds)
            return;

        struct InvaderTarget
        {
            uint32 GuidLow = 0;
            std::string Name;
            TeamSide ResponseTeam = TeamSide::Any;
            uint32 MapId = 0;
            float X = 0.0f;
            float Y = 0.0f;
            float Z = 0.0f;
            float O = 0.0f;
            uint8 Level = 0;
            uint32 GroupCount = 0;
        };
        std::optional<InvaderTarget> target;

        UpdateRealPlayersSnapshot();
        for (RealPlayerSnapshotEntry const& candidate : _cachedRealPlayers)
        {
            if (!candidate.IsAlive || !candidate.IsPvP || candidate.IsGameMaster || candidate.Security > SEC_PLAYER)
                continue;
            if (!candidate.IsGroupLeader || candidate.GroupMembersCount < _reactiveMinGroupSize)
                continue;

            TeamSide responseTeam = candidate.Team == TeamSide::Alliance ? TeamSide::Horde : TeamSide::Alliance;
            if (responseTeam == TeamSide::Horde && !_reactiveHordeEnable)
                continue;
            if (responseTeam == TeamSide::Alliance && !_reactiveAllianceEnable)
                continue;

            Player* leader = FindPlayer(candidate.GuidLow);
            if (!leader || !leader->GetSession() || !leader->IsInWorld() || !leader->GetMap() || leader->GetMap()->Instanceable())
                continue;

            AreaTableEntry const* area = sAreaTableStore.LookupEntry(leader->GetAreaId());
            if (area && (area->flags & AREA_FLAG_SANCTUARY))
                continue;
            // An invasion, not a raid in its own faction's territory: skip zones the raid's faction
            // controls (and team 6, which the core treats as never hostile), and Wintergrasp while
            // its battle is on.
            uint32 leaderZone = leader->GetZoneId();
            if (IsWintergraspWarZone(leaderZone))
                continue;
            if (AreaTableEntry const* zoneEntry = sAreaTableStore.LookupEntry(leaderZone))
            {
                uint32 ownTerritory = candidate.Team == TeamSide::Alliance ? AREATEAM_ALLY : AREATEAM_HORDE;
                if (zoneEntry->team == ownTerritory || zoneEntry->team == AREATEAM_ANY)
                    continue;
            }

            Group* group = leader->GetGroup();
            if (!group || !group->IsLeader(leader->GetGUID()) || group->GetMembersCount() < _reactiveMinGroupSize)
                continue;

            uint32 nearbyRealPlayers = 0;
            for (GroupReference const* ref = group->GetFirstMember(); ref != nullptr; ref = ref->next())
            {
                Player const* member = ref->GetSource();
                if (member && IsHumanPlayer(member) && member->GetMapId() == leader->GetMapId() &&
                    member->GetExactDist(leader) <= 100.0f)
                {
                    ++nearbyRealPlayers;
                }
            }
            if (nearbyRealPlayers < _reactiveMinGroupSize)
                continue;

            InvaderTarget it;
            it.GuidLow = leader->GetGUID().GetCounter();
            it.Name = leader->GetName();
            it.ResponseTeam = responseTeam;
            it.MapId = leader->GetMapId();
            it.X = leader->GetPositionX();
            it.Y = leader->GetPositionY();
            it.Z = leader->GetPositionZ();
            it.O = leader->GetOrientation();
            it.Level = leader->GetLevel();
            it.GroupCount = nearbyRealPlayers;
            target = it;
            break;
        }

        if (!target)
            return;

        uint32 budget = CalculatePopulationBudget();
        uint32 currentActive = PlayerbotsLife::GetCooperationSnapshot().PvPLifeCount;

        uint32 availableBudget = (budget > currentActive) ? (budget - currentActive) : 0;
        if (availableBudget < 2)
            return;

        uint32 desired = std::min<uint32>(target->GroupCount, _reactiveMaxResponders);
        desired = std::min<uint32>(desired, availableBudget);

        std::unordered_set<uint32> excluded;
        for (ActiveEvent const& event : _activeEvents)
            for (Participant const& member : event.Members)
                excluded.insert(member.Bot.GuidLow);
        for (PendingRaidResponse const& r : _pendingResponses)
            for (BotCandidate const& b : r.Bots)
                excluded.insert(b.GuidLow);
        for (ReleasingParticipant const& rp : _releasingParticipants)
            excluded.insert(rp.Part.Bot.GuidLow);

        uint8 minLvl;
        uint8 maxLvl;
        if (_reactiveLevelMode == ResponderLevelMode::Fixed)
        {
            minLvl = _reactiveFixedLevel;
            maxLvl = _reactiveFixedLevel;
        }
        else
        {
            minLvl = static_cast<uint8>(std::max<int32>(1, static_cast<int32>(target->Level) - _reactiveMaxLevelDifference));
            maxLvl = static_cast<uint8>(std::min<int32>(80, static_cast<int32>(target->Level) + _reactiveMaxLevelDifference));
        }
        minLvl = EffectiveMinLevel(minLvl); // PvPLife.Bots.GlobalMinLevel applies to defenders too
        if (maxLvl < minLvl)
            return;

        std::vector<BotCandidate> responders = LoadCandidates(target->ResponseTeam, minLvl, maxLvl, excluded, desired);
        if (responders.size() > desired)
            responders.resize(desired);
        for (BotCandidate const& bot : responders)
            excluded.insert(bot.GuidLow);

        std::vector<uint32> pendingLogins;
        if (responders.size() < desired && _reactiveLoginOfflineBots)
        {
            std::vector<BotCandidate> offline = LoadOfflineCandidates(target->ResponseTeam, minLvl, maxLvl, excluded);
            if (!_offlineCandidateCache.count(OfflineCacheKey(minLvl, maxLvl)))
                return; // Re-evaluate the raid after the asynchronous snapshot is available.
            uint32 need = desired - static_cast<uint32>(responders.size());
            need = std::min<uint32>(need, _reactiveMaxConcurrentLogins);
            if (offline.size() > need)
                offline.resize(need);
            for (BotCandidate const& bot : offline)
            {
                responders.push_back(bot);
                pendingLogins.push_back(bot.GuidLow);
            }
        }

        if (responders.empty())
            return;

        PendingRaidResponse pending;
        pending.ResponseId = _nextResponseId++;
        pending.TriggerGuidLow = target->GuidLow;
        pending.ResponseTeam = target->ResponseTeam;
        pending.MapId = target->MapId;
        pending.TargetX = target->X;
        pending.TargetY = target->Y;
        pending.TargetZ = target->Z;
        pending.TargetO = target->O;
        pending.StartAt = now + _reactiveLoginDelaySeconds;
        pending.Deadline = now + _reactiveLoginTimeoutSeconds;
        pending.State = ResponseState::Reserved;
        pending.Bots = std::move(responders);
        pending.PendingLoginGuids = std::move(pendingLogins);

        std::vector<uint32> admissionGuids;
        for (BotCandidate const& bot : pending.Bots)
            admissionGuids.push_back(bot.GuidLow);
        pending.AdmissionRequestId = PlayerbotsLife::SubmitAdmissionRequest(
            PlayerbotsLife::ReservationOwner::PvPLife, (uint64(1) << 63) | pending.ResponseId,
            admissionGuids, budget, now, pending.Deadline);
        if (!pending.AdmissionRequestId)
            return;
        _pendingResponses.push_back(std::move(pending));

        LOG_INFO("module", "[PvPLife] Queued {} raid response for {} invaders (leader {}).",
            TeamName(target->ResponseTeam), target->GroupCount, target->Name);
        AuditLog("REACTIVE_START", "responseId=" + std::to_string(_pendingResponses.back().ResponseId) +
            " trigger=" + std::to_string(target->GuidLow) +
            " team=" + TeamName(target->ResponseTeam) + " responders=" + std::to_string(_pendingResponses.back().Bots.size()));
    }

    bool Manager::CancelPendingRaidResponse(uint64 responseId, std::string const& reason)
    {
        bool found = false;
        for (auto& pending : _pendingResponses)
        {
            if ((responseId == 0 || pending.ResponseId == responseId) && !pending.Cancelled)
            {
                found = true;
                LOG_INFO("module", "[PvPLife] Cancelled pending raid response #{} (reason: {})", pending.ResponseId, reason);
                for (BotCandidate const& b : pending.Bots)
                    PlayerbotsLife::ReleaseLeaseExact(b.Token);
                if (pending.AdmissionRequestId)
                    PlayerbotsLife::CancelAdmissionRequest(pending.AdmissionRequestId, PlayerbotsLife::ReservationOwner::PvPLife);
                pending.Bots.clear();
                pending.Cancelled = true;
                if (responseId != 0)
                    break;
            }
        }
        _pendingResponses.erase(std::remove_if(_pendingResponses.begin(), _pendingResponses.end(),
            [](PendingRaidResponse const& p) { return p.Cancelled; }), _pendingResponses.end());
        return found;
    }

    void Manager::StartPendingRaidResponses()
    {
        uint32 now = GameTimeSeconds();
        for (PendingRaidResponse& pending : _pendingResponses)
        {
            if (pending.Cancelled)
                continue;
            Player* leader = FindPlayer(pending.TriggerGuidLow);
            Group* group = leader ? leader->GetGroup() : nullptr;
            bool validTrigger = leader && IsHumanPlayer(leader) && leader->IsInWorld() && leader->IsAlive() &&
                leader->IsPvP() && leader->GetMapId() == pending.MapId && group &&
                group->IsLeader(leader->GetGUID()) && group->GetMembersCount() >= _reactiveMinGroupSize;
            bool expired = pending.Deadline <= now;
            if (!validTrigger || (expired && !pending.LeasesGranted))
            {
                if (pending.AdmissionRequestId)
                    PlayerbotsLife::CancelAdmissionRequest(pending.AdmissionRequestId, PlayerbotsLife::ReservationOwner::PvPLife);
                for (BotCandidate const& bot : pending.Bots)
                    PlayerbotsLife::ReleaseLeaseExact(bot.Token);
                pending.Cancelled = true;
                continue;
            }
            if (!pending.LeasesGranted || pending.StartAt > now)
                continue;
            // Wait for every responder to be in the world, but only until the deadline: then start
            // with whoever made it rather than drop the whole response for one failed login.
            bool allOnline = std::all_of(pending.Bots.begin(), pending.Bots.end(), [&](BotCandidate const& bot)
            {
                Player* player = FindPlayer(bot.GuidLow);
                return player && player->IsInWorld() && !player->IsBeingTeleported();
            });
            if (!allOnline && !expired)
                continue;
            TeamSide invaderTeam = pending.ResponseTeam == TeamSide::Horde ? TeamSide::Alliance : TeamSide::Horde;
            ActiveEvent event;
            event.EventId = _nextEventId++;
            event.Origin = EventOrigin::ReactiveRaidResponse;
            event.TriggerGuidLow = pending.TriggerGuidLow;
            event.EventZone.Name = "Reactive Defense (" + std::string(TeamName(pending.ResponseTeam)) + ")";
            event.EventZone.MapId = pending.MapId;
            event.EventZone.ZoneId = leader->GetZoneId();
            event.EventZone.Type = ActivityType::Skirmish;
            event.EventZone.AttackerTeam = pending.ResponseTeam;
            event.EventZone.DefenderTeam = invaderTeam;
            event.EventZone.BotChat = _botChatEnable;
            event.EventZone.TargetX = pending.TargetX;
            event.EventZone.TargetY = pending.TargetY;
            event.EventZone.TargetZ = pending.TargetZ;
            event.EventZone.TargetO = pending.TargetO;
            event.DurationSeconds = _reactiveDurationMinutes * MINUTE;
            uint32 queuedWork = static_cast<uint32>(pending.Bots.size() + _releasingParticipants.size());
            for (ActiveEvent const& queued : _activeEvents)
                if (!queued.Closing)
                    queuedWork += static_cast<uint32>(std::count_if(queued.Members.begin(), queued.Members.end(),
                        [](Participant const& part) { return !part.Deployed; }));
            uint32 batches = (queuedWork + _maxChangesPerTick - 1) / _maxChangesPerTick;
            event.PrepareDeadline = now + std::max<uint32>(60, 2 * _tickSeconds * batches + _arrivalStaggerMax);
            event.EndsAt = event.PrepareDeadline + event.DurationSeconds;
            event.MinimumTotal = 1;
            float angle = pending.TargetO + 3.14159265f;
            uint32 index = 0;
            uint32 skipped = 0;
            uint8 minLevel = 80;
            uint8 maxLevel = 1;
            for (BotCandidate const& bot : pending.Bots)
            {
                Player* player = FindPlayer(bot.GuidLow);
                if (!IsSafeBot(player, true) || IsHumanControlled(player))
                {
                    PlayerbotsLife::ReleaseLeaseExact(bot.Token);
                    ++skipped;
                    continue;
                }
                Participant part;
                part.Bot = bot;
                part.Attacker = true;
                part.Token = bot.Token;
                if (!PlayerbotsLife::PromoteLease(part.Token, event.EventId))
                {
                    PlayerbotsLife::ReleaseLeaseExact(bot.Token);
                    ++skipped;
                    continue;
                }
                // Columns side by side, rows further back, so no two bots share a spot.
                uint32 column = index % _reactiveFormationColumns;
                uint32 row = index / _reactiveFormationColumns;
                float lane = static_cast<float>(static_cast<int32>(column) -
                    static_cast<int32>(_reactiveFormationColumns / 2)) * _reactiveLaneSpacing;
                float depth = _reactiveRallyDistance + static_cast<float>(row) * _reactiveLaneSpacing;
                part.ActivityX = pending.TargetX + std::cos(angle) * depth - std::sin(angle) * lane;
                part.ActivityY = pending.TargetY + std::sin(angle) * depth + std::cos(angle) * lane;
                part.ActivityZ = GroundZ(pending.MapId, part.ActivityX, part.ActivityY, pending.TargetZ);
                part.ActivityO = angle;
                minLevel = std::min(minLevel, bot.Level);
                maxLevel = std::max(maxLevel, bot.Level);
                event.Members.push_back(part);
                ++index;
            }
            pending.Bots.clear();
            pending.Cancelled = true;
            if (event.Members.empty())
            {
                LOG_INFO("module", "[PvPLife] Raid response #{} abandoned: none of its responders was ready ({} dropped).",
                    pending.ResponseId, skipped);
                continue;
            }
            event.EventZone.MinLevel = minLevel;
            event.EventZone.MaxLevel = maxLevel;
            // The cooldown starts with a response that actually deploys.
            _lastReactiveResponse = now;
            LOG_INFO("module", "[PvPLife] START #{} {} responders={} (dropped {})",
                event.EventId, event.EventZone.Name, event.Members.size(), skipped);
            AuditLog("START", "event=" + std::to_string(event.EventId) + " spot=" + event.EventZone.Name +
                " mode=reactive responseId=" + std::to_string(pending.ResponseId) +
                " trigger=" + std::to_string(pending.TriggerGuidLow) + " team=" + TeamName(pending.ResponseTeam) +
                " responders=" + std::to_string(event.Members.size()) + " dropped=" + std::to_string(skipped) +
                " durationSeconds=" + std::to_string(event.DurationSeconds));
            _activeEvents.push_back(std::move(event));
        }
        _pendingResponses.erase(std::remove_if(_pendingResponses.begin(), _pendingResponses.end(),
            [](PendingRaidResponse const& pending) { return pending.Cancelled; }), _pendingResponses.end());
    }

    void Manager::Announce(std::string const& text) const
    {
        if (!_announce)
            return;
        sWorldSessionMgr->SendServerMessage(SERVER_MSG_STRING, text);
    }

    void Manager::Update(uint32 diff)
    {
        ProcessQueuedDisengages();
        bool managementTick = _timerMs <= diff;
        if (managementTick)
        {
            _timerMs = _tickSeconds * IN_MILLISECONDS;
            _changesThisTick = 0;
        }
        else
            _timerMs -= diff;
        _transactionProcessor.ProcessReadyCallbacks();
        _queryProcessor.ProcessReadyCallbacks();
        PlayerbotsLife::ResolveAdmissionRequests(GameTimeSeconds());
        ProcessReleasingParticipants(); // Works while disabled, reloading, or DB-unavailable.
        if (!_enable || !_databaseReady)
            return;
        if (_startupElapsedMs < _startupDelaySeconds * IN_MILLISECONDS)
        {
            _startupElapsedMs += diff;
            return;
        }
        ExpireEvents(); // Reject expired work before any movement or combat operation.
        ProcessAdmissionGrants();
        StartPendingRaidResponses();
        ProcessMoveQueue();
        ProcessPendingWalks();
        ProcessDuelQueue();
        ProcessEngagementQueue();
        ProcessChatQueue();
        if (_duelGuardTimerMs <= diff)
        {
            _duelGuardTimerMs = _duelGuardIntervalMs;
            MaintainDuelParticipants();
        }
        else
            _duelGuardTimerMs -= diff;
        UpdateFootwork();
        _anticTimerMs += diff;
        if (_anticTimerMs >= 100)
        {
            _anticTimerMs = 0;
            KeepDuelStrategies();
            UpdateAntics();
            UpdateDuelMedics();
        }
        if (managementTick)
        {
            uint32 nowSec = GameTimeSeconds();
            PlayerbotsLife::ScavengeStaleLeases(nowSec);
            if (nowSec - _lastCooldownPruneTime >= 60)
            {
                _lastCooldownPruneTime = nowSec;
                PruneExpiredCooldowns();
                PlayerbotsLife::PruneRealPlayerCooldowns(nowSec);
            }
            UpdateRealPlayersSnapshot(); // keeps real-player presence current for DemandMode
            ProcessPreparingEvents();
            TryQueueReactiveRaidResponse();
            MaintainPersistentDuelZones();
            MaintainPersistentWorldPvp();
            TryStartRandomSkirmish();
            TryStartFactionCampaign();
            MaintainBotDuels();
            ProcessRealPlayerChallenges();
            KeepLeasedLevelsFixed();
        }
    }

    void Manager::PrintStatus(ChatHandler* handler) const
    {
        uint32 startupTarget = _startupDelaySeconds * IN_MILLISECONDS;
        uint32 startupLeft = _startupElapsedMs < startupTarget ?
            (startupTarget - _startupElapsedMs + 999) / 1000 : 0;
        handler->PSendSysMessage("PvP Life: enabled={} DB={} startup delay={}s events={} pendingResponses={} move={} duel={} engage={}",
            _enable ? 1 : 0, _databaseReady ? "ready" : "missing", startupLeft,
            _activeEvents.size(), _pendingResponses.size(), _moveQueue.size(), _duelQueue.size(), _engagementQueue.size());

        uint32 budget = CalculatePopulationBudget();
        uint32 liveBots = GetLiveWorldBotCount();
        handler->PSendSysMessage(
            "Population: budget={} (live bots={}) mode={} borrowPercent={}% maxTotal={} proportionalScaling={} bots: prefix='{}' respectActivity={} minOnlineReq={}",
            budget, liveBots, _budgetMode == BudgetMode::Percentage ? "percentage" : "fixed", _borrowPercent,
            _maxTotal == 0 ? "uncapped" : std::to_string(_maxTotal).c_str(),
            _proportionalZoneScaling ? 1 : 0,
            _botAccountPrefix, _respectPlayerbotActivity ? 1 : 0, _minOnlineRequired);

        uint32 anticsRunning = uint32(std::count_if(_anticRoutines.begin(), _anticRoutines.end(),
            [](auto const& entry) { return LifeMotion::Running(entry.second); }));
        handler->PSendSysMessage("Antics: enabled={} running={} chance={}%/min cooldown={}s hopDismount={}%",
            _antics.Enable ? 1 : 0, anticsRunning, _antics.ChancePerMinute, _antics.CooldownSeconds, _antics.HopDismountChance);
        handler->PSendSysMessage("Duel medics: enabled={} medics={}% of healers chance={}% max={} radius={} pending={}",
            _medics.Enable ? 1 : 0, _medics.MedicPercent, _medics.Chance, _medics.MaxPerDuel, _medics.Radius, _pendingHeals.size());

        handler->PSendSysMessage("Audit: linesWritten={} dropped={} openFailures={} writeFailures={} inQueue={}",
            sPvPLifeAudit.GetLinesWritten(), sPvPLifeAudit.GetDroppedCount(),
            sPvPLifeAudit.GetOpenFailures(), sPvPLifeAudit.GetWriteFailures(),
            sPvPLifeAudit.GetQueueSize());

        PlayerbotsLife::CooperationSnapshot snap = PlayerbotsLife::GetCooperationSnapshot();
        handler->PSendSysMessage("Reservations: total={} city={} pvp={} active={} releasing={} provisional={} duelSlots={}",
            snap.TotalReserved, snap.CityLifeCount, snap.PvPLifeCount, snap.ActiveCount, snap.ReleasingCount,
            snap.ProvisionalCount, snap.ActiveDuelSlots);

        uint32 now = GameTimeSeconds();
        for (ActiveEvent const& e : _activeEvents)
        {
            uint32 left = e.EndsAt > now ? e.EndsAt - now : 0;
            handler->PSendSysMessage("#{} {} [{}] origin={} level {}~{} bots={} time left={}m",
                e.EventId, e.EventZone.Name, TypeName(e.EventZone.Type),
                e.Origin == EventOrigin::ReactiveRaidResponse ? "ReactiveDefense" : "Zone",
                e.EventZone.MinLevel, e.EventZone.MaxLevel,
                e.Members.size(), left / MINUTE);
        }

        for (PendingRaidResponse const& p : _pendingResponses)
        {
            uint32 left = p.StartAt > now ? p.StartAt - now : 0;
            handler->PSendSysMessage("[Pending] Reactive Defense team={} triggerGuid={} responders={} startsIn={}s",
                TeamName(p.ResponseTeam), p.TriggerGuidLow, p.Bots.size(), left);
        }
    }

    void Manager::PrintScan(ChatHandler* handler, uint8 minLevel, uint8 maxLevel)
    {
        uint8 effectiveMin = EffectiveMinLevel(minLevel);
        uint8 effectiveMax = maxLevel;
        if (effectiveMax < effectiveMin)
            effectiveMax = effectiveMin;

        uint32 alliance = 0;
        uint32 horde = 0;
        uint32 total = 0;

        // QueryLimit 0 means "scale with the population"; give the scan the same ceiling.
        uint32 scanLimit = _botQueryLimit ? _botQueryLimit : 1500;
        QueryResult result = CharacterDatabase.Query(
            "SELECT guid,account,name,level,race,class,online FROM characters "
            "WHERE online=1 AND level BETWEEN {} AND {} ORDER BY level DESC LIMIT {}",
            effectiveMin, effectiveMax, scanLimit);

        if (result)
        {
            do
            {
                Field* f = result->Fetch();
                BotCandidate b;
                b.GuidLow = f[0].Get<uint32>();
                b.AccountId = f[1].Get<uint32>();
                b.Name = f[2].Get<std::string>();
                b.Level = f[3].Get<uint8>();
                b.Race = f[4].Get<uint8>();
                b.Class = f[5].Get<uint8>();
                b.Online = f[6].Get<uint8>() != 0;
                b.Team = TeamForRace(b.Race);

                // The same test admission uses (random bots and configured accounts alike).
                if (!IsSafeBot(FindPlayer(b.GuidLow)))
                    continue;

                ++total;
                if (b.Team == TeamSide::Alliance)
                    ++alliance;
                else if (b.Team == TeamSide::Horde)
                    ++horde;
            } while (result->NextRow());
        }

        handler->PSendSysMessage("PvP Life scan lvl {}..{} prefix='{}': Alliance={} Horde={} Total={} queryLimit={} globalMin={}.",
            effectiveMin, effectiveMax, _botAccountPrefix, alliance, horde, total, scanLimit, _globalMinLevel);
        if (effectiveMin > minLevel)
            handler->PSendSysMessage("(The lower bound was raised to PvPLife.Bots.GlobalMinLevel.)");

        std::vector<Zone> spots = LoadZones(true);
        if (spots.empty())
        {
            handler->PSendSysMessage("No enabled/cooldown-ready hotspots right now.");
            return;
        }

        std::unordered_set<uint32> exclude;
        uint32 shown = 0;
        for (Zone const& h : spots)
        {
            if (shown >= 12)
                break;
            uint8 zoneMin = EffectiveMinLevel(h.MinLevel, h.Type);
            std::vector<BotCandidate> attackers = LoadCandidates(h.AttackerTeam, zoneMin, h.MaxLevel, exclude, _maxBotsPerSide);
            std::vector<BotCandidate> defenders = LoadCandidates(h.DefenderTeam, zoneMin, h.MaxLevel, exclude, _maxBotsPerSide);
            handler->PSendSysMessage("{}: {} attackers={} need={}..{} / {} defenders={} need={}..{} lvl={}..{} weight={}",
                h.Name, TeamName(h.AttackerTeam), attackers.size(), h.AttackersMin, h.AttackersMax,
                TeamName(h.DefenderTeam), defenders.size(), h.DefendersMin, h.DefendersMax,
                zoneMin, h.MaxLevel, h.Weight);
            ++shown;
        }
        if (spots.size() > shown)
            handler->PSendSysMessage("... and {} more ready hotspots (zones on cooldown are not listed).", spots.size() - shown);
    }

    void Manager::PrintCompat(ChatHandler* handler) const
    {
        uint32 activeFlags = PlayerbotsLife::GetActiveModules();
        bool clActive = (activeFlags & PlayerbotsLife::MODULE_CITY_LIFE) != 0;
        bool pvpActive = (activeFlags & PlayerbotsLife::MODULE_PVP_LIFE) != 0;
        bool artActive = (activeFlags & PlayerbotsLife::MODULE_ARTISANS) != 0;

        handler->PSendSysMessage("=== Life Modules Compatibility & Coexistence ===");
        handler->PSendSysMessage("CityLife: {}", clActive ? "|cff00ff00ACTIVE|r" : "|cffff0000INACTIVE|r");
        handler->PSendSysMessage("PvPLife:  {}", pvpActive ? "|cff00ff00ACTIVE|r" : "|cffff0000INACTIVE|r");
        handler->PSendSysMessage("Artisans: {}", artActive ? "|cff00ff00ACTIVE|r" : "|cffff0000INACTIVE|r");

        handler->PSendSysMessage("Protocol Version: {} | Registry Address: {:p}",
            PlayerbotsLife::GetReservationProtocolVersion(),
            PlayerbotsLife::GetReservationRegistryAddress());
        handler->PSendSysMessage("Exact ownership protection: ALWAYS ENABLED (a bot reserved by another Life module is never borrowed)");
        handler->PSendSysMessage("Admission is automatic; accounts and existing assignments are not partitioned.");

        PrintReservations(handler);
    }

    void Manager::PrintReservations(ChatHandler* handler) const
    {
        auto snapshot = PlayerbotsLife::GetCooperationSnapshot(GameTimeSeconds());
        handler->PSendSysMessage("Total Reserved Bots: {} (CityLife: {}, PvPLife: {}, Artisans: {})",
            snapshot.TotalReserved, snapshot.CityLifeCount, snapshot.PvPLifeCount, snapshot.ArtisansCount);
        handler->PSendSysMessage("Lease States: Active={} | Provisional={} (stale={}, oldest={}s) | Releasing={} (stale={}, oldest={}s)",
            snapshot.ActiveCount, snapshot.ProvisionalCount, snapshot.StaleProvisionalCount, snapshot.OldestProvisionalAgeSec,
            snapshot.ReleasingCount, snapshot.StaleReleasingCount, snapshot.OldestReleasingAgeSec);
        handler->PSendSysMessage("Admission requests={} unacknowledged grants={} releasing actors={} work used={}/{}",
            snapshot.WaitingAdmissions, snapshot.UnacknowledgedGrants, _releasingParticipants.size(),
            _changesThisTick, _maxChangesPerTick);
    }

    void Manager::PrintDebugStatus(ChatHandler* handler) const
    {
        handler->PSendSysMessage("=== PvPLife Debug Counters ===");
        handler->PSendSysMessage("Active Events: {}", _activeEvents.size());
        handler->PSendSysMessage("Pending Moves: {}", _moveQueue.size());
        handler->PSendSysMessage("Pending Duels: {}", _duelQueue.size());
        handler->PSendSysMessage("Pending Engagements: {}", _engagementQueue.size());
        handler->PSendSysMessage("Pending Chats: {}", _chatQueue.size());
        handler->PSendSysMessage("Pending Raid Responses: {}", _pendingResponses.size());
        handler->PSendSysMessage("Cached Bot Accounts: {}", _botAccountCache.size());

        PlayerbotsLife::CooperationSnapshot snap = PlayerbotsLife::GetCooperationSnapshot();
        handler->PSendSysMessage("Cooperation: Reserved Total={} (City={}, PvP={}) | Leases: Prov={} Active={} Rel={} | DuelSlots={}",
            snap.TotalReserved, snap.CityLifeCount, snap.PvPLifeCount,
            snap.ProvisionalCount, snap.ActiveCount, snap.ReleasingCount, snap.ActiveDuelSlots);
    }

    void Manager::PrintHelp(ChatHandler* handler, bool isWpvp) const
    {
        std::string cmdPrefix = isWpvp ? ".wpvp" : ".pvplife";
        handler->PSendSysMessage("{} commands:", cmdPrefix);
        handler->PSendSysMessage("{} help", cmdPrefix);
        handler->PSendSysMessage("{} status", cmdPrefix);
        handler->PSendSysMessage("{} scan [minLvl] [maxLvl]", cmdPrefix);
        handler->PSendSysMessage("{} start <zone|spot>", cmdPrefix);
        handler->PSendSysMessage("{} stop <id|all>", cmdPrefix);
        handler->PSendSysMessage("{} reload   (re-checks the tables and re-reads zones and chat lines; .reload config re-reads the conf file)", cmdPrefix);
        handler->PSendSysMessage("{} compat", cmdPrefix);
        handler->PSendSysMessage("{} reservations", cmdPrefix);
        handler->PSendSysMessage("{} response status | stop <id|all>", cmdPrefix);
        handler->PSendSysMessage("{} debug [status | validate | bot <guid|name> | antic <name> <antic>]", cmdPrefix);
        handler->PSendSysMessage("{} zone list", cmdPrefix);
        handler->PSendSysMessage("{} zone create <name> <type> <attacker> <defender> <minLvl> <maxLvl>   (created disabled)", cmdPrefix);
        handler->PSendSysMessage("   <type>: skirmish, duel, forthehorde, forthealliance; <attacker>/<defender>: alliance, horde, any");
        handler->PSendSysMessage("{} zone rally <name> | target <name>   (save GM position)", cmdPrefix);
        handler->PSendSysMessage("{} zone counts <name> <attMin> <attMax> <defMin> <defMax>", cmdPrefix);
        handler->PSendSysMessage("{} zone duration <name> <minM> <maxM>", cmdPrefix);
        handler->PSendSysMessage("{} zone weight <name> <weight> | cooldown <name> <seconds>", cmdPrefix);
        handler->PSendSysMessage("{} zone challenge <name> <0|1> | chat <name> <0|1> | enable <name> <0|1>", cmdPrefix);
        if (isWpvp)
        {
            handler->PSendSysMessage(".wpvp spot create <name> <attacker> <defender> <minLvl> <maxLvl>   (same faction = duel, else skirmish)");
            handler->PSendSysMessage(".wpvp spot list | rally | target | counts | duration | weight | cooldown | challenge | chat | enable   (as zone ...)");
        }
    }

    bool Manager::TryParseUInt32(std::string const& str, uint32& outVal)
    {
        // Digits only: strtoul would also take a sign or leading spaces ("-1" -> 4294967295).
        if (str.empty() || !std::isdigit(static_cast<unsigned char>(str[0])))
            return false;
        char* end = nullptr;
        errno = 0;
        unsigned long val = std::strtoul(str.c_str(), &end, 10);
        if (errno != 0 || end == str.c_str() || *end != '\0' || val > std::numeric_limits<uint32>::max())
            return false;
        outVal = static_cast<uint32>(val);
        return true;
    }

    bool Manager::TryParseUInt64(std::string const& str, uint64& outVal)
    {
        if (str.empty() || !std::isdigit(static_cast<unsigned char>(str[0])))
            return false;
        char* end = nullptr;
        errno = 0;
        unsigned long long val = std::strtoull(str.c_str(), &end, 10);
        if (errno != 0 || end == str.c_str() || *end != '\0')
            return false;
        outVal = static_cast<uint64>(val);
        return true;
    }

    bool Manager::QueueZoneMutation(std::string const& sql)
    {
        if (_pendingZoneWrites >= 32)
        {
            LOG_WARN("module", "[PvPLife] Zone write queue is full; GM edit was not submitted.");
            return false;
        }
        WorldDatabaseTransaction transaction = WorldDatabase.BeginTransaction();
        transaction->Append(sql);
        ++_pendingZoneWrites;
        AuditLog("ZONE_EDIT", sql);
        _transactionProcessor.AddCallback(WorldDatabase.AsyncCommitTransaction(transaction)).AfterComplete(
            [this](bool success)
            {
                --_pendingZoneWrites;
                if (!success)
                    LOG_ERROR("module", "[PvPLife] Zone edit transaction failed; cached zones remain unchanged.");
                // Refresh once no other edit is pending, even if a reload ran meanwhile: that
                // reload's own snapshot may have been read before this write committed.
                if (_pendingZoneWrites == 0 && _databaseReady)
                    LoadZoneConfig();
            });
        return true;
    }

    bool Manager::HandleZoneCommand(ChatHandler* handler, std::vector<std::string> const& args)
    {
        if (args.size() < 2)
        {
            PrintHelp(handler);
            return true;
        }
        std::string action = Lower(args[1]);
        if (action == "list")
        {
            // From the cache the scheduler uses, not a blocking query on the world thread.
            if (_zoneCache.empty())
            {
                handler->PSendSysMessage("PvP Life: No configured zones found.");
                return true;
            }
            std::vector<Zone> zones = _zoneCache;
            std::sort(zones.begin(), zones.end(), [](Zone const& a, Zone const& b)
            {
                return a.Type != b.Type ? a.Type < b.Type : a.Name < b.Name;
            });
            for (Zone& z : zones)
            {
                ApplyZoneConfig(z);
                handler->PSendSysMessage("#{} {} [{}] enabled={} {}->{} level={}~{} pop={}~{} ratio={}~{} / {}~{} cooldown={}s challenge={} chat={}",
                    z.Id, z.Name, TypeName(z.Type), z.Enabled ? 1 : 0, TeamName(z.AttackerTeam), TeamName(z.DefenderTeam),
                    z.MinLevel, z.MaxLevel, z.PopulationMin, z.PopulationMax,
                    z.AttackersMin, z.AttackersMax, z.DefendersMin, z.DefendersMax,
                    z.CooldownSeconds, z.ChallengePlayers ? 1 : 0, z.BotChat ? 1 : 0);
            }
            if (_pendingZoneWrites)
                handler->PSendSysMessage("({} zone edit(s) still being written; the list refreshes once they commit.)", _pendingZoneWrites);
            return true;
        }
        if (action == "create")
        {
            if (args.size() < 8)
            {
                handler->PSendSysMessage("Syntax: .pvplife zone create <name> <type> <attacker> <defender> <minLvl> <maxLvl>");
                return true;
            }
            Player* gm = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
            if (!gm)
            {
                handler->PSendSysMessage("This command requires an in-game GM position.");
                return true;
            }
            if (!std::isfinite(gm->GetPositionX()) || !std::isfinite(gm->GetPositionY()) ||
                !std::isfinite(gm->GetPositionZ()) || !std::isfinite(gm->GetOrientation()))
            {
                handler->PSendSysMessage("Invalid GM position coordinates.");
                return true;
            }
            ActivityType type;
            TeamSide attacker, defender;
            if (!ParseType(args[3], type) || !ParseTeam(args[4], attacker) || !ParseTeam(args[5], defender))
            {
                handler->PSendSysMessage("Invalid activity type or faction value.");
                return true;
            }
            // Duel pairs are drawn from one faction (PvPLifeDuelSelection.h).
            if (type == ActivityType::Duel && (attacker != defender || attacker == TeamSide::Any))
            {
                handler->PSendSysMessage("A duel zone needs the same faction (alliance or horde) on both sides.");
                return true;
            }
            if (args[2].size() > 64)
            {
                handler->PSendSysMessage("Zone names are at most 64 characters.");
                return true;
            }
            Zone existing;
            if (LoadZoneByName(args[2], existing))
            {
                handler->PSendSysMessage("PvP Life: Zone {} already exists; change it with rally, target, counts and the other zone commands.", existing.Name);
                return true;
            }
            std::string name = args[2];
            WorldDatabase.EscapeString(name);
            uint32 minLevelRaw = 0, maxLevelRaw = 0;
            if (!TryParseUInt32(args[6], minLevelRaw) || !TryParseUInt32(args[7], maxLevelRaw))
            {
                handler->PSendSysMessage("Invalid level numbers.");
                return true;
            }
            uint32 minLevel = std::clamp<uint32>(minLevelRaw, 1, 80);
            uint32 maxLevel = std::clamp<uint32>(maxLevelRaw, minLevel, 80);
            // Created disabled: rally and target both start at the GM's position, so the zone must
            // not be scheduled before they are set.
            bool queued = QueueZoneMutation(Acore::StringFormat(
                "INSERT INTO pvp_life_zone (name,enabled,activity_type,attacker_team,defender_team,min_level,max_level,map_id,"
                "rally_x,rally_y,rally_z,rally_o,target_x,target_y,target_z,target_o) VALUES ('{}',0,{},{},{},{},{},{},{},{},{},{},{},{},{},{})",
                name, static_cast<uint32>(type), static_cast<uint32>(attacker), static_cast<uint32>(defender), minLevel, maxLevel,
                gm->GetMapId(), gm->GetPositionX(), gm->GetPositionY(), gm->GetPositionZ(), gm->GetOrientation(),
                gm->GetPositionX(), gm->GetPositionY(), gm->GetPositionZ(), gm->GetOrientation()));
            if (queued)
                handler->PSendSysMessage("PvP Life: Queued creation of {} (levels {}-{}, disabled). Set rally and target once it shows in zone list, then enable it.",
                    args[2], minLevel, maxLevel);
            else
                handler->PSendSysMessage("PvP Life: Failed to queue creation of {}: write queue is full.", args[2]);
            return true;
        }

        bool known = action == "rally" || action == "target" || action == "counts" || action == "duration" ||
            action == "weight" || action == "cooldown" || action == "challenge" || action == "chat" || action == "enable";
        if (!known || args.size() < 3)
        {
            PrintHelp(handler);
            return true;
        }
        Zone zone;
        if (!LoadZoneByName(args[2], zone))
        {
            handler->PSendSysMessage("PvP Life: Zone '{}' not found (a zone just created shows up once its write commits).", args[2]);
            return true;
        }
        std::string name = zone.Name;
        WorldDatabase.EscapeString(name);
        // A conf override (PvPLife.Zone.<name>.*) wins over the database, so say so.
        auto warnOverride = [&](char const* what)
        {
            if (!sConfigMgr->GetKeysByString("PvPLife.Zone." + zone.Name + ".").empty())
                handler->PSendSysMessage("Note: PvPLife.Zone.{}.* in the conf overrides the stored {} until it is removed.", zone.Name, what);
        };
        auto report = [&](bool queued)
        {
            if (queued)
                handler->PSendSysMessage("PvP Life: Queued {} update for {}.", action, zone.Name);
            else
                handler->PSendSysMessage("PvP Life: Failed to queue {} update for {}: write queue is full.", action, zone.Name);
        };

        if (action == "rally" || action == "target")
        {
            Player* gm = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
            if (!gm)
            {
                handler->PSendSysMessage("This command requires an in-game GM position.");
                return true;
            }
            if (!std::isfinite(gm->GetPositionX()) || !std::isfinite(gm->GetPositionY()) ||
                !std::isfinite(gm->GetPositionZ()) || !std::isfinite(gm->GetOrientation()))
            {
                handler->PSendSysMessage("Invalid GM position coordinates.");
                return true;
            }
            if (action == "rally")
                report(QueueZoneMutation(Acore::StringFormat("UPDATE pvp_life_zone SET map_id={},rally_x={},rally_y={},rally_z={},rally_o={} WHERE name='{}'",
                    gm->GetMapId(), gm->GetPositionX(), gm->GetPositionY(), gm->GetPositionZ(), gm->GetOrientation(), name)));
            else
                report(QueueZoneMutation(Acore::StringFormat("UPDATE pvp_life_zone SET map_id={},target_x={},target_y={},target_z={},target_o={} WHERE name='{}'",
                    gm->GetMapId(), gm->GetPositionX(), gm->GetPositionY(), gm->GetPositionZ(), gm->GetOrientation(), name)));
            return true;
        }
        if (action == "counts")
        {
            uint32 attMin = 0, attMax = 0, defMin = 0, defMax = 0;
            if (args.size() < 7 || !TryParseUInt32(args[3], attMin) || !TryParseUInt32(args[4], attMax) ||
                !TryParseUInt32(args[5], defMin) || !TryParseUInt32(args[6], defMax) ||
                attMax > 65535 || defMax > 65535 || attMin > attMax || defMin > defMax)
            {
                handler->PSendSysMessage("Counts are 0-65535 with min <= max: zone counts <name> <attMin> <attMax> <defMin> <defMax>");
                return true;
            }
            report(QueueZoneMutation(Acore::StringFormat("UPDATE pvp_life_zone SET attackers_min={},attackers_max={},defenders_min={},defenders_max={} WHERE name='{}'",
                attMin, attMax, defMin, defMax, name)));
            warnOverride("population");
            return true;
        }
        if (action == "duration")
        {
            uint32 durMin = 0, durMax = 0;
            if (args.size() < 5 || !TryParseUInt32(args[3], durMin) || !TryParseUInt32(args[4], durMax) ||
                durMin < 1 || durMax > 65535 || durMin > durMax)
            {
                handler->PSendSysMessage("Durations are 1-65535 minutes with min <= max: zone duration <name> <minM> <maxM>");
                return true;
            }
            report(QueueZoneMutation(Acore::StringFormat("UPDATE pvp_life_zone SET duration_min={},duration_max={} WHERE name='{}'",
                durMin, durMax, name)));
            return true;
        }

        uint32 value = 0;
        if (args.size() < 4 || !TryParseUInt32(args[3], value))
        {
            handler->PSendSysMessage("Invalid value parameter.");
            return true;
        }
        bool flag = action == "challenge" || action == "chat" || action == "enable";
        if (flag && value > 1)
        {
            handler->PSendSysMessage("Use 0 or 1.");
            return true;
        }
        if (action == "weight")
        {
            report(QueueZoneMutation(Acore::StringFormat("UPDATE pvp_life_zone SET weight={} WHERE name='{}'", value, name)));
            if (!value)
                handler->PSendSysMessage("Note: weight 0 means the zone is never picked automatically.");
        }
        else if (action == "cooldown")
            report(QueueZoneMutation(Acore::StringFormat("UPDATE pvp_life_zone SET cooldown_seconds={} WHERE name='{}'", value, name)));
        else if (action == "challenge")
            report(QueueZoneMutation(Acore::StringFormat("UPDATE pvp_life_zone SET challenge_players={} WHERE name='{}'", value, name)));
        else if (action == "chat")
            report(QueueZoneMutation(Acore::StringFormat("UPDATE pvp_life_zone SET bot_chat={} WHERE name='{}'", value, name)));
        else
        {
            report(QueueZoneMutation(Acore::StringFormat("UPDATE pvp_life_zone SET enabled={} WHERE name='{}'", value, name)));
            warnOverride("enabled flag");
        }
        return true;
    }

    bool Manager::HandleSpotCommand(ChatHandler* handler, std::vector<std::string> const& args)
    {
        if (args.size() < 2)
        {
            PrintHelp(handler, true);
            return true;
        }
        std::string action = Lower(args[1]);
        if (action == "list")
        {
            std::vector<std::string> forwardArgs = { "zone", "list" };
            return HandleZoneCommand(handler, forwardArgs);
        }
        if (action == "create")
        {
            if (args.size() < 7)
            {
                handler->PSendSysMessage("Syntax: .wpvp spot create <name> <attacker> <defender> <minLvl> <maxLvl>");
                return true;
            }
            TeamSide attacker, defender;
            if (!ParseTeam(args[3], attacker) || !ParseTeam(args[4], defender))
            {
                handler->PSendSysMessage("Bad team. Use alliance, horde or any.");
                return true;
            }
            ActivityType type = (attacker != TeamSide::Any && attacker == defender) ? ActivityType::Duel : ActivityType::Skirmish;
            std::string typeStr = (type == ActivityType::Duel) ? "duel" : "skirmish";
            std::vector<std::string> forwardArgs = { "zone", "create", args[2], typeStr, args[3], args[4], args[5], args[6] };
            return HandleZoneCommand(handler, forwardArgs);
        }
        if (action == "rally" || action == "target" || action == "counts" || action == "duration" ||
            action == "weight" || action == "cooldown" || action == "challenge" || action == "chat" || action == "enable")
        {
            std::vector<std::string> forwardArgs = args;
            forwardArgs[0] = "zone";
            return HandleZoneCommand(handler, forwardArgs);
        }
        PrintHelp(handler, true);
        return true;
    }

    bool Manager::HandleResponseCommand(ChatHandler* handler, std::vector<std::string> const& args)
    {
        if (args.size() < 2)
        {
            handler->PSendSysMessage("Syntax: .pvplife response status | stop <id|all>");
            return true;
        }
        std::string sub = Lower(args[1]);
        if (sub == "status")
        {
            handler->PSendSysMessage("=== Reactive Raid Responses ===");
            handler->PSendSysMessage("Pending: {}", _pendingResponses.size());
            uint32 now = GameTimeSeconds();
            for (PendingRaidResponse const& p : _pendingResponses)
            {
                uint32 left = p.StartAt > now ? p.StartAt - now : 0;
                handler->PSendSysMessage("#{} team={} triggerGuid={} responders={} startsIn={}s",
                    p.ResponseId, TeamName(p.ResponseTeam), p.TriggerGuidLow, p.Bots.size(), left);
            }
            return true;
        }
        if (sub == "stop" && args.size() >= 3)
        {
            if (Lower(args[2]) == "all")
            {
                size_t pending = _pendingResponses.size();
                CancelPendingRaidResponse(0, "gm-stop-all");
                handler->PSendSysMessage("PvP Life: Stopped {} pending raid response(s).", pending);
                return true;
            }
            uint64 id = 0;
            if (!TryParseUInt64(args[2], id) || id == 0)
            {
                handler->PSendSysMessage("Invalid response id.");
                return true;
            }
            if (CancelPendingRaidResponse(id, "gm-stop"))
                handler->PSendSysMessage("PvP Life: Stopped pending response #{}.", id);
            else
                handler->PSendSysMessage("PvP Life: No pending response #{}.", id);
            return true;
        }
        handler->PSendSysMessage("Syntax: .pvplife response status | stop <id|all>");
        return true;
    }

    bool Manager::HandleDebugCommand(ChatHandler* handler, std::vector<std::string> const& args)
    {
        if (args.size() < 2)
        {
            PrintDebugStatus(handler);
            return true;
        }
        std::string sub = Lower(args[1]);
        if (sub == "status" || sub == "counters")
        {
            PrintDebugStatus(handler);
            return true;
        }
        if (sub == "validate")
        {
            bool db = VerifyDatabase();
            handler->PSendSysMessage("Database Validation: {}", db ? "OK" : "ERROR");
            PrintReservations(handler);
            return true;
        }
        if (sub == "bot" && args.size() >= 3)
        {
            uint32 guidLow = 0;
            if (!TryParseUInt32(args[2], guidLow))
            {
                Player* p = ObjectAccessor::FindPlayerByName(args[2]);
                if (p)
                    guidLow = p->GetGUID().GetCounter();
            }

            if (!guidLow)
            {
                handler->PSendSysMessage("Bot '{}' not found.", args[2]);
                return true;
            }

            Player* p = FindPlayer(guidLow);
            bool isPart = IsParticipant(guidLow);
            bool isRes = PlayerbotsLife::IsReserved(guidLow);
            handler->PSendSysMessage("Bot guid={}: online={} participant={} reserved={}",
                guidLow, p ? "YES" : "NO", isPart ? "YES" : "NO", isRes ? "YES" : "NO");
            return true;
        }
        if (sub == "antic" && args.size() >= 4)
        {
            // Plays an antic on an idle duelist right away, to see what it looks like.
            Player* p = ObjectAccessor::FindPlayerByName(args[2]);
            LifeMotion::Antic kind = LifeMotion::AnticFromName(args[3]);
            if (!p || !FindDuelGroundParticipant(p->GetGUID().GetCounter()))
                handler->PSendSysMessage("'{}' is not a bot on a duel ground right now.", args[2]);
            else if (kind == LifeMotion::Antic::None || kind == LifeMotion::Antic::HopDismount)
                handler->PSendSysMessage("Antics: Dance, HopBackForth, HopSideToSide, BunnyHop.");
            else if (p->duel || p->IsInCombat() || !TryStartAntic(p, p->GetGUID().GetCounter(), kind))
                handler->PSendSysMessage("{} is busy (dueling or fighting).", p->GetName());
            else
                handler->PSendSysMessage("{} starts {}.", p->GetName(), LifeMotion::AnticName(kind));
            return true;
        }
        PrintDebugStatus(handler);
        return true;
    }

    void Manager::StopEventById(uint64 id, ChatHandler* handler)
    {
        if (id == 0)
        {
            CancelPendingRaidResponse(0, "gm-stop");
            for (ActiveEvent& e : _activeEvents)
                EndEvent(e, "gm-stop");
            _activeEvents.clear();
            _moveQueue.clear();
            EraseDuelSteps([](DuelStep const&) { return true; });
            _engagementQueue.clear();
            _chatQueue.clear();
            if (handler)
                handler->PSendSysMessage("PvP Life: Stopped all activities.");
            return;
        }

        auto event = std::find_if(_activeEvents.begin(), _activeEvents.end(),
            [id](ActiveEvent const& current) { return current.EventId == id; });
        if (event == _activeEvents.end())
        {
            if (handler)
                handler->PSendSysMessage("PvP Life: did not find #{}.", id);
            return;
        }
        if (event->Closing)
        {
            if (handler)
                handler->PSendSysMessage("PvP Life: #{} is already ending.", id);
            return;
        }
        bool duelZone = event->EventZone.Type == ActivityType::Duel && event->Origin == EventOrigin::Zone;
        std::string zoneName = event->EventZone.Name;
        EndEvent(*event, "gm-stop"); // also purges the event's queued steps
        _activeEvents.erase(event);
        if (handler)
        {
            handler->PSendSysMessage("PvP Life: stopped #{}.", id);
            if (duelZone && _duelEnable && _duelAlwaysActive)
                handler->PSendSysMessage("Duel zones restart on their own while PvPLife.Duel.AlwaysActive = 1; "
                    ".pvplife zone enable {} 0 keeps this one off.", zoneName);
        }
    }

    bool Manager::HandleCommand(ChatHandler* handler, std::string const& text)
    {
        std::vector<std::string> args = Tokenize(text);
        if (args.empty() || Lower(args[0]) == "help")
        {
            PrintHelp(handler);
            return true;
        }
        std::string cmd = Lower(args[0]);
        if (cmd == "status")
        {
            PrintStatus(handler);
            return true;
        }
        if (cmd == "scan")
        {
            uint32 minLevelVal = _globalMinLevel;
            uint32 maxLevelVal = 80;
            if (args.size() >= 2 && !TryParseUInt32(args[1], minLevelVal))
                minLevelVal = _globalMinLevel;
            if (args.size() >= 3 && !TryParseUInt32(args[2], maxLevelVal))
                maxLevelVal = 80;
            uint8 minLevel = static_cast<uint8>(std::clamp<uint32>(minLevelVal, 1, 80));
            uint8 maxLevel = static_cast<uint8>(std::clamp<uint32>(maxLevelVal, minLevel, 80));
            PrintScan(handler, minLevel, maxLevel);
            return true;
        }
        if (cmd == "compat")
        {
            PrintCompat(handler);
            return true;
        }
        if (cmd == "reservations")
        {
            PrintReservations(handler);
            return true;
        }
        if (cmd == "reload")
        {
            LoadConfig();
            handler->PSendSysMessage("PvP Life: Re-applied the loaded configuration; zones and chat lines are being re-read. "
                "(.reload config re-reads the conf file itself.)");
            return true;
        }
        if (cmd == "response")
            return HandleResponseCommand(handler, args);
        if (cmd == "debug")
            return HandleDebugCommand(handler, args);
        if (cmd == "zone")
            return HandleZoneCommand(handler, args);
        if (cmd == "start" && args.size() >= 2)
        {
            Zone zone;
            if (!LoadZoneByName(args[1], zone))
            {
                handler->PSendSysMessage("PvP Life: Zone '{}' not found.", args[1]);
                return true;
            }
            StartZone(zone, handler, true); // refuses, with a message, while the module is disabled
            return true;
        }
        if (cmd == "stop" && args.size() >= 2)
        {
            if (Lower(args[1]) == "all")
            {
                StopEventById(0, handler);
                return true;
            }
            uint64 id = 0;
            if (!TryParseUInt64(args[1], id) || id == 0)
            {
                handler->PSendSysMessage("Invalid event id. Use .pvplife stop <id|all>.");
                return true;
            }
            StopEventById(id, handler);
            return true;
        }
        PrintHelp(handler);
        return true;
    }

    bool Manager::HandleWpvpCommand(ChatHandler* handler, std::string const& text)
    {
        // The legacy .wpvp root takes every .pvplife subcommand, plus "spot" for "zone".
        std::vector<std::string> args = Tokenize(text);
        if (args.empty() || Lower(args[0]) == "help")
        {
            PrintHelp(handler, true);
            return true;
        }
        if (Lower(args[0]) == "spot")
            return HandleSpotCommand(handler, args);
        return HandleCommand(handler, text);
    }
}

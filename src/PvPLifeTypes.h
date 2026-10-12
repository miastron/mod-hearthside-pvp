/*
 * PvP Life
 * Copyright (C) 2026 iCore
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PVP_LIFE_TYPES_H
#define PVP_LIFE_TYPES_H

#include "Define.h"
#include "LifeBotReservation.h"
#include <string>
#include <vector>

namespace PvPLife
{
    enum class TeamSide : uint8
    {
        Alliance = 0,
        Horde = 1,
        Any = 2
    };

    enum class ActivityType : uint8
    {
        Skirmish = 0,
        Duel = 1,
        ForTheHorde = 2,
        ForTheAlliance = 3
    };

    enum class ChatChannel : uint8
    {
        Yell = 0,
        World = 1
    };

    enum class BudgetMode : uint8
    {
        Fixed = 0,
        Percentage = 1
    };

    enum class DemandMode : uint8
    {
        Fixed = 0,
        Dynamic = 1,
        Hybrid = 2
    };

    enum class EventOrigin : uint8
    {
        Zone = 0,
        ReactiveRaidResponse = 1
    };

    enum class ResponderLevelMode : uint8
    {
        Raid = 0,
        Fixed = 1
    };

    enum class AccountFilterMode : uint8
    {
        LegacyAny = 0,
        All = 1
    };

    struct Zone
    {
        uint32 Id = 0;
        std::string Name;
        bool Enabled = true;
        ActivityType Type = ActivityType::Skirmish;
        TeamSide AttackerTeam = TeamSide::Horde;
        TeamSide DefenderTeam = TeamSide::Alliance;
        uint8 MinLevel = 1;
        uint8 MaxLevel = 80;
        uint32 MapId = 0;
        uint32 ZoneId = 0;
        float RallyX = 0.0f;
        float RallyY = 0.0f;
        float RallyZ = 0.0f;
        float RallyO = 0.0f;
        float TargetX = 0.0f;
        float TargetY = 0.0f;
        float TargetZ = 0.0f;
        float TargetO = 0.0f;
        uint32 AttackersMin = 3;
        uint32 AttackersMax = 8;
        uint32 DefendersMin = 3;
        uint32 DefendersMax = 8;
        uint32 PopulationMin = 6;
        uint32 PopulationMax = 16;
        uint32 DurationMin = 10;
        uint32 DurationMax = 25;
        uint32 Weight = 100;
        uint32 CooldownSeconds = 900;
        uint32 LastStart = 0;
        bool ChallengePlayers = false;
        bool BotChat = false;
    };

    struct BotCandidate
    {
        uint32 GuidLow = 0;
        uint32 AccountId = 0;
        std::string Name;
        uint8 Level = 1;
        uint8 Race = 0;
        uint8 Class = 0;
        TeamSide Team = TeamSide::Any;
        bool Online = true;
        PlayerbotsLife::LeaseToken Token;
    };

    struct AIStateJournal
    {
        std::vector<std::string> CombatStrategies;
        std::vector<std::string> NonCombatStrategies;
        bool OriginalPvPFlag = false;
        bool HadStayPosition = false;
        float StayX = 0.0f;
        float StayY = 0.0f;
        float StayZ = 0.0f;
        uint32 StayMapId = 0;
        bool Captured = false;
        std::vector<std::string> AppliedCombatStrategies;
        std::vector<std::string> AppliedNonCombatStrategies;
        bool AppliedPvPFlag = false;
    };

    struct Participant
    {
        BotCandidate Bot;
        bool Attacker = false;
        uint32 OriginalMap = 0;
        float OriginalX = 0.0f;
        float OriginalY = 0.0f;
        float OriginalZ = 0.0f;
        float OriginalO = 0.0f;
        float ActivityX = 0.0f;
        float ActivityY = 0.0f;
        float ActivityZ = 0.0f;
        float ActivityO = 0.0f;
        PlayerbotsLife::LeaseToken Token;
        uint32 DuelStagingSlotId = 0;
        uint64 PairId = 0;
        AIStateJournal Journal;
        bool Deployed = false;
        uint64 DuelArbiter = 0;
        uint32 DuelOpponentGuid = 0;
    };

    enum class DuelPairState : uint8
    {
        Approaching = 0,
        ArrivedIdle = 1,
        Challenged  = 2,
        Countdown   = 3,
        Active      = 4,
        Closing     = 5
    };

    struct DuelPair
    {
        uint64 PairId = 0;
        uint64 EventId = 0;
        uint32 ChallengerGuidLow = 0;
        uint32 TargetGuidLow = 0;
        uint32 StagingSlotId = 0;
        DuelPairState State = DuelPairState::Approaching;
        bool Deployed = false;
    };

    struct ActiveEvent
    {
        uint64 EventId = 0;
        Zone EventZone;
        EventOrigin Origin = EventOrigin::Zone;
        uint32 TriggerGuidLow = 0;
        uint32 StartedAt = 0;
        uint32 EndsAt = 0;
        std::vector<Participant> Members;
        std::vector<DuelPair> DuelPairs;
        bool FullyPrepared = false;
        uint32 PrepareDeadline = 0;
        uint32 DurationSeconds = 0;
        uint64 AdmissionRequestId = 0;
        uint32 RequiredAttackers = 0;
        uint32 RequiredDefenders = 0;
        uint32 MinimumTotal = 1;
        bool Closing = false;
    };

    struct MoveStep
    {
        uint64 EventId = 0;
        uint64 Generation = 0;
        PlayerbotsLife::LeaseToken Token;
        uint32 GuidLow = 0;
        std::string Name;
        uint32 ExecuteAt = 0;
        uint32 MapId = 0;
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
        float O = 0.0f;
        bool Teleport = false;
        uint8 RetryCount = 0;
    };

    struct DuelStep
    {
        uint64 EventId = 0;
        uint64 Generation = 0;
        uint64 PairId = 0;
        PlayerbotsLife::LeaseToken ChallengerToken;
        PlayerbotsLife::LeaseToken TargetToken;
        uint32 ChallengerGuidLow = 0;
        uint32 TargetGuidLow = 0;
        std::string ChallengerName;
        std::string TargetName;
        uint32 ExecuteAt = 0;
        uint8 RetryCount = 0;
        bool TargetIsRealPlayer = false;
        bool RequestSent = false;
        uint32 RequestDeadline = 0;
    };

    struct ChatStep
    {
        uint64 EventId = 0;
        uint64 Generation = 0;
        PlayerbotsLife::LeaseToken Token;
        uint32 SpeakerGuidLow = 0;
        uint32 ExecuteAt = 0;
        ChatChannel Channel = ChatChannel::Yell;
        std::string Text;
    };

    enum class ResponseState : uint8
    {
        Reserved       = 0,
        LoginRequested = 1,
        OnlineReady    = 2,
        Promoted       = 3,
        Failed         = 4,
        Cancelled      = 5
    };

    struct PendingRaidResponse
    {
        uint64 ResponseId = 0;
        uint32 TriggerGuidLow = 0;
        TeamSide ResponseTeam = TeamSide::Any;
        uint32 MapId = 0;
        float TargetX = 0.0f;
        float TargetY = 0.0f;
        float TargetZ = 0.0f;
        float TargetO = 0.0f;
        uint32 StartAt = 0;
        uint32 Deadline = 0;
        std::vector<BotCandidate> Bots;
        std::vector<uint32> PendingLoginGuids;
        bool Cancelled = false;
        ResponseState State = ResponseState::Reserved;
        uint64 AdmissionRequestId = 0;
        bool LeasesGranted = false;
    };

    struct EngagementStep
    {
        uint64 EventId = 0;
        uint64 Generation = 0;
        PlayerbotsLife::LeaseToken BotToken;
        uint32 BotGuidLow = 0;
        uint32 TargetGuidLow = 0;
        uint32 ExecuteAt = 0;
        uint8 RetryCount = 0;
        uint8 MaxRetries = 12;
    };
}

#endif

/*
 * Shared bot reservations for Playerbots Life modules
 * Copyright (C) 2026 iCore
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef LIFE_BOT_RESERVATION_H
#define LIFE_BOT_RESERVATION_H

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <utility>

namespace PlayerbotsLife
{
    constexpr std::uint32_t LIFE_BOT_RESERVATION_PROTOCOL_VERSION = 2026101104;

    enum class ReservationOwner : std::uint8_t
    {
        CityLife = 0,
        PvPLife  = 1,
        Artisans = 2
    };

    enum ModuleSystemFlags : std::uint32_t
    {
        MODULE_NONE       = 0x00,
        MODULE_CITY_LIFE  = 0x01,
        MODULE_PVP_LIFE   = 0x02,
        MODULE_ARTISANS   = 0x04
    };

    enum class LeaseState : std::uint8_t
    {
        Provisional = 0,
        Active      = 1,
        Releasing   = 2,
        Released    = 3
    };

    struct LeaseToken
    {
        std::uint32_t GuidLow = 0;
        ReservationOwner Owner = ReservationOwner::CityLife;
        std::uint64_t ActivityId = 0;
        std::uint64_t AssignmentId = 0;
        std::uint64_t Generation = 0;
        LeaseState State = LeaseState::Provisional;
        bool IsLegacy = false;
        std::uint32_t StateTimestampSec = 0;
    };

    struct DuelStagingSlot
    {
        std::uint32_t SlotId = 0;
        std::uint32_t MapId = 0;
        float CenterX = 0.0f;
        float CenterY = 0.0f;
        float CenterZ = 0.0f;
        std::uint64_t ActivityId = 0;
        std::uint64_t PairId = 0;
        ReservationOwner Owner = ReservationOwner::CityLife;
        std::uint32_t ExpiryTimeMs = 0;
    };

    struct CooperationSnapshot
    {
        std::uint32_t TotalReserved = 0;
        std::uint32_t CityLifeCount = 0;
        std::uint32_t PvPLifeCount = 0;
        std::uint32_t ArtisansCount = 0;
        std::uint32_t ProvisionalCount = 0;
        std::uint32_t ActiveCount = 0;
        std::uint32_t ReleasingCount = 0;
        std::uint32_t ActiveDuelSlots = 0;
        std::uint32_t WaitingAdmissions = 0;
        std::uint32_t UnacknowledgedGrants = 0;
        std::uint64_t CurrentGeneration = 0;
        std::uint32_t StaleProvisionalCount = 0;
        std::uint32_t StaleReleasingCount = 0;
        std::uint32_t OldestProvisionalAgeSec = 0;
        std::uint32_t OldestReleasingAgeSec = 0;
    };

    struct AdmissionRequest
    {
        std::uint64_t RequestId{0};
        ReservationOwner Owner{ReservationOwner::CityLife};
        std::uint64_t ActivityId{0};
        std::vector<std::uint32_t> Candidates;
        std::uint32_t MaximumOwned{0};
        std::uint32_t TimestampSec{0};
        std::uint32_t DeadlineSec{0};
    };

    struct AdmissionGrant
    {
        std::uint64_t RequestId{0};
        ReservationOwner Owner{ReservationOwner::CityLife};
        std::vector<LeaseToken> Tokens;
        std::uint32_t TimestampSec{0};
    };

    struct ReservationRegistry
    {
        std::mutex Mutex;
        std::unordered_map<std::uint32_t, ReservationOwner> Owners;
        std::unordered_map<std::uint32_t, LeaseToken> Leases;
        std::unordered_map<std::uint32_t, std::uint32_t> RealPlayerCooldowns;
        std::vector<DuelStagingSlot> ActiveDuelSlots;
        std::atomic<std::uint32_t> ActiveModules{MODULE_NONE};
        std::uint64_t NextGeneration{1};
        std::uint64_t NextAdmissionRequest{1};
        std::uint32_t StagingSlotIdCounter{1};
        std::uint32_t AdmissionRoundCounter{0};
        std::uint32_t LastArbitrationSecond{0};
        ReservationOwner CurrentPriorityOwner{ReservationOwner::CityLife};
        std::vector<AdmissionRequest> PendingAdmissionRequests;
        std::vector<AdmissionGrant> RecentAdmissionGrants;
        std::atomic<bool> TelemetryBridgeRegistered{false};
    };

    inline ReservationRegistry& GetReservationRegistry()
    {
        static ReservationRegistry registry;
        return registry;
    }

    inline void const* GetReservationRegistryAddress()
    {
        return static_cast<void const*>(&GetReservationRegistry());
    }

    constexpr std::uint32_t GetReservationProtocolVersion()
    {
        return LIFE_BOT_RESERVATION_PROTOCOL_VERSION;
    }

    inline void RegisterActiveModule(ModuleSystemFlags flag)
    {
        ReservationRegistry& registry = GetReservationRegistry();
        registry.ActiveModules.fetch_or(static_cast<std::uint32_t>(flag));
    }

    inline void UnregisterActiveModule(ModuleSystemFlags flag)
    {
        ReservationRegistry& registry = GetReservationRegistry();
        registry.ActiveModules.fetch_and(~static_cast<std::uint32_t>(flag));
    }

    inline bool IsModuleActive(ModuleSystemFlags flag)
    {
        ReservationRegistry& registry = GetReservationRegistry();
        return (registry.ActiveModules.load() & static_cast<std::uint32_t>(flag)) != 0;
    }

    inline std::uint32_t GetActiveModules()
    {
        return GetReservationRegistry().ActiveModules.load();
    }

    // -------------------------------------------------------------
    // Tokenized Lease Operations
    // -------------------------------------------------------------

    inline bool TryAcquireLease(LeaseToken& outToken, std::uint32_t guidLow, ReservationOwner owner,
                                std::uint64_t activityId, LeaseState initialState = LeaseState::Active,
                                std::uint64_t assignmentId = 0, bool isLegacy = false, std::uint32_t nowSec = 0)
    {
        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);

        if (registry.Owners.find(guidLow) != registry.Owners.end())
            return false;

        if (nowSec == 0)
            nowSec = static_cast<std::uint32_t>(std::time(nullptr));

        std::uint64_t gen = ++registry.NextGeneration;
        outToken.GuidLow = guidLow;
        outToken.Owner = owner;
        outToken.ActivityId = activityId;
        outToken.AssignmentId = assignmentId;
        outToken.Generation = gen;
        outToken.State = initialState;
        outToken.IsLegacy = isLegacy;
        outToken.StateTimestampSec = nowSec;

        registry.Owners[guidLow] = owner;
        registry.Leases[guidLow] = outToken;
        return true;
    }

    inline bool TryAcquireBatch(std::vector<LeaseToken>& outTokens, std::vector<std::uint32_t> const& guids,
                                ReservationOwner owner, std::uint64_t activityId, std::uint64_t assignmentId = 0,
                                std::uint32_t nowSec = 0)
    {
        if (guids.empty())
        {
            outTokens.clear();
            return true;
        }

        std::unordered_set<std::uint32_t> uniqueGuids(guids.begin(), guids.end());
        if (uniqueGuids.size() != guids.size())
            return false;

        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);

        for (std::uint32_t guidLow : guids)
        {
            if (registry.Owners.find(guidLow) != registry.Owners.end())
                return false;
        }

        if (nowSec == 0)
            nowSec = static_cast<std::uint32_t>(std::time(nullptr));

        outTokens.clear();
        outTokens.reserve(guids.size());
        for (std::uint32_t guidLow : guids)
        {
            std::uint64_t gen = ++registry.NextGeneration;
            LeaseToken token;
            token.GuidLow = guidLow;
            token.Owner = owner;
            token.ActivityId = activityId;
            token.AssignmentId = assignmentId;
            token.Generation = gen;
            token.State = LeaseState::Active;
            token.IsLegacy = false;
            token.StateTimestampSec = nowSec;

            registry.Owners[guidLow] = owner;
            registry.Leases[guidLow] = token;
            outTokens.push_back(token);
        }
        return true;
    }

    inline bool MatchesLease(LeaseToken const& current, LeaseToken const& token)
    {
        return token.GuidLow != 0 && token.Generation != 0 &&
            current.GuidLow == token.GuidLow && current.Owner == token.Owner &&
            current.ActivityId == token.ActivityId && current.AssignmentId == token.AssignmentId &&
            current.Generation == token.Generation;
    }

    inline bool ValidateLeaseState(LeaseToken const& token, LeaseState state)
    {
        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);
        auto it = registry.Leases.find(token.GuidLow);
        return it != registry.Leases.end() && MatchesLease(it->second, token) && it->second.State == state;
    }

    inline bool ValidateLease(LeaseToken const& token)
    {
        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);
        auto it = registry.Leases.find(token.GuidLow);
        return it != registry.Leases.end() && MatchesLease(it->second, token) &&
            (it->second.State == LeaseState::Provisional || it->second.State == LeaseState::Active);
    }

    inline bool ValidateActiveLease(LeaseToken const& token)
    {
        return ValidateLeaseState(token, LeaseState::Active);
    }

    inline bool BeginRelease(LeaseToken const& token, std::uint32_t nowSec = 0)
    {
        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);
        auto it = registry.Leases.find(token.GuidLow);
        if (it == registry.Leases.end() || !MatchesLease(it->second, token))
            return false;
        if (nowSec == 0)
            nowSec = static_cast<std::uint32_t>(std::time(nullptr));
        it->second.State = LeaseState::Releasing;
        it->second.StateTimestampSec = nowSec;
        return true;
    }

    inline bool ReleaseLeaseExact(LeaseToken const& token)
    {
        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);
        auto it = registry.Leases.find(token.GuidLow);
        if (it == registry.Leases.end() || !MatchesLease(it->second, token))
            return false;
        registry.Leases.erase(it);
        registry.Owners.erase(token.GuidLow);
        return true;
    }

    inline void ReleaseLease(LeaseToken const& token)
    {
        ReleaseLeaseExact(token);
    }

    inline bool PromoteLease(LeaseToken& token, std::uint64_t newActivityId = 0, std::uint32_t nowSec = 0)
    {
        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);
        auto it = registry.Leases.find(token.GuidLow);
        if (it == registry.Leases.end() || !MatchesLease(it->second, token) ||
            (it->second.State != LeaseState::Provisional && it->second.State != LeaseState::Active))
            return false;
        if (nowSec == 0)
            nowSec = static_cast<std::uint32_t>(std::time(nullptr));
        it->second.State = LeaseState::Active;
        it->second.StateTimestampSec = nowSec;
        if (newActivityId != 0)
            it->second.ActivityId = newActivityId;
        token = it->second;
        return true;
    }

    // -------------------------------------------------------------
    // Shared Duel Staging Lattice (20yd Spacing)
    // -------------------------------------------------------------

    inline bool ClaimDuelStagingSlot(std::uint32_t mapId, float reqX, float reqY, float reqZ,
                                     float maxRadius, std::uint64_t activityId, ReservationOwner owner,
                                     std::uint32_t nowMs, std::uint32_t durationMs, DuelStagingSlot& outSlot,
                                     std::uint64_t pairId = 0)
    {
        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);

        // Prune expired slots
        auto& slots = registry.ActiveDuelSlots;
        for (auto it = slots.begin(); it != slots.end();)
        {
            if (it->ExpiryTimeMs != 0 && static_cast<std::int32_t>(nowMs - it->ExpiryTimeMs) >= 0)
                it = slots.erase(it);
            else
                ++it;
        }

        // Search lattice candidates around reqX, reqY in 20-yard increments
        constexpr float SlotSpacing = 20.0f;
        constexpr float MinSeparation = 20.0f;
        int maxRings = static_cast<int>(maxRadius / SlotSpacing) + 1;

        for (int ring = 0; ring <= maxRings; ++ring)
        {
            for (int dx = -ring; dx <= ring; ++dx)
            {
                for (int dy = -ring; dy <= ring; ++dy)
                {
                    if (std::abs(dx) != ring && std::abs(dy) != ring)
                        continue;

                    float candX = reqX + static_cast<float>(dx) * SlotSpacing;
                    float candY = reqY + static_cast<float>(dy) * SlotSpacing;
                    float distFromReq = std::sqrt((candX - reqX) * (candX - reqX) + (candY - reqY) * (candY - reqY));
                    if (distFromReq > maxRadius)
                        continue;

                    bool collides = false;
                    for (DuelStagingSlot const& existing : slots)
                    {
                        if (existing.MapId != mapId)
                            continue;
                        float dz = std::abs(existing.CenterZ - reqZ);
                        float d = std::sqrt((existing.CenterX - candX) * (existing.CenterX - candX) +
                                            (existing.CenterY - candY) * (existing.CenterY - candY));
                        if (d < MinSeparation && dz <= 6.0f)
                        {
                            collides = true;
                            break;
                        }
                    }

                    if (!collides)
                    {
                        DuelStagingSlot slot;
                        slot.SlotId = ++registry.StagingSlotIdCounter;
                        slot.MapId = mapId;
                        slot.CenterX = candX;
                        slot.CenterY = candY;
                        slot.CenterZ = reqZ;
                        slot.ActivityId = activityId;
                        slot.PairId = pairId;
                        slot.Owner = owner;
                        slot.ExpiryTimeMs = nowMs + durationMs;

                        slots.push_back(slot);
                        outSlot = slot;
                        return true;
                    }
                }
            }
        }
        return false;
    }

    inline void ReleaseDuelStagingSlot(std::uint32_t slotId)
    {
        if (slotId == 0)
            return;

        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);

        auto& slots = registry.ActiveDuelSlots;
        for (auto it = slots.begin(); it != slots.end();)
        {
            if (it->SlotId == slotId)
                it = slots.erase(it);
            else
                ++it;
        }
    }

    inline void ReleaseDuelStagingSlot(std::uint32_t slotId, ReservationOwner owner)
    {
        if (slotId == 0)
            return;

        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);

        auto& slots = registry.ActiveDuelSlots;
        for (auto it = slots.begin(); it != slots.end();)
        {
            if (it->SlotId == slotId && it->Owner == owner)
                it = slots.erase(it);
            else
                ++it;
        }
    }

    inline void ReleaseDuelStagingSlotByPair(std::uint64_t pairId, ReservationOwner owner)
    {
        if (pairId == 0)
            return;

        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);

        auto& slots = registry.ActiveDuelSlots;
        for (auto it = slots.begin(); it != slots.end();)
        {
            if (it->PairId == pairId && it->Owner == owner)
                it = slots.erase(it);
            else
                ++it;
        }
    }

    inline void ReleaseDuelStagingSlotByActivity(std::uint64_t activityId, ReservationOwner owner)
    {
        if (activityId == 0)
            return;

        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);

        auto& slots = registry.ActiveDuelSlots;
        for (auto it = slots.begin(); it != slots.end();)
        {
            if (it->ActivityId == activityId && it->Owner == owner)
                it = slots.erase(it);
            else
                ++it;
        }
    }

    // -------------------------------------------------------------
    // Shared Real Player Challenge Cooldown Ledger
    // -------------------------------------------------------------

    constexpr std::size_t MAX_REAL_PLAYER_COOLDOWNS = 1024;

    inline void PruneRealPlayerCooldowns(std::uint32_t nowSec = 0)
    {
        if (nowSec == 0)
            nowSec = static_cast<std::uint32_t>(std::time(nullptr));

        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);

        for (auto it = registry.RealPlayerCooldowns.begin(); it != registry.RealPlayerCooldowns.end();)
        {
            if (nowSec >= it->second)
                it = registry.RealPlayerCooldowns.erase(it);
            else
                ++it;
        }
    }

    inline bool CanChallengeRealPlayer(std::uint32_t playerGuidLow, std::uint32_t nowSec, std::uint32_t cooldownSec = 0)
    {
        (void)cooldownSec;
        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);

        auto it = registry.RealPlayerCooldowns.find(playerGuidLow);
        if (it == registry.RealPlayerCooldowns.end() || nowSec >= it->second)
            return true;
        return false;
    }

    inline bool TryClaimRealPlayerChallenge(std::uint32_t playerGuidLow, std::uint32_t nowSec, std::uint32_t cooldownSec)
    {
        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);

        // Bounded pruning of expired cooldowns to prevent unbounded memory growth
        if (registry.RealPlayerCooldowns.size() > 256)
        {
            for (auto it = registry.RealPlayerCooldowns.begin(); it != registry.RealPlayerCooldowns.end();)
            {
                if (nowSec >= it->second)
                    it = registry.RealPlayerCooldowns.erase(it);
                else
                    ++it;
            }
        }

        // Hard cap to prevent memory exhaustion under adversarial / high-churn conditions
        if (registry.RealPlayerCooldowns.size() >= MAX_REAL_PLAYER_COOLDOWNS)
        {
            auto oldestIt = registry.RealPlayerCooldowns.begin();
            for (auto it = registry.RealPlayerCooldowns.begin(); it != registry.RealPlayerCooldowns.end(); ++it)
            {
                if (it->second < oldestIt->second)
                    oldestIt = it;
            }
            if (oldestIt != registry.RealPlayerCooldowns.end() && nowSec >= oldestIt->second)
                registry.RealPlayerCooldowns.erase(oldestIt);
            else
                return false; // Active cooldown protection: reject claim when ledger is saturated with unexpired records
        }

        auto it = registry.RealPlayerCooldowns.find(playerGuidLow);
        if (it != registry.RealPlayerCooldowns.end() && nowSec < it->second)
            return false;

        registry.RealPlayerCooldowns[playerGuidLow] = nowSec + cooldownSec;
        return true;
    }

    inline void ReleaseRealPlayerChallengeClaim(std::uint32_t playerGuidLow, std::uint32_t claimExpirySec)
    {
        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);

        auto it = registry.RealPlayerCooldowns.find(playerGuidLow);
        if (it != registry.RealPlayerCooldowns.end() && it->second == claimExpirySec)
            registry.RealPlayerCooldowns.erase(it);
    }

    inline void RecordRealPlayerChallenge(std::uint32_t playerGuidLow, std::uint32_t nowSec, std::uint32_t cooldownSec)
    {
        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);

        // Bounded pruning of expired cooldowns to prevent memory leaks
        if (registry.RealPlayerCooldowns.size() > 256)
        {
            for (auto it = registry.RealPlayerCooldowns.begin(); it != registry.RealPlayerCooldowns.end();)
            {
                if (nowSec >= it->second)
                    it = registry.RealPlayerCooldowns.erase(it);
                else
                    ++it;
            }
        }
        if (registry.RealPlayerCooldowns.size() >= MAX_REAL_PLAYER_COOLDOWNS)
        {
            auto oldestIt = registry.RealPlayerCooldowns.begin();
            for (auto it = registry.RealPlayerCooldowns.begin(); it != registry.RealPlayerCooldowns.end(); ++it)
            {
                if (it->second < oldestIt->second)
                    oldestIt = it;
            }
            if (oldestIt != registry.RealPlayerCooldowns.end() && nowSec >= oldestIt->second)
                registry.RealPlayerCooldowns.erase(oldestIt);
        }
        registry.RealPlayerCooldowns[playerGuidLow] = nowSec + cooldownSec;
    }

    // -------------------------------------------------------------
    // Fair Admission Round Arbitration
    // -------------------------------------------------------------

    constexpr std::size_t MAX_ADMISSION_REQUESTS = 64;
    constexpr std::uint32_t ADMISSION_GRANT_SECONDS = 5;

    inline std::uint64_t SubmitAdmissionRequest(ReservationOwner owner, std::uint64_t activityId,
        std::vector<std::uint32_t> const& guids, std::uint32_t maximumOwned,
        std::uint32_t nowSec, std::uint32_t deadlineSec)
    {
        if (activityId == 0 || guids.empty() || maximumOwned < guids.size() || deadlineSec <= nowSec)
            return 0;
        std::unordered_set<std::uint32_t> unique(guids.begin(), guids.end());
        if (unique.size() != guids.size() || unique.count(0))
            return 0;
        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);
        if (registry.PendingAdmissionRequests.size() + registry.RecentAdmissionGrants.size() >= MAX_ADMISSION_REQUESTS)
            return 0;
        AdmissionRequest request;
        request.RequestId = ++registry.NextAdmissionRequest;
        request.Owner = owner;
        request.ActivityId = activityId;
        request.Candidates = guids;
        request.MaximumOwned = maximumOwned;
        request.TimestampSec = nowSec;
        request.DeadlineSec = deadlineSec;
        registry.PendingAdmissionRequests.push_back(std::move(request));
        return registry.PendingAdmissionRequests.back().RequestId;
    }

    // Metadata only: no entity, AI, database, or chat access under this mutex.
    inline void ResolveAdmissionRequests(std::uint32_t nowSec)
    {
        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);
        for (auto it = registry.RecentAdmissionGrants.begin(); it != registry.RecentAdmissionGrants.end();)
        {
            if (nowSec - it->TimestampSec < ADMISSION_GRANT_SECONDS)
            {
                ++it;
                continue;
            }
            for (LeaseToken const& token : it->Tokens)
            {
                auto lease = registry.Leases.find(token.GuidLow);
                if (lease != registry.Leases.end() && MatchesLease(lease->second, token) &&
                    lease->second.State == LeaseState::Provisional)
                {
                    registry.Leases.erase(lease);
                    registry.Owners.erase(token.GuidLow);
                }
            }
            it = registry.RecentAdmissionGrants.erase(it);
        }
        auto& requests = registry.PendingAdmissionRequests;
        requests.erase(std::remove_if(requests.begin(), requests.end(),
            [nowSec](AdmissionRequest const& request) { return request.DeadlineSec <= nowSec; }), requests.end());
        if (registry.LastArbitrationSecond == nowSec)
            return;
        registry.LastArbitrationSecond = nowSec;
        ++registry.AdmissionRoundCounter;
        registry.CurrentPriorityOwner = (registry.AdmissionRoundCounter % 2 == 0)
            ? ReservationOwner::CityLife : ReservationOwner::PvPLife;
        ReservationOwner preferred = registry.CurrentPriorityOwner;
        while (!requests.empty())
        {
            auto selected = requests.end();
            for (auto it = requests.begin(); it != requests.end(); ++it)
            {
                // Collect both managers' requests before resolving the next second.
                if (it->TimestampSec >= nowSec)
                    continue;
                std::uint32_t owned = 0;
                for (auto const& entry : registry.Owners)
                    if (entry.second == it->Owner)
                        ++owned;
                if (owned > it->MaximumOwned || it->Candidates.size() > it->MaximumOwned - owned)
                    continue;
                bool free = true;
                for (std::uint32_t guid : it->Candidates)
                    if (registry.Owners.count(guid))
                    {
                        free = false;
                        break;
                    }
                if (!free)
                    continue;
                if (selected == requests.end() || (it->Owner == preferred && selected->Owner != preferred))
                    selected = it;
            }
            if (selected == requests.end())
                break;
            AdmissionGrant grant;
            grant.RequestId = selected->RequestId;
            grant.Owner = selected->Owner;
            grant.TimestampSec = nowSec;
            for (std::uint32_t guid : selected->Candidates)
            {
                LeaseToken token;
                token.GuidLow = guid;
                token.Owner = selected->Owner;
                token.ActivityId = selected->ActivityId;
                token.AssignmentId = ++registry.NextGeneration;
                token.Generation = token.AssignmentId;
                token.State = LeaseState::Provisional;
                token.IsLegacy = false;
                token.StateTimestampSec = nowSec;
                registry.Owners[guid] = token.Owner;
                registry.Leases[guid] = token;
                grant.Tokens.push_back(token);
            }
            preferred = selected->Owner == ReservationOwner::CityLife
                ? ReservationOwner::PvPLife : ReservationOwner::CityLife;
            registry.RecentAdmissionGrants.push_back(std::move(grant));
            requests.erase(selected);
        }
    }

    inline bool TakeAdmissionGrant(std::uint64_t requestId, ReservationOwner owner,
        std::vector<LeaseToken>& tokens)
    {
        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);
        auto& grants = registry.RecentAdmissionGrants;
        auto it = std::find_if(grants.begin(), grants.end(), [requestId, owner](AdmissionGrant const& grant)
        {
            return grant.RequestId == requestId && grant.Owner == owner;
        });
        if (it == grants.end())
            return false;
        tokens = std::move(it->Tokens);
        grants.erase(it); // Acknowledgement; leases remain provisional until explicitly promoted.
        return true;
    }

    inline void CancelAdmissionRequest(std::uint64_t requestId, ReservationOwner owner)
    {
        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);
        auto& requests = registry.PendingAdmissionRequests;
        requests.erase(std::remove_if(requests.begin(), requests.end(), [requestId, owner](AdmissionRequest const& request)
        {
            return request.Owner == owner && (requestId == 0 || request.RequestId == requestId);
        }), requests.end());
        auto& grants = registry.RecentAdmissionGrants;
        for (auto it = grants.begin(); it != grants.end();)
        {
            if (it->Owner != owner || (requestId != 0 && it->RequestId != requestId))
            {
                ++it;
                continue;
            }
            for (LeaseToken const& token : it->Tokens)
            {
                auto lease = registry.Leases.find(token.GuidLow);
                if (lease != registry.Leases.end() && MatchesLease(lease->second, token) &&
                    lease->second.State == LeaseState::Provisional)
                {
                    registry.Leases.erase(lease);
                    registry.Owners.erase(token.GuidLow);
                }
            }
            it = grants.erase(it);
        }
    }

    // -------------------------------------------------------------
    // Truthful Diagnostics Snapshot
    // -------------------------------------------------------------

    inline CooperationSnapshot GetCooperationSnapshot(std::uint32_t nowSec = 0)
    {
        if (nowSec == 0)
            nowSec = static_cast<std::uint32_t>(std::time(nullptr));

        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);

        CooperationSnapshot snap;
        snap.TotalReserved = static_cast<std::uint32_t>(registry.Owners.size());
        snap.ActiveDuelSlots = static_cast<std::uint32_t>(registry.ActiveDuelSlots.size());
        snap.CurrentGeneration = registry.NextGeneration;
        snap.WaitingAdmissions = static_cast<std::uint32_t>(registry.PendingAdmissionRequests.size());
        snap.UnacknowledgedGrants = static_cast<std::uint32_t>(registry.RecentAdmissionGrants.size());

        for (auto const& pair : registry.Owners)
        {
            if (pair.second == ReservationOwner::CityLife)
                ++snap.CityLifeCount;
            else if (pair.second == ReservationOwner::PvPLife)
                ++snap.PvPLifeCount;
            else if (pair.second == ReservationOwner::Artisans)
                ++snap.ArtisansCount;
        }

        constexpr std::uint32_t STALE_PROVISIONAL_THRESHOLD_SEC = 30;
        constexpr std::uint32_t STALE_RELEASING_THRESHOLD_SEC = 60;

        for (auto const& pair : registry.Leases)
        {
            std::uint32_t age = (nowSec >= pair.second.StateTimestampSec && pair.second.StateTimestampSec > 0)
                ? (nowSec - pair.second.StateTimestampSec) : 0;

            if (pair.second.State == LeaseState::Provisional)
            {
                ++snap.ProvisionalCount;
                if (age >= STALE_PROVISIONAL_THRESHOLD_SEC)
                    ++snap.StaleProvisionalCount;
                if (age > snap.OldestProvisionalAgeSec)
                    snap.OldestProvisionalAgeSec = age;
            }
            else if (pair.second.State == LeaseState::Active)
            {
                ++snap.ActiveCount;
            }
            else if (pair.second.State == LeaseState::Releasing)
            {
                ++snap.ReleasingCount;
                if (age >= STALE_RELEASING_THRESHOLD_SEC)
                    ++snap.StaleReleasingCount;
                if (age > snap.OldestReleasingAgeSec)
                    snap.OldestReleasingAgeSec = age;
            }
        }

        return snap;
    }

    // -------------------------------------------------------------
    // Stale Lease Scavenger (Bounded World-Thread Recovery)
    // -------------------------------------------------------------

    inline std::size_t ScavengeStaleLeases(std::uint32_t nowSec = 0,
                                           std::uint32_t provisionalTtlSec = 30,
                                           std::uint32_t releasingTtlSec = 60,
                                           std::vector<LeaseToken>* outScavenged = nullptr)
    {
        if (nowSec == 0)
            nowSec = static_cast<std::uint32_t>(std::time(nullptr));

        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);

        std::size_t scavengedCount = 0;
        for (auto it = registry.Leases.begin(); it != registry.Leases.end();)
        {
            if (it->second.StateTimestampSec == 0)
                it->second.StateTimestampSec = nowSec;

            bool isStaleProvisional = (it->second.State == LeaseState::Provisional &&
                                       nowSec >= it->second.StateTimestampSec + provisionalTtlSec);
            bool isStaleReleasing = (it->second.State == LeaseState::Releasing &&
                                     nowSec >= it->second.StateTimestampSec + releasingTtlSec);

            if (isStaleProvisional || isStaleReleasing)
            {
                if (outScavenged)
                    outScavenged->push_back(it->second);

                registry.Owners.erase(it->first);
                it = registry.Leases.erase(it);
                ++scavengedCount;
            }
            else
            {
                ++it;
            }
        }
        return scavengedCount;
    }

    // -------------------------------------------------------------
    // Legacy Compatibility API (Preserved for existing consumers)
    // -------------------------------------------------------------

    inline bool IsReserved(std::uint32_t guidLow)
    {
        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);
        return registry.Owners.find(guidLow) != registry.Owners.end();
    }

    inline bool IsReservedBy(std::uint32_t guidLow, ReservationOwner owner)
    {
        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);
        auto const entry = registry.Owners.find(guidLow);
        return entry != registry.Owners.end() && entry->second == owner;
    }

    inline bool TryReserve(std::uint32_t guidLow, ReservationOwner owner)
    {
        LeaseToken token;
        return TryAcquireLease(token, guidLow, owner, 0, LeaseState::Active, 0, true);
    }

    inline void Release(std::uint32_t guidLow, ReservationOwner owner)
    {
        ReservationRegistry& registry = GetReservationRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);
        auto const entry = registry.Owners.find(guidLow);
        if (entry != registry.Owners.end() && entry->second == owner)
        {
            auto leaseIt = registry.Leases.find(guidLow);
            // GUID-only legacy release must not delete modern generation-protected non-legacy leases
            if (leaseIt != registry.Leases.end() && !leaseIt->second.IsLegacy)
                return;

            registry.Owners.erase(entry);
            if (leaseIt != registry.Leases.end())
                registry.Leases.erase(leaseIt);
        }
    }
}

#endif

/* PvP Life local extension; GPL-2.0-or-later, matching the module. */
#ifndef PVP_LIFE_DUEL_SELECTION_H
#define PVP_LIFE_DUEL_SELECTION_H
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace PvPLife
{
    // Pools are already shuffled. Prefer underrepresented level bands, but only
    // reserve complete compatible pairs; never change a character's level.
    // `pickGap` draws the level gap each pair aims for: the closest gap is not the
    // goal, since every duel at the exact same level is a tell (realm 2026-10-12:
    // 19 vs 19, 36 vs 36 down the line). `accept(a, b)` vetoes a pair (the role
    // matchup); it is asked about each pair at most once per call.
    template<class Candidate, class GapPicker, class Accept>
    std::pair<std::vector<Candidate>, std::vector<Candidate>> SelectDiverseDuelPairs(
        std::vector<Candidate> const& attackers, std::vector<Candidate> const& defenders,
        std::size_t maxPairs, unsigned maxDifference, GapPicker pickGap, Accept accept)
    {
        std::unordered_set<std::uint64_t> vetoed, accepted;
        auto allowed = [&](Candidate const& a, Candidate const& b)
        {
            std::uint32_t lo = a.GuidLow < b.GuidLow ? a.GuidLow : b.GuidLow;
            std::uint32_t hi = a.GuidLow < b.GuidLow ? b.GuidLow : a.GuidLow;
            std::uint64_t key = (std::uint64_t(lo) << 32) | hi;
            if (vetoed.count(key))
                return false;
            if (accepted.count(key))
                return true;
            bool ok = accept(a, b);
            (ok ? accepted : vetoed).insert(key);
            return ok;
        };
        std::pair<std::vector<Candidate>, std::vector<Candidate>> result;
        std::unordered_set<std::uint32_t> used;
        std::array<unsigned, 3> bands{};
        while (result.first.size() < maxPairs)
        {
            unsigned targetGap = pickGap();
            Candidate const* bestA = nullptr;
            Candidate const* bestB = nullptr;
            unsigned bestBand = 0;
            unsigned bestScore = std::numeric_limits<unsigned>::max();
            for (auto const& a : attackers)
            {
                if (used.count(a.GuidLow))
                    continue;
                for (auto const& b : defenders)
                {
                    if (a.GuidLow == b.GuidLow || a.Team != b.Team || used.count(b.GuidLow))
                        continue;
                    unsigned difference = a.Level > b.Level ? a.Level - b.Level : b.Level - a.Level;
                    if (difference > maxDifference || !allowed(a, b))
                        continue;
                    unsigned level = (unsigned(a.Level) + unsigned(b.Level)) / 2;
                    unsigned band = level < 30 ? 0 : level < 50 ? 1 : 2;
                    unsigned miss = difference > targetGap ? difference - targetGap : targetGap - difference;
                    unsigned score = bands[band] * 256 + miss;
                    if (score < bestScore)
                    {
                        bestA = &a;
                        bestB = &b;
                        bestBand = band;
                        bestScore = score;
                    }
                }
            }
            if (!bestA)
                break;
            used.insert(bestA->GuidLow);
            used.insert(bestB->GuidLow);
            ++bands[bestBand];
            result.first.push_back(*bestA);
            result.second.push_back(*bestB);
        }
        return result;
    }
}
#endif

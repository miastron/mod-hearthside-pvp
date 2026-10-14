/* PvP Life local extension; GPL-2.0-or-later, matching the module. */
#ifndef PVP_LIFE_FOOTWORK_H
#define PVP_LIFE_FOOTWORK_H

#include <cstdint>

// How a 3.3.5a class/spec moves in a 1v1 fight, read by Manager::Footwork.
// Source: docs/duel-tactics-3.3.5a.md (WotLK-era duel and arena guides).
//
// The rule that keeps a caster and a melee from "dancing" with neither one
// acting: a spec whose damage needs cast time (HardCaster) never walks away
// from melee on its own. It first opens a window -- a control spell on the
// opponent, a self-centred root/fear, or an escape -- and only repositions
// while the opponent is rooted, stunned, feared or slowed. Otherwise it
// stands and casts, the way a real caster facing an uncontrolled warrior
// does. Spell names are lowercase, as mod-playerbots' "spell id" lookup takes
// them; a spell the bot does not know (level, talents) is simply skipped.
// Spells cast on a ground location (Shadowfury, Blizzard) do not work through this path and are left out.
namespace PvPLife
{
    struct FootworkSpell
    {
        char const* Name;
        bool SelfCast; // centred on the caster (Frost Nova, Psychic Scream) rather than cast on the opponent
    };

    constexpr uint8_t MaxFootworkSpells = 5;

    struct FootworkProfile
    {
        float MinRange;        // preferred band to fight at, yards (0 = in melee)
        float MaxRange;
        bool HardCaster;       // main damage has cast time: stand still to cast, move only through a window
        bool WantsBehind;      // positional abilities from behind (Backstab, Ambush, Shred)
        FootworkSpell Controls[MaxFootworkSpells]; // open a window when the opponent is too close
        FootworkSpell Escapes[MaxFootworkSpells];  // leave melee outright (Blink faces away first, Disengage faces in)
        FootworkSpell Closers[MaxFootworkSpells];  // close the gap to a target out of reach
        // Cast on itself during the 3-second duel countdown, first castable one, then the next a GCD later:
        // stealth, a shield or HoT, a form, a Demonic Circle. One the bot already wears is skipped.
        FootworkSpell Prep[MaxFootworkSpells];
    };

    // Where a duelist wants to stand from its opponent when the countdown ends: a ranged spec at the
    // near edge of its band, a warrior far enough out to Charge (8-25 yards), anyone else where it is.
    inline float PrepDistance(uint8_t playerClass, FootworkProfile const& profile)
    {
        if (profile.MinRange > 0.0f)
            return profile.MinRange;
        return playerClass == 1 ? 11.0f : 0.0f;
    }

    // Indexed [class][talent tab]; tab order is the client's (e.g. mage 0 Arcane, 1 Fire, 2 Frost).
    inline FootworkProfile const& FootworkProfileFor(uint8_t playerClass, uint8_t tab)
    {
        static FootworkProfile const melee = { 0.0f, 5.0f, false, false, {}, {}, {}, {} };
        static FootworkProfile const table[12][3] =
        {
            {}, // 0 unused
            // 1 Warrior: Arms, Fury, Protection. Charge is Battle Stance and out of combat (Arms' Juggernaut and
            // Protection's Warbringer lift that), Intercept is Berserker Stance (any stance with Warbringer); Intervene
            // targets an ally, so it is no closer.
            {
                { 0.0f, 5.0f, false, false, { { "hamstring", false }, { "piercing howl", true } }, {},
                  { { "charge", false }, { "intercept", false } }, {} },
                { 0.0f, 5.0f, false, false, { { "hamstring", false }, { "piercing howl", true } }, {},
                  { { "charge", false }, { "intercept", false } }, {} },
                { 0.0f, 5.0f, false, false, { { "shockwave", false }, { "concussion blow", false }, { "hamstring", false } }, {},
                  { { "charge", false }, { "intercept", false } }, {} },
            },
            // 2 Paladin: Holy, Protection, Retribution. Holy stays 10-20 yards off (limited mobility, easily
            // interrupted); Repentance is the Retribution gap closer, Avenger's Shield the Protection one.
            {
                { 10.0f, 20.0f, true, false, { { "hammer of justice", false } }, {}, {}, { { "sacred shield", true } } },
                { 0.0f, 5.0f, false, false, { { "hammer of justice", false } }, {}, { { "avenger's shield", false } }, { { "sacred shield", true } } },
                { 0.0f, 5.0f, false, false, { { "hammer of justice", false }, { "repentance", false } }, {}, {}, { { "sacred shield", true } } },
            },
            // 3 Hunter: Beast Mastery, Marksmanship, Survival. Out of the dead zone (here up to ~10 yards:
            // Spell::CheckRange adds melee range to the 5-yard minimum), and a HardCaster: Auto Shot cannot fire
            // while moving in 3.3.5a, so strafing in band costs damage. Inside the dead zone it still opens a
            // window (trap, Wing Clip) or Disengages, then backs out while the target is slowed. 25-35 yards: the guides want 30+, not the maximum, so it can follow a runner.
            {
                { 25.0f, 35.0f, true, false, { { "frost trap", true }, { "intimidation", false }, { "wing clip", false }, { "concussive shot", false } },
                  { { "disengage", true } }, {}, {} },
                { 25.0f, 35.0f, true, false, { { "frost trap", true }, { "scatter shot", false }, { "wing clip", false }, { "concussive shot", false } },
                  { { "disengage", true } }, {}, {} },
                { 25.0f, 35.0f, true, false, { { "frost trap", true }, { "wyvern sting", false }, { "scatter shot", false }, { "wing clip", false }, { "concussive shot", false } },
                  { { "disengage", true } }, {}, {} },
            },
            // 4 Rogue: Assassination, Combat, Subtlety. Backstab/Ambush need the back; Shadowstep lands behind.
            {
                { 0.0f, 5.0f, false, true, { { "kidney shot", false }, { "blind", false }, { "gouge", false } }, {},
                  { { "sprint", true } }, { { "stealth", true } } },
                { 0.0f, 5.0f, false, false, { { "kidney shot", false }, { "blind", false }, { "gouge", false } }, {},
                  { { "sprint", true } }, { { "stealth", true } } },
                { 0.0f, 5.0f, false, true, { { "kidney shot", false }, { "blind", false }, { "gouge", false } }, {},
                  { { "shadowstep", false }, { "sprint", true }, { "deadly throw", false } }, { { "stealth", true } } },
            },
            // 5 Priest: Discipline, Holy, Shadow. 16 yards puts the "too close" line at 8, Psychic Scream's radius.
            {
                { 16.0f, 30.0f, true, false, { { "psychic scream", true } }, {}, {}, { { "power word: shield", true }, { "inner fire", true } } },
                { 16.0f, 30.0f, true, false, { { "psychic scream", true } }, {}, {}, { { "power word: shield", true }, { "renew", true } } },
                { 16.0f, 30.0f, true, false, { { "psychic scream", true }, { "psychic horror", false } }, {}, {}, { { "shadowform", true }, { "power word: shield", true } } },
            },
            // 6 Death Knight: Blood, Frost, Unholy
            {
                { 0.0f, 5.0f, false, false, { { "chains of ice", false } }, {}, { { "death grip", false } }, { { "horn of winter", true } } },
                { 0.0f, 5.0f, false, false, { { "chains of ice", false } }, {}, { { "death grip", false } }, { { "horn of winter", true } } },
                { 0.0f, 5.0f, false, false, { { "chains of ice", false } }, {}, { { "death grip", false } }, { { "bone shield", true }, { "horn of winter", true } } },
            },
            // 7 Shaman: Elemental, Enhancement, Restoration
            {
                { 20.0f, 30.0f, true, false, { { "thunderstorm", true }, { "frost shock", false }, { "earthbind totem", true }, { "hex", false } }, {}, {}, { { "water shield", true } } },
                { 0.0f, 5.0f, false, false, { { "frost shock", false }, { "earthbind totem", true } }, {}, { { "frost shock", false } }, { { "lightning shield", true } } },
                { 20.0f, 30.0f, true, false, { { "frost shock", false }, { "earthbind totem", true }, { "hex", false } }, {}, {}, { { "riptide", true }, { "water shield", true } } },
            },
            // 8 Mage: Arcane, Fire, Frost. Deep Freeze only fires on a frozen target or a Fingers of Frost proc,
            // so it goes first and saves the Nova.
            {
                { 20.0f, 30.0f, true, false, { { "frost nova", true }, { "slow", false }, { "cone of cold", true } }, { { "blink", true } }, {}, {} },
                { 20.0f, 30.0f, true, false, { { "frost nova", true }, { "dragon's breath", true }, { "blast wave", true }, { "cone of cold", true } }, { { "blink", true } }, {}, {} },
                { 20.0f, 30.0f, true, false, { { "deep freeze", false }, { "frost nova", true }, { "cone of cold", true } }, { { "blink", true } }, {}, { { "ice barrier", true } } },
            },
            // 9 Warlock: Affliction, Demonology, Destruction. Shadowfury is cast on the ground, so it is left out.
            // Death Coil is the instant fear, so it leads; Affliction's Improved Howl of Terror makes the Howl worth
            // casting before Fear.
            {
                { 20.0f, 30.0f, true, false, { { "death coil", false }, { "howl of terror", true }, { "fear", false }, { "curse of exhaustion", false } }, {}, {}, { { "demonic circle: summon", true } } },
                { 20.0f, 30.0f, true, false, { { "death coil", false }, { "fear", false }, { "howl of terror", true } }, { { "demonic circle: teleport", true } }, {}, { { "demonic circle: summon", true } } },
                { 20.0f, 30.0f, true, false, { { "death coil", false }, { "fear", false }, { "howl of terror", true } }, {}, {}, { { "demonic circle: summon", true } } },
            },
            {}, // 10 unused
            // 11 Druid: Balance, Feral Combat, Restoration. Shapeshifting sheds roots and snares, but Travel Form and
            // Cat Form are not casting forms, so they are not listed as escapes.
            {
                { 20.0f, 30.0f, true, false, { { "typhoon", true }, { "nature's grasp", true }, { "entangling roots", false } }, {}, {}, { { "moonkin form", true } } },
                { 0.0f, 5.0f, false, true, { { "maim", false } }, {}, { { "feral charge - cat", false }, { "feral charge - bear", false }, { "dash", true } }, { { "cat form", true }, { "prowl", true } } },
                { 20.0f, 30.0f, true, false, { { "nature's grasp", true }, { "entangling roots", false }, { "cyclone", false } }, {}, {}, { { "rejuvenation", true }, { "lifebloom", true } } },
            },
        };
        if (playerClass >= 12 || tab >= 3 || (table[playerClass][tab].MaxRange == 0.0f))
            return melee;
        return table[playerClass][tab];
    }
}

#endif

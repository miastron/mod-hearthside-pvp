# 1v1 Duel Movement and Positioning Tactics, WotLK 3.3.5a

Reference for the bot "footwork" system (`src/PvPLifeFootwork.h`, read by `Manager::Footwork` in `src/PvPLifeMgr.cpp`). Per class and spec: range band, facing, what can be cast on the move, gap-closers/snares/escapes (exact spell names), and the kiting pattern.

## How to read the confidence tags

- **[V]** = checked against a spell data page (wowhead `/wotlk/spell=ID`) or in this repo's AzerothCore source (`azerothcore-wotlk-pb`). Caveat: wowhead's `/wotlk/` pages are the WotLK *Classic* (3.4.x) dataset; for 3.3.5a they agree on every spell checked here, but a few numbers (cooldown, slow %) could differ by a talent tweak.
- **[G: file]** = stated by a downloaded PvP guide; `file` is the guide's short name (see "Sources" for the URL, e.g. `iv_frost-mage` = Icy Veins, `wt_pvp-frost-mage-rotations` = Warcraft Tavern). Both sites write for WotLK Classic (3.4.x) or mix 3.3.5a and Classic text, and the Icy Veins pages are dated Jan-Oct 2023. A [G] item is a practitioner statement, not a data check; where I know the Classic and 3.3.5a versions differ, the item says so.
- **[G!]** = a guide contradicts a [V] item or another guide; both versions are given and the item is on the "Unverified items" list.
- **[M]** = memory / player convention, nothing in the guides or data covers it. Treat as unverified and measure on the realm before hard-coding.

The guides are almost entirely **arena** advice (what to do against a team, with LoS and pillars). They say little about open-field 1v1 spacing beyond "kite", "stay behind", and "keep 30+ yd". Their value here is the per-spec tool list, which spell opens a window, which gap-closer a spec owns, and positional requirements. Specs with no guide in the set (Fury, Blood DK, Holy Priest, Restoration Shaman beyond a short note) keep their old, mostly [M], entries.

---

## 0. General mechanics (all classes)

### Movement and casting in 3.3.5a
- Cast-time spells cannot be cast while moving or jumping; starting to move, jumping, or turning the camera into movement cancels the cast (the "Steady Shot / Scorch / Penance while moving" entries on the wiki's Cast time page are **later-expansion** behaviour; ignore them). [V: wiki states moving/jumping interrupts; later-era exceptions are the only listed exceptions] Cancelling a cast by moving is a standard bait: "fake casting" starts a cast and moves to cancel it so an interrupt is wasted. [G: iv_elemental-shaman, wt_pvp-frost-mage-rotations]
- **Instant** spells castable while moving or mid-jump. [V/G: every guide's "on the move" lists agree] **Channeled** spells (Mind Flay, Drain Life, Arcane Missiles, Penance, Hurricane, Mend Pet) break on movement. [M; the guides call Mind Flay a "filler" cast standing, wt_pvp-shadow-priest-rotations]
- Direct damage taken pushes back cast time by a set amount; channels lose duration. Does not matter for the bot's footwork except that a melee on top of a caster makes casting costly. [V: wiki] Ice Barrier removes the pushback for a mage. [G: iv_frost-mage]
- Melee attacks need facing and range; most targeted spells do **not** need facing the target in 3.3.5a (AzerothCore: only spells flagged `SPELL_FACING_FLAG_INFRONT`, i.e. melee strikes and cones, are checked, `Spell.cpp` `CheckRange`). [V]
- Melee abilities from behind the target cannot be parried or blocked (those need the target to face you); dodge is unaffected. [M; the only guide hint is that Retaliation does not work against attacks from behind, wt_pvp-arms-warrior-rotation]
- Base run speed 7 yd/s, backpedal about 4.5 yd/s. Equal-speed kiting means a melee never closes without a gap-closer, but a caster who *backpedals* loses ground. 50% snare = 3.5 yd/s. [M] Speed tools the guides name: Sprint +70% (rogue), Dash +70% (cat), Travel Form and Ghost Wolf (outdoor run forms), Aspect of the Cheetah/Pack +30% (hunter, dazed when hit), Unholy Presence +15% move speed [G: wt_pvp-unholy-death-knight-rotations; the previous version of this doc found no speed effect in the checked spell data, [G!] unverified], stealth -30% (-15% talented) [G: wt_pvp-subtlety-rogue-rotations].
- **Line of sight** is the guides' main positioning tool at every level: casters and hunters break LoS around pillars to cancel casts, melee use it to close on hunters. Nothing in the bot table can express it (see "Not expressible yet").

### Range and "melee range" (what the server enforces)
- **Melee range** = `max(5.0, myCombatReach + targetCombatReach + 1.333)` yd measured **center to center** (`Unit::GetMeleeRange`, `Unit.cpp:807`). Two 1.5-reach players -> 5.0 yd. [V]
- **Spell max range** is extended by both combat reaches (`IsWithinCombatRange`, about +3 yd between two players) and, when the pre-cast check is non-strict, by 10% (max +3 yd). So a "30 yd" spell can start at about 33 yd. [V]
- **Hunter dead zone**: the wiki (Autoshot page) says Auto Shot min range was 8 yd until patch 2.3.0 (2007) and **5 yd** after, removed only in 5.0.4. [V: warcraft.wiki.gg/wiki/Autoshot] But AzerothCore checks ranged spells as `min_range + GetMeleeRange(target)` center to center (`Spell.cpp:7399-7403`), so with the 5 yd DBC minimum the *effective* dead zone on this server is up to **about 10 yd center to center**, not 5. [V for code; the DBC `RangeMin` for the Hunter range entry was not readable here, so the exact number is **unverified**; measure by watching for `SPELL_FAILED_TOO_CLOSE` at known distances.] Use 8-10 yd as the "safe to shoot" threshold and 5 yd as "must melee/Wing Clip". The guides say "melee will sometimes get into your minimum range... do not try to fight melee in melee range, your main goal is to get away", i.e. they treat the dead zone as something to leave, never stand in. [G: iv_marksmanship-hunter]
- **Hunter max range**: 35 yd base; Hawk Eye adds range (the guides credit it for out-ranging casters; wt_pvp-marksmanship-hunter-rotations quotes Silencing Shot working from 41 yd, i.e. +6). Auto Shot, Arcane/Aimed/Chimera/Steady/Silencing Shot, Concussive Shot all 35 base. [V; Hawk Eye G]
- **Auto Shot cannot fire while moving** in 3.3.5a (changed in 4.0.6); it fires 0.5 s after the hunter stops. Melee swing and Auto Shot share a reset in Wrath. [V: Autoshot wiki page; the swing-reset claim is from a Blizzard dev post surfaced in search, [M]] The guides' practical rule: use instants on the move and "if you have the space, stop for just enough time to get an Auto Shot off". [G: iv_survival-hunter, iv_marksmanship-hunter, iv_beast-mastery-hunter]

### Duel boundary (AzerothCore, matches 3.3.5a behaviour)
- Duel flag is spawned at the **midpoint** between the two duelists (`Spell::EffectDuel`, `SpellEffects.cpp:4320`). [V]
- Countdown to start: **3 s** (`DuelHandler.cpp:42`, `StartTime = now + 3`). [V]
- Out of bounds: when a duelist is **more than 50 yd** from the flag, a **10 s** timer starts and the client gets `SMSG_DUEL_OUTOFBOUNDS`. Coming back **within 40 yd** cancels it (`SMSG_DUEL_INBOUNDS`). If the timer expires the player loses (`DUEL_FLED`). (`Player::CheckDuelDistance`, `Player.cpp:6565-6599`, called every player update.) [V]
- Implication for footwork: keep both duelists within about 40 yd of the flag (hysteresis between 40 and 50). A kite that needs more than 40 yd of open ground should be bent into an arc around the flag. Teleporting to another map loses instantly. Dueling is only allowed in `AREA_FLAG_ALLOW_DUELS` areas. [V]
- Wiki: duels are blocked in most cities and instances, players are in combat for the whole duel, and `/forfeit` / `/yield` end it. [V: warcraft.wiki.gg/wiki/Dueling]

### Player-like flourishes (all classes)
- Jump while casting an **instant** (looks natural, no penalty). Never jump during a cast-time spell, it cancels. [M]
- Strafe-circle a melee target to stay out of its rear arc, or (as melee) step behind. Face the target before a melee ability; casters spin-face a pursuer while backpedalling only for instants. [M]
- "Hunter spin shot": jump, turn 180, fire an instant (Concussive Shot) at the pursuer, turn back before landing. Retail-era technique reported on Blizzard forums; works with any instant. [M]
- Stop-and-cast rule for every caster: only stand still when the target is rooted/stunned/feared/out of range by enough that the cast time fits (cast time + 0.3 s margin at the target's closing speed), or when the target is itself casting. [M; the guides agree in spirit: "only resume Frostbolt when your target is frozen or under Deep Freeze", wt_pvp-frost-mage-rotations; "never sit still to cast Steady Shot", wt_pvp-marksmanship-hunter-rotations]
- Diminishing returns the bot should respect when chaining windows: Polymorph, Gouge, Sap, Repentance, Freezing Trap and Wyvern Sting share one category; Frost Nova shares with Entangling Roots; Fear/Howl of Terror/Psychic Scream each have their own spam limit, Death Coil is not on the fear DR. [G: iv_frost-mage, wt_pvp-affliction-warlock-rotation]

### How `Manager::Footwork` reads the table (read before editing profiles)
- Ranged profile (`MinRange > 0`): "too close" is `MinRange / 2`. Below it, an opponent that is not controlled gets the first castable `Controls` spell, then the first castable `Escapes` spell, else (non-HardCaster only) a step away; a HardCaster just stands and casts. A controlled opponent (rooted, stunned, feared, confused, frozen, **or slowed**) makes the bot walk away to the band's midpoint. Inside the band a non-HardCaster jitters laterally, a HardCaster stands.
- Melee profile (`MinRange == 0`): only `Closers` (8-25 yd) and `WantsBehind` are read, plus the random circling. **`Controls` and `Escapes` on a melee profile are never read** (verified in the function), so those entries are documentation until the function changes.
- `Closers` and `Controls` have no aura gate: a spell with no cooldown placed in `Closers` is recast every 1.5-4 s tick. That is why Chains of Ice (no cooldown, costs a rune) is not a DK closer, and why `Controls` on casters are safe (a slow or root makes the opponent "controlled" and the list is not consulted again).
- Because any slow counts as control, a slow-only spell (Frost Shock, Curse of Exhaustion, Slow, Concussive Shot, Wing Clip) opens the same repositioning window as a root.

---

## 1. Warrior

Everything is instant, so warriors never stand still to cast. Melee range, 5 yd center to center. Charge and Intercept are the gap-closers; Heroic Throw is the ranged tool. Warriors do not need to be behind the target ("beside/behind when free, in front when rooted", [M]).

Tools: **Charge** (8-25 yd, 15 s cd, **Battle Stance**, out of combat only; 1.5 s stun in wt_pvp-arms-warrior-rotation, 2 s in wt_pvp-protection-warrior, +15 rage), **Intercept** (8-25 yd, 30 s cd, **Berserker Stance**, usable in combat, 3 s stun), **Hamstring** (5 yd, -50% run speed, 15 s, Battle or Berserker Stance), **Piercing Howl** (10 yd radius, -50%, spammable), **Heroic Throw** (30 yd, instant, 1 min cd, can be thrown during the Charge animation with no GCD cost), **Shockwave** (Prot, frontal cone, 4 s stun, 20 s cd), **Concussion Blow** (Prot, 5 yd, 5 s stun), **Intimidating Shout** (8 yd radius, up to 5 targets, 8 s fear, breaks if you hit the feared target), **Intervene** (rush to an *ally*, not a closer), **Bladestorm** (Arms, 6 s: can move, immune to CC, only disarm or death stops it). [V for numbers from spell data (Charge/Intercept/Hamstring/Piercing Howl/Heroic Throw/Shockwave); G for stance requirements, stun lengths, Intimidating Shout and Concussion Blow: wt_pvp-arms-warrior-rotation, wt_pvp-protection-warrior, iv_arms-warrior, iv_protection-warrior] **Heroic Fury** (Fury: removes roots/snares, resets Intercept) [M, no Fury guide in the set].

Talents that lift the stance/combat limits: **Juggernaut** (Arms): Charge usable in combat. [G: iv_arms-warrior, wt_pvp-arms-warrior-rotation "greatly aid you when facing foes that abuse LoS"; the realm has not confirmed it on this core] **Warbringer** (Protection): Charge usable in combat and in any stance, Intercept in any stance, and it removes movement-impairing effects. [G: iv_protection-warrior] A root blocks both gap-closers. [M]

- **Arms**: "limited mobility, very susceptible to being kited" [G: iv_arms-warrior]. Opens with Charge -> Heroic Throw -> Hamstring -> Rend -> Mortal Strike -> Piercing Howl; keeps Hamstring and Piercing Howl up on anything that runs; against LoS or a teleporter, Juggernaut Charge then Intercept (stance dance: Berserker for Intercept, back to Battle). [G: wt_pvp-arms-warrior-rotation]
- **Fury**: no guide in the set. Same closers (Charge out of combat, Intercept in Berserker), Piercing Howl and Hamstring; Heroic Fury sheds a root. [M]
- **Protection**: "extremely mobile, very hard to kite or root" with Warbringer [G: iv_protection-warrior]; Shockwave is the signature stun, Concussion Blow the single-target one; Charge and Intercept both stay available in Defensive Stance.

Pattern: Charge from 8-25 yd -> Heroic Throw -> Hamstring -> melee; against a teleporter or a pillar use Intercept (or Juggernaut/Warbringer Charge); do not chase an unsnared same-speed runner without a closer. Hamstring and Piercing Howl are the only snares; a snare does not stop a charge. [M]

Bot table: Arms/Fury `Closers` = charge, intercept (Intervene removed, it targets an ally); Protection `Controls` = shockwave, concussion blow, hamstring (stuns before the slow). `Controls` are not read for melee (see above).

---

## 2. Paladin

No gap-closer in the base kit and no ranged snare, so paladins fight at melee range or Judgement range and rely on stuns; the guides all say "stay on your target as much as possible" and treat mobility as the weakness. [G: iv_retribution-paladin, iv_protection-paladin]

Tools: **Hammer of Justice** (10 yd, 6 s stun, 1 min cd; Improved HoJ cuts it by 20 s) [V; G: wt_pvp-holy-paladin-rotation], **Repentance** (Ret talent, 20 yd, incapacitate, breaks on any damage incl. DoTs; "a great tool to close the gap or set up burst") [V; G: iv_retribution-paladin], **Hand of Freedom** (30 yd, 25 s cd, 6 s immunity to roots/snares, removes existing ones; with Divine Purpose it also breaks stuns but not incapacitates; a Magic effect, dispellable) [V; G: iv_*-paladin, wt_pvp-retribution-paladin-rotation, wt_pvp-holy-paladin-rotation], **Avenger's Shield** (Prot, 30 yd, instant, 30 s cd, dazes) [V; not in the guides], **Judgement of Light/Wisdom/Justice** (10 yd, instant, 10 s cd), **Hammer of Wrath** (30 yd, instant, execute range <= 20%) [V; G: wt_pvp-holy-paladin-rotation], **Exorcism** (30 yd; instant only on an Art of War proc) [G: iv_retribution-paladin], **Holy Shock** (20 yd, instant, damage or heal) [G: wt_pvp-holy-paladin-rotation], **Divine Storm** (Ret, 8 yd radius), **Divine Shield** (clears most debuffs; Mass Dispel removes it) [G: iv_*-paladin]. **Judgement of Justice** has no snare in the checked data [V]; iv_retribution-paladin says it only blunts enemy speed *bonuses* while wt_pvp-retribution-paladin-rotation says "slow your target as much as possible with it" [G!]. Crusader Aura is mounted only. [M]

- **Retribution** (Crusader Strike, Divine Storm, Seal of Righteousness/Vengeance): melee, 5 yd, no positional need. Open with Hammer of Justice -> Avenging Wrath, or burn at melee with Flash of Light weaving. [G: wt_pvp-retribution-paladin-rotation] Closes with Repentance on a runner; Hand of Freedom when rooted or snared.
- **Protection** (Holy Shield, Shield of Righteousness, Avenger's Shield): "stay on your target"; open Avenger's Shield at range (daze while it closes), then melee; stays in front. Avenging Wrath blocks its own defensive cooldowns. [G: iv_protection-paladin]
- **Holy** (Holy Shock, Flash of Light 1.5 s, Holy Light 2.5 s, Judgement): "extremely susceptible to CC and interrupts due to very limited mobility and dependence on casting, so constantly position yourself at a distance from your foes"; "in the backline you can simply kite any melee attackers while healing". [G: iv_holy-paladin, wt_pvp-holy-paladin-rotation] Holy Shock (instant, damage or heal) on cooldown, Hammer of Justice to "escape a dangerous situation or create a kill window", Flash of Light/Holy Light in standing windows.

Pattern: no kiting to speak of for Ret/Prot. When rooted/snared use Hand of Freedom first, then melee. When a melee runs the paladin down, Hammer of Justice and then back-to-back casts. Does not need to be behind anything. [M]

Bot table: Holy is now a ranged profile (10-20 yd, HardCaster, `Controls` = hammer of justice; "too close" is 5 yd); Retribution `Closers` = repentance; Protection `Closers` = avenger's shield. Hand of Freedom is left out: a closer has no "am I snared" check and it would be cast every chase.

---

## 3. Hunter

The most position-sensitive class. Instants (everything below except Auto Shot and Steady Shot) are castable on the move; Auto Shot and Steady Shot need standing still. [V; G: all hunter guides]

Range: the guides want the melee **30+ yd** away "a reasonable distance... well out of melee range, but not max distance, so you can turn around and follow them if they run"; never inside the dead zone (section 0: about 8-10 yd center to center on this server); Wing Clip/Raptor Strike/Mongoose Bite inside 5 yd. [G: iv_marksmanship-hunter, iv_survival-hunter, iv_beast-mastery-hunter] Behind/in front is irrelevant for ranged shots.

Tools: **Concussive Shot** (35 yd, 12 s cd, -50% for 4 s; "keep it as the answer to a gap-closer"), **Wing Clip** (melee 5 yd, -50% for 10 s, no cooldown), **Scatter Shot** (15 yd, 30 s cd, 4 s disorient; Survival-tree talent, taken by Marksmanship too), **Frost Trap** (30 s cd, -50% in the cloud; "always use it on a melee if Wing Clip is not available"), **Freezing Trap** (8 s, 10.4 s with Trap Mastery; placed as an obstacle around a pillar), **Silencing Shot** (MM, 35 yd, 20 s cd, 3 s silence, off the GCD), **Intimidation** (BM, pet stun, 3 s window, 1 min cd; needs a live pet), **Wyvern Sting** (Survival, instant sleep 8 s), **Disengage** (instant, 25 s cd, 16 s with Survival Tactics + Glyph of Disengage; leaps backward), **Deterrence** (5 s, deflects ranged attacks and parries melee but you cannot attack), **Aspect of the Cheetah** (+30% speed, dazed on hit), **Feign Death** (forces enemies to drop target and cancels a cast aimed at you), **Entrapment** (traps root), **Master's Call** (BM: frees you from roots/snares, pet must be alive). [V for data; G: iv_marksmanship-hunter, iv_survival-hunter, iv_beast-mastery-hunter, wt_pvp-marksmanship-hunter-rotations, wt_pvp-survival-hunter-rotations. Master's Call [M], not covered.]

Cast times: **Aimed Shot** is **instant**, 10 s cd [V; G: all three IV guides list it among the shots to fire on the move]; **Steady Shot** needs standing still ("2 seconds" in wt_pvp-marksmanship-hunter-rotations, 1.5 s in the checked spell data [G!]); **Auto Shot** not while moving; **Arcane Shot**, **Chimera Shot** (MM), **Explosive Shot** and **Black Arrow** (SV), **Serpent Sting**, **Multi-Shot**, **Kill Shot** are instants. **Mend Pet** is a channel. [V/G]

- **Beast Mastery**: pet and Bestial Wrath bursts; "glass cannon", "rushing casters while they cannot kite is viable". Kites like the others against melee, uses Intimidation when the melee is on top of you (3 s window to get away), Master's Call for a rooted self. [G: iv_beast-mastery-hunter]
- **Marksmanship**: "never sit still to cast Steady Shot" against melee; Rapid Fire -> Serpent Sting -> Chimera Shot -> Aimed Shot -> Arcane Shot -> Concussive Shot when the Chimera daze expires -> Scatter Shot as soon as the enemy is in medium range -> Silencing Shot on a caster. Against casters: out-range them (Hawk Eye) and use LoS. [G: wt_pvp-marksmanship-hunter-rotations, iv_marksmanship-hunter]
- **Survival**: the sturdiest hunter ("far easier against melee"), most instants (Black Arrow, Explosive Shot, Serpent Sting, Aimed Shot). Wing Clip when they reach you, Deterrence plus Frost/Freezing Trap, Disengage, Scatter Shot, Wyvern Sting; "you cannot use LoS against a warrior or DK, their gap-closers stick to you". [G: wt_pvp-survival-hunter-rotations]

Pattern (all specs): open at 30+ yd; Concussive Shot on cooldown; when a melee gets within about 10 yd: **Frost Trap** at your feet (guides: first answer), then the instant CC (Scatter Shot / Intimidation / Wyvern Sting), Wing Clip in melee, **Disengage** to reset to range, then Concussive Shot and back-pedal 5-10 yd while firing instants. Deterrence when the melee starts to burst; Feign Death to drop a caster's cast. Do not trade in melee unless the target is nearly dead. Stand still only for Auto Shot when the target is snared/trapped/stunned or more than about 20 yd away. Never Auto Shot-walk. [G; the 8-10 yd trigger and 2 s trap arming time [M]]

Bot table: all three tabs are HardCaster (Auto Shot cannot fire while moving in 3.3.5a, so strafing in band costs damage; inside the dead zone the hunter opens a window or Disengages, then backs out while the target is slowed) with band **25-35** yd ("too close" at 12.5 yd, a little outside the dead zone) and `Controls` led by frost trap, then the spec's instant CC (BM intimidation; MM scatter shot; SV wyvern sting, scatter shot), wing clip, concussive shot; `Escapes` = disengage. Freezing Arrow and Explosive/Snake Trap are ground-placed and left out.

---

## 4. Rogue

All abilities are instant (no standing-still constraint). Positional: **Backstab** (dagger) and **Ambush** must be behind the target; **Hemorrhage**, **Mutilate**, **Sinister Strike**, **Eviscerate**, **Cheap Shot**, **Garrote**, **Kidney Shot** do not. [G: wt_pvp-subtlety-rogue-rotations: "Backstab can only be used while behind a target... prioritize Backstab over Hemorrhage"; Hemorrhage "does not require you to be positioned behind"] Melee range 5 yd center to center; **Gouge** requires the target to be facing you [M, not in the guides].

Tools: **Sprint** (+70% run speed, 15 s, 3 min cd; "use it only when your target is LoS-ing you or creating a large gap"), **Shadowstep** (Subtlety, 25 yd, 30 s cd, teleports behind the target, +20% damage on the next ability, castable in stealth, "your only true gap-closer"), **Cheap Shot** (stealth, 4 s stun), **Kidney Shot** (5 yd, 20 s cd, 1 s plus combo points; "prioritize over everything"), **Gouge** (5 s incapacitate, breaks on any damage, a DoT included), **Blind** (10 yd, 10 s disorient; mid-fight options are "Blind -> Gouge"), **Garrote** (stealth, 3 s silence), **Kick** (5 yd, 10 s cd, 5 s interrupt), **Deadly Throw** (30 yd finisher, -50% for 6 s, needs a thrown weapon; "use it when Shadowstep is on cooldown and the target escapes with combo points left"), **Dismantle** (10 s disarm, the answer to melee burst), **Cloak of Shadows**, **Vanish**, **Evasion**, **Shadow Dance** (Sub), **Premeditation**, **Preparation**, **Distract** [V for numbers; G: wt_pvp-subtlety-rogue-rotations, iv_subtlety-rogue]. **Crippling Poison** (off-hand snare) [G: wt_pvp-subtlety-rogue; do not use Deadly Poison in PvP, its DoT breaks Blind and Gouge, wt_pvp-mutilate-rogue]. **Killing Spree** (Combat, teleports between hits), **Blade Flurry**, **Adrenaline Rush**, **Hunger For Blood**, **Cold Blood** [M].

- **Assassination** (Mutilate, Envenom, Cold Blood): Mutilate has no positional requirement [G: wt_pvp-mutilate-rogue "the Mutilate ability itself is your go-to"]; the stealth openers Ambush/Garrote and any Backstab filler still want the back, and behind avoids parry/block [M], so the table keeps `WantsBehind` on. Kidney Shot at 5 combo points to chain stuns.
- **Combat** (Sinister Strike, Blade Flurry, Adrenaline Rush, Killing Spree): no positional requirement, so `WantsBehind` stays off; front or behind; Kick the caster. [M, thin guide wt_pvp-combat-rogue]
- **Subtlety** (Hemorrhage, Backstab, Shadowstep, Shadow Dance, Premeditation): Shadowstep as the mid-fight closer and the back-positioner for Backstab/Ambush; Shadow Dance reopens Cheap Shot/Ambush; Sap -> Premeditation -> Cheap Shot or Garrote (casters/healers) or Ambush (squishy) as the opener; mid-fight Blind -> Gouge to reset into stealth. [G: wt_pvp-subtlety-rogue-rotations]

Pattern: Stealth approach (stealth slows by 30%, 15% talented) -> Shadowstep/Sprint into range -> Cheap Shot -> Kidney Shot -> burn, then Kick every caster cast, Dismantle a melee burst, Cloak of Shadows vs spells, Vanish/Evasion on retreat. When snared Sprint does not break snares per data [V]; chasing a runner: Shadowstep, then Deadly Throw while Shadowstep is down. [G]

Bot table: `Controls` (unread for melee) = kidney shot, blind, gouge on all three tabs; `Closers`: Assassination and Combat sprint; Subtlety shadowstep, sprint, deadly throw. Assassination `WantsBehind` unchanged (see above); Combat/Subtlety unchanged.

---

## 5. Priest

No gap-closer, no ranged snare except **Mind Flay** (3 s channel, 30 yd, -50% run speed [V], cannot move while channelling). Best at range, never in melee; **Psychic Scream** (8 yd radius fear, 8 s, 30 s cd [V]) is "a Priest's only real form of crowd control... to set up a kill or peel for yourself". [G: iv_shadow-priest, iv_discipline-priest] Priests have **no speed boost** in the checked data (the Holy-tree **Body and Soul** speed on Power Word: Shield [unverified]; the wowhead page for the passive showed no speed effect, the guides do not mention it either).

Casts on the move (instants): **Shadow Word: Pain**, **Devouring Plague**, **Shadow Word: Death** (30 yd, 12 s cd, damages the priest if it does not kill), **Power Word: Shield** (also in Shadowform), **Renew**, **Holy Nova** (not castable in Shadowform), **Silence** (Shadow, 30 yd, 45 s cd, 5 s), **Psychic Horror** (30 yd, 2 min cd, 3 s horror plus a 10 s disarm of main hand and ranged), **Dispel Magic**, **Fade** (with Glyph of Fade it removes slows), **Inner Fire**, **Mass Dispel** (Disc: 0.5 s cast). Needs standing: **Mind Blast** (1.5 s, 8 s cd [V]), **Vampiric Touch** (short cast), **Mind Flay** (channel), **Smite**, **Holy Fire**, **Penance** (channel), **Flash Heal/Greater Heal**, **Mind Control**. [V for numbers; G: wt_pvp-shadow-priest-rotations, wt_pvp-discipline-priest-rotation, iv_shadow-priest]

- **Shadow**: **Shadowform** (no Holy spells; Disc spells and Mana Burn still work), **Dispersion** (6 s, -90% damage, castable while stunned/feared/silenced; wt_pvp-shadow-priest-rotations says it also removes movement-impairing effects, the checked spell data did not show that [G!]). Vs melee: dot them (VT, SW:Pain, Devouring Plague), **Psychic Scream** "at the beginning of a fight if dealing with melee enemies", **Shadowfiend** to deter, **Psychic Horror** after the fear; Mind Flay is "your filler at all times in ranged combat". Against a caster: Silence -> Vampiric Touch -> SW:Pain -> Devouring Plague -> Mind Flay -> Scream/Horror -> SW:Death. Strategy: LoS with the DoTs ticking. [G]
- **Discipline**: Power Word: Shield first, Penance, Pain Suppression; Psychic Scream is the peel; fights at range with DoTs and Mana Burn, LoS when pressured. [G: wt_pvp-discipline-priest-rotation, iv_discipline-priest]
- **Holy**: no guide in the set; Holy Fire/Smite need standing, Holy Nova as a melee deterrent. [M]

Pattern: stay in the 16-30 yd band; when a melee closes, **Psychic Scream** (not too early: it is the only fear and has DR), walk away casting instants, stand for Mind Blast/Penance/Smite only during the fear or a Horror/Silence window. Backpedalling while casting instants is fine; backpedalling is slower (about 4.5 yd/s), so turn and run when a melee is on top. [M]

Bot table: all three tabs now use a **16-30** yd band so "too close" is 8 yd, which is Psychic Scream's radius (at the old 20 yd minimum the bot would have fired a 30-second fear from 9-10 yd that hits nothing). `Controls`: Disc/Holy psychic scream; Shadow psychic scream then psychic horror (the 8 s fear is the longer window, Horror is the follow-up when Scream is down).

---

## 6. Death Knight

All instant, no mana. Good gap tool (**Death Grip**) and strong snares. Melee-range 5 yd for strikes.

Tools [V]: **Death Grip** (30 yd, instant, 35 s cd; pulls the target to you; also a secondary interrupt) [G: iv_unholy-death-knight, wt_pvp-unholy-death-knight-rotations], **Chains of Ice** (20 yd, instant, 95% snare, 10 s; re-apply whenever the target regains speed or removes it) [G: wt_pvp-unholy-death-knight-rotations], **Icy Touch** (20 yd, applies Frost Fever; Chilblains adds a snare [M]), **Death Coil** (30 yd, damage, or heals your undead incl. yourself under Lichborne), **Strangulate** (30 yd, 2 min cd, 5 s silence that a trinket cannot remove; talent) [G: wt_pvp-unholy-death-knight-rotations], **Mind Freeze** (5 yd, 10 s cd, 4 s interrupt), **Howling Blast** (Frost, 20 yd, 10 yd radius, 8 s cd), **Hungering Cold** (Frost talent, "a great CC ability" [G: wt_pvp-frost-death-knight], numbers [M]), **Gnaw** (the Unholy ghoul's stun), **Anti-Magic Shell** (immune to magical effects, roots and CC included), **Icebound Fortitude** (stun immunity), **Lichborne** (fear/charm/sleep immunity). **Unholy Presence**: the guide says +15% move and attack speed; the checked 3.3.5-era data showed no movement effect [G!].

- **Blood**: no guide in the set; melee with Heart Strike/Death Strike, Strangulate casters, Death Grip runners. Stay in front. [M]
- **Frost**: Icy Touch/Howling Blast on approach, Chains of Ice to hold range, Hungering Cold to freeze, Frost Strike/Obliterate in melee. [G: wt_pvp-frost-death-knight; tactics [M]]
- **Unholy** (Scourge Strike, Death Strike, Death Coil, Ghoul, Gargoyle): the one DK spec with real matchup guidance: against **warriors, paladins and DKs kite** (Chains of Ice first, diseases, Gargoyle, Unholy Presence speed, back away); against **rogues and ferals stay on top** with Death Grip and Chains of Ice, since ferals shapeshift out of snares and rogues Cloak/Vanish; if bursted early, Icebound Fortitude + Anti-Magic Shell while gaining distance, Death Strike plus Empower Rune Weapon if you cannot; against casters Anti-Magic Shell first, then Strangulate when it ends, Death Grip to close. [G: wt_pvp-unholy-death-knight-rotations]

Pattern: open with **Death Grip** at 20-30 yd, then **Chains of Ice** the instant the target tries to leave, **Strangulate** a caster, **Mind Freeze** an interrupt in melee. A runner at 8+ yd gets Death Grip if off cooldown, else Chains of Ice + chase. Death Coil is the on-the-move ranged filler at 30 yd. Never needs to stand still. [G/M]

Bot table: unchanged (`Closers` = death grip, `Controls` = chains of ice). Chains of Ice is not a closer on purpose: no cooldown, so it would be recast every tick and burn Frost runes. The "kite warriors, stick to rogues" split needs the opponent's class and is not expressible yet.

---

## 7. Shaman

Mixed. Casting classes stand still; Enhancement uses Maelstrom Weapon stacks to cast on the move.

Tools [V]: **Frost Shock** (25 yd, instant, 6 s cd, -50% for 8 s; Enhancement's Frozen Power uses it to close the distance), **Earth Shock** (25 yd, instant, 6 s cd; interrupt), **Wind Shear** (25 yd, instant, 6 s cd, 2 s interrupt, "very spammable"), **Thunderstorm** (Elemental, self, 10 yd, 45 s cd, knockback, +8% mana) [V; G: wt_pvp-elemental-shaman-rotations], **Earthbind Totem** (instant, 15 s cd, 45 s, slows in range; Earthen Power makes it also remove slows from you) [V; G: iv_enhancement-shaman], **Ghost Wolf** (**2 s cast**, +40% run speed, [V]; the guides' escape tool for Elemental and Restoration), **Hex** (cast 1.5 s, 20 yd, 45 s cd; does not heal the target and is "harder to break" than Polymorph) [V; G: wt_pvp-elemental-shaman-rotations], **Lightning Bolt** (2.5 s), **Chain Lightning** (2 s, 6 s cd), **Lava Burst** (2 s, 8 s cd), **Stormstrike**, **Lava Lash**, **Purge**, **Grounding Totem** (eats a Chaos Bolt/CC), **Tremor Totem**, **Frostbrand Weapon** (melee-hit snare, paired with Windfury in PvP) [G: wt_pvp-enhancement-shaman-rotation-cooldowns]. **Maelstrom Weapon** (Enh): at full stacks Lightning Bolt/Chain Lightning/Hex/healing become instant; the stack count is not stated in the guides [G; count unverified].

- **Elemental**: 20-30 yd; instants while moving: Flame Shock, Earth Shock, Frost Shock, Thunderstorm, Wind Shear; stands for Lightning Bolt/Chain Lightning/Lava Burst/Hex. "Keep yourself alive and away from melee with Thunderstorm, Earthbind Totem, Ghost Wolf and Frost Shock", then Lava Burst/Chain Lightning followed by Frost Shock; "a static caster who creates distance through various means". [G: iv_elemental-shaman, wt_pvp-elemental-shaman-rotations]
- **Enhancement**: melee; Stormstrike, Lava Lash, Windfury + Frostbrand, Feral Spirit. "Weakness to being kited": close with **Frost Shock** (Frozen Power) and let **Earthbind Totem** strip slows; Ghost Wolf is not used to chase. Weave Earth/Frost Shock from melee; Maelstrom stacks for Chain Lightning, Hex or a heal while repositioning. Behind not needed. [G: iv_enhancement-shaman, wt_pvp-enhancement-shaman-rotation-cooldowns]
- **Restoration**: "the word of the game is kite, run as fast as you can": Ghost Wolf, Earthbind Totem, Frost Shock, Grounding Totem, Hex setups; stands to heal. [G: wt_pvp-restoration-shaman-rotation-cooldowns, iv_restoration-shaman]

Pattern: shock-snare then walk; drop Earthbind/Grounding at the feet; Wind Shear/Earth Shock a caster; Thunderstorm is an anti-melee reset, not a damage button. [G]

Bot table: Elemental `Controls` = thunderstorm, frost shock, earthbind totem, hex; Enhancement `Closers` = frost shock (cooldown-gated, 6 s); Restoration `Controls` = frost shock, earthbind totem, hex. **Ghost Wolf is not an escape**: it needs a 2 s standing cast and the bot would then be stuck in a non-casting form (see "Not expressible yet").

---

## 8. Mage

Casters stand to cast; mages have the best instants for movement. Ranges below per spell [V]: **Frostbolt** 30 yd (3 s, 40% slow 9 s); **Frostfire Bolt** 40 yd (3 s, slow); **Fireball** 35 yd 3.5 s [M]; **Pyroblast** 35 yd, **5 s** (instant with Hot Streak [G: iv_fire-mage]); **Arcane Blast** 30 yd 2.5 s; **Arcane Barrage** 30 yd **instant** (3 s cd) [G: iv_arcane-mage]; **Ice Lance** 30 yd instant, triple damage on frozen targets [G: wt_pvp-frost-mage-rotations]; **Fire Blast** 20 yd instant, 8 s cd; **Polymorph** 30 yd, 1.5 s, 50 s; **Slow** (Arcane talent) 30 yd instant, -60% move, 15 s; **Counterspell** 30 yd instant, 24 s cd, 8 s lockout.

Instants for movement/escape: **Frost Nova** (self, 10 yd radius, root, 25 s cd, 8 s; damage breaks it) [V], **Blink** (self, 20 yd forward, **15 s cd**; the guide says it escapes both stuns and roots, the checked data showed stun/root immunity during the jump; you must be facing away from the pursuer) [V; G: wt_pvp-frost-mage-rotations, [G!] on whether 3.3.5a Blink frees you from an existing root], **Cone of Cold** (10 yd, -50% for 8 s, 10 s cd; "a fast defensive option vs melee when Frost Nova is down") [V; G], **Dragon's Breath** (Fire, 10 yd cone, 20 s cd, 5 s disorient), **Blast Wave** (Fire, 10 yd, 30 s cd, daze, knockback per iv_fire-mage; the previous version said -50% 6 s) [V/G], **Deep Freeze** (Frost, 30 yd instant, 30 s cd, 5 s stun; **only castable on a frozen target**, i.e. after Frost Nova or the Water Elemental's Freeze, or on a **Fingers of Frost** proc, which saves the Nova) [V; G: iv_frost-mage, wt_pvp-frost-mage-rotations], **Mirror Image** (3 images that Frostbolt-slow the target), **Ice Block**, **Cold Snap**, **Frost Ward**, **Ice Barrier** (also prevents cast pushback), **Presence of Mind**, **Improved Blizzard** + Permafrost (a slow, channelled on the ground, left out). Mage speed items: none; run speed is stock. [V/G]

Fingers of Frost and Brain Freeze mean Frost mages weave instants: Brain Freeze procs an instant Frostfire Bolt, Fingers of Frost two Ice Lances or a Deep Freeze; "you can even proc Brain Freeze from kiting with Ice Lance". [G: iv_frost-mage]

- **Frost**: the canonical kiter ("Kiting is the name of the game... isolate a target"). Against melee: "survive the opener (Death Grip, Charge, Garrote, Pounce); Blink away from the stun or snare; then create distance, Polymorph to reset, and kite with Cone of Cold, Fire Blast, Ice Lance and Arcane Explosion; resume Frostbolt only when the target is frozen or under Deep Freeze." Against casters, snares matter less than hard CC (Counterspell, Polymorph, Deep Freeze): free-cast Frostbolt after the lockout. [G: wt_pvp-frost-mage-rotations]
- **Fire**: Fireball/Pyroblast/Frostfire Bolt need standing; the CC kit is Frost Nova, Blast Wave, Dragon's Breath, Blink; Living Bomb is instant; Scorch/Fire Blast stun with Impact; Hot Streak Pyroblasts are instant. [G: iv_fire-mage; the Scorch "cast while moving" change is later-expansion [V]]
- **Arcane**: Arcane Blast (2.5 s) then Arcane Barrage (instant) as the retreat filler; Arcane Missiles channel breaks on move; **Slow** + **Polymorph** (Presence of Mind makes it instant) buy windows; Frost Nova, Blink, Dampen Magic for defense; burst is Arcane Power + Icy Veins + Presence of Mind, "if you fail to kill in that window you will run out of mana". [G: iv_arcane-mage]

Stop-to-cast rule: Frostbolt needs 3 s, so stand only when the target is rooted (Frost Nova), slowed to under half speed at 20+ yd, or stunned/Polymorphed/blinked-away. Backpedal casting instants only; face the pursuer. Counterspell every cast; "fake casting" a different school baits interrupts. [G]

Bot table: Arcane `Controls` = frost nova, slow, cone of cold; Fire = frost nova, dragon's breath, blast wave, cone of cold; Frost = deep freeze, frost nova, cone of cold (Deep Freeze first: when it is castable the Nova is saved); all three `Escapes` = blink. A Nova'd target is "controlled", so Deep Freeze after a Nova never reaches the list, Fingers of Frost is the only way it fires.

---

## 9. Warlock

Heavy on cast times; relies on DoTs (instants), a pet, and Death Coil/Fear to buy standing windows.

Ranges [V]: **Shadow Bolt** 30 yd, 3 s; **Immolate** 30 yd, 2 s; **Incinerate** 30 yd, 2.5 s; **Chaos Bolt** 30 yd, 2.5 s, 12 s cd; **Unstable Affliction** 30 yd, 1.5 s; **Haunt** 30 yd, 1.5 s, 8 s cd; **Drain Life** channel 5 s; **Fear** 20 yd, **1.5 s**; **Howl of Terror** 10 yd radius, **1.5 s** (Affliction's Improved Howl of Terror shortens it; Destruction has the full cast time [G: wt_pvp-destruction-warlock-rotation]), 40 s cd, 8 s; **Death Coil** 30 yd instant, 3 s horror, **2 min cd**, heals the warlock; the guides call it a "panic button... instant fear that is not on the fear DR" [G: wt_pvp-affliction-warlock-rotation]; **Shadowfury** (Destruction) 30 yd, 8 yd radius, instant, 20 s cd, 3 s stun, **cast on the ground, so left out of the table**; **Curse of Exhaustion** (Affliction) 30 yd instant, -30% for 12 s, 41 yd with its minor glyph [G: iv_affliction-warlock]; **Shadowflame** (cone, with Glyph of Shadowflame it snares; "kite or peel") [G: iv_affliction-warlock, iv_destruction-warlock; cone and glyph-dependent, left out]; **Demonic Circle: Teleport** (40 yd, instant, 30 s cd; needs the Circle placed first; "Fear in conjunction with Demonic Circle: Teleport is a powerful escape", [G: wt_pvp-affliction-warlock-rotation]) [V]; **Metamorphosis** (Demonology, 30 s, 3 min cd); **Felguard Intercept** 8-25 yd, 3 s stun [wowhead page returned the warrior Intercept; felguard pet ability unverified]. Pets: **Felhunter** (Spell Lock, Devour Magic), **Succubus** (Seduction, good vs warriors/DKs), **Voidwalker** (Sacrifice shield) [G: wt_pvp-affliction-warlock-rotation]. **Corruption**, **Curse of Agony**, **Conflagrate**, **Shadowburn**, **Seed of Corruption** and **Siphon Life** are instants / cheap. [V/M]

- **Affliction**: Corruption, Curse of Agony/Elements, Unstable Affliction, Haunt, Drain Life as a filler; "try to always keep someone in a Fear while covered by Unstable Affliction"; **Curse of Exhaustion** and Death Coil/Howl of Terror for pressure; stands for Unstable Affliction/Haunt/Drain Life only in a CC window. [G: iv_affliction-warlock] Range 25-30 yd.
- **Demonology**: demon does the close work (Felguard: Intercept stun, Felhunter: Spell Lock interrupt, Succubus: Seduction); **Metamorphosis** as a melee escape/tank; fights from 20-30 yd, **Demonic Circle: Teleport** to reset distance. [M, no Demonology guide beyond wt_pvp-affliction-warlock-rotation's pet section]
- **Destruction**: Immolate, Conflagrate, Chaos Bolt, Incinerate; "create as many windows to free cast as possible": Shadowfury into Fear on the healer, then damage; Howl of Terror has a cast time without Improved Howl. [G: iv_destruction-warlock, wt_pvp-destruction-warlock-rotation] Stands for Chaos Bolt only during stun/fear.

Pattern: DoTs and curse (instants) while walking; **Death Coil** when a melee is on top, **Howl of Terror** (Affliction) or **Fear** (stand 1.5 s) when no stun is up; apply DoTs before the Fear (the guides keep a feared target dotted, so existing DoTs do not end it; direct damage does [M]); **Demonic Circle: Teleport** (or **Shadowfury**) when cornered. Back-pedalling is not free: Shadow Bolt cannot be cast moving, so cast only inside the fear/stun/teleport windows. Pet stays near the target; warlock keeps 20-30 yd. [G/M]

Bot table: Affliction `Controls` = death coil, howl of terror, fear, curse of exhaustion; Demonology and Destruction = death coil, fear, howl of terror; Demonology `Escapes` = demonic circle: teleport (a no-op until something places the Circle; it never fails badly, `CanCastSpell` just returns false). Destruction's old first entry, shadowfury, is removed (ground-cast).

---

## 10. Druid

Form-dependent: Cat/Bear forms only use physical instants; casting forms are Moonkin, Tree of Life, and caster form. Shapeshifting removes roots and snares (about 1.5 s GCD [M]). [G: wt_pvp-balance-druid-rotation, wt_pvp-feral-druid-rotation, iv_balance-druid]

Tools [V]: **Feral Charge - Bear** (8-25 yd, 15 s cd, 4 s immobilize and interrupt; has a minimum distance, so it cannot be used on a target that is already on you) [G: wt_pvp-feral-druid-rotation], **Feral Charge - Cat** (8-25 yd, 30 s cd, "leaps behind the target", applies **Dazed**), **Dash** (Cat, +70%, 15 s, 3 min cd, does not break Prowl) [G], **Entangling Roots** (30 yd, 1.5 s; PvP duration 3.5 s in wt_pvp-balance-druid-rotation, 9 s in wt_pvp-feral-druid-rotation, 27 s in spell data [G!]), **Cyclone** (20 yd, 1.5 s, 6 s, spammable until DR), **Hibernate** (beasts only, 30 yd, 1.5 s), **Typhoon** (Balance, instant, cone, 20 s cd; knockback plus a 6 s daze; "a great option for distancing yourself from a melee attacker") [G: wt_pvp-balance-druid-rotation, iv_balance-druid], **Nature's Grasp** (instant, 1 min cd, 45 s; roots the melee that hits you, "used to gain distance or LoS") [G], **Maim** (Cat, 5 yd, 10 s cd, finisher stun up to 5 s; Predatory Strikes makes the next spell, e.g. Cyclone, instant) [G: iv_feral-druid], **Pounce** (stealth, 3 s stun), **Bash** (Bear, 5 yd, 4 s stun), **Faerie Fire (Feral)** (30 yd instant, 6 s cd), **Moonfire** (30 yd instant), **Wrath** (30 yd, 2 s), **Starfire** (30 yd, 3.5 s), **Infected Wounds** (Feral talent, the chase snare) [G: iv_feral-druid]. **Travel Form** (outdoors, ghost-wolf-like speed, "mainly used to break LoS and reset the fight"), **Barkskin** (castable stunned [M]), **Insect Swarm**, **Starfall** (stops working while you are CC'd), **Hurricane** (channel) [M/G].

- **Balance**: Moonkin Form. Stand-and-cast: Wrath/Starfire/Entangling Roots/Cyclone (both CC spells are hard casts). On the move: Moonfire, Insect Swarm, Starfall, Typhoon, Nature's Grasp, Barkskin. Pattern: **Moonfire/Insect Swarm** on the run, **Typhoon** (knockback + daze) when caught, **Nature's Grasp**, **Entangling Roots** -> stand and cast Starfire/Wrath; "if enemies turn the pressure on you, kite with Travel Form and Nature's Grasp, or tank in Dire Bear Form". Range 25-30 yd. [G: iv_balance-druid, wt_pvp-balance-druid-rotation]
- **Feral (Cat)**: Prowl -> Pounce -> Faerie Fire (Feral) -> Berserk -> Shred/Mangle -> Rake -> Rip -> Maim -> Ferocious Bite. **Shred needs the back** and the guide says the Shred build must stay behind "for the whole fight, even in 1v1" while the Mangle build does not care; the `WantsBehind` flag cannot tell the builds apart, so it stays on. Chase with **Feral Charge - Cat** (lands behind) then Dash; Cat Form breaks roots and snares on shift. [G: wt_pvp-feral-druid-rotation, iv_feral-druid]
- **Feral (Bear)**: Feral Charge - Bear + Bash chain; tanky, in front is fine; "swap to Dire Bear in anticipation of stun swaps". [G]
- **Restoration**: Tree of Life, Lifebloom/Rejuvenation/Wild Growth/Regrowth (HoTs "heal on the move... enabling safe windows where Cyclone and Entangling Roots can be used"), Nature's Swiftness; Entangling Roots pins "low mobility melee"; weakest damage, stands only in CC windows. [G: iv_restoration-druid]

Pattern (melee form): Feral Charge on a target 8+ yd away; stay behind for Shred; Dash a fleeing target; shift out of a root. Caster forms: Entangling Roots then Starfire; on being caught Typhoon -> Cyclone. [G]

Bot table: Balance `Controls` = typhoon, nature's grasp, entangling roots (the instants before the 1.5 s cast); Restoration = nature's grasp, entangling roots, cyclone; Feral unchanged (`Closers` = feral charge - cat, feral charge - bear, dash). **Travel Form is not an escape** (non-casting form, the bot would not shift back).

---

## Cross-class snapshot for the bot (range bands, yd, center to center)

| Archetype | Preferred band | Must stand to cast | Free while moving |
|-----------|---------------|--------------------|-------------------|
| Warrior, Rogue, DK, Ret/Prot Paladin, Feral Druid, Enh Shaman | melee (<= 5, reach rule) | none (Enh needs Maelstrom stacks for Lightning Bolt; Ghost Wolf 2 s) | all |
| Hunter | 25-35 (guides: 30+, dead zone about 8-10 on this server) | Auto Shot, Steady Shot | Aimed/Arcane/Chimera/Explosive/Black Arrow/Serpent, Concussive, Scatter, Disengage |
| Mage | 20-30 | Frostbolt, Fireball, Pyroblast, Arcane Blast, Polymorph | Ice Lance, Frost Nova, Blink, Cone of Cold, Dragon's Breath, Fire Blast, Arcane Barrage |
| Warlock | 20-30 | Shadow Bolt, Immolate, Incinerate, Chaos Bolt, Fear, Howl of Terror, UA, Haunt, Drain Life | Corruption, CoA, Curse of Exhaustion, Death Coil, Demonic Circle: Teleport (Shadowfury is ground-cast) |
| Priest | 16-30 | Mind Blast, Mind Flay (channel), VT, Smite, Holy Fire, heals | SW:Pain, Devouring Plague, SW:Death, Silence, PW:Shield, Psychic Scream/Horror, Fade |
| Holy Paladin | 10-20 | Flash of Light, Holy Light | Holy Shock, Judgement, Hammer of Justice, Hammer of Wrath |
| Elemental/Resto Shaman | 20-30 | Lightning Bolt, Chain Lightning, Lava Burst, Hex, Healing Wave, Ghost Wolf | Flame/Earth/Frost Shock, Wind Shear, Thunderstorm, Earthbind Totem, Riptide |
| Balance/Resto Druid | 20-30 | Wrath, Starfire, Entangling Roots, Cyclone, Hibernate, Regrowth, Healing Touch | Moonfire, Insect Swarm, Typhoon, Starfall, Nature's Grasp, Lifebloom, Rejuvenation |

Boundary reminder: stay inside 40 yd of the duel flag (timer starts beyond 50 yd, lasts 10 s, resets inside 40 yd).

---

## Not expressible yet

Things the guides describe that the current table/`Footwork` cannot do. Listed rather than coded.

1. **Line-of-sight and pillar play.** The main theme of every caster, hunter and druid guide (casters break LoS to cancel casts and heal, hunters and warriors use Charge/Juggernaut and traps to beat it; "Fire Blast can clip through a wall if cast while the target is in sight before they break LoS"). Needs map geometry (a collision-aware search for an occluder) and a notion of which side of it to stand.
2. **Fake casting** (start a cast, move to cancel it, bait an interrupt; Frost/Elemental/Arcane/Shadow guides). Needs knowledge that the opponent has a ready interrupt.
3. **Class-aware footwork.** Unholy DK kites warriors/paladins/DKs but sticks to rogues/ferals; Frost Mage Polymorphs a melee to reset; Hunter saves Silencing Shot for casters and fires it at once on melee; Warlock picks Succubus vs warriors. `Footwork` does not look at the opponent's class or spec.
4. **Melee `Controls`/`Escapes`.** `Footwork` never reads them for a melee profile, so Hamstring/Piercing Howl/Shockwave, Kidney Shot/Blind/Gouge, Hammer of Justice/Repentance, Maim and Chains of Ice are data only; the combat AI casts them (or not) by itself. Wiring them in would need a gate so a cooldown-less spell is not recast every tick.
5. **Conditional use.** Hand of Freedom / Hand of Protection / Heroic Fury / shapeshifting only when rooted or snared; Chains of Ice only when the slow has dropped; Concussive Shot only "after the gap-closer is used"; Sprint only when the target is LoS-ing or far; Deterrence, Ice Block, Dispersion, Cloak of Shadows and Spell Reflection are defensive cooldowns, not movement. The lists are "first castable", with no state check.
6. **Stance and form dances.** Warrior Battle -> Berserker -> Battle around Charge/Intercept/Hamstring; druid Cat/Bear/Travel; Ghost Wolf (2 s cast, then a non-casting form the bot would have to shift out of). Escapes that change form are therefore omitted for druids and shamans.
7. **Ground-placed spells.** Shadowfury, Blizzard (Improved Blizzard slow), Freezing Arrow, Explosive/Snake Trap placement, Death and Decay, Flare, Demonic Circle: Summon: the cast path takes the opponent or the caster, not a position. Frost Trap works only because it is cast at the hunter's own feet.
8. **Trap and totem as an obstacle.** Dropping a Freezing/Frost Trap in the path or around a pillar, Earthbind/Grounding Totem placement relative to the opponent.
9. **Facing for cones.** Cone of Cold, Dragon's Breath, Typhoon, Shockwave and Shadowflame hit the arc the caster faces; `Footwork` only sets facing for Blink and Disengage.
10. **Keeping the pet or demon in play** (Intimidation, Master's Call, Felguard Intercept, Succubus Seduction, Gnaw) is the pet AI's job, not the table's.
11. **Shred vs Mangle build** for the feral `WantsBehind` flag and Mutilate vs Backstab for the Assassination one.

---

## Sources

The 66 guide files are in the session scratchpad (`guides/txt/`); each short name maps to a URL as below. Icy Veins: `https://www.icy-veins.com/wotlk-classic/<name>`. Warcraft Tavern: `https://www.warcrafttavern.com/wotlk/guides/<name>/` (the `wt_` prefix is dropped, the rest of the file name is the slug).

Icy Veins (`iv_` files, name = slug): `affliction-warlock-pvp-guide`, `arcane-mage-pvp-guide`, `arms-warrior-pvp-guide`, `balance-druid-pvp-guide`, `beast-mastery-hunter-pvp-guide`, `destruction-warlock-pvp-guide`, `discipline-priest-pvp-guide`, `elemental-shaman-pvp-guide`, `enhancement-shaman-pvp-guide`, `feral-druid-pvp-guide`, `fire-mage-pvp-guide`, `frost-mage-pvp-guide`, `holy-paladin-pvp-guide`, `marksmanship-hunter-pvp-guide`, `protection-paladin-pvp-guide`, `protection-warrior-pvp-guide`, `restoration-druid-pvp-guide`, `restoration-shaman-pvp-guide`, `retribution-paladin-pvp-guide`, `shadow-priest-pvp-guide`, `subtlety-rogue-pvp-guide`, `survival-hunter-pvp-guide`, `unholy-death-knight-pvp-guide`. Last updated Jan 2023 (Frost Mage) to Oct 2023 (hunters).

Warcraft Tavern (`wt_` files, slug as shown; short names in the tags above drop the `wt_` and the ability suffix where unambiguous):
- Hunter: `pvp-beast-mastery-hunter`, `pvp-marksmanship-hunter`, `pvp-marksmanship-hunter-rotation-cooldowns-abilities`, `pvp-survival-hunter`, `pvp-survival-hunter-rotations-cooldowns`
- Mage: `pvp-arcane-mage`, `pvp-fire-mage`, `pvp-frost-mage`, `pvp-frost-mage-rotations-cooldowns-abilities`
- Warlock: `pvp-affliction-warlock-guide`, `pvp-affliction-warlock-rotation-cooldowns-abilities`, `pvp-destruction-warlock`, `pvp-destruction-warlock-rotation-cooldowns-abilities`
- Priest: `pvp-discipline-priest`, `pvp-discipline-priest-rotation-cooldowns-abilities`, `pvp-shadow-priest-guide`, `pvp-shadow-priest-rotations-cooldowns-abilities`
- Druid: `pvp-balance-druid`, `pvp-balance-druid-rotation-cooldowns-abilities`, `pvp-feral-druid`, `pvp-feral-druid-rotation-cooldowns-abilities`, `pvp-restoration-druid`, `pvp-restoration-druid-rotation-cooldowns-abilities`
- Shaman: `pvp-elemental-shaman-guide`, `pvp-elemental-shaman-rotations-cooldowns-abilities`, `pvp-enhancement-shaman`, `pvp-enhancement-shaman-rotation-cooldowns-abilities`, `pvp-restoration-shaman-guide`, `pvp-restoration-shaman-rotation-cooldowns-abilities`
- Paladin: `pvp-holy-paladin-guide`, `pvp-holy-paladin-rotation-cooldowns-abilities`, `pvp-retribution-paladin`, `pvp-retribution-paladin-rotation-cooldowns-abilities`
- Warrior: `pvp-arms-warrior`, `pvp-arms-warrior-rotation-cooldowns-abilities`, `pvp-protection-warrior`
- Rogue: `pvp-combat-rogue`, `pvp-mutilate-rogue`, `pvp-subtlety-rogue`, `pvp-subtlety-rogue-rotations-cooldowns`
- Death Knight: `pvp-frost-death-knight`, `pvp-unholy-death-knight`, `pvp-unholy-death-knight-rotations-abilities`

No guide in the set for: Fury Warrior, Protection Paladin beyond Icy Veins' (largely a copy of the Retribution text), Blood DK, Holy Priest, Assassination beyond `pvp-mutilate-rogue`. A few Warcraft Tavern pages are explicitly 3.3.5a (`pvp-mutilate-rogue`, `pvp-frost-death-knight`, the BM hunter page); the rotation pages mix in WotLK Classic details (Mass Dispel changes, Freezing Arrow).

Other sources (unchanged from the previous version):
- AzerothCore source in this workspace (verified): `azerothcore-wotlk-pb/src/server/game/Entities/Player/Player.cpp` (`CheckDuelDistance`, lines 6565-6599), `Handlers/DuelHandler.cpp` (3 s countdown), `Spells/SpellEffects.cpp` (`EffectDuel`, flag at midpoint, line 4278), `Spells/Spell.cpp` (`CheckRange`, lines 7331-7420), `Entities/Unit/Unit.cpp` (`GetMeleeRange`, line 807), `Entities/Object/ObjectDefines.h` (melee constants).
- Warcraft Wiki: https://warcraft.wiki.gg/wiki/Autoshot (min range 8->5 in 2.3.0; no Auto Shot while moving until 4.0.6), https://warcraft.wiki.gg/wiki/Dead_zone, https://warcraft.wiki.gg/wiki/Dueling, https://warcraft.wiki.gg/wiki/Cast_time
- Wowhead WotLK Classic spell data (`https://www.wowhead.com/wotlk/spell=<id>`): ids queried include 781 Disengage, 49576 Death Grip, 16979/49376 Feral Charge (Bear/Cat), 1953 Blink, 36554 Shadowstep, 11578 Charge, 25275 Intercept, 6215 Fear, 2645 Ghost Wolf, 51514 Hex, 42917 Frost Nova, 57755 Heroic Throw, 1715 Hamstring, 12323 Piercing Howl, 46968 Shockwave, 10308 Hammer of Justice, 48827 Avenger's Shield, 20066 Repentance, 1044 Hand of Freedom, 5116 Concussive Shot, 19503 Scatter Shot, 2974 Wing Clip, 49050 Aimed Shot, 49052 Steady Shot, 34490 Silencing Shot, 45524 Chains of Ice, 47476 Strangulate, 51411 Howling Blast, 48265 Unholy Presence, 10890 Psychic Scream, 48156 Mind Flay, 15487 Silence, 64044 Psychic Horror, 47585 Dispersion, 53308 Entangling Roots, 33786 Cyclone, 33357 Dash, 61384 Typhoon, 42842 Frostbolt, 2139 Counterspell, 42950 Dragon's Breath, 42931 Cone of Cold, 44572 Deep Freeze, 31589 Slow, 47809 Shadow Bolt, 47860 Death Coil, 17928 Howl of Terror, 47847 Shadowfury, 18223 Curse of Exhaustion, 48020 Demonic Circle: Teleport, 49236 Frost Shock, 49231 Earth Shock, 57994 Wind Shear, 59159 Thunderstorm, 11305 Sprint, 1766 Kick, 8643 Kidney Shot, 48674 Deadly Throw, 1833 Cheap Shot, and others listed in the body.

## Unverified items to measure on the realm

1. Effective hunter dead zone on AzerothCore (5 yd DBC min + melee range = up to about 10 center to center); the new 25-35 yd band and 12.5 yd "too close" line assume it.
2. Whether Warbringer/Juggernaut allow Charge in combat on this core (the guides say yes); whether Blink removes an existing root or stun in 3.3.5a (guide says both; data showed only immunity during the jump).
3. Unholy Presence movement speed (guide +15%, data none), Body and Soul speed, Dispersion removing snares (guide yes, data no), Judgement of Justice as a snare vs a speed cap, Feral Charge interrupt behaviour, Typhoon knockback/daze, Felguard Intercept, Maelstrom Weapon stack count, Hungering Cold/Intimidating Shout/Concussion Blow numbers, Steady Shot cast time (1.5 s data vs "2 s" guide), Entangling Roots PvP duration (3.5 s / 9 s / 27 s in three sources).
4. Disengage jump distance (data gives only `Jump` value 75, no yard figure).
5. That a Hexed or Cycloned target registers as "controlled" in `IsControlled` (it relies on `UNIT_STATE_CONFUSED`/stun/flee states; the new Hex/Cyclone/Deep Freeze/Death Coil entries assume it) and that `ai->CanCastSpell` rejects Deep Freeze when the target is neither frozen nor under a Fingers of Frost proc.
6. Whether hunters kite enough with HardCaster on (stand-to-shoot in band; move only through a window or Disengage).

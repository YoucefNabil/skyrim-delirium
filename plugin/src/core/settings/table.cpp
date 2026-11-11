// The settings table's rows: every setting, in the ini's order. A new setting is one row here (and its member in
// engine.h); loading, the check (tools\check-settings.ps1) and later the menu and presets pick it up.
#include "settings/table.h"

namespace delirium
{
	namespace
	{
		using namespace settings;
		constexpr unsigned P = kPreset;
		constexpr unsigned D = kDetail;

		Setting B(const char* a_sec, const char* a_key, bool& a_v, bool a_def, Tab a_tab, unsigned a_flags, const char* a_label, const char* a_help)
		{
			return { a_sec, a_key, &a_v, a_def ? 1.0 : 0.0, nullptr, kAny, a_tab, a_flags, a_label, a_help };
		}
		Setting F(const char* a_sec, const char* a_key, float& a_v, double a_def, Limit a_lim, Tab a_tab, unsigned a_flags, const char* a_label, const char* a_help)
		{
			return { a_sec, a_key, &a_v, a_def, nullptr, a_lim, a_tab, a_flags, a_label, a_help };
		}
		template <class T>
		Setting I(const char* a_sec, const char* a_key, T& a_v, long a_def, Limit a_lim, Tab a_tab, unsigned a_flags, const char* a_label, const char* a_help)
		{
			return { a_sec, a_key, &a_v, static_cast<double>(a_def), nullptr, a_lim, a_tab, a_flags, a_label, a_help };
		}
		Setting S(const char* a_sec, const char* a_key, std::string& a_v, const char* a_def, Tab a_tab, unsigned a_flags, const char* a_label, const char* a_help,
			const char* a_oldKey = nullptr)
		{
			return { a_sec, a_key, &a_v, 0.0, a_def, kAny, a_tab, a_flags, a_label, a_help, a_oldKey };
		}
		// a push that another setting caps (react.cpp: a projectile's push at most [Hits] fMaxPush, a blow's [Melee] fMaxPush)
		Setting CappedBy(Setting a_s, const float& a_cap)
		{
			a_s.cappedBy = &a_cap;
			return a_s;
		}
		// the low end of a min / max pair (the next row has AtLeast this one): the menu draws both as one range
		Setting Pair(Setting a_s, const char* a_label)
		{
			a_s.rangeLabel = a_label;
			return a_s;
		}
	}

	std::vector<settings::Setting> Engine::SettingsTable()
	{
		using enum Tab;
		return {
			// [Data]
			S("Data", "sGTAFile", fredPath_, "", Advanced, kRestart, "GTA IV Fred.art",
				"Full path to Fred.art in your own GTA IV install, for the behaviour closest to GTA IV. Empty: the mod's own character", "sCharacterFile"),
			S("Data", "sCharacter", buildPath_, "Data/SKSE/Plugins/SkyrimDelirium/skyrim_character.json", Advanced, kRestart, "Character file",
				"The mod's own character (body, joints, muscles), relative to the game folder"),
			S("Data", "sRig", rigPath_, "Data/SKSE/Plugins/SkyrimDelirium/skyrim_character_rig.txt", Advanced, kRestart, "Rig file",
				"How the character maps onto the Skyrim skeleton, relative to the game folder"),

			// [Test]
			I("Test", "iPushKey", pushKey_, 0x41, kAny, Advanced, kNone, "Push key", "DirectX scan code of the test push on the NPC under the crosshair (0x41 = F7)"),
			I("Test", "iReleaseKey", releaseKey_, 0x3C, kAny, Advanced, kNone, "Release key", "DirectX scan code that hands every body back to Skyrim (0x3C = F2)"),
			F("Test", "fPush", push_, 80.0, kAny, Advanced, kNone, "Test push", "The test key's push, N s: 80 stumbles, two or three in a row knock the NPC down"),
			B("Test", "bAutoRelease", autoRelease_, true, Advanced, kNone, "Hand back when caught",
				"Hand back to Skyrim's animation once the NPC has caught their balance; a fallen body is held until Skyrim's get-up"),
			B("Test", "bFollowAnimation", followAnimation_, true, Advanced, kNone, "Follow the animation",
				"The body holds the NPC's live animation pose (guard up, gestures) instead of a fixed stance"),
			B("Test", "bRecordPoses", recordPoses_, false, Advanced, kHidden, "Record poses", "Dev: the held pose each frame while standing, for fit-test"),
			B("Test", "bStandingLegs", standingLegs_, true, Advanced, kHidden, "Standing legs", "Dev: the legs hold the body's own stance, not the animation's"),

			// [Ragdoll]
			B("Ragdoll", "bTakeOver", takeOver_, true, Reactions, P, "Take over Skyrim's ragdolls",
				"Delirium takes over every living NPC Skyrim ragdolls (Fus Ro Dah, pushes, explosions, other mods) until the NPC gets up"),
			F("Ragdoll", "fTorqueGain", gain_, 1.0, kAny, Advanced, P, "Muscle strength", "Delirium's muscles on Skyrim's ragdoll bodies (1 = Delirium's own)"),
			F("Ragdoll", "fJointDamping", damp_, 0.0, Range(0.0, 1.0), Advanced, kHidden, "Joint damping", "Dev: share of a joint's spin its muscles take out each step"),
			F("Ragdoll", "fJointFriction", friction_, 0.0, Range(0.0, 1.0), Advanced, P, "Skyrim's joint friction",
				"Skyrim's own joint friction while Delirium drives (0 = off so the behaviours show, 1 = untouched)"),
			B("Ragdoll", "bMatchMass", matchMass_, true, Advanced, P, "Delirium's masses", "While Delirium drives, Skyrim's bodies weigh what Delirium's parts do"),
			B("Ragdoll", "bMatchContact", matchContact_, true, Advanced, P, "Delirium's grip and bounce", "And grip the ground and bounce as Delirium's parts do"),
			B("Ragdoll", "bTurnAboutJoint", aboutJoint_, true, Advanced, kHidden, "Turn about the joint", "Dev: a muscle turns the limb about its joint"),

			// [Creatures]
			B("Creatures", "bDraugr", draugr_, true, Reactions, P, "Draugr", "Draugr react as people do (their body is fitted to their own bones). Off: Skyrim's own reactions"),
			B("Creatures", "bSkeletons", skeletons_, true, Reactions, P, "Skeletons (very experimental)", "Very experimental. Skeletons react as people do (fitted to their own bones). Off: Skyrim's own reactions"),
			B("Creatures", "bFalmer", falmer_, true, Reactions, P, "Falmer (very experimental)", "Very experimental. Falmer stand hunched, which Delirium can't balance yet: only blows that put them down (throws, knockdowns, killing blows) are Delirium's, the rest stays Skyrim's. Off: Skyrim's own reactions"),

			// [Arrows]
			B("Arrows", "bShotReaction", arrows_, true, Reactions, P, "Arrows", "An arrow that hits a living NPC: Delirium's shot reaction (stumble, clutch the wound, catch their balance or fall)"),
			CappedBy(F("Arrows", "fPush", arrowPush_, 55.0, kAny, Strength, P, "Arrow push", "The arrow's push at the point it went in, N s (the test push is 80)"),
				hitMaxPush_),
			F("Arrows", "fWoundedBelow", wounded_, 0.2, Range(0.0, 1.0), Strength, P | D, "Wounded below", "Under this share of the NPC's health a hit knocks them down instead of them catching their balance"),

			// [Spells]
			B("Spells", "bShotReaction", spells_, true, Reactions, P, "Spells",
				"Destruction spells' missiles hit like arrows: frost as an arrow, fire burns on top, shock jolts"),
			F("Spells", "fBurnTime", burnTime_, 2.5, Min(0.0), Timing, P, "Burning", "Seconds the NPC burns after a fire hit (the arms beat at the flames)"),
			F("Spells", "fShockTime", shockTime_, 2.0, Min(0.0), Timing, P, "Shocked", "Seconds the NPC is shocked after a lightning hit"),
			F("Spells", "fChannelLinger", channelLinger_, 0.6, Min(0.1), Timing, P, "Channel linger",
				"Channelled spells (Flames, Frostbite, Sparks): their reaction lasts this long after the last hit, s"),
			F("Spells", "fStartChance", spellChance_[0], 1.0, Range(0.0, 1.0), Difficulty, P, "Spell: chance at full health",
				"Chance a spell's missile brings the NPC into Delirium at full health (in between by their health). Once in, every hit plays"),
			F("Spells", "fStartChanceHurt", spellChance_[1], 1.0, Range(0.0, 1.0), Difficulty, P, "Spell: chance near death",
				"The same near death. A killing blow and a spell that staggers always do"),
			F("Spells", "fChannelChance", channelChance_[0], 1.0, Range(0.0, 1.0), Difficulty, P, "Channel: chance at full health",
				"Chance per second of a channelled spell, at full health. Once in, every tick keeps the NPC in"),
			F("Spells", "fChannelChanceHurt", channelChance_[1], 1.0, Range(0.0, 1.0), Difficulty, P, "Channel: chance near death", "The same near death"),

			// [Stagger]
			B("Stagger", "bReplace", staggers_, true, Reactions, P, "Staggers",
				"Every stagger (heavy blow, shield bash, Fus, magic, area staggers) is played by Delirium as a push away from the attacker"),
			CappedBy(F("Stagger", "fPush", staggerPush_, 90.0, Min(0.0), Strength, P, "Stagger push",
						 "Push for the strongest stagger (magnitude 1), N s. A melee blow that staggers takes the bigger of this and the boosted blow"),
				staggerMaxPush_),
			F("Stagger", "fMinShare", staggerMin_, 0.4, Range(0.0, 1.0), Strength, P | D, "Light stagger share", "A light stagger (magnitude 0) gets this share of the push"),
			F("Stagger", "fHitBoost", staggerHitBoost_, 0.5, Min(0.0), Strength, P, "Staggering blow boost",
				"A melee blow that staggers pushes as hard as the blow x (1 + this x the stagger's magnitude)"),
			F("Stagger", "fMaxPush", staggerMaxPush_, 200.0, Min(0.0), Strength, P, "Stagger push at most",
				"A stagger while standing pushes at most this, N s, in place of the blow's or spell's cap (past ~200 the NPC may fall)"),

			// [Bump]
			B("Bump", "bReplace", bumps_, true, Reactions, P, "Bumps",
				"Running into an NPC (sprinting, fast): Delirium plays their stumble (the stand.bump row) in place of Skyrim's. "
				"They still step out of the way and say their line"),
			F("Bump", "fPush", bumpPush_, 50.0, Min(0.0), Strength, P, "Bump push", "Push when you run into an NPC, N s, along your run"),
			F("Bump", "fRepeatAfter", bumpRepeat_, 1.0, Min(0.0), Timing, P | D, "Bump again after",
				"Skyrim bumps every frame you touch: one push per run-in, and a new one only after this many seconds without contact"),

			// [Bodies]
			B("Bodies", "bCollide", bodies_, true, Reactions, P, "Bodies collide",
				"NPCs' bodies are in Skyrim's physics world while they stand: a thrown or falling body stops against them, and a body "
				"that hits one hard enough is a blow on them (the stand.bump row), from a ragdoll or from an NPC Delirium has stumbling"),
			F("Bodies", "fSpeed", bodySpeed_, 2.5, Min(0.0), Strength, P | D, "Body strikes from",
				"A ragdolled body is a blow on a standing NPC from this speed against them, m/s"),
			F("Bodies", "fStumbleSpeed", stumbleSpeed_, 0.8, Min(0.0), Strength, P | D, "Stumble strikes from",
				"The same for an NPC Delirium has on their feet (a stagger into someone); they are pushed back as hard"),
			F("Bodies", "fPush", bodyPush_, 12.0, Min(0.0), Strength, P, "Body push", "Push per m/s the body comes at them, N s"),
			F("Bodies", "fMaxPush", bodyMaxPush_, 160.0, Min(0.0), Strength, P, "Body push at most", "A body's push at most, N s"),
			F("Bodies", "fRepeatAfter", bodyRepeat_, 1.5, Min(0.0), Timing, P | D, "Body strikes again after",
				"Seconds before the same NPC takes another blow from a body"),

			// [Melee]
			B("Melee", "bReact", melee_, true, Reactions, P, "Melee blows",
				"While Delirium has the NPC, every blow reacts: a light one flinches, a heavier one makes them step, past that they fall"),
			CappedBy(F("Melee", "fPush", meleePush_, 50.0, Min(0.0), Strength, P, "Blow push", "Push for a plain blow (iron sword, 15 damage), N s"),
				meleeMaxPush_),
			F("Melee", "fPower", meleePower_, 2.2, Min(0.0), Strength, P, "Power attack", "x the push for a power attack"),
			F("Melee", "fBash", meleeBash_, 1.6, Min(0.0), Strength, P, "Shield bash", "x the push for a shield bash"),
			F("Melee", "fSneak", meleeSneak_, 1.3, Min(0.0), Strength, P | D, "Sneak attack", "x the push for a sneak attack"),
			F("Melee", "fBlocked", meleeBlocked_, 0.5, Min(0.0), Strength, P | D, "Blocked blow", "x the push for a blocked blow"),
			F("Melee", "fWeightRef", meleeWeightRef_, 9.0, Min(0.1), Strength, P | D, "Weapon weight at 1x",
				"Heavier weapons push harder, by the square root of weight / this"),
			Pair(F("Melee", "fHeftMin", meleeHeftMin_, 0.6, Min(0.0), Strength, P | D, "Lightest weapon", "The weight factor at least (dagger 0.6)"), "Weapon weight factor"),
			F("Melee", "fHeftMax", meleeHeftMax_, 1.7, AtLeast(meleeHeftMin_), Strength, P | D, "Heaviest weapon", "The weight factor at most (warhammer ~1.7)"),
			F("Melee", "fSwing", meleeSwing_, 0.5, Min(0.0), Strength, P | D, "Swing sweep", "How much a swing's sideways sweep bends the push (0 = straight away from the attacker)"),
			F("Melee", "fChop", meleeChop_, 0.35, Min(0.0), Strength, P | D, "Overhead chop", "An overhead chop pushes this much down"),
			F("Melee", "fFlinchBelow", flinchBelow_, 55.0, Min(0.0), Strength, P | D, "Flinch below", "Blows pushing less than this flinch, harder ones step, N s (the 'light' rows)"),
			Pair(F("Melee", "fOomphFrom", meleeOomphFrom_, 1.3, Min(0.0), Strength, P | D, "Heavy blow from", "A blow this many times a plain one's push starts to be heavy (oomph 0)"), "Heavy blow: starts / full"),
			F("Melee", "fOomphTo", meleeOomphTo_, 4.0, AtLeast(meleeOomphFrom_, 0.1f), Strength, P | D, "Heavy blow full", "At this many times it is fully heavy (oomph 1)"),
			F("Melee", "fHeadWhip", headWhip_, 0.2, Min(0.0), Strength, P | D, "Head whip", "Share of a heavy blow's push at the head too (the head snaps)"),
			F("Melee", "fMaxPush", meleeMaxPush_, 160.0, Min(0.0), Strength, P, "Blow push at most",
				"A blow pushes at most this, N s (past it the stagger and fall carry it: one big shove looks limp)"),

			// [Explosions]
			B("Explosions", "bPush", blasts_, true, Reactions, P, "Explosions", "An explosion (Fireball, runes, explosive arrows) pushes everyone it catches away from its centre"),
			F("Explosions", "fPush", blastPush_, 90.0, Min(0.0), Strength, P, "Explosion push", "N s at the centre for a 15-damage explosion ([Hits] damage at 1x)"),
			F("Explosions", "fFarShare", blastFar_, 0.4, Range(0.0, 1.0), Strength, P | D, "At the edge", "Share of the push at the explosion's edge"),
			F("Explosions", "fUp", blastUp_, 0.25, Range(0.0, 1.0), Strength, P | D, "Upward", "The push's upward part (0 flat, 1 straight up)"),
			F("Explosions", "fFlySpeed", flySpeed_, 3.0, Min(0.0), Strength, P, "Thrown speed", "m/s the NPC is thrown at, at the centre, for a 15-damage explosion"),
			F("Explosions", "fFlyUp", flyUp_, 0.65, Min(0.0), Strength, P | D, "Thrown upward", "m/s up per m/s along"),
			Pair(F("Explosions", "fFlyMin", flyMin_, 2.0, Min(0.0), Strength, P | D, "Thrown from", "Under this m/s the NPC only takes the push, not thrown"), "Thrown between (m/s)"),
			F("Explosions", "fFlyMax", flyMax_, 9.0, AtLeast(flyMin_), Strength, P | D, "Thrown at most", "m/s at most"),
			F("Explosions", "fHighFallAt", highFallAt_, 4.5, Min(0.0), Strength, P | D, "Thrown far from", "From this m/s the NPC is thrown far ('far' rows: Delirium's high fall)"),
			F("Explosions", "fTumble", tumble_, 10.0, Min(0.0), Strength, P | D, "Tumble", "N s at the head per m/s thrown: the NPC tumbles head first"),

			// [Falls]
			B("Falls", "bHighFall", highFalls_, true, Reactions, P, "Ledge falls",
				"An NPC who drops off a ledge falls in Delirium (arms out, legs ready, landing into a ragdoll and Skyrim's get-up). Skyrim's fall damage is unchanged"),
			F("Falls", "fHighFallDrop", highFallDrop_, 3.0, Min(0.5), Strength, P | D, "Ledge fall from", "Metres of drop before the NPC's fall is Delirium's"),

			// [Down]
			F("Down", "fWritheTime", writheTime_, 1.0, Min(0.0), Timing, P, "Writhe", "Knocked down, the NPC writhes this many s + the next x oomph (half for a blow)"),
			F("Down", "fWritheAdd", writheAdd_, 1.2, Min(0.0), Timing, P | D, "Writhe per oomph", "Extra s of writhing for a harder hit"),
			F("Down", "fFoetalChance", foetalChance_, 0.3, Range(0.0, 1.0), Reactions, P, "Curl up chance", "Chance the NPC curls up on the ground instead (when no down row decides)"),

			// [Hits]
			F("Hits", "fDamageRef", damageRef_, 15.0, Min(0.1), Strength, P | D, "Damage at 1x", "A hit of this much damage pushes at 1x (arrows, spells, melee, staggers)"),
			F("Hits", "fExponent", damageExp_, 0.8, Range(0.0, 2.0), Strength, P | D, "Damage curve", "Twice the damage pushes 2^this as hard (1.74x at 0.8)"),
			Pair(F("Hits", "fScaleMin", damageMin_, 0.4, Min(0.0), Strength, P | D, "Damage scale at least", "The damage scale at least"), "Damage scale range"),
			F("Hits", "fScaleMax", damageMax_, 3.0, AtLeast(damageMin_), Strength, P | D, "Damage scale at most", "The damage scale at most"),
			Pair(F("Hits", "fOomphFrom", oomphFrom_, 1.0, kAny, Strength, P | D, "Hard hit from", "Damage scale where a hit starts jolting harder (oomph 0)"), "Hard hit: starts / full"),
			F("Hits", "fOomphTo", oomphTo_, 2.5, AtLeast(oomphFrom_, 0.01f), Strength, P | D, "Hard hit full", "Damage scale for the full jolt (oomph 1)"),
			F("Hits", "fOomphSpin", oomphSpin_, 2.0, Min(0.0), Strength, P | D, "Hard hit twist", "The shot twists the NPC x (1 + this x oomph)"),
			F("Hits", "fOomphPush", oomphPush_, 1.0, Min(0.0), Strength, P | D, "Hard hit push", "The push x (1 + this x oomph)"),
			F("Hits", "fMaxPush", hitMaxPush_, 150.0, Min(0.0), Strength, P, "Hit push at most",
				"A spell's or arrow's push at most, N s (past it one shove knocks the NPC over limp)"),
			F("Hits", "fPelvisShare", pelvisShare_, 0.0, Range(0.0, 1.0), Strength, P | D, "At the pelvis",
				"Share of a standing hit's push at the pelvis instead of the part hit: catch steps instead of folding at the waist"),
			I("Hits", "iMaxSteps", maxSteps_, 10, Range(0.0, 100.0), Strength, P | D, "Catch steps", "Most catch steps a hit takes before the NPC stops stepping (Delirium's default 6)"),

			// [Collision]
			B("Collision", "bWorldCollision", collision_, true, Advanced, kNone, "World collision",
				"The body collides with Skyrim's world (floors, stairs, walls, terrain) instead of a flat floor"),
			F("Collision", "fRadius", colRadius_, 600.0, kAny, Advanced, kNone, "Around the NPC", "How far around the NPC to take the world, game units (70 = 1 m)"),
			F("Collision", "fHeight", colHeight_, 300.0, kAny, Advanced, kNone, "Above and below", "How far up and down, game units"),
			I("Collision", "iMaxTriangles", colMaxTris_, 40000, Min(1000.0), Advanced, kNone, "Triangles at most", "The most world triangles taken (performance)"),

			// [Deaths]
			B("Deaths", "bNoKillMoves", noKillMoves_, true, Reactions, P | kRestart, "No killmoves",
				"Skyrim's killmoves off (a save keeps it: `set killmove to 1` brings them back)"),
			B("Deaths", "bNoKillCams", noKillCams_, true, Reactions, P | kRestart, "No kill cams", "Kill cams off"),
			F("Deaths", "fDyingTime", dyingTime_, 3.0, Range(0.0, 10.0), Timing, P, "Dying standing",
				"Seconds a killed NPC may die standing in Delirium before the death goes through (0 = die at once)"),
			B("Deaths", "bDieAtOnce", dieAtOnce_, true, Reactions, P, "Die at once",
				"The killing blow kills at once; she stumbles on as Skyrim's ragdoll held to Delirium's pose (off = kept alive at 1 health until she collapses)"),
			Pair(F("Deaths", "fCollapseAfterMin", collapseMin_, 0.8, Range(0.0, 10.0), Timing, P, "Collapses after (min)", "Dying standing: s the NPC fights to stay up, at least"), "Collapses after (s)"),
			F("Deaths", "fCollapseAfterMax", collapseMax_, 1.6, AtLeast(collapseMin_), Timing, P, "Collapses after (max)", "And at most"),
			F("Deaths", "fDyingHealth", dyingHealth_, 70.0, Range(0.0, 99.0), Strength, P, "Killing blow health",
				"The killing blow as Delirium's shot at this health (0 = a limp flop, under 100 = wounded, falls)"),
			Pair(F("Deaths", "fLimpAfterMin", limpMin_, 1.8, Min(0.2), Timing, P, "Limp after (min)", "Seconds until the corpse goes limp, at least"), "Limp after (s)"),
			F("Deaths", "fLimpAfterMax", limpMax_, 3.0, AtLeast(limpMin_), Timing, P, "Limp after (max)", "And at most"),
			F("Deaths", "fWritheChance", dyingWrithe_, 0.5, Range(0.0, 1.0), Reactions, P, "Death writhe chance", "Chance of a weak writhe on the ground first"),
			F("Deaths", "fCatchFallChance", dyingCatch_, 0.5, Range(0.0, 1.0), Reactions, P, "Break the fall chance", "Chance the arms go out to break the fall"),
			F("Deaths", "fDeathPush", deathPush_, 1.5, Range(0.0, 6.0), Strength, P, "Death push", "m/s away from the killer as the NPC dies (in place of Skyrim's lunge toward the killer)"),
			F("Deaths", "fDeathMaxSpeed", deathMaxSpeed_, 6.0, Min(1.0), Strength, P | D, "Death speed cap", "m/s cap on any body as the NPC dies (Skyrim's death throws limbs at 17 m/s)"),
			F("Deaths", "fShotTime", dyingReact_, 1.2, Range(0.0, 5.0), Timing, P | D, "Dying shot", "Seconds the dying shot (reaching for the wound, breaking the fall) plays before a writhe"),

			// [Reactions] (the rows themselves are read by LoadReactions)
			F("Reactions", "fHeavyOomph", heavyOomph_, 0.5, Range(0.0, 1.01), Reactions, P | D, "Heavy from", "From this oomph a blow picks the 'heavy' rows"),
		};
	}

}

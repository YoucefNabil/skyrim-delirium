// Every setting's value and every reaction row as text, one per line: the settings check (tools\check-settings.ps1)
// compares it before and after a change to how settings are read. Written member by member, not from the settings
// table, so it checks the table instead of repeating it.
#include "shared.h"

namespace delirium
{
	std::string Engine::DumpSettings() const
	{
		std::string out;
		const auto  f = [&](const char* a_name, float a_v) { out += std::format("{} = {:.9g}\n", a_name, a_v); };
		const auto  b = [&](const char* a_name, bool a_v) { out += std::format("{} = {}\n", a_name, a_v); };
		const auto  i = [&](const char* a_name, long long a_v) { out += std::format("{} = {}\n", a_name, a_v); };
		const auto  s = [&](const char* a_name, const std::string& a_v) { out += std::format("{} = '{}'\n", a_name, a_v); };

		s("Data.sGTAFile", fredPath_);
		s("Data.sCharacter", buildPath_);
		s("Data.sRig", rigPath_);
		i("Test.iPushKey", pushKey_);
		i("Test.iReleaseKey", releaseKey_);
		f("Test.fPush", push_);
		b("Test.bAutoRelease", autoRelease_);
		b("Test.bFollowAnimation", followAnimation_);
		b("Test.bRecordPoses", recordPoses_);
		b("Test.bStandingLegs", standingLegs_);
		b("Collision.bWorldCollision", collision_);
		f("Collision.fRadius", colRadius_);
		f("Collision.fHeight", colHeight_);
		i("Collision.iMaxTriangles", static_cast<long long>(colMaxTris_));
		b("Ragdoll.bTakeOver", takeOver_);
		f("Ragdoll.fTorqueGain", gain_);
		f("Ragdoll.fJointDamping", damp_);
		f("Ragdoll.fJointFriction", friction_);
		b("Ragdoll.bMatchMass", matchMass_);
		b("Ragdoll.bMatchContact", matchContact_);
		b("Ragdoll.bTurnAboutJoint", aboutJoint_);
		b("Creatures.bDraugr", draugr_);
		b("Creatures.bSkeletons", skeletons_);
		b("Creatures.bFalmer", falmer_);
		b("Deaths.bNoKillMoves", noKillMoves_);
		b("Deaths.bNoKillCams", noKillCams_);
		f("Deaths.fDyingHealth", dyingHealth_);
		f("Deaths.fLimpAfterMin", limpMin_);
		f("Deaths.fLimpAfterMax", limpMax_);
		f("Deaths.fWritheChance", dyingWrithe_);
		f("Deaths.fCatchFallChance", dyingCatch_);
		f("Deaths.fDeathPush", deathPush_);
		f("Deaths.fDeathMaxSpeed", deathMaxSpeed_);
		f("Deaths.fShotTime", dyingReact_);
		f("Deaths.fDyingTime", dyingTime_);
		b("Deaths.bDieAtOnce", dieAtOnce_);
		f("Deaths.fCollapseAfterMin", collapseMin_);
		f("Deaths.fCollapseAfterMax", collapseMax_);
		f("Reactions.fHeavyOomph", heavyOomph_);
		b("Arrows.bShotReaction", arrows_);
		f("Arrows.fPush", arrowPush_);
		f("Arrows.fWoundedBelow", wounded_);
		b("Spells.bShotReaction", spells_);
		f("Spells.fBurnTime", burnTime_);
		f("Spells.fShockTime", shockTime_);
		f("Spells.fChannelLinger", channelLinger_);
		f("Spells.fStartChance", spellChance_[0]);
		f("Spells.fStartChanceHurt", spellChance_[1]);
		f("Spells.fChannelChance", channelChance_[0]);
		f("Spells.fChannelChanceHurt", channelChance_[1]);
		b("Stagger.bReplace", staggers_);
		f("Stagger.fPush", staggerPush_);
		f("Stagger.fMinShare", staggerMin_);
		f("Stagger.fHitBoost", staggerHitBoost_);
		f("Stagger.fMaxPush", staggerMaxPush_);
		b("Bump.bReplace", bumps_);
		f("Bump.fPush", bumpPush_);
		f("Bump.fRepeatAfter", bumpRepeat_);
		b("Bodies.bCollide", bodies_);
		f("Bodies.fSpeed", bodySpeed_);
		f("Bodies.fStumbleSpeed", stumbleSpeed_);
		f("Bodies.fPush", bodyPush_);
		f("Bodies.fMaxPush", bodyMaxPush_);
		f("Bodies.fRepeatAfter", bodyRepeat_);
		f("Hits.fDamageRef", damageRef_);
		f("Hits.fExponent", damageExp_);
		f("Hits.fScaleMin", damageMin_);
		f("Hits.fScaleMax", damageMax_);
		f("Hits.fOomphFrom", oomphFrom_);
		f("Hits.fOomphTo", oomphTo_);
		f("Hits.fOomphSpin", oomphSpin_);
		f("Hits.fOomphPush", oomphPush_);
		f("Hits.fMaxPush", hitMaxPush_);
		f("Hits.fPelvisShare", pelvisShare_);
		i("Hits.iMaxSteps", maxSteps_);
		b("Melee.bReact", melee_);
		f("Melee.fPush", meleePush_);
		f("Melee.fPower", meleePower_);
		f("Melee.fBash", meleeBash_);
		f("Melee.fSneak", meleeSneak_);
		f("Melee.fBlocked", meleeBlocked_);
		f("Melee.fWeightRef", meleeWeightRef_);
		f("Melee.fHeftMin", meleeHeftMin_);
		f("Melee.fHeftMax", meleeHeftMax_);
		f("Melee.fSwing", meleeSwing_);
		f("Melee.fChop", meleeChop_);
		f("Melee.fFlinchBelow", flinchBelow_);
		f("Melee.fOomphFrom", meleeOomphFrom_);
		f("Melee.fOomphTo", meleeOomphTo_);
		f("Melee.fHeadWhip", headWhip_);
		f("Melee.fMaxPush", meleeMaxPush_);
		b("Explosions.bPush", blasts_);
		f("Explosions.fPush", blastPush_);
		f("Explosions.fFarShare", blastFar_);
		f("Explosions.fUp", blastUp_);
		f("Explosions.fFlySpeed", flySpeed_);
		f("Explosions.fFlyUp", flyUp_);
		f("Explosions.fFlyMin", flyMin_);
		f("Explosions.fFlyMax", flyMax_);
		b("Falls.bHighFall", highFalls_);
		f("Falls.fHighFallDrop", highFallDrop_);
		f("Explosions.fHighFallAt", highFallAt_);
		f("Explosions.fTumble", tumble_);
		f("Down.fWritheTime", writheTime_);
		f("Down.fWritheAdd", writheAdd_);
		f("Down.fFoetalChance", foetalChance_);

		std::vector<std::string> keys;
		for (const auto& [k, r] : rules_) {
			keys.push_back(k);
		}
		std::sort(keys.begin(), keys.end());
		for (const auto& k : keys) {
			const Rule& r = rules_.at(k);
			out += std::format("rule {} = base '{}' |", k, r.base);
			for (const auto& [n, v] : r.layers) {
				out += std::format(" {}:{:.9g}", n, v);
			}
			out += std::format(" | text '{}'\n", r.text);
		}
		return out;
	}

}

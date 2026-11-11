// Hits come in: arrows and spells (OnHit), weapons and explosions (OnHitData, Blast), staggers. Who is
// eligible, the chance a hit brings her into Delirium, how hard it pushes.
#include "shared.h"

namespace delirium
{
	bool Engine::Eligible(RE::Actor* a_target, const char* a_what, bool a_quiet) const
	{
		if (!ready_ || !a_target || a_target->IsPlayerRef() || (a_target->IsDead() && !DeadOnFeet(a_target)) || a_target->IsDisabled() || !a_target->Get3D(false)) {
			if (!a_quiet) {
				logger::info("{}: skipped (off, dead, disabled or no 3D)", a_what);
			}
			return false;
		}
		// people only (the rig is a human's) and the creatures [Creatures] turns on, not riding or sitting (the body
		// would pull her off)
		auto* race = a_target->GetRace();
		if (!race || !(race->HasKeywordString("ActorTypeNPC") || Creature(a_target)) || a_target->IsOnMount() ||
			a_target->AsActorState()->GetSitSleepState() != RE::SIT_SLEEP_STATE::kNormal) {
			if (!a_quiet) {
				logger::info("{}: skipped {:08X} (not a person, riding or sitting)", a_what, a_target->GetFormID());
			}
			return false;
		}
		return true;
	}

	// by the race's editor ID (mods rename them: TDC_DraugrRace), not its keywords: draugr and skeletons are both
	// ActorTypeUndead. Skeletal horses, dragons and the rest have no person's bones. Falmer have a person's bones but
	// stand hunched (their body is fitted to them: body/fit.cpp).
	bool Engine::Creature(RE::Actor* a_actor) const
	{
		if (!(draugr_ || skeletons_ || falmer_)) {
			return false;
		}
		auto*       race = a_actor ? a_actor->GetRace() : nullptr;
		const char* id = race ? race->GetFormEditorID() : nullptr;
		if (!id || !*id) {
			return false;
		}
		std::string s = id;
		std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (s.find("horse") != std::string::npos || s.find("dragon") != std::string::npos) {
			return false;
		}
		return (draugr_ && s.find("draugr") != std::string::npos) || (skeletons_ && s.find("skeleton") != std::string::npos) ||
		       (falmer_ && s.find("falmer") != std::string::npos);
	}

	float Engine::StartChance(RE::Actor* a_target, const float (&a_chance)[2]) const
	{
		auto*       av = a_target->AsActorValueOwner();
		const float full = av->GetPermanentActorValue(RE::ActorValue::kHealth);
		const float share = full > 0.0f ? std::clamp(av->GetActorValue(RE::ActorValue::kHealth) / full, 0.0f, 1.0f) : 1.0f;
		return a_chance[0] + (a_chance[1] - a_chance[0]) * (1.0f - share);
	}

	void Engine::OnHit(RE::Actor* a_target, const RE::NiPoint3& a_at, const RE::NiPoint3& a_vel, bool a_exact, int a_kind, bool a_channel, bool a_sure)
	{
		const char* what = kHitNames[static_cast<std::size_t>(std::clamp(a_kind, 0, 4))];
		if (!(a_kind == kArrow ? arrows_ : spells_)) {
			return;
		}
		if (a_channel) {
			// a held spell: no blow, only its element while it keeps hitting
			if (a_kind == kMagic || !Eligible(a_target, what, true)) {
				return;
			}
			std::scoped_lock l(lock_);
			for (auto& a : active_) {
				if (a->actor.get().get() == a_target) {
					if (!a->death) {
						Channel(*a, a_target, a_vel, a_kind);
					}
					return;
				}
			}
			if (a_target->IsInRagdollState()) {
				return;
			}
			// not in Delirium yet: each second of channel brings her in by chance (then every tick keeps her in, above).
			// Ticks come ~0.15 s apart: the chance for the time since the last one (a fresh channel counts as one tick)
			if (!a_sure && !Dying(a_target)) {
				const auto  now = std::chrono::steady_clock::now();
				const auto  it = channelTick_.find(a_target->GetFormID());
				const float dt = it == channelTick_.end() ? 0.15f :
				                                            std::clamp(std::chrono::duration<float>(now - it->second).count(), 0.0f, 0.5f);
				if (channelTick_.size() > 64) {
					std::erase_if(channelTick_, [&](const auto& p) { return now - p.second > std::chrono::seconds(5); });
				}
				channelTick_[a_target->GetFormID()] = now;
				const float perSecond = StartChance(a_target, channelChance_);
				if (Rand(0.0f, 1.0f) >= 1.0f - std::pow(1.0f - perSecond, dt)) {
					return;  // Skyrim's own burn / shock / frost plays
				}
				logger::info("{:08X}: {} channel brings her in ({:.0f} % a second)", a_target->GetFormID(), what, perSecond * 100.0f);
			}
			channelTick_.erase(a_target->GetFormID());
			if (Activate(a_target)) {
				Channel(*active_.back(), a_target, a_vel, a_kind);
			}
			return;
		}
		const float speed = a_vel.Length();
		if (speed < 1.0f) {
			return;
		}
		// as hard as the hit (its damage), at the point it went in, along its flight
		Blow b;
		b.what = what;
		b.at = a_at;
		b.exact = a_exact;
		b.dir = { a_vel.x / speed, a_vel.y / speed, a_vel.z / speed };
		b.shot = true;
		b.element = a_kind;
		b.type = a_kind == kArrow ? kBlowArrow : kBlowSpell;
		LastHit     last;
		const bool  hit = a_target && RecentHit(a_target->GetFormID(), 0.5f, last);
		const float scale = hit ? DamageScale(a_target, last.damage) : 1.0f;
		b.oomph = std::clamp((scale - oomphFrom_) / (oomphTo_ - oomphFrom_), 0.0f, 1.0f);
		b.push = arrowPush_ * scale * (1.0f + oomphPush_ * b.oomph) * Rand(1.0f, 1.17f);
		if (hit) {
			b.by = last.from.get();
		}
		b.note = fmt::format("{:.0f} damage x{:.2f}, oomph {:.2f}", hit ? last.damage : 0.0f, scale, b.oomph);
		if (hit && last.shout && a_kind != kArrow) {
			// a fire / frost / shock shout (a breath, any mod's): a shout's rows by its words (stand.shout.word1-3), pushed
			// like a stagger of that many words, not like a bolt of its damage; its element on top
			b.what = "shout";
			b.type = kBlowShout;
			b.shot = false;
			b.word = last.word;
			b.knocks = last.stagger >= kKnockStagger;
			const float words = b.word ? static_cast<float>(b.word) / 3.0f : 0.5f;
			b.oomph = words;
			b.push = staggerPush_ * (staggerMin_ + (1.0f - staggerMin_) * words) * Rand(0.9f, 1.1f);
			b.note = fmt::format("{} word{}, {}, {:.0f} damage", b.word, b.word == 1 ? "" : "s", what, last.damage);
		}
		if (a_kind != kArrow && !a_sure) {
			b.startChance = StartChance(a_target, spellChance_);  // arrows always; a spell by chance unless she is in already
		}
		React(a_target, b);
	}

	float Engine::DamageScale([[maybe_unused]] RE::Actor* a_target, float a_damage) const
	{
		// by the damage itself, the same for everyone: fDamageRef pushes at 1x, twice the damage 2^fExponent
		// as hard (1.74x at 0.8), within fScaleMin-fScaleMax. Her health does not flatten a stronger spell
		if (a_damage <= 0.0f) {
			return 1.0f;
		}
		return std::clamp(std::pow(a_damage / damageRef_, damageExp_), damageMin_, damageMax_);
	}

	void Engine::OnHitData(RE::Actor* a_target, const RE::HitData& a_hit)
	{
		if (!ready_ || a_target->IsPlayerRef()) {
			return;
		}
		LastHit h;
		h.when = std::chrono::steady_clock::now();
		h.from = a_hit.aggressor;
		h.at = a_hit.hitPosition;
		h.dir = a_hit.hitDirection;
		h.damage = a_hit.totalDamage;
		h.flags = a_hit.flags.underlying();
		h.exact = (h.at - a_target->GetPosition()).Length() < 200.0f;  // a point on her (some paths leave it empty)
		// an explosion's hit comes from the explosion itself (no flag, no damage, no point): never a melee blow
		const auto src = a_hit.sourceRef.get();
		auto*      srcBase = src ? src->GetBaseObject() : nullptr;
		const bool blast = srcBase && srcBase->GetFormType() == RE::FormType::Explosion;
		h.strike = blast ? 0.0f : StrikePush(a_target, a_hit, h.strikeDir);
		// the explosion's own hit has no damage: its spell's hit came just before it (Fireball's 40), kept
		float spellDamage = 0.0f;
		int   spellElement = -1;
		if (LastHit prev; blast && RecentHit(a_target->GetFormID(), 0.5f, prev)) {
			spellDamage = prev.damage;
			spellElement = prev.element;
		}
		if (!blast) {
			RecordHit(a_target->GetFormID(), h);
		}
		// a blow while Delirium has her (no stagger then: she is staggered already). Not the killing one: death plays that
		if (h.strike > 0.0f && a_target->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth) > a_hit.totalDamage) {
			Blow b;
			b.what = "blow";
			b.by = a_hit.aggressor.get();
			b.at = h.at;
			b.exact = h.exact;
			b.dir = { h.strikeDir.x, h.strikeDir.y, h.strikeDir.z };
			b.push = h.strike;
			b.canStart = false;
			b.melee = true;
			b.type = kBlowMelee;
			b.oomph = meleePush_ > 0.0f ? std::clamp((b.push / meleePush_ - meleeOomphFrom_) / (meleeOomphTo_ - meleeOomphFrom_), 0.0f, 1.0f) : 0.0f;
			b.note = fmt::format("{:.0f} damage, {}{}{}", a_hit.totalDamage, a_hit.weapon ? a_hit.weapon->GetName() : "no weapon",
				a_hit.flags.any(RE::HitData::Flag::kPowerAttack) ? ", power" : "", a_hit.flags.any(RE::HitData::Flag::kBash) ? ", bash" : "");
			React(a_target, b);
		}
		if (blast) {
			Blast(a_target, a_hit, static_cast<RE::Explosion*>(src.get()), srcBase->As<RE::BGSExplosion>(), spellDamage, spellElement);
		}
		// trace: what Skyrim's hits carry, and the object it came from (a projectile, an explosion)
		const auto by = a_hit.aggressor.get();
		const auto  srcPos = src ? src->GetPosition() : RE::NiPoint3{};
		logger::info("hitdata: {:08X} by {:08X} ('{}'), {:.0f} damage, stagger {:.2f}, flags {:X}, at {} ({:.0f} {:.0f} {:.0f}), dir {:.2f} {:.2f} {:.2f}; source {:08X} type {} '{}' at {:.0f} {:.0f} {:.0f}",
			a_target->GetFormID(), by ? by->GetFormID() : 0, a_hit.weapon ? a_hit.weapon->GetName() : "", a_hit.totalDamage, a_hit.stagger, h.flags,
			h.exact ? "on her" : "far", h.at.x, h.at.y, h.at.z, h.dir.x, h.dir.y, h.dir.z,
			src ? src->GetFormID() : 0, srcBase ? static_cast<int>(srcBase->GetFormType()) : -1, srcBase ? srcBase->GetName() : "",
			srcPos.x, srcPos.y, srcPos.z);
	}

	void Engine::Blast(RE::Actor* a_target, const RE::HitData& a_hit, RE::Explosion* a_ex, RE::BGSExplosion* a_base, float a_spellDamage, int a_spellElement)
	{
		if (!blasts_ || !a_ex || !a_base) {
			return;
		}
		// away from its centre (flat, a little up); at the centre itself (a direct hit) away from whoever cast it
		const RE::NiPoint3 c = a_ex->GetPosition();
		RE::NiPoint3       d = a_target->GetPosition() - c;
		d.z = 0.0f;
		const float dist = d.Length();
		auto        by = a_hit.aggressor.get();
		if (dist < 5.0f) {
			d = by && by.get() != a_target ? a_target->GetPosition() - by->GetPosition() : RE::NiPoint3{};
			d.z = 0.0f;
		}
		if (d.Length() < 1e-3f) {
			const float z = a_target->GetAngleZ();
			d = { -std::sin(z), -std::cos(z), 0.0f };  // backwards
		}
		d.Unitize();
		const float side = std::sqrt(1.0f - blastUp_ * blastUp_);
		const V3    dir{ d.x * side, d.y * side, blastUp_ };
		// by its damage (the one this explosion does, else its form's) and how near: full at the centre, fFarShare at the edge
		const auto& ex = a_ex->GetExplosionRuntimeData();  // its layout differs between SE and AE
		const float radius = std::max(1.0f, ex.radius > 0.0f ? ex.radius : a_base->data.radius);
		// its damage: the spell's that set it off, else the explosion's own, else its form's
		const float damage = std::clamp(a_spellDamage > 0.0f ? a_spellDamage : ex.damage > 0.0f ? ex.damage : a_base->data.damage, 0.0f, 1000.0f);
		const float closeness = std::clamp(1.0f - dist / radius, 0.0f, 1.0f);
		const float share = blastFar_ + (1.0f - blastFar_) * closeness;
		const float scale = DamageScale(a_target, damage) * share;
		Blow b;
		b.what = "blast";
		b.type = kBlowBlast;
		b.by = by;
		b.at = c;
		b.dir = dir;
		b.oomph = std::clamp((scale - oomphFrom_) / (oomphTo_ - oomphFrom_), 0.0f, 1.0f);
		b.push = blastPush_ * scale * (1.0f + oomphPush_ * b.oomph) * Rand(0.95f, 1.1f);
		b.join = true;
		// thrown: by the damage and how near, away from the centre and up
		const float v = std::min(flyMax_, flySpeed_ * scale * Rand(0.9f, 1.15f));
		if (v >= flyMin_) {
			b.fly = { d.x * v, d.y * v, v * flyUp_ };
		}
		// its element: the explosion's enchantment (Fireball's fire damage), by the resistance it goes against
		if (auto* ench = a_base->formEnchanting) {
			for (auto* e : ench->effects) {
				const auto* mgef = e ? e->baseEffect : nullptr;
				if (!mgef) {
					continue;
				}
				const auto rv = mgef->data.resistVariable;
				if (rv == RE::ActorValue::kResistFire) {
					b.element = kFire;
				} else if (rv == RE::ActorValue::kResistShock) {
					b.element = kShock;
				} else if (rv == RE::ActorValue::kResistFrost) {
					b.element = kFrost;
				}
				if (b.element >= 0) {
					break;
				}
			}
		}
		if (b.element < 0 && a_spellElement >= 0 && a_spellElement <= kShock) {
			b.element = a_spellElement;
		}
		b.note = fmt::format("'{}' {:.0f} damage x{:.2f}, {:.0f} of {:.0f} from its centre (x{:.2f}), force {:.0f}, oomph {:.2f}, thrown {:.1f} m/s",
			a_base->GetName(), damage, DamageScale(a_target, damage), dist, radius, share, a_base->data.force, b.oomph, v >= flyMin_ ? v : 0.0f);
		React(a_target, b);
	}

	float Engine::StrikePush(RE::Actor* a_target, const RE::HitData& a_hit, RE::NiPoint3& a_dir) const
	{
		using F = RE::HitData::Flag;
		const auto& fl = a_hit.flags;
		const auto* w = a_hit.weapon;
		auto        by = a_hit.aggressor.get();
		if (!melee_ || fl.any(F::kExplosion) || (w && (w->IsBow() || w->IsCrossbow() || w->IsStaff())) || !by || by.get() == a_target) {
			return 0.0f;
		}
		float kind = 1.0f;
		if (fl.any(F::kPowerAttack)) {
			kind *= meleePower_;
		}
		if (fl.any(F::kBash)) {
			kind *= meleeBash_;
		}
		if (fl.any(F::kSneakAttack)) {
			kind *= meleeSneak_;
		}
		if (fl.any(F::kBlocked)) {
			kind *= meleeBlocked_;
		}
		// a heavier weapon hits harder (fists and claws weigh nothing: half the reference)
		const float weight = w && w->weight > 0.0f ? w->weight : 0.5f * meleeWeightRef_;
		const float heft = std::clamp(std::sqrt(weight / meleeWeightRef_), meleeHeftMin_, meleeHeftMax_);
		// away from the attacker, bent sideways along the swing's sweep; an overhead chop also pushes down
		RE::NiPoint3 away = a_target->GetPosition() - by->GetPosition();
		away.z = 0.0f;
		if (away.Length() < 1.0f) {
			const float heading = a_target->data.angle.z;
			away = { -std::sin(heading), -std::cos(heading), 0.0f };
		}
		away.Unitize();
		RE::NiPoint3       d = away;
		const RE::NiPoint3 s = a_hit.hitDirection;
		const float        sl = s.Length();
		if (sl > 1.0f) {
			RE::NiPoint3 side(s.x, s.y, 0.0f);
			side -= away * side.Dot(away);  // only the sweep across her: the way in is always away
			d += side * (meleeSwing_ / sl);
			if (s.z < -0.7f * sl) {
				d.z = -meleeChop_;
			}
		}
		d.Unitize();
		a_dir = d;
		return meleePush_ * kind * heft * DamageScale(a_target, a_hit.totalDamage) * Rand(0.85f, 1.2f);
	}

	void Engine::OnStagger(RE::Actor* a_target, float a_magnitude, RE::Actor* a_source, bool a_started)
	{
		// held upright her stagger flag is ours, so Skyrim's stagger returns at once and never "starts": Delirium plays it
		const bool held = !a_started && a_magnitude > 0.0f && HoldsCombat(a_target);
		if (!ready_ || !staggers_ || (!a_started && !held) || a_target->IsPlayerRef()) {
			return;
		}
		// what Skyrim staggered with: after its perks and its cap of 1 (held: its graph never got this one)
		float mag = std::clamp(a_magnitude, 0.0f, 1.0f);
		if (!held) {
			a_target->GetGraphVariableFloat("staggerMagnitude", mag);
		}
		// a frame later: a spell's or shout's hit event comes after its stagger, and with it who cast it
		g_tasks->AddTask([h = a_target->GetHandle(), s = a_source ? a_source->GetHandle() : RE::ActorHandle{}, mag]() {
			if (auto t = h.get()) {
				auto src = s.get();
				Engine::Get().Stagger(t.get(), mag, src.get());
			}
		});
	}

	void Engine::Stagger(RE::Actor* a_target, float a_magnitude, RE::Actor* a_source)
	{
		if (!a_target) {
			return;
		}
		constexpr auto kExplosion = static_cast<std::uint32_t>(RE::HitData::Flag::kExplosion);
		// the way it pushes: away from where an explosion went off, from the attacker (the stagger's own, else her
		// last hit's), else straight back
		const auto   me = a_target->GetPosition();
		RE::NiPoint3 d;
		const char*  why = "from the attacker";
		LastHit      last;
		const bool   hit = RecentHit(a_target->GetFormID(), 0.5f, last);
		RE::ActorPtr by = a_source && a_source != a_target ? RE::ActorPtr(a_source) : RE::ActorPtr{};
		if (!by && hit) {
			if (auto from = last.from.get(); from && from.get() != a_target) {
				by = from;
			}
		}
		if (hit && last.exact && (last.flags & kExplosion)) {
			d = me - last.at;
			why = "from the explosion";
		} else if (by) {
			d = me - by->GetPosition();
			if (!a_source) {
				why = "from her last hit's attacker";
			}
		}
		d.z = 0.0f;
		if (d.Length() < 1.0f) {
			const float heading = a_target->data.angle.z;  // forward = (sin, cos)
			d = { -std::sin(heading), -std::cos(heading), 0.0f };
			why = "nothing to go by: back";
		}
		d.Unitize();
		Blow b;
		b.what = "stagger";
		b.type = kBlowStagger;
		b.stagger = true;
		b.by = by;
		b.dir = { d.x, d.y, 0.0f };
		const float scale = hit ? DamageScale(a_target, last.damage) : 1.0f;
		b.push = staggerPush_ * (staggerMin_ + (1.0f - staggerMin_) * a_magnitude) * scale * Rand(0.9f, 1.1f);
		// a melee blow that staggers: as hard as the blow (attack, weapon, damage), more for a bigger stagger, along its swing
		if (hit && last.strike > 0.0f) {
			b.push = std::max(b.push, last.strike * (1.0f + staggerHitBoost_ * a_magnitude));
			b.dir = { last.strikeDir.x, last.strikeDir.y, last.strikeDir.z };
			why = "the blow";
			b.melee = true;
			b.type = kBlowMelee;
			b.oomph = meleePush_ > 0.0f ? std::clamp((last.strike / meleePush_ - meleeOomphFrom_) / (meleeOomphTo_ - meleeOomphFrom_), 0.0f, 1.0f) : 0.0f;
		}
		// where the blow landed if it was on her (a sword's edge, a shield bash), else the chest
		b.exact = hit && last.exact && !(last.flags & kExplosion);
		b.at = last.at;
		if (hit && last.shout && !b.melee) {
			// a shout: its own rows, one per word (stand.shout.word1-3: push, throw). The words from the spell that hit
			// (its shout's variation), else the shouter's shout level, else a guess from the magnitude, which is no measure
			// of them: it falls off with distance (one Fus Ro Dah came in at 0.48 to 1.00)
			b.what = "shout";
			b.type = kBlowShout;
			b.oomph = a_magnitude;
			const char* from = "the spell";
			b.word = last.word;
			if (last.element == kFire || last.element == kFrost || last.element == kShock) {
				b.element = last.element;  // a fire / frost / shock shout that staggers: that reaction on top
			}
			if (!b.word) {
				from = "a guess from the magnitude";
				b.word = a_magnitude < 0.55f ? 1 : a_magnitude < 0.86f ? 2 : 3;
				if (const auto* proc = by ? by->GetActorRuntimeData().currentProcess : nullptr; proc && proc->high && proc->high->currentShout) {
					if (const auto v = static_cast<std::uint32_t>(proc->high->currentShoutVariation); v < 3) {
						b.word = static_cast<int>(v) + 1;
						from = "the shouter's level";
					}
				}
			}
			b.knocks = last.stagger >= kKnockStagger;
			b.note = fmt::format("{} word{} ({}), stagger {:.2f}{}, ", b.word, b.word > 1 ? "s" : "", from, last.stagger, b.knocks ? " (knocks down)" : "");
		}
		b.note += fmt::format("magnitude {:.2f}, {}, {:.0f} damage x{:.2f}, oomph {:.2f}", a_magnitude, why, hit ? last.damage : 0.0f, scale, b.oomph);
		React(a_target, b);
	}

	bool Engine::OnBump(RE::Actor* a_target, RE::Actor* a_bumper)
	{
		// on her feet only (Skyrim doesn't bump a ragdoll; a falmer's balance can't take a step): else Skyrim's stumble
		if (!ready_ || !bumps_ || !Eligible(a_target, "bump", true) || a_target->IsInRagdollState() ||
			a_target->AsActorState()->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal || Hunched(a_target)) {
			return false;
		}
		// Skyrim bumps every frame the two touch: one push per run-in, the frames after it only keep its stumble off
		{
			const auto       now = std::chrono::steady_clock::now();
			std::scoped_lock l(bumpLock_);
			auto&            last = bumpAt_[a_target->GetFormID()];
			const bool       again = now - last < std::chrono::duration<float>(bumpRepeat_);
			last = now;
			if (bumpAt_.size() > 64) {
				std::erase_if(bumpAt_, [&](const auto& p) { return now - p.second > std::chrono::seconds(10); });
			}
			if (again) {
				return true;
			}
		}
		g_tasks->AddTask([h = a_target->GetHandle(), s = a_bumper ? a_bumper->GetHandle() : RE::ActorHandle{}]() {
			if (auto t = h.get()) {
				auto by = s.get();
				Engine::Get().Bump(t.get(), by.get());
			}
		});
		return true;
	}

	void Engine::Bump(RE::Actor* a_target, RE::Actor* a_bumper)
	{
		// along the bumper's run (flat), else away from them, else back
		RE::NiPoint3 d, run;
		const char*  why = "back";
		if (a_bumper && a_bumper != a_target) {
			a_bumper->GetLinearVelocity(run);
			run.z = 0.0f;
			if (run.Length() > 50.0f) {
				d = run;
				why = "along the run";
			} else {
				d = a_target->GetPosition() - a_bumper->GetPosition();
				d.z = 0.0f;
				why = "from the bumper";
			}
		}
		if (d.Length() < 1.0f) {
			const float heading = a_target->data.angle.z;
			d = { -std::sin(heading), -std::cos(heading), 0.0f };
		}
		d.Unitize();
		Blow b;
		b.what = "bump";
		b.type = kBlowBump;
		b.by = a_bumper && a_bumper != a_target ? RE::ActorPtr(a_bumper) : RE::ActorPtr{};
		b.dir = { d.x, d.y, 0.0f };
		b.push = bumpPush_ * Rand(0.9f, 1.1f);
		b.note = fmt::format("{:08X} at {:.0f} units/s, {}", a_bumper ? a_bumper->GetFormID() : 0, run.Length(), why);
		React(a_target, b);
	}

}

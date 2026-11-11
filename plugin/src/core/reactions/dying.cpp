// Deaths: the killing blow, dying on her feet in Delirium, the corpse going limp.
//
// What Skyrim does with a kill (KillImpl, read in IDA): AI off, death events, then either the ragdoll at once
// ("instant ragdoll") or first a canned death animation if her behaviour graph has one for the moment; once a ragdoll
// it is pushed by the killing hit (stored on her process), else carries her movement, else gets a nudge forward.
//
// [Deaths] bDieAtOnce (the default): the killing blow kills now, with the instant ragdoll (no canned animation can fight
// the pose). Delirium has her on her feet and plays the stumble; Skyrim's ragdoll is pinned to Delirium's pose (the
// lead drive, body/drive.cpp, kept on: each step every body gets the velocity that lands it on Delirium's pose) until
// she is down, then she dies in Delirium as she lies (update.cpp). Off: the old way, her death held back at 1 health
// until she collapses (held_).
#include "shared.h"

namespace delirium
{
	namespace
	{
		constexpr std::array<int, 6> kUpperParts{ 8, 9, 10, 11, 13, 17 };  // spine1-3, neck, clavicles: where a killing blow lands
		constexpr float              kHeldHealth = 150.0f;  // the killing blow on her feet: the shot task's "stays up, fights it" health

		// who is dead on her feet: read by Eligible and the hit code outside lock_
		std::mutex              g_pinLock;
		std::vector<RE::FormID> g_pinned;
		std::atomic<int>        g_pinnedCount{ 0 };

		void SetPinned(RE::FormID a_id, bool a_on)
		{
			std::scoped_lock l(g_pinLock);
			std::erase(g_pinned, a_id);
			if (a_on) {
				g_pinned.push_back(a_id);
			}
			g_pinnedCount = static_cast<int>(g_pinned.size());
		}

		// the kill we make ourselves comes back through the kill hook on the same thread: let through. Per thread,
		// Skyrim deals blows from its job threads too, several at once
		thread_local const RE::Actor* t_ourKill = nullptr;

		void Kill(RE::Actor* a_actor, RE::Actor* a_attacker, float a_damage, bool a_sendEvent, bool a_ragdollInstant)
		{
			t_ourKill = a_actor;
			a_actor->KillImpl(a_attacker, a_damage, a_sendEvent, a_ragdollInstant);
			t_ourKill = nullptr;
		}
	}

	bool DeadOnFeet(const RE::Actor* a_actor)
	{
		if (!a_actor || g_pinnedCount.load(std::memory_order_relaxed) == 0) {
			return false;
		}
		std::scoped_lock l(g_pinLock);
		return std::ranges::find(g_pinned, a_actor->GetFormID()) != g_pinned.end();
	}

	bool Engine::Dying(const RE::Actor* a_actor) const
	{
		return held_.contains(a_actor->GetFormID()) || DeadOnFeet(a_actor);
	}

	bool Engine::OnKill(RE::Actor* a_actor, RE::Actor* a_attacker, float a_damage, bool a_sendEvent, bool a_ragdollInstant)
	{
		if (!a_actor || a_actor == t_ourKill || dyingTime_ <= 0.0f || !ready_ || a_actor->IsPlayerRef()) {
			return false;
		}
		const auto id = a_actor->GetFormID();
		if (dieAtOnce_) {
			bool held;
			{
				std::scoped_lock l(lock_);
				held = held_.contains(id);  // held the old way before the setting changed: ends the old way
			}
			if (!held) {
				return DieOnFeet(a_actor, a_attacker, a_damage, a_sendEvent);  // kills outside lock_, as KillNow does
			}
		}
		std::scoped_lock l(lock_);
		const auto it = held_.find(id);
		if (it == held_.end()) {
			// people on their feet only: one already in a ragdoll (knocked down, paralysed) dies as she lies
			if (!Eligible(a_actor, "killing blow", false)) {
				return false;
			}
			if (a_actor->IsInRagdollState() || a_actor->IsInKillMove()) {
				logger::info("{:08X}: killing blow in a ragdoll or killmove: dies at once", id);
				return false;
			}
		}
		// she lives on at 1 health (blows during it hit the hold, not her life)
		auto*       av = a_actor->AsActorValueOwner();
		const float hp = av->GetActorValue(RE::ActorValue::kHealth);
		if (hp < 1.0f) {
			av->RestoreActorValue(RE::ActorValue::kHealth, 1.0f - hp);
		}
		if (it != held_.end()) {
			if (a_attacker) {
				it->second.attacker = a_attacker->GetHandle();
			}
			return true;
		}
		held_[id] = Held{ a_actor->GetHandle(), a_attacker ? a_attacker->GetHandle() : RE::ActorHandle{}, a_damage, a_sendEvent, a_ragdollInstant,
			std::chrono::steady_clock::now() };
		g_held = static_cast<int>(held_.size());
		logger::info("{:08X}: killing blow ({:.0f} damage, health {:.0f} of {:.0f}, by {:08X}{}): dying on her feet, up to {:.1f} s", id, a_damage, hp,
			av->GetPermanentActorValue(RE::ActorValue::kHealth), a_attacker ? a_attacker->GetFormID() : 0, a_ragdollInstant ? ", instant ragdoll" : "",
			dyingTime_);
		g_tasks->AddTask([h = a_actor->GetHandle()]() {
			if (auto t = h.get()) {
				Engine::Get().DyingOnFeet(t.get());
			}
		});
		return true;
	}

	void Engine::DyingOnFeet(RE::Actor* a_actor)
	{
		std::scoped_lock l(lock_);
		const auto it = held_.find(a_actor->GetFormID());
		if (it == held_.end() || a_actor->IsDead()) {
			return;
		}
		Active* act = nullptr;
		for (auto& p : active_) {
			if (p->actor.get().get() == a_actor) {
				act = p.get();
			}
		}
		if (!act) {
			if (!Activate(a_actor, false)) {
				logger::info("{:08X}: dying on her feet: Delirium can't take her, dies now", a_actor->GetFormID());
				it->second.at -= std::chrono::seconds(60);
				return;
			}
			act = active_.back().get();
		}
		// her hit's own reaction already plays it (React, wounded at the dying health), or she is down already
		if (act->heldDeath || act->knockQueued || act->death) {
			act->heldDeath = true;
			return;
		}
		if (act->flying > 0) {
			act->heldDeath = true;
			logger::info("{:08X}: dying on her feet: thrown, the throw plays on, she dies once down", a_actor->GetFormID());
			return;
		}
		act->heldDeath = true;
		// no reaction from a blow (a burn, a fall, the console): the killing blow as a wounded shot, from the killer
		auto k = it->second.attacker.get();
		StartShot(*act, a_actor, k.get());
	}

	bool Engine::DieOnFeet(RE::Actor* a_actor, RE::Actor* a_attacker, float a_damage, bool a_sendEvent)
	{
		// on the thread the blow came on (Skyrim's main thread or one of its job threads), where Skyrim's own kill
		// would have run
		const auto id = a_actor->GetFormID();
		bool       pin = true;
		{
			std::scoped_lock l(lock_);
			Active* act = nullptr;
			for (auto& p : active_) {
				if (p->actor.get().get() == a_actor) {
					act = p.get();
				}
			}
			if (act) {
				// down, thrown or dying already: she dies as it plays (update.cpp), only without a canned death animation
				pin = !(act->knockQueued || act->death || act->flying > 0 || act->takeover || act->pinned);
			} else {
				// people on their feet only: one already in a ragdoll (knocked down, paralysed) dies as she lies
				if (!Eligible(a_actor, "killing blow", false)) {
					return false;
				}
				if (a_actor->IsInRagdollState() || a_actor->IsInKillMove()) {
					logger::info("{:08X}: killing blow in a ragdoll or killmove: dies at once", id);
					return false;
				}
				if (!Activate(a_actor, false)) {
					logger::info("{:08X}: killing blow: Delirium can't take her, Skyrim's own death", id);
					return false;
				}
				act = active_.back().get();
			}
			if (pin) {
				act->pinned = true;
				act->pinUntil = act->age + dyingTime_;
				act->pinId = id;
				SetPinned(id, true);
			}
			logger::info("{:08X}: killing blow ({:.0f} damage, by {:08X}): dies now{}", id, a_damage, a_attacker ? a_attacker->GetFormID() : 0,
				pin ? fmt::format(", on her feet in Delirium (Skyrim's ragdoll pinned to her, up to {:.1f} s)", dyingTime_) : std::string(", as she is"));
		}
		Kill(a_actor, a_attacker, a_damage, a_sendEvent, true);  // instant ragdoll: no canned death animation
		if (!a_actor->IsDead()) {
			logger::info("{:08X}: still alive after the kill? (essential, a script)", id);
			std::scoped_lock l(lock_);
			for (auto& p : active_) {
				if (p->actor.get().get() == a_actor) {
					Unpin(*p, "not dead");
				}
			}
			return true;
		}
		if (pin) {
			// the blow's own reaction comes after the kill (React, as dying); none (a burn, a fall, the console): a shot
			g_tasks->AddTask([h = a_actor->GetHandle(), k = a_attacker ? a_attacker->GetHandle() : RE::ActorHandle{}]() {
				if (auto t = h.get()) {
					auto killer = k.get();
					Engine::Get().StumbleShot(t.get(), killer.get());
				}
			});
		}
		return true;
	}

	void Engine::StumbleShot(RE::Actor* a_actor, RE::Actor* a_killer)
	{
		std::scoped_lock l(lock_);
		for (auto& p : active_) {
			if (p->actor.get().get() == a_actor && p->pinned && !p->heldDeath) {
				p->heldDeath = true;
				StartShot(*p, a_actor, a_killer);
			}
		}
	}

	void Engine::StartShot(Active& a, RE::Actor* a_actor, RE::Actor* a_killer)
	{
		float cp[3]{}, cq[4]{};
		dl.nm_agent_get_part_transform(a.agent, kChest, cp, cq);
		V3 dir{};
		if (a_killer) {
			const RE::NiPoint3 kp = (a_killer->GetPosition() - a.origin) * kUnit;
			dir = { cp[0] - kp.x, cp[1] - kp.y, 0.0f };
		}
		if (dir.x * dir.x + dir.y * dir.y < 1e-4f) {
			const float h = a_actor->GetAngleZ();
			dir = { -std::sin(h), -std::cos(h), 0.0f };  // from the front
		}
		const float ln = std::sqrt(dir.x * dir.x + dir.y * dir.y);
		const float d[3]{ dir.x / ln, dir.y / ln, 0.0f };
		const int   part = kUpperParts[std::uniform_int_distribution<int>(0, 5)(g_rng)];
		float       hp[3]{}, hq[4]{};
		dl.nm_agent_get_part_transform(a.agent, part, hp, hq);
		std::string key;
		const Rule* row = FindRule({ "stand.lethal" }, key);
		const bool  collapse = !row || row->Has("collapse");
		if (dl.nm_agent_task_shot(a.agent, a.world, nullptr, part, hp, d, row ? row->Value("shot", kHeldHealth) : kHeldHealth) < 0) {
			logger::error("  shot task: {}", LastError());
		} else {
			a.collapseAt = collapse ? a.age + Rand(collapseMin_, collapseMax_) : -1.0f;
			logger::info("{:08X}: dying on her feet: shot task on part {}, collapsing in {:.1f} s", a_actor->GetFormID(), part, a.collapseAt - a.age);
		}
	}

	void Engine::Unpin(Active& a, const char* a_why)
	{
		if (!a.pinned) {
			return;
		}
		a.pinned = false;
		SetPinned(a.pinId, false);
		logger::info("{:08X}: dead on her feet no more ({}), {:.1f} s pinned: Delirium follows Skyrim's ragdoll", a.pinId, a_why,
			a.age - (a.pinUntil - dyingTime_));
	}

	void Engine::CheckHeld(RE::Actor* a_actor)
	{
		Held h;
		{
			std::scoped_lock l(lock_);
			const auto it = held_.find(a_actor->GetFormID());
			if (it == held_.end()) {
				return;
			}
			const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - it->second.at).count();
			Active*     act = nullptr;
			for (auto& p : active_) {
				if (p->actor.get().get() == a_actor) {
					act = p.get();
				}
			}
			const bool  down = act && act->knockQueued;
			const bool  reacting = down && (act->writheAt >= 0.0f || act->writheUntil > act->age);  // writhing, convulsing, burning on the ground
			const char* why = t >= dyingTime_ ? "her time is up" :
			                  down && !reacting && t >= 1.5f ? "she is down" :
			                  !act && t >= 0.5f      ? "Delirium let her go" :
			                                           nullptr;
			if (!why) {
				return;
			}
			h = it->second;
			held_.erase(it);
			g_held = static_cast<int>(held_.size());
			h.why = why;
		}
		g_tasks->AddTask([h]() { Engine::Get().KillNow(h, h.why); });
	}

	void Engine::KillHeld(const char* a_why)
	{
		std::vector<Held> all;
		{
			std::scoped_lock l(lock_);
			for (auto& [id, h] : held_) {
				all.push_back(h);
			}
			held_.clear();
			g_held = 0;
		}
		for (const auto& h : all) {
			KillNow(h, a_why);
		}
	}

	void Engine::KillNow(const Held& a_held, const char* a_why)
	{
		auto t = a_held.actor.get();
		if (!t || t->IsDead()) {
			return;
		}
		auto att = a_held.attacker.get();
		{
			std::scoped_lock l(lock_);
			heldDied_.insert(t->GetFormID());
		}
		const float s = std::chrono::duration<float>(std::chrono::steady_clock::now() - a_held.at).count();
		Kill(t.get(), att.get(), a_held.damage, a_held.send, a_held.instant);
		logger::info("{:08X}: dies now ({}), {:.1f} s after the killing blow{}", t->GetFormID(), a_why, s, t->IsDead() ? "" : " (still alive?)");
	}

	void Engine::StartDying(Active& a, const V3& a_moving, bool a_held)
	{
		// the reaction table: die.held(.element) after dying on her feet, else die.element, die.type, die (her last blow);
		// by default from [Deaths]
		const std::string el = ElementName(a.lastElement);
		std::vector<std::string> keys;
		if (a_held) {
			if (!el.empty()) {
				keys.push_back("die.held." + el);
			}
			keys.push_back("die.held");
		} else {
			if (!el.empty()) {
				keys.push_back("die." + el);
			}
			if (a.lastType >= 0) {
				keys.push_back(std::string("die.") + TypeName(a.lastType));
			}
			keys.push_back("die");
		}
		std::string key = "([Deaths])";
		Rule        rule;
		bool        found = false;
		for (const auto& k : keys) {
			if (auto it = rules_.find(k); it != rules_.end()) {
				rule = it->second;
				key = k;
				found = true;
				break;
			}
		}
		if (!found) {
			rule = ParseRule(a_held ? fmt::format("writhe:{}", dyingWrithe_) :
			                          fmt::format("shot:{}, push, catch:{}, writhe:{}", dyingHealth_, dyingCatch_, dyingWrithe_));
		}
		const auto chance = [&](std::string_view n) { return rule.Has(n) ? rule.Value(n, 1.0f) : 0.0f; };
		a.dieAmp = rule.Value("amp", -1.0f);
		a.diePeriod = rule.Value("period", -1.0f);
		logger::info("  dying row [{}: {}]", key, rule.text);
		if (a_held) {
			// she already took the blow on her feet: no new shot or push, her reaction runs on into the fall, then limp
			a.dyingAt = a.age - dyingReact_;
			a.writhe = Rand(0.0f, 1.0f) < chance("writhe");
			a.relaxAt = a.age + Rand(0.8f, 1.6f);
			logger::info("  dying plan (after dying on her feet): {}limp after {:.1f} s", a.writhe ? "writhe, then " : "", a.relaxAt - a.age);
			return;
		}
		// the killing blow as a shot at no health: Delirium's own hit reaction (stagger, clutch, collapse). From the
		// killer to the chest, else along the way the body already moves, else from the front
		float cp[3]{}, cq[4]{};
		dl.nm_agent_get_part_transform(a.agent, kChest, cp, cq);
		V3          dir = a_moving;
		std::string from = "its own motion";
		if (auto self = a.actor.get()) {
			if (auto killer = self->GetActorRuntimeData().myKiller.get()) {
				const RE::NiPoint3 k = (killer->GetPosition() - a.origin) * kUnit;
				dir = { cp[0] - k.x, cp[1] - k.y, 0.0f };
				from = "the killer";
			}
		}
		if (dir.x * dir.x + dir.y * dir.y < 1e-4f) {
			dir = { 0.0f, 0.0f, 0.0f };
			from = "nowhere (no motion, no killer)";
		} else {
			// up to 25 deg either way, so every corpse doesn't fall the same way
			const float t = Rand(-0.44f, 0.44f), cs = std::cos(t), sn = std::sin(t);
			dir = { dir.x * cs - dir.y * sn, dir.x * sn + dir.y * cs, 0.0f };
			const float l = std::sqrt(dir.x * dir.x + dir.y * dir.y);
			dir = { dir.x / l, dir.y / l, 0.0f };
		}
		// hit somewhere on the upper body: spine1-3, neck or a clavicle
		constexpr std::array<int, 6> kHitParts{ 8, 9, 10, 11, 13, 17 };
		int part = kHitParts[std::uniform_int_distribution<int>(0, 5)(g_rng)];
		float hp[3]{}, hq[4]{};
		dl.nm_agent_get_part_transform(a.agent, part, hp, hq);
		// killed by an arrow: where it went in and the way it flew
		if (auto self = a.actor.get()) {
			RE::NiPoint3 at, vel;
			if (RecentImpact(self.get(), 1.5f, at, vel) && vel.Length() > 1.0f) {
				const RE::NiPoint3 h = (at - a.origin) * kUnit;
				hp[0] = h.x;
				hp[1] = h.y;
				hp[2] = h.z;
				part = NearestPart(a, hp);
				from = fmt::format("{}, where it went in", from);
			}
		}
		a.dyingAt = a.age;
		const float push = deathPush_ * (rule.Has("push") ? rule.Value("push", 1.0f) : 0.0f);
		if (push > 0.0f && (dir.x != 0.0f || dir.y != 0.0f)) {
			float       comp[3]{}, comv[3]{};
			dl.nm_agent_get_com(a.agent, comp, comv);
			const float along = comv[0] * dir.x + comv[1] * dir.y;
			const V3    perp{ comv[0] - dir.x * along, comv[1] - dir.y * along, 0.0f };
			const float sp = std::max(push * Rand(0.8f, 1.2f), along);  // running away from him: she keeps it
			const V3    shift{ dir.x * sp + perp.x * 0.3f - comv[0], dir.y * sp + perp.y * 0.3f - comv[1], 0.0f };
			if (a.ragdolled || a.takeover) {
				a.kicks.push_back({ -1, shift, {} });
				a.speedCap = deathMaxSpeed_;
			} else {
				const float imp[3]{ shift.x * 60.0f, shift.y * 60.0f, 0.0f };  // about her weight
				dl.nm_agent_apply_push(a.agent, cp, imp);
			}
			logger::info("  dying: moving {:.2f} {:.2f} -> {:.2f} {:.2f} m/s (away from {}, {:.1f} m/s)", comv[0], comv[1], comv[0] + shift.x,
				comv[1] + shift.y, from, sp);
		}
		const float d[3]{ dir.x, dir.y, dir.z };
		// badly wounded, not dead on the spot: at no health the shot relaxes every muscle in 0.5 s (the flop). Wounded, she
		// reaches for the wound and her muscles fade over the fall
		if (rule.base == "shot") {
			const float health = rule.Value("shot", dyingHealth_);
			if (dl.nm_agent_task_shot(a.agent, a.world, nullptr, part, hp, d, health) < 0) {
				logger::error("  shot task: {}", LastError());
			} else {
				logger::info("  dying: shot task at health {:.0f} on part {}, hit from {}", health, part, from);
			}
		}
		// now and then the arms go out to break the fall (the shot only catches it at full health)
		const bool catches = Rand(0.0f, 1.0f) < chance("catch");
		if (catches) {
			dl.nm_msg_begin(a.agent, "catchFall");
			dl.nm_msg_set_bool(a.agent, "start", 1);
			dl.nm_msg_send(a.agent, a.world);
		}
		// then: a weak writhe on the ground now and then, and limp after it
		a.writhe = Rand(0.0f, 1.0f) < chance("writhe");
		a.relaxAt = a.age + Rand(limpMin_, limpMax_);
		logger::info("  dying plan: {}{}limp after {:.1f} s", catches ? "breaking the fall, " : "", a.writhe ? "writhe, then " : "", a.relaxAt - a.age);
	}

}

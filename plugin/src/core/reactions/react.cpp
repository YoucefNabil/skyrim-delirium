// What a blow does to her: on her feet, thrown, down; channels, the elements, Skyrim's own knock.
#include "shared.h"

namespace delirium
{
	void Engine::React(RE::Actor* a_target, const Blow& a_blow)
	{
		// a blow that may not bring her in: nothing to do when no one is in Delirium
		if (!a_blow.canStart && g_count.load(std::memory_order_relaxed) == 0) {
			return;
		}
		if (!Eligible(a_target, a_blow.what, !a_blow.shot)) {
			return;
		}
		std::scoped_lock l(lock_);
		Active*          act = nullptr;
		for (auto& p : active_) {
			if (p->actor.get().get() == a_target) {
				act = p.get();
			}
		}
		if (act && (act->knockQueued || act->death)) {
			DownHit(*act, a_target, a_blow);
			return;
		}
		if (!act) {
			if (!a_blow.canStart) {
				return;  // Skyrim's own hit reaction plays
			}
			if (a_target->IsInRagdollState()) {
				using K = RE::KNOCK_STATE_ENUM;
				const auto ks = a_target->AsActorState()->GetKnockState();
				const bool letGo = releasedAt_.contains(a_target->GetFormID());
				if (letGo && !a_target->IsDead() && ks != K::kNormal && ks != K::kQueued && ks != K::kGetUp &&
					a_target->AsActorValueOwner()->GetActorValue(RE::ActorValue::kParalysis) <= 0.0f) {
					std::erase_if(skip_, [&](const RE::ActorHandle& h) {
						auto x = h.get();
						return !x || x.get() == a_target;
					});
					g_skip = static_cast<int>(skip_.size());
					logger::info("{:08X}: {} while lying in Skyrim's ragdoll (knock state {}): taken back", a_target->GetFormID(), a_blow.what, static_cast<int>(ks));
					if (Activate(a_target, true)) {
						DownHit(*active_.back(), a_target, a_blow);
					}
					return;
				}
				if (a_blow.shot) {
					logger::info("{}: skipped {:08X} (in Skyrim's own ragdoll, knock state {})", a_blow.what, a_target->GetFormID(), static_cast<int>(ks));
				}
				return;  // Skyrim's own ragdoll, not ours (its get-up, paralysis)
			}
			if (a_blow.startChance < 1.0f && !held_.contains(a_target->GetFormID())) {
				if (Rand(0.0f, 1.0f) >= a_blow.startChance) {
					logger::info("{:08X}: {} doesn't bring her in ({:.0f} % chance): Skyrim's own hit", a_target->GetFormID(), a_blow.what, a_blow.startChance * 100.0f);
					return;
				}
				logger::info("{:08X}: {} brings her in ({:.0f} % chance)", a_target->GetFormID(), a_blow.what, a_blow.startChance * 100.0f);
			}
			// a falmer can't keep her feet in Delirium (its balance is a person's): only a blow that puts her down brings
			// her in (thrown, a knockdown, a killing blow); the rest is Skyrim's own stagger
			if (Hunched(a_target)) {
				const bool  held = held_.contains(a_target->GetFormID());
				std::string key;
				const Rule& rule = PickRule("stand", a_blow, held, key);
				const bool  thrown = rule.Has("throw") && rule.Value("throw", 6.0f) > 0.0f && (a_blow.type != kBlowShout || a_blow.knocks);
				const bool  down = held || a_blow.knocks || a_blow.type == kBlowKnock || thrown || a_blow.fly.x != 0.0f || a_blow.fly.y != 0.0f || a_blow.fly.z != 0.0f;
				if (!down) {
					logger::info("{:08X}: {} doesn't put her down ([{}]): a falmer, Skyrim's own hit", a_target->GetFormID(), a_blow.what, key);
					return;
				}
			}
			if (!Activate(a_target)) {
				return;
			}
			act = active_.back().get();
		}
		ReactOn(*act, a_target, a_blow);
	}

	void Engine::ReactOn(Active& a, RE::Actor* a_actor, const Blow& b)
	{
		a.lastType = b.type;
		a.lastElement = b.element;
		// where it landed if that was on her, else the chest
		float at[3]{}, q[4]{};
		int   part = kChest;
		if (b.exact) {
			const RE::NiPoint3 h = (b.at - a.origin) * kUnit;
			at[0] = h.x;
			at[1] = h.y;
			at[2] = h.z;
			part = NearestPart(a, at);
		} else {
			dl.nm_agent_get_part_transform(a.agent, kChest, at, q);
		}
		float      look[3]{};
		const bool sees = b.by && b.by.get() != a_actor;
		if (sees) {
			LookAt(a, b.by.get(), look);
		}
		if (b.fly.x != 0.0f || b.fly.y != 0.0f || b.fly.z != 0.0f) {
			Fly(a, a_actor, b);
			return;
		}
		if (a.flying > 0 && (!a.knockQueued || a.takeover)) {
			// in the air: the throw's behaviours play on, the blow only adds its push (a killing blow too: she dies once down)
			if (held_.contains(a_actor->GetFormID())) {
				a.heldDeath = true;
			}
			const float added = AddPush(a, at, b.dir, std::min(b.push, b.melee ? meleeMaxPush_ : hitMaxPush_));
			logger::info("{:08X}: {} in the air: push {:.0f} N s added, the throw plays on{} ({})", a_actor->GetFormID(), b.what, added,
				a.heldDeath ? ", dying" : "", b.note);
			return;
		}
		if (b.join && a.age - a.lastPushAt < 0.3f) {
			const float added = AddPush(a, at, b.dir, b.push);
			logger::info("{:08X}: {} with the reaction just started: push {:.0f} N s added (wanted {:.0f}; {})", a_actor->GetFormID(), b.what, added,
				b.push, b.note);
			return;
		}
		// the reaction, from what the blow is and how hard: a projectile went in (GTA's shot task: health on its scale,
		// 100 and up stays on its feet and catches a fall, under 100 is hurt and falls); else a light blow flinches (guard
		// up, turned to whoever did it) and a harder one makes her step to catch it, looking at them. Past what the steps
		// can catch she falls (the knock, as for any push). Each task stops every behaviour first.
		std::string how;
		float       health = 0.0f;
		int         r;
		// a projectile, or a blow past a flinch: Delirium's shot task, wounded where it landed and along it, fighting to
		// stay on her feet (a blow as GTA's melee set)
		const bool held = held_.contains(a_actor->GetFormID());  // the killing blow, held back: she dies on her feet
		if (held) {
			a.heldDeath = true;
			if (a.collapseAt < 0.0f) {
				a.collapseAt = a.age + Rand(collapseMin_, collapseMax_);
			}
		}
		// the reaction table: this blow's row (phase, type, strength), the element's layers on top
		std::string key;
		Rule        rule = PickRule("stand", b, held, key);
		{
			const Rule el = ElementRule(b.element);
			rule.layers.insert(rule.layers.end(), el.layers.begin(), el.layers.end());
		}
		// throw:m/s (the row or the element's): thrown along the blow as an explosion throws (fly rows, up by fFlyUp)
		// a shout's only if it knocks down in Skyrim (Fus Ro Dah, a mod's as strong): the others' last word stays standing
		if (b.type == kBlowShout && !b.knocks && rule.Has("throw")) {
			logger::info("{:08X}: [{}] throws, but this shout doesn't knock down (stagger under {:.1f}): not thrown", a_actor->GetFormID(), key, kKnockStagger);
		} else if (const float v = rule.Value("throw", 6.0f); rule.Has("throw") && v > 0.0f) {
			const float fl = std::sqrt(b.dir.x * b.dir.x + b.dir.y * b.dir.y);
			if (fl > 1e-3f) {
				Blow t = b;
				t.fly = { b.dir.x / fl * v, b.dir.y / fl * v, flyUp_ * v };
				t.oomph = std::max(b.oomph, std::clamp(v / std::max(flyMax_, 1e-3f), 0.0f, 1.0f));
				t.note = fmt::format("{}; thrown at {:.1f} m/s by [{}]", b.note, v, key);
				Fly(a, a_actor, t);
				return;
			}
		}
		if (held && !rule.Has("collapse")) {
			a.collapseAt = -1.0f;  // the row says she doesn't go down by herself: her time runs out on her feet
		}
		const bool asShot = rule.base == "shot";
		if (asShot) {
			auto*       av = a_actor->AsActorValueOwner();
			const float full = av->GetPermanentActorValue(RE::ActorValue::kHealth);
			const float share = full > 0.0f ? std::clamp(av->GetActorValue(RE::ActorValue::kHealth) / full, 0.0f, 1.0f) : 1.0f;
			health = rule.Value("shot", share >= wounded_ ? 100.0f + 100.0f * share : 50.0f + 49.0f * share / std::max(wounded_, 1e-3f));
			const float d[3]{ b.dir.x, b.dir.y, b.dir.z };
			r = dl.nm_agent_task_shot(a.agent, a.world, b.melee ? "MELEE" : nullptr, part, at, d, health);
			how = b.melee ? "wounded (shot task, melee)" : "shot";
			if (held) {
				how += ", dying on her feet";
			}
			if (r >= 0 && b.oomph > 0.0f && rule.Has("spin")) {
				// a harder hit jolts harder: Delirium's shot twists the spine on impact (its shock spin, on any part now), read
				// on the shot's first tick. No lean on top: any leanInDirection with the shot takes her off her feet (fit-test
				// react: lean 0.15 falls at 2.5 s, 1.2 at once; without it she stays up to a 200 N s push)
				const float k = 1.0f + oomphSpin_ * b.oomph;
				dl.nm_msg_begin(a.agent, "shot");
				dl.nm_msg_set_bool(a.agent, "alwaysAddShockSpin", 1);
				dl.nm_msg_set_float(a.agent, "shockSpinMin", 40.0f * k);
				dl.nm_msg_set_float(a.agent, "shockSpinMax", 80.0f * k);
				if (dl.nm_msg_send(a.agent, a.world) < 0) {
					logger::error("  shot oomph: {}", LastError());
				}
			}
			float pp[3]{}, pq[4]{};
			dl.nm_agent_get_part_transform(a.agent, 0, pp, pq);
			const float fl = std::sqrt(b.dir.x * b.dir.x + b.dir.y * b.dir.y);
			a.shotPelvis = { pp[0], pp[1], pp[2] };
			a.shotDir = fl > 1e-3f ? V3{ b.dir.x / fl, b.dir.y / fl, 0.0f } : V3{};
			a.haveShot = fl > 1e-3f;
		} else if (rule.base == "flinch" && sees) {
			r = dl.nm_agent_task_flinch(a.agent, a.world, nullptr, look);
			how = "flinch";
		} else if (rule.base == "none") {
			r = 0;
			how = "no behaviour, the push only";
		} else {
			r = dl.nm_agent_task_balance(a.agent, a.world, nullptr, sees ? look : nullptr);
			how = "step";
		}
		// the push at most: past it one shove knocks her over limp before anything plays; the behaviours carry the rest.
		// A stagger has its own, so a blow that staggers can push harder than one that doesn't
		const float want = rule.Value("push", 1.0f) *
			(b.stagger ? std::min(b.push, staggerMaxPush_) :
			 b.melee   ? std::min(b.push, meleeMaxPush_) :
			 b.shot    ? std::min(b.push, hitMaxPush_) :
						 b.push);
		if (r >= 0 && asShot && b.oomph > 0.0f) {
			// a heavy hit: the head snaps with it, she stumbles back along it on the shot task's own steps
			if (rule.Has("headsnap")) {
				float hp[3]{}, hq[4]{};
				dl.nm_agent_get_part_transform(a.agent, kHead, hp, hq);
				const float w = headWhip_ * want * (0.5f + b.oomph) * rule.Value("headsnap", 1.0f);
				const float imp[3]{ w * b.dir.x, w * b.dir.y, w * b.dir.z };
				dl.nm_agent_apply_push(a.agent, hp, imp);
				how += fmt::format(", head snaps {:.0f} N s", w);
			}
			// if she goes down anyway: on the ground for a while (a blow, half as long)
			a.writheFor = std::max(a.writheFor, (b.melee ? 0.5f : 1.0f) * (writheTime_ + writheAdd_ * b.oomph));
			a.writheOomph = std::max(a.writheOomph, b.oomph);
			if (b.element >= 0) {
				a.writheElement = b.element;
			}
		}
		if (r < 0) {
			logger::error("  {} task: {}", how, LastError());
		}
		const float push = AddPush(a, at, b.dir, want, pelvisShare_);
		{
			float hp[3]{}, hq[4]{};
			dl.nm_agent_get_part_transform(a.agent, kHead, hp, hq);
			if (hp[2] < 1.0f) {
				a.writheFor = std::max(a.writheFor, (b.melee ? 0.8f : 1.0f) * (writheTime_ + writheAdd_ * b.oomph));
				a.writheOomph = std::max(a.writheOomph, b.oomph);
				if (b.element >= 0) {
					a.writheElement = b.element;
				}
				how += ", on the ground already";
			}
		}
		// the element on top, for its own time; one still running goes on over the blow, to its own end
		if (const float t = rule.Value("burn", burnTime_); rule.Has("burn") && t > 0.0f) {
			a.burnUntil = std::max(a.burnUntil, a.age + t);
		}
		if (const float t = rule.Value("shock", shockTime_); rule.Has("shock") && t > 0.0f) {
			a.shockUntil = std::max(a.shockUntil, a.age + t);
		}
		if (const float t = rule.Value("chill", channelLinger_); rule.Has("chill") && t > 0.0f) {
			a.chillUntil = std::max(a.chillUntil, a.age + t);
		}
		how = fmt::format("[{}: {}] ", key, rule.text) + how;
		Overlays(a, V3{ b.dir.x, b.dir.y, 0.0f });
		Reacting(a);
		logger::info("{:08X}: {} in the {}{}: {}{}, push {:.0f} N s (wanted {:.0f}; {}) along {:.2f} {:.2f} {:.2f}{}{}", a_actor->GetFormID(), b.what,
			rig_[static_cast<std::size_t>(part)].name, b.exact ? "" : " (no point: the chest)", how,
			asShot ? fmt::format(" (health {:.0f})", health) : "", push, b.push, b.note, b.dir.x, b.dir.y, b.dir.z,
			a.burnUntil >= 0.0f ? fmt::format(", burning for {:.1f} s", a.burnUntil - a.age) : "",
			a.shockUntil >= 0.0f ? fmt::format(", shocked for {:.1f} s", a.shockUntil - a.age) : "");
	}

	void Engine::DownHit(Active& a, RE::Actor* a_actor, const Blow& b)
	{
		if (!a.death) {
			a.lastType = b.type;
			a.lastElement = b.element;
		}
		// the blow moves the part it landed on: Skyrim's body once it is in its ragdoll (Delirium only follows it then),
		// else Delirium's own. A blast (thrown) moves all of her
		// a body on the ground takes less than one on its feet: no stance to knock out ([Reactions] downhit push)
		std::string     key = "(none)";
		static const Rule kDefault = ParseRule("react, push:0.6");
		const Rule*     found = FindRule({ std::string("downhit.") + TypeName(b.type), "downhit" }, key);
		const Rule&     rule = found ? *found : kDefault;
		const float     cap = b.melee ? meleeMaxPush_ : hitMaxPush_;
		const float     push = std::min(b.push, cap) * rule.Value("push", 1.0f);
		const bool      thrown = b.fly.x != 0.0f || b.fly.y != 0.0f || b.fly.z != 0.0f;
		float           at[3]{}, q[4]{};
		int             part = kChest;
		if (b.exact) {
			const RE::NiPoint3 h = (b.at - a.origin) * kUnit;
			at[0] = h.x, at[1] = h.y, at[2] = h.z;
			part = NearestPart(a, at);
		} else {
			dl.nm_agent_get_part_transform(a.agent, kChest, at, q);
		}
		const bool skyrims = a.ragdolled || a.takeover;
		if (!skyrims) {
			if (thrown) {
				const float m = 60.0f * 0.5f;  // about half her weight's worth of the blast
				const float imp[3]{ b.fly.x * m, b.fly.y * m, b.fly.z * m };
				dl.nm_agent_apply_push(a.agent, at, imp);
			} else if (push > 0.0f) {
				AddPush(a, at, b.dir, push);
			}
		} else if (thrown) {
			a.kicks.push_back({ -1, Scl(b.fly, 0.5f), {} });
		} else if (push > 0.0f) {
			if (a.kicks.size() > 8) {
				a.kicks.erase(a.kicks.begin());  // no drive ran to take them (a corpse held still)
			}
			float pp[3]{}, pq[4]{};
			dl.nm_agent_get_part_transform(a.agent, part, pp, pq);
			a.kicks.push_back({ part, Scl(b.dir, push), { at[0] - pp[0], at[1] - pp[1], at[2] - pp[2] } });
		}
		std::string how = thrown ? "the blast moves her" : fmt::format("{:.0f} N s on the {}", push, rig_[static_cast<std::size_t>(part)].name);
		// alive: she squirms from it on the ground (or burns, convulses, curls up shivering), for longer the harder it was.
		// One already going goes on longer; another element starts its own
		how = fmt::format("[{}: {}] ", key, rule.text) + how;
		if (!a.death && rule.Has("react")) {
			float dur = rule.Value("time", 1.0f) * (writheTime_ + writheAdd_ * b.oomph);
			if (b.element == kFire) {
				dur = std::max(dur, burnTime_);
			}
			const bool other = b.element >= 0 && b.element != a.writheElement;
			if (b.element >= 0) {
				a.writheElement = b.element;
			}
			a.writheOomph = std::max(a.writheOomph, b.oomph);
			a.writheFor = std::max(a.writheFor, dur);
			if (a.writheUntil >= 0.0f && !other) {
				a.writheUntil = std::max(a.writheUntil, a.age + dur);
				how += fmt::format(", on the ground {:.1f} s more", a.writheUntil - a.age);
			} else if (a.writheAt < 0.0f || other) {
				a.writheFor = a.writheAt >= 0.0f ? std::max(a.writheFor, dur) : dur;  // one still to start keeps its time
				a.writheAt = a.age + 0.1f;  // a beat after the blow, so the blow itself shows
				how += ", reacting on the ground";
			}
			a.settled = 0;
		}
		a.blend = 1.0f;
		a.blendingOut = false;
		logger::info("{:08X}: {} while down{}: {} ({})", a_actor->GetFormID(), b.what, a.death ? " (dead)" : "", how, b.note);
	}

	void Engine::Fly(Active& a, RE::Actor* a_actor, const Blow& b)
	{
		a.lastType = b.type;
		a.lastElement = b.element;
		// thrown, alive the whole way, Delirium's own behaviours (their own gains) stacked by phase:
		// in the air: far throws Delirium's highFall (arms windmill, legs pedal, the body turns into the fall, its own
		// catch); shorter ones arms windmilling and legs pedalling, or tucked in a roll. A push at the head tumbles her.
		// on the way: arms out at whatever she is thrown at (braceForImpact, the frame update)
		// coming down: reaching to catch it; down: writhing or curled up a while, then Skyrim gets her up
		dl.nm_msg_begin(a.agent, "stopAllBehaviours");
		dl.nm_msg_send(a.agent, a.world);
		a.reelUntil = -1.0f;
		const float along = std::sqrt(b.fly.x * b.fly.x + b.fly.y * b.fly.y);
		// the reaction table: fly.element.far, fly.element, fly.far, fly; the element's layers on top
		std::string       key = "(none)";
		const std::string el = ElementName(b.element);
		const bool        isFar = along >= highFallAt_;
		std::vector<std::string> keys;
		if (!el.empty()) {
			if (isFar) {
				keys.push_back("fly." + el + ".far");
			}
			keys.push_back("fly." + el);
		}
		const std::string ty = TypeName(b.type);
		if (isFar) {
			keys.push_back("fly." + ty + ".far");
		}
		keys.push_back("fly." + ty);
		if (isFar) {
			keys.push_back("fly.far");
		}
		keys.push_back("fly");
		Rule rule;
		for (const auto& k : keys) {
			if (auto it = rules_.find(k); it != rules_.end()) {
				rule = it->second;
				key = k;
				break;
			}
		}
		{
			const Rule e = ElementRule(b.element);
			rule.layers.insert(rule.layers.end(), e.layers.begin(), e.layers.end());
		}
		a.flyMode = Choose(rule, { "windmill", "rollup", "highfall", "foetal" });
		if (a.flyMode < 0) {
			a.flyMode = 3;
		}
		if (a.flyMode == 2) {
			dl.nm_msg_begin(a.agent, "highFall");
			dl.nm_msg_set_bool(a.agent, "start", 1);
		} else if (a.flyMode == 3) {
			// curled up (the upper body) with the legs kicking hard: the busiest throw (fit-test layers: 250 deg/s vs
			// highFall's 210, foetal alone 95)
			dl.nm_msg_begin(a.agent, "bodyFoetal");
			dl.nm_msg_set_bool(a.agent, "start", 1);
			dl.nm_msg_set_string(a.agent, "mask", "fb");
			dl.nm_msg_set_float(a.agent, "stiffness", 13.0f);
			dl.nm_msg_set_float(a.agent, "dampingFactor", 1.0f);
			dl.nm_msg_set_float(a.agent, "asymmetry", Rand(0.0f, 0.4f));
			dl.nm_msg_set_float(a.agent, "backTwist", Rand(-0.3f, 0.3f));
			dl.nm_msg_set_int(a.agent, "randomSeed", static_cast<int>(Rand(1.0f, 999.0f)));
		} else {
			dl.nm_msg_begin(a.agent, "pedalLegs");
			dl.nm_msg_set_bool(a.agent, "start", 1);
			dl.nm_msg_set_bool(a.agent, "backPedal", Rand(0.0f, 1.0f) < 0.5f);
			dl.nm_msg_send(a.agent, a.world);
			if (a.flyMode == 1) {
				dl.nm_msg_begin(a.agent, "bodyRollUp");
				dl.nm_msg_set_bool(a.agent, "start", 1);
				dl.nm_msg_set_string(a.agent, "mask", "fb");
			} else {
				dl.nm_msg_begin(a.agent, "armsWindmillAdaptive");
				dl.nm_msg_set_bool(a.agent, "start", 1);
				dl.nm_msg_set_bool(a.agent, "disableOnImpact", 1);
			}
		}
		if (dl.nm_msg_send(a.agent, a.world) < 0) {
			logger::error("  thrown: {}", LastError());
		}
		std::string layer;
		if (a.flyMode == 0) {
			// the spine writhes over the throw (its mask only): burning, it thrashes; else it twists and arches
			const bool fire = rule.Has("thrash");
			dl.nm_msg_begin(a.agent, "bodyWrithe");
			dl.nm_msg_set_bool(a.agent, "start", 1);
			dl.nm_msg_set_string(a.agent, "mask", "us");
			dl.nm_msg_set_float(a.agent, "backAmplitude", fire ? 1.8f : 1.1f);
			dl.nm_msg_set_float(a.agent, "backPeriod", (fire ? 0.4f : 0.7f) * Rand(0.85f, 1.15f));
			dl.nm_msg_send(a.agent, a.world);
			layer = fire ? ", thrashing on fire" : ", spine writhing";
		}
		if (rule.Has("shock")) {
			dl.nm_msg_begin(a.agent, "electrocute");
			dl.nm_msg_set_bool(a.agent, "start", 1);
			dl.nm_msg_set_float(a.agent, "stunMag", 0.4f);
			dl.nm_msg_set_float(a.agent, "stunInterval", 0.07f);
			dl.nm_msg_set_float(a.agent, "largeMult", 2.5f);
			dl.nm_msg_send(a.agent, a.world);
			layer += ", electrocuted";
		}
		const float dv[3]{ b.fly.x, b.fly.y, b.fly.z };
		if (!b.inherited && dl.nm_agent_add_velocity(a.agent, dv) < 0) {
			logger::error("  thrown: {}", LastError());
		}
		// the head goes first, the legs swing under: a tumble (a little sideways, random)
		float hp[3]{}, hq[4]{};
		dl.nm_agent_get_part_transform(a.agent, kHead, hp, hq);
		const float t = b.inherited ? 0.0f : tumble_ * rule.Value("tumble", 1.0f) * along * Rand(0.8f, 1.2f);
		const float side = Rand(-0.35f, 0.35f);
		const float fx = along > 1e-3f ? b.fly.x / along : 0.0f, fy = along > 1e-3f ? b.fly.y / along : 0.0f;
		const float imp[3]{ t * (fx - side * fy), t * (fy + side * fx), 0.0f };
		if (t > 0.0f) {
			dl.nm_agent_apply_push(a.agent, hp, imp);
		}
		a.flying = 1;
		a.flyAt = a.age;
		a.braced = false;
		a.braceCheckAt = a.age + 0.1f;
		const float hard = std::clamp(along / std::max(flyMax_, 1e-3f), 0.0f, 1.0f);
		a.writheFor = writheTime_ + writheAdd_ * std::max(hard, b.oomph);
		a.writheOomph = std::max(hard, b.oomph);
		a.writheElement = b.element;
		a.lastPushAt = a.age;  // the hit and blast that come with it add nothing more
		a.lastPush = 1e6f;
		Reacting(a);
		static constexpr const char* kModes[]{ "arms windmilling, legs pedalling", "rolled up, legs pedalling", "high fall", "curled up" };
		logger::info("{:08X}: {}: [{}: {}] thrown {:.1f} m/s along, {:.1f} up, {}{}, tumble {:.0f} N s; writhes {:.1f} s once down ({})", a_actor->GetFormID(),
			b.what, key, rule.text, along, b.fly.z, kModes[a.flyMode], layer, t, a.writheFor, b.note);
	}

	void Engine::Channel(Active& a, RE::Actor* a_actor, const RE::NiPoint3& a_vel, int a_kind)
	{
		const float fl = std::sqrt(a_vel.x * a_vel.x + a_vel.y * a_vel.y);
		const V3    away = fl > 1.0f ? V3{ a_vel.x / fl, a_vel.y / fl, 0.0f } : V3{};
		const float until = a.age + channelLinger_;
		const char* now = nullptr;
		if (a_kind != kFire && a_kind != kShock && a_kind != kFrost) {
			return;
		}
		// on the ground: rolling in the flames, convulsing, curled up shivering, for as long as it keeps hitting (and a
		// moment after); before, every tick let her go as lying still and she got up into the next one
		constexpr float kTail = 1.0f;
		a.writheElement = a_kind;
		a.writheOomph = std::max(a.writheOomph, 0.5f);
		a.writheFor = std::max(a.writheFor, channelLinger_ + kTail);
		if (a.knockQueued) {
			if (a.writheUntil >= 0.0f) {
				a.writheUntil = std::max(a.writheUntil, until + kTail);
			} else if (a.writheAt < 0.0f) {
				a.writheAt = a.age;
				now = "down, starting on the ground";
			}
			a.settled = 0;
			a.blend = 1.0f;
			a.blendingOut = false;
			if (now) {
				logger::info("{:08X}: {} channel: {}", a_actor->GetFormID(), kHitNames[static_cast<std::size_t>(std::clamp(a_kind, 0, 4))], now);
			}
			return;
		}
		if (a_kind == kFire) {
			if (a.burnUntil < 0.0f) {
				StartBurn(a, away);
				now = "burning";
			}
			a.burnUntil = std::max(a.burnUntil, until);
		} else if (a_kind == kShock) {
			if (a.shockUntil < 0.0f) {
				StartShock(a);
				now = "shocked";
			}
			a.shockUntil = std::max(a.shockUntil, until);
		} else if (a_kind == kFrost) {
			if (a.chillUntil < 0.0f) {
				StartChill(a);
				now = "freezing";
			}
			a.chillUntil = std::max(a.chillUntil, until);
		} else {
			return;
		}
		a.calm = 0;
		a.sincePush = 0;
		a.blend = 1.0f;
		a.blendingOut = false;
		if (now) {
			logger::info("{:08X}: {} channel: {}", a_actor->GetFormID(), kHitNames[static_cast<std::size_t>(std::clamp(a_kind, 0, 4))], now);
		}
	}

	void Engine::Reacting(Active& a)
	{
		a.calm = 0;
		a.sincePush = 0;
		a.blend = 1.0f;
		a.blendingOut = false;
	}

	float Engine::AddPush(Active& a, const float* a_at, const V3& a_dir, float a_push, float a_pelvis)
	{
		// a_pelvis: that share goes to the pelvis, so the whole body is driven back into catch steps instead of folding
		float push = a_push;
		if (a.age - a.lastPushAt < 0.3f) {
			push = std::max(0.0f, a_push - a.lastPush);  // the same blow seen twice (hit + stagger): the stronger one counts
			a.lastPush = std::max(a.lastPush, a_push);
		} else {
			a.lastPush = a_push;
			a.lastPushAt = a.age;
		}
		if (push > 0.0f) {
			const float k = 1.0f - a_pelvis;
			const float imp[3]{ k * push * a_dir.x, k * push * a_dir.y, k * push * a_dir.z };
			dl.nm_agent_apply_push(a.agent, a_at, imp);
			if (a_pelvis > 0.0f) {
				float pp[3]{}, pq[4]{};
				dl.nm_agent_get_part_transform(a.agent, 0, pp, pq);
				const float ip[3]{ a_pelvis * push * a_dir.x, a_pelvis * push * a_dir.y, a_pelvis * push * a_dir.z };
				dl.nm_agent_apply_push(a.agent, pp, ip);
			}
		}
		return push;
	}

	void Engine::LookAt(Active& a, RE::Actor* a_by, float* a_out) const
	{
		RE::NiPoint3 head = a_by->GetPosition();
		head.z += 115.0f * a_by->GetScale();
		const RE::NiPoint3 h = (head - a.origin) * kUnit;
		a_out[0] = h.x;
		a_out[1] = h.y;
		a_out[2] = h.z;
	}

	void Engine::Overlays(Active& a, const V3& a_away)
	{
		if (a.burnUntil >= 0.0f) {
			StartBurn(a, a_away);
		}
		if (a.shockUntil >= 0.0f) {
			StartShock(a);
		}
		if (a.chillUntil >= 0.0f) {
			StartChill(a);
		}
	}

	void Engine::StartBurn(Active& a, const V3& a_away)
	{
		// GTA's onFire done by hand (its tuning is in GTA's task params, not loaded): she keeps her balance while the
		// arms and back writhe fast, beating at the flames, and stumbles away from the hit
		dl.nm_msg_begin(a.agent, "bodyWrithe");
		dl.nm_msg_set_bool(a.agent, "start", 1);
		dl.nm_msg_set_string(a.agent, "mask", "ub");
		dl.nm_msg_set_float(a.agent, "armStiffness", 12.0f);
		dl.nm_msg_set_float(a.agent, "armDamping", 0.5f);
		dl.nm_msg_set_float(a.agent, "armPeriod", Rand(0.4f, 0.6f));
		dl.nm_msg_set_float(a.agent, "armAmplitude", Rand(1.5f, 2.0f));
		dl.nm_msg_set_float(a.agent, "elbowAmplitude", 1.5f);
		dl.nm_msg_set_float(a.agent, "backStiffness", 11.0f);
		dl.nm_msg_set_float(a.agent, "backDamping", 0.6f);
		dl.nm_msg_set_float(a.agent, "backPeriod", Rand(0.8f, 1.2f));
		dl.nm_msg_set_float(a.agent, "backAmplitude", Rand(0.8f, 1.2f));
		dl.nm_msg_send(a.agent, a.world);
		if (a_away.x != 0.0f || a_away.y != 0.0f) {
			const float v[3]{ a_away.x, a_away.y, 0.0f };
			dl.nm_msg_begin(a.agent, "leanInDirection");
			dl.nm_msg_set_bool(a.agent, "start", 1);
			dl.nm_msg_set_bool(a.agent, "applyAsForce", 0);
			dl.nm_msg_set_vec3(a.agent, "dir", v);
			dl.nm_msg_set_float(a.agent, "leanAmount", Rand(0.2f, 0.35f));
			dl.nm_msg_send(a.agent, a.world);
		}
	}

	void Engine::StopBurn(Active& a)
	{
		a.burnUntil = -1.0f;
		for (const char* m : { "bodyWrithe", "leanInDirection" }) {
			dl.nm_msg_begin(a.agent, m);
			dl.nm_msg_set_bool(a.agent, "start", 0);
			dl.nm_msg_send(a.agent, a.world);
		}
	}

	void Engine::StartShock(Active& a)
	{
		dl.nm_msg_begin(a.agent, "electrocute");
		dl.nm_msg_set_bool(a.agent, "start", 1);
		dl.nm_msg_set_float(a.agent, "stunMag", Rand(0.45f, 0.55f));
		dl.nm_msg_set_float(a.agent, "stunInterval", 0.07f);
		dl.nm_msg_set_float(a.agent, "largeMult", 3.0f);
		dl.nm_msg_set_float(a.agent, "largeMinTime", 0.2f);
		dl.nm_msg_set_float(a.agent, "largeMaxTime", 0.45f);
		dl.nm_msg_set_float(a.agent, "directionRandomness", 0.6f);
		dl.nm_msg_send(a.agent, a.world);
	}

	void Engine::StopShock(Active& a)
	{
		a.shockUntil = -1.0f;
		dl.nm_msg_begin(a.agent, "electrocute");
		dl.nm_msg_set_bool(a.agent, "start", 0);
		dl.nm_msg_send(a.agent, a.world);
	}

	void Engine::StartChill(Active& a)
	{
		// all Delirium's own: the upper body curls in (arms clutched, back and neck bent; the legs stay the balancer's),
		// a fine fast shiver that also stiffens the muscles, and short low shuffling steps
		dl.nm_msg_begin(a.agent, "bodyFoetal");
		dl.nm_msg_set_bool(a.agent, "start", 1);
		dl.nm_msg_set_string(a.agent, "mask", "ub");
		dl.nm_msg_set_float(a.agent, "stiffness", 7.0f);
		dl.nm_msg_set_float(a.agent, "dampingFactor", 1.6f);
		dl.nm_msg_set_float(a.agent, "asymmetry", 0.3f);
		dl.nm_msg_set_int(a.agent, "randomSeed", static_cast<int>(Rand(1.0f, 10000.0f)));
		dl.nm_msg_send(a.agent, a.world);
		dl.nm_msg_begin(a.agent, "electrocute");
		dl.nm_msg_set_bool(a.agent, "start", 1);
		dl.nm_msg_set_bool(a.agent, "leftLeg", 0);
		dl.nm_msg_set_bool(a.agent, "rightLeg", 0);
		dl.nm_msg_set_float(a.agent, "stunMag", 0.06f);
		dl.nm_msg_set_float(a.agent, "stunInterval", 0.05f);
		dl.nm_msg_set_float(a.agent, "largeMult", 1.0f);
		dl.nm_msg_set_float(a.agent, "directionRandomness", 0.3f);
		dl.nm_msg_send(a.agent, a.world);
		dl.nm_msg_begin(a.agent, "configureBalance");
		dl.nm_msg_set_float(a.agent, "stepClampScale", 0.5f);
		dl.nm_msg_set_float(a.agent, "stepHeight", 0.05f);
		dl.nm_msg_send(a.agent, a.world);
	}

	void Engine::StopChill(Active& a)
	{
		a.chillUntil = -1.0f;
		dl.nm_msg_begin(a.agent, "bodyFoetal");
		dl.nm_msg_set_bool(a.agent, "start", 0);
		dl.nm_msg_send(a.agent, a.world);
		if (a.shockUntil < 0.0f) {  // the shiver is an electrocute: a shock still running keeps it
			dl.nm_msg_begin(a.agent, "electrocute");
			dl.nm_msg_set_bool(a.agent, "start", 0);
			dl.nm_msg_send(a.agent, a.world);
		}
		dl.nm_msg_begin(a.agent, "configureBalanceReset");
		dl.nm_msg_send(a.agent, a.world);
	}

	int Engine::NearestPart(const Active& a, const float* a_at) const
	{
		// distance to each part's bone: from the part to its children (a hand or the head: the part itself)
		std::array<V3, 21> p;
		for (int i = 0; i < 21; ++i) {
			float t[3]{}, q[4]{};
			dl.nm_agent_get_part_transform(a.agent, i, t, q);
			p[static_cast<std::size_t>(i)] = { t[0], t[1], t[2] };
		}
		const V3 x{ a_at[0], a_at[1], a_at[2] };
		auto     seg = [&](const V3& s, const V3& e) {
            const V3    d = Sub(e, s), w = Sub(x, s);
            const float dd = d.x * d.x + d.y * d.y + d.z * d.z;
            const float t = dd > 1e-8f ? std::clamp((w.x * d.x + w.y * d.y + w.z * d.z) / dd, 0.0f, 1.0f) : 0.0f;
            const V3    c = Sub(x, Add(s, Scl(d, t)));
            return c.x * c.x + c.y * c.y + c.z * c.z;
		};
		int   best = 0;
		float bestD = std::numeric_limits<float>::max();
		for (std::size_t i = 0; i < 21; ++i) {
			float di = seg(p[i], p[i]);
			for (std::size_t c = 1; c < 21; ++c) {
				if (kParent[c] == static_cast<int>(i)) {
					di = std::min(di, seg(p[i], p[c]));
				}
			}
			if (di < bestD) {
				bestD = di;
				best = static_cast<int>(i);
			}
		}
		return best;
	}

	void Engine::Knocked(Active& a, const V3& a_vel)
	{
		// Skyrim knocked her down by itself (a shout, a trap, a script's push, its own stagger fall): thrown fast, the
		// throw's behaviours on Skyrim's own motion (the fly rows, type knock); else she falls over, breaks the fall and
		// reacts on the ground (the down rows)
		auto self = a.actor.get();
		if (!self) {
			return;
		}
		const float along = std::sqrt(a_vel.x * a_vel.x + a_vel.y * a_vel.y);
		if (along >= flyMin_) {
			Blow b;
			b.what = "Skyrim's knock";
			b.note = "taken over";
			b.type = kBlowKnock;
			b.fly = a_vel;
			b.inherited = true;
			b.oomph = std::clamp(along / std::max(flyMax_, 1e-3f), 0.0f, 1.0f);
			// a shout's knock-down (Fus Ro Dah, a mod's): its own thrown rows (fly.shout), its element on top
			if (LastHit last; RecentHit(self->GetFormID(), 1.0f, last) && last.shout) {
				b.what = "shout";
				b.note = fmt::format("{} words, Skyrim's push taken over", last.word);
				b.type = kBlowShout;
				b.word = last.word;
				if (last.element == kFire || last.element == kFrost || last.element == kShock) {
					b.element = last.element;
				}
			}
			Fly(a, self.get(), b);
			return;
		}
		a.lastType = kBlowKnock;
		a.writheFor = writheTime_;
		a.writheOomph = 0.0f;
		a.writheElement = -1;
		a.writheAt = a.age + 0.8f;  // once she is down
		logger::info("{:08X}: Skyrim's knock, {:.1f} m/s: falling over, breaking the fall, then reacting on the ground", self->GetFormID(), along);
	}

}

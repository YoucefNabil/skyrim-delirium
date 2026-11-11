// Body meets body ([Bodies]): NPCs' ragdoll bodies are in Skyrim's Havok world while they stand (body/world.cpp), so a
// thrown or falling body stops against them, and a body that comes at a standing NPC hard enough is a blow on her:
// she reacts in Delirium like to any other blow. The body that does it is one Skyrim has ragdolled, or one Delirium
// has on its feet (its Havok bodies follow Delirium's pose, body/drive.cpp). The one on its feet is stopped by the
// others' bodies inside Delirium's own physics: they go into her step as capsules (Others), each frame, from where
// Skyrim's world has them. Two NPCs Delirium has on their feet each meet the other's bodies that way.
#include "shared.h"

#include "body/world.h"

namespace delirium
{
	namespace
	{
		constexpr float kTouch = 0.32f;  // m between two bodies' centres: touching (a thrown body's stopped at 0.27-0.29)
		constexpr float kNear = 250.0f;  // game units between the two actors: worth looking at their bodies
		constexpr float kMostOverlap = 0.05f;  // m another's capsule may be inside one of her parts: deeper, it is thinned
		                                       // (seen: a capsule 0.3 m inside her threw her arm out at 9 m/s)

		V3 Pt(const float* p) { return { p[0], p[1], p[2] }; }
		float Dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

		// between segments p1-q1 and p2-q2
		float SegDist(const V3& p1, const V3& q1, const V3& p2, const V3& q2)
		{
			const V3    d1 = Sub(q1, p1), d2 = Sub(q2, p2), r = Sub(p1, p2);
			const float a = Dot(d1, d1), e = Dot(d2, d2), f = Dot(d2, r);
			float       s = 0.0f, t = 0.0f;
			if (a > 1e-9f && e > 1e-9f) {
				const float b = Dot(d1, d2), c = Dot(d1, r), den = a * e - b * b;
				s = den > 1e-9f ? std::clamp((b * f - c * e) / den, 0.0f, 1.0f) : 0.0f;
				t = (b * s + f) / e;
				if (t < 0.0f) {
					t = 0.0f;
					s = std::clamp(-c / a, 0.0f, 1.0f);
				} else if (t > 1.0f) {
					t = 1.0f;
					s = std::clamp((b - c) / a, 0.0f, 1.0f);
				}
			} else if (a > 1e-9f) {
				s = std::clamp(-Dot(d1, r) / a, 0.0f, 1.0f);
			} else if (e > 1e-9f) {
				t = std::clamp(f / e, 0.0f, 1.0f);
			}
			const V3 d = Sub(Add(p1, Scl(d1, s)), Add(p2, Scl(d2, t)));
			return std::sqrt(Dot(d, d));
		}

		// on her feet in Skyrim's eyes: her bodies belong in the world, and a body can strike her
		bool Stands(RE::Actor* a)
		{
			return !a->IsPlayerRef() && !a->IsDead() && !a->IsDisabled() && a->Get3D(false) && !a->IsInRagdollState() &&
			       a->AsActorState()->GetKnockState() == RE::KNOCK_STATE_ENUM::kNormal;
		}
	}

	bool Engine::Upright(RE::Actor* a_actor)
	{
		if (g_count.load(std::memory_order_relaxed) == 0) {
			return false;
		}
		std::scoped_lock l(lock_);
		return std::ranges::any_of(active_, [&](const auto& a) { return a->actor.get().get() == a_actor && !a->knockQueued && !a->ragdolled; });
	}

	void Engine::Others(RE::Actor* a_actor, bool a_upright)
	{
		if (g_count.load(std::memory_order_relaxed) == 0) {
			return;
		}
		// 11 floats each (nm_agent_set_others): the ends, radius, velocity, and 1: gives way like a body (the overlap is
		// shared; anyone on their feet), 0: solid (Skyrim's ragdoll carries it, a corpse). Skyrim's world first, outside
		// lock_ (their graphs' locks)
		std::vector<float> caps;
		if (a_upright && bodies_) {
			const auto put = [&](const world::Capsule& c, float a_body) {
				caps.insert(caps.end(), { c.a.x, c.a.y, c.a.z, c.b.x, c.b.y, c.b.z, c.radius, c.vel.x, c.vel.y, c.vel.z, a_body });
			};
			for (const auto& h : world::Added()) {
				const auto t = h.get();
				if (!t || t.get() == a_actor || !t->Get3D(false) || t->IsDisabled() || t->GetPosition().GetDistance(a_actor->GetPosition()) > kNear) {
					continue;
				}
				const float body = t->IsDead() || t->IsInRagdollState() ? 0.0f : 1.0f;
				for (const auto& c : world::Capsules(t.get())) {
					put(c, body);
				}
			}
			// the player: Skyrim moves them and nothing of hers can, so solid. Their ragdoll when it is in the world
			// (knocked down), else the volume they stand in
			if (auto* pc = RE::PlayerCharacter::GetSingleton();
				pc && pc->Get3D(false) && !pc->IsOnMount() && pc->GetPosition().GetDistance(a_actor->GetPosition()) <= kNear) {
				const auto ragdoll = world::Capsules(pc);
				for (const auto& c : ragdoll) {
					put(c, 0.0f);
				}
				if (ragdoll.empty()) {
					put(world::Standing(pc), 0.0f);
				}
			}
		}
		std::scoped_lock l(lock_);
		for (auto& p : active_) {
			auto& a = *p;
			if (a.actor.get().get() != a_actor) {
				continue;
			}
			if (a.knockQueued || a.ragdolled) {
				caps.clear();  // she went down since
			}
			// into her physics frame (metres from her origin), and none deeper than kMostOverlap inside her as she is now
			std::array<float, 21 * 11> mine{};
			const int                  parts = caps.empty() ? 0 : std::min(dl.nm_agent_get_capsules(a.agent, mine.data(), 21), 21);
			for (std::size_t k = 0; k + 10 < caps.size(); k += 11) {
				float* o = caps.data() + k;
				for (int i = 0; i < 2; ++i) {
					o[i * 3] -= a.origin.x * kUnit;
					o[i * 3 + 1] -= a.origin.y * kUnit;
					o[i * 3 + 2] -= a.origin.z * kUnit;
				}
				float deep = 0.0f;
				for (int i = 0; i < parts; ++i) {
					const float* m = mine.data() + i * 11;
					deep = std::max(deep, m[6] + o[6] - SegDist(Pt(m), Pt(m + 3), Pt(o), Pt(o + 3)));
				}
				if (deep > kMostOverlap) {
					o[6] = std::max(0.02f, o[6] - (deep - kMostOverlap));
				}
			}
			const int n = static_cast<int>(caps.size() / 11);
			if (n != a.others) {
				logger::info("{:08X}: {} bodies of others near her in her physics (were {})", a_actor->GetFormID(), n, a.others);
			}
			if (n > 0 || a.others > 0) {
				dl.nm_agent_set_others(a.agent, caps.data(), n);
			}
			a.others = n;
			return;
		}
	}

	void Engine::Meet(RE::Actor* a_actor)
	{
		const bool upright = !a_actor->IsPlayerRef() && Upright(a_actor);
		Others(a_actor, upright);
		if (!bodies_) {
			// switched off: everything back as Skyrim had it, once
			if (world::Changed() && !leaving_.exchange(true)) {
				g_tasks->AddTask([]() {
					world::Leave();
					Engine::Get().leaving_ = false;
				});
			}
			return;
		}
		if (!ready_ || a_actor->IsPlayerRef()) {
			return;
		}
		const bool down = a_actor->IsInRagdollState();
		if (down || upright) {
			Strikes(a_actor, down);
		}
		if (down || !Stands(a_actor)) {
			return;
		}
		// her bodies are looked at twice a second, each NPC on her own clock
		{
			const auto       now = std::chrono::steady_clock::now();
			std::scoped_lock l(meetLock_);
			auto&            last = lookedAt_[a_actor->GetFormID()];
			if (now - last < std::chrono::milliseconds(500)) {
				return;
			}
			last = now;
			if (lookedAt_.size() > 256) {
				std::erase_if(lookedAt_, [&](const auto& p) { return now - p.second > std::chrono::seconds(10); });
			}
		}
		auto* race = a_actor->GetRace();
		if (!race || !(race->HasKeywordString("ActorTypeNPC") || Creature(a_actor)) || a_actor->IsOnMount() || !world::Bodies(a_actor).at.empty()) {
			return;
		}
		// she stands with her bodies out of the world (new here, back up from a fall, or Skyrim took them out): in they go
		g_tasks->AddTask([h = a_actor->CreateRefHandle()]() {
			if (const auto a = h.get(); a && Engine::Get().bodies_ && Stands(a.get())) {
				world::Collide();
				world::Add(a.get());
			}
		});
	}

	void Engine::Strikes(RE::Actor* a_body, bool a_down)
	{
		const float   least = a_down ? bodySpeed_ : stumbleSpeed_;
		world::Sample mine;
		bool          sampled = false;
		for (const auto& h : world::Added()) {
			const auto t = h.get();
			if (!t || t.get() == a_body || !Stands(t.get()) || t->GetPosition().GetDistance(a_body->GetPosition()) > kNear) {
				continue;
			}
			if (!a_down && Upright(t.get())) {
				continue;  // both on their feet in Delirium: their bodies meet in its physics (Others), no blow on top
			}
			if (!sampled) {
				mine = world::Bodies(a_body);
				sampled = true;
			}
			if (mine.at.empty()) {
				return;
			}
			const auto   hers = world::Bodies(t.get());
			float        best = 0.0f;
			RE::NiPoint3 at, vel;
			for (std::size_t i = 0; i < mine.at.size(); ++i) {
				for (std::size_t j = 0; j < hers.at.size(); ++j) {
					auto        gap = hers.at[j] - mine.at[i];
					const float d = gap.Length();
					if (d > kTouch || d < 1e-4f) {
						continue;
					}
					gap /= d;
					// the body has to come at her: her own walk into one that lies still is not a strike
					if (mine.vel[i].Dot(gap) < 0.5f * least) {
						continue;
					}
					const auto rel = mine.vel[i] - hers.vel[j];
					if (const float s = rel.Length(); rel.Dot(gap) > 0.0f && s > best) {
						best = s;
						at = (mine.at[i] + hers.at[j]) * 0.5f;
						vel = rel;
					}
				}
			}
			if (best < least) {
				continue;
			}
			{
				const auto       now = std::chrono::steady_clock::now();
				std::scoped_lock l(meetLock_);
				auto&            last = struckAt_[t->GetFormID()];
				if (now - last < std::chrono::duration<float>(bodyRepeat_)) {
					continue;
				}
				last = now;
				if (struckAt_.size() > 256) {
					std::erase_if(struckAt_, [&](const auto& p) { return now - p.second > std::chrono::seconds(10); });
				}
			}
			logger::info("{:08X}'s body ({}) strikes {:08X} at {:.1f} m/s", a_body->GetFormID(), a_down ? "ragdolled" : "on her feet", t->GetFormID(), best);
			g_tasks->AddTask([th = h, bh = a_body->CreateRefHandle(), at, vel]() {
				const auto target = th.get();
				const auto by = bh.get();
				if (!target) {
					return;
				}
				Engine::Get().BodyHit(target.get(), by.get(), at / kUnit, vel);
			});
		}
	}

	void Engine::BodyHit(RE::Actor* a_target, RE::Actor* a_by, const RE::NiPoint3& a_at, const RE::NiPoint3& a_vel)
	{
		if (!ready_ || !Eligible(a_target, "body", true) || a_target->IsInRagdollState() ||
			a_target->AsActorState()->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal) {
			return;
		}
		const float speed = a_vel.Length();
		if (speed < 0.1f) {
			return;
		}
		Blow b;
		b.what = "body";
		b.type = kBlowBump;
		b.by = a_by && a_by != a_target ? RE::ActorPtr(a_by) : RE::ActorPtr{};
		b.at = a_at;
		b.exact = true;
		b.dir = { a_vel.x / speed, a_vel.y / speed, a_vel.z / speed };
		// a limb or a torso at that speed: some 12 kg behind it, no more than a hard shove
		// (seen: a thrown body arrives at 11 m/s; 25 per m/s knocked her flat every time)
		b.push = std::min(bodyPush_ * speed, bodyMaxPush_);
		b.note = fmt::format("{:08X}'s body at {:.1f} m/s", a_by ? a_by->GetFormID() : 0, speed);
		React(a_target, b);
	}
}

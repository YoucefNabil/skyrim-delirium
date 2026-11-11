// Body meets body ([Bodies]): NPCs' ragdoll bodies are in Skyrim's Havok world while they stand (body/world.cpp), so a
// thrown or falling body stops against them, and a body that comes at a standing NPC hard enough is a blow on her:
// she reacts in Delirium like to any other blow. The body that does it is one Skyrim has ragdolled, or one Delirium
// has on its feet (its Havok bodies follow Delirium's pose, body/drive.cpp); that one is pushed back as hard.
#include "shared.h"

#include "body/world.h"

namespace delirium
{
	namespace
	{
		constexpr float kTouch = 0.32f;  // m between two bodies' centres: touching (a thrown body's stopped at 0.27-0.29)
		constexpr float kNear = 250.0f;  // game units between the two actors: worth looking at their bodies

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

	void Engine::Meet(RE::Actor* a_actor)
	{
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
		if (down || Upright(a_actor)) {
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
				if (!a_down) {
					struckAt_[a_body->GetFormID()] = now;  // pushed back by the same meeting: she doesn't strike back at once
				}
				if (struckAt_.size() > 256) {
					std::erase_if(struckAt_, [&](const auto& p) { return now - p.second > std::chrono::seconds(10); });
				}
			}
			logger::info("{:08X}'s body ({}) strikes {:08X} at {:.1f} m/s", a_body->GetFormID(), a_down ? "ragdolled" : "on her feet", t->GetFormID(), best);
			g_tasks->AddTask([th = h, bh = a_body->CreateRefHandle(), at, vel, back = !a_down]() {
				const auto target = th.get();
				const auto by = bh.get();
				if (!target) {
					return;
				}
				auto& e = Engine::Get();
				e.BodyHit(target.get(), by.get(), at / kUnit, vel);
				if (back && by) {
					// a ragdolled body is stopped by Havok itself; one on its feet only follows Delirium's pose, so
					// what she ran into pushes back through Delirium
					e.BodyHit(by.get(), target.get(), at / kUnit, -vel);
				}
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

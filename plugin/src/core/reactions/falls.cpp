// Off a ledge on her own: an NPC that drops more than [Falls] fHighFallDrop m (Skyrim's character controller in the
// air) is taken into Delirium mid-air, moving as she fell (no-snap entry, body/takeover.cpp), and falls with its
// highFall (arms out, legs ready, its own catch). On the ground the landing hands her to Skyrim's ragdoll and its
// get-up, as a throw does (body/update.cpp). Skyrim still deals its own fall damage.
#include "shared.h"

namespace delirium
{
	void Engine::WatchFall(RE::Actor* a_actor)
	{
		if (a_actor->IsPlayerRef() || a_actor->IsInRagdollState() || a_actor->IsDead()) {
			return;
		}
		auto* cc = a_actor->GetCharController();
		if (!cc || cc->context.currentState != RE::hkpCharacterStateType::kInAir) {
			return;
		}
		const float drop = (cc->fallStartHeight - a_actor->GetPosition().z) * kUnit;
		// a drop she can have fallen in her time in the air. Seen: getting up from a ragdoll the controller is in the
		// air for a frame with fallStartHeight 0, which below z 0 (Whiterun) read as a 49 m fall after 0.02 s; she was
		// taken down again on every get-up
		constexpr float kMostG = 40.0f;  // m/s2, well over Skyrim's
		const float     fallen = 1.0f + 0.5f * kMostG * cc->fallTime * cc->fallTime;
		// and she is coming down: also seen, a controller left in the air for seconds on an NPC standing still
		RE::NiPoint3 vel;
		a_actor->GetLinearVelocity(vel);
		if (drop < highFallDrop_ || drop > fallen || vel.z * kUnit > -3.0f || !Eligible(a_actor, "high fall", true)) {
			return;
		}
		std::scoped_lock l(lock_);
		for (auto& p : active_) {
			if (p->actor.get().get() == a_actor) {
				return;
			}
		}
		for (auto& h : skip_) {
			if (h.get().get() == a_actor) {
				return;
			}
		}
		if (std::ranges::find(noFallBody_, a_actor->GetFormID()) != noFallBody_.end()) {
			return;
		}
		logger::info("{:08X} '{}': falling {:.1f} m so far ({:.2f} s in the air): high fall", a_actor->GetFormID(), a_actor->GetName(), drop, cc->fallTime);
		if (!Activate(a_actor)) {
			noFallBody_.push_back(a_actor->GetFormID());  // no Delirium body for this skeleton: not tried again
			return;
		}
		auto& a = *active_.back();
		dl.nm_msg_begin(a.agent, "stopAllBehaviours");
		dl.nm_msg_send(a.agent, a.world);
		dl.nm_msg_begin(a.agent, "highFall");
		dl.nm_msg_set_bool(a.agent, "start", 1);
		if (dl.nm_msg_send(a.agent, a.world) < 0) {
			logger::error("  high fall: {}", LastError());
		}
		a.flying = 1;
		a.flyMode = 2;
		a.ledgeFall = true;
		a.flyAt = a.age;
		a.braced = false;
		a.braceCheckAt = a.age + 0.1f;
		a.writheFor = writheTime_ + writheAdd_ * std::clamp(drop / 10.0f, 0.0f, 1.0f);
		a.writheOomph = std::clamp(drop / 10.0f, 0.0f, 1.0f);
		a.writheElement = -1;
		Reacting(a);
	}

	bool Engine::LedgeLanded(const Active& a) const
	{
		// her own drop: the head close over the ground under it, or the fall stopped (on a slope, a ledge, a body)
		float hp[3]{}, hq[4]{}, cp[3]{}, cv[3]{};
		dl.nm_agent_get_part_transform(a.agent, kHead, hp, hq);
		dl.nm_agent_get_com(a.agent, cp, cv);
		const float t = a.age - a.flyAt;
		const float down[3]{ 0.0f, 0.0f, -1.0f };
		float       dist = 0.0f, n[3]{};
		const bool  close = dl.nm_world_raycast(a.world, hp, down, 0.6f, &dist, n) > 0;
		return (a.flying == 2 && t > 0.3f && (close || cv[2] > -1.0f)) || t > 8.0f;
	}
}

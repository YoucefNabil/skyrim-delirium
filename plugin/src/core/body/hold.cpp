// Upright in Delirium her AI still runs: without this she draws and looses arrows, casts and swings while the body
// staggers. Skyrim's combat AI asks "can she act?" before any attack or cast and says no while the actor is
// staggered (actorState2 bit 13, timed by middleHigh's stagger timer). So while we hold her up we keep that flag
// on, cancel the cast or attack she was in when we took her, and the animation handlers that fire an arrow, a
// spell, a shout or a swing do nothing for her (hooks.cpp). Skyrim's own stagger running when we took her is
// remembered and handed back on release, so release still waits for it (update.cpp).
#include "shared.h"

namespace delirium
{
	namespace
	{
		std::mutex                g_holdLock;  // its own lock: the handlers run on animation threads, never under lock_
		std::vector<RE::Actor*>   g_heldUp;
		std::atomic<int>          g_heldUpCount{ 0 };

		void SetHeld(RE::Actor* a_actor, bool a_on)
		{
			std::scoped_lock l(g_holdLock);
			std::erase(g_heldUp, a_actor);
			if (a_on) {
				g_heldUp.push_back(a_actor);
			}
			g_heldUpCount = static_cast<int>(g_heldUp.size());
		}

		RE::MiddleHighProcessData* MiddleHigh(RE::Actor* a_actor)
		{
			auto* proc = a_actor->GetActorRuntimeData().currentProcess;
			return proc ? proc->middleHigh : nullptr;
		}
	}

	bool HoldsCombat(const RE::Actor* a_actor)
	{
		if (g_heldUpCount.load(std::memory_order_relaxed) == 0) {
			return false;
		}
		std::scoped_lock l(g_holdLock);
		return std::ranges::find(g_heldUp, a_actor) != g_heldUp.end();
	}

	void Engine::HoldCombat(Active& a, RE::Actor* a_actor, float a_dt)
	{
		auto* mh = MiddleHigh(a_actor);
		if (!mh) {
			return;
		}
		auto* st = a_actor->AsActorState();
		if (!a.holding) {
			a.holding = true;
			a.heldActor = a_actor;
			a.skyStagger = st->IsStaggered();
			a.skyStaggerLeft = a.skyStagger ? mh->unk2DC : 0.0f;
			// whatever she was doing stops now: a channelled spell (Flames), a drawn bow, a swing on its way
			a_actor->InterruptCast(false);
			a_actor->NotifyAnimationGraph("attackStop");
			SetHeld(a_actor, true);
			logger::info("{:08X}: combat held (Skyrim's stagger {}, {:.2f} s left)", a_actor->GetFormID(), a.skyStagger ? "running" : "not running", a.skyStaggerLeft);
		} else if (a.skyStagger) {
			a.skyStaggerLeft -= a_dt;
			a.skyStagger = a.skyStaggerLeft > 0.0f;
		}
		st->actorState2.staggered = 1;
		mh->unk2DC = std::max(mh->unk2DC, 0.5f);  // Skyrim counts it down and clears the flag at 0: kept topped up
	}

	void Engine::FreeCombat(Active& a)
	{
		if (!a.holding) {
			return;
		}
		a.holding = false;
		SetHeld(a.heldActor, false);
		a.heldActor = nullptr;
		auto act = a.actor.get();
		if (!act) {
			return;
		}
		if (auto* mh = MiddleHigh(act.get())) {
			if (a.skyStagger) {
				mh->unk2DC = a.skyStaggerLeft;  // her own stagger runs out as it would have
			} else {
				act->AsActorState()->actorState2.staggered = 0;
				mh->unk2DC = 0.0f;
			}
		}
		logger::info("{:08X}: combat free", act->GetFormID());
	}
}

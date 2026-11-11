// Debug: one NPC thrown into another, measured (the `throw` command). The test for [Bodies]: his ragdolled body has to
// stop against her standing one, and she has to react.
#include "shared.h"

#include "body/world.h"

namespace delirium
{
	namespace
	{
		// what a throw did, measured each frame: how near the two came, how hard she was shoved, whether he got past her
		struct Watch
		{
			RE::ActorHandle thrown, target;
			RE::NiPoint3    dir;   // of the throw, level
			RE::NiPoint3    from;  // where the push comes from
			float           force{};
			int             tick{}, frame{};  // his updates since the move, frames measured
			float           nearest{ 1e9f }, nearestAt{}, shoved{}, startGap{};
			RE::NiPoint3    targetWas;
		};

		std::mutex             g_lock;
		std::shared_ptr<Watch> g_watch;  // the throw being measured
		std::atomic<bool>      g_on{ false };

		// one frame of the measurement; false: done
		bool Step(Watch* a_w)
		{
			const auto a = a_w->thrown.get();
			const auto b = a_w->target.get();
			if (!a || !b) {
				return false;
			}
			if (++a_w->tick == 2) {
				// the frame after the move: Skyrim's own push, from behind him on the line
				g_tasks->AddTask([h = a_w->thrown, from = a_w->from, force = a_w->force]() {
					if (const auto t = h.get(); t && t->Get3D(false)) {
						if (auto* p = t->GetActorRuntimeData().currentProcess) {
							p->KnockExplosion(t.get(), from, force);
						}
					}
				});
			}
			const auto sa = world::Bodies(a.get());
			const auto sb = world::Bodies(b.get());
			if (sa.at.empty() || sb.at.empty()) {
				if (a_w->tick < 240) {
					return true;  // his ragdoll is not in the world yet
				}
				logger::warn("throw: no bodies to measure (his {}, hers {})", sa.at.size(), sb.at.size());
				return false;
			}
			const float along = (sa.mean - sb.mean).Dot(a_w->dir);  // < 0: still before her, > 0: past her
			if (a_w->frame == 0) {
				a_w->targetWas = sb.mean;
				a_w->startGap = along;
			}
			for (const auto& p : sa.at) {
				for (const auto& q : sb.at) {
					if (const float d = p.GetDistance(q); d < a_w->nearest) {
						a_w->nearest = d;
						a_w->nearestAt = a_w->frame / 60.0f;
					}
				}
			}
			a_w->shoved = std::max(a_w->shoved, sb.fastest);
			if (++a_w->frame < 120) {
				return true;
			}
			logger::info("throw {:08X} at {:08X}, {} frames: began {:.2f} m before her, ended {:.2f} m {} her; bodies came within {:.2f} m (at {:.2f} s); "
						 "her fastest body {:.2f} m/s, she moved {:.2f} m; bodies in the world: his {}, hers {}",
				a->GetFormID(), b->GetFormID(), a_w->frame, -a_w->startGap, std::abs(along), along > 0 ? "PAST" : "before", a_w->nearest, a_w->nearestAt,
				a_w->shoved, sb.mean.GetDistance(a_w->targetWas), sa.at.size(), sb.at.size());
			return false;
		}
	}

	void Engine::ThrowStep(RE::Actor* a_actor)
	{
		if (!g_on.load(std::memory_order_relaxed)) {
			return;
		}
		std::scoped_lock l(g_lock);
		if (g_watch && g_watch->thrown.get().get() == a_actor && !Step(g_watch.get())) {
			g_watch.reset();
			g_on = false;
		}
	}

	void Engine::Throw(RE::Actor* a_thrown, RE::Actor* a_target, float a_force)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player || !a_thrown->GetActorRuntimeData().currentProcess || a_thrown == a_target) {
			return;
		}
		// on the line from the player through her, one metre short of her: the throw goes away from the player, into her
		const auto   to = a_target->GetPosition();
		RE::NiPoint3 dir = to - player->GetPosition();
		dir.z = 0.0f;
		if (dir.Unitize() < 1.0f) {
			return;
		}
		auto pos = to - dir * 70.0f;
		pos.z += 5.0f;
		world::Remove(a_thrown);  // seen: with his bodies in the world the move does not take
		a_thrown->SetPosition(pos, true);
		a_thrown->SetAngle({ 0.0f, 0.0f, std::atan2(dir.x, dir.y) });
		auto w = std::make_shared<Watch>();
		w->thrown = a_thrown->CreateRefHandle();
		w->target = a_target->CreateRefHandle();
		w->dir = dir;
		w->from = pos - dir * 100.0f;
		w->force = a_force;
		{
			std::scoped_lock l(g_lock);
			g_watch = std::move(w);
			g_on = true;
		}
		logger::info("throw {:08X} at {:08X}: he stands 1 m before her, force {}", a_thrown->GetFormID(), a_target->GetFormID(), a_force);
	}
}

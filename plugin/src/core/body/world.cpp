// Skyrim's own Havok world: standing NPCs' ragdoll bodies in it, and biped bodies colliding (body/world.h).
//
// Skyrim builds a living NPC's ragdoll (18 bodies, keyframed, layer Biped) but keeps it out of the Havok world until
// she is knocked down, and its collision filter lets Biped bodies meet only static geometry (seen with Cheat Engine on
// 1.6.1170: bodies' world 0, graph's physicsWorld set, canAddRagdollToWorld 1; Biped's row has no Biped, Clutter or
// Props bit). So nothing a falling body does can reach a standing NPC, and two fallen living NPCs pass through each
// other. Here: the graph's own AddRagdollToWorld (BSIRagdollDriver slot 2; returns at once without physicsWorld) and
// the filter's Biped bits, as PLANCK and Precision do. Skyrim's ragdoll driver then keeps the bodies on her animation.
#include "shared.h"

#include "contacts.h"
#include "world.h"

#include <RE/H/hkpCapsuleShape.h>

namespace delirium::world
{
	namespace
	{
		using Layer = RE::COL_LAYER;

		std::mutex                                   g_lock;       // g_added
		std::vector<RE::ActorHandle>                 g_added;      // whose ragdoll we put in the world
		// the bits we set in the collision filter's layer rows that were not set before (main thread). Only those are
		// ours to clear: another mod (PLANCK, Precision) may have set its own in the same rows since
		std::optional<std::array<std::uint64_t, 64>> g_filterOurs;
		std::atomic<bool>                            g_changed{ false };

		// every graph of hers with a ragdoll, under the manager's update lock; false: none
		template <class F>
		bool ForGraphs(RE::Actor* a_actor, F&& a_do)
		{
			RE::BSTSmartPointer<RE::BSAnimationGraphManager> mgr;
			if (!a_actor->GetAnimationGraphManager(mgr) || !mgr) {
				return false;
			}
			RE::BSSpinLockGuard l(mgr->GetRuntimeData().updateLock);
			bool                any = false;
			for (auto& g : mgr->graphs) {
				if (g && g->characterInstance.ragdollDriver && g->characterInstance.ragdollDriver->ragdoll) {
					a_do(*g);
					any = true;
				}
			}
			return any;
		}

		RE::bhkWorld* CellWorld(RE::Actor* a_actor)
		{
			auto* cell = a_actor->GetParentCell();
			return cell ? cell->GetbhkWorld() : nullptr;
		}

		void Allow(RE::bhkCollisionFilter& a_f, std::array<std::uint64_t, 64>& a_ours, Layer a_a, Layer a_b)
		{
			auto set = [&](Layer a_row, Layer a_bit) {
				const auto bit = 1ull << std::to_underlying(a_bit);
				auto&      row = a_f.layerBitfields[std::to_underlying(a_row)];
				if (!(row & bit)) {
					row |= bit;
					a_ours[std::to_underlying(a_row)] |= bit;
				}
			};
			set(a_a, a_b);
			set(a_b, a_a);
		}
	}

	Sample Bodies(RE::Actor* a_actor)
	{
		Sample s;
		ForGraphs(a_actor, [&](RE::BShkbAnimationGraph& g) {
			for (auto* b : g.characterInstance.ragdollDriver->ragdoll->rigidBodies) {
				if (!b || !b->world) {
					continue;
				}
				const auto& p = b->motion.motionState.sweptTransform.centerOfMass1.quad.m128_f32;
				const auto& v = b->motion.linearVelocity.quad.m128_f32;
				s.at.emplace_back(p[0], p[1], p[2]);
				s.vel.emplace_back(v[0], v[1], v[2]);
				s.mean += s.at.back();
				s.fastest = std::max(s.fastest, std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
			}
		});
		if (!s.at.empty()) {
			s.mean /= static_cast<float>(s.at.size());
		}
		return s;
	}

	std::vector<Capsule> Capsules(RE::Actor* a_actor)
	{
		std::vector<Capsule> out;
		ForGraphs(a_actor, [&](RE::BShkbAnimationGraph& g) {
			for (auto* b : g.characterInstance.ragdollDriver->ragdoll->rigidBodies) {
				const auto* shape = b && b->world ? b->collidable.shape : nullptr;
				if (!shape) {
					continue;
				}
				float la[3]{}, lb[3]{}, r = 0.0f;
				if (shape->type == RE::hkpShapeType::kCapsule) {
					const auto* cs = static_cast<const RE::hkpCapsuleShape*>(shape);
					std::memcpy(la, &cs->vertexA, sizeof(la));
					std::memcpy(lb, &cs->vertexB, sizeof(lb));
					r = cs->radius;
				} else {
					RE::hkAabb      box;
					RE::hkTransform id{};
					id.rotation.col0.quad.m128_f32[0] = 1.0f;
					id.rotation.col1.quad.m128_f32[1] = 1.0f;
					id.rotation.col2.quad.m128_f32[2] = 1.0f;
					shape->GetAabbImpl(id, 0.0f, box);
					const auto& lo = box.min.quad.m128_f32;
					const auto& hi = box.max.quad.m128_f32;
					float       h[3]{}, m[3]{};
					int         ax = 0;
					for (int i = 0; i < 3; ++i) {
						h[i] = (hi[i] - lo[i]) * 0.5f;
						m[i] = (hi[i] + lo[i]) * 0.5f;
						ax = h[i] > h[ax] ? i : ax;
					}
					r = std::min(h[(ax + 1) % 3], h[(ax + 2) % 3]);
					std::memcpy(la, m, sizeof(la));
					std::memcpy(lb, m, sizeof(lb));
					la[ax] -= std::max(0.0f, h[ax] - r);
					lb[ax] += std::max(0.0f, h[ax] - r);
				}
				if (!(r > 0.0f) || r > 1.0f) {
					continue;
				}
				const auto& xf = b->motion.motionState.transform;
				const auto& c0 = xf.rotation.col0.quad.m128_f32;
				const auto& c1 = xf.rotation.col1.quad.m128_f32;
				const auto& c2 = xf.rotation.col2.quad.m128_f32;
				const auto& t = xf.translation.quad.m128_f32;
				auto        out3 = [&](const float* l) {
					return RE::NiPoint3(c0[0] * l[0] + c1[0] * l[1] + c2[0] * l[2] + t[0], c0[1] * l[0] + c1[1] * l[1] + c2[1] * l[2] + t[1],
						c0[2] * l[0] + c1[2] * l[1] + c2[2] * l[2] + t[2]);
				};
				const auto& v = b->motion.linearVelocity.quad.m128_f32;
				out.push_back({ out3(la), out3(lb), r, RE::NiPoint3(v[0], v[1], v[2]) });
			}
		});
		return out;
	}

	Capsule Standing(RE::Actor* a_actor)
	{
		constexpr float kRadius = 0.25f;  // metres: a torso
		const float     s = RE::bhkWorld::GetWorldScale();
		const auto      feet = a_actor->GetPosition() * s;
		const float     tall = std::max(a_actor->GetHeight() * s, 2.0f * kRadius);
		RE::NiPoint3    vel;
		a_actor->GetLinearVelocity(vel);
		return { feet + RE::NiPoint3(0.0f, 0.0f, kRadius), feet + RE::NiPoint3(0.0f, 0.0f, tall - kRadius), kRadius, vel * s };
	}

	bool Add(RE::Actor* a_actor, bool a_log)
	{
		auto* world = CellWorld(a_actor);
		if (!world) {
			if (a_log) {
				logger::warn("world: {:08X} has no Havok world", a_actor->GetFormID());
			}
			return false;
		}
		bool                           added = false;
		std::vector<RE::hkpRigidBody*> bodies;
		const bool                     found = ForGraphs(a_actor, [&](RE::BShkbAnimationGraph& g) {
			if (!g.physicsWorld) {
				g.physicsWorld = world;
			}
			RE::BSWriteLockGuard wl(world->worldLock);
			const bool           ok = g.AddRagdollToWorld();
			if (ok) {
				g.SetRagdollConstraintsFromBhkConstraints();
				for (auto* b : g.characterInstance.ragdollDriver->ragdoll->rigidBodies) {
					bodies.push_back(b);
				}
			}
			added = added || ok;
		});
		if (a_log) {
			logger::info("world: {:08X} '{}' ragdoll {}", a_actor->GetFormID(), a_actor->GetName(),
				!found ? "missing" : added ? "added to Skyrim's world" : "NOT added to Skyrim's world");
		}
		if (added) {
			{
				RE::BSWriteLockGuard wl(world->worldLock);
				contacts::Listen(a_actor->GetFormID(), bodies);
			}
			const auto       h = a_actor->CreateRefHandle();
			std::scoped_lock l(g_lock);
			// the list only grows by who stands here now: the ones that are gone make room
			std::erase_if(g_added, [&](const RE::ActorHandle& x) { return x == h || !x.get(); });
			g_added.push_back(h);
			g_changed = true;
		}
		return added;
	}

	bool Remove(RE::Actor* a_actor)
	{
		bool removed = false;
		ForGraphs(a_actor, [&](RE::BShkbAnimationGraph& g) {
			// seen: the graph's physicsWorld is null again while its bodies are still in the cell's world
			if (auto* world = g.physicsWorld ? g.physicsWorld : CellWorld(a_actor)) {
				if (!g.physicsWorld) {
					g.physicsWorld = world;
				}
				RE::BSWriteLockGuard wl(world->worldLock);
				removed = g.RemoveRagdollFromWorld() || removed;
			}
		});
		if (auto* world = CellWorld(a_actor)) {
			RE::BSWriteLockGuard wl(world->worldLock);
			contacts::Forget(a_actor->GetFormID());
		} else {
			contacts::Forget(a_actor->GetFormID());
		}
		std::scoped_lock l(g_lock);
		std::erase(g_added, a_actor->CreateRefHandle());
		return removed;
	}

	std::vector<RE::ActorHandle> Added()
	{
		std::scoped_lock l(g_lock);
		return g_added;
	}

	void Collide()
	{
		if (g_filterOurs) {
			return;
		}
		auto* f = RE::bhkCollisionFilter::GetSingleton();
		if (!f) {
			return;
		}
		auto& ours = g_filterOurs.emplace();
		// a standing NPC's bodies are on Biped; a seated or furniture-bound one's on BipedNoCC (seen: Lydia, Bassianus
		// in the inn); a knocked-down one's on DeadBip, living or not (seen: Lydia in the air after Skyrim's push)
		for (const auto a : { Layer::kBiped, Layer::kBipedNoCC, Layer::kDeadBip }) {
			Allow(*f, ours, a, Layer::kBiped);
			Allow(*f, ours, a, Layer::kBipedNoCC);
			Allow(*f, ours, a, Layer::kDeadBip);
		}
		g_changed = true;
		logger::info("world: biped bodies meet each other; Biped's row {:016X} (our bits {:016X})", f->layerBitfields[std::to_underlying(Layer::kBiped)],
			ours[std::to_underlying(Layer::kBiped)]);
	}

	bool Changed()
	{
		return g_changed.load(std::memory_order_relaxed);
	}

	void Leave()
	{
		if (g_filterOurs) {
			if (auto* f = RE::bhkCollisionFilter::GetSingleton()) {
				for (std::size_t i = 0; i < g_filterOurs->size(); ++i) {
					f->layerBitfields[i] &= ~(*g_filterOurs)[i];
				}
				logger::info("world: our filter bits off again");
			}
			g_filterOurs.reset();
		}
		std::vector<RE::ActorHandle> added;
		{
			std::scoped_lock l(g_lock);
			added.swap(g_added);
		}
		// Skyrim's own ragdoll (she was knocked down meanwhile) stays in: only the standing ones go back out
		int out = 0;
		for (const auto& h : added) {
			if (const auto a = h.get(); a && !a->IsInRagdollState()) {
				out += Remove(a.get());
			}
		}
		contacts::ForgetAll();
		g_changed = false;
		if (!added.empty()) {
			logger::info("world: {} standing ragdolls out of Skyrim's world again", out);
		}
	}

	void Log(RE::Actor* a_actor)
	{
		logger::info("world: {}", Heard());
		ForGraphs(a_actor, [&](RE::BShkbAnimationGraph& g) {
			auto* d = g.characterInstance.ragdollDriver.get();
			int   in = 0, n = 0;
			int   motions[16]{};
			int   layers[64]{};
			for (auto* b : d->ragdoll->rigidBodies) {
				if (!b) {
					continue;
				}
				++n;
				in += b->world ? 1 : 0;
				++motions[std::to_underlying(b->motion.type.get()) & 15];
				++layers[std::to_underlying(b->collidable.GetCollisionLayer()) & 63];
			}
			std::string m, l;
			for (int i = 0; i < 16; ++i) {
				if (motions[i]) {
					m += std::format(" {}x motion {}", motions[i], i);
				}
			}
			for (int i = 0; i < 64; ++i) {
				if (layers[i]) {
					l += std::format(" {}x layer {}", layers[i], i);
				}
			}
			logger::info("world: {:08X} '{}': {} bodies, {} in a world;{};{}; physicsWorld {}, driver enabled {}, powered {}, rigid-body {}, all keyframed {}",
				a_actor->GetFormID(), a_actor->GetName(), n, in, m, l, g.physicsWorld ? "set" : "null", d->isEnabled, d->isPoweredControllerEnabled,
				d->isRigidBodyControllerEnabled, d->allBonesKeyframed);
		});
	}
}

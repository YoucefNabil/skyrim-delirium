// Havok's own contact events on ragdoll bodies (body/contacts.h, world::Touches in body/world.h).
//
// A listener on the body itself (hkpEntity::AddContactListener): Havok calls it for that body's contacts only, so
// nothing global is hooked and no other mod's listener is in the way. The call comes from whichever thread steps the
// island, in the middle of the solver: it only measures the contact and puts it in a small buffer.
// Each body listened on is held by a reference until the listener is off it again, so taking the listener off is
// safe whatever Skyrim did with the ragdoll meanwhile.
#include "shared.h"

#include "contacts.h"
#include "world.h"

#include <RE/H/hkContactPoint.h>
#include <RE/H/hkpContactListener.h>
#include <RE/H/hkpContactPointEvent.h>

namespace delirium::world
{
	namespace
	{
		constexpr std::size_t kMost = 128;  // touches kept between two Touches calls
		constexpr float       kLeast = 0.3f;  // m/s towards each other below which a contact is resting, not a touch

		std::mutex         g_touchLock;  // g_touches: Havok's threads and whoever takes them
		std::vector<Touch> g_touches;
		std::atomic<int>   g_pending{ 0 };
		std::atomic<std::uint32_t> g_heard{ 0 }, g_moving{ 0 }, g_kept{ 0 };  // trace: callbacks, those against a moving body, touches kept

		RE::NiPoint3 P(const RE::hkVector4& a_v)
		{
			const auto& f = a_v.quad.m128_f32;
			return { f[0], f[1], f[2] };
		}

		// the body's velocity at a point of it
		RE::NiPoint3 VelocityAt(const RE::hkpRigidBody& a_body, const RE::NiPoint3& a_at)
		{
			const auto r = a_at - P(a_body.motion.motionState.sweptTransform.centerOfMass1);
			return P(a_body.motion.linearVelocity) + P(a_body.motion.angularVelocity).Cross(r);
		}

		class Ears final : public RE::hkpContactListener
		{
		public:
			explicit Ears(RE::FormID a_who) :
				who_(a_who) {}
			Ears(const Ears&) = delete;
			Ears& operator=(const Ears&) = delete;
			~Ears() override { Off(); }

			void On(std::span<RE::hkpRigidBody* const> a_bodies)
			{
				for (auto* b : a_bodies) {
					if (b && !std::ranges::contains(bodies_, b)) {
						b->AddReference();
						b->AddContactListener(this);
						bodies_.push_back(b);
					}
				}
			}

			void Off()
			{
				for (auto* b : bodies_) {
					b->RemoveContactListener(this);
					b->RemoveReference();
				}
				bodies_.clear();
			}

			[[nodiscard]] RE::FormID Who() const { return who_; }
			// Skyrim took her ragdoll out of the world (her 3D went, or our own Remove): nothing left to hear
			[[nodiscard]] bool Idle() const
			{
				return std::ranges::none_of(bodies_, [](const RE::hkpRigidBody* b) { return b->world != nullptr; });
			}

			void ContactPointCallback(const RE::hkpContactPointEvent& a_event) override
			{
				// the event names the body this listener hangs on (the world's own listeners get kSourceWorld)
				g_heard.fetch_add(1, std::memory_order_relaxed);
				const auto src = std::to_underlying(a_event.source);
				if (src > 1 || !a_event.contactPoint) {
					return;
				}
				const auto* mine = a_event.bodies[src];
				const auto* other = a_event.bodies[1 - src];
				if (!mine || !other || other->motion.type == RE::hkpMotion::MotionType::kFixed) {
					return;  // the ground and the walls: no news
				}
				g_moving.fetch_add(1, std::memory_order_relaxed);
				const auto at = P(a_event.contactPoint->position);
				// Havok's contact normal points from body B to body A; here: from the other body to hers
				const auto n = P(a_event.contactPoint->separatingNormal) * (src == 0 ? 1.0f : -1.0f);
				const auto theirs = VelocityAt(*other, at);
				const auto vel = theirs - VelocityAt(*mine, at);
				const float closing = vel.Dot(n);
				if (closing < kLeast) {
					return;
				}
				const auto* ref = RE::TESHavokUtilities::FindCollidableRef(*other->GetCollidable());
				const auto  by = ref ? ref->GetFormID() : RE::FormID{};
				if (by == who_) {
					return;  // her own bodies against each other
				}
				std::scoped_lock l(g_touchLock);
				// one touch per pair between two takes: the hardest
				if (const auto it = std::ranges::find_if(g_touches, [&](const Touch& t) { return t.who == who_ && t.by == by; }); it != g_touches.end()) {
					if (closing <= it->closing) {
						return;
					}
					g_touches.erase(it);
				} else if (g_touches.size() >= kMost) {
					return;
				}
				g_kept.fetch_add(1, std::memory_order_relaxed);
				g_touches.push_back({ who_, by, other->collidable.GetCollisionLayer(), at, vel, closing, theirs.Dot(n) });
				g_pending.store(static_cast<int>(g_touches.size()), std::memory_order_relaxed);
			}

		private:
			RE::FormID                     who_;
			std::vector<RE::hkpRigidBody*> bodies_;  // each held by a reference while we are on it
		};

		std::mutex                         g_earsLock;  // g_ears (the main thread changes it; kept for the day it is not alone)
		std::vector<std::unique_ptr<Ears>> g_ears;
	}

	namespace contacts
	{
		void Listen(RE::FormID a_who, std::span<RE::hkpRigidBody* const> a_bodies)
		{
			std::scoped_lock l(g_earsLock);
			// the ones Skyrim took out of the world meanwhile make room, and her own old bodies go
			std::erase_if(g_ears, [&](const auto& e) { return e->Who() == a_who || e->Idle(); });
			g_ears.push_back(std::make_unique<Ears>(a_who));
			g_ears.back()->On(a_bodies);
		}

		void Forget(RE::FormID a_who)
		{
			std::scoped_lock l(g_earsLock);
			std::erase_if(g_ears, [&](const auto& e) { return e->Who() == a_who; });
		}

		void ForgetAll()
		{
			{
				std::scoped_lock l(g_earsLock);
				g_ears.clear();
			}
			std::scoped_lock l(g_touchLock);
			g_touches.clear();
			g_pending = 0;
		}
	}

	std::string Heard()
	{
		return std::format("{} contacts heard, {} against a moving body, {} touches kept", g_heard.load(), g_moving.load(), g_kept.load());
	}

	std::vector<Touch> Touches()
	{
		if (g_pending.load(std::memory_order_relaxed) == 0) {
			return {};
		}
		std::vector<Touch> out;
		std::scoped_lock   l(g_touchLock);
		out.swap(g_touches);
		g_pending = 0;
		return out;
	}
}

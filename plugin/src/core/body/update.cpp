// Every frame: after the animation (the physics steps), after the root move, after the pose.
#include "shared.h"

namespace delirium
{
	namespace
	{
		constexpr int   kMaxSteps = 4;

		constexpr float               kBlendOut = 0.4f;  // seconds from Delirium's pose back to the animation
		constexpr float               kCorpseHold = 30.0f;  // seconds a corpse is driven at most, still or not
		constexpr float               kRunawayKE = 50.0f;   // energy above which growth while down counts as running away
		constexpr int                 kRunawaySeconds = 5;  // seconds in a row of that before Skyrim's ragdoll takes over

		// Mu Joint Fix's height fix moves the body (its "MJF" nodes between COM and the pelvis) to keep standing feet out of
		// the ground, and turns itself off when it sees the pelvis touch it: on a falling body that throws the hips 20 cm
		// around and back. The ragdoll owns the hips while it falls, so the offset goes. True when one was there
		bool ClearJointFixOffset(RE::NiAVObject* a_pelvis, RE::NiAVObject* a_com)
		{
			bool had = false;
			for (RE::NiAVObject* n = a_pelvis ? a_pelvis->parent : nullptr; n && n != a_com; n = n->parent) {
				if (std::strncmp(n->name.c_str(), "MJF ", 4) == 0 && (n->local.translate.x != 0.0f || n->local.translate.y != 0.0f || n->local.translate.z != 0.0f)) {
					n->local.translate = {};
					had = true;
				}
			}
			return had;
		}

		// still the 3D she was taken on (Active::root): else every bone we hold is the old one's
		template <class A>
		bool Same3D(const A& a_active, RE::Actor* a_actor)
		{
			return a_actor->Get3D(false) == a_active.root.get();
		}
	}

	namespace
	{
		// Each node's previousWorld is what TAA, the upscaler and motion blur take as last frame. UpdateWorldData sets it to
		// the world it replaces, and in WriteBones that is Skyrim's fresh animated pose, never drawn: motion vectors would
		// point from a pose that was never on screen (in a stagger, far from ours).
		// Put back the world each node was drawn with last frame, then remember this frame's.
		void KeepPrevious(RE::NiAVObject* a_root, std::unordered_map<RE::NiAVObject*, RE::NiTransform>& a_drawn)
		{
			std::unordered_map<RE::NiAVObject*, RE::NiTransform> now;
			now.reserve(a_drawn.size() + 16);
			std::vector<RE::NiAVObject*> stack{ a_root };
			while (!stack.empty()) {
				auto* n = stack.back();
				stack.pop_back();
				if (const auto it = a_drawn.find(n); it != a_drawn.end()) {
					n->previousWorld = it->second;
				}
				now.emplace(n, n->world);
				if (auto* node = n->AsNode()) {
					for (auto& c : node->GetChildren()) {
						if (c) {
							stack.push_back(c.get());
						}
					}
				}
			}
			a_drawn = std::move(now);
		}
	}

	void Engine::AfterRootMove(RE::Actor* a_actor)
	{
		std::scoped_lock l(lock_);
		for (auto& p : active_) {
			auto& a = *p;
			if (!a.knockQueued || a.actor.get().get() != a_actor) {
				continue;
			}
			auto* top = a_actor->Get3D(false);
			if (!top || !Same3D(a, a_actor)) {
				return;  // a new 3D: our bones are the old one's (AfterAnimation lets her go)
			}
			struct TraceAfter
			{
				Engine* e;
				Active& a;
				RE::Actor* act;
				~TraceAfter() { e->Trace(a, act, "root move"); }
			} traceAfter{ this, a, a_actor };
			RE::NiUpdateData ud{};
			ClearJointFixOffset(a.ref[0], a.com);  // again: it can run between the pose copy and here
			if (!TorqueMode(a) && a.ragdolled && a.haveCom) {
				// lead-in: ours moved toward Skyrim's COM, straight from its body (bone = body * (mapR, mapT)^-1)
				for (const auto& b : a.sky.bodies) {
					if (b.node != a.com) {
						continue;
					}
					const auto& ms = b.body->motion.motionState;
					const auto* cl = reinterpret_cast<const float*>(&ms.sweptTransform.centerOfMassLocal);
					const auto* cw = reinterpret_cast<const float*>(&ms.sweptTransform.centerOfMass1);
					const auto* qc = reinterpret_cast<const float*>(&ms.sweptTransform.rotation1);
					const Q     qb{ qc[0], qc[1], qc[2], qc[3] };
					const V3    pb = Sub({ cw[0], cw[1], cw[2] }, Rot(qb, { cl[0], cl[1], cl[2] }));
					const Q     nr = Mul(qb, Conj(b.mapR));
					V3          np = Scl(Sub(pb, Rot(nr, Scl(b.mapT, a.scale))), 1.0f / kUnit);
					const float s = LeadShare(a);
					if (a.haveSkyCom) {
						// Skyrim's own pose copy draws COM a steady ~4 cm up from its body (seen in game, cause unknown), and
						// draws it so after the lead-in: the lead-in ends on its position, not the body's
						top->UpdateWorldData(&ud);
						std::vector<RE::NiAVObject*> up;
						for (auto* n = a.com->parent; n && n != top; n = n->parent) {
							up.push_back(n);
						}
						for (auto it = up.rbegin(); it != up.rend(); ++it) {
							(*it)->UpdateWorldData(&ud);
						}
						const RE::NiTransform sky = a.com->parent->world * a.skyComLocal;
						const RE::NiPoint3    d = (sky.translate - RE::NiPoint3(np.x, np.y, np.z)) * kUnit;
						const RE::NiPoint3    dl = top->world.rotate.Transpose() * d;
						logger::info("    lag {:.3f} lead: Skyrim's COM vs body t1 {:.3f} m (top frame {:.3f} {:.3f} {:.3f}), top scale {:.3f}", a.age, d.Length(), dl.x, dl.y, dl.z,
							top->world.scale);
						np = { sky.translate.x, sky.translate.y, sky.translate.z };
						logger::info("    blend {:.3f} s {:.2f}: ours {:.1f} {:.1f} {:.1f} sky {:.1f} {:.1f} {:.1f} top {:.1f} {:.1f} {:.1f}", a.age, s, a.comWorld.translate.x,
							a.comWorld.translate.y, a.comWorld.translate.z, sky.translate.x, sky.translate.y, sky.translate.z, top->world.translate.x, top->world.translate.y,
							top->world.translate.z);
					}
					if (std::isfinite(np.x + np.y + np.z + nr.w)) {
						a.comWorld.rotate = ToMat(Slerp(nr, FromMat(a.comWorld.rotate), s));
						a.comWorld.translate = RE::NiPoint3(np.x, np.y, np.z) + (a.comWorld.translate - RE::NiPoint3(np.x, np.y, np.z)) * s;
					}
				}
			}
			if (TorqueMode(a)) {
				// Skyrim's ragdoll poses the bones: its pose copy set them for the top node it has only just put on the
				// body, so COM's world is taken under that top node, then kept while ours glides after it
				top->UpdateWorldData(&ud);
				std::vector<RE::NiAVObject*> up;
				for (auto* n = a.com->parent; n && n != top; n = n->parent) {
					up.push_back(n);
				}
				for (auto it = up.rbegin(); it != up.rend(); ++it) {
					(*it)->UpdateWorldData(&ud);
				}
				a.comWorld = a.com->parent->world * a.com->local;
				if (a.age - a.ragdollAge < 0.45f) {
					// trace: which body state Skyrim's pose copy drew COM from (t1 = after the last step, t0 = before it)
					for (const auto& b : a.sky.bodies) {
						if (b.node != a.com) {
							continue;
						}
						const auto& st = b.body->motion.motionState.sweptTransform;
						const auto* cl = reinterpret_cast<const float*>(&st.centerOfMassLocal);
						auto bonePos = [&](const float* cw, const float* qc) {
							const Q  qb{ qc[0], qc[1], qc[2], qc[3] };
							const V3 pb = Sub({ cw[0], cw[1], cw[2] }, Rot(qb, { cl[0], cl[1], cl[2] }));
							const Q  nr = Mul(qb, Conj(b.mapR));
							const V3 np = Scl(Sub(pb, Rot(nr, Scl(b.mapT, a.scale))), 1.0f / kUnit);
							return RE::NiPoint3(np.x, np.y, np.z);
						};
						const auto  p1 = bonePos(reinterpret_cast<const float*>(&st.centerOfMass1), reinterpret_cast<const float*>(&st.rotation1));
						const auto  p0 = bonePos(reinterpret_cast<const float*>(&st.centerOfMass0), reinterpret_cast<const float*>(&st.rotation0));
						const auto* tt = reinterpret_cast<const float*>(&st.centerOfMass0);
						const RE::NiPoint3 dl = top->world.rotate.Transpose() * ((a.comWorld.translate - p1) * kUnit);
						logger::info("    lag {:.3f}: drawn COM to body t1 {:.3f} m (top frame {:.3f} {:.3f} {:.3f}), to t0 {:.3f} m, t1-t0 {:.3f} m, top scale {:.3f}", a.age,
							(a.comWorld.translate - p1).Length() * kUnit, dl.x, dl.y, dl.z, (a.comWorld.translate - p0).Length() * kUnit, (p1 - p0).Length() * kUnit,
							top->world.scale);
						(void)tt;
						{
							// trace: every node from the top down to the pelvis (local translation, game units)
							std::vector<RE::NiAVObject*> ch;
							for (RE::NiAVObject* n = a.ref[0]; n; n = n->parent) {
								ch.push_back(n);
								if (n == top) {
									break;
								}
							}
							std::string cs;
							for (auto it = ch.rbegin(); it != ch.rend(); ++it) {
								cs += fmt::format(" {}({:.1f} {:.1f} {:.1f})", (*it)->name.c_str(), (*it)->local.translate.x, (*it)->local.translate.y, (*it)->local.translate.z);
							}
							logger::info("    lag {:.3f} chain:{}", a.age, cs);
						}
						// trace: the bones between COM and the first bodies (no body of their own): what poses them
						for (RE::NiAVObject* n : std::array<RE::NiAVObject*, 2>{ a.ref[0], a.ref[0] ? static_cast<RE::NiAVObject*>(a.ref[0]->parent) : nullptr }) {
							if (!n) {
								continue;
							}
							const auto& lr = n->local.rotate;
							logger::info("    lag {:.3f}: {} (parent {}) local t {:.1f} {:.1f} {:.1f} r {:.2f} {:.2f} {:.2f} / {:.2f} {:.2f} {:.2f}", a.age, n->name.c_str(),
								n->parent ? n->parent->name.c_str() : "-", n->local.translate.x, n->local.translate.y, n->local.translate.z, lr.entry[0][0], lr.entry[0][1],
								lr.entry[0][2], lr.entry[1][0], lr.entry[1][1], lr.entry[1][2]);
						}
					}
				}
			}
			// 1. the top node glides after Skyrim's instead of jumping (FSMP and SMP gear simulate relative to it)
			SmoothRoot(a, a_actor);
			if (!a.haveCom && !TorqueMode(a)) {
				return;
			}
			// 2. the nodes between it and COM, refreshed from it (only those: the bones must not move even for a moment)
			std::vector<RE::NiAVObject*> chain;
			for (auto* n = a.com->parent; n && n != top; n = n->parent) {
				chain.push_back(n);
			}
			for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
				(*it)->UpdateWorldData(&ud);
			}
			// 3. COM keeps the world transform the last pose copy gave it, and with it every bone below
			const auto& pw = a.com->parent->world;
			const auto  before = pw.rotate * (a.com->local.translate * pw.scale) + pw.translate;
			const float off = (before - a.comWorld.translate).Length();
			const auto  inv = pw.rotate.Transpose();
			a.com->local.rotate = inv * a.comWorld.rotate;
			a.com->local.translate = (inv * (a.comWorld.translate - pw.translate)) / pw.scale;
			a.com->local.scale = a.comWorld.scale / pw.scale;
			a.com->world = a.comWorld;
			a.maxRootFix = std::max(a.maxRootFix, off);
			if (off > 1.0f && a.rootFixLog < 10) {
				++a.rootFixLog;
				logger::info("    root moved after the pose: COM would have moved {:.1f} units, held (age {:.3f}, top node lags {:.2f})", off, a.age,
					a.rootLag);
			}
			return;
		}
	}

	void Engine::AfterAnimation(RE::Actor* a_actor, float a_delta)
	{
		ThrowStep(a_actor);
		Meet(a_actor);
		if (takeOver_ && ready_ && !a_actor->IsPlayerRef()) {
			if (a_actor->IsInRagdollState()) {
				TryTakeOver(a_actor, a_actor->IsDead());
			} else if (g_skip.load(std::memory_order_relaxed) != 0) {
				ForgetSkip(a_actor);
			}
		}
		if (g_held.load(std::memory_order_relaxed) != 0) {
			CheckHeld(a_actor);
		}
		if (g_count.load(std::memory_order_relaxed) == 0) {
			return;
		}
		std::scoped_lock l(lock_);
		for (std::size_t i = 0; i < active_.size(); ++i) {
			auto& a = *active_[i];
			if (a.actor.get().get() != a_actor) {
				continue;
			}
			if (!a_actor->Get3D(false) || a_actor->IsDisabled()) {
				Release(i, "actor gone");
				return;
			}
			if (!Same3D(a, a_actor)) {
				Release(i, "her 3D was replaced (a script: outfit, disable / enable, a scene)");
				return;
			}
			if (a.death && !a_actor->IsDead()) {
				Release(i, "alive again");
				return;
			}
			if (a_actor->IsDead() && !a.death) {
				a.death = true;
				a.knockQueued = true;  // Skyrim's death ragdoll comes on by itself: no knock of ours
				a.writheAt = a.writheUntil = -1.0f;
				a.reelUntil = -1.0f;
				a.burnUntil = a.shockUntil = a.chillUntil = -1.0f;
				a.blend = 1.0f;
				a.blendingOut = false;
				a.settled = 0.0f;
				const bool held = a.heldDeath || heldDied_.contains(a_actor->GetFormID());
				heldDied_.erase(a_actor->GetFormID());
				if (!held) {
					dl.nm_msg_begin(a.agent, "stopAllBehaviours");
					dl.nm_msg_send(a.agent, a.world);
				}
				float cp[3]{}, cv[3]{};
				dl.nm_agent_get_com(a.agent, cp, cv);
				logger::info("{:08X}: died while Delirium has her ({}{}): dying in Delirium", a_actor->GetFormID(),
					a.ragdolled ? "in Skyrim's ragdoll" : "on her feet", held ? ", after dying on her feet" : "");
				StartDying(a, { cv[0], cv[1], 0.0f }, held);
			}
			const float dt = std::clamp(a_delta, 0.0f, 0.1f);
			++a.calls;
			a.dtSum += a_delta;
			a.age += dt;
			if (a.death) {
				a.deadFor += dt;
			}
			a.sincePush += dt;
			// the bones were already posed from the last step, inside the animation update (AfterPose)
			// every frame advances by exactly its own time, split into equal steps of at most kStep: a fixed step with
			// an accumulator runs 0 steps one frame and 2 the next, and that stutter shakes CBPC/SMP physics
			if (dt > 0.0f && TorqueMode(a)) {
				if (!a.following) {
					dl.nm_agent_set_mode(a.agent, NM_MODE_FOLLOW);
					a.following = true;
					MapParts(a);
				}
				SyncFromSkyrim(a);
				a.haveDrv = false;
				if (dl.nm_agent_step(a.agent, a.world, dt) < 0) {
					if (!a.logged) {
						logger::error("step (follow): {}", LastError());
						a.logged = true;
					}
				} else {
					a.haveDrv = dl.nm_agent_get_part_drives(a.agent, a.drv.data(), 21) == 21 &&
					            dl.nm_agent_get_joint_accels(a.agent, a.acc.data(), 21) == 21 &&
					            dl.nm_agent_get_part_ext_torques(a.agent, a.ext.data(), 21) == 21;
				}
			} else if (dt > 0.0f) {
				if (a.following) {
					dl.nm_agent_set_mode(a.agent, NM_MODE_LEAD);
					a.following = false;
				}
				const int   steps = std::clamp(int(std::ceil(dt / kStep - 0.05f)), 1, kMaxSteps);
				const float h = dt / float(steps);
				for (int n = 0; n < steps; ++n) {
					if (dl.nm_agent_step(a.agent, a.world, h) < 0 && !a.logged) {
						logger::error("step: {}", LastError());
						a.logged = true;
					}
				}
			}
			if (collision_) {
				// Skyrim's collision was gathered around one spot: thrown far (Fus Ro Dah) she would fly through walls
				// and out of the map past its edge. Past 40% of the patch, gather a new one around her.
				float cp[3]{}, cv[3]{};
				dl.nm_agent_get_com(a.agent, cp, cv);
				const RE::NiPoint3 at = a.origin + RE::NiPoint3(cp[0], cp[1], cp[2]) / kUnit;
				const float        dx = at.x - a.colCenter.x, dy = at.y - a.colCenter.y;
				if (std::isfinite(at.x + at.y + at.z) &&
					(dx * dx + dy * dy > 0.16f * colRadius_ * colRadius_ || std::abs(at.z - a.colCenter.z) > 0.4f * colHeight_)) {
					if (auto* w = dl.nm_world_create()) {
						dl.nm_world_destroy(a.world);
						a.world = w;
						AddWorld(a, a_actor, at, true);
					}
				}
			}
			const bool  fallen = dl.nm_agent_has_fallen(a.agent) != 0;
			const bool  balancing = dl.nm_agent_is_balancing(a.agent) != 0;
			const float ke = dl.nm_agent_get_kinetic_energy(a.agent);
			if (static_cast<int>(a.age) != static_cast<int>(a.age - dt)) {
				float head[3]{}, q[4]{};
				dl.nm_agent_get_part_transform(a.agent, kHead, head, q);
				logger::info("  t {:.0} s: head {:.2} m, KE {:.1}, balancing {}, fallen {}; {} animation updates ({:.0} per s of their time), {} pose copies", a.age, head[2], ke,
					balancing, fallen, a.calls, a.dtSum > 0 ? a.calls / a.dtSum : 0.0f, a.poses);
				// down and gaining energy second after second: nothing pushes her, so the simulation is running away
				a.keRising = (a.knockQueued && ke > kRunawayKE && a.keLast >= 0.0f && ke > a.keLast * 1.1f) ? a.keRising + 1 : 0;
				a.keLast = ke;
				if (a.filled) {
					logger::info("  {} frames without Skyrim's pose copy: ours put on after its update", a.filled);  // trace
				}
				a.filled = 0;
				a.calls = 0;
				a.poses = 0;
				a.dtSum = 0;
				if (a.haveShot) {
					float pp[3]{}, pq[4]{};
					dl.nm_agent_get_part_transform(a.agent, 0, pp, pq);
					const float mx = pp[0] - a.shotPelvis.x, my = pp[1] - a.shotPelvis.y;
					logger::info("  since the arrow: pelvis {:.2f} m along its flight, {:.2f} m across", mx * a.shotDir.x + my * a.shotDir.y,
						-mx * a.shotDir.y + my * a.shotDir.x);
				}
			}
			a.wasBalancing |= balancing;
			if (a.flying > 0 && (!a.knockQueued || a.takeover)) {
				float cp[3]{}, cv[3]{};
				dl.nm_agent_get_com(a.agent, cp, cv);
				// thrown at something (a wall, a rock, the ground coming up): arms out at it
				if (!a.braced && a.flyMode != 3 && a.age >= a.braceCheckAt) {
					a.braceCheckAt = a.age + 0.1f;
					const float sp = std::sqrt(cv[0] * cv[0] + cv[1] * cv[1] + cv[2] * cv[2]);
					if (sp > 1.0f) {
						const float dir[3]{ cv[0] / sp, cv[1] / sp, cv[2] / sp };
						float       dist = 0.0f, n[3]{};
						if (dl.nm_world_raycast(a.world, cp, dir, 0.5f + 0.35f * sp, &dist, n) > 0) {
							const float pos[3]{ cp[0] + dir[0] * dist, cp[1] + dir[1] * dist, cp[2] + dir[2] * dist };
							a.braced = true;
							if (a.flyMode != 2) {
								dl.nm_msg_begin(a.agent, "armsWindmillAdaptive");
								dl.nm_msg_set_bool(a.agent, "start", 0);
								dl.nm_msg_send(a.agent, a.world);
							}
							dl.nm_msg_begin(a.agent, "braceForImpact");
							dl.nm_msg_set_bool(a.agent, "start", 1);
							dl.nm_msg_set_vec3(a.agent, "pos", pos);
							dl.nm_msg_set_vec3(a.agent, "look", pos);
							if (dl.nm_msg_send(a.agent, a.world) < 0) {
								logger::error("  brace: {}", LastError());
							}
							logger::info("{:08X}: thrown at something {:.1f} m ahead (facing {:.1f} {:.1f} {:.1f}) at {:.1f} m/s: bracing", a_actor->GetFormID(), dist, n[0], n[1],
								n[2], sp);
						}
					}
				}
				// coming down: reach to catch it (highFall catches by itself)
				if (a.flying == 1 && a.age - a.flyAt > 0.25f && (cv[2] < -1.5f || a.age - a.flyAt > 2.5f)) {
					a.flying = 2;
					if (a.flyMode < 2) {
						dl.nm_msg_begin(a.agent, "pedalLegs");
						dl.nm_msg_set_bool(a.agent, "start", 0);
						dl.nm_msg_send(a.agent, a.world);
						if (a.flyMode == 1) {
							dl.nm_msg_begin(a.agent, "bodyRollUp");
							dl.nm_msg_set_bool(a.agent, "start", 0);
							dl.nm_msg_send(a.agent, a.world);
						}
						dl.nm_msg_begin(a.agent, "catchFall");
						dl.nm_msg_set_bool(a.agent, "start", 1);
						dl.nm_msg_send(a.agent, a.world);
					}
					logger::info("{:08X}: coming down {:.1f} m/s after {:.2f} s in the air: catching the fall", a_actor->GetFormID(), -cv[2], a.age - a.flyAt);
				}
			}
			// down: by what took her down, then still, and Skyrim gets her up. Burning: rolling, thrashing at the flames;
			// shocked: convulsing, the current still in her; frozen: curled up, shivering; else writhing or curled up
			if (a.writheAt >= 0.0f && a.age >= a.writheAt) {
				a.writheAt = -1.0f;
				const float o = a.writheOomph;
				const int   el = a.writheElement;
				if (el == kFire) {
					a.writheFor = std::max(a.writheFor, burnTime_ + 0.5f * o);
				}
				a.writheUntil = a.age + a.writheFor;
				// the reaction table: down.element, down (by default from [Down] fFoetalChance)
				std::string key = "(fFoetalChance)";
				const Rule* found = FindRule({ std::string("down.") + ElementName(el), "down" }, key);
				const Rule  rule = found ? *found :
				                           ParseRule(fmt::format("foetal:{}, writhe:{}, roll:{}", foetalChance_, (1.0f - foetalChance_) * 0.6f,
											   (1.0f - foetalChance_) * 0.4f));
				const int   mode = Choose(rule, { "writhe", "roll", "foetal", "still" });  // -1: writhe
				const bool  foetal = mode == 2;
				dl.nm_msg_begin(a.agent, "stopAllBehaviours");
				dl.nm_msg_send(a.agent, a.world);
				if (a.chillUntil >= 0.0f) {
					dl.nm_msg_begin(a.agent, "configureBalanceReset");
					dl.nm_msg_send(a.agent, a.world);
				}
				a.burnUntil = a.shockUntil = a.chillUntil = -1.0f;
				if (mode == 3) {
					dl.nm_msg_begin(a.agent, "bodyRelax");
					dl.nm_msg_set_bool(a.agent, "start", 1);
				} else if (foetal) {
					dl.nm_msg_begin(a.agent, "bodyFoetal");
					dl.nm_msg_set_bool(a.agent, "start", 1);
					dl.nm_msg_set_float(a.agent, "asymmetry", Rand(0.0f, 0.6f));
					dl.nm_msg_set_float(a.agent, "backTwist", Rand(-0.5f, 0.5f));
					dl.nm_msg_set_int(a.agent, "randomSeed", static_cast<int>(Rand(1.0f, 999.0f)));
				} else {
					// amplitude, period: harder, bigger and faster (the row's amp / period fix them: fire 2.2 / 0.4, shock 1.3 / 0.3)
					const float k = rule.Value("amp", 0.8f + 0.6f * o), p = rule.Value("period", 1.1f - 0.4f * o);
					dl.nm_msg_begin(a.agent, "bodyWrithe");
					dl.nm_msg_set_bool(a.agent, "start", 1);
					dl.nm_msg_set_string(a.agent, "mask", "fb");
					dl.nm_msg_set_float(a.agent, "armAmplitude", k);
					dl.nm_msg_set_float(a.agent, "legAmplitude", k);
					dl.nm_msg_set_float(a.agent, "backAmplitude", k);
					dl.nm_msg_set_float(a.agent, "armPeriod", p * Rand(0.85f, 1.15f));
					dl.nm_msg_set_float(a.agent, "legPeriod", p * Rand(0.85f, 1.15f));
					dl.nm_msg_set_float(a.agent, "backPeriod", p * Rand(0.85f, 1.15f));
					dl.nm_msg_set_bool(a.agent, "rollOverFlag", mode == 1);
				}
				if (dl.nm_msg_send(a.agent, a.world) < 0) {
					logger::error("  down: {}", LastError());
				}
				// the current, or the cold's shiver, on top
				const bool jolt = rule.Has("shock");
				if (jolt || rule.Has("shiver")) {
					dl.nm_msg_begin(a.agent, "electrocute");
					dl.nm_msg_set_bool(a.agent, "start", 1);
					dl.nm_msg_set_float(a.agent, "stunMag", jolt ? Rand(0.4f, 0.55f) : 0.12f);
					dl.nm_msg_set_float(a.agent, "stunInterval", jolt ? 0.07f : 0.05f);
					dl.nm_msg_set_float(a.agent, "largeMult", jolt ? 3.0f : 1.5f);
					dl.nm_msg_set_float(a.agent, "directionRandomness", 0.5f);
					dl.nm_msg_send(a.agent, a.world);
				}
				static constexpr const char* kHow[]{ "writhing", "rolling", "curled up", "lying still" };
				logger::info("{:08X}: down: [{}: {}] {}{} for {:.1f} s", a_actor->GetFormID(), key, rule.text, kHow[mode < 0 ? 0 : mode],
					jolt ? ", convulsing" : rule.Has("shiver") ? ", shivering" : "", a.writheFor);
			}
			if (a.writheUntil >= 0.0f && a.age >= a.writheUntil) {
				a.writheUntil = -1.0f;
				dl.nm_msg_begin(a.agent, "stopAllBehaviours");
				dl.nm_msg_send(a.agent, a.world);
				dl.nm_msg_begin(a.agent, "bodyRelax");
				dl.nm_msg_set_bool(a.agent, "start", 1);
				dl.nm_msg_send(a.agent, a.world);
				logger::info("{:08X}: down: still now", a_actor->GetFormID());
			}
			if (a.reelUntil >= 0.0f && a.age >= a.reelUntil) {
				// the reel is over: she catches herself (the balance task), the element still running goes on over it
				a.reelUntil = -1.0f;
				dl.nm_msg_begin(a.agent, "staggerFall");
				dl.nm_msg_set_bool(a.agent, "start", 0);
				dl.nm_msg_send(a.agent, a.world);
				if (!a.knockQueued && !fallen) {
					if (dl.nm_agent_task_balance(a.agent, a.world, nullptr, nullptr) < 0) {
						logger::error("  balance task: {}", LastError());
					}
					Overlays(a, a.shotDir);
				}
				logger::info("{:08X}: the reel is over{}", a_actor->GetFormID(), a.knockQueued || fallen ? " (she is down)" : ", catching herself");
			}
			if (a.burnUntil >= 0.0f && a.age >= a.burnUntil) {
				StopBurn(a);
				logger::info("{:08X}: the fire is out", a_actor->GetFormID());
			}
			if (a.shockUntil >= 0.0f && a.age >= a.shockUntil) {
				StopShock(a);
				logger::info("{:08X}: the shock is over", a_actor->GetFormID());
			}
			if (a.chillUntil >= 0.0f && a.age >= a.chillUntil) {
				StopChill(a);
				logger::info("{:08X}: warm again", a_actor->GetFormID());
			}
			if (a.heldDeath && !a.death && a.collapseAt >= 0.0f && a.age >= a.collapseAt) {
				a.collapseAt = -1.0f;
				if (!a.knockQueued && a.flying == 0) {
					// she stops fighting it: the strength leaves her over a second or two, she goes down (and dies there)
					dl.nm_msg_begin(a.agent, "stayUpright");
					dl.nm_msg_set_bool(a.agent, "start", 0);
					dl.nm_msg_send(a.agent, a.world);
					const float period = Rand(1.0f, 2.0f);
					dl.nm_msg_begin(a.agent, "shotRelax");
					dl.nm_msg_set_float(a.agent, "relaxPeriod", period);
					dl.nm_msg_send(a.agent, a.world);
					logger::info("{:08X}: dying on her feet: collapsing over {:.1f} s", a_actor->GetFormID(), period);
				}
			}
			if (a.death && a.ragdolled && !a.relaxed) {
				if (a.writhe && !a.writhing && ((fallen && a.age - a.dyingAt >= dyingReact_) || a.age >= a.relaxAt)) {
					a.writhing = true;
					dl.nm_msg_begin(a.agent, "stopAllBehaviours");
					dl.nm_msg_send(a.agent, a.world);
					const float stiff = Rand(9.0f, 12.0f), period = a.diePeriod > 0.0f ? a.diePeriod : Rand(2.0f, 4.0f),
								amp = a.dieAmp >= 0.0f ? a.dieAmp : Rand(0.7f, 1.3f);
					dl.nm_msg_begin(a.agent, "bodyWrithe");
					dl.nm_msg_set_bool(a.agent, "start", 1);
					for (const char* n : { "armStiffness", "legStiffness", "backStiffness" }) {
						dl.nm_msg_set_float(a.agent, n, stiff);
					}
					for (const char* n : { "armPeriod", "legPeriod", "backPeriod" }) {
						dl.nm_msg_set_float(a.agent, n, period * Rand(0.8f, 1.2f));
					}
					for (const char* n : { "armAmplitude", "legAmplitude", "backAmplitude", "kneeAmplitude", "elbowAmplitude" }) {
						dl.nm_msg_set_float(a.agent, n, amp * Rand(0.8f, 1.2f));
					}
					for (const char* n : { "armDamping", "legDamping", "backDamping" }) {
						dl.nm_msg_set_float(a.agent, n, 1.0f);
					}
					dl.nm_msg_send(a.agent, a.world);
					a.relaxAt = a.age + Rand(1.0f, 2.0f);
					logger::info("{:08X}: dying: writhe (stiffness {:.1f}, period {:.1f} s) for {:.1f} s", a_actor->GetFormID(), stiff, period, a.relaxAt - a.age);
				} else if ((!a.writhe || a.writhing) && a.age >= a.relaxAt) {
					a.relaxed = true;
					dl.nm_msg_begin(a.agent, "stopAllBehaviours");
					dl.nm_msg_send(a.agent, a.world);
					const float relax = Rand(60.0f, 100.0f);
					dl.nm_msg_begin(a.agent, "bodyRelax");
					dl.nm_msg_set_bool(a.agent, "start", 1);
					dl.nm_msg_set_float(a.agent, "relaxation", relax);
					dl.nm_msg_set_float(a.agent, "damping", 1.0f);
					dl.nm_msg_send(a.agent, a.world);
					logger::info("{:08X}: dying: limp (relaxation {:.0f})", a_actor->GetFormID(), relax);
				}
			}
			// thrown: no balancer runs in the air, so Delirium never calls it a fall; down is her head near the ground
			bool landed = false;
			if (a.flying > 0 && (!a.knockQueued || a.takeover)) {
				float hp[3]{}, hq[4]{};
				dl.nm_agent_get_part_transform(a.agent, kHead, hp, hq);
				landed = (a.flying == 2 && a.age - a.flyAt > 0.5f && hp[2] < 0.6f) || a.age - a.flyAt > 3.0f;
				if (landed) {
					logger::info("{:08X}: landed after {:.2f} s (head at {:.2f} m)", a_actor->GetFormID(), a.age - a.flyAt, hp[2]);
					if (a.knockQueued) {
						// Skyrim's own throw: she is in its ragdoll already, only her reaction on the ground is left
						a.flying = 0;
						if (a.writheFor > 0.0f && !a.death) {
							a.writheAt = a.age + 0.3f;
						}
					}
				}
			}
			// thrown: only the landing counts (her balance stopped in the air on purpose)
			a.offBalance = a.wasBalancing && !balancing ? a.offBalance + dt : 0.0f;
			// on her feet still (the flag can come on the moment a hard hit lands): only once her head is going down
			float hd[3]{}, hdq[4]{};
			dl.nm_agent_get_part_transform(a.agent, kHead, hd, hdq);
			if (!a.knockQueued && (a.flying > 0 ? landed : ((fallen || a.offBalance > 0.25f) && hd[2] < 1.0f))) {
				logger::info("{:08X}: down after {:.2f} s: {}", a_actor->GetFormID(), a.age,
					a.flying > 0 ? "landed" : fallen ? "Delirium says she fell" : "balance lost for 0.25 s");
				// balance lost: hand the actor to Skyrim's own ragdoll state now, while our pose is still close to the
				// animation Skyrim builds its ragdoll from (no push of its own); DriveRagdoll makes it follow Delirium
				a.knockQueued = true;
				if (a.writheFor > 0.0f && !a.death) {
					a.writheAt = a.age + (a.flying > 0 ? 0.3f : 0.7f);
				}
				// the way she is falling (Delirium's head over its pelvis, flat): the knock's source goes behind her
				float hp[3]{}, pp[3]{}, hq[4]{};
				dl.nm_agent_get_part_transform(a.agent, kHead, hp, hq);
				dl.nm_agent_get_part_transform(a.agent, 0, pp, hq);
				const RE::NiPoint3 lean(hp[0] - pp[0], hp[1] - pp[1], 0.0f);
				g_tasks->AddTask([h = a.actor, lean]() {
					// this runs a frame later: her 3D can be unloaded by then (Skyrim's knock reads it without a check),
					// and we may have let her go already
					if (auto act = h.get(); act && act->Get3D(false) && !act->IsDisabled() && Engine::Get().KeepsDown(act.get())) {
						if (auto* proc = act->GetActorRuntimeData().currentProcess) {
							// skeletons' race says "no knockdowns": Skyrim staggers them instead, never ragdolls them, and
							// there is no get-up to hand back to (she snapped up). Off for our knock only, back at once.
							auto*      race = act->GetRace();
							const bool lift = race && race->data.flags.any(RE::RACE_DATA::Flag::kNoKnockdowns) && Engine::Get().Creature(act.get());
							if (lift) {
								race->data.flags.reset(RE::RACE_DATA::Flag::kNoKnockdowns);
							}
							proc->KnockExplosion(act.get(), KnockSource(act.get(), lean), 0.0f);
							if (lift) {
								race->data.flags.set(RE::RACE_DATA::Flag::kNoKnockdowns);
							}
							logger::info("{:08X}: into Skyrim's ragdoll state{}", act->GetFormID(), lift ? " (her race's no-knockdowns lifted for it)" : "");
						}
					}
				});
			} else if (!a.knockQueued) {
				FollowActor(a, a_actor);
			}
			if (a.knockQueued && static_cast<int>(a.age) != static_cast<int>(a.age - dt)) {
				logger::info("  ragdoll: knock state {}, in state {}, {} bodies, {} drives, fastest steer {:.1f} m/s, top node lag up to {:.2f} rad",
					static_cast<int>(a_actor->AsActorState()->GetKnockState()), a_actor->IsInRagdollState(), a.sky.bodies.size(), a.drives, a.maxSpeed,
					a.maxRootLag);
				if (a.following) {
					logger::info("  muscles: joints asked for up to {:.2f} rad/s per step ({} at age {:.2f}; {} capped), bodies turned up to {:.2f} rad/s per step, {} spins capped (gain {:.2f}); limbs moving {:.0f} deg/s, behaviours' torques turned up to {:.2f} rad/s per step",
						a.maxWant, rig_[static_cast<std::size_t>(a.wantPart)].name, a.wantAge, a.clamped, a.maxDw, a.spinCapped, gain_,
						a.actN ? a.actSum / a.actN : 0.0f, a.extMax);
					{
						float sw = 0.0f, top = 0.0f;
						for (float s : a.actSwing) {
							sw += s;
							top = std::max(top, s);
						}
						logger::info("  limbs swung on average {:.0f} deg this second (most {:.0f}); Havok counted a body still up to {} frames",
							a.actSwing.empty() ? 0.0f : sw / a.actSwing.size(), top, a.sleepMax);
						a.sleepMax = 0;
						a.actRef.clear();
						a.actSwing.clear();
					}
					a.actSum = 0;
					a.actN = 0;
					a.extMax = 0;
					a.maxWant = 0;
					a.clamped = 0;
					a.spinCapped = 0;
					a.maxDw = 0;
				}
				a.drives = 0;
				a.maxSpeed = 0;
				a.maxRootLag = 0;
			}
			// back to Skyrim's animation once it has caught itself and stood still
			a.calm = (balancing && !fallen && ke < 2.0f && a.sincePush > 1.0f && !a.knockQueued && a.burnUntil < 0.0f && a.shockUntil < 0.0f && a.chillUntil < 0.0f && a.reelUntil < 0.0f) ? a.calm + dt : 0.0f;
			// lying still in Skyrim's ragdoll: let go, Skyrim gets up from this pose on its own
			// (a corpse is held for good: it keeps moving in Delirium until it is really still, so the hold doesn't freeze a slide)
			a.settled = (a.ragdolled && ke < (a.death ? 0.05f : 1.0f) && (!a.death || a.relaxed) && a.writheAt < 0.0f && a.writheUntil < 0.0f) ? a.settled + dt : 0.0f;
			// not while Skyrim's stagger still runs underneath: its clip would show in the fade (Delirium plays the stagger,
			// Skyrim keeps only its gameplay: the interrupted attack and the staggered state)
			if (autoRelease_ && a.calm > 1.5f && !a.blendingOut && !a_actor->AsActorState()->IsStaggered()) {
				a.blendingOut = true;  // fade back into Skyrim's animation over kBlendOut, physics still running
			}
			if (a.blendingOut) {
				a.blend -= dt / kBlendOut;
				if (a.blend <= 0.0f) {
					Release(i, "recovered");
					return;
				}
			}
			// one body, Skyrim's ragdoll: once it lies still Skyrim takes over (corpse, or its own get-up)
			if (autoRelease_ && a.settled > (a.death ? 1.0f : 0.15f)) {
				Release(i, a.death ? "settled: the corpse is Skyrim's ragdoll already" : "settled: Skyrim's ragdoll gets up by itself");
				return;
			}
			// a corpse that never lies still (wedged, or under the ground) would be driven for good: Skyrim's ragdoll
			// is already running underneath, so hand it over as it is
			if (autoRelease_ && a.death && a.deadFor > kCorpseHold) {
				Release(i, "corpse held for 30 s: Skyrim's ragdoll takes it");
				return;
			}
			if (autoRelease_ && a.keRising >= kRunawaySeconds) {
				Release(i, "energy kept growing while down: Skyrim's ragdoll takes over");
				return;
			}
			// the frame Skyrim switches to its ragdoll has no pose copy at all (and we only learn that a frame later):
			// while knocked, put our pose on the bones here too, so a frame without a copy still draws ours. Held still
			// too: with only Skyrim's one copy a frame, its own pose (0.1-0.2 m off ours) showed through as a twitch
			if (a.knockQueued && !TorqueMode(a)) {
				if (a.copies == 0) {
					logger::info("  age {:.3f}: the last frame had no pose copy", a.age);
				}
				AfterPose(a_actor, false);
			} else if (!a.knockQueued && a.copies == 0) {
				// upright, Skyrim skipped its pose copy this frame (its animation update ran without one): the bones
				// would hold last frame's pose and the body stutters, so ours go on here
				++a.filled;
				a.filling = true;
				AfterPose(a_actor, false);
				a.filling = false;
			}
			a.copies = 0;
			// knocked down in Delirium but Skyrim's ragdoll never came, or is over (a save loaded, a get-up we missed): let go
			a.noRagdoll = a.knockQueued && !a_actor->IsInRagdollState() ? a.noRagdoll + dt : 0.0f;
			if (a.noRagdoll > 3.0f) {
				Release(i, "not in Skyrim's ragdoll state for 3 s");
				return;
			}
			if (a.ragdolled) {
				const auto ks = a_actor->AsActorState()->GetKnockState();
				if (ks == RE::KNOCK_STATE_ENUM::kQueued || ks == RE::KNOCK_STATE_ENUM::kGetUp) {
					Release(i, "Skyrim started its get-up");
				}
			}
			return;
		}
	}

	void Engine::AfterPose(RE::Actor* a_actor, bool a_postPhysics)
	{
		std::scoped_lock l(lock_);
		for (auto& p : active_) {
			if (p->actor.get().get() == a_actor) {
				++p->copies;
			}
		}
		AfterPoseImpl(a_actor, a_postPhysics);
		// the skeleton's top node as it gets drawn: SmoothRoot turns it from here on the next pose copy
		for (auto& p : active_) {
			if (auto* top = a_actor->Get3D(false); top && p->actor.get().get() == a_actor && Same3D(*p, a_actor)) {
				if (!p->haveDrawnRoot || !p->knockQueued) {
					// upright the top node is Skyrim's; ragdolled SmoothRoot keeps this up to date
					p->drawnRootQ = FromMat(top->world.rotate);
					p->drawnRootT = top->world.translate;
					p->drawnAge = p->age;
					p->haveDrawnRoot = true;
				}
				// COM as this pose copy left it (ours or Skyrim's ragdoll pose): AfterRootMove holds it there
				p->comWorld = p->com->world;
				p->haveCom = p->knockQueued && !TorqueMode(*p);
				if (p->haveCom && p->ragdolled && p->traceN < 100) {
					logger::info("    copy {:.3f} {}: COM {:.1f} {:.1f} {:.1f}, top {:.1f} {:.1f} {:.1f}", p->age, a_postPhysics ? "post" : "pre ", p->comWorld.translate.x,
						p->comWorld.translate.y, p->comWorld.translate.z, top->world.translate.x, top->world.translate.y, top->world.translate.z);
				}
			}
		}
	}

	// Skyrim's fresh bone locals into world transforms, for the bones Delirium reads only (and the chains above them, from
	// NPC COM's parent down). Walking the whole skeleton also rewrote the attached models (sword, shield, hair) outside
	// Skyrim's update, and their textures flickered.
	void Engine::RefreshBones(Active& a)
	{
		auto* top = a.com->parent;
		std::vector<RE::NiAVObject*> order;
		auto add = [&](RE::NiAVObject* n) {
			std::vector<RE::NiAVObject*> chain;
			for (; n && n != top; n = n->parent) {
				chain.push_back(n);
			}
			if (n != top) {
				return;  // not below NPC COM's parent
			}
			for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
				if (std::find(order.begin(), order.end(), *it) == order.end()) {
					order.push_back(*it);  // parents always come before their children
				}
			}
		};
		add(a.com);
		for (std::size_t i = 0; i < a.ref.size(); ++i) {
			add(a.ref[i]);
			add(a.ja[i]);
			add(a.jb[i]);
		}
		for (const auto& b : a.sky.bodies) {
			add(b.node);
		}
		if (top->parent) {
			top->world = top->parent->world * top->local;
		}
		for (auto* n : order) {
			n->world = n->parent->world * n->local;
		}
	}

	void Engine::AfterPoseImpl(RE::Actor* a_actor, bool a_postPhysics)
	{
		for (auto& p : active_) {
			auto& a = *p;
			if (a.actor.get().get() != a_actor || !a_actor->Get3D(false) || !Same3D(a, a_actor)) {
				continue;
			}
			++a.poses;
			// Skyrim's animated pose in world space (the bone locals are fresh from the graph), then Delirium's bones over it
			RefreshBones(a);
			if (a_postPhysics && !a.ragdolled && a.sky.driver && a_actor->IsInRagdollState()) {
				// the bones were just posed from Skyrim's ragdoll bodies: where each body sits on its bone
				Calibrate(a, a_actor);
				if (a.sky.linked) {
					a.ragdolled = true;
					a.ragdollAge = a.age;
				}
				if (a.takeover) {
					InheritVelocity(a);
				}
			}
			if (bodies_ && !a.ragdolled && !a.sky.linked && !a_actor->IsInRagdollState() && a.sky.InWorld()) {
				// on her feet with her bodies in Skyrim's world ([Bodies]): from here they follow Delirium's pose
				// (DriveRagdoll), so other bodies meet her where she is drawn, not where her animation would have her
				LinkBodies(a, a_actor);
				logger::info("{:08X}: on her feet with {} bodies in Skyrim's world: they follow Delirium's pose", a_actor->GetFormID(), a.sky.bodies.size());
			}
			if (TorqueMode(a)) {
				// Skyrim's ragdoll is the body: Skyrim poses the bones from it. Not yet through F7's lead-in: Skyrim
				// builds its ragdoll from the animation, not from our pose, and the lead-in first pulls it onto ours
				Unshow(a);  // Skyrim poses the bones: her meshes read them again
				return;
			}
			std::array<V3, 21> pos;
			std::array<Q, 21>  rot;
			// the live animation only from a real pose copy: on a filler frame the bones still hold our own pose from the last
			// frame, and taking that as the pose to hold let her own fall become the target (she kept leaning into it)
			const bool follow = followAnimation_ && !a.knockQueued && !a.filling;
			if (follow) {
				PartsFromBones(a, pos, rot, nullptr);
			}
			if (a.knockQueued) {
				// SmoothRoot refreshes the top node's world even when it leaves it where Skyrim put it: the skeleton below
				// has to follow before WriteBones reads it (a stale NPC Root left the pelvis ~5 cm off)
				// (only the chain from the 3D root down to NPC Root: walking the whole 3D tree reaches gear whose nif root is
				// a plain NiNode and made it flicker again; WriteBones updates the skeleton below NPC COM itself)
				SmoothRoot(a, a_actor);
				auto*                          root3d = a_actor->Get3D(false);
				std::array<RE::NiAVObject*, 8> chain{};
				std::size_t                    n = 0;
				for (auto* c = a.com->parent; c && c != root3d && n < chain.size(); c = c->parent) {
					chain[n++] = c;
				}
				while (n > 0) {
					auto* c = chain[--n];
					if (c->parent) {
						c->world = c->parent->world * c->local;
					}
				}
				// WriteBones puts COM where the pelvis lands on ours: an offset left below COM would move COM instead
				if (ClearJointFixOffset(a.ref[0], a.com)) {
					UpdateTransforms(a.com);
				}
			}
			// trace, through F7's lead-in: Skyrim's pose (from its bodies) vs ours, relative to the pelvis = the jump the
			// switch to Skyrim's bodies will show
			std::array<RE::NiTransform, 21> sky{}, skyC{};
			const bool                      gap = a_postPhysics && a.ragdolled && a.traceN < 100;
			const bool                      dump = gap && a.age - a.ragdollAge > 0.12f && !a.dumped;
			if (dump) {
				for (std::size_t i = 0; i < 21; ++i) {
					if (a.ref[i]) {
						skyC[i] = a.com->world.Invert() * a.ref[i]->world;
					}
				}
			}
			if (gap) {
				for (std::size_t i = 0; i < 21; ++i) {
					if (a.ref[i]) {
						sky[i] = a.ref[0]->world.Invert() * a.ref[i]->world;
					}
				}
			}
			a.haveSkyCom = a.ragdolled && a_postPhysics;
			if (a.haveSkyCom) {
				a.skyComLocal = a.com->local;
			}
			a.limbBlend = a.ragdolled ? LeadShare(a) : 1.0f;
			WriteBones(a);
			a.limbBlend = 1.0f;
			KeepPrevious(a_actor->Get3D(false), a.drawnWorld);
			if (show_) {
				Show(a, a_actor->Get3D(false));  // her meshes draw this finished pose (body/show.cpp)
			} else {
				Unshow(a);
			}
			for (std::size_t i = 0; i < 21; ++i) {
				if (a.ref[i]) {
					a.written[i] = a.ref[i]->world;
				}
			}
			a.haveWritten = true;
			if (dump) {
				// every bone relative to NPC COM (a body in Skyrim's ragdoll): Skyrim's pose of it vs ours
				a.dumped = true;
				std::string s;
				for (std::size_t i = 0; i < 21; ++i) {
					if (!a.ref[i]) {
						continue;
					}
					const auto  ours = a.com->world.Invert() * a.ref[i]->world;
					const float d = (ours.translate - skyC[i].translate).Length() * a.com->world.scale * kUnit;
					const auto  m = skyC[i].rotate.Transpose() * ours.rotate;
					const float r = std::acos(std::clamp((m.entry[0][0] + m.entry[1][1] + m.entry[2][2] - 1.0f) * 0.5f, -1.0f, 1.0f)) * 57.2958f;
					s += fmt::format(" {}={:.3f}m/{:.0f}", rig_[i].name, d, r);
				}
				logger::info("    gap dump {:.3f} (from NPC COM, Skyrim's vs ours):{}", a.age, s);
			}
			if (gap) {
				float       wp = 0, wr = 0;
				std::size_t ip = 0, ir = 0;
				std::string cl;
				for (std::size_t i = 1; i < 21; ++i) {
					if (!a.ref[i]) {
						continue;
					}
					const auto  ours = a.ref[0]->world.Invert() * a.ref[i]->world;
					const float d = (ours.translate - sky[i].translate).Length() * a.ref[0]->world.scale * kUnit;
					const auto  m = sky[i].rotate.Transpose() * ours.rotate;
					const float r = std::acos(std::clamp((m.entry[0][0] + m.entry[1][1] + m.entry[2][2] - 1.0f) * 0.5f, -1.0f, 1.0f)) * 57.2958f;
					if (d > wp) {
						wp = d, ip = i;
					}
					if (r > wr) {
						wr = r, ir = i;
					}
					if (i == 13 || i == 17 || i == 14 || i == 18) {
						cl += fmt::format(" {} {:.0f}", rig_[i].name, r);
					}
				}
				logger::info("    gap {:.3f}: Skyrim's pose vs ours (from the pelvis): {:.3f} m ({}), {:.0f} deg ({});{}", a.age, wp, rig_[ip].name, wr, rig_[ir].name, cl);
			}
			// the bones as we just posed them: the bodies' targets until DriveRagdoll's next step sets its own
			for (auto& b : a.sky.bodies) {
				if (a_postPhysics || !b.hasTarget) {
					b.target = Scl({ b.node->world.translate.x, b.node->world.translate.y, b.node->world.translate.z }, kUnit);
					b.targetRot = FromMat(b.node->world.rotate);
					b.hasTarget = true;
				}
			}
			if (follow) {
				// the posture to hold follows the live animation: a guard stays up, a talking NPC keeps gesturing
				// (not once ragdolled: then the "animation" is Skyrim's ragdoll, which is us)
				RecordPose(a, a_actor, pos, rot);
				StandingLegs(a, pos, rot);
				SetZeroPose(a, pos, rot);
			}
			return;
		}
	}

}

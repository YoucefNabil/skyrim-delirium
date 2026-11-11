// Where each of Skyrim's ragdoll bodies sits on its bone.
#include "shared.h"

namespace delirium
{
	namespace
	{
		// hkaSkeletonMapper (Havok 2010): hkReferencedObject, then hkaSkeletonMapperData
		struct SimpleMapping
		{
			std::int16_t  boneA, boneB;
			std::uint8_t  pad04[0xC];
			float         t[4], r[4], s[4];  // aFromBTransform: B = A * aFromB
		};
		static_assert(sizeof(SimpleMapping) == 0x40);
		struct SkeletonMapper
		{
			std::uint8_t           pad00[0x10];
			const RE::hkaSkeleton* skeletonA;
			const RE::hkaSkeleton* skeletonB;
			const SimpleMapping*   simple;
			std::int32_t           simpleCount, simpleCap;
		};
	}

	void Engine::LinkBodies(Active& a, RE::Actor* a_actor)
	{
		// Skyrim hangs each ragdoll body on a skeleton bone's collision object
		std::unordered_map<const void*, RE::NiAVObject*> owner;
		std::function<void(RE::NiAVObject*)>             walk = [&](RE::NiAVObject* n) {
			if (auto* co = n->collisionObject.get()) {
				if (auto* bco = skyrim_cast<RE::bhkNiCollisionObject*>(co); bco && bco->body) {
					owner[bco->body->referencedObject.get()] = n;
				}
			}
			if (auto* node = n->AsNode()) {
				for (auto& c : node->GetChildren()) {
					if (c) {
						walk(c.get());
					}
				}
			}
		};
		walk(a_actor->Get3D(false));
		a.sky.bodies.clear();
		std::string list;
		for (auto* body : a.sky.driver->ragdoll->rigidBodies) {
			auto it = body ? owner.find(body) : owner.end();
			if (it == owner.end()) {
				continue;
			}
			BodyLink l{ body, it->second };
			std::memcpy(l.massInv.data(), &body->motion.inertiaAndMassInv, sizeof(l.massInv));
			a.sky.bodies.push_back(l);
		}
		// Skyrim hangs each body exactly on its bone (havok units = game units * kUnit). Check it on distances between
		// bodies vs between their bones: those don't care that the root hasn't caught up with the ragdoll yet.
		float worst = 0;
		for (std::size_t i = 1; i < a.sky.bodies.size(); ++i) {
			auto pos = [](const BodyLink& l) {
				const float* t = reinterpret_cast<const float*>(&l.body->motion.motionState.transform.translation);
				return V3{ t[0], t[1], t[2] };
			};
			auto bone = [](const BodyLink& l) { return Scl({ l.node->world.translate.x, l.node->world.translate.y, l.node->world.translate.z }, kUnit); };
			const V3    db = Sub(pos(a.sky.bodies[i]), pos(a.sky.bodies[0])), dn = Sub(bone(a.sky.bodies[i]), bone(a.sky.bodies[0]));
			const float lb = std::sqrt(db.x * db.x + db.y * db.y + db.z * db.z), ln = std::sqrt(dn.x * dn.x + dn.y * dn.y + dn.z * dn.z);
			worst = std::max(worst, std::abs(lb - ln));
			list += fmt::format(" {}", a.sky.bodies[i].node->name.c_str());
		}
		list = fmt::format(" body-vs-bone spacing differs by at most {:.3} m;", worst) + list;
		// Skyrim's own mapping between ragdoll bodies and animation bones (the one its ragdoll uses to pose the skeleton)
		std::string mapLog;
		int         mappedCount = 0;
		{
			auto* ch = a.sky.driver->character;
			auto* setup = ch ? ch->setup.get() : nullptr;
			auto* m = setup ? reinterpret_cast<const SkeletonMapper*>(setup->animationToRagdollSkeletonMapper.get()) : nullptr;
			auto* rd = a.sky.driver->ragdoll;
			auto* rs = rd->skeleton.get();
			if (!m || !rs) {
				mapLog = " no animation-to-ragdoll mapper";
			} else {
				const bool animIsA = m->skeletonB == rs;
				const auto* anim = animIsA ? m->skeletonA : m->skeletonB;
				mapLog = fmt::format(" mapper: ragdoll skeleton is {} ({} simple mappings)", animIsA ? "B" : (m->skeletonA == rs ? "A" : "neither"), m->simpleCount);
				for (auto& l : a.sky.bodies) {
					int k = -1;
					for (int i = 0; i < static_cast<int>(rd->rigidBodies.size()); ++i) {
						k = rd->rigidBodies[i] == l.body ? i : k;
					}
					int rb = -1;
					for (int i = 0; i < static_cast<int>(rd->boneToRigidBodyMap.size()); ++i) {
						rb = rd->boneToRigidBodyMap[i] == k ? i : rb;
					}
					for (int i = 0; rb >= 0 && i < m->simpleCount; ++i) {
						const auto& sm = m->simple[i];
						if ((animIsA ? sm.boneB : sm.boneA) != rb) {
							continue;
						}
						const int   ab = animIsA ? sm.boneA : sm.boneB;
						const char* an = anim->bones[ab].name.c_str();
						for (int j = 0; j < 21; ++j) {
							const bool same = SameBone(rig_[j].ref, an, a.looseBones);
							if (same && rig_[j].ref == rig_[j].jointA && rig_[j].ref == rig_[j].jointB) {
								l.posPart = j;
							}
							if (same && (rig_[j].write || l.rotPart < 0)) {
								l.rotPart = j;
							}
						}
						Q r{ sm.r[0], sm.r[1], sm.r[2], sm.r[3] };
						// Skyrim's mapper stores offsets in game units (hkbRagdollDriver maps the poses before the havok scale)
						V3 t = Scl({ sm.t[0], sm.t[1], sm.t[2] }, kUnit);
						if (!animIsA) {  // the mapping goes the other way: invert it
							r = Conj(r);
							t = Rot(r, Scl(t, -1.0f));
						}
						l.mapR = r;
						l.mapT = t;
						l.mapped = l.posPart >= 0 && l.rotPart >= 0;
						mappedCount += l.mapped;
						mapLog += fmt::format(" {}{}<-{}({:.3f} m {:.0f} deg)", l.mapped ? "" : "[no part] ", l.node->name.c_str(), an,
							std::sqrt(t.x * t.x + t.y * t.y + t.z * t.z), Angle(r, Q{}) * 57.2958f);
						break;
					}
				}
			}
		}
		logger::info("{:08X}: {} of {} bodies mapped to Delirium parts;{}", a_actor->GetFormID(), mappedCount, a.sky.bodies.size(), mapLog);
		{
			// the ragdoll-to-animation mapper: what Skyrim's pose copy draws from each body
			auto*       ch = a.sky.driver->character;
			auto*       setup = ch ? ch->setup.get() : nullptr;
			const auto* m = setup ? reinterpret_cast<const SkeletonMapper*>(setup->ragdollToAnimationSkeletonMapper.get()) : nullptr;
			auto*       rd = a.sky.driver->ragdoll;
			const auto* rs = rd ? rd->skeleton.get() : nullptr;
			std::string drawLog;
			if (m && rs && (m->skeletonA == rs || m->skeletonB == rs)) {
				const bool ragIsA = m->skeletonA == rs;
				for (auto& l : a.sky.bodies) {
					int k = -1;
					for (int i = 0; i < static_cast<int>(rd->rigidBodies.size()); ++i) {
						k = rd->rigidBodies[i] == l.body ? i : k;
					}
					int rb = -1;
					for (int i = 0; i < static_cast<int>(rd->boneToRigidBodyMap.size()); ++i) {
						rb = rd->boneToRigidBodyMap[i] == k ? i : rb;
					}
					for (int i = 0; rb >= 0 && i < m->simpleCount; ++i) {
						const auto& sm = m->simple[i];
						if ((ragIsA ? sm.boneA : sm.boneB) != rb) {
							continue;
						}
						Q  r{ sm.r[0], sm.r[1], sm.r[2], sm.r[3] };
						V3 t = Scl({ sm.t[0], sm.t[1], sm.t[2] }, kUnit);
						if (!ragIsA) {  // body = bone * (r, t): invert it
							r = Conj(r);
							t = Rot(r, Scl(t, -1.0f));
						}
						l.drawR = r;
						l.drawT = t;
						l.haveDraw = true;
						// against the inverse of the other mapper
						const Q  ir = Conj(l.mapR);
						const V3 it = Rot(ir, Scl(l.mapT, -1.0f));
						drawLog += fmt::format(" {} {:.3f} m {:.1f} deg", l.node->name.c_str(), [](const V3& d) { return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z); }(Sub(t, it)), Angle(r, ir) * 57.2958f);
						break;
					}
				}
				logger::info("{:08X}: ragdoll-to-animation mapper ({} is the ragdoll), off the other's inverse by:{}", a_actor->GetFormID(), ragIsA ? "A" : "B", drawLog);
			} else {
				logger::info("{:08X}: no ragdoll-to-animation mapper", a_actor->GetFormID());
			}
		}
		// a body without a part still has to move: as a puppet (below) nothing else can. Skyrim takes the model's
		// placement from the ragdoll's root body (NPC COM), so a frozen one freezes the whole skeleton.
		{
			auto pose = [](const BodyLink& l, V3& p, Q& r) {
				const float* t = reinterpret_cast<const float*>(&l.body->motion.motionState.transform.translation);
				const float* q = reinterpret_cast<const float*>(&l.body->motion.motionState.sweptTransform.rotation1);
				p = { t[0], t[1], t[2] };
				r = { q[0], q[1], q[2], q[3] };
			};
			std::string fl;
			for (auto& l : a.sky.bodies) {
				if (l.mapped) {
					continue;
				}
				V3 p;
				Q  r;
				pose(l, p, r);
				// the nearest bone that Delirium poses with its own part (NPC COM -> NPC Pelvis: they share a spot)
				float best = 1e9f;
				for (int j = 0; j < 21; ++j) {
					if (a.ref[j] && rig_[j].ref == rig_[j].jointA && rig_[j].ref == rig_[j].jointB) {
						const float d = (a.ref[j]->world.translate - l.node->world.translate).Length();
						if (d < best) {
							best = d;
							l.follow = j;
						}
					}
				}
				if (l.follow >= 0) {
					// bone in bone (both skeleton nodes, so the root not having caught up yet doesn't matter); the body
					// then sits on its bone through Skyrim's own mapping (mapR/mapT), as in BodyTarget
					const auto& n = a.ref[l.follow]->world;
					const V3    lp = Scl({ n.translate.x, n.translate.y, n.translate.z }, kUnit);
					const Q     lr = FromMat(n.rotate);
					p = Scl({ l.node->world.translate.x, l.node->world.translate.y, l.node->world.translate.z }, kUnit);
					r = FromMat(l.node->world.rotate);
					l.followR = Mul(Conj(lr), r);
					l.followT = Rot(Conj(lr), Sub(p, lp));
					fl += fmt::format(" {} on {} ({:.3f} m)", l.node->name.c_str(), a.ref[l.follow]->name.c_str(), best * kUnit);
				} else {
					fl += fmt::format(" {} on nothing", l.node->name.c_str());
				}
			}
			if (!fl.empty()) {
				logger::info("{:08X}: bodies without a part ride on a neighbour:{}", a_actor->GetFormID(), fl);
			}
		}
		a.sky.linked = true;
		logger::info("{:08X}: Skyrim ragdoll has {} bodies, {} on bones:{}", a_actor->GetFormID(),
			a.sky.driver->ragdoll->rigidBodies.size(), a.sky.bodies.size(), list);
	}

	void Engine::Calibrate(Active& a, RE::Actor* a_actor)
	{
		// Skyrim's ragdoll is the body from here (its own mass, gravity, collision); DriveRagdoll only moves it
		LinkBodies(a, a_actor);
		// trace: the joints' own friction and motors (they may eat the behaviours' small steady push)
		{
			std::string js;
			int         friction = 0, motors = 0;
			for (auto* ci : a.sky.driver->ragdoll->constraints) {
				auto* d = ci ? ci->data : nullptr;
				if (!d) {
					continue;
				}
				const auto ty = d->GetType();
				if (ty == 7) {
					auto& at = static_cast<RE::hkpRagdollConstraintData*>(d)->atoms;
					const bool m = at.ragdollMotors.enabled;
					js += fmt::format(" ragdoll(friction {}{:.1f}, motors {}{})", at.angFriction.enabled ? "" : "off ", at.angFriction.maxFrictionTorque,
						m ? "on" : "off", at.ragdollMotors.motors[0] ? fmt::format(" type {}", static_cast<int>(at.ragdollMotors.motors[0]->type.underlying())) : "");
					friction += at.angFriction.enabled && at.angFriction.maxFrictionTorque > 0.0f;
					motors += m;
				} else if (ty == 2) {
					auto& at = static_cast<RE::hkpLimitedHingeConstraintData*>(d)->atoms;
					const bool m = at.angMotor.enabled;
					js += fmt::format(" hinge(friction {}{:.1f}, motor {}{})", at.angFriction.enabled ? "" : "off ", at.angFriction.maxFrictionTorque,
						m ? "on" : "off", at.angMotor.motor ? fmt::format(" type {}", static_cast<int>(at.angMotor.motor->type.underlying())) : "");
					friction += at.angFriction.enabled && at.angFriction.maxFrictionTorque > 0.0f;
					motors += m;
				} else {
					js += fmt::format(" type{}", ty);
				}
			}
			logger::info("{:08X}: Skyrim ragdoll joints: {}, {} with friction, {} with motors on:{}", a_actor->GetFormID(),
				a.sky.driver->ragdoll->constraints.size(), friction, motors, js);
		}
		EaseJoints(a);
		{
			std::string bs;
			for (auto* body : a.sky.driver->ragdoll->rigidBodies) {
				if (body) {
					const auto& ms = body->motion.motionState;
					bs += fmt::format(" {:.2f}/{:.2f}/{}", static_cast<float>(ms.linearDamping), static_cast<float>(ms.angularDamping), ms.maxAngularVelocity.value);
				}
			}
			logger::info("{:08X}: Skyrim ragdoll bodies' damping (linear/angular/max spin code):{}", a_actor->GetFormID(), bs);
			std::string ms;
			for (auto* body : a.sky.driver->ragdoll->rigidBodies) {
				if (body) {
					ms += fmt::format(" {:.2f}/{:.2f}/{}", body->material.friction, body->material.restitution,
						body->motion.motionState.deactivationClass);
				}
			}
			logger::info("{:08X}: Skyrim ragdoll bodies' friction/bounce/sleep class:{}", a_actor->GetFormID(), ms);
			std::string ls;
			for (auto* ci : a.sky.driver->ragdoll->constraints) {
				auto* d = ci ? ci->data : nullptr;
				if (!d) {
					continue;
				}
				std::string who = "?", dlim;
				for (const auto& b : a.sky.bodies) {
					if (b.body && reinterpret_cast<RE::hkpEntity*>(b.body) == ci->entities[0]) {
						who = b.node ? b.node->name.c_str() : "?";
						if (b.mapped && b.rotPart > 0) {
							float lim[3]{};
							const int kind = dl.nm_agent_get_joint(a.agent, b.rotPart, nullptr, nullptr, nullptr, lim, nullptr);
							dlim = kind == 2 ? fmt::format(" | Delirium {} hinge {:.0f}..{:.0f}", rig_[static_cast<std::size_t>(b.rotPart)].name, lim[0] * 57.3f, lim[1] * 57.3f) :
							       kind == 1 ? fmt::format(" | Delirium {} lean +-{:.0f}/+-{:.0f} twist +-{:.0f}", rig_[static_cast<std::size_t>(b.rotPart)].name, lim[0] * 57.3f,
							                       lim[1] * 57.3f, lim[2] * 57.3f) : "";
						}
						break;
					}
				}
				ls += fmt::format("\n    {}:", who);
				if (d->GetType() == 7) {
					auto& at = static_cast<RE::hkpRagdollConstraintData*>(d)->atoms;
					ls += fmt::format(" ragdoll(twist {:.0f}..{:.0f}, cone {:.0f}, planes {:.0f}..{:.0f}, stab {})", at.twistLimit.minAngle * 57.3f,
						at.twistLimit.maxAngle * 57.3f, at.coneLimit.maxAngle * 57.3f, at.planesLimit.minAngle * 57.3f, at.planesLimit.maxAngle * 57.3f,
						at.setupStabilization.enabled);
					ls += dlim;
				} else if (d->GetType() == 2) {
					auto& at = static_cast<RE::hkpLimitedHingeConstraintData*>(d)->atoms;
					ls += fmt::format(" hinge({:.0f}..{:.0f}, stab {})", at.angLimit.minAngle * 57.3f, at.angLimit.maxAngle * 57.3f,
						at.setupStabilization.enabled);
					ls += dlim;
				}
			}
			logger::info("{:08X}: Skyrim ragdoll joint limits (deg):{}", a_actor->GetFormID(), ls);
		}
		DumpRagdoll(a, a_actor);
	}

}

// Taking an NPC into Delirium and letting her go: her body, world collision, Skyrim's ragdoll taken over
// and its velocities, her pose as Delirium's parts.
#include "shared.h"

namespace delirium
{
	namespace
	{
		// the bone by its exact name; a creature's (a_loose) also by its name without the tag (SameBone)
		RE::NiAVObject* Node(RE::NiAVObject* a_root, const std::string& a_name, bool a_loose = false)
		{
			if (auto* n = a_root->GetObjectByName(RE::BSFixedString(a_name.c_str())); n || !a_loose) {
				return n;
			}
			std::function<RE::NiAVObject*(RE::NiAVObject*)> find = [&](RE::NiAVObject* o) -> RE::NiAVObject* {
				if (SameBone(a_name, o->name.c_str(), true)) {
					return o;
				}
				if (auto* node = o->AsNode()) {
					for (auto& c : node->GetChildren()) {
						if (auto* f = c ? find(c.get()) : nullptr) {
							return f;
						}
					}
				}
				return nullptr;
			};
			return find(a_root);
		}
	}

	bool Engine::Activate(RE::Actor* a_actor, bool a_takeover, bool a_death)
	{
		auto* root = a_actor->Get3D(false);
		if (!root) {
			return false;
		}
		HookUpdate3D(a_actor);
		auto act = std::make_unique<Active>();
		act->actor = a_actor->GetHandle();
		act->origin = a_actor->GetPosition();
		act->root.reset(root);
		auto* race = a_actor->GetRace();
		act->looseBones = race && Creature(a_actor);
		for (std::size_t i = 0; i < 21; ++i) {
			act->ref[i] = Node(root, rig_[i].ref, act->looseBones);
			act->ja[i] = Node(root, rig_[i].jointA, act->looseBones);
			act->jb[i] = Node(root, rig_[i].jointB, act->looseBones);
			if (!act->ref[i] || !act->ja[i] || !act->jb[i]) {
				logger::warn("{:08X}: no bone for {} ({})", a_actor->GetFormID(), rig_[i].name, rig_[i].ref);
				return false;
			}
		}
		act->com = Node(root, "NPC COM [COM ]");
		if (!act->com || !act->com->parent) {
			logger::warn("{:08X}: no NPC COM bone", a_actor->GetFormID());
			return false;
		}
		act->comInPelvis = act->ref[0]->world.rotate.Transpose() * act->com->world.rotate;
		// the NPC's size (race height, NPC weight scale): the body and the rig offsets are for a scale-1 skeleton
		act->scale = std::clamp(act->ref[0]->world.scale, 0.3f, 3.0f);
		// a creature's body on her own bones (her limbs aren't a person's lengths); a person's: the rig at her scale
		NmBuild* fit = act->looseBones ? FitCreature(*act, a_actor) : nullptr;
		if (!fit) {
			PersonOffsets(*act);
		}
		std::array<V3, 21> pos;
		std::array<Q, 21>  rot;
		std::string        detail;
		const float        gap = PartsFromBones(*act, pos, rot, &detail);
		logger::info("{:08X}: joint vs bone gap, worst {:.3} m:{}", a_actor->GetFormID(), gap, detail);
		// the pose for replaying offline (fit-test REPLAY=): per part, ref bone world rotation rows and the joint bones' positions
		{
			auto rel = [&](RE::NiAVObject* n) {
				const auto w = (n->world.translate - act->origin) * kUnit;
				return V3{ w.x, w.y, w.z };
			};
			std::ostringstream f;
			f << "heading " << a_actor->data.angle.z << " scale " << act->ref[0]->world.scale << "\n";
			for (std::size_t i = 0; i < 21; ++i) {
				const auto& m = act->ref[i]->world.rotate;
				const V3    r = rel(act->ref[i]), a = rel(act->ja[i]), b = rel(act->jb[i]);
				f << rig_[i].name << "|";
				for (int r0 = 0; r0 < 3; ++r0) {
					for (int c = 0; c < 3; ++c) {
						f << m.entry[r0][c] << " ";
					}
				}
				f << "|" << r.x << " " << r.y << " " << r.z << "|" << a.x << " " << a.y << " " << a.z << "|" << b.x << " " << b.y << " " << b.z << "\n";
			}
			std::ofstream(*SKSE::log::log_directory() / "SkyrimDelirium_pose.txt") << f.str();
		}
		const float heading = a_actor->data.angle.z;  // forward = (sin, cos)
		const float yaw = std::atan2(std::cos(heading), std::sin(heading)) - rigYaw_;
		const float zero[3]{};
		if (fit) {
			// her size is in it already (her own bones); the agent keeps its own copy
			act->agent = dl.nm_agent_create(fit, zero, yaw, 0.3f);
			dl.nm_build_destroy(fit);
		} else if (std::abs(act->scale - 1.0f) > 1e-3f) {
			// a copy of the body at the NPC's size (mass by volume); the agent keeps its own copy
			std::array<float, 21> len;
			len.fill(act->scale);
			NmBuild* sized = dl.nm_build_scale(build_, len.data(), act->scale * act->scale * act->scale);
			if (!sized) {
				logger::error("scale {:.2}: {}", act->scale, LastError());
				return false;
			}
			act->agent = dl.nm_agent_create(sized, zero, yaw, 0.3f);
			dl.nm_build_destroy(sized);
		} else {
			act->agent = dl.nm_agent_create(build_, zero, yaw, 0.3f);
		}
		act->world = dl.nm_world_create();
		if (!act->agent || !act->world) {
			logger::error("agent: {}", LastError());
			return false;
		}
		AddWorld(*act, a_actor, act->origin, false);
		{
			// the agent starts in its build's standing stance: kept, each part in its parent's frame, for StandingLegs
			std::array<V3, 21> rp;
			std::array<Q, 21>  rq;
			for (int i = 0; i < 21; ++i) {
				float p[3]{}, q[4]{ 0, 0, 0, 1 };
				dl.nm_agent_get_part_transform(act->agent, i, p, q);
				rp[i] = { p[0], p[1], p[2] };
				rq[i] = { q[0], q[1], q[2], q[3] };
			}
			for (std::size_t i = 1; i < 21; ++i) {
				const auto pa = static_cast<std::size_t>(kParent[i]);
				act->restRot[i] = Mul(Conj(rq[pa]), rq[i]);
				act->restPos[i] = Rot(Conj(rq[pa]), Sub(rp[i], rp[pa]));
			}
		}
		dl.nm_agent_set_mode(act->agent, NM_MODE_LEAD);
		// in the air (a fall, a jump) she goes in moving as her animation moved her: no stop dead. On the ground the
		// balancer starts from a stand, and with a run's speed and its swinging limbs she fell to any blow (seen: a 20
		// damage spell on a runner, down in 0.3 s): she keeps [Test] fCarrySpeed of her travel, the whole body as one
		// and level, none of the limbs' swing. Taken over from Skyrim's ragdoll its bodies' own speeds come in later
		// (InheritVelocity)
		std::array<V3, 21> pv{}, pw{};
		if (!a_takeover) {
			PartSpeeds(*act, pos, rot, pv, pw);
			auto* cc = a_actor->GetCharController();
			if (!cc || cc->context.currentState != RE::hkpCharacterStateType::kInAir) {
				pv.fill({ pv[0].x * carrySpeed_, pv[0].y * carrySpeed_, 0.0f });
				pw.fill({});
			}
		}
		for (int i = 0; i < 21; ++i) {
			const float p[3]{ pos[i].x, pos[i].y, pos[i].z }, q[4]{ rot[i].x, rot[i].y, rot[i].z, rot[i].w };
			const float lv[3]{ pv[i].x, pv[i].y, pv[i].z }, av[3]{ pw[i].x, pw[i].y, pw[i].z };
			dl.nm_agent_set_part_state(act->agent, i, p, q, lv, av);
		}
		logger::info("  went in moving: pelvis {:.2f} {:.2f} {:.2f} m/s", pv[0].x, pv[0].y, pv[0].z);
		// the posture the muscles and balance hold: the NPC's own pose, not the GTA character's stance (standing: her
		// legs from the body's own stance, see StandingLegs)
		{
			auto zp = pos;
			auto zr = rot;
			if (!a_takeover) {
				StandingLegs(*act, zp, zr);
			}
			SetZeroPose(*act, zp, zr);
		}
		// upright (F7): balance. Taken over from Skyrim's ragdoll: already falling, so break the fall
		// (dead: the shot task once Skyrim's velocities are in, see InheritVelocity)
		if (!a_death) {
			dl.nm_msg_begin(act->agent, a_takeover ? "catchFall" : "bodyBalance");
			dl.nm_msg_set_bool(act->agent, "start", 1);
			dl.nm_msg_send(act->agent, act->world);
		}
		act->takeover = a_takeover;
		act->death = a_death;
		act->knockQueued = a_takeover;  // Skyrim's ragdoll state is already on: no knock of our own
		logger::info("{:08X} '{}': active{}; scale {:.2}, pelvis at {:.2} m, head at {:.2} m, heading {:.2}", a_actor->GetFormID(), a_actor->GetName(),
			a_takeover ? " (took over Skyrim's ragdoll)" : "", act->scale, pos[0].z, pos[kHead].z, heading);
		active_.push_back(std::move(act));
		g_count = static_cast<int>(active_.size());
		return true;
	}

	void Engine::AddWorld(Active& a, RE::Actor* a_actor, const RE::NiPoint3& a_center, bool a_quiet)
	{
		// Skyrim's own collision around a_center (floors, walls, stairs, terrain, rocks, trees), in metres from the
		// NPC's origin; a flat floor at its feet if there is none (first world only)
		a.colCenter = a_center;
		bool        ground = false;
		std::size_t tris = 0;
		if (collision_) {
			const auto t0 = std::chrono::steady_clock::now();
			Tris        mesh;
			GatherStats st;
			const bool  ok = GatherStatic(a_actor->GetParentCell(), a_center, a.origin, colRadius_, colHeight_, colMaxTris_, mesh, st);
			if (ok && mesh.Count() > 0) {
				if (dl.nm_world_add_mesh(a.world, mesh.pos.data(), static_cast<std::int32_t>(mesh.pos.size() / 3), mesh.idx.data(),
						static_cast<std::int32_t>(mesh.Count()), nullptr) < 0) {
					logger::error("world mesh: {}", LastError());
				}
			}
			tris = static_cast<std::size_t>(std::max(0, dl.nm_world_triangle_count(a.world)));
			if (a_quiet) {
				const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
				logger::info("  world re-gathered around the body ({:.1f} ms): {} triangles", ms, tris);
				return;
			}
			// what is under the pelvis, and which way it faces (a downward face = triangles wound inside out)
			const float from[3]{ 0, 0, 1.0f }, down[3]{ 0, 0, -1.0f };
			float       dist = 0, n[3]{};
			ground = tris > 0 && dl.nm_world_raycast(a.world, from, down, 2.5f, &dist, n) > 0;
			const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
			logger::info("world: {} ({:.1f} ms): {} bodies, {} shapes, {} triangles kept ({} mesh tris, {} boxes, {} hulls, {} boxed, {} culled, {} over cap, {} other layers){}{}",
				ok ? "Skyrim collision" : "no Havok world", ms, st.bodies, st.shapes, tris, st.triangles, st.boxes, st.hulls, st.fallbacks, st.culled,
				st.capped, st.skippedLayer, ground ? fmt::format("; ground {:.2} m below the feet+1m, normal {:.2} {:.2} {:.2}", dist, n[0], n[1], n[2]) : "; no ground under the pelvis",
				st.unknown.empty() ? "" : "; boxed:" + st.unknown);
		}
		if (!ground) {
			dl.nm_world_add_floor(a.world, 0, 0, 0, 100.0f);
			logger::info("world: flat floor at the feet");
		}
	}

	float Engine::PartsFromBones(const Active& a, std::array<V3, 21>& pos, std::array<Q, 21>& rot, std::string* detail, bool a_previous) const
	{
		// parts from the bones as Skyrim posed them: rotations through the rig, positions down the chain from the pelvis.
		// Returns the worst gap between a physics joint and its Skyrim bone (calibration check).
		auto frame = [&](RE::NiAVObject* n) -> const RE::NiTransform& { return a_previous ? n->previousWorld : n->world; };
		auto rel = [&](RE::NiAVObject* n) {
			const auto w = (frame(n).translate - a.origin) * kUnit;
			return V3{ w.x, w.y, w.z };
		};
		float worst = 0;
		for (std::size_t i = 0; i < 21; ++i) {
			rot[i] = Mul(FromMat(frame(a.ref[i]).rotate), Conj(rig_[i].offset));
			const V3 anchor = a.anchor[i];  // at her size (body/fit.cpp)
			if (i == 0) {
				pos[0] = Sub(rel(a.ja[0]), Rot(rot[0], anchor));
			} else {
				const auto p = static_cast<std::size_t>(kParent[i]);
				pos[i] = Sub(Add(pos[p], Rot(rot[p], a.inParent[i])), Rot(rot[i], anchor));
			}
			if (detail) {
				const V3    j = Add(pos[i], Rot(rot[i], anchor));
				const V3    ja = rel(a.ja[i]), jb = rel(a.jb[i]);
				const V3    d = Sub(j, { (ja.x + jb.x) / 2, (ja.y + jb.y) / 2, (ja.z + jb.z) / 2 });
				const float g = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
				worst = std::max(worst, g);
				*detail += fmt::format(" {}={:.2}", rig_[i].name, g);
			}
		}
		return worst;
	}

	void Engine::PartSpeeds(const Active& a, const std::array<V3, 21>& pos, const std::array<Q, 21>& rot, std::array<V3, 21>& v, std::array<V3, 21>& w) const
	{
		v.fill({});
		w.fill({});
		const float dt = RE::GetSecondsSinceLastFrame();
		if (!(dt > 1e-3f && dt < 0.1f)) {
			return;
		}
		std::array<V3, 21> pp;
		std::array<Q, 21>  pr;
		PartsFromBones(a, pp, pr, nullptr, true);
		// a teleport, a fresh 3D (last frame is the origin or junk), or no speed at all: still, as before
		const V3    d0 = Sub(pos[0], pp[0]);
		const float jump = std::sqrt(d0.x * d0.x + d0.y * d0.y + d0.z * d0.z);
		if (!std::isfinite(jump) || jump > 0.5f) {
			return;
		}
		constexpr float kMaxV = 12.0f, kMaxW = 25.0f;  // m/s, rad/s: a sprint and a fast swing, no more
		for (std::size_t i = 0; i < 21; ++i) {
			V3 lv = Scl(Sub(pos[i], pp[i]), 1.0f / dt);
			Q  dq = Mul(rot[i], Conj(pr[i]));  // last frame's turn to this frame's, in the world
			if (dq.w < 0.0f) {
				dq = { -dq.x, -dq.y, -dq.z, -dq.w };
			}
			const float s = std::sqrt(dq.x * dq.x + dq.y * dq.y + dq.z * dq.z);
			const float ang = 2.0f * std::atan2(s, dq.w);
			V3 av = s > 1e-6f ? Scl({ dq.x, dq.y, dq.z }, ang / (s * dt)) : V3{};
			const float lvn = std::sqrt(lv.x * lv.x + lv.y * lv.y + lv.z * lv.z), avn = std::sqrt(av.x * av.x + av.y * av.y + av.z * av.z);
			if (!std::isfinite(lvn + avn)) {
				v.fill({});
				w.fill({});
				return;
			}
			v[i] = lvn > kMaxV ? Scl(lv, kMaxV / lvn) : lv;
			w[i] = avn > kMaxW ? Scl(av, kMaxW / avn) : av;
		}
	}

	// [Test] bStandingLegs: while she stands the hips, knees and ankles hold the body's own stance, soles level, under
	// the animation's pelvis; the spine, arms and head keep the animation. Skyrim's combat animations lunge, step and
	// tiptoe with planted feet, and holding that pose tipped her over with no hit at all (fit-test STANCE_TRACK on the
	// recorded tracks: 100% falls, 0% with these legs)
	void Engine::StandingLegs(const Active& a, std::array<V3, 21>& pos, std::array<Q, 21>& rot) const
	{
		if (!standingLegs_) {
			return;
		}
		for (std::size_t i = 1; i <= 6; ++i) {
			const auto p = static_cast<std::size_t>(kParent[i]);
			rot[i] = Mul(rot[p], a.restRot[i]);
			pos[i] = Add(pos[p], Rot(rot[p], a.restPos[i]));
		}
		for (std::size_t i : { std::size_t{ 3 }, std::size_t{ 6 } }) {
			// a foot's local y is up when it stands flat: turn it the shortest way back to world up
			const V3    u = Rot(rot[i], { 0, 1, 0 });
			const Q     t{ u.y, -u.x, 0.0f, 1.0f + u.z };  // half-way quaternion from u to +z (u x z, 1 + u.z)
			const float n = std::sqrt(t.x * t.x + t.y * t.y + t.z * t.z + t.w * t.w);
			if (n > 1e-4f) {
				rot[i] = Mul({ t.x / n, t.y / n, t.z / n, t.w / n }, rot[i]);
			}
		}
	}

	void Engine::SetZeroPose(Active& a, const std::array<V3, 21>& pos, const std::array<Q, 21>& rot)
	{
		std::array<float, 21 * 7> tf{};
		for (std::size_t i = 0; i < 21; ++i) {
			const float v[7]{ pos[i].x, pos[i].y, pos[i].z, rot[i].x, rot[i].y, rot[i].z, rot[i].w };
			std::copy_n(v, 7, tf.begin() + static_cast<std::ptrdiff_t>(i * 7));
		}
		if (dl.nm_agent_set_zero_pose(a.agent, tf.data(), 21) < 0 && !a.logged) {
			logger::error("zero pose: {}", LastError());
			a.logged = true;
		}
	}

	void Engine::Release(std::size_t a_i, const char* a_why)
	{
		auto& a = active_[a_i];
		logger::info("released ({}) after {:.1} s", a_why, a->age);
		Unshow(*a);  // her meshes read her bones again (body/show.cpp)
		FreeCombat(*a);
		Unpin(*a, "let go");
		RestoreJoints(*a);
		for (std::size_t k = 0; k < a->sky.weighed.size(); ++k) {
			if (auto* body = a->sky.weighed[k]) {
				std::memcpy(&body->motion.inertiaAndMassInv, a->sky.bodies[k].massInv.data(), sizeof(a->sky.bodies[k].massInv));
				if (k < a->sky.material.size()) {
					body->material.friction = a->sky.material[k][0];
					body->material.restitution = a->sky.material[k][1];
				}
				body->RemoveReference();
			}
		}
		a->sky.weighed.clear();
		if (auto act = a->actor.get(); act && act->IsInRagdollState()) {
			releasedAt_[act->GetFormID()] = std::chrono::steady_clock::now();
			skip_.push_back(a->actor);  // still in Skyrim's ragdoll (its get-up, or F2): not taken over again
			g_skip = static_cast<int>(skip_.size());
		}
		dl.nm_agent_destroy(a->agent);
		dl.nm_world_destroy(a->world);
		active_.erase(active_.begin() + static_cast<std::ptrdiff_t>(a_i));
		g_count = static_cast<int>(active_.size());
	}

	// Every NPC's animation update comes through here, so any way into Skyrim's ragdoll is seen the frame it starts:
	// Fus Ro Dah, PushActorAway, explosions, other mods. Delirium takes the body from the pose on screen, with the
	// velocities Skyrim's push gave the ragdoll bodies (InheritVelocity), and keeps it until Skyrim's get-up, like F7.
	void Engine::TryTakeOver(RE::Actor* a_actor, bool a_dead)
	{
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
		if (a_dead) {
			// Skyrim's death ragdoll: Delirium dies instead (the shot task at no health, InheritVelocity), the corpse is
			// held still once it lies there
			if (!a_actor->Get3D(false)) {
				return;
			}
			if (a_actor->AsActorState()->GetLifeState() != RE::ACTOR_LIFE_STATE::kDying) {
				skip_.push_back(a_actor->GetHandle());
				g_skip = static_cast<int>(skip_.size());
				logger::info("{:08X} '{}': a corpse already lying there, left to Skyrim", a_actor->GetFormID(), a_actor->GetName());
				return;
			}
			logger::info("{:08X} '{}': died into Skyrim's ragdoll (knock state {}): taking over", a_actor->GetFormID(), a_actor->GetName(),
				static_cast<int>(a_actor->AsActorState()->GetKnockState()));
			if (!Activate(a_actor, true, true)) {
				skip_.push_back(a_actor->GetHandle());
				g_skip = static_cast<int>(skip_.size());
				logger::info("{:08X}: dead, left to Skyrim: no Delirium body for this skeleton", a_actor->GetFormID());
			}
			return;
		}
		using K = RE::KNOCK_STATE_ENUM;
		const auto ks = a_actor->AsActorState()->GetKnockState();
		if (ks == K::kNormal || !a_actor->Get3D(false)) {
			return;  // not knocked (yet): looked at again next frame
		}
		auto skip = [&](std::string_view a_why) {
			// until it leaves the ragdoll state (getting up, or let go of by us)
			skip_.push_back(a_actor->GetHandle());
			g_skip = static_cast<int>(skip_.size());
			logger::info("{:08X} '{}': Skyrim ragdoll (knock state {}), left to Skyrim: {}", a_actor->GetFormID(), a_actor->GetName(), static_cast<int>(ks), a_why);
		};
		if (ks == K::kQueued || ks == K::kGetUp) {
			return skip("already getting up");
		}
		if (a_actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kParalysis) > 0.0f) {
			return skip("paralysed (Skyrim holds the body stiff)");
		}
		// the lead-in comes before Skyrim's push: taken over then, the push would hit puppet bodies (no mass) and do
		// nothing. Skyrim's ragdoll runs it; Delirium starts once the push is on the bodies.
		{
			float mean = 0, fastest = 0, mass = 0;
			RE::BSAnimationGraphManagerPtr mgr;
			if (a_actor->GetAnimationGraphManager(mgr) && mgr && !mgr->graphs.empty() && mgr->graphs[0]) {
				if (auto* d = mgr->graphs[0]->characterInstance.ragdollDriver.get(); d && d->ragdoll) {
					for (auto* b : d->ragdoll->rigidBodies) {
						if (!b) {
							continue;
						}
						const auto* lv = reinterpret_cast<const float*>(&b->motion.linearVelocity);
						const float im = reinterpret_cast<const float*>(&b->motion.inertiaAndMassInv)[3];
						const float m = im > 0.0f ? 1.0f / im : 0.0f, s = std::sqrt(lv[0] * lv[0] + lv[1] * lv[1] + lv[2] * lv[2]);
						mean += m * s, mass += m, fastest = std::max(fastest, s);
					}
				}
			}
			mean = mass > 0.0f ? mean / mass : 0.0f;
			logger::info("{:08X}: Skyrim ragdoll, knock state {}: bodies {:.2f} m/s mass-weighted, fastest {:.2f} m/s", a_actor->GetFormID(), static_cast<int>(ks),
				mean, fastest);
			if (ks == K::kExplodeLeadIn || ks == K::kOutLeadIn) {
				return;  // looked at again next frame
			}
		}
		logger::info("{:08X} '{}': into Skyrim's ragdoll (knock state {}): taking over", a_actor->GetFormID(), a_actor->GetName(), static_cast<int>(ks));
		if (!Activate(a_actor, true)) {
			return skip("no Delirium body for this skeleton");
		}
	}

	void Engine::ForgetSkip(RE::Actor* a_actor)
	{
		std::scoped_lock l(lock_);
		if (auto it = releasedAt_.find(a_actor->GetFormID()); it != releasedAt_.end()) {
			logger::info("{:08X}: up from Skyrim's ragdoll {:.1f} s after we let go", a_actor->GetFormID(),
				std::chrono::duration<float>(std::chrono::steady_clock::now() - it->second).count());
			releasedAt_.erase(it);
		}
		std::erase_if(skip_, [&](const RE::ActorHandle& h) {
			auto a = h.get();
			return !a || a.get() == a_actor;
		});
		g_skip = static_cast<int>(skip_.size());
	}

	// Taken over: Skyrim's ragdoll bodies have run a physics step or two with the push Skyrim gave them. Delirium's
	// parts start with those velocities (a part without a body of its own takes its parent's).
	// Skyrim's ragdoll bodies' velocities, per Delirium part (parts without a body: see below and the caller); returns the
	// fastest body's speed
	float Engine::BodyVelocities(const Active& a, std::array<V3, 21>& v, std::array<V3, 21>& w, std::array<bool, 21>& have) const
	{
		float fastest = 0;
		for (const auto& b : a.sky.bodies) {
			const int i = b.rotPart >= 0 ? b.rotPart : b.posPart;
			if (i < 0 || have[i]) {
				continue;
			}
			const auto* lv = reinterpret_cast<const float*>(&b.body->motion.linearVelocity);
			const auto* av = reinterpret_cast<const float*>(&b.body->motion.angularVelocity);
			v[i] = { lv[0], lv[1], lv[2] };
			w[i] = { av[0], av[1], av[2] };
			if (!std::isfinite(v[i].x + v[i].y + v[i].z + w[i].x + w[i].y + w[i].z)) {
				continue;
			}
			have[i] = true;
			fastest = std::max(fastest, std::sqrt(v[i].x * v[i].x + v[i].y * v[i].y + v[i].z * v[i].z));
		}
		// a part without a body of its own but carrying one (Skyrim's root body NPC COM rides on the pelvis): that body's
		// velocity. The pelvis is Delirium's root: left at rest, the joint solve would pull the whole body back to it.
		for (const auto& b : a.sky.bodies) {
			if (b.follow < 0 || b.follow >= 21 || have[b.follow]) {
				continue;
			}
			const auto* lv = reinterpret_cast<const float*>(&b.body->motion.linearVelocity);
			const auto* av = reinterpret_cast<const float*>(&b.body->motion.angularVelocity);
			const V3 lin{ lv[0], lv[1], lv[2] }, ang{ av[0], av[1], av[2] };
			if (std::isfinite(lin.x + lin.y + lin.z + ang.x + ang.y + ang.z)) {
				v[b.follow] = lin;
				w[b.follow] = ang;
				have[b.follow] = true;
			}
		}
		if (!have[0]) {
			// else the mass-weighted mean of all bodies (no spin)
			V3    sum{};
			float mass = 0;
			for (const auto& b : a.sky.bodies) {
				const auto* lv = reinterpret_cast<const float*>(&b.body->motion.linearVelocity);
				const float im = b.massInv[3];
				const float m = im > 0.0f ? 1.0f / im : 0.0f;
				if (m > 0.0f && std::isfinite(lv[0] + lv[1] + lv[2])) {
					sum = Add(sum, Scl({ lv[0], lv[1], lv[2] }, m));
					mass += m;
				}
			}
			if (mass > 0.0f) {
				v[0] = Scl(sum, 1.0f / mass);
				have[0] = true;
			}
		}
		return fastest;
	}

	// Native: Skyrim's ragdoll is the body. After each physics step Delirium's parts are put where Skyrim's ragdoll is
	// (through the bones Skyrim just posed from it, down Delirium's own joint chain) and move as its bodies move; Delirium
	// then steps from there, and that step is where DriveRagdoll steers Skyrim's bodies next
	// Delirium's parts straight from Skyrim's ragdoll bodies: the exact inverse of BodyTarget (and of the riding bodies'
	// formula in DriveRagdoll), so Skyrim -> Delirium -> Skyrim lands every body where it was. The bones lag the bodies by a
	// physics step, so they only fill in what no body gives (a part's rotation when its body gives only the position, and
	// the parts with no body at all, down the chain from their parent)
	void Engine::PartsFromBodies(const Active& a, std::array<V3, 21>& pos, std::array<Q, 21>& rot) const
	{
		PartsFromBones(a, pos, rot, nullptr);
		const V3             o = Scl({ a.origin.x, a.origin.y, a.origin.z }, kUnit);
		std::array<bool, 21> gotR{}, gotP{};
		std::array<V3, 21>   joint{};
		for (const auto& b : a.sky.bodies) {
			const auto& ms = b.body->motion.motionState;
			const auto* cl = reinterpret_cast<const float*>(&ms.sweptTransform.centerOfMassLocal);
			const auto* cw = reinterpret_cast<const float*>(&ms.sweptTransform.centerOfMass1);
			const auto* qc = reinterpret_cast<const float*>(&ms.sweptTransform.rotation1);
			const Q     qb{ qc[0], qc[1], qc[2], qc[3] };
			const V3    pb = Sub({ cw[0], cw[1], cw[2] }, Rot(qb, { cl[0], cl[1], cl[2] }));
			// body = bone * (mapR, mapT)  =>  bone = body * (mapR, mapT)^-1
			Q   nr = Mul(qb, Conj(b.mapR));
			V3  np = Sub(pb, Rot(nr, Scl(b.mapT, a.scale)));
			int rp = -1, pp = -1;
			if (b.mapped) {
				rp = b.rotPart;
				pp = b.posPart;
			} else if (b.follow >= 0) {
				// its own bone rides on the follow bone by (followR, followT)
				const Q br = Mul(nr, Conj(b.followR));
				np = Sub(np, Rot(br, b.followT));
				nr = br;
				rp = pp = b.follow;
			} else {
				continue;
			}
			if (!gotR[rp]) {
				rot[rp] = Mul(nr, Conj(rig_[rp].offset));
				gotR[rp] = true;
			}
			if (!gotP[pp]) {
				joint[pp] = Sub(np, o);
				gotP[pp] = true;
			}
		}
		for (std::size_t i = 0; i < 21; ++i) {
			const V3 anchor = a.anchor[i];
			if (gotP[i]) {
				pos[i] = Sub(joint[i], Rot(rot[i], anchor));
			} else if (kParent[i] >= 0) {
				const auto p = static_cast<std::size_t>(kParent[i]);
				pos[i] = Sub(Add(pos[p], Rot(rot[p], a.inParent[i])), Rot(rot[i], anchor));
			}
		}
	}

	void Engine::SyncFromSkyrim(Active& a)
	{
		std::array<V3, 21> pos;
		std::array<Q, 21>  rot;
		PartsFromBodies(a, pos, rot);
		std::array<V3, 21>   v{}, w{};
		std::array<bool, 21> have{};
		BodyVelocities(a, v, w, have);
		for (int i = 1; i < 21; ++i) {
			if (!have[i] && kParent[i] >= 0) {
				v[i] = v[kParent[i]];
				w[i] = w[kParent[i]];
			}
		}
		for (int i = 0; i < 21; ++i) {
			if (!std::isfinite(pos[i].x + pos[i].y + pos[i].z + rot[i].x + rot[i].y + rot[i].z + rot[i].w + v[i].x + v[i].y + v[i].z + w[i].x + w[i].y + w[i].z)) {
				if (a.sky.nanLog < 30) {
					++a.sky.nanLog;
					logger::error("    NaN sync age {:.3f}: part {}; Delirium left as it is", a.age, i);
				}
				return;
			}
		}
		for (int i = 0; i < 21; ++i) {
			const float p[3]{ pos[i].x, pos[i].y, pos[i].z }, q[4]{ rot[i].x, rot[i].y, rot[i].z, rot[i].w };
			const float lv[3]{ v[i].x, v[i].y, v[i].z }, av[3]{ w[i].x, w[i].y, w[i].z };
			dl.nm_agent_set_part_state(a.agent, i, p, q, lv, av);
		}
	}

	void Engine::InheritVelocity(Active& a)
	{
		std::array<V3, 21> v{}, w{};
		std::array<bool, 21> have{};
		const float fastest = BodyVelocities(a, v, w, have);
		int set = 0;
		for (int i = 0; i < 21; ++i) {
			if (!have[i] && i > 0 && kParent[i] >= 0) {
				v[i] = v[kParent[i]];
				w[i] = w[kParent[i]];
				have[i] = have[kParent[i]];
			}
			if (!have[i]) {
				continue;
			}
			float p[3]{}, q[4]{};
			dl.nm_agent_get_part_transform(a.agent, i, p, q);
			const float lv[3]{ v[i].x, v[i].y, v[i].z }, av[3]{ w[i].x, w[i].y, w[i].z };
			dl.nm_agent_set_part_state(a.agent, i, p, q, lv, av);
			++set;
		}
		logger::info("  took Skyrim's ragdoll velocities: {} of 21 parts, fastest body {:.2f} m/s", set, fastest);
		float cp[3]{}, cv[3]{};
		dl.nm_agent_get_com(a.agent, cp, cv);
		logger::info("  Delirium's body now moves at {:.2f} {:.2f} {:.2f} m/s", cv[0], cv[1], cv[2]);
		if (a.death) {
			bool held = false;
			if (auto self = a.actor.get()) {
				held = heldDied_.erase(self->GetFormID()) > 0;
			}
			StartDying(a, { cv[0], cv[1], 0.0f }, held);
		} else if (a.takeover) {
			Knocked(a, { cv[0], cv[1], cv[2] });
		}
	}

}

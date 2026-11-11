// Delirium driving Skyrim's ragdoll: joint friction, which body each part drives, the muscles' torques,
// the bodies' targets, the root and the bones written back.
#include "shared.h"

namespace delirium
{
	namespace
	{
		constexpr float kMaxSpin = 3.0f * 6.2831853f;  // Delirium's MAX_SPIN_NM (rad/s)
		constexpr float kMaxExtTurn = 4.0f;              // the behaviours' own torques: most a body turns from them in a step (rad/s)
	}

	namespace
	{
		RE::hkpAngFrictionConstraintAtom* FrictionOf(RE::hkpConstraintData* d)
		{
			switch (d->GetType()) {
			case 7:
				return &static_cast<RE::hkpRagdollConstraintData*>(d)->atoms.angFriction;
			case 2:
				return &static_cast<RE::hkpLimitedHingeConstraintData*>(d)->atoms.angFriction;
			default:
				return nullptr;
			}
		}
	}

	// Skyrim's ragdoll joints carry friction (10 N m on most): it takes ~3 rad/s a step out of a forearm, more than the
	// behaviours' steady push, so a writhe or a shock died after the landing. Off while Delirium drives, back on release.
	void Engine::EaseJoints(Active& a)
	{
		if (friction_ >= 1.0f || !a.sky.eased.empty() || !a.sky.driver || !a.sky.driver->ragdoll) {
			return;
		}
		int n = 0;
		for (auto* ci : a.sky.driver->ragdoll->constraints) {
			auto* d = ci ? ci->data : nullptr;
			auto* f = d ? FrictionOf(d) : nullptr;
			if (!f) {
				continue;
			}
			auto it = eased_.find(d);
			if (it == eased_.end()) {
				if (f->maxFrictionTorque <= 0.0f) {
					continue;
				}
				it = eased_.emplace(d, std::pair{ f->maxFrictionTorque, 0 }).first;
				f->maxFrictionTorque = it->second.first * friction_;
			}
			++it->second.second;
			d->AddReference();
			a.sky.eased.push_back(d);
			++n;
		}
		logger::info("  joint friction x{:.2f} on {} joints while Delirium drives", friction_, n);
	}

	void Engine::RestoreJoints(Active& a)
	{
		for (auto* d : a.sky.eased) {
			if (auto it = eased_.find(d); it != eased_.end() && --it->second.second <= 0) {
				if (auto* f = FrictionOf(d)) {
					f->maxFrictionTorque = it->second.first;
				}
				eased_.erase(it);
			}
			d->RemoveReference();
		}
		a.sky.eased.clear();
	}

	bool Engine::DeliriumBone(const Active& a, const BodyLink& l, V3& a_pos, Q& a_rot) const
	{
		if (!l.mapped) {
			return false;
		}
		float p[3]{}, q[4]{};
		dl.nm_agent_get_part_transform(a.agent, l.rotPart, p, q);
		a_rot = Mul({ q[0], q[1], q[2], q[3] }, rig_[l.rotPart].offset);
		dl.nm_agent_get_part_transform(a.agent, l.posPart, p, q);
		const V3 j = Add({ p[0], p[1], p[2] }, Rot({ q[0], q[1], q[2], q[3] }, a.anchor[static_cast<std::size_t>(l.posPart)]));
		a_pos = Add(Scl({ a.origin.x, a.origin.y, a.origin.z }, kUnit), j);
		return true;
	}

	float Engine::LeadShare(const Active& a) const
	{
		if (!a.ragdolled || a.takeover) {
			return a.ragdolled ? 0.0f : 1.0f;
		}
		return std::clamp(1.0f - (a.age - a.ragdollAge) / 0.15f, 0.0f, 1.0f);
	}

	bool Engine::TorqueMode(const Active& a) const
	{
		// F7: Delirium leads the first 0.15 s (the push is Delirium's), then follows
		return a.ragdolled && (a.takeover || a.age - a.ragdollAge >= 0.15f);
	}

	// which of Skyrim's bodies each Delirium part's drive lands on: the body it gives its rotation to, else its position,
	// else the body riding on it; parts with no body (clavicles) go to the nearest body up the chain
	void Engine::MapParts(Active& a)
	{
		a.sky.partBody.fill(-1);
		int pivots = 0;
		for (auto& b : a.sky.bodies) {
			b.havePivot = false;
		}
		if (a.sky.driver && a.sky.driver->ragdoll) {
			for (auto* ci : a.sky.driver->ragdoll->constraints) {
				auto* d = ci ? ci->data : nullptr;
				if (!d || (d->GetType() != 7 && d->GetType() != 2)) {
					continue;
				}
				const auto& tA = d->GetType() == 7 ? static_cast<RE::hkpRagdollConstraintData*>(d)->atoms.transforms.transformA :
				                                     static_cast<RE::hkpLimitedHingeConstraintData*>(d)->atoms.transforms.transformA;
				const auto* t = reinterpret_cast<const float*>(&tA.translation);
				for (auto& b : a.sky.bodies) {
					if (b.body && reinterpret_cast<RE::hkpEntity*>(b.body) == ci->entities[0]) {
						b.pivot = { t[0], t[1], t[2] };
						b.havePivot = true;
						++pivots;
						break;
					}
				}
			}
		}
		logger::info("  {} of Skyrim's bodies have their joint's pivot: muscles turn them about it{}", pivots, aboutJoint_ ? "" : " (off)");
		for (int pass = 0; pass < 3; ++pass) {
			for (std::size_t k = 0; k < a.sky.bodies.size(); ++k) {
				const auto& b = a.sky.bodies[k];
				const int   p = pass == 0 ? (b.mapped ? b.rotPart : -1) : pass == 1 ? (b.mapped ? b.posPart : -1) : (b.mapped ? -1 : b.follow);
				if (p >= 0 && p < 21 && a.sky.partBody[p] < 0) {
					a.sky.partBody[p] = static_cast<int>(k);
				}
			}
		}
		for (int p = 0; p < 21; ++p) {
			int up = p;
			while (a.sky.partBody[up] < 0 && kParent[up] >= 0) {
				up = kParent[up];
			}
			if (a.sky.partBody[p] < 0) {
				a.sky.partBody[p] = a.sky.partBody[up];
			}
		}
		std::vector<float> dm(a.sky.bodies.size(), 0.0f);
		for (int p = 0; p < 21; ++p) {
			if (a.sky.partBody[p] >= 0) {
				dm[static_cast<std::size_t>(a.sky.partBody[p])] += a.partMass[static_cast<std::size_t>(p)];  // at her size (body/fit.cpp)
			}
		}
		std::string s;
		float       dt = 0, ht = 0;
		for (std::size_t k = 0; k < a.sky.bodies.size(); ++k) {
			const auto& b = a.sky.bodies[k];
			const float hm = b.massInv[3] > 0 ? 1.0f / b.massInv[3] : 0.0f;
			dt += dm[k];
			ht += hm;
			s += fmt::format(" {} {:.1f}/{:.1f}", b.node ? b.node->name.c_str() : "?", dm[k], hm);
		}
		logger::info("  torque drive: Delirium follows Skyrim's ragdoll, its muscles move it; mass Delirium/Skyrim kg {:.1f}/{:.1f}:{}", dt, ht, s);
		if (matchMass_ && a.sky.weighed.empty()) {
			a.sky.weighed.assign(a.sky.bodies.size(), nullptr);
			int n = 0;
			for (std::size_t k = 0; k < a.sky.bodies.size(); ++k) {
				auto&       b = a.sky.bodies[k];
				const float hm = b.massInv[3] > 0 ? 1.0f / b.massInv[3] : 0.0f;
				if (!b.body || dm[k] < 0.05f || hm <= 0.0f) {
					continue;
				}
				// inverse mass and inertia scale together: same shape, Delirium's weight
				const float r = hm / dm[k];
				float       mi[4];
				for (int e = 0; e < 4; ++e) {
					mi[e] = b.massInv[e] * r;
				}
				b.body->AddReference();
				std::memcpy(&b.body->motion.inertiaAndMassInv, mi, sizeof(mi));
				a.sky.weighed[k] = b.body;
				++n;
				if (matchContact_) {
					// Delirium's grip (feet 1, hands 2.25, else 0.75) and bounce (0.4): Skyrim's 2.5 grip held her limbs to the
					// ground, its 0.8-1.2 bounce kicked them off it
					if (a.sky.material.size() != a.sky.bodies.size()) {
						a.sky.material.assign(a.sky.bodies.size(), { 0.0f, 0.0f });
					}
					a.sky.material[k] = { b.body->material.friction, b.body->material.restitution };
					const std::string nm = b.node ? b.node->name.c_str() : "";
					b.body->material.friction = nm.find("Foot") != std::string::npos ? 1.0f : nm.find("Hand") != std::string::npos ? 2.25f : 0.75f;
					b.body->material.restitution = 0.4f;
				}
			}
			logger::info("  {} of Skyrim's bodies weigh{} as Delirium's parts while it drives", n, matchContact_ ? ", grip and bounce" : "");
		}
	}

	// Delirium's muscles on Skyrim's bodies for one physics step. Its torques are sized for its own whole-body inertia, so
	// on one light Skyrim body (a hand) they spin it up ten times too fast; what carries over is what they do to each
	// joint: the change of the joint's relative spin. That change is split between the joint's two bodies by their own
	// inertia (equal and opposite torque), so the joint turns as Delirium's would and no spin comes from nowhere.
	// Behaviour forces (lift) go on through the body's mass. Gravity, joints, ground and contacts stay Skyrim's.
	void Engine::ApplyTorques(Active& a, float a_dt)
	{
		if (!a.haveDrv) {
			return;
		}
		using M3 = std::array<float, 9>;
		auto mul = [](const M3& m, const V3& v) {
			return V3{ m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z, m[6] * v.x + m[7] * v.y + m[8] * v.z };
		};
		auto len = [](const V3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); };
		const std::size_t n = a.sky.bodies.size();
		std::vector<M3>   inv(n);
		for (std::size_t k = 0; k < n; ++k) {
			// world inverse inertia: R diag(i) R^T (havok keeps it diagonal in the body's frame)
			const auto& mo = a.sky.bodies[k].body->motion;
			const auto* d = reinterpret_cast<const float*>(&mo.inertiaAndMassInv);
			const auto* qc = reinterpret_cast<const float*>(&mo.motionState.sweptTransform.rotation1);
			const Q     q{ qc[0], qc[1], qc[2], qc[3] };
			const V3    ax[3]{ Rot(q, { 1, 0, 0 }), Rot(q, { 0, 1, 0 }), Rot(q, { 0, 0, 1 }) };
			M3          m{};
			for (int r = 0; r < 3; ++r) {
				for (int col = 0; col < 3; ++col) {
					float s = 0;
					for (int e = 0; e < 3; ++e) {
						const float* v = &ax[e].x;
						s += v[r] * d[e] * v[col];
					}
					m[r * 3 + col] = s;
				}
			}
			inv[k] = m;
		}
		const float     h = std::clamp(a_dt, 0.0f, 0.05f) * gain_;
		std::vector<V3> dw(n), dv(n);
		for (int p = 1; p < 21; ++p) {
			const int c = a.sky.partBody[p], pb = a.sky.partBody[kParent[p]];
			const float* ac = a.acc.data() + p * 3;
			if (c < 0 || pb < 0 || c == pb || !std::isfinite(ac[0] + ac[1] + ac[2])) {
				continue;
			}
			V3 want = Scl({ ac[0], ac[1], ac[2] }, h);
			if (damp_ > 0.0f) {
				// tensed muscles: the joint's spin (child vs parent) is cut by damp_ this step. Skyrim's joints have almost
				// no damping of their own, so limbs bounce on landing
				const auto* wc = reinterpret_cast<const float*>(&a.sky.bodies[c].body->motion.angularVelocity);
				const auto* wp = reinterpret_cast<const float*>(&a.sky.bodies[pb].body->motion.angularVelocity);
				want = Sub(want, Scl({ wc[0] - wp[0], wc[1] - wp[1], wc[2] - wp[2] }, damp_));
			}
			const float lw = len(want);
			if (lw > a.maxWant) {
				a.maxWant = lw;
				a.wantPart = p;
				a.wantAge = a.age;
			}
			if (lw > 30.0f) {
				want = Scl(want, 30.0f / lw);
				++a.clamped;
			}
			// torque t with (inv_c + inv_p) t = want: the child turns by inv_c t, the parent back by inv_p t
			M3 s{};
			for (int e = 0; e < 9; ++e) {
				s[e] = inv[c][e] + inv[pb][e];
			}
			const float det = s[0] * (s[4] * s[8] - s[5] * s[7]) - s[1] * (s[3] * s[8] - s[5] * s[6]) + s[2] * (s[3] * s[7] - s[4] * s[6]);
			if (!(std::abs(det) > 1e-12f)) {
				continue;
			}
			const M3 si{ (s[4] * s[8] - s[5] * s[7]) / det, (s[2] * s[7] - s[1] * s[8]) / det, (s[1] * s[5] - s[2] * s[4]) / det,
				(s[5] * s[6] - s[3] * s[8]) / det, (s[0] * s[8] - s[2] * s[6]) / det, (s[2] * s[3] - s[0] * s[5]) / det,
				(s[3] * s[7] - s[4] * s[6]) / det, (s[1] * s[6] - s[0] * s[7]) / det, (s[0] * s[4] - s[1] * s[3]) / det };
			const V3 t = mul(si, want);
			const V3 wc = mul(inv[c], t), wp = Scl(mul(inv[pb], t), -1.0f);
			dw[c] = Add(dw[c], wc);
			dw[pb] = Add(dw[pb], wp);
			if (aboutJoint_ && a.sky.bodies[c].havePivot) {
				auto cross = [](const V3& x, const V3& y) { return V3{ x.y * y.z - x.z * y.y, x.z * y.x - x.x * y.z, x.x * y.y - x.y * y.x }; };
				const auto& mc = a.sky.bodies[c].body->motion.motionState;
				const auto* oc = reinterpret_cast<const float*>(&mc.transform.translation);
				const auto* qc = reinterpret_cast<const float*>(&mc.sweptTransform.rotation1);
				const V3    piv = Add({ oc[0], oc[1], oc[2] }, Rot(Q{ qc[0], qc[1], qc[2], qc[3] }, a.sky.bodies[c].pivot));
				const auto* cc = reinterpret_cast<const float*>(&mc.sweptTransform.centerOfMass1);
				const auto* cp = reinterpret_cast<const float*>(&a.sky.bodies[pb].body->motion.motionState.sweptTransform.centerOfMass1);
				dv[c] = Add(dv[c], cross(wc, Sub({ cc[0], cc[1], cc[2] }, piv)));
				dv[pb] = Add(dv[pb], cross(wp, Sub({ cp[0], cp[1], cp[2] }, piv)));
			}
		}
		for (int p = 0; p < 21; ++p) {
			const int    k = a.sky.partBody[p];
			const float* e = a.ext.data() + p * 3;
			if (k >= 0 && std::isfinite(e[0] + e[1] + e[2])) {
				V3          turn = Scl(mul(inv[k], { e[0], e[1], e[2] }), h);
				const float tl = len(turn);
				a.extMax = std::max(a.extMax, tl);
				if (tl > kMaxExtTurn) {
					turn = Scl(turn, kMaxExtTurn / tl);
				}
				dw[k] = Add(dw[k], turn);
			}
		}
		for (int p = 0; p < 21; ++p) {
			const int    k = a.sky.partBody[p];
			const float* d = a.drv.data() + p * 6;
			if (k >= 0 && std::isfinite(d[0] + d[1] + d[2])) {
				const float im = reinterpret_cast<const float*>(&a.sky.bodies[k].body->motion.inertiaAndMassInv)[3];
				dv[k] = Add(dv[k], Scl({ d[0], d[1], d[2] }, im * h));
			}
		}
		// blows on her while down: the push on the body it hit, and the spin about its centre from where it hit
		std::vector<V3> kv(n);
		for (const auto& kk : a.kicks) {
			if (kk.part < 0) {
				for (std::size_t k = 0; k < n; ++k) {
					kv[k] = Add(kv[k], kk.j);
				}
				continue;
			}
			const int bi = kk.part < 21 ? a.sky.partBody[kk.part] : -1;  // mapped by now (a body taken back is mapped after the blow)
			if (bi < 0 || static_cast<std::size_t>(bi) >= n) {
				continue;
			}
			const auto  k = static_cast<std::size_t>(bi);
			const float im = reinterpret_cast<const float*>(&a.sky.bodies[k].body->motion.inertiaAndMassInv)[3];
			kv[k] = Add(kv[k], Scl(kk.j, im));
			const V3 tq{ kk.arm.y * kk.j.z - kk.arm.z * kk.j.y, kk.arm.z * kk.j.x - kk.arm.x * kk.j.z, kk.arm.x * kk.j.y - kk.arm.y * kk.j.x };
			dw[k] = Add(dw[k], mul(inv[k], tq));
		}
		a.kicks.clear();
		const float cap = a.speedCap;  // once: her death's spikes
		a.speedCap = -1.0f;
		for (std::size_t k = 0; k < n; ++k) {
			auto& b = a.sky.bodies[k];
			auto& mo = b.body->motion;
			V3    v = dv[k];
			if (const float l = len(v); l > 3.0f) {
				v = Scl(v, 3.0f / l);
			}
			v = Add(v, kv[k]);
			a.maxDw = std::max(a.maxDw, len(dw[k]));
			if (k == 0) {
				// trace: how much the limbs visibly move (mean spin of every body against the pelvis body), as fit-test layers measures it
				const int pk = a.sky.partBody[0];
				if (pk >= 0) {
					const auto* w0 = reinterpret_cast<const float*>(&a.sky.bodies[pk].body->motion.angularVelocity);
					float       sum = 0.0f;
					int         cnt = 0;
					for (std::size_t j = 0; j < n; ++j) {
						if (static_cast<int>(j) == pk) {
							continue;
						}
						const auto* wj = reinterpret_cast<const float*>(&a.sky.bodies[j].body->motion.angularVelocity);
						sum += len({ wj[0] - w0[0], wj[1] - w0[1], wj[2] - w0[2] });
						++cnt;
					}
					if (cnt) {
						a.actSum += sum / cnt * 57.3f;
						++a.actN;
					}
					// trace: how far each limb actually swings (a jitter spins fast and goes nowhere)
					const auto* q0c = reinterpret_cast<const float*>(&a.sky.bodies[pk].body->motion.motionState.sweptTransform.rotation1);
					const Q     q0i = Conj(Q{ q0c[0], q0c[1], q0c[2], q0c[3] });
					const bool  fresh = a.actRef.size() != n;
					if (fresh) {
						a.actRef.assign(n, Q{});
						a.actSwing.assign(n, 0.0f);
					}
					for (std::size_t j = 0; j < n; ++j) {
						const auto* qc = reinterpret_cast<const float*>(&a.sky.bodies[j].body->motion.motionState.sweptTransform.rotation1);
						const Q     r = Mul(q0i, Q{ qc[0], qc[1], qc[2], qc[3] });
						if (fresh) {
							a.actRef[j] = r;
							continue;
						}
						a.sleepMax = std::max<int>(a.sleepMax, a.sky.bodies[j].body->motion.deactivationNumInactiveFrames[0]);
						const Q&    f = a.actRef[j];
						const float d = std::min(1.0f, std::abs(f.x * r.x + f.y * r.y + f.z * r.z + f.w * r.w));
						a.actSwing[j] = std::max(a.actSwing[j], 2.0f * std::acos(d) * 57.3f);
					}
				}
			}
			auto*    lv = reinterpret_cast<float*>(&mo.linearVelocity);
			auto*    av = reinterpret_cast<float*>(&mo.angularVelocity);
			V3 nl{ lv[0] + v.x, lv[1] + v.y, lv[2] + v.z };
			if (cap > 0.0f) {
				if (const float s = len(nl); s > cap) {
					nl = Scl(nl, cap / s);
				}
			}
			V3       na{ av[0] + dw[k].x, av[1] + dw[k].y, av[2] + dw[k].z };
			// Delirium's own cap on a body's spin (3 turns/s, MAX_SPIN_NM): its muscles never spin a body past it. Spin
			// Skyrim's contacts gave the body is kept
			const float was = len({ av[0], av[1], av[2] }), now = len(na);
			if (now > kMaxSpin && now > was) {
				na = Scl(na, std::max(was, kMaxSpin) / now);
				++a.spinCapped;
			}
			if (!std::isfinite(nl.x + nl.y + nl.z + na.x + na.y + na.z)) {
				continue;
			}
			lv[0] = nl.x, lv[1] = nl.y, lv[2] = nl.z, lv[3] = 0;
			av[0] = na.x, av[1] = na.y, av[2] = na.z, av[3] = 0;
			b.lin = nl;
			b.ang = na;
			b.hasTarget = true;  // re-applied after Skyrim's own driver, as the steering is
			a.maxSpeed = std::max(a.maxSpeed, len(nl));
		}
	}

	void Engine::BodyTarget(const Active& a, const BodyLink& l, V3& a_pos, Q& a_rot) const
	{
		V3 bp;
		Q  br;
		DeliriumBone(a, l, bp, br);
		// body = bone * (mapR, mapT): checked in game against Skyrim's own pose from the bodies, right within 0.5 deg
		a_rot = Mul(br, l.mapR);
		a_pos = Add(bp, Rot(br, Scl(l.mapT, a.scale)));
	}

	void Engine::DriveRagdoll(RE::Actor* a_actor, RE::hkbRagdollDriver* a_driver, float a_dt, bool a_after)
	{
		std::scoped_lock l(lock_);
		for (auto& p : active_) {
			auto& a = *p;
			if (a.actor.get().get() != a_actor) {
				continue;
			}
			a.sky.driver = a_driver;
			if (!a_after) {
				++a.physSteps;
			}
			if (!a.sky.linked || !a_driver->ragdoll) {
				return;
			}
			if (!a_after && TorqueMode(a)) {
				if (a.following) {
					ApplyTorques(a, a_dt);
					++a.drives;
				} else {
					// Delirium starts following on the next frame: until then Skyrim's ragdoll runs on its own
					for (auto& b : a.sky.bodies) {
						b.hasTarget = false;
					}
				}
				return;
			}
			if (a_after) {
				const auto [over, overBody] = a.sky.Reapply();
				if (!TorqueMode(a) && a.traceN < 100) {
					logger::info("    kf {:.3f}: Skyrim's driver changed a body's velocity by up to {:.2f} m/s ({})", a.age, over, overBody);
				}
				return;
			}
			// the targets come straight from Delirium's body through Skyrim's own body<->bone mapping: no bone, no root involved
			for (auto& b : a.sky.bodies) {
				if (b.mapped) {
					BodyTarget(a, b, b.target, b.targetRot);
					b.hasTarget = true;
				}
			}
			for (auto& b : a.sky.bodies) {
				if (b.follow >= 0) {
					BodyLink bone;
					bone.mapped = true;
					bone.posPart = bone.rotPart = b.follow;
					V3 bp;
					Q  br;
					DeliriumBone(a, bone, bp, br);
					// this body's own bone, then the body on it (BodyTarget's formula)
					const Q nr = Mul(br, b.followR);
					const V3 np = Add(bp, Rot(br, b.followT));
					b.targetRot = Mul(nr, b.mapR);
					b.target = Add(np, Rot(nr, Scl(b.mapT, a.scale)));
					b.hasTarget = true;
				}
			}
			// F7's first 0.15 s (the push is Delirium's): the velocities that carry each body onto Delirium's pose in one step
			if (a.traceN < 100) {
				const auto [miss, missBody] = a.sky.Missed();
				logger::info("    kf {:.3f}: dt {:.4f}, bodies ended up to {:.3f} m from where the last step sent them ({})", a.age, a_dt, miss, missBody);
			}
			a.maxSpeed = std::max(a.maxSpeed, a.sky.DriveToTargets(a_dt, a.age));
			++a.drives;
			return;
		}
	}

	void Engine::FollowActor(Active& a, RE::Actor* a_actor)
	{
		// upright: the actor (capsule, crosshair, AI) moves under the pelvis; the bones are placed relative to it anyway
		float p[3]{}, q[4]{};
		dl.nm_agent_get_part_transform(a.agent, 0, p, q);
		RE::NiPoint3 want = a.origin + RE::NiPoint3(p[0], p[1], 0) / kUnit;
		const auto   cur = a_actor->GetPosition();
		want.z = cur.z;
		// every frame, however small: a threshold moves her every other frame in 7-unit snaps, and bone physics
		// (CBPC, SMP) that works relative to the actor sees that as a shake
		if ((want - cur).Length() < 0.01f) {
			return;
		}
		g_tasks->AddTask([h = a.actor, want]() {
			if (auto act = h.get(); act && !act->IsInRagdollState()) {
				act->SetPosition(want, true);
			}
		});
	}

	bool Engine::SmoothRoot(Active& a, RE::Actor* a_actor)
	{
		// Going ragdoll, Skyrim puts the skeleton's top node onto the tumbling body: it turns 30 deg a frame and jumps
		// from her feet to her hips (60+ units) in one frame. Our bones hide it (they are posed in world space), but the
		// meshes hang on the top node and cloth physics watches its turn: either can show the jump for a frame. Move the
		// top node at most kRootRate / kRootSpeed; the bones still land where Delirium wants them, so the pose is unchanged.
		constexpr float kRootRate = 8.0f;     // rad/s, under FSMP's default limit (10)
		constexpr float kRootSpeed = 600.0f;  // game units/s beyond which the top node trails Skyrim's
		auto* top = a_actor->Get3D(false);
		if (!top || !a.haveDrawnRoot) {
			return false;
		}
		RE::NiUpdateData ud{};
		top->UpdateWorldData(&ud);  // Skyrim may just have set its local
		const float dt = std::max(a.age - a.drawnAge, 0.0f);
		const Q     sky = FromMat(top->world.rotate);
		const float gap = Angle(sky, a.drawnRootQ);
		const float step = kRootRate * dt;
		const RE::NiPoint3 skyT = top->world.translate;
		const float        dist = (skyT - a.drawnRootT).Length();
		const float        reach = kRootSpeed * dt;
		if (gap < 1e-4f && dist < 0.01f) {
			return false;  // still where we left it: nothing new from Skyrim (the lag stays what it was)
		}
		auto remember = [&]() {
			a.drawnRootQ = FromMat(top->world.rotate);
			a.drawnRootT = top->world.translate;
			a.drawnAge = a.age;
		};
		if (gap <= step && dist <= reach) {
			a.rootLag = 0.0f;
			remember();
			return false;
		}
		const RE::NiMatrix3 want = gap <= step ? top->world.rotate : ToMat(Slerp(a.drawnRootQ, sky, step / gap));
		const RE::NiPoint3  wantT = dist <= reach ? skyT : a.drawnRootT + (skyT - a.drawnRootT) * (reach / dist);
		if (top->parent) {
			const auto& pw = top->parent->world;
			top->local.rotate = pw.rotate.Transpose() * want;
			top->local.translate = (pw.rotate.Transpose() * (wantT - pw.translate)) / pw.scale;
		} else {
			top->local.rotate = want;
			top->local.translate = wantT;
		}
		a.rootLag = std::max(gap - step, 0.0f) + std::max(dist - reach, 0.0f) * 0.01f;  // ~1 deg per 1.75 units
		a.maxRootLag = std::max(a.maxRootLag, a.rootLag);
		top->UpdateWorldData(&ud);  // the top node only: the caller refreshes what hangs below it
		remember();
		return true;
	}

	void Engine::WriteBones(Active& a)
	{
		// the caller has brought the skeleton's world transforms (NPC Root down) up to date
		RE::NiUpdateData ud{};
		auto*            top = a.com->parent;

		// the whole body follows the pelvis: move COM (the pelvis and the spine both hang off it) by the pelvis's error
		{
			float p[3]{}, q[4]{};
			dl.nm_agent_get_part_transform(a.agent, 0, p, q);
			if (!std::isfinite(p[0] + p[1] + p[2] + q[0] + q[1] + q[2] + q[3])) {
				if (a.nanLog < 30) {
					++a.nanLog;
					logger::error("    NaN pelvis age {:.3f}: Delirium's pelvis ({} {} {}) rot ({} {} {} {}); nothing written", a.age, p[0], p[1], p[2], q[0], q[1], q[2], q[3]);
				}
				return;
			}
			const V3           j = Add({ p[0], p[1], p[2] }, Rot({ q[0], q[1], q[2], q[3] }, a.anchor[0]));
			const RE::NiPoint3 want = a.origin + RE::NiPoint3(j.x, j.y, j.z) / kUnit;
			if (a.knockQueued) {
				// ragdolled, Skyrim turns the skeleton root back and forth every frame: COM takes its rotation from our
				// pelvis instead, so nothing below it (spine, arms, fingers, hair, armour) depends on the root's heading
				const RE::NiMatrix3 pelvis = ToMat(Mul({ q[0], q[1], q[2], q[3] }, rig_[0].offset));
				a.com->local.rotate = top->world.rotate.Transpose() * (pelvis * a.comInPelvis);
				UpdateTransforms(a.com);
			}
			const RE::NiPoint3 d = (want - a.ref[0]->world.translate) * a.blend;
			a.com->local.translate += (top->world.rotate.Transpose() * d) / top->world.scale;
			UpdateTransforms(a.com);
		}

		std::array<bool, 21> done{};
		for (std::size_t i = 0; i < 21; ++i) {
			if (!rig_[i].write) {
				continue;
			}
			auto* node = a.ref[i];
			if (!node->parent) {
				continue;
			}
			// refresh the in-between nodes (CME helpers) from the nearest bone already written down to this one's parent
			std::vector<RE::NiAVObject*> chain;
			for (auto* n = node->parent; n && n != a.com; n = n->parent) {
				bool written = false;
				for (std::size_t k = 0; k < 21; ++k) {
					written |= done[k] && a.ref[k] == n;
				}
				if (written) {
					break;
				}
				chain.push_back(n);
			}
			for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
				(*it)->UpdateWorldData(&ud);
			}
			float p[3]{}, q[4]{};
			dl.nm_agent_get_part_transform(a.agent, static_cast<std::int32_t>(i), p, q);
			if (!std::isfinite(q[0] + q[1] + q[2] + q[3]) || !std::isfinite(node->parent->world.rotate.entry[0][0])) {
				if (a.nanLog < 30) {
					++a.nanLog;
					logger::error("    NaN bone age {:.3f} {}: Delirium part {} rot ({} {} {} {}), parent rot {}", a.age, node->name.c_str(), i, q[0], q[1], q[2], q[3],
						node->parent->world.rotate.entry[0][0]);
				}
				continue;
			}
			const RE::NiMatrix3 want = ToMat(Mul({ q[0], q[1], q[2], q[3] }, rig_[i].offset));
			const RE::NiMatrix3 ours = node->parent->world.rotate.Transpose() * want;
			const float w = a.blend * a.limbBlend;
			node->local.rotate = w >= 1.0f ? ours : ToMat(Slerp(FromMat(node->local.rotate), FromMat(ours), std::max(w, 0.0f)));
			node->UpdateWorldData(&ud);
			done[i] = true;
		}
		// everything hanging off the driven bones (twist bones, weapons, ...) follows
		UpdateTransforms(top);
	}
}

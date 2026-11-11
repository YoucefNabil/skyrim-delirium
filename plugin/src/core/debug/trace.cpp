// Dev tools: recorded poses, the per-frame trace, Skyrim's ragdoll joints for fit-test.
#include "shared.h"

namespace delirium
{
	// [Test] bRecordPoses: the pose she holds each frame while standing (her live animation: stance, staggers, hit
	// reactions), one line per frame (age, then each part's position and rotation xyzw relative to where she was taken),
	// first 4 NPCs a session, 10 s each. fit-test sweep STANCE_TRACK= replays it as the zero pose
	void Engine::RecordPose(Active& a, RE::Actor* a_actor, const std::array<V3, 21>& pos, const std::array<Q, 21>& rot)
	{
		if (!recordPoses_ || a.trackN >= 600) {
			return;
		}
		if (a.trackFile.empty()) {
			if (tracks_ >= 4) {
				return;
			}
			++tracks_;  // numbered: a summoned NPC's FormID is reused by the next one
			a.trackFile = (*SKSE::log::log_directory() / fmt::format("SkyrimDelirium_track_{}_{:08X}.txt", tracks_, a_actor->GetFormID())).string();
			std::ofstream(a.trackFile) << "heading " << a_actor->data.angle.z << " scale " << a.scale << "\n";
			logger::info("{:08X}: recording her held poses to {}", a_actor->GetFormID(), a.trackFile);
		}
		std::ostringstream f;
		f << a.age;
		for (std::size_t i = 0; i < 21; ++i) {
			f << " " << pos[i].x << " " << pos[i].y << " " << pos[i].z << " " << rot[i].x << " " << rot[i].y << " " << rot[i].z << " " << rot[i].w;
		}
		std::ofstream(a.trackFile, std::ios::app) << f.str() << "\n";
		++a.trackN;
	}

	// The pose as it will be drawn (each bone's world from its locals up to the 3D root, nothing written), once a frame
	// after the root move, compared with the last frame: top node and pelvis moves, the worst limb move and turn relative
	// to the pelvis, and how many limbs turned back against their last frame's turn (shaking)
	void Engine::Trace(Active& a, RE::Actor* a_actor, const char*)
	{
		auto* root3d = a_actor->Get3D(false);
		if (!root3d || !a.knockQueued || a.traceN >= 100 || (a.ragdolled && a.age - a.ragdollAge > 1.0f)) {
			return;
		}
		std::function<RE::NiTransform(RE::NiAVObject*)> world = [&](RE::NiAVObject* n) -> RE::NiTransform {
			if (n == root3d || !n->parent) {
				return n->parent ? n->parent->world * n->local : n->local;
			}
			return world(n->parent) * n->local;
		};
		const RE::NiTransform           top = world(root3d);
		std::array<RE::NiTransform, 21> bone{};
		std::array<RE::NiMatrix3, 21>   rel{};
		for (std::size_t i = 0; i < 21; ++i) {
			if (a.ref[i]) {
				bone[i] = world(a.ref[i]);
			}
		}
		for (std::size_t i = 0; i < 21; ++i) {
			rel[i] = bone[0].rotate.Transpose() * bone[i].rotate;
		}
		{
			// trace: every drawn bone's world position per frame (m), for plotting
			std::ofstream f(*SKSE::log::log_directory() / "SkyrimDelirium_trace.csv", a.haveTrace ? std::ios::app : std::ios::trunc);
			if (!a.haveTrace) {
				f << "age,torque,steps";
				for (std::size_t i = 0; i < 21; ++i) {
					f << ',' << rig_[i].name << "_x," << rig_[i].name << "_y," << rig_[i].name << "_z";
				}
				f << ",COM_x,COM_y,COM_z\n";
			}
			f << fmt::format("{:.4f},{},{}", a.age, TorqueMode(a) ? 1 : 0, a.physSteps - a.trSteps);
			for (std::size_t i = 0; i < 21; ++i) {
				const auto& p = bone[i].translate;
				f << fmt::format(",{:.4f},{:.4f},{:.4f}", p.x * kUnit, p.y * kUnit, p.z * kUnit);
			}
			const auto c = world(a.com).translate;
			f << fmt::format(",{:.4f},{:.4f},{:.4f}\n", c.x * kUnit, c.y * kUnit, c.z * kUnit);
		}
		auto angle = [](const RE::NiMatrix3& x, const RE::NiMatrix3& y) {
			const auto  d = x.Transpose() * y;
			const float tr = d.entry[0][0] + d.entry[1][1] + d.entry[2][2];
			return std::acos(std::clamp((tr - 1.0f) * 0.5f, -1.0f, 1.0f)) * 57.2958f;
		};
		std::array<RE::NiPoint3, 21> spin{};
		if (a.haveTrace) {
			const RE::NiPoint3 pelvisMove = bone[0].translate - a.trBone[0].translate;
			float              worst = 0, worstRot = 0;
			std::size_t        wi = 0, wr = 0;
			int                back = 0;
			std::string        backs;
			for (std::size_t i = 1; i < 21; ++i) {
				if (!a.ref[i]) {
					continue;
				}
				const float d = (bone[i].translate - a.trBone[i].translate - pelvisMove).Length();
				if (d > worst) {
					worst = d, wi = i;
				}
				const auto  m = a.trRel[i].Transpose() * rel[i];
				const float r = angle(a.trRel[i], rel[i]);
				if (r > worstRot) {
					worstRot = r, wr = i;
				}
				// turn axis in the pelvis frame (rel_prev * m's axis), so frames compare
				const RE::NiPoint3 ax = a.trRel[i] * RE::NiPoint3(m.entry[2][1] - m.entry[1][2], m.entry[0][2] - m.entry[2][0], m.entry[1][0] - m.entry[0][1]);
				spin[i] = ax;
				const auto& pv = a.trSpin[i];
				if (r > 4.0f && pv.Length() > 0.07f && ax.Dot(pv) < 0.0f) {
					++back;
					backs += ' ';
					backs += rig_[i].name;
				}
			}
			const char* phase = !a.ragdolled ? "ours" : TorqueMode(a) ? (a.following ? "torque" : "torque-wait") : "lead";
			float       comSpeed = -1.0f;
			for (auto& b : a.sky.bodies) {
				if (b.node == a.com) {
					const auto* lv = reinterpret_cast<const float*>(&b.body->motion.linearVelocity);
					comSpeed = std::sqrt(lv[0] * lv[0] + lv[1] * lv[1] + lv[2] * lv[2]);
				}
			}
			const bool pelvisBack = pelvisMove.Dot(a.trPelvisMove) < 0.0f;
			float      off = 0;
			if (a.haveWritten && !TorqueMode(a)) {
				for (std::size_t i = 0; i < 21; ++i) {
					if (a.ref[i]) {
						off = std::max(off, (bone[i].translate - a.written[i].translate).Length() * kUnit);
					}
				}
			}
			logger::info("    trace {:.3f} {:<11} drawn off ours {:.3f} m, {} steps, COM body {:.1f} m/s; top {:.3f} m {:.1f} deg (lag {:.2f}), pelvis {:.3f} m{}, limb {:.3f} m ({}), turn {:.1f} deg ({}), {} turned back{}",
				a.age, phase, off, a.physSteps - a.trSteps, comSpeed, (top.translate - a.trTop.translate).Length() * kUnit, angle(a.trTop.rotate, top.rotate),
				a.rootLag, pelvisMove.Length() * kUnit, pelvisBack ? " BACK" : "", worst * kUnit, rig_[wi].name, worstRot, rig_[wr].name, back, backs);
			a.trPelvisMove = pelvisMove;
		}
		a.trSteps = a.physSteps;
		a.trTop = top;
		a.trBone = bone;
		a.trRel = rel;
		a.trSpin = spin;
		a.haveTrace = true;
		++a.traceN;
	}

	// Skyrim's ragdoll joints for the offline test body (fit-test): each constraint's Havok frames (in its bodies' frames),
	// each body's rotation in its Delirium part (rig offset * Skyrim's mapping) and the limits. One file per new set of limits.
	void Engine::DumpRagdoll(const Active& a, RE::Actor* a_actor)
	{
		if (!a.sky.driver || !a.sky.driver->ragdoll || ragdollDumps_.size() >= 12) {
			return;
		}
		const auto body = [&](RE::hkpEntity* e) -> const BodyLink* {
			for (const auto& b : a.sky.bodies) {
				if (b.body && static_cast<RE::hkpEntity*>(b.body) == e) {
					return &b;
				}
			}
			return nullptr;
		};
		const auto q = [](const Q& r) { return fmt::format("[{:.6f},{:.6f},{:.6f},{:.6f}]", r.x, r.y, r.z, r.w); };
		const auto cols = [](const RE::hkTransform& t) {
			const auto* f = reinterpret_cast<const float*>(&t);
			return fmt::format("[[{:.6f},{:.6f},{:.6f}],[{:.6f},{:.6f},{:.6f}],[{:.6f},{:.6f},{:.6f}]]", f[0], f[1], f[2], f[4], f[5], f[6], f[8], f[9], f[10]);
		};
		const auto side = [&](const BodyLink* b) {
			if (!b) {
				return std::string("null");
			}
			const bool ok = b->mapped && b->rotPart >= 0;
			return fmt::format("{{\"bone\":\"{}\",\"part\":{},\"part_ref\":\"{}\",\"inv_inertia_mass\":[{:.6g},{:.6g},{:.6g},{:.6g}],\"body_in_part\":{}}}", b->node ? b->node->name.c_str() : "?", ok ? b->rotPart : -1,
				ok ? rig_[static_cast<std::size_t>(b->rotPart)].ref : std::string("?"), b->massInv[0], b->massInv[1], b->massInv[2], b->massInv[3],
				ok ? q(Mul(rig_[static_cast<std::size_t>(b->rotPart)].offset, b->mapR)) : "null");
		};
		std::string js, sig;
		for (auto* ci : a.sky.driver->ragdoll->constraints) {
			auto* d = ci ? ci->data : nullptr;
			if (!d || (d->GetType() != 7 && d->GetType() != 2)) {
				continue;
			}
			std::string lim, fa, fb;
			if (d->GetType() == 7) {
				auto& at = static_cast<RE::hkpRagdollConstraintData*>(d)->atoms;
				lim = fmt::format("\"type\":\"ragdoll\",\"twist\":[{:.5f},{:.5f}],\"cone\":{:.5f},\"planes\":[{:.5f},{:.5f}]", at.twistLimit.minAngle,
					at.twistLimit.maxAngle, at.coneLimit.maxAngle, at.planesLimit.minAngle, at.planesLimit.maxAngle);
				fa = cols(at.transforms.transformA);
				fb = cols(at.transforms.transformB);
			} else {
				auto& at = static_cast<RE::hkpLimitedHingeConstraintData*>(d)->atoms;
				lim = fmt::format("\"type\":\"hinge\",\"angle\":[{:.5f},{:.5f}]", at.angLimit.minAngle, at.angLimit.maxAngle);
				fa = cols(at.transforms.transformA);
				fb = cols(at.transforms.transformB);
			}
			sig += lim;
			js += fmt::format("{}\n  {{{},\"child\":{},\"parent\":{},\"frame_child\":{},\"frame_parent\":{}}}", js.empty() ? "" : ",", lim,
				side(body(ci->entities[0])), side(body(ci->entities[1])), fa, fb);
		}
		if (!ragdollDumps_.insert(sig).second) {
			return;
		}
		const auto* base = a_actor->GetActorBase();
		const auto  name = fmt::format("SkyrimDelirium_ragdoll_{:08X}.json", a_actor->GetFormID());
		std::ofstream(*SKSE::log::log_directory() / name) << fmt::format(
			"{{\"format\":\"skyrim-ragdoll-1\",\"actor\":\"{:08X}\",\"female\":{},\"race\":\"{:08X}\",\"scale\":{:.4f},\n \"note\":\"frame columns: ragdoll twist/planes/cross, hinge axle/perp/perp2; body_in_part: the body's rotation in the Delirium part (xyzw)\",\n \"joints\":[{}\n]}}\n",
			a_actor->GetFormID(), base && base->IsFemale(), a_actor->GetRace() ? a_actor->GetRace()->GetFormID() : 0u, a.scale, js);
		logger::info("{:08X}: wrote {} (Skyrim's ragdoll joints for the test body)", a_actor->GetFormID(), name);
	}

}

// Skyrim's ragdoll bodies carried onto a pose (body/skyrim_body.h).
#include "shared.h"

namespace delirium
{
	SkyrimBody::Far SkyrimBody::Missed() const
	{
		Far most;
		for (const auto& b : bodies) {
			if (!b.haveAim) {
				continue;
			}
			const auto* cw = reinterpret_cast<const float*>(&b.body->motion.motionState.sweptTransform.centerOfMass1);
			const V3    d = Sub({ cw[0], cw[1], cw[2] }, b.aimCom);
			const float l = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
			if (l > most.by) {
				most.by = l;
				most.body = b.node ? b.node->name.c_str() : "?";
			}
		}
		return most;
	}

	float SkyrimBody::DriveToTargets(float a_dt, float a_age)
	{
		const float inv = 1.0f / std::max(a_dt, 1.0f / 240.0f);
		float       fastest = 0.0f;
		for (auto& b : bodies) {
			if (!b.hasTarget) {
				continue;
			}
			auto&       ms = b.body->motion.motionState;
			const Q     qt = b.targetRot;
			const V3    pt = b.target;
			const auto* cl = reinterpret_cast<const float*>(&ms.sweptTransform.centerOfMassLocal);
			const auto* cw = reinterpret_cast<const float*>(&ms.sweptTransform.centerOfMass1);
			const auto* qc = reinterpret_cast<const float*>(&ms.sweptTransform.rotation1);
			const V3    com = Add(pt, Rot(qt, { cl[0], cl[1], cl[2] }));
			const V3    lin = Scl(Sub(com, { cw[0], cw[1], cw[2] }), inv);
			b.aimCom = com;
			b.haveAim = true;
			Q           d = Mul(qt, Conj({ qc[0], qc[1], qc[2], qc[3] }));
			if (d.w < 0) {
				d = { -d.x, -d.y, -d.z, -d.w };
			}
			const float s = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
			const float ang = 2.0f * std::atan2(s, d.w);
			const V3    w = s > 1e-6f ? Scl({ d.x, d.y, d.z }, ang / s * inv) : V3{};
			// capped: one bad frame must never launch the body (it then catches up over a few frames)
			auto cap = [](V3 v, float m) {
				const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
				return l > m ? Scl(v, m / l) : v;
			};
			// the cap only guards against garbage
			V3 linC = cap(lin, 200.0f), wC = cap(w, 400.0f);
			auto finite = [](const V3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
			if (!finite(linC) || !finite(wC) || !finite(pt) || !std::isfinite(qt.w)) {
				// never hand a NaN to havok: the whole ragdoll (and the cloth, hair and gear hanging on it) breaks
				if (nanLog < 30) {
					++nanLog;
					logger::error("    NaN drive age {:.3f} {}: target ({} {} {}) rot w {}, body now ({} {} {}), steer ({} {} {}) turn ({} {} {})", a_age,
						b.node ? b.node->name.c_str() : "?", pt.x, pt.y, pt.z, qt.w, cw[0], cw[1], cw[2], lin.x, lin.y, lin.z, w.x, w.y, w.z);
				}
				linC = {};
				wC = {};
			}
			fastest = std::max(fastest, std::sqrt(lin.x * lin.x + lin.y * lin.y + lin.z * lin.z));
			auto*       lv = reinterpret_cast<float*>(&b.body->motion.linearVelocity);
			auto*       av = reinterpret_cast<float*>(&b.body->motion.angularVelocity);
			lv[0] = linC.x, lv[1] = linC.y, lv[2] = linC.z, lv[3] = 0;
			av[0] = wC.x, av[1] = wC.y, av[2] = wC.z, av[3] = 0;
			b.lin = linC;
			b.ang = wC;
		}
		return fastest;
	}

	SkyrimBody::Far SkyrimBody::Reapply()
	{
		Far most;
		for (auto& b : bodies) {
			if (!b.hasTarget) {
				continue;
			}
			auto* lv = reinterpret_cast<float*>(&b.body->motion.linearVelocity);
			auto* av = reinterpret_cast<float*>(&b.body->motion.angularVelocity);
			const float o = std::sqrt((lv[0] - b.lin.x) * (lv[0] - b.lin.x) + (lv[1] - b.lin.y) * (lv[1] - b.lin.y) + (lv[2] - b.lin.z) * (lv[2] - b.lin.z));
			if (o > most.by) {
				most.by = o;
				most.body = b.node ? b.node->name.c_str() : "?";
			}
			lv[0] = b.lin.x, lv[1] = b.lin.y, lv[2] = b.lin.z, lv[3] = 0;
			av[0] = b.ang.x, av[1] = b.ang.y, av[2] = b.ang.z, av[3] = 0;
		}
		return most;
	}
}

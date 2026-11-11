// Skyrim's own ragdoll of one actor as Delirium uses it: which of its bodies sits on which Delirium part, and the
// pose drive that carries the bodies onto a pose in one physics step. Havok units throughout (game units * kUnit).
#pragma once

#include "vec.h"

namespace delirium
{
	// one of Skyrim's ragdoll bodies and the bone it hangs on (the body sits on the bone)
	struct BodyLink
	{
		RE::hkpRigidBody* body{};
		RE::NiAVObject*   node{};
		V3                target;  // where we posed the bone (havok units), saved before Skyrim syncs bones to bodies
		Q                 targetRot;
		bool              hasTarget{};
		std::array<float, 4> massInv{};  // the body's own inertia/mass inverse
		bool              havePivot{};     // its joint to its parent body: the pivot in the body's own frame (havok units)
		V3                pivot;
		V3                lin, ang;  // the velocities we set this step (re-applied after Skyrim's driver)
		V3                aimCom;    // trace: where the last keyframe sent the body's centre of mass
		bool              haveAim{};
		// Skyrim's own ragdoll<->animation mapping for this body: body = animation bone * (mapR, mapT) (mapT unscaled)
		bool              mapped{};
		Q                 mapR;
		V3                mapT;
		// the way back, as Skyrim's pose copy draws it (its ragdoll-to-animation mapper): bone = body * (drawR, drawT)
		bool              haveDraw{};
		Q                 drawR;
		V3                drawT;
		int               posPart{ -1 }, rotPart{ -1 };  // the Delirium parts that give the bone's position / rotation
		// no Delirium part (NPC COM, the ragdoll's root): rides rigidly on the nearest Delirium bone (rig part index)
		int               follow{ -1 };
		Q                 followR;  // this body in that bone's frame, from calibration (havok units)
		V3                followT;
	};

	struct SkyrimBody
	{
		RE::hkbRagdollDriver*               driver{};
		std::vector<BodyLink>               bodies;      // filled by Engine::Calibrate
		bool                                linked{};    // bodies is filled: we know how each body sits on Delirium's parts
		std::array<int, 21>                 partBody{};  // the body (index in bodies) each part's drive goes to
		std::vector<RE::hkpConstraintData*> eased;       // joints whose friction we took off (held, put back on release)
		std::vector<RE::hkpRigidBody*>      weighed;     // bodies given Delirium's mass (held, Skyrim's put back on release)
		std::vector<std::array<float, 2>>   material;    // Skyrim's own friction and bounce of each weighed body (put back on release)
		int                                 nanLog{};

		struct Far
		{
			float       by{};  // m, or m/s
			std::string body;  // the body that was furthest
		};
		// trace: how far each body ended from where the last DriveToTargets sent it
		[[nodiscard]] Far Missed() const;
		// every body with a target (BodyLink::target / targetRot, hasTarget) gets the velocity and spin that carry it
		// there in one step of a_dt; they are kept for Reapply. Returns the fastest steer, m/s. a_age: for the log
		float DriveToTargets(float a_dt, float a_age);
		// Skyrim's own driver has run: our velocities go back on. Returns how far it had changed them
		Far Reapply();
		[[nodiscard]] bool InWorld() const;  // any of her ragdoll bodies is in a Havok world
	};
}

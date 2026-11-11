// Skyrim's own Havok world: which NPCs' ragdoll bodies are in it while they stand, and which body layers collide
// (body/world.cpp). Skyrim's side only: nothing here knows Delirium.
#pragma once

namespace delirium::world
{
	// her ragdoll bodies that are in a Havok world right now, Havok metres
	struct Sample
	{
		RE::NiPoint3              mean;       // of the bodies' centres of mass
		float                     fastest{};  // her fastest body, m/s
		std::vector<RE::NiPoint3> at, vel;    // each body's centre of mass and its velocity
	};
	[[nodiscard]] Sample Bodies(RE::Actor* a_actor);

	// one of those bodies as a capsule (its own shape if it is one, else its bounds down their longest side), Havok metres
	struct Capsule
	{
		RE::NiPoint3 a, b;  // the ends of its axis
		float        radius{};
		RE::NiPoint3 vel;  // m/s
	};
	[[nodiscard]] std::vector<Capsule> Capsules(RE::Actor* a_actor);
	// an actor on their feet whose ragdoll is in no world (the player): the upright volume they stand in, as one capsule
	[[nodiscard]] Capsule Standing(RE::Actor* a_actor);

	// main thread: her ragdoll bodies into her cell's Havok world while she stands (Skyrim keeps them on her
	// animation, keyframed) and report what touches them (Touches); false: nothing added
	bool Add(RE::Actor* a_actor, bool a_log = false);
	bool Remove(RE::Actor* a_actor);  // main thread: and out again
	[[nodiscard]] std::vector<RE::ActorHandle> Added();  // whose bodies we put in (some may have left since)

	// something that moves touched one of the bodies Add put in, as Havok's own contact reports it (body/contacts.cpp)
	struct Touch
	{
		RE::FormID    who{};    // whose body was touched
		RE::FormID    by{};     // the reference the other body belongs to; 0: none
		RE::COL_LAYER byLayer{ RE::COL_LAYER::kUnidentified };
		RE::NiPoint3  at;       // the contact point, Havok metres
		RE::NiPoint3  vel;      // the other body's velocity there, relative to hers, m/s
		float         closing{};  // of that, towards her along the contact normal
		float         coming{};   // the other body's own velocity towards her along the contact normal
	};
	[[nodiscard]] std::vector<Touch> Touches();  // any thread: the touches since the last call, the hardest of each pair

	[[nodiscard]] std::string Heard();           // trace: how many contacts Havok reported since the core loaded

	void Collide();                  // main thread: biped bodies meet each other (Leave takes off only the filter bits this set)
	[[nodiscard]] bool Changed();    // anything of ours is in place (the filter, an added ragdoll)
	void Leave();                    // main thread: the filter as Skyrim had it, the standing ragdolls we added out again

	void Log(RE::Actor* a_actor);    // her ragdoll bodies: in a world or not, motion type, layer
}

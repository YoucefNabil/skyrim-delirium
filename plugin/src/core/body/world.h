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
	// animation, keyframed); false: nothing added
	bool Add(RE::Actor* a_actor, bool a_log = false);
	bool Remove(RE::Actor* a_actor);  // main thread: and out again
	[[nodiscard]] std::vector<RE::ActorHandle> Added();  // whose bodies we put in (some may have left since)

	void Collide();                  // main thread: biped bodies meet each other (Skyrim's own rows are kept for Leave)
	[[nodiscard]] bool Changed();    // anything of ours is in place (the filter, an added ragdoll)
	void Leave();                    // main thread: the filter as Skyrim had it, the standing ragdolls we added out again

	void Log(RE::Actor* a_actor);    // her ragdoll bodies: in a world or not, motion type, layer
}

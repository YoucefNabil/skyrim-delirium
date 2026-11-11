// Havok's own contact events on the ragdoll bodies world.cpp puts in Skyrim's world (body/contacts.cpp). What they
// report comes out of world::Touches (body/world.h); this is the half world.cpp drives.
#pragma once

namespace delirium::world::contacts
{
	// her bodies report what touches them from now on; the caller holds their world's write lock (or they are in none).
	// Called again for her: the bodies she has now replace the ones listened on before
	void Listen(RE::FormID a_who, std::span<RE::hkpRigidBody* const> a_bodies);
	void Forget(RE::FormID a_who);  // and no longer; the same lock
	// main thread, no physics step running: nobody's bodies report any more. Must run before the core is unloaded
	// (the listeners live in it)
	void ForgetAll();
}

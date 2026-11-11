#pragma once

// What the core's files share: Delirium's API (dl), the per-part constants, the last hit of every actor and the
// counters every animation update reads. A helper used by one file stays in that file.
#include "engine.h"
#include "vec_math.h"

#include "body/collision.h"

#include <RE/H/hkpLimitedHingeConstraintData.h>
#include <RE/H/hkpRagdollConstraintData.h>

#include <SimpleIni.h>
#include <fstream>
#include <cstring>
#include <functional>
#include <random>
#include <sstream>
#include <unordered_map>

namespace delirium
{
	extern std::filesystem::path       g_pluginDir;
	extern const SKSE::TaskInterface* g_tasks;

	constexpr float kUnit = 0.0142875f;  // metres per game unit
	constexpr float kStep = 1.0f / 60.0f;

	// NM part order's parents (Buttocks is the root)
	constexpr std::array<int, 21> kParent{ -1, 0, 1, 2, 0, 4, 5, 0, 7, 8, 9, 10, 11, 10, 13, 14, 15, 10, 17, 18, 19 };
	constexpr int                 kChest = 10;
	constexpr int                 kHead = 12;
	constexpr float               kKnockStagger = 1.5f;  // a shout's stagger effect from which Skyrim knocks down (Fus Ro Dah)

	inline std::mt19937                  g_rng{ std::random_device{}() };
	inline float                         Rand(float a_lo, float a_hi) { return std::uniform_real_distribution<float>(a_lo, a_hi)(g_rng); }

	// ---- Delirium's C API, loaded at runtime from SKSE/Plugins/SkyrimDelirium/delirium_physics.dll
#define DL_FNS(X)                                                                                           \
	X(nm_last_error) X(nm_init) X(nm_world_create) X(nm_world_destroy) X(nm_world_add_floor) X(nm_build_load) \
	X(nm_agent_create) X(nm_agent_destroy) X(nm_agent_step) X(nm_agent_set_mode) X(nm_agent_get_part_transform) \
	X(nm_agent_set_part_state) X(nm_agent_get_kinetic_energy) X(nm_agent_apply_push) X(nm_agent_add_velocity) X(nm_msg_begin)           \
	X(nm_msg_set_bool) X(nm_msg_set_int) X(nm_msg_set_float) X(nm_msg_set_string) X(nm_msg_set_vec3) X(nm_msg_send) X(nm_agent_is_balancing) X(nm_agent_has_fallen) X(nm_agent_set_zero_pose) \
	X(nm_build_scale) X(nm_build_destroy) X(nm_world_add_mesh) X(nm_world_raycast) X(nm_world_triangle_count) X(nm_agent_get_com) X(nm_agent_task_shot) X(nm_agent_task_balance) X(nm_agent_task_flinch) \
	X(nm_agent_get_part_drives) X(nm_build_part_info) X(nm_agent_get_joint_accels) X(nm_agent_get_part_ext_torques) X(nm_agent_get_joint) \
	X(nm_init_character_file) X(nm_build_from_character) X(nm_set_task_param) X(nm_build_remap) X(nm_build_part_frame) X(nm_build_part_joint) X(nm_build_turn_limit) \
	X(nm_agent_set_others) X(nm_agent_get_capsules)
	struct Api
	{
#define DL_DECL(f) decltype(&::f) f = nullptr;
		DL_FNS(DL_DECL)
#undef DL_DECL
	};
	inline Api dl;

	inline std::string LastError()
	{
		char b[512]{};
		if (dl.nm_last_error) {
			dl.nm_last_error(b, sizeof(b));
		}
		return b;
	}

	// Skeleton nodes only, transforms only (world = parent world * local). Attached models (sheathed sword, shield: their
	// own nif root, a BSFadeNode) and meshes are left to Skyrim's own update: rewriting or UpdateWorldData-ing them here
	// made their textures flicker and doubled FSMP hair. Skinned meshes (body, armour, hair) draw from the bones anyway.
	inline void UpdateTransforms(RE::NiAVObject* a_obj, bool a_start = true)
	{
		auto* node = a_obj->AsNode();
		if (!node || (!a_start && a_obj->AsFadeNode())) {
			return;
		}
		if (a_obj->parent) {
			a_obj->world = a_obj->parent->world * a_obj->local;
		}
		for (auto& ch : node->GetChildren()) {
			if (ch) {
				UpdateTransforms(ch.get(), false);
			}
		}
	}

	void HookUpdate3D(RE::Actor* a_actor);  // below, with AfterRootMove
	void UnhookUpdate3D();
	void HookArrows();  // below, with OnArrow
	void UnhookArrows();
	void HookPresent();  // the render frame's end, for PublishShown
	void UnhookPresent();
	void PublishShown();  // body/show.cpp: every NPC's finished pose into what her skins read
	void FrameSync();     // body/show.cpp: after the physics mods' frame sync, the same with what they wrote over her bones
	inline std::atomic<bool> g_atPresent{ false };  // Present hooked: Show's pose waits for it (body/show.cpp)
	// a_spell: 0 arrows only, 1 spells only, -1 any
	bool RecentImpact(RE::TESObjectREFR* a_ref, float a_maxAge, RE::NiPoint3& a_at, RE::NiPoint3& a_vel, int a_spell = -1);
	inline std::atomic<int> g_count{ 0 };  // active agents, read without the lock by every actor's animation update

	// Every actor's last hit, from wherever it is seen first (ProcessHitData for weapons, arrows, bashes and
	// explosions; the hit event for spells and shouts): who, from where, how hard. Reactions read it a frame later
	// (a stagger has no attacker for Fus or magic, an arrow's push scales by its damage).
	struct LastHit
	{
		std::chrono::steady_clock::time_point when;
		RE::ActorHandle                       from;
		RE::NiPoint3                          at, dir;  // game units; dir: the way it went (0 if not known)
		float                                 damage{};
		bool                                  exact{};  // at is where it hit
		std::uint32_t                         flags{};  // RE::HitData::Flag (0 for spells)
		float                                 strike{};  // a melee blow's push, N s (0: not one)
		RE::NiPoint3                          strikeDir;  // its way (unit)
		int                                   element{ -1 };  // a spell's (Element): its explosion's, which has none of its own
		bool                                  shout{};        // a shout's (a voice power's) hit
		int                                   word{};         // its words, 1-3, from its spell (ShoutWord); 0: not known
		float                                 stagger{};      // its spell's strongest stagger effect (Fus Ro Dah 1.5+)
	};
	inline std::mutex                              g_hitLock;
	inline std::unordered_map<RE::FormID, LastHit> g_lastHit;

	inline void RecordHit(RE::FormID a_id, const LastHit& a_hit)
	{
		std::scoped_lock l(g_hitLock);
		auto&            h = g_lastHit[a_id];
		// the same hit seen twice (its HitData, then its event): the first one's point and direction stay
		if (a_hit.when - h.when < std::chrono::milliseconds(200) && h.exact && !a_hit.exact) {
			h.damage = std::max(h.damage, a_hit.damage);
			if (h.element < 0) {
				h.element = a_hit.element;
			}
			if (!h.from) {
				h.from = a_hit.from;
			}
			h.shout |= a_hit.shout;
			if (!h.word) {
				h.word = a_hit.word;
			}
			h.stagger = std::max(h.stagger, a_hit.stagger);
			return;
		}
		h = a_hit;
		if (g_lastHit.size() > 256) {
			const auto now = std::chrono::steady_clock::now();
			std::erase_if(g_lastHit, [&](const auto& e) { return now - e.second.when > std::chrono::seconds(5); });
		}
	}
	inline bool RecentHit(RE::FormID a_id, float a_maxAge, LastHit& a_out)
	{
		std::scoped_lock l(g_hitLock);
		const auto       it = g_lastHit.find(a_id);
		if (it == g_lastHit.end() || std::chrono::duration<float>(std::chrono::steady_clock::now() - it->second.when).count() > a_maxAge) {
			return false;
		}
		a_out = it->second;
		return true;
	}
	inline std::atomic<int> g_skip{ 0 };   // ragdolled NPCs left to Skyrim (skip_), same
	inline std::atomic<int> g_held{ 0 };   // deaths held back (held_), same

	constexpr std::array<const char*, 5> kHitNames{ "arrow", "frost spell", "fire spell", "shock spell", "spell" };

	// Where Skyrim's knock "explosion" comes from: never the actor's own position, the direction from the source to
	// the actor would be zero and its normal NaN (a NaN ragdoll impulse: SMP cloth turns grey and duplicates).
	// Behind her along a_dir (flat), else behind her facing.
	inline RE::NiPoint3 KnockSource(RE::Actor* a_actor, RE::NiPoint3 a_dir)
	{
		if (a_dir.Length() < 1e-4f) {
			const float z = a_actor->GetAngleZ();
			a_dir = { -std::sin(z), -std::cos(z), 0.0f };  // falling backwards
		}
		a_dir.z = 0.0f;
		a_dir.Unitize();
		return a_actor->GetPosition() - a_dir * 64.0f;
	}

	// A bone's name without its " [tag]": creatures' skeletons name the same bones differently (falmer have no tag,
	// draugr and skeletons' left foot is "[LLft ]")
	inline std::string_view BoneBase(std::string_view a_name)
	{
		const auto b = a_name.find(" [");
		return b == std::string_view::npos ? a_name : a_name.substr(0, b);
	}

	// a falmer (by her race's editor ID): she stands hunched. Delirium's balance can't hold that stance yet, so only blows
	// that put her down bring her in (React), and her joint limits are moved on to her stance (body/fit.cpp).
	inline bool Hunched(RE::Actor* a_actor)
	{
		auto*       race = a_actor ? a_actor->GetRace() : nullptr;
		const char* id = race ? race->GetFormEditorID() : nullptr;
		if (!id) {
			return false;
		}
		std::string s = id;
		std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return s.find("falmer") != std::string::npos;
	}

	// the rig's bone is this one: the exact name, or (a creature, a_loose) the same name without its tag
	inline bool SameBone(std::string_view a_rig, std::string_view a_bone, bool a_loose)
	{
		return a_rig == a_bone || (a_loose && BoneBase(a_rig) == BoneBase(a_bone));
	}

}

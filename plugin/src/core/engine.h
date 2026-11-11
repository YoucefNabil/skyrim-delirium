#pragma once

#define DELIRIUM_PHYSICS_STATIC
#include "delirium_physics.h"
#include "vec.h"
#include "body/skyrim_body.h"

#include <SimpleIni.h>

#include <map>
#include <span>
#include <variant>

namespace delirium
{
	namespace settings
	{
		struct Setting;  // settings/table.h
		// a setting's value itself (whole numbers of any width as long long)
		using Any = std::variant<bool, long long, float, std::string>;
	}


	// One line of the rig file (fit-test's write_rig): how a physics part sits on its Skyrim bone.
	struct RigPart
	{
		std::string name, ref, jointA, jointB;
		bool        write{};
		Q           offset;    // ref bone rotation = part rotation * offset
		V3          anchor;    // the part's joint in its own frame (root: its hips' midpoint = the pelvis bone)
		V3          inParent;  // the same joint in the parent's frame
	};

	// body/show.cpp: an NPC's bones as her meshes read them while Delirium writes them, and the skins moved over to them
	struct ShownPose
	{
		struct Bone
		{
			RE::NiTransform drawn;  // what her skins read
			RE::NiTransform next;   // the finished pose, drawn from the next render frame on (PublishShown)
		};
		// a bone of hers whose world a physics mod may write after Show (SMP cloth, hair, breasts), and the nearest
		// bone above it in the copy, which it is drawn from (PhysicsIn)
		struct Follow
		{
			RE::NiPointer<RE::NiAVObject> node, parent, anchor;
			Bone*                         bone;
			Bone*                         anchorBone;
		};
		std::unordered_map<RE::NiAVObject*, Bone>      bones;
		std::vector<Follow>                            follow;  // this frame's, parents first
		std::vector<RE::NiPointer<RE::NiSkinInstance>> skins;
		int                                            published{}, held{};  // frames, for the release log
		bool                                           heldLast{};          // the last Present held her pose back
	};

	class Engine
	{
	public:
		static Engine& Get();

		void LoadSettings();  // SkyrimDelirium.ini, through the settings table (core/settings/)
		void ReadSettings(const CSimpleIniA& a_ini);  // every setting and reaction row from an ini (unknown keys warned)
		void LogSettings() const;                     // the start-up summary of the settings
		[[nodiscard]] std::string DumpSettings() const;  // every setting and reaction row, one per line (the settings check)
		std::vector<settings::Setting> SettingsTable();  // every setting: its ini key, default, limits, menu name and help

		// ---- settings while the game runs (core/settings/live.cpp): every change is logged as Section.key old -> new.
		// Writers take lock_ (the reaction rows are only read under it); the numbers are read as they are.
		struct SettingsSnap
		{
			std::vector<std::string>           values;  // each table row's value as the ini writes it
			std::map<std::string, std::string> rows;    // the reaction rows' texts
			bool operator==(const SettingsSnap&) const = default;
		};
		SettingsSnap Snapshot();
		void ReloadSettings(const char* a_why);  // SkyrimDelirium.ini again
		void SettingsFileChanged();              // the host saw the ini change: reloaded, unless it was our own save
		// one setting (the menu): limited, and settings that must stay at least it follow. True if it changed
		bool SetSetting(const settings::Setting& a_s, const settings::Any& a_v);
		void SetRow(const std::string& a_key, const std::string& a_text);  // a reaction row (added if new)
		void ResetRow(const std::string& a_key);  // back to the built-in row, or gone if it isn't one
		void DefaultSettings(const char* a_why);  // every preset setting and reaction row at its default
		void NoteEdit(const std::string& a_name, const std::string& a_was, const std::string& a_now);  // the menu's edit, done
		[[nodiscard]] bool Unsaved() const { return unsaved_; }  // the live settings differ from SkyrimDelirium.ini's
		[[nodiscard]] const std::string& SaveError() const { return saveError_; }  // the last save's failure, else empty
		void RevertSession();  // back to the settings the game started with (saved)
		std::vector<std::pair<std::string, std::string>> Rows();  // every reaction row and its text, sorted
		static bool IsBuiltInRow(const std::string& a_key, const char** a_text = nullptr);
		// a row's words as the engine reads them: name, number (NaN: none given), the first one its base
		static std::vector<std::pair<std::string, float>> RowWords(std::string_view a_text) { return ParseRule(a_text).layers; }
		void MenuReactionsHelp();  // the Help page's part on the reaction table (core/menu/reactions.cpp)
		void MenuRow(const std::string& a_key, const char* a_label);  // one reaction row's controls, on its own (the Quick page)
		// ---- saving (core/settings/save.cpp): only the lines whose value differs are rewritten, comments stay
		bool SaveIni(std::string& a_msg);
		bool SavePreset(const std::string& a_name, std::string& a_msg);  // Presets\<name>.ini: what differs from the defaults
		bool LoadPreset(const std::string& a_name, std::string& a_msg);  // the defaults, then the preset's values
		static std::vector<std::string> PresetNames();
		void ApplyChanged(std::span<const std::string> a_names);  // engine.cpp: what start-up applied once, again
		// ---- the in-game menu (core/menu/): SKSE Menu Framework pages, drawn through the host
		void Menu(int a_page);
		bool Init();  // kDataLoaded: Delirium's DLL, the character file, our body and rig
		void OnKey(std::uint32_t a_key, RE::Actor* a_target = nullptr);  // a_target: else the crosshair's / console's NPC
		void Command(std::string_view a_line);  // main thread: a line of the command file (see Command in engine.cpp)
		void Shutdown();                        // main thread, before the core is unloaded: let go of every NPC
		void ReleaseAll(const char* a_why);     // let go of every NPC (loading screen)
		void NoKillMoves();  // [Deaths]: Skyrim's killmoves and kill cams off (Delirium plays the death)
		void AfterAnimation(RE::Actor* a_actor, float a_delta);  // after Skyrim's animation update: steps the physics
		void AfterPose(RE::Actor* a_actor, bool a_postPhysics);
		void AfterPoseImpl(RE::Actor* a_actor, bool a_postPhysics);  // right after Skyrim copied its pose into the bones: ours go on
		void DriveRagdoll(RE::Actor* a_actor, RE::hkbRagdollDriver* a_driver, float a_dt, bool a_after);  // around Skyrim's ragdoll drive
		static bool Busy();                                     // any body active (cheap, no lock)
		bool KeepsDown(RE::Actor* a_actor);                     // Delirium still moves this fallen body: no get-up yet
		void AfterRootMove(RE::Actor* a_actor);                 // Skyrim just put the 3D root on the actor: our bones stay put
		// what hit her: an arrow or bolt, or a destruction spell's projectile by its element
		enum HitKind : int { kArrow, kFrost, kFire, kShock, kMagic };
		// main thread: a projectile hit a living NPC (game units, units/s; a_exact: a_at is where it went in)
		// a_sure: brings her in whatever the chance (a spell that staggers, the console)
		void OnHit(RE::Actor* a_target, const RE::NiPoint3& a_at, const RE::NiPoint3& a_vel, bool a_exact, int a_kind, bool a_channel = false, bool a_sure = false);
		// any weapon hit on an actor (Actor::ProcessHitData, before Skyrim's): kept as her last hit
		void OnHitData(RE::Actor* a_target, const RE::HitData& a_hit);
		// Skyrim staggered her (a_started: it really did now): Delirium's push takes the stagger's place on the body
		void OnStagger(RE::Actor* a_target, float a_magnitude, RE::Actor* a_source, bool a_started);
		void Stagger(RE::Actor* a_target, float a_magnitude, RE::Actor* a_source);  // main thread, a frame later
		// someone ran into her hard (Skyrim's big bump, before it): true when Delirium plays it (Bump, a frame later) in
		// place of Skyrim's stumble
		bool OnBump(RE::Actor* a_target, RE::Actor* a_bumper);
		void Bump(RE::Actor* a_target, RE::Actor* a_bumper);  // main thread
		// body meets body ([Bodies], reactions/bodies.cpp). Every animation update, before her own: a standing NPC's
		// ragdoll bodies go into Skyrim's Havok world (body/world.cpp), and a body that comes at one of them hard enough
		// (a ragdolled one, or one Delirium has on its feet) is a blow on her
		void Meet(RE::Actor* a_actor);
		// main thread: a body (a_by's) struck her where she stands, at a_at (game units) moving a_vel (m/s against her)
		void BodyHit(RE::Actor* a_target, RE::Actor* a_by, const RE::NiPoint3& a_at, const RE::NiPoint3& a_vel);
		// debug (debug/throw.cpp): stand a_thrown 1 m before a_target on the player's line to her, Skyrim's own push into
		// her, and log what the two sets of bodies did over the next 2 s
		void Throw(RE::Actor* a_thrown, RE::Actor* a_target, float a_force);
		void ThrowStep(RE::Actor* a_actor);  // every animation update: the throw being measured
		// her killing blow (Character::KillImpl, before Skyrim's): true holds the death back, at 1 health, for up to
		// [Deaths] fDyingTime s: she dies on her feet in Delirium (wounded shot, stagger, collapse), then really dies
		bool OnKill(RE::Actor* a_actor, RE::Actor* a_attacker, float a_damage, bool a_sendEvent, bool a_ragdollInstant);

	private:
		struct Active
		{
			RE::ActorHandle                    actor;
			NmAgent*                           agent{};
			NmWorld*                           world{};
			RE::NiPoint3                       origin;  // game units; physics works in metres from here
			RE::NiPoint3                       colCenter;  // game units: where the collision patch was gathered around
			// the 3D she was taken on, held: a script can swap it under us (outfit, disable / enable, Helgen's scenes) and
			// free the bones below; held, they stay alive until we let go, and a new 3D lets her go (Same3D)
			RE::NiPointer<RE::NiAVObject>      root;
			std::array<RE::NiAVObject*, 21>    ref{};
			std::array<RE::NiAVObject*, 21>    ja{}, jb{};  // the bones whose midpoint is each part's joint
			RE::NiAVObject*                    com{};  // NPC COM: carries both the pelvis and the spine branches
			bool                               looseBones{};  // a creature turned on in [Creatures]: bones also matched without their tag (SameBone)
			bool                               fitted{};      // her body was rebuilt on her own bones (body/fit.cpp), not the rig's person
			// each part's joint in its own frame and in its parent's, at her size (a person: the rig's times her scale)
			std::array<V3, 21>                 anchor{}, inParent{};
			std::array<float, 21>              partMass{};    // each part's mass at her size, kg
			// snap trace around the fall (knock until 0.6 s after calibration): the drawn pose at each pose copy / root move
			bool                               haveTrace{};
			std::array<RE::NiTransform, 21>    trBone{};
			std::array<RE::NiMatrix3, 21>      trRel{};   // each bone's rotation relative to the pelvis
			std::array<RE::NiPoint3, 21>       trSpin{};  // and its last change (axis * sin angle)
			RE::NiTransform                    trTop;
			RE::NiPoint3                       trPelvisMove;
			std::array<RE::NiTransform, 21>    written{};  // bone worlds as WriteBones left them (trace: drawn vs ours)
			bool                               haveWritten{}, dumped{};
			// F7's lead-in: what is drawn goes from our pose to Skyrim's (its ragdoll can't reach every pose of ours)
			float                              limbBlend{ 1.0f };  // share of ours on the limbs in WriteBones
			RE::NiTransform                    skyComLocal;        // NPC COM from Skyrim's last pose copy
			bool                               haveSkyCom{};
			int                                traceN{}, physSteps{}, trSteps{};
			RE::NiMatrix3                      comInPelvis;  // COM's rotation in the pelvis's frame (from the standing pose)
			float                              age{}, calm{};
			float                              sincePush{};  // seconds since the last push (no calm-out before 1 s)
			int                                calls{}, poses{};  // animation updates / pose copies since the last log line
			// every node of her 3D as we drew it last frame: put back as previousWorld after WriteBones (see KeepPrevious)
			std::unordered_map<RE::NiAVObject*, RE::NiTransform> drawnWorld;
			ShownPose                          shown;  // her bones as her meshes read them (body/show.cpp)
			float                              dtSum{};
			float                              scale{ 1.0f };  // the NPC's skeleton scale
			bool                               logged{};
			// Skyrim's side: upright we move the actor under the pelvis; fallen, Skyrim's own ragdoll state carries it
			bool                               knockQueued{}, wasBalancing{};
			// she is down in Skyrim's ragdoll state and its bodies are her body: they lead, Delirium follows with torques
			// after F7's lead-in. Not the same as sky.linked (we know how the bodies sit on Delirium's parts).
			bool                               ragdolled{};
			bool                               takeover{};  // started from Skyrim's own ragdoll (not F7)
			// upright: her AI held (no arrows, casts or swings, body/hold.cpp); Skyrim's own stagger when we took her and its time left
			bool                               holding{}, skyStagger{};
			float                              skyStaggerLeft{};
			RE::Actor*                         heldActor{};  // as registered for the handler gates (compared, never read)
			bool                               death{};     // a corpse: dies in Delirium, then fades over to Skyrim's ragdoll
			bool                               writhe{}, writhing{}, relaxed{};  // the dying plan: writhe first or not, gone limp
			bool                               heldDeath{};  // dying on her feet: her death is held back (OnKill)
			bool                               pinned{};    // dead on her feet: Skyrim's ragdoll follows Delirium's pose exactly until she is down
			float                              pinUntil{};  // age: pinned at most until then ([Deaths] fDyingTime)
			RE::FormID                         pinId{};     // as registered for DeadOnFeet
			float                              collapseAt{ -1.0f };  // age: dying on her feet, she stops fighting it and goes down
			float                              relaxAt{};   // age: when the next dying step (writhe end / limp) comes
			float                              dyingAt{ -1.0f };  // age: when she died (the shot plays before any writhe)
			int                                lastType{ -1 }, lastElement{ -1 };  // her last blow's type and element ([Reactions] die rows)
			float                              dieAmp{ -1.0f }, diePeriod{ -1.0f };  // her death writhe's size and s per cycle (-1: random)
			float                              speedCap{ -1.0f }; // m/s: the next drive caps every Skyrim body at this (her death's spikes), once
			float                              ragdollAge{};  // age when Skyrim's ragdoll took her (ragdolled)
			int                                others{};      // other NPCs' bodies in her physics as capsules right now (Others)
			bool                               following{};   // torque drive: Delirium in FOLLOW mode on Skyrim's ragdoll
			bool                               haveDrv{};
			std::array<float, 21 * 6>          drv{};         // Delirium's force and torque per part from its last step (world)
			std::array<float, 21 * 3>          acc{};         // Delirium's joint accelerations from its last step (world)
			std::array<float, 21 * 3>          ext{};         // its torques on the parts themselves (a shot's spin, electrocute's jolts; world, N m)
			float                              maxDw{}, maxWant{};
			float                              actSum{}, extMax{};  // trace: Skyrim's bodies' spin vs the pelvis (deg/s, summed per step); behaviours' biggest torque turn
			int                                actN{};
			std::vector<Q>                     actRef;   // trace: each body's turn against the pelvis body when the second began
			std::vector<float>                 actSwing; // and the most it has swung from it since (deg)
			int                                sleepMax{}; // trace: most frames a body has been counted still by Havok (deactivation) this second
			int                                clamped{}, spinCapped{}, wantPart{};
			float                              wantAge{};     // age of the strongest joint ask since the last log line
			SkyrimBody                         sky;  // Skyrim's own ragdoll of her (body/skyrim_body.h)
			float                              settled{}, noRagdoll{};  // seconds lying still in Skyrim's ragdoll state
			float                              deadFor{};     // seconds since she died in Delirium (a corpse is let go after kCorpseHold)
			float                              keLast{ -1.0f };  // her energy at the last once-a-second check
			int                                keRising{};    // seconds in a row her energy grew while down (the simulation running away)
			int                                drives{};
			float                              maxSpeed{};    // fastest body steering since the last log line (m/s)
			float                              blend{ 1.0f };  // 1 = Delirium's pose, 0 = Skyrim's animation
			bool                               blendingOut{};
			// Ragdolled, Delirium writes the skeleton until Skyrim's get-up (no blend over to Skyrim's own ragdoll pose:
			// that pose sits 0.1-0.2 m off ours and showed as a snap at landing)
			int                                copies{};  // Skyrim's pose copies since the last animation update
			// the skeleton's top node as last drawn, and how far SmoothRoot holds it behind Skyrim's (rad)
			Q                                  drawnRootQ;
			RE::NiPoint3                       drawnRootT;
			float                              drawnAge{}, rootLag{}, maxRootLag{};
			bool                               haveDrawnRoot{};
			// NPC COM in the world as WriteBones left it: when Skyrim moves the 3D root afterwards, COM's local is redone so
			// the bones under it stay where we put them
			RE::NiTransform                    comWorld;
			bool                               haveCom{};
			int                                rootFixLog{}, nanLog{};
			std::array<Q, 21>                  restRot{};  // the body's own standing stance, each part in its parent's frame (legs use it)
			std::array<V3, 21>                 restPos{};
			std::string                        trackFile;  // [Test] bRecordPoses: where her held poses go, empty when not recorded
			int                                trackN{};
			float                              maxRootFix{};
			// trace: the pelvis when the last arrow hit, and the way it flew (flat)
			bool                               haveShot{};
			float                              burnUntil{ -1.0f }, shockUntil{ -1.0f };  // age the fire / shock stops (-1: none)
			float                              chillUntil{ -1.0f };                        // a frost channel: freezing until then
			float                              reelUntil{ -1.0f };  // a hard hit: staggering back on loose legs until then, then she catches herself
			int                                flying{};            // thrown: 1 in the air, 2 coming down (Fly)
			float                              offBalance{};        // s her balance has been off (a behaviour restarting it is off for a moment: not a fall)
			int                                flyMode{};           // 0 windmill + pedal, 1 rolled up, 2 highFall, 3 curled up kicking
			bool                               ledgeFall{};         // flying off a ledge on her own (reactions/falls.cpp): landed by the ground under her
			bool                               braced{};            // thrown at something: arms out at it
			float                              braceCheckAt{};      // age of the next look ahead
			float                              writheFor{};         // s she writhes once she is down (0: she doesn't)
			float                              writheAt{ -1.0f };   // age the writhe starts (she has caught the fall first)
			float                              writheUntil{ -1.0f };
			float                              writheOomph{};       // how hard it was: a bigger writhe
			int                                writheElement{ -1 }; // what took her down: burning rolls, shock convulses, frost curls up shivering
			struct Kick
			{
				int  part{ -1 };  // Delirium part it landed on: its Skyrim body at the drive (-1: all of them)
				V3   j{};         // N s on that body; all of them: m/s
				V3   arm{};       // from the body's centre to where it landed (m): the spin it gives
			};
			std::vector<Kick>                  kicks;  // blows on her while she is down, put on Skyrim's bodies at the next drive
			float                              flyAt{};             // age she was thrown
			int                                filled{};  // trace: frames this second Skyrim skipped its pose copy (ours put on here)
			bool                               filling{};  // in that filler AfterPose: the bones hold our pose, not the animation
			V3                                 shotPelvis, shotDir;
			// the last push (age, N s): a hit and its stagger come together and push once, the stronger one
			float                              lastPushAt{ -10.0f }, lastPush{};
		};

		bool Activate(RE::Actor* a_actor, bool a_takeover = false, bool a_death = false);
		void TryTakeOver(RE::Actor* a_actor, bool a_dead);  // in Skyrim's ragdoll (or death ragdoll): Delirium takes the body over (or skips it)
		void ForgetSkip(RE::Actor* a_actor);   // out of Skyrim's ragdoll: may be taken over next time
		void InheritVelocity(Active& a);
		void Release(std::size_t a_i, const char* a_why);
		void Push(Active& a_act, RE::Actor* a_actor);
		// a push at a_at (metres, the agent's frame) along a_dir; with another push in the last 0.3 s only what this
		// one adds over it. Returns the N s applied
		float AddPush(Active& a, const float* a_at, const V3& a_dir, float a_push, float a_pelvis = 0.0f);
		float DamageScale(RE::Actor* a_target, float a_damage) const;  // the push's scale for a hit of this much damage
		// a melee blow's push (N s, 0 if it isn't one) and its way (unit): by the attack, the weapon's weight and the damage
		float StrikePush(RE::Actor* a_target, const RE::HitData& a_hit, RE::NiPoint3& a_dir) const;
		// an explosion caught her (direct hit or near it): pushed away from its centre, by its damage and how near she was
		void  Blast(RE::Actor* a_target, const RE::HitData& a_hit, RE::Explosion* a_ex, RE::BGSExplosion* a_base, float a_spellDamage, int a_spellElement);
		void  LookAt(Active& a, RE::Actor* a_by, float* a_out) const;  // her attacker's head, Delirium's frame
		// fire / shock / frost on again after a task stopped every behaviour (their end time stays: nothing is extended)
		void  Overlays(Active& a, const V3& a_away);
		void  Reacting(Active& a);  // a new reaction: no calm-out, the body fully ours again
		// an arrow's hit (game units; a_exact: a_at is where it went in, else the chest): Delirium's shot reaction
		// One blow, whatever dealt it (an arrow, a spell, a sword, a stagger): every push goes through React, which
		// picks the reaction by what the blow is and how hard it is. Its sources only say what hit her and how hard.
		struct Blow
		{
			const char*  what{ "hit" };  // for the log
			std::string  note;           // for the log: what its strength came from
			RE::ActorPtr by;             // who did it (none: a trap, a stagger with no caster seen)
			RE::NiPoint3 at;             // where it landed, game units
			bool         exact{};        // at is on her (else the chest)
			V3           dir{};          // the way it pushes (unit)
			float        push{};         // N s
			bool         knocks{};      // a shout that staggers as hard as Fus Ro Dah (kKnockStagger): only it takes a row's throw
			int          element{ -1 };  // kFire / kShock: that reaction on top, for its own time
			bool         shot{};         // a projectile went in: Delirium's shot task. Else by strength: a flinch or a step
			bool         canStart{ true };  // may bring her into Delirium (a melee blow only joins a reaction)
			float        startChance{ 1.0f };  // chance it brings her in when she isn't in Delirium yet (a killing blow always does)
			float        oomph{};        // 0-1, how far past a plain hit it is: a harder jolt (the shot's spin, lean, push)
			bool         join{};         // a reaction started this moment (a direct hit, then its blast): only the push adds
			V3           fly{};          // m/s: thrown (an explosion), alive in the air; zero: on her feet
			bool         melee{};        // a weapon blow (or the stagger it makes): its oomph is from how much harder than a plain blow
			bool         stagger{};      // Skyrim staggered her with it: on her feet it pushes up to [Stagger] fMaxPush
			int          type{ 0 };      // BlowType: its row in the reaction table
			bool         inherited{};    // fly: she already moves (Skyrim's own knock, taken over): no velocity or tumble of ours
			int          word{};         // a shout's words, 1-3 (its rows: stand.shout.word1-3); 0: not a shout
		};
		// the reaction table ([Reactions]): a blow's phase, type and strength pick one rule, a base behaviour and its layers
		enum BlowType : int { kBlowOther, kBlowMelee, kBlowArrow, kBlowSpell, kBlowBlast, kBlowStagger, kBlowKnock, kBlowShout, kBlowBump };
		static const char* TypeName(int a_type);  // other, melee, arrow, spell, blast, stagger, knock, shout, bump
		void               Knocked(Active& a, const V3& a_vel);  // Skyrim knocked her down itself (a shout, a trap, a script): her reaction
		struct Rule
		{
			std::string                                base;    // shot, flinch, step, none
			std::vector<std::pair<std::string, float>> layers;  // name, value (NaN: none given)
			std::string                                text;    // as written
			[[nodiscard]] bool  Has(std::string_view a_name) const;
			[[nodiscard]] float Value(std::string_view a_name, float a_or) const;  // the layer's (or base's) value, else a_or
		};
		static Rule ParseRule(std::string_view a_text);
		void        LoadReactions(const CSimpleIniA& a_ini);  // the built-in rows, then the ini's [Reactions] rows over them
		static std::span<const std::pair<const char*, const char*>> BuiltInRows();
		static bool IsRowKey(const char* a_key);  // a [Reactions] key that is a row (not a setting such as fHeavyOomph)
		static std::vector<std::string> UnknownBehaviours(const Rule& a_rule);
		void        LogChanges(const SettingsSnap& a_before, const char* a_why);  // what differs from a_before, then ApplyChanged
		void        LoadPresetIni(const CSimpleIniA& a_ini);  // preset settings from a_ini (absent: default), rows built in + a_ini's
		void        MenuReactions();      // core/menu/reactions.cpp
		SettingsSnap                    saved_;   // the settings as SkyrimDelirium.ini gives them (last load or save)
		std::filesystem::file_time_type savedTime_{};  // the ini's time after our own save
		bool                            unsaved_{};
		std::string                     startText_;  // SkyrimDelirium.ini as the game started with it (RevertSession)
		std::string                     saveError_;
		void                            AutoSave();  // every change is kept: SkyrimDelirium.ini written when it differs
		const Rule& PickRule(const char* a_phase, const Blow& a_blow, bool a_lethal, std::string& a_key) const;
		Rule        ElementRule(int a_element) const;  // the element's layers ([Reactions] fire / shock / frost)
		const Rule* FindRule(std::initializer_list<std::string> a_keys, std::string& a_key) const;  // the first of these rows there is
		static const char* ElementName(int a_element);  // arrow, frost, fire, shock, magic; "" for none
		// one of the row's choices (its names among a_names), by their weights (name:weight, 1 if none given); -1 if none
		static int Choose(const Rule& a_rule, std::initializer_list<std::string_view> a_names);
		void React(RE::Actor* a_target, const Blow& a_blow);
		void ReactOn(Active& a, RE::Actor* a_actor, const Blow& a_blow);  // under lock_
		void DownHit(Active& a, RE::Actor* a_actor, const Blow& a_blow);  // a blow on her while she is down (or a corpse); under lock_
		void StartDying(Active& a, const V3& a_moving, bool a_held = false);  // the killing blow as a shot, then writhe/limp; under lock_
		struct Held
		{
			RE::ActorHandle                       actor, attacker;
			float                                 damage{};
			bool                                  send{}, instant{};
			std::chrono::steady_clock::time_point at{};
			const char*                           why{ "" };
		};
		void DyingOnFeet(RE::Actor* a_actor);              // main thread, after her held killing blow: Delirium has her, wounded
		bool DieOnFeet(RE::Actor* a_actor, RE::Actor* a_attacker, float a_damage, bool a_sendEvent);  // [Deaths] bDieAtOnce: killed now, stumbles pinned
		void StumbleShot(RE::Actor* a_actor, RE::Actor* a_killer);  // main thread, after the kill: no reaction of her own came, a shot
		void StartShot(Active& a, RE::Actor* a_actor, RE::Actor* a_killer);  // the killing blow as a wounded shot (no reaction of its own); under lock_
		void Unpin(Active& a, const char* a_why);  // dead on her feet no more: Delirium follows Skyrim's ragdoll again; under lock_
		bool Dying(const RE::Actor* a_actor) const;  // her death held back, or dead on her feet: the killing blow plays as dying; under lock_
		void CheckHeld(RE::Actor* a_actor);                // every animation update: her time up (or down): she dies
		void KillHeld(const char* a_why);                  // main thread: every held death now
		void KillNow(const Held& a_held, const char* a_why);  // main thread: Skyrim's kill, with the killer and blow it had
		void Fly(Active& a, RE::Actor* a_actor, const Blow& b);  // thrown: arms windmill / legs pedal or tucked in a roll, then a catch
		bool Eligible(RE::Actor* a_target, const char* a_what, bool a_quiet) const;  // a person Delirium can take
		bool Creature(RE::Actor* a_actor) const;  // a draugr, skeleton or falmer whose kind [Creatures] turns on
		void PersonOffsets(Active& a) const;      // body/fit.cpp: a person's joint offsets and masses: the rig's, at her scale
		NmBuild* FitCreature(Active& a, RE::Actor* a_actor) const;  // body/fit.cpp: the body rebuilt on her own bones, its offsets in a
		void StartBurn(Active& a, const V3& a_away);  // fire: the arms beat at the flames, a stumble away
		void StopBurn(Active& a);
		void StartShock(Active& a);  // lightning: jolts through the body
		void StopShock(Active& a);
		void StartChill(Active& a);  // frost: hunched, shivering, short stiff steps
		void StopChill(Active& a);
		// a channelled spell (Flames, Frostbite, Sparks): no shot, only the element's reaction while it keeps hitting
		void Channel(Active& a, RE::Actor* a_actor, const RE::NiPoint3& a_vel, int a_kind);
		void WatchFall(RE::Actor* a_actor);         // reactions/falls.cpp: every NPC not in Delirium, each frame
		bool LedgeLanded(const Active& a) const;    // her own drop is over
		int  NearestPart(const Active& a, const float* a_at) const;  // the part closest to a point (Delirium's frame)
		float BodyVelocities(const Active& a, std::array<V3, 21>& v, std::array<V3, 21>& w, std::array<bool, 21>& have) const;
		void  SyncFromSkyrim(Active& a);
		bool  TorqueMode(const Active& a) const;
		float LeadShare(const Active& a) const;  // F7's lead-in: 1 at its start, 0 at its end (and after)
		void  MapParts(Active& a);
		void  ApplyTorques(Active& a, float a_dt);
		void  PartsFromBodies(const Active& a, std::array<V3, 21>& pos, std::array<Q, 21>& rot) const;
		// a_previous: from the bones as drawn last frame (previousWorld), for the speed she moved at
		float PartsFromBones(const Active& a, std::array<V3, 21>& pos, std::array<Q, 21>& rot, std::string* detail, bool a_previous = false) const;
		// upright entry: each part's speed from her animation (now against last frame), so she goes in moving as she moved
		void PartSpeeds(const Active& a, const std::array<V3, 21>& pos, const std::array<Q, 21>& rot, std::array<V3, 21>& v, std::array<V3, 21>& w) const;
		void  SetZeroPose(Active& a, const std::array<V3, 21>& pos, const std::array<Q, 21>& rot);
		void WriteBones(Active& a_act);
		// body/show.cpp: her meshes read the finished pose (Active::shown), not the bones while they are written
		void Show(Active& a, RE::NiAVObject* a_root);
		void Unshow(Active& a);
		bool SmoothRoot(Active& a, RE::Actor* a_actor);  // ragdolled: the top node turns no faster than cloth physics allows (true: moved)
		void AddWorld(Active& a, RE::Actor* a_actor, const RE::NiPoint3& a_center, bool a_quiet);  // Skyrim's collision around a_center
		void LinkBodies(Active& a, RE::Actor* a_actor);  // which of Skyrim's ragdoll bodies sits on which Delirium part (sky.linked)
		void Calibrate(Active& a, RE::Actor* a_actor);   // she went down: the links again, and Skyrim's joints eased for our torques
		void DumpRagdoll(const Active& a, RE::Actor* a_actor);  // Skyrim's ragdoll joints to a JSON for the fit-test body
		void RecordPose(Active& a, RE::Actor* a_actor, const std::array<V3, 21>& pos, const std::array<Q, 21>& rot);
		void StandingLegs(const Active& a, std::array<V3, 21>& pos, std::array<Q, 21>& rot) const;
		void Trace(Active& a, RE::Actor* a_actor, const char* a_where);
		void RefreshBones(Active& a);

		bool DeliriumBone(const Active& a, const BodyLink& l, V3& a_pos, Q& a_rot) const;  // havok world
		void BodyTarget(const Active& a, const BodyLink& l, V3& a_pos, Q& a_rot) const;
		void FollowActor(Active& a, RE::Actor* a_actor);
		void HoldCombat(Active& a, RE::Actor* a_actor, float a_dt);  // body/hold.cpp: every upright frame
		void FreeCombat(Active& a);                                  // going down, or let go

		std::recursive_mutex                  lock_;  // recursive: Skyrim may call a hooked function from inside one of ours
		std::vector<std::unique_ptr<Active>>  active_;
		std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> releasedAt_;  // let go in Skyrim's ragdoll: when (the get-up log)
		std::vector<RE::ActorHandle>          skip_;  // in Skyrim's ragdoll but not ours (getting up, paralysed, ...)
		std::array<RigPart, 21>               rig_;
		float                                 rigYaw_{};
		NmBuild*                              build_{};
		bool                                  ready_{};

		// settings
		std::string   fredPath_, buildPath_, rigPath_;
		bool          usingGta_{ false };
		std::uint32_t pushKey_{ 0x41 }, releaseKey_{ 0x3C };  // F7, F2
		float         push_{ 80.0f };
		bool          autoRelease_{ true };
		bool          followAnimation_{ true };
		bool          standingLegs_{ true };  // [Test] bStandingLegs: the legs hold the body's own stance, not the animation's
		float         carrySpeed_{ 0.0f };    // [Test] fCarrySpeed: share of her travel speed she keeps going into Delirium on the ground
		bool          recordPoses_{ false };  // [Test] bRecordPoses: the held pose each frame while standing, for fit-test sweep STANCE_TRACK=
		int           tracks_{ 0 };
		bool          collision_{ true };
		bool          takeOver_{ true };  // [Ragdoll] bTakeOver
		float         gain_{ 1.0f };      // [Ragdoll] fTorqueGain
		float         damp_{ 0.0f };      // [Ragdoll] fJointDamping: share of a joint's spin its muscles take out per step
		bool          show_{ true };      // her meshes read the finished pose (body/show.cpp); `show off` for a before/after check
		float         friction_{ 0.0f };  // [Ragdoll] fJointFriction: Skyrim's joint friction while Delirium drives (1 = untouched)
		bool          matchMass_{ true }; // [Ragdoll] bMatchMass: Skyrim's bodies weigh what Delirium's parts do while it drives
		bool          matchContact_{ true }; // [Ragdoll] bMatchContact: and grip and bounce as Delirium's parts do
		bool          noKillMoves_{ true };  // [Deaths] bNoKillMoves
		bool          noKillCams_{ true };   // [Deaths] bNoKillCams
		float         dyingHealth_{ 70.0f };  // [Deaths] fDyingHealth: the killing blow as a shot at this health (0: fatal, every muscle off in 0.5 s)
		float         limpMin_{ 1.8f }, limpMax_{ 3.0f };  // [Deaths] fLimpAfterMin/Max: s until the corpse goes limp
		float         dyingWrithe_{ 0.5f };   // [Deaths] fWritheChance: a weak writhe on the ground first
		float         dyingCatch_{ 0.5f };    // [Deaths] fCatchFallChance: the arms go out to break the fall
		float         deathPush_{ 1.5f };     // [Deaths] fDeathPush: m/s away from the killer, in place of Skyrim's death motion
		float         deathMaxSpeed_{ 6.0f }; // [Deaths] fDeathMaxSpeed: m/s cap on any body as she dies
		float         dyingReact_{ 1.2f };    // [Deaths] fShotTime: s the dying shot plays before a writhe
		float         collapseMin_{ 0.8f }, collapseMax_{ 1.6f };  // [Deaths] fCollapseAfterMin/Max: s she fights to stay up, dying
		float         dyingTime_{ 3.0f };
		bool          dieAtOnce_{ true };  // [Deaths] bDieAtOnce: the killing blow kills; she stumbles as a ragdoll pinned to Delirium
		std::unordered_map<std::string, Rule> rules_;  // [Reactions], over the built-in table
		float         heavyOomph_{ 0.5f };  // [Reactions] fHeavyOomph: from this oomph a blow is heavy
		std::unordered_map<RE::FormID, Held> held_;      // deaths held back (under lock_)
		std::unordered_set<std::string>      ragdollDumps_;  // limit sets already written by DumpRagdoll
		std::unordered_set<RE::FormID>       heldDied_;  // killed after a hold: her death ragdoll gets no new shot or push
		bool          aboutJoint_{ true };   // [Ragdoll] bTurnAboutJoint: a muscle turns the limb about its joint (the spin and the swing that goes with it)
		bool          draugr_{ true }, skeletons_{ false }, falmer_{ false };  // [Creatures] bDraugr / bSkeletons / bFalmer: these creatures too (a person's bones)
		// Skyrim's joints' own friction, by joint data (it may be shared between ragdolls): the original and how many hold it
		std::unordered_map<RE::hkpConstraintData*, std::pair<float, int>> eased_;
		void EaseJoints(Active& a);
		void RestoreJoints(Active& a);
		bool          arrows_{ true };      // [Arrows] bShotReaction
		float         arrowPush_{ 55.0f };  // [Arrows] fPush: N s at the hit point
		bool          spells_{ true };      // [Spells] bShotReaction
		float         burnTime_{ 2.5f };    // [Spells] fBurnTime, s
		float         shockTime_{ 2.0f };   // [Spells] fShockTime, s
		bool          staggers_{ true };      // [Stagger] bReplace: Skyrim's staggers are played by Delirium
		float         staggerPush_{ 100.0f };  // [Stagger] fPush: N s for a full stagger (magnitude 1)
		float         staggerMin_{ 0.4f };     // [Stagger] fMinShare: the push's share at magnitude 0
		float         damageRef_{ 15.0f };     // [Hits] fDamageRef: a hit of this much damage pushes at 1x
		float         damageExp_{ 0.8f };      // [Hits] fExponent: twice the damage pushes 2^this as hard
		float         damageMin_{ 0.4f }, damageMax_{ 3.0f };  // [Hits] fScaleMin / fScaleMax
		float         oomphFrom_{ 1.0f }, oomphTo_{ 2.5f };  // [Hits] fOomphFrom / fOomphTo: damage scale for oomph 0 / 1
		float         oomphSpin_{ 2.0f };      // [Hits] fOomphSpin: the shot's twist x(1 + this x oomph)
		float         oomphPush_{ 1.0f };      // [Hits] fOomphPush: the push x(1 + this x oomph)
		float         staggerHitBoost_{ 0.5f };  // [Stagger] fHitBoost: a blow that staggers pushes x(1 + this x magnitude)
		float         staggerMaxPush_{ 200.0f };  // [Stagger] fMaxPush: a stagger's push at most on her feet (in place of the blow's or spell's cap)
		bool          bumps_{ true };          // [Bump] bReplace: a big bump (run into) is played by Delirium, not Skyrim's stumble
		float         bumpPush_{ 50.0f };      // [Bump] fPush: N s, along the bumper's run
		float         bumpRepeat_{ 1.0f };     // [Bump] fRepeatAfter: s of no contact before a bump pushes again
		bool          bodies_{ true };         // [Bodies] bCollide: standing NPCs' bodies are in Skyrim's world, a body that hits one is a blow
		float         bodySpeed_{ 2.5f };      // [Bodies] fSpeed: m/s a ragdolled body comes at her from which it is a blow
		float         stumbleSpeed_{ 0.8f };   // [Bodies] fStumbleSpeed: the same for a body Delirium has on its feet
		float         bodyPush_{ 12.0f };      // [Bodies] fPush: N s per m/s it comes at her
		float         bodyMaxPush_{ 160.0f };  // [Bodies] fMaxPush: N s at most
		float         bodyRepeat_{ 1.5f };     // [Bodies] fRepeatAfter: s before the same NPC is struck again
		std::mutex    meetLock_;               // struckAt_, lookedAt_: every actor's update, any thread
		std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> struckAt_, lookedAt_;  // her last blow from a body; her bodies last looked at
		std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> busyAt_;  // a Delirium body was last near her (her bodies stay in the world a while after)
		bool Busy(const RE::NiPoint3& a_at, float a_within);  // someone Delirium has (on their feet or down) is that near, game units
		std::atomic<bool> leaving_{ false };   // world::Leave is on its way (bCollide switched off)
		bool Upright(RE::Actor* a_actor);      // Delirium has her on her feet
		// the other NPCs' bodies near her, as Skyrim's world has them, into her Delirium physics (nm_agent_set_others):
		// on her feet she is stopped by them there. None while she is down (Havok has her) or with [Bodies] off.
		void Others(RE::Actor* a_actor, bool a_upright);
		void Strikes(RE::Actor* a_body, bool a_down);  // whom of the standing NPCs her moving body strikes this frame
		std::mutex    bumpLock_;               // bumpAt_: bumps come on any thread
		std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> bumpAt_;  // her last bump (any, pushed or not)
		bool          melee_{ true };           // [Melee] bReact: blows push her while Delirium has her
		bool          blasts_{ true };          // [Explosions] bPush: an explosion pushes everyone it catches
		float         blastPush_{ 90.0f };      // [Explosions] fPush: N s at its centre for fDamageRef damage
		float         blastFar_{ 0.4f };        // [Explosions] fFarShare: the push's share at its edge
		float         blastUp_{ 0.25f };        // [Explosions] fUp: the push's upward part (0 flat, 1 straight up)
		float         flySpeed_{ 3.0f };        // [Explosions] fFlySpeed: m/s she is thrown at, at the centre for fDamageRef damage
		bool          highFalls_{ true };       // [Falls] bHighFall: an NPC dropping off a ledge falls in Delirium
		float         highFallDrop_{ 3.0f };    // [Falls] fHighFallDrop: from this many m of drop
		std::vector<RE::FormID> noFallBody_;    // NPCs a ledge fall couldn't take (no body for the skeleton)
		float         highFallAt_{ 4.5f };      // [Explosions] fHighFallAt: from this m/s she is thrown far: Delirium's highFall
		float         tumble_{ 10.0f };         // [Explosions] fTumble: N s at the head per m/s thrown: she tumbles
		float         writheTime_{ 1.0f }, writheAdd_{ 1.2f };  // [Down] fWritheTime + fWritheAdd x oomph: s she writhes on the ground
		float         foetalChance_{ 0.3f };    // [Down] fFoetalChance: she curls up instead
		float         meleeOomphFrom_{ 1.3f }, meleeOomphTo_{ 4.0f };  // [Melee] fOomphFrom / fOomphTo: x a plain blow's push for oomph 0 / 1
		float         headWhip_{ 0.2f };        // [Melee] fHeadWhip: share of the push at the head too
		float         hitMaxPush_{ 150.0f };    // [Hits] fMaxPush: a projectile's push at most (past it, the behaviours carry it)
		float         pelvisShare_{ 0.0f };     // [Hits] fPelvisShare: share of a standing hit's push at the pelvis (catch steps, not folding)
		int           maxSteps_{ 10 };          // [Hits] iMaxSteps: catch steps a shot may take (Delirium's default 6)
		float         meleeMaxPush_{ 160.0f };  // [Melee] fMaxPush: a blow's push at most (past it, the behaviours carry it: one shove is a limp ragdoll)
		float         flyUp_{ 0.65f };          // [Explosions] fFlyUp: m/s up per m/s along
		float         flyMin_{ 2.0f }, flyMax_{ 9.0f };  // [Explosions] fFlyMin / fFlyMax: slower than this she only reels; the cap
		float         meleePush_{ 45.0f };      // [Melee] fPush: N s for a plain blow (iron sword, 15% of her health)
		float         meleePower_{ 2.2f }, meleeBash_{ 1.6f }, meleeSneak_{ 1.3f }, meleeBlocked_{ 0.5f };  // [Melee] fPower / fBash / fSneak / fBlocked
		float         meleeWeightRef_{ 9.0f };  // [Melee] fWeightRef: a weapon this heavy pushes at 1x (by the square root)
		float         meleeHeftMin_{ 0.6f }, meleeHeftMax_{ 1.7f };  // [Melee] fHeftMin / fHeftMax
		float         meleeSwing_{ 0.5f };      // [Melee] fSwing: how much a swing's sideways sweep bends the push
		float         meleeChop_{ 0.35f };      // [Melee] fChop: an overhead chop pushes this much down too
		float         flinchBelow_{ 55.0f };    // [Melee] fFlinchBelow: a lighter blow flinches, a heavier one steps
		float         channelLinger_{ 0.6f };  // [Spells] fChannelLinger: s a channel's reaction lasts after its last hit
		float         spellChance_[2]{ 0.5f, 0.8f };    // [Spells] fStartChance / fStartChanceHurt: a missile brings her in (full health, near death)
		float         channelChance_[2]{ 0.3f, 0.6f };  // [Spells] fChannelChance / fChannelChanceHurt: per second of channel, the same way
		std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> channelTick_;  // last channel tick on someone not in Delirium (under lock_)
		float StartChance(RE::Actor* a_target, const float (&a_chance)[2]) const;  // by her health: the more hurt, the likelier
		float         wounded_{ 0.2f };     // [Arrows] fWoundedBelow: under this share of health a hit takes her down
		float         colRadius_{ 600.0f }, colHeight_{ 300.0f };  // game units around the NPC
		std::size_t   colMaxTris_{ 40000 };
	};
}

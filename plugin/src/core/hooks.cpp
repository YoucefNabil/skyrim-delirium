// The core's own patches and event sink, put back when it unloads: UpdateActor3DPosition, projectile impacts,
// the hit event.
#include "shared.h"

namespace delirium
{
	// Skyrim's Actor::UpdateActor3DPosition (vtable slot 0x11F) puts the 3D root on the actor's position, heading and
	// controller tilt. Going ragdoll it moves the actor from her feet to the ragdoll's root bone and tilts it (50+ units,
	// 90+ deg) after we wrote the bones: every bone would follow for a frame, and bone physics (CBPC, SMP) whips.
	// Patched in the actor's own vtable by the core and put back when it unloads.
	namespace
	{
		using Update3DFn = void (*)(RE::Actor*);
		std::uintptr_t* g_vtSlot{};
		Update3DFn      g_origUpdate3D{};

		void Update3DHook(RE::Actor* a_actor)
		{
			g_origUpdate3D(a_actor);
			if (Engine::Busy()) {
				Engine::Get().AfterRootMove(a_actor);
			}
		}
		void PatchSlot(std::uintptr_t* a_slot, std::uintptr_t a_value)
		{
			DWORD old{};
			VirtualProtect(a_slot, sizeof(*a_slot), PAGE_EXECUTE_READWRITE, &old);
			*a_slot = a_value;
			VirtualProtect(a_slot, sizeof(*a_slot), old, &old);
		}
	}

	void HookUpdate3D(RE::Actor* a_actor)
	{
		if (g_vtSlot) {
			return;
		}
		auto* vt = *reinterpret_cast<std::uintptr_t**>(a_actor);
		g_vtSlot = vt + 0x11F;
		g_origUpdate3D = reinterpret_cast<Update3DFn>(*g_vtSlot);
		PatchSlot(g_vtSlot, reinterpret_cast<std::uintptr_t>(&Update3DHook));
		logger::info("hooked UpdateActor3DPosition (vtable {:X}, was {:X})", reinterpret_cast<std::uintptr_t>(vt),
			reinterpret_cast<std::uintptr_t>(g_origUpdate3D));
	}
	void UnhookUpdate3D()
	{
		if (g_vtSlot) {
			PatchSlot(g_vtSlot, reinterpret_cast<std::uintptr_t>(g_origUpdate3D));
			g_vtSlot = nullptr;
		}
	}


	// IDXGISwapChain::Present (vtable slot 8) of Skyrim's window: the end of a render frame (main thread, Skyrim counts
	// the next one right after it), where body/show.cpp hands the renderer the poses finished since. Patched by the core
	// like UpdateActor3DPosition and put back when it unloads.
	namespace
	{
		using PresentFn = HRESULT (*)(void*, UINT, UINT);
		std::uintptr_t* g_presentSlot{};
		PresentFn       g_origPresent{};

		HRESULT PresentHook(void* a_chain, UINT a_sync, UINT a_flags)
		{
			PublishShown();
			return g_origPresent(a_chain, a_sync, a_flags);
		}
	}

	void HookPresent()
	{
		if (g_presentSlot) {
			return;
		}
		auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
		auto* chain = renderer ? renderer->GetRuntimeData().renderWindows[0].swapChain : nullptr;
		if (!chain) {
			logger::warn("Present not hooked (no swap chain): her meshes draw each pose as soon as it is done");
			return;
		}
		auto* vt = *reinterpret_cast<std::uintptr_t**>(chain);
		g_presentSlot = vt + 8;
		g_origPresent = reinterpret_cast<PresentFn>(*g_presentSlot);
		PatchSlot(g_presentSlot, reinterpret_cast<std::uintptr_t>(&PresentHook));
		g_atPresent = true;
		logger::info("hooked Present (vtable {:X}, was {:X})", reinterpret_cast<std::uintptr_t>(vt), reinterpret_cast<std::uintptr_t>(g_origPresent));
	}
	void UnhookPresent()
	{
		if (!g_presentSlot) {
			return;
		}
		g_atPresent = false;
		if (*g_presentSlot == reinterpret_cast<std::uintptr_t>(&PresentHook)) {
			PatchSlot(g_presentSlot, reinterpret_cast<std::uintptr_t>(g_origPresent));
		} else {
			// another mod hooked Present over ours since and calls ours on: this copy of the core stays loaded (with no NPC
			// left, its hook only passes Present on)
			HMODULE self{};
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&PresentHook), &self);
			logger::warn("Present hooked over ours by another mod: this core stays loaded");
		}
		g_presentSlot = nullptr;
	}


	// Projectiles. AddImpact (vtable slot 0xBD) is where an arrow or a spell's missile or beam meets something: what,
	// where and how fast (the velocity comes in reversed for arrows; it is checked against the shooter). Skyrim's hit
	// (TESHitEvent) comes after it, without a point, and says what hit (bow, spell) and whether it landed (not blocked).
	// Flame projectiles (Flames, Frostbite, Sparks: a stream, hits every frame) are left out. Patched by the core like
	// UpdateActor3DPosition and put back when it unloads.
	namespace
	{
		using AddImpactFn = std::uintptr_t (*)(RE::Projectile*, RE::TESObjectREFR*, const RE::NiPoint3&, const RE::NiPoint3&, RE::hkpCollidable*, std::int32_t,
			std::uint32_t);
		struct Impact
		{
			RE::ObjectRefHandle                   target;
			RE::NiPoint3                          at, vel;  // game units, units/s
			bool                                  spell{};  // a missile or beam (else an arrow)
			std::chrono::steady_clock::time_point when;
		};
		constexpr std::array<const char*, 3> kProjNames{ "ArrowProjectile", "MissileProjectile", "BeamProjectile" };
		std::array<std::uintptr_t*, 3>       g_impactSlot{};
		std::array<AddImpactFn, 3>           g_origImpact{};
		std::mutex                           g_impactLock;
		std::vector<Impact>                  g_impacts;  // projectiles on actors, the last 2 s

		// what it returns passes through untouched (the game's may be a pointer)
		template <int K>
		std::uintptr_t ImpactHook(RE::Projectile* a_proj, RE::TESObjectREFR* a_ref, const RE::NiPoint3& a_at, const RE::NiPoint3& a_vel, RE::hkpCollidable* a_col,
			std::int32_t a_6, std::uint32_t a_7)
		{
			if (a_ref && a_ref->Is(RE::FormType::ActorCharacter)) {
				// trace: impacts on actors
				logger::info("{} impact: ref {:08X} at {:.0f} {:.0f} {:.0f}, velocity {:.0f} {:.0f} {:.0f}", kProjNames[K], a_ref->GetFormID(), a_at.x, a_at.y, a_at.z,
					a_vel.x, a_vel.y, a_vel.z);
				const auto       now = std::chrono::steady_clock::now();
				std::scoped_lock l(g_impactLock);
				std::erase_if(g_impacts, [&](const Impact& i) { return now - i.when > std::chrono::seconds(2); });
				g_impacts.push_back({ a_ref->GetHandle(), a_at, a_vel, K != 0, now });
			}
			return g_origImpact[K](a_proj, a_ref, a_at, a_vel, a_col, a_6, a_7);
		}
		constexpr std::array<AddImpactFn, 3> kImpactHooks{ &ImpactHook<0>, &ImpactHook<1>, &ImpactHook<2> };
	}

	bool RecentImpact(RE::TESObjectREFR* a_ref, float a_maxAge, RE::NiPoint3& a_at, RE::NiPoint3& a_vel, int a_spell)
	{
		const auto       now = std::chrono::steady_clock::now();
		std::scoped_lock l(g_impactLock);
		for (auto it = g_impacts.rbegin(); it != g_impacts.rend(); ++it) {
			if (std::chrono::duration<float>(now - it->when).count() <= a_maxAge && it->target.get().get() == a_ref &&
				(a_spell < 0 || it->spell == (a_spell == 1))) {
				a_at = it->at;
				a_vel = it->vel;
				return true;
			}
		}
		return false;
	}

	namespace
	{
		// a spell's element, by its damage effect's resistance or its keyword (a shout's fire breath has the keyword, not
		// always the resistance, and isn't Destruction); -1: not a destruction spell
		int Element(const RE::MagicItem* a_magic)
		{
			if (!a_magic) {
				return -1;
			}
			bool destruction = false;
			for (const auto* e : a_magic->effects) {
				const auto* m = e ? e->baseEffect : nullptr;
				if (!m) {
					continue;
				}
				switch (m->data.resistVariable) {
				case RE::ActorValue::kResistFire:
					return Engine::kFire;
				case RE::ActorValue::kResistFrost:
					return Engine::kFrost;
				case RE::ActorValue::kResistShock:
					return Engine::kShock;
				default:
					break;
				}
				if (m->HasKeywordString("MagicDamageFire")) {
					return Engine::kFire;
				}
				if (m->HasKeywordString("MagicDamageFrost")) {
					return Engine::kFrost;
				}
				if (m->HasKeywordString("MagicDamageShock")) {
					return Engine::kShock;
				}
				destruction |= m->data.associatedSkill == RE::ActorValue::kDestruction;
			}
			return destruction ? Engine::kMagic : -1;
		}

		std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> g_lastSpellHit;  // hit sink thread only

		bool Staggers(const RE::MagicItem* a_magic)
		{
			if (a_magic) {
				for (const auto* e : a_magic->effects) {
					if (e && e->baseEffect && e->baseEffect->HasArchetype(RE::EffectSetting::Archetype::kStagger)) {
						return true;
					}
				}
			}
			return false;
		}

		// a spell's strongest stagger effect (0: none). Unrelenting Force: Fus 0.75, Fus Ro 1.25, Fus Ro Dah 1.52 (vanilla;
		// mods raise them): from about 1.5 Skyrim knocks down, the one thing in a shout's data that says it throws
		float StaggerOf(const RE::MagicItem* a_magic)
		{
			float s = 0.0f;
			if (a_magic) {
				for (const auto* e : a_magic->effects) {
					if (e && e->baseEffect && e->baseEffect->HasArchetype(RE::EffectSetting::Archetype::kStagger)) {
						s = std::max(s, e->effectItem.magnitude);
					}
				}
			}
			return s;
		}

		// a voice power's words, 1-3: which of its shout's variations it is (Unrelenting Force: Fus 13E09, Fus Ro 13F39,
		// Fus Ro Dah 13F3A; dragons' and mods' shouts the same way). 0: no shout has it. The shouts read once (hit sink thread)
		int ShoutWord(const RE::MagicItem* a_magic)
		{
			static std::unordered_map<const RE::MagicItem*, int> words;
			static bool                                          read = false;
			if (!read) {
				read = true;
				if (auto* dh = RE::TESDataHandler::GetSingleton()) {
					for (auto* shout : dh->GetFormArray<RE::TESShout>()) {
						for (int i = 0; shout && i < static_cast<int>(RE::TESShout::VariationIDs::kTotal); ++i) {
							if (const auto* spell = shout->variations[i].spell) {
								words.emplace(spell, i + 1);
							}
						}
					}
				}
				logger::info("shouts: {} spells by word", words.size());
			}
			const auto it = words.find(a_magic);
			return it != words.end() ? it->second : 0;
		}

		// a spell's health damage (its harmful health effects' magnitudes; perks and resistances left out)
		float SpellDamage(const RE::MagicItem* a_magic)
		{
			float d = 0.0f;
			if (a_magic) {
				for (const auto* e : a_magic->effects) {
					const auto* m = e ? e->baseEffect : nullptr;
					if (m && m->data.primaryAV == RE::ActorValue::kHealth && m->IsDetrimental()) {
						d += e->effectItem.magnitude;
					}
				}
			}
			return d;
		}

		struct HitSink : RE::BSTEventSink<RE::TESHitEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_event, RE::BSTEventSource<RE::TESHitEvent>*) override
			{
				// (its projectile is 0 in this setup: what hit is known by the source, a bow or a spell, and the impact)
				if (a_event && a_event->target) {
					// trace: every hit
					auto* sf = RE::TESForm::LookupByID(a_event->source);
					logger::info("hit: target {:08X} cause {:08X} source {:08X} ('{}', type {}) projectile {:08X} flags {:X}", a_event->target->GetFormID(),
						a_event->cause ? a_event->cause->GetFormID() : 0, a_event->source, sf ? sf->GetName() : "", sf ? static_cast<int>(sf->GetFormType()) : -1,
						a_event->projectile, static_cast<int>(a_event->flags.underlying()));
				}
				if (!a_event || !a_event->target || a_event->flags.any(RE::TESHitEvent::Flag::kHitBlocked)) {
					return RE::BSEventNotifyControl::kContinue;
				}
				auto* target = a_event->target->As<RE::Actor>();
				if (!target || target->IsPlayerRef()) {
					return RE::BSEventNotifyControl::kContinue;
				}
				auto* src = RE::TESForm::LookupByID(a_event->source);
				// her last hit, for a spell or shout (a weapon's came already with its HitData); an explosion's own hit
				// (no damage) keeps the spell's that set it off
				if (auto* cause = a_event->cause ? a_event->cause->As<RE::Actor>() : nullptr; cause && !(src && src->GetFormType() == RE::FormType::Explosion)) {
					const RE::MagicItem* magic = src ? src->As<RE::MagicItem>() : nullptr;
					if (auto* w = src ? src->As<RE::TESObjectWEAP>() : nullptr; w && w->IsStaff()) {
						magic = w->formEnchanting;
					}
					LastHit h;
					h.when = std::chrono::steady_clock::now();
					h.from = cause->GetHandle();
					h.damage = SpellDamage(magic);
					h.element = Element(magic);
					h.shout = magic && magic->GetSpellType() == RE::MagicSystem::SpellType::kVoicePower;
					h.word = h.shout ? ShoutWord(magic) : 0;
					h.stagger = h.shout ? StaggerOf(magic) : 0.0f;
					RecordHit(target->GetFormID(), h);
				}
				int  kind = -1;
				bool channel = false;  // a concentration spell: hits as long as it is held
				bool staggers = false;  // a spell with a stagger effect: always brings her in, like any stagger
				bool breath = false;    // a shout's fire / frost / shock: a cone, no missile to wait for
				if (auto* w = src ? src->As<RE::TESObjectWEAP>() : nullptr) {
					if (w->IsBow() || w->IsCrossbow()) {
						kind = Engine::kArrow;
					} else if (w->IsStaff()) {
						kind = Element(w->formEnchanting);  // a staff: its spell is its enchantment
						channel = w->formEnchanting && w->formEnchanting->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;
						staggers = Staggers(w->formEnchanting);
					}
				} else if (auto* m = src ? src->As<RE::MagicItem>() : nullptr) {
					kind = Element(m);
					channel = m->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;
					staggers = Staggers(m);
					breath = m->GetSpellType() == RE::MagicSystem::SpellType::kVoicePower &&
					         (kind == Engine::kFire || kind == Engine::kFrost || kind == Engine::kShock);
				}
				if (kind < 0) {
					return RE::BSEventNotifyControl::kContinue;  // a melee hit, a heal, a ward, ...
				}
				const bool spell = kind != Engine::kArrow;
				if (spell && !channel) {
					// a spell sends one hit per effect, all at once: one reaction
					const auto now = std::chrono::steady_clock::now();
					auto&      last = g_lastSpellHit[target->GetFormID()];
					if (now - last < std::chrono::milliseconds(300)) {
						return RE::BSEventNotifyControl::kContinue;
					}
					last = now;
				}
				// a spell's hit comes before its missile's impact is recorded: look for it a frame later
				g_tasks->AddTask([h = target->GetHandle(), c = a_event->cause ? a_event->cause->GetHandle() : RE::ObjectRefHandle{}, kind, spell, channel, staggers, breath]() {
					auto t = h.get();
					if (!t) {
						return;
					}
					if (channel) {
						RE::NiPoint3 away;
						if (auto cause = c.get()) {
							away = t->GetPosition() - cause->GetPosition();
							away.z = 0.0f;
						}
						Engine::Get().OnHit(t.get(), t->GetPosition(), away, false, kind, true, staggers);
						return;
					}
					RE::NiPoint3 at, vel;
					const bool   exact = RecentImpact(t.get(), 0.5f, at, vel, spell ? 1 : 0);
					logger::info("  hit kind {}, impact {}", kind, exact);  // trace
					if (spell && !exact && !breath) {
						return;  // a spell without a missile or beam: a stream, a cloak, a rune
					}
					if (!exact) {
						at = t->GetPosition();  // a breath: on her, from the shouter (below)
					}
					if (auto cause = c.get()) {
						// the way it flew must be away from the shooter (arrows: reversed); a missile's is only a unit vector, a beam's may be nothing
						const RE::NiPoint3 away = t->GetPosition() - cause->GetPosition();
						if (!exact || vel.Length() < 10.0f) {
							vel = { away.x, away.y, 0.0f };
							const float l = vel.Length();
							vel = l > 1.0f ? vel * (3000.0f / l) : RE::NiPoint3(0.0f, 3000.0f, 0.0f);
						} else if (vel.x * away.x + vel.y * away.y < 0.0f) {
							vel = -vel;
						}
					} else if (!exact) {
						return;
					}
					Engine::Get().OnHit(t.get(), at, vel, exact, kind, false, staggers);
				});
				return RE::BSEventNotifyControl::kContinue;
			}
		} g_hitSink;
		bool g_hitSinkOn = false;
	}

	// The animation event handlers that loose an arrow, fire a spell, staff or shout, or swing a weapon (slot 1,
	// ExecuteHandler): for an NPC held upright in Delirium they do nothing (body/hold.cpp)
	namespace
	{
		using HandlerFn = bool (*)(void*, RE::Actor&, const RE::BSFixedString&);
		constexpr std::size_t kGates = 6;
		std::array<std::uintptr_t*, kGates> g_gateSlot{};
		std::array<HandlerFn, kGates>       g_origGate{};

		template <std::size_t K>
		bool GateHook(void* a_this, RE::Actor& a_actor, const RE::BSFixedString& a_event)
		{
			if (HoldsCombat(&a_actor)) {
				return true;
			}
			return g_origGate[K](a_this, a_actor, a_event);
		}
		constexpr std::array<HandlerFn, kGates> kGateHooks{ &GateHook<0>, &GateHook<1>, &GateHook<2>, &GateHook<3>, &GateHook<4>, &GateHook<5> };

		void HookGates()
		{
			// arrow release, left/right hand spell fire (staffs too), voice (shouts), right/left weapon swing
			const std::array<REL::Relocation<std::uintptr_t>, kGates> vts{ REL::Relocation<std::uintptr_t>{ RE::VTABLE_ArrowReleaseHandler[0] },
				REL::Relocation<std::uintptr_t>{ RE::VTABLE_LeftHandSpellFireHandler[0] }, REL::Relocation<std::uintptr_t>{ RE::VTABLE_RightHandSpellFireHandler[0] },
				REL::Relocation<std::uintptr_t>{ RE::VTABLE_VoiceSpellFireHandler[0] }, REL::Relocation<std::uintptr_t>{ RE::VTABLE_WeaponRightSwingHandler[0] },
				REL::Relocation<std::uintptr_t>{ RE::VTABLE_WeaponLeftSwingHandler[0] } };
			for (std::size_t k = 0; k < kGates; ++k) {
				if (g_gateSlot[k]) {
					continue;
				}
				g_gateSlot[k] = reinterpret_cast<std::uintptr_t*>(vts[k].address()) + 1;
				g_origGate[k] = reinterpret_cast<HandlerFn>(*g_gateSlot[k]);
				PatchSlot(g_gateSlot[k], reinterpret_cast<std::uintptr_t>(kGateHooks[k]));
			}
			logger::info("gated {} attack/cast handlers for held NPCs", kGates);
		}
		void UnhookGates()
		{
			for (std::size_t k = 0; k < kGates; ++k) {
				if (g_gateSlot[k]) {
					PatchSlot(g_gateSlot[k], reinterpret_cast<std::uintptr_t>(g_origGate[k]));
					g_gateSlot[k] = nullptr;
				}
			}
		}
	}

	void HookArrows()
	{
		HookGates();
		const std::array<REL::Relocation<std::uintptr_t>, 3> vts{ REL::Relocation<std::uintptr_t>{ RE::VTABLE_ArrowProjectile[0] },
			REL::Relocation<std::uintptr_t>{ RE::VTABLE_MissileProjectile[0] }, REL::Relocation<std::uintptr_t>{ RE::VTABLE_BeamProjectile[0] } };
		for (std::size_t k = 0; k < 3; ++k) {
			if (g_impactSlot[k]) {
				continue;
			}
			g_impactSlot[k] = reinterpret_cast<std::uintptr_t*>(vts[k].address()) + 0xBD;
			g_origImpact[k] = reinterpret_cast<AddImpactFn>(*g_impactSlot[k]);
			PatchSlot(g_impactSlot[k], reinterpret_cast<std::uintptr_t>(kImpactHooks[k]));
			logger::info("hooked {}::AddImpact (vtable {:X}, was {:X})", kProjNames[k], vts[k].address(), reinterpret_cast<std::uintptr_t>(g_origImpact[k]));
		}
		if (auto* events = RE::ScriptEventSourceHolder::GetSingleton(); events && !g_hitSinkOn) {
			events->AddEventSink<RE::TESHitEvent>(&g_hitSink);
			g_hitSinkOn = true;
		}
	}
	void UnhookArrows()
	{
		UnhookGates();
		if (g_hitSinkOn) {
			if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) {
				events->RemoveEventSink<RE::TESHitEvent>(&g_hitSink);
			}
			g_hitSinkOn = false;
		}
		for (std::size_t k = 0; k < 3; ++k) {
			if (g_impactSlot[k]) {
				PatchSlot(g_impactSlot[k], reinterpret_cast<std::uintptr_t>(g_origImpact[k]));
				g_impactSlot[k] = nullptr;
			}
		}
		std::scoped_lock l(g_impactLock);
		g_impacts.clear();
	}
}

// Start-up (Delirium's DLL, the character, the rig), shut-down, the loading screen.
#include "shared.h"

#include "body/world.h"

namespace delirium
{
	namespace
	{
		std::filesystem::path PluginDir() { return g_pluginDir; }  // from the host: the core runs from a temporary copy

		bool LoadApi()
		{
			// loaded from a copy beside the core's own temporary copy: the mod's file is never locked, so a rebuilt
			// Delirium goes in with a hot reload like the core
			auto            path = PluginDir() / L"SkyrimDelirium" / L"delirium_physics.dll";
			HMODULE         self{};
			wchar_t         own[MAX_PATH]{};
			std::error_code ec;
			if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCWSTR>(&LoadApi), &self) &&
				GetModuleFileNameW(self, own, MAX_PATH)) {
				const auto copy = std::filesystem::path(own).parent_path() / L"delirium_physics.dll";
				if (CopyFileW(path.c_str(), copy.c_str(), FALSE)) {
					path = copy;
				}
			}
			const HMODULE h = LoadLibraryW(path.c_str());
			logger::info("Delirium from {}", path.string());
			if (!h) {
				logger::error("can't load {} (error {})", path.string(), GetLastError());
				return false;
			}
			bool ok = true;
#define DL_LOAD(f)                                                          \
	dl.f = reinterpret_cast<decltype(dl.f)>(GetProcAddress(h, #f));         \
	if (!dl.f) {                                                            \
		logger::error("delirium_physics.dll has no " #f " (old build?)"); \
		ok = false;                                                         \
	}
			DL_FNS(DL_LOAD)
#undef DL_LOAD
			return ok;
		}

		std::vector<float> Floats(const std::string& s)
		{
			std::vector<float> v;
			std::istringstream in(s);
			for (float f; in >> f;) {
				v.push_back(f);
			}
			return v;
		}

		// The loading screen (a save loaded, a door): every NPC is let go. Their 3D is rebuilt after it, and an entry
		// left over kept a reloaded NPC on the ground (KeepsDown) with nothing moving her.
		struct LoadSink : RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (a_event && a_event->menuName == RE::LoadingMenu::MENU_NAME) {
					if (a_event->opening) {
						Engine::Get().ReleaseAll("loading screen");
					} else {
						Engine::Get().NoKillMoves();
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		} g_loadSink;
		bool g_loadSinkOn = false;
	}

	Engine& Engine::Get()
	{
		static Engine e;
		return e;
	}

	bool Engine::Init()
	{
		if (ready_) {
			return true;
		}
		if (!LoadApi()) {
			return false;
		}
		// the body (parts, masses, shapes) is always the mod's own character; its brain (joints, muscles, zero pose) is
		// GTA IV's Fred.art when the player points sGTAFile at their own copy and it exists, else the character's own
		std::ifstream cf(buildPath_, std::ios::binary);
		const std::string text((std::istreambuf_iterator<char>(cf)), std::istreambuf_iterator<char>());
		if (!cf) {
			logger::error("can't read character {}", buildPath_);
			return false;
		}
		std::error_code ec;
		const bool gta = !fredPath_.empty() && std::filesystem::is_regular_file(fredPath_, ec);
		if (!fredPath_.empty() && !gta) {
			logger::warn("GTA IV file {} not found: using the mod's own character", fredPath_);
		}
		if (gta ? dl.nm_init(fredPath_.c_str()) < 0 : dl.nm_init_character_file(buildPath_.c_str()) < 0) {
			logger::error("character {}: {}", gta ? fredPath_ : buildPath_, LastError());
			return false;
		}
		usingGta_ = gta;
		dl.nm_set_task_param("NMShot_Standing", "*", "shotMaxSteps", static_cast<float>(maxSteps_));
		build_ =dl.nm_build_from_character(text.c_str());
		if (!build_) {
			logger::error("body {}: {}", buildPath_, LastError());
			return false;
		}
		std::ifstream in(rigPath_);
		if (!in) {
			logger::error("can't read rig {}", rigPath_);
			return false;
		}
		std::string line;
		std::size_t n = 0;
		while (std::getline(in, line)) {
			if (line.rfind("yaw ", 0) == 0) {
				rigYaw_ = std::stof(line.substr(4));
				continue;
			}
			std::vector<std::string> f;
			for (std::size_t s = 0, e; s <= line.size(); s = e + 1) {
				e = line.find('|', s);
				if (e == std::string::npos) {
					e = line.size();
				}
				f.push_back(line.substr(s, e - s));
			}
			if (f.size() != 8 || n >= rig_.size()) {
				continue;
			}
			const auto q = Floats(f[5]);
			const auto a = Floats(f[6]);
			const auto p = Floats(f[7]);
			if (q.size() != 4 || a.size() != 3 || p.size() != 3) {
				continue;
			}
			rig_[n++] = { f[0], f[1], f[3], f[4], f[2] == "1", { q[0], q[1], q[2], q[3] }, { a[0], a[1], a[2] }, { p[0], p[1], p[2] } };
		}
		if (n != rig_.size()) {
			logger::error("rig {}: {} parts, want 21", rigPath_, n);
			return false;
		}
		ready_ = true;
		if (auto* ui = RE::UI::GetSingleton(); ui && !g_loadSinkOn) {
			ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_loadSink);
			g_loadSinkOn = true;
		}
		HookArrows();
		HookPresent();
		NoKillMoves();
		logger::info("ready: {} on body {}, rig {} (build faces {:.2} rad)", usingGta_ ? fredPath_ : std::string("own character"), buildPath_, rigPath_, rigYaw_);
		return true;
	}

	void Engine::NoKillMoves()
	{
		// Skyrim.esm's KillMove global (the engine checks it before any paired kill), the kill cam's odds, and the
		// VATS-style kill camera. Saved games keep the global: `set killmove to 1` in the console brings them back
		std::string done;
		if (noKillMoves_) {
			if (auto* g = RE::TESForm::LookupByID<RE::TESGlobal>(0x100F19)) {
				done += fmt::format(" KillMove {:.0f} -> 0;", g->value);
				g->value = 0.0f;
			} else {
				done += " KillMove global not found;";
			}
		}
		if (noKillCams_) {
			if (auto* gs = RE::GameSettingCollection::GetSingleton(); gs) {
				if (auto* st = gs->GetSetting("fKillCamBaseOdds")) {
					done += fmt::format(" fKillCamBaseOdds {:.2f} -> 0;", st->data.f);
					st->data.f = 0.0f;
				} else {
					done += " fKillCamBaseOdds not found;";
				}
			}
			if (auto* is = RE::INISettingCollection::GetSingleton(); is) {
				if (auto* st = is->GetSetting("bVATSDisable:VATS")) {
					done += fmt::format(" bVATSDisable {} -> 1;", st->data.b);
					st->data.b = true;
				} else {
					done += " bVATSDisable not found;";
				}
			}
		}
		if (!done.empty()) {
			logger::info("deaths are Delirium's:{}", done);
		}
	}

	void Engine::ApplyChanged(std::span<const std::string> a_names)
	{
		// most settings are read as they are used; these were applied once at start-up
		const auto has = [&](std::string_view n) { return std::find(a_names.begin(), a_names.end(), n) != a_names.end(); };
		if (!ready_) {
			return;
		}
		std::scoped_lock l(lock_);  // the menu calls this on the render thread
		if (has("Hits.iMaxSteps")) {
			dl.nm_set_task_param("NMShot_Standing", "*", "shotMaxSteps", static_cast<float>(maxSteps_));
		}
		if ((has("Deaths.bNoKillMoves") && noKillMoves_) || (has("Deaths.bNoKillCams") && noKillCams_)) {
			NoKillMoves();  // turned off again only by a new game start (a save keeps the KillMove global)
		}
	}

	void Engine::ReleaseAll(const char* a_why)
	{
		KillHeld(a_why);
		std::scoped_lock l(lock_);
		heldDied_.clear();
		while (!active_.empty()) {
			Release(active_.size() - 1, a_why);
		}
		skip_.clear();
		g_skip = 0;
	}

	void Engine::Shutdown()
	{
		if (auto* ui = RE::UI::GetSingleton(); ui && g_loadSinkOn) {
			ui->RemoveEventSink<RE::MenuOpenCloseEvent>(&g_loadSink);
			g_loadSinkOn = false;
		}
		UnhookArrows();
		KillHeld("core unloading");
		std::scoped_lock l(lock_);
		while (!active_.empty()) {
			Release(active_.size() - 1, "core unloading");
		}
		world::Leave();
		UnhookUpdate3D();
		UnhookPresent();
		if (build_) {
			dl.nm_build_destroy(build_);
			build_ = nullptr;
		}
		ready_ = false;
		logger::info("core shut down");
	}

	bool Engine::Busy() { return g_count.load(std::memory_order_relaxed) != 0; }

	bool Engine::KeepsDown(RE::Actor* a_actor)
	{
		std::scoped_lock l(lock_);
		for (auto& p : active_) {
			if (p->actor.get().get() == a_actor) {
				// fallen and not yet settled: Delirium owns the body, Skyrim must not start its get-up
				return p->knockQueued;
			}
		}
		return false;
	}

}

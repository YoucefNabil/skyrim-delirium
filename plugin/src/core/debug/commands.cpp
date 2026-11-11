// The command file (tools\hotreload.ps1 -Cmd) and the test keys (F7 push, F2 release).
#include "shared.h"
#include "settings/table.h"
#include "body/world.h"

namespace delirium
{
	// Command file lines (written by tools, run on the main thread):
	//   key <hex DIK> [formid]   as if the key was pressed, aimed at that NPC (else crosshair / console pick)
	//   push <hex formid> [N s]  push that NPC (optionally with this impulse)
	//   release                  let go of every NPC
	//   shot <hex formid> [N s] [hurt] [fire|frost|shock]  an arrow (or spell) from the player into that NPC's chest (the reaction only, no damage)
	//   reset <hex formid> [dist] stand that NPC in front of the player, facing him
	//   spells                   give the player test spells: Flames, Frostbite, Sparks (channels), Firebolt, Fireball,
	//                            Ice Spike, Icy Spear, Ice Storm, Lightning Bolt, Chain Lightning
	//   fus <hex formid> [force] Skyrim's own push from the player (as Fus Ro Dah: force 15), no Delirium involved
	//   spawn <hex base> [dist]  place that NPC / leveled NPC in front of the player (3DECD = a melee bandit)
	//   undead [dist]            place a draugr, a skeleton and a falmer side by side in front of the player
	//   npcs <word>              log the non-unique NPCs whose race editor ID contains the word (bases for spawn)
	//   near                     log the NPCs near the player
	//   bodies add|remove|log <hex formid>  her ragdoll bodies into / out of Skyrim's Havok world while she stands; what is in it
	//   throw <hex thrown> <hex target> [force]  stand the first NPC before the second, Skyrim's push into her (15), log what their bodies did
	//   nodes <hex formid>       log every node of that NPC's 3D with its address (Cheat Engine breakpoints)
	//   show on|off              her meshes read the finished pose (default) / the live bones (before/after check)
	//   status                   log what is active
	//   settings reload|save|defaults   SkyrimDelirium.ini again / the live settings into it / every preset setting at default
	//   preset load|save <name>  Presets\<name>.ini
	//   set <Section.key> <value> one setting, as the menu sets it (logged old -> new)

	void Engine::Command(std::string_view a_line)
	{
		std::vector<std::string> w;
		for (std::size_t s = 0, e; s < a_line.size(); s = e + 1) {
			e = a_line.find(' ', s);
			e = e == std::string_view::npos ? a_line.size() : e;
			if (e > s) {
				w.emplace_back(a_line.substr(s, e - s));
			}
		}
		if (w.empty()) {
			return;
		}
		logger::info("command: {}", a_line);
		auto actor = [&](std::size_t i) -> RE::Actor* {
			if (i >= w.size()) {
				return nullptr;
			}
			auto* a = RE::TESForm::LookupByID<RE::Actor>(static_cast<RE::FormID>(std::stoul(w[i], nullptr, 16)));
			if (!a) {
				logger::warn("command: no actor {}", w[i]);
			}
			return a;
		};
		try {
			if (w[0] == "key" && w.size() >= 2) {
				OnKey(static_cast<std::uint32_t>(std::stoul(w[1], nullptr, 16)), actor(2));
			} else if (w[0] == "push" && w.size() >= 2) {
				const float keep = push_;
				if (w.size() >= 3) {
					push_ = std::stof(w[2]);
				}
				if (auto* a = actor(1)) {
					OnKey(pushKey_, a);
				}
				push_ = keep;
			} else if (w[0] == "spells") {
				auto* pc = RE::PlayerCharacter::GetSingleton();
				for (const RE::FormID id : { 0x012FCDu, 0x02B96Bu, 0x02DD2Au, 0x012FD0u, 0x01C789u, 0x02B96Cu, 0x10F7ECu, 0x045F9Cu, 0x02DD29u, 0x045F9Du }) {
					if (auto* s = RE::TESForm::LookupByID<RE::SpellItem>(id)) {
						pc->AddSpell(s);
						logger::info("  spell {:08X} '{}'", id, s->GetName());
					} else {
						logger::warn("  no spell {:08X}", id);
					}
				}
			} else if (w[0] == "shot" && w.size() >= 2) {
				if (auto* a = actor(1)) {
					const float keep = arrowPush_, keepWounded = wounded_;
					if (w.size() >= 3) {
						arrowPush_ = std::stof(w[2]);
					}
					int kind = kArrow;
					for (std::size_t k = 3; k < w.size(); ++k) {
						if (w[k] == "hurt") {
							wounded_ = 1.01f;  // as if under the wounded threshold
						} else if (w[k] == "fire") {
							kind = kFire;
						} else if (w[k] == "frost") {
							kind = kFrost;
						} else if (w[k] == "shock") {
							kind = kShock;
						}
					}
					RE::NiPoint3 at = a->GetPosition();
					at.z += 90.0f;  // chest height
					RE::NiPoint3 from = RE::PlayerCharacter::GetSingleton()->GetPosition();
					from.z += 110.0f;
					const RE::NiPoint3 v = at - from;
					const float        l = v.Length();
					OnHit(a, at, l > 1.0f ? v * (3000.0f / l) : RE::NiPoint3(0.0f, 3000.0f, 0.0f), true, kind, false, true);
					arrowPush_ = keep;
					wounded_ = keepWounded;
				}
			} else if (w[0] == "release") {
				OnKey(releaseKey_);
			} else if (w[0] == "reset") {
				// stand the NPC 150 units in front of the player, facing the player (a push sends her away from him)
				if (auto* a = actor(1)) {
					{
						std::scoped_lock l(lock_);
						for (std::size_t i = active_.size(); i-- > 0;) {
							if (active_[i]->actor.get().get() == a) {
								Release(i, "reset");
							}
						}
					}
					auto*       player = RE::PlayerCharacter::GetSingleton();
					const float h = player->GetAngleZ();
					const float dist = w.size() >= 3 ? std::stof(w[2]) : 150.0f;
					auto        pos = player->GetPosition() + RE::NiPoint3(std::sin(h), std::cos(h), 0.0f) * dist;
					pos.z += 5.0f;
					a->SetPosition(pos, true);
					a->SetAngle({ 0.0f, 0.0f, h + 3.14159265f });
					logger::info("reset: {:08X} at ({:.0f} {:.0f} {:.0f}), ragdoll {}", a->GetFormID(), pos.x, pos.y, pos.z, a->IsInRagdollState());
				}
			} else if (w[0] == "fus" && w.size() >= 2) {
				if (auto* a = actor(1)) {
					const float force = w.size() >= 3 ? std::stof(w[2]) : 15.0f;
					g_tasks->AddTask([h = a->GetHandle(), force]() {
						auto* player = RE::PlayerCharacter::GetSingleton();
						if (auto act = h.get(); act && player && act->Get3D(false) && !act->IsDisabled()) {
							if (auto* proc = act->GetActorRuntimeData().currentProcess) {
								proc->KnockExplosion(act.get(), player->GetPosition(), force);
							}
						}
					});
				}
			} else if (w[0] == "spawn" && w.size() >= 2) {
				// place a base object (an NPC or leveled NPC) in front of the player
				auto* player = RE::PlayerCharacter::GetSingleton();
				auto* base = RE::TESForm::LookupByID<RE::TESBoundObject>(static_cast<RE::FormID>(std::stoul(std::string(w[1]), nullptr, 16)));
				if (player && base) {
					if (auto ref = player->PlaceObjectAtMe(base, false)) {
						const float h = player->GetAngleZ();
						const float dist = w.size() >= 3 ? std::stof(std::string(w[2])) : 300.0f;
						auto        pos = player->GetPosition() + RE::NiPoint3(std::sin(h), std::cos(h), 0.0f) * dist;
						pos.z += 5.0f;
						ref->SetPosition(pos);
						ref->SetAngle({ 0.0f, 0.0f, h + 3.14159265f });
						logger::info("spawn: {:08X} '{}' (base {:08X}) at ({:.0f} {:.0f} {:.0f})", ref->GetFormID(), ref->GetName(), base->GetFormID(), pos.x, pos.y,
							pos.z);
					}
				} else {
					logger::info("spawn: no base object {}", w[1]);
				}
			} else if (w[0] == "undead") {
				// one draugr, one skeleton, one falmer side by side in front of the player: the first non-unique named NPC
				// of each race (race editor ID: exact, else ending in it as mods rename them (TDC_DraugrRace), else containing the word)
				auto* player = RE::PlayerCharacter::GetSingleton();
				auto* data = RE::TESDataHandler::GetSingleton();
				const float dist = w.size() >= 2 ? std::stof(std::string(w[1])) : 300.0f;
				const char* races[] = { "DraugrRace", "SkeletonRace", "FalmerRace" };
				const char* words[] = { "draugr", "skeleton", "falmer" };
				auto lower = [](std::string s) {
					std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
					return s;
				};
				for (int k = 0; k < 3 && player && data; ++k) {
					RE::TESNPC* pick = nullptr;
					for (const int pass : { 0, 1, 2 }) {
						for (auto* npc : data->GetFormArray<RE::TESNPC>()) {
							auto* race = npc ? npc->GetRace() : nullptr;
							if (!race || npc->IsUnique() || !npc->GetName() || !*npc->GetName()) {
								continue;
							}
							const std::string id = race->GetFormEditorID() ? race->GetFormEditorID() : "";
							const std::string_view r = races[k];
							const bool ok = pass == 0 ? id == r : pass == 1 ? id.ends_with(r) : lower(id).find(words[k]) != std::string::npos;
							if (ok) {
								pick = npc;
								break;
							}
						}
						if (pick) {
							break;
						}
					}
					if (!pick) {
						logger::warn("undead: no {} NPC found", words[k]);
						continue;
					}
					if (auto ref = player->PlaceObjectAtMe(pick, false)) {
						const float h = player->GetAngleZ();
						const float side = (k - 1) * 150.0f;  // left, middle, right
						auto pos = player->GetPosition() + RE::NiPoint3(std::sin(h), std::cos(h), 0.0f) * dist +
						           RE::NiPoint3(std::cos(h), -std::sin(h), 0.0f) * side;
						pos.z += 5.0f;
						ref->SetPosition(pos);
						ref->SetAngle({ 0.0f, 0.0f, h + 3.14159265f });
						logger::info("undead: {:08X} '{}' (base {:08X}, race {}) at ({:.0f} {:.0f} {:.0f})", ref->GetFormID(), ref->GetName(), pick->GetFormID(),
							pick->GetRace()->GetFormEditorID(), pos.x, pos.y, pos.z);
					}
				}
			} else if (w[0] == "npcs" && w.size() >= 2) {
				// every non-unique named NPC whose race editor ID contains the word: base id, name, race id, race editor ID
				std::string want = w[1];
				std::ranges::transform(want, want.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				int n = 0;
				for (auto* npc : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESNPC>()) {
					auto* race = npc ? npc->GetRace() : nullptr;
					if (!race || npc->IsUnique() || !npc->GetName() || !*npc->GetName()) {
						continue;
					}
					std::string id = race->GetFormEditorID() ? race->GetFormEditorID() : "";
					std::string low = id;
					std::ranges::transform(low, low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
					if (low.find(want) != std::string::npos && n++ < 200) {
						logger::info("npcs: {:08X} '{}' race {:08X} {}", npc->GetFormID(), npc->GetName(), race->GetFormID(), id);
					}
				}
				logger::info("npcs: {} with '{}' in the race", n, want);
			} else if (w[0] == "gain" && w.size() >= 2) {
				gain_ = std::clamp(std::stof(std::string(w[1])), 0.0f, 10.0f);
				logger::info("torque gain {:.2f}", gain_);
			} else if (w[0] == "damp" && w.size() >= 2) {
				damp_ = std::clamp(std::stof(std::string(w[1])), 0.0f, 1.0f);
				logger::info("joint damping {:.2f}", damp_);
			} else if (w[0] == "bodies" && w.size() >= 3) {
				if (auto* a = actor(2)) {
					if (w[1] == "add") {
						world::Collide();
						world::Add(a, true);
					} else if (w[1] == "remove") {
						logger::info("world: {:08X} ragdoll {} Skyrim's world", a->GetFormID(), world::Remove(a) ? "out of" : "was not in");
					}
					world::Log(a);
				}
			} else if (w[0] == "throw" && w.size() >= 3) {
				auto* a = actor(1);
				auto* b = actor(2);
				if (a && b) {
					Throw(a, b, w.size() >= 4 ? std::stof(w[3]) : 15.0f);
				}
			} else if (w[0] == "near") {
				// NPCs near the player: id, name, distance
				auto* player = RE::PlayerCharacter::GetSingleton();
				for (auto& h : RE::ProcessLists::GetSingleton()->highActorHandles) {
					if (auto a = h.get(); a && a.get() != player) {
						auto* race = a->GetRace();
						logger::info("near: {:08X} '{}' {:.0f} units, ragdoll {}, dead {}, race {} flags {:08X}{}", a->GetFormID(), a->GetName(),
							a->GetPosition().GetDistance(player->GetPosition()), a->IsInRagdollState(), a->IsDead(), race ? race->GetFormEditorID() : "?",
							race ? race->data.flags.underlying() : 0u,
							race && race->data.flags.any(RE::RACE_DATA::Flag::kNoKnockdowns) ? " (no knockdowns)" : "");
					}
				}
			} else if (w[0] == "show" && w.size() >= 2) {
				// her meshes read the finished pose (on, the default) or the live bones (off: as before body/show.cpp)
				show_ = w[1] != "off";
				logger::info("show: her meshes read {}", show_ ? "the finished pose" : "the live bones");
			} else if (w[0] == "nodes" && w.size() >= 2) {
				// every node of that NPC's 3D with its address (for Cheat Engine write breakpoints on world, +0x7C)
				if (auto* a = actor(1); a && a->Get3D(false)) {
					auto walk = [](auto& self, RE::NiAVObject* o, int depth) -> void {
						const auto& t = o->world.translate;
						logger::info("nodes: {:>2} {:016X} {}'{}' ({:.0f} {:.0f} {:.0f}){}", depth, reinterpret_cast<std::uintptr_t>(o), std::string(depth, ' '),
							o->name.c_str(), t.x, t.y, t.z, o->AsGeometry() ? " geometry" : "");
						if (auto* n = o->AsNode()) {
							for (auto& c : n->GetChildren()) {
								if (c) {
									self(self, c.get(), depth + 1);
								}
							}
						}
					};
					walk(walk, a->Get3D(false), 0);
				}
			} else if (w[0] == "status") {
				std::scoped_lock l(lock_);
				logger::info("status: ready {}, {} active", ready_, active_.size());
				for (const auto& a : active_) {
					auto act = a->actor.get();
					logger::info("  {:08X}: age {:.1f} s, knocked {}, in ragdoll {}", act ? act->GetFormID() : 0, a->age, a->knockQueued,
						act && act->IsInRagdollState());
				}
			} else if (w[0] == "settings" && w.size() >= 2) {
				std::string msg;
				if (w[1] == "reload") {
					ReloadSettings("read again (command)");
				} else if (w[1] == "save") {
					SaveIni(msg);
				} else if (w[1] == "defaults") {
					DefaultSettings("the defaults (command)");
				}
			} else if (w[0] == "preset" && w.size() >= 3) {
				std::string name = w[2], msg;
				for (std::size_t k = 3; k < w.size(); ++k) {
					name += " " + w[k];
				}
				if (w[1] == "load") {
					LoadPreset(name, msg);
				} else if (w[1] == "save") {
					SavePreset(name, msg);
				}
			} else if (w[0] == "set" && w.size() >= 3) {
				bool found = false;
				for (const auto& row : SettingsTable()) {
					if (_stricmp(settings::Name(row).c_str(), w[1].c_str()) != 0) {
						continue;
					}
					found = true;
					// the value through an ini line, so it reads exactly as the ini would read it
					CSimpleIniA one;
					const std::string line = std::format("[{}]\n{}={}\n", row.section, row.key, a_line.substr(a_line.find(w[2], a_line.find(w[1]) + w[1].size())));
					one.LoadData(line.data(), line.size());
					const auto was = settings::Text(row);
					SetSetting(row, settings::Parse(one, row));
					NoteEdit(settings::Name(row), was, settings::Text(row));
				}
				if (!found) {
					logger::warn("set: no setting {}", w[1]);
				}
			} else {
				logger::warn("command: unknown '{}'", w[0]);
			}
		} catch (const std::exception& e) {
			logger::warn("command '{}': {}", a_line, e.what());
		}
	}

	void Engine::OnKey(std::uint32_t a_key, RE::Actor* a_target)
	{
		if (!ready_ || (a_key != pushKey_ && a_key != releaseKey_)) {
			return;
		}
		RE::Actor* target = a_target;
		if (auto* pick = target ? nullptr : RE::CrosshairPickData::GetSingleton()) {
			if (auto ref = pick->GetActiveTarget().get()) {
				target = ref->As<RE::Actor>();
			}
		}
		if (!target) {
			if (auto ref = RE::Console::GetSelectedRef()) {
				target = ref->As<RE::Actor>();
			}
		}
		std::scoped_lock l(lock_);
		if (a_key == releaseKey_) {
			for (std::size_t i = active_.size(); i-- > 0;) {
				Release(i, "release key");
			}
			return;
		}
		if (!target || target->IsPlayerRef() || target->IsDead()) {
			logger::info("push key: no living NPC under the crosshair (or selected in the console)");
			return;
		}
		for (auto& a : active_) {
			if (a->actor.get().get() == target) {
				Push(*a, target);
				return;
			}
		}
		if (Activate(target)) {
			Push(*active_.back(), target);
		}
	}

	void Engine::Push(Active& a_act, RE::Actor* a_actor)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto  d = a_actor->GetPosition() - player->GetPosition();
		d.z = 0;
		const float len = std::max(d.Length(), 1.0f);
		float chest[3]{}, q[4]{};
		dl.nm_agent_get_part_transform(a_act.agent, kChest, chest, q);
		const float imp[3]{ push_ * d.x / len, push_ * d.y / len, 0.0f };
		dl.nm_agent_apply_push(a_act.agent, chest, imp);
		a_act.calm = 0;
		a_act.sincePush = 0;  // not age: SmoothRoot and the logs time themselves by age
		a_act.blend = 1.0f;  // a new push mid fade-out takes the body back fully
		a_act.blendingOut = false;
		logger::info("{:08X}: push {} N s", a_actor->GetFormID(), push_);
	}

}

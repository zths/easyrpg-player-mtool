/*
 * MTool online translation integration. See mtool_rpc.h.
 */

#include "mtool_rpc.h"

#if defined(PLAYER_WITH_MTOOL) && defined(HAVE_NLOHMANN_JSON)

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <lcf/data.h>
#include <lcf/lmu/reader.h>
#include <lcf/reader_util.h>

#include "mtoolsocket_transport.h"
#include "text_rpc_runtime.h"
#include "LRUCache.hpp"

#include "filefinder.h"
#include "font.h"
#include "game_actor.h"
#include "game_actors.h"
#include "game_battle.h"
#include "game_clock.h"
#include "game_enemyparty.h"
#include "game_enemy.h"
#include "game_event.h"
#include "game_map.h"
#include "game_party.h"
#include "game_player.h"
#include "game_switches.h"
#include "game_system.h"
#include "game_variables.h"
#include "main_data.h"
#include "map_data.h"
#include "mtool_extract.h"
#include "output.h"
#include "player.h"
#include "scene.h"
#include "scene_debug.h"
#include "scene_equip.h"
#include "scene_item.h"
#include "scene_load.h"
#include "scene_menu.h"
#include "scene_save.h"
#include "scene_settings.h"
#include "scene_skill.h"
#include "scene_status.h"
#include "scene_title.h"
#include "teleport_target.h"
#include "utils.h"

#ifdef _WIN32
#  include <windows.h>
#  include "key_event_bridge.h"
#else
#  include <unistd.h>
#endif

namespace {

std::unique_ptr<loader_common::ws::MToolSocketTransport> g_transport;
loader_common::ws::TextRpcRuntime g_rpc;
mtool::TranslationCache g_cache(10, 500, 50);

std::atomic<bool> g_enabled{false};
std::atomic<bool> g_trsLoaded{false};
std::atomic<bool> g_wordWrap{true};

// Circuit breaker: after several consecutive RPC failures, pause online
// lookups for a cooldown period so map loads don't stall on timeouts.
std::atomic<int> g_failCount{0};
std::atomic<long long> g_breakerUntilMs{0};
constexpr int kMaxFailures = 3;
constexpr long long kBreakerCooldownMs = 10000;
constexpr long kTrsTimeoutMs = 500;

std::string CurrentDirectory() {
#ifdef _WIN32
	DWORD len = GetCurrentDirectoryW(0, nullptr);
	if (len == 0) {
		return "";
	}
	std::wstring buf(len, L'\0');
	len = GetCurrentDirectoryW(len, &buf[0]);
	if (len == 0) {
		return "";
	}
	buf.resize(len);
	return Utils::FromWideString(buf);
#else
	char buf[4096];
	if (!getcwd(buf, sizeof(buf))) {
		return "";
	}
	return buf;
#endif
}

long long NowMs() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

/*
 * 字体请求由传输线程收下，实际替换要等主线程的 Update：FreeType 与 Bitmap 都不是线程安全的。
 */
std::mutex g_fontMutex;
std::string g_fontPath;
int g_fontSizeOffset = 0;
bool g_fontDirty = false;

/** 用户配置里的原始字号，offset 始终以它为基准，避免反复设置时累积。 */
int g_baseFontSize = 0;

const nlohmann::json* PickArg(const nlohmann::json& args, const char* key, size_t index);

/*
 * 游戏状态命令（getState/setGold/...）只能在主线程读写 Game_* 单例。
 * 传输线程把命令排进队列，MtoolRpc::Update 每帧取走执行并回填结果。
 */
struct PendingCmd {
	std::string cmd;
	nlohmann::json args;
	std::string result;
	bool done = false;
};
std::mutex g_cmdMutex;
std::condition_variable g_cmdCv;
std::deque<std::shared_ptr<PendingCmd>> g_cmdQueue;
constexpr long kGameCmdTimeoutMs = 2000;

// 锁血/锁蓝：每帧把低于锁定值的 HP/SP 写回去（MV 侧是 hook，这里退化成轮询）
// lockMax 时锁定值取每个角色自己的最大值
std::atomic<bool> g_lockHp{false};
std::atomic<int> g_lockHpVal{1};
std::atomic<bool> g_lockHpMax{true};
std::atomic<bool> g_lockMp{false};
std::atomic<int> g_lockMpVal{1};
std::atomic<bool> g_lockMpMax{true};

// through 目标状态；玩家每步移动会自行重算 through（见 Game_Player::UpdateMove），
// 外部设置无效，只能靠 MtoolRpc::ForceThrough 在那一步参与判定
std::atomic<bool> g_through{false};

// 随机遇敌抑制，在 Game_Player::UpdateEncounterSteps 的调试早退处判定
std::atomic<bool> g_noEncounter{false};

// applyTrs 时若处在标题界面需要重建场景刷新译文（窗口文字是缓存位图，不会自动重绘）
std::atomic<bool> g_reloadTitle{false};

// 失焦暂停的等待循环里不做场景操作，reloadTitle 延迟到回焦
std::atomic<bool> g_focusWait{false};

#ifdef _WIN32
/*
 * 工具的全局快捷键要游戏窗口的按键：子类化 SDL 窗口的 WndProc，
 * 按键事件经 key_event_bridge 原样推给工具（fire-and-forget，不阻塞消息流）。
 */
bool g_keyDownStatus[256] = {};
WNDPROC g_oldWndProc = nullptr;

LRESULT CALLBACK MtoolKeyEvWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
	switch (uMsg) {
		case WM_KEYDOWN:
		case WM_KEYUP:
		case WM_SYSKEYDOWN:
		case WM_SYSKEYUP: {
			int keyCode = (int)wParam & 0xff;
			bool keyUp = uMsg == WM_KEYUP || uMsg == WM_SYSKEYUP;
			bool altDown = uMsg == WM_SYSKEYDOWN || uMsg == WM_SYSKEYUP || wParam == VK_MENU;
			int scanCode = (lParam >> 16) & 0xff;
			const auto dispatch = loader_common::ws::buildKeyEventDispatch(
				g_keyDownStatus, keyCode, keyUp, altDown, scanCode);
			try {
				loader_common::ws::dispatchKeyEvent(dispatch,
					[](const std::string& cmd, const nlohmann::json& args, long) {
						if (g_transport && g_transport->IsConnected()) {
							g_rpc.doTask(cmd, args, -1);
						}
					}, -1);
			} catch (const std::exception&) {
			}
			break;
		}
	}
	return CallWindowProc(g_oldWndProc, hwnd, uMsg, wParam, lParam);
}

void InstallKeyEvHook() {
	if (g_oldWndProc) {
		return;
	}
	// 在主线程（MtoolRpc::Update）枚举本线程的顶层窗口，SDL 窗口就在这上面
	HWND hwnd = nullptr;
	EnumThreadWindows(GetCurrentThreadId(), [](HWND h, LPARAM p) -> BOOL {
		if (IsWindowVisible(h) && GetWindow(h, GW_OWNER) == nullptr) {
			*reinterpret_cast<HWND*>(p) = h;
			return FALSE;
		}
		return TRUE;
	}, reinterpret_cast<LPARAM>(&hwnd));
	if (!hwnd) {
		return;
	}
	WNDPROC cur = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
	if (cur == MtoolKeyEvWndProc) {
		return;
	}
	g_oldWndProc = cur;
	SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(MtoolKeyEvWndProc));
}
#endif

// 加速倍率，Player::UpdateInput 每帧读取；1 表示不干预
std::atomic<double> g_speedFactor{1.0};

bool IsGameCommand(const std::string& cmd) {
	static const char* kCmds[] = {
		"getState", "setGold", "setThrough", "setAllowMenu", "setAllowSave",
		"setMoveSpeed", "recoverAll", "setPartyHp", "lockHp", "lockMp",
		"speedMulti", "teleport", "loadSave", "forceKill",
		"dumpSave", "loadSaveFile", "setEncounter", "pushScene",
		"getItemDb", "getItemCounts", "setItemCount",
		"getSwitchDb", "getVariableDb", "getSwitchVals", "getVariableVals",
		"setSwitch", "setVariable",
		"getActorDb", "getActors", "setActorName", "setActorTitle",
		"setActorLevel", "setActorExp", "setActorHp", "setActorSp",
		"setActorParam", "setActorSkill", "setActorState", "setActorInTeam",
		"getMapInfo", "getEventPages", "getCommonEvents", "getCommonEvent",
		"getMapList", "getMapData", "getMapPassable",
		"setEnemyHp", "battleEscape"
	};
	for (const char* c : kCmds) {
		if (cmd == c) {
			return true;
		}
	}
	return false;
}

bool JsonBool(const nlohmann::json& args, const char* key, size_t index, bool def) {
	const nlohmann::json* v = PickArg(args, key, index);
	if (!v) {
		return def;
	}
	if (v->is_boolean()) {
		return v->get<bool>();
	}
	if (v->is_number()) {
		return v->get<int>() != 0;
	}
	if (v->is_string()) {
		std::string s = v->get<std::string>();
		return !(s == "0" || s == "false" || s.empty());
	}
	return def;
}

int JsonInt(const nlohmann::json& args, const char* key, size_t index, int def) {
	const nlohmann::json* v = PickArg(args, key, index);
	if (!v) {
		return def;
	}
	if (v->is_number()) {
		return v->get<int>();
	}
	if (v->is_string()) {
		return std::atoi(v->get<std::string>().c_str());
	}
	return def;
}

bool InGame() {
	return Main_Data::game_party != nullptr;
}

/* 事件查看器：指令列表原始 JSON（代码名翻译表在工具侧） */
nlohmann::json CommandsToJson(const std::vector<lcf::rpg::EventCommand>& commands) {
	nlohmann::json arr = nlohmann::json::array();
	for (const auto& com : commands) {
		arr.push_back({
			{"code", com.code},
			{"indent", com.indent},
			{"string", lcf::ToString(com.string)},
			{"parameters", com.parameters}
		});
	}
	return arr;
}

nlohmann::json PageConditionToJson(const lcf::rpg::EventPageCondition& c) {
	return {
		{"switch1_on", c.flags.switch_a}, {"switch1_id", c.switch_a_id},
		{"switch2_on", c.flags.switch_b}, {"switch2_id", c.switch_b_id},
		{"variable_on", c.flags.variable}, {"variable_id", c.variable_id},
		{"variable_value", c.variable_value}, {"compare_operator", c.compare_operator},
		{"item_on", c.flags.item}, {"item_id", c.item_id},
		{"actor_on", c.flags.actor}, {"actor_id", c.actor_id},
		{"timer_on", c.flags.timer}, {"timer_sec", c.timer_sec},
		{"timer2_on", c.flags.timer2}, {"timer2_sec", c.timer2_sec}
	};
}

nlohmann::json EventPageToJson(const lcf::rpg::EventPage& page) {
	return {
		{"condition", PageConditionToJson(page.condition)},
		{"trigger", (int)page.trigger},
		{"commands", CommandsToJson(page.event_commands)}
	};
}

/*
 * 离线通行度：静态部分（芯片组通行标志 + 地图图层 + LMT 替换表）是
 * Game_Map::IsPassableTile 几何分支的纯函数，不依赖游戏运行状态，
 * 任意地图都能算。不含事件/载具——和工具侧"静态底图"的语义一致。
 */
std::string OfflineMapPassable(int mapId) {
	if (mapId < 1 || mapId > 9999) {
		return "BadArg";
	}
	auto is = FileFinder::Game().OpenInputStream(fmt::format("Map{:04d}.lmu", mapId));
	if (!is) {
		return "NoSuchMap";
	}
	auto map = lcf::LMU_Reader::Load(is, Player::GetEncoding());
	if (!map) {
		return "NoSuchMap";
	}
	// 瓦片替换表（SaveMapInfo::lower/upper_tiles）默认是恒等映射；
	// 离线没有存档状态，统一按恒等算，运行时的 TileSubstitution 改动不反映
	const auto* chipset = lcf::ReaderUtil::GetElement(lcf::Data::chipsets, map->chipset_id);
	if (!chipset) {
		return "NoSuchMap";
	}

	auto passagesDown = chipset->passable_data_lower;
	auto passagesUp = chipset->passable_data_upper;
	if (passagesDown.size() < 162) {
		passagesDown.resize(162, 0x0F);
	}
	if (passagesUp.size() < 144) {
		passagesUp.resize(144, 0x0F);
	}

	int w = map->width;
	int h = map->height;
	auto passAt = [&](const std::vector<unsigned char>& passages, int tileId, int bit) {
		return tileId >= 0 && tileId < (int)passages.size() && (passages[tileId] & bit) != 0;
	};
	auto lowerPassable = [&](int bit, int tileIndex) {
		// Game_Map::IsPassableLowerTile 的离线版（含自动瓦片"墙顶可走"规则）
		int tileRawId = map->lower_layer[tileIndex];
		int tileId = 0;
		if (tileRawId >= BLOCK_E) {
			tileId = (tileRawId - BLOCK_E) + BLOCK_E_INDEX;
		} else if (tileRawId >= BLOCK_D) {
			tileId = (tileRawId - BLOCK_D) / BLOCK_D_STRIDE + BLOCK_D_INDEX;
			int autotileId = (tileRawId - BLOCK_D) % BLOCK_D_STRIDE;
			if (passAt(passagesDown, tileId, Passable::Wall) && (
					(autotileId >= 20 && autotileId <= 23) ||
					(autotileId >= 33 && autotileId <= 37) ||
					autotileId == 42 || autotileId == 43 ||
					autotileId == 45 || autotileId == 46)) {
				return true;
			}
		} else if (tileRawId >= BLOCK_C) {
			tileId = (tileRawId - BLOCK_C) / BLOCK_C_STRIDE + BLOCK_C_INDEX;
		} else {
			tileId = tileRawId / BLOCK_B_STRIDE;
		}
		return passAt(passagesDown, tileId, bit);
	};

	nlohmann::json pass = nlohmann::json::array();
	for (int tileIndex = 0; tileIndex < w * h; ++tileIndex) {
		int bits = 0;
		for (int bit : {Passable::Down, Passable::Left, Passable::Right, Passable::Up}) {
			// 上层瓦片为空（小于 BLOCK_F）时直接落到下层判定
			int upperId = map->upper_layer[tileIndex] - BLOCK_F;
			bool ok;
			if (upperId < 0) {
				ok = lowerPassable(bit, tileIndex);
			} else if (!passAt(passagesUp, upperId, bit)) {
				ok = false;
			} else if (!passAt(passagesUp, upperId, Passable::Above)) {
				ok = true;
			} else {
				ok = lowerPassable(bit, tileIndex);
			}
			if (ok) {
				bits |= bit;
			}
		}
		pass.push_back(bits);
	}
	return nlohmann::json({{"w", w}, {"h", h}, {"pass", std::move(pass)}}).dump();
}

/* RPC 是文本通道，存档字节只能 base64 带过去 */
std::string Base64Encode(const std::string& data) {
	static const char* kTable = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	out.reserve((data.size() + 2) / 3 * 4);
	for (size_t i = 0; i < data.size(); i += 3) {
		uint32_t v = static_cast<uint8_t>(data[i]) << 16;
		if (i + 1 < data.size()) v |= static_cast<uint8_t>(data[i + 1]) << 8;
		if (i + 2 < data.size()) v |= static_cast<uint8_t>(data[i + 2]);
		out += kTable[(v >> 18) & 63];
		out += kTable[(v >> 12) & 63];
		out += i + 1 < data.size() ? kTable[(v >> 6) & 63] : '=';
		out += i + 2 < data.size() ? kTable[v & 63] : '=';
	}
	return out;
}

/* 全部跑在主线程（MtoolRpc::Update 里），可以直接碰 Game_* 单例 */
std::string HandleGameCommand(const std::string& cmd, const nlohmann::json& args) {
	if (cmd == "forceKill") {
		// Player::Exit 的优雅退出会卡在清理阶段；强杀就该是真的强杀
#ifdef _WIN32
		TerminateProcess(GetCurrentProcess(), 0);
#else
		std::_Exit(0);
#endif
		return "0";
	}
	if (cmd == "speedMulti") {
		double v = JsonInt(args, "value", 0, 1);
		if (v < 1) {
			v = 1;
		} else if (v > 100) {
			v = 100;
		}
		g_speedFactor = v;
		return "0";
	}
	if (cmd == "lockHp") {
		g_lockHpVal = JsonInt(args, "value", 1, 1);
		g_lockHpMax = JsonBool(args, "max", 2, false);
		g_lockHp = JsonBool(args, "enabled", 0, true);
		return "0";
	}
	if (cmd == "lockMp") {
		g_lockMpVal = JsonInt(args, "value", 1, 1);
		g_lockMpMax = JsonBool(args, "max", 2, false);
		g_lockMp = JsonBool(args, "enabled", 0, true);
		return "0";
	}

	if (!InGame()) {
		return "NotInGame";
	}

	if (cmd == "getState") {
		nlohmann::json state;
		state["gold"] = Main_Data::game_party->GetGold();
		state["allowMenu"] = Main_Data::game_system->GetAllowMenu();
		state["allowSave"] = Main_Data::game_system->GetAllowSave();
		state["lockHp"] = g_lockHp.load();
		state["lockHpVal"] = g_lockHpVal.load();
		state["lockHpMax"] = g_lockHpMax.load();
		state["lockMp"] = g_lockMp.load();
		state["lockMpVal"] = g_lockMpVal.load();
		state["lockMpMax"] = g_lockMpMax.load();
		state["speedFactor"] = g_speedFactor.load();
		state["through"] = g_through.load();
		state["encounter"] = !g_noEncounter.load();
		if (Main_Data::game_player) {
			state["mapId"] = Main_Data::game_player->GetMapId();
			state["x"] = Main_Data::game_player->GetX();
			state["y"] = Main_Data::game_player->GetY();
			state["moveSpeed"] = Main_Data::game_player->GetMoveSpeed();
		}
		nlohmann::json party = nlohmann::json::array();
		for (Game_Actor* actor : Main_Data::game_party->GetActors()) {
			nlohmann::json a;
			a["id"] = actor->GetId();
			a["name"] = std::string(actor->GetName());
			a["hp"] = actor->GetHp();
			a["maxHp"] = actor->GetMaxHp();
			a["sp"] = actor->GetSp();
			a["maxSp"] = actor->GetMaxSp();
			a["level"] = actor->GetLevel();
			party.push_back(std::move(a));
		}
		state["party"] = std::move(party);
		return state.dump();
	}
	if (cmd == "setGold") {
		int value = JsonInt(args, "value", 0, 0);
		int diff = value - Main_Data::game_party->GetGold();
		if (diff > 0) {
			Main_Data::game_party->GainGold(diff);
		} else if (diff < 0) {
			Main_Data::game_party->LoseGold(-diff);
		}
		return "0";
	}
	if (cmd == "setThrough") {
		// 只在移动判定点生效（MtoolRpc::ForceThrough），这里只记目标状态
		g_through = JsonBool(args, "enabled", 0, true);
		return "0";
	}
	if (cmd == "setAllowMenu") {
		Main_Data::game_system->SetAllowMenu(JsonBool(args, "enabled", 0, true));
		return "0";
	}
	if (cmd == "setAllowSave") {
		Main_Data::game_system->SetAllowSave(JsonBool(args, "enabled", 0, true));
		return "0";
	}
	if (cmd == "setEncounter") {
		g_noEncounter = !JsonBool(args, "enabled", 0, true);
		return "0";
	}
	if (cmd == "getItemDb") {
		// 静态库数据：工具侧只拉一次。RM2k 武器/防具/物品同库，type 区分
		nlohmann::json items = nlohmann::json::array();
		int id = 0;
		for (const auto& item : lcf::Data::items) {
			++id;
			if (item.name.empty()) {
				continue;
			}
			nlohmann::json it;
			it["id"] = id;
			it["name"] = ToString(item.name);
			it["desc"] = ToString(item.description);
			it["type"] = item.type;
			items.push_back(std::move(it));
		}
		return items.dump();
	}
	if (cmd == "getSwitchDb") {
		nlohmann::json arr = nlohmann::json::array();
		int id = 0;
		for (const auto& sw : lcf::Data::switches) {
			++id;
			arr.push_back({{"id", id}, {"name", ToString(sw.name)}});
		}
		return arr.dump();
	}
	if (cmd == "getVariableDb") {
		nlohmann::json arr = nlohmann::json::array();
		int id = 0;
		for (const auto& var : lcf::Data::variables) {
			++id;
			arr.push_back({{"id", id}, {"name", ToString(var.name)}});
		}
		return arr.dump();
	}
	if (cmd == "getSwitchVals") {
		// 轮询用：下标即 id（0 号位占位）
		nlohmann::json vals = nlohmann::json::array();
		vals.push_back(false);
		for (size_t id = 1; id <= lcf::Data::switches.size(); ++id) {
			vals.push_back(Main_Data::game_switches->Get((int)id));
		}
		return vals.dump();
	}
	if (cmd == "getVariableVals") {
		nlohmann::json vals = nlohmann::json::array();
		vals.push_back(0);
		for (size_t id = 1; id <= lcf::Data::variables.size(); ++id) {
			vals.push_back(Main_Data::game_variables->Get((int)id));
		}
		return vals.dump();
	}
	if (cmd == "setSwitch") {
		int id = JsonInt(args, "id", 0, 0);
		if (id < 1 || id > (int)lcf::Data::switches.size()) {
			return "BadArg";
		}
		Main_Data::game_switches->Set(id, JsonBool(args, "enabled", 1, true));
		return "0";
	}
	if (cmd == "setVariable") {
		int id = JsonInt(args, "id", 0, 0);
		if (id < 1 || id > (int)lcf::Data::variables.size()) {
			return "BadArg";
		}
		Main_Data::game_variables->Set(id, JsonInt(args, "value", 1, 0));
		return "0";
	}
	if (cmd == "getActorDb") {
		// 静态库数据：角色/技能/状态。RM2k 角色没有简介字段，昵称就是"称号"
		nlohmann::json db;
		nlohmann::json actors = nlohmann::json::array();
		int id = 0;
		for (const auto& actor : lcf::Data::actors) {
			++id;
			if (actor.name.empty()) {
				continue;
			}
			nlohmann::json a;
			a["id"] = id;
			a["name"] = ToString(actor.name);
			a["title"] = ToString(actor.title);
			a["classId"] = actor.class_id;
			const auto* cls = lcf::ReaderUtil::GetElement(lcf::Data::classes, actor.class_id);
			a["className"] = cls ? ToString(cls->name) : "";
			actors.push_back(std::move(a));
		}
		nlohmann::json skills = nlohmann::json::array();
		id = 0;
		for (const auto& skill : lcf::Data::skills) {
			++id;
			if (skill.name.empty()) {
				continue;
			}
			skills.push_back({{"id", id}, {"name", ToString(skill.name)}, {"desc", ToString(skill.description)}});
		}
		nlohmann::json states = nlohmann::json::array();
		id = 0;
		for (const auto& state : lcf::Data::states) {
			++id;
			if (state.name.empty()) {
				continue;
			}
			states.push_back({{"id", id}, {"name", ToString(state.name)}});
		}
		db["actors"] = std::move(actors);
		db["skills"] = std::move(skills);
		db["states"] = std::move(states);
		return db.dump();
	}
	if (cmd == "getActors") {
		nlohmann::json arr = nlohmann::json::array();
		int id = 0;
		for (const auto& dbActor : lcf::Data::actors) {
			++id;
			if (dbActor.name.empty()) {
				continue;
			}
			Game_Actor* actor = Main_Data::game_actors->GetActor(id);
			if (!actor) {
				continue;
			}
			nlohmann::json a;
			a["id"] = id;
			a["inTeam"] = Main_Data::game_party->IsActorInParty(id);
			a["name"] = std::string(actor->GetName());
			a["title"] = std::string(actor->GetTitle());
			a["level"] = actor->GetLevel();
			a["maxLevel"] = actor->GetMaxLevel();
			a["exp"] = actor->GetExp();
			a["nextExp"] = actor->GetLevel() < actor->GetMaxLevel()
				? actor->GetBaseExp(actor->GetLevel() + 1) : 0;
			a["hp"] = actor->GetHp();
			a["maxHp"] = actor->GetMaxHp();
			a["sp"] = actor->GetSp();
			a["maxSp"] = actor->GetMaxSp();
			a["base"] = {
				{"maxHp", actor->GetBaseMaxHp(false)}, {"maxSp", actor->GetBaseMaxSp(false)},
				{"atk", actor->GetBaseAtk(Game_Actor::WeaponAll, false, false)},
				{"def", actor->GetBaseDef(Game_Actor::WeaponAll, false, false)},
				{"spi", actor->GetBaseSpi(Game_Actor::WeaponAll, false, false)},
				{"agi", actor->GetBaseAgi(Game_Actor::WeaponAll, false, false)}
			};
			a["cur"] = {
				{"atk", actor->GetAtk()}, {"def", actor->GetDef()},
				{"spi", actor->GetSpi()}, {"agi", actor->GetAgi()}
			};
			a["skills"] = actor->GetSkills();
			a["states"] = actor->GetStates();
			arr.push_back(std::move(a));
		}
		return arr.dump();
	}
	if (cmd == "setActorName" || cmd == "setActorTitle") {
		Game_Actor* actor = Main_Data::game_actors->GetActor(JsonInt(args, "id", 0, 0));
		if (!actor) {
			return "BadArg";
		}
		const nlohmann::json* v = PickArg(args, "value", 1);
		if (!v || !v->is_string()) {
			return "BadArg";
		}
		if (cmd == "setActorName") {
			actor->SetName(v->get<std::string>());
		} else {
			actor->SetTitle(v->get<std::string>());
		}
		return "0";
	}
	if (cmd == "setActorLevel") {
		Game_Actor* actor = Main_Data::game_actors->GetActor(JsonInt(args, "id", 0, 0));
		if (!actor) {
			return "BadArg";
		}
		int level = Utils::Clamp(JsonInt(args, "value", 1, 1), 1, actor->GetMaxLevel());
		actor->ChangeLevel(level, nullptr);
		return "0";
	}
	if (cmd == "setActorExp") {
		Game_Actor* actor = Main_Data::game_actors->GetActor(JsonInt(args, "id", 0, 0));
		if (!actor) {
			return "BadArg";
		}
		actor->ChangeExp(std::max(0, JsonInt(args, "value", 1, 0)), nullptr);
		return "0";
	}
	if (cmd == "setActorHp" || cmd == "setActorSp") {
		Game_Actor* actor = Main_Data::game_actors->GetActor(JsonInt(args, "id", 0, 0));
		if (!actor) {
			return "BadArg";
		}
		if (cmd == "setActorHp") {
			actor->SetHp(JsonInt(args, "value", 1, 0));
		} else {
			actor->SetSp(JsonInt(args, "value", 1, 0));
		}
		return "0";
	}
	if (cmd == "setActorParam") {
		// 改的是基础值（对应 RPG_RT 的"增减能力值"事件），装备/状态加成保持不动
		Game_Actor* actor = Main_Data::game_actors->GetActor(JsonInt(args, "id", 0, 0));
		const nlohmann::json* field = PickArg(args, "field", 1);
		if (!actor || !field || !field->is_string()) {
			return "BadArg";
		}
		std::string f = field->get<std::string>();
		int val = JsonInt(args, "value", 2, 0);
		if (f == "maxHp") actor->SetBaseMaxHp(val);
		else if (f == "maxSp") actor->SetBaseMaxSp(val);
		else if (f == "atk") actor->SetBaseAtk(val);
		else if (f == "def") actor->SetBaseDef(val);
		else if (f == "spi") actor->SetBaseSpi(val);
		else if (f == "agi") actor->SetBaseAgi(val);
		else return "BadArg";
		return "0";
	}
	if (cmd == "setActorSkill") {
		Game_Actor* actor = Main_Data::game_actors->GetActor(JsonInt(args, "id", 0, 0));
		int skillId = JsonInt(args, "skill", 1, 0);
		if (!actor || skillId < 1 || skillId > (int)lcf::Data::skills.size()) {
			return "BadArg";
		}
		if (JsonBool(args, "has", 2, true)) {
			actor->LearnSkill(skillId, nullptr);
		} else {
			actor->UnlearnSkill(skillId);
		}
		return "0";
	}
	if (cmd == "setActorState") {
		Game_Actor* actor = Main_Data::game_actors->GetActor(JsonInt(args, "id", 0, 0));
		int stateId = JsonInt(args, "state", 1, 0);
		if (!actor || stateId < 1 || stateId > (int)lcf::Data::states.size()) {
			return "BadArg";
		}
		if (JsonBool(args, "has", 2, true)) {
			actor->AddState(stateId, true);
		} else {
			actor->RemoveState(stateId, true);
		}
		return "0";
	}
	if (cmd == "setActorInTeam") {
		int id = JsonInt(args, "id", 0, 0);
		if (id < 1 || id > (int)lcf::Data::actors.size()) {
			return "BadArg";
		}
		if (JsonBool(args, "inTeam", 1, true)) {
			Main_Data::game_party->AddActor(id);
		} else {
			Main_Data::game_party->RemoveActor(id);
		}
		return "0";
	}
	if (cmd == "getMapInfo") {
		// 没进游戏时 Game_Map::GetMap() 是空指针，事件遍历会崩
		if (!Main_Data::game_player || Main_Data::game_player->GetMapId() == 0) {
			return "NotInGame";
		}
		nlohmann::json info;
		const auto& mapInfo = Game_Map::GetMapInfo();
		info["mapId"] = Main_Data::game_player ? Main_Data::game_player->GetMapId() : 0;
		info["mapName"] = lcf::ToString(mapInfo.name);
		info["width"] = Game_Map::GetTilesX();
		info["height"] = Game_Map::GetTilesY();
		if (Main_Data::game_player) {
			info["player"] = {{"x", Main_Data::game_player->GetX()}, {"y", Main_Data::game_player->GetY()}};
		}
		nlohmann::json events = nlohmann::json::array();
		for (const auto& lcfEv : Game_Map::GetMap().events) {
			Game_Event* gev = Game_Map::GetEvent(lcfEv.ID);
			int activePage = 0;
			if (gev) {
				for (size_t i = 0; i < lcfEv.pages.size(); ++i) {
					if (gev->GetActivePage() == &lcfEv.pages[i]) {
						activePage = (int)i + 1;
						break;
					}
				}
			}
			events.push_back({
				{"id", (int)lcfEv.ID},
				{"name", lcf::ToString(lcfEv.name)},
				{"x", gev ? gev->GetX() : (int)lcfEv.x},
				{"y", gev ? gev->GetY() : (int)lcfEv.y},
				{"activePage", activePage},
				{"pageCount", (int)lcfEv.pages.size()}
			});
		}
		info["events"] = std::move(events);
		return info.dump();
	}
	if (cmd == "getEventPages") {
		int eventId = JsonInt(args, "id", 0, 0);
		for (const auto& ev : Game_Map::GetMap().events) {
			if (ev.ID == eventId) {
				nlohmann::json pages = nlohmann::json::array();
				for (const auto& page : ev.pages) {
					pages.push_back(EventPageToJson(page));
				}
				return nlohmann::json({
					{"id", ev.ID}, {"name", lcf::ToString(ev.name)}, {"pages", std::move(pages)}
				}).dump();
			}
		}
		return "NoSuchEvent";
	}
	if (cmd == "getCommonEvents") {
		nlohmann::json arr = nlohmann::json::array();
		int id = 0;
		for (const auto& ce : lcf::Data::commonevents) {
			++id;
			if (ce.name.empty()) {
				continue;
			}
			arr.push_back({
				{"id", id},
				{"name", lcf::ToString(ce.name)},
				{"trigger", ce.trigger},
				{"switchId", ce.switch_id},
				{"cmdCount", (int)ce.event_commands.size()}
			});
		}
		return arr.dump();
	}
	if (cmd == "getCommonEvent") {
		int id = JsonInt(args, "id", 0, 0);
		const auto* ce = lcf::ReaderUtil::GetElement(lcf::Data::commonevents, id);
		if (!ce) {
			return "NoSuchEvent";
		}
		return nlohmann::json({
			{"id", id},
			{"name", lcf::ToString(ce->name)},
			{"trigger", ce->trigger},
			{"switchId", ce->switch_id},
			{"commands", CommandsToJson(ce->event_commands)}
		}).dump();
	}
	if (cmd == "setEnemyHp") {
		if (!Game_Battle::IsBattleRunning() || !Main_Data::game_enemyparty) {
			return "NotInBattle";
		}
		int val = JsonInt(args, "value", 0, 1);
		for (Game_Enemy* enemy : Main_Data::game_enemyparty->GetEnemies()) {
			enemy->SetHp(val);
		}
		return "0";
	}
	if (cmd == "battleEscape") {
		// 伪造一条 ForceFlee 事件指令喂给战斗解释器，走正常逃跑流程。
		// 仅 2k3 指令集支持；2k 游戏退化为杀光敌人（胜利）由工具侧决定
		if (!Game_Battle::IsBattleRunning()) {
			return "NotInBattle";
		}
		if (!Player::IsRPG2k3Commands()) {
			return "NotSupported";
		}
		lcf::rpg::EventCommand com;
		com.code = (int)lcf::rpg::EventCommand::Code::ForceFlee;
		com.parameters = {0, 0, 0};
		Game_Battle::GetInterpreterBattle().ExecuteCommand(com);
		return "0";
	}
	if (cmd == "getMapList") {
		// LMT 树（数组序即编辑器里的显示序），deep 由工具侧按 parent 链算
		nlohmann::json arr = nlohmann::json::array();
		for (const auto& mi : lcf::Data::treemap.maps) {
			arr.push_back({
				{"id", mi.ID},
				{"name", lcf::ToString(mi.name)},
				{"parentId", mi.parent_map},
				{"type", mi.type}
			});
		}
		return arr.dump();
	}
	if (cmd == "getMapData") {
		int mapId = JsonInt(args, "id", 0, 0);
		if (mapId < 1 || mapId > 9999) {
			return "BadArg";
		}
		auto is = FileFinder::Game().OpenInputStream(fmt::format("Map{:04d}.lmu", mapId));
		if (!is) {
			return "NoSuchMap";
		}
		auto map = lcf::LMU_Reader::Load(is, Player::GetEncoding());
		if (!map) {
			return "NoSuchMap";
		}
		nlohmann::json events = nlohmann::json::array();
		for (const auto& ev : map->events) {
			nlohmann::json pages = nlohmann::json::array();
			for (const auto& page : ev.pages) {
				pages.push_back(EventPageToJson(page));
			}
			events.push_back({
				{"id", (int)ev.ID},
				{"name", lcf::ToString(ev.name)},
				{"x", (int)ev.x}, {"y", (int)ev.y},
				{"pages", std::move(pages)}
			});
		}
		return nlohmann::json({
			{"width", map->width}, {"height", map->height},
			{"chipsetId", map->chipset_id},
			{"events", std::move(events)}
		}).dump();
	}
	if (cmd == "getMapPassable") {
		int mapId = JsonInt(args, "id", 0, 0);
		return OfflineMapPassable(mapId);
	}
	if (cmd == "getItemCounts") {
		// 轮询用：下标即 id（0 号位占位）
		nlohmann::json counts = nlohmann::json::array();
		counts.push_back(0);
		for (size_t id = 1; id <= lcf::Data::items.size(); ++id) {
			counts.push_back(Main_Data::game_party->GetItemCount((int)id));
		}
		return counts.dump();
	}
	if (cmd == "setItemCount") {
		if (!InGame()) {
			return "NotInGame";
		}
		int id = JsonInt(args, "id", 0, 0);
		int count = JsonInt(args, "count", 1, 0);
		if (id < 1 || id > (int)lcf::Data::items.size()) {
			return "BadArg";
		}
		count = Utils::Clamp(count, 0, 99);
		int diff = count - Main_Data::game_party->GetItemCount(id);
		if (diff > 0) {
			Main_Data::game_party->AddItem(id, diff);
		} else if (diff < 0) {
			Main_Data::game_party->RemoveItem(id, -diff);
		}
		return "0";
	}
	if (cmd == "pushScene") {
		const nlohmann::json* nameArg = PickArg(args, "name", 0);
		if (!nameArg || !nameArg->is_string()) {
			return "BadArg";
		}
		std::string name = nameArg->get<std::string>();
		if (name == "title") {
			if (Scene::instance) {
				Scene::ReturnToTitleScene();
			}
			return "0";
		}
		if (name == "pop") {
			Scene::Pop();
			return "0";
		}
		if (!InGame()) {
			return "NotInGame";
		}
		std::shared_ptr<Scene> scene;
		auto actors = Main_Data::game_party->GetActors();
		if (name == "menu") {
			scene = std::make_shared<Scene_Menu>();
		} else if (name == "item") {
			scene = std::make_shared<Scene_Item>();
		} else if (name == "skill" && !actors.empty()) {
			scene = std::make_shared<Scene_Skill>(actors, 0);
		} else if (name == "equip" && !actors.empty()) {
			scene = std::make_shared<Scene_Equip>(actors, 0);
		} else if (name == "status" && !actors.empty()) {
			scene = std::make_shared<Scene_Status>(actors, 0);
		} else if (name == "save") {
			scene = std::make_shared<Scene_Save>();
		} else if (name == "load") {
			scene = std::make_shared<Scene_Load>();
		} else if (name == "settings") {
			scene = std::make_shared<Scene_Settings>();
		} else if (name == "debug") {
			scene = std::make_shared<Scene_Debug>();
		}
		if (!scene) {
			return "BadArg";
		}
		// 走延迟请求而不是直接 Push：当前场景可能在任意状态，
		// SetRequestedScene 会在本帧场景更新结束后安全切换
		if (Scene::instance) {
			Scene::instance->SetRequestedScene(scene);
		} else {
			Scene::Push(scene);
		}
		return "0";
	}
	if (cmd == "setMoveSpeed") {
		if (Main_Data::game_player) {
			int speed = Utils::Clamp(JsonInt(args, "value", 0, 4), 1, 6);
			Main_Data::game_player->SetMoveSpeed(speed);
		}
		return "0";
	}
	if (cmd == "recoverAll") {
		for (Game_Actor* actor : Main_Data::game_party->GetActors()) {
			actor->FullHeal();
		}
		return "0";
	}
	if (cmd == "setPartyHp") {
		int value = JsonInt(args, "value", 0, 1);
		for (Game_Actor* actor : Main_Data::game_party->GetActors()) {
			actor->SetHp(value);
		}
		return "0";
	}
	if (cmd == "teleport") {
		if (Main_Data::game_player) {
			int mapId = JsonInt(args, "mapId", 0, Main_Data::game_player->GetMapId());
			int x = JsonInt(args, "x", 1, Main_Data::game_player->GetX());
			int y = JsonInt(args, "y", 2, Main_Data::game_player->GetY());
			Main_Data::game_player->ReserveTeleport(mapId, x, y, -1, TeleportTarget::eForegroundTeleport);
		}
		return "0";
	}
	if (cmd == "loadSave") {
		int slot = JsonInt(args, "slot", 0, 1);
		if (slot < 1 || slot > 99) {
			return "BadSlot";
		}
		std::string saveName = FileFinder::Game().FindFile(fmt::format("Save{:02d}.lsd", slot));
		if (saveName.empty()) {
			return "NoSuchSave";
		}
		Player::LoadSavegame(saveName, slot);
		return "0";
	}
	if (cmd == "dumpSave") {
		// 序列化到内存流，不落任何槽位；slot 参数只影响存档头里的元数据
		std::ostringstream os(std::ios::binary);
		int slot = JsonInt(args, "slot", 0, 0);
		if (!Scene_Save::Save(os, slot)) {
			return "SaveFailed";
		}
		return Base64Encode(os.str());
	}
	if (cmd == "loadSaveFile") {
		// 读侧没有流式加载接口：工具先把备份写成游戏目录下的临时文件，这里按名加载
		const nlohmann::json* nameArg = PickArg(args, "file", 0);
		if (!nameArg || !nameArg->is_string()) {
			return "BadArg";
		}
		std::string name = nameArg->get<std::string>();
		// 防目录穿越：只接受纯文件名
		if (name.empty() || name.find_first_of("/\\:") != std::string::npos) {
			return "BadArg";
		}
		int slot = JsonInt(args, "slot", 1, 0);
		Player::LoadSavegame(name, slot);
		return "0";
	}
	return "NoSuchCommand";
}

void ApplyStatLocks() {
	if (!InGame()) {
		return;
	}
	if (g_lockHp) {
		for (Game_Actor* actor : Main_Data::game_party->GetActors()) {
			int val = g_lockHpMax ? actor->GetMaxHp() : g_lockHpVal.load();
			if (actor->GetHp() < val) {
				actor->SetHp(val);
			}
		}
	}
	if (g_lockMp) {
		for (Game_Actor* actor : Main_Data::game_party->GetActors()) {
			int val = g_lockMpMax ? actor->GetMaxSp() : g_lockMpVal.load();
			if (actor->GetSp() < val) {
				actor->SetSp(val);
			}
		}
	}
}

/**
 * 取 args 里的一个字段，兼容三种形态: 字符串/数字直给、数组 [path, offset]、
 * 对象 {"path": ..., "sizeOffset": ...}。MTool 各处发命令的参数形状并不统一。
 */
const nlohmann::json* PickArg(const nlohmann::json& args, const char* key, size_t index) {
	if (args.is_object()) {
		auto it = args.find(key);
		return it == args.end() ? nullptr : &*it;
	}
	if (args.is_array()) {
		return index < args.size() ? &args[index] : nullptr;
	}
	return index == 0 ? &args : nullptr;
}

std::string HandleCommand(const std::string& cmd, const nlohmann::json& args) {
	if (IsGameCommand(cmd)) {
		// 跑在传输线程上，游戏状态只能由主线程碰：排队等 Update 执行
		auto pending = std::make_shared<PendingCmd>();
		pending->cmd = cmd;
		pending->args = args;
		{
			std::lock_guard<std::mutex> lock(g_cmdMutex);
			g_cmdQueue.push_back(pending);
		}
		std::unique_lock<std::mutex> lock(g_cmdMutex);
		if (!g_cmdCv.wait_for(lock, std::chrono::milliseconds(kGameCmdTimeoutMs),
				[&] { return pending->done; })) {
			return "Timeout";
		}
		return pending->result;
	}
	if (cmd == "setFont") {
		const nlohmann::json* pathArg = PickArg(args, "path", 0);
		const nlohmann::json* offsetArg = PickArg(args, "sizeOffset", 1);
		std::lock_guard<std::mutex> lock(g_fontMutex);
		if (pathArg && pathArg->is_string()) {
			g_fontPath = pathArg->get<std::string>();
		}
		if (offsetArg) {
			if (offsetArg->is_number()) {
				g_fontSizeOffset = offsetArg->get<int>();
			} else if (offsetArg->is_string()) {
				g_fontSizeOffset = std::atoi(offsetArg->get<std::string>().c_str());
			}
		}
		g_fontDirty = true;
		return "0";
	}
	if (cmd == "whoareyou") {
		return "EasyRPG";
	}
	if (cmd == "cwd") {
		std::string path = FileFinder::GetFullFilesystemPath(FileFinder::Game());
		if (path.empty()) {
			// 没给 --project-path 时游戏文件系统的根就是进程工作目录，此时它的
			// base path 和 sub path 都是空串，只能自己去问系统。
			path = CurrentDirectory();
		}
		return path;
	}
	if (cmd == "setWordWrap") {
		const nlohmann::json* arg = PickArg(args, "enabled", 0);
		bool enabled = true;
		if (arg) {
			if (arg->is_boolean()) {
				enabled = arg->get<bool>();
			} else if (arg->is_number()) {
				enabled = arg->get<int>() != 0;
			} else if (arg->is_string()) {
				std::string v = arg->get<std::string>();
				enabled = !(v == "0" || v == "false" || v.empty());
			}
		}
		g_wordWrap = enabled;
		return "0";
	}
	if (cmd == "applyTrs") {
		g_cache.clear();
		g_trsLoaded = true;
		// 场景操作只能主线程做，转交 Update
		g_reloadTitle = true;
		return "0";
	}
	if (cmd == "unloadTrs") {
		g_cache.clear();
		g_trsLoaded = false;
		return "0";
	}
	if (cmd == "extractStrings") {
		// 注意：跑在传输线程上，不要在这里调 Output
		return MtoolExtract::ExtractAllJson();
	}
	return "NoSuchCommand";
}

void OnMessage(const std::string& message) {
	loader_common::ws::RpcIncomingEvent event;
	try {
		event = g_rpc.onIncomingText(message);
	} catch (const std::exception&) {
		return;
	}
	if (event.kind != loader_common::ws::RpcIncomingKind::Request) {
		return;
	}
	// Commands (extraction in particular) can be slow; never block the
	// transport receive loop.
	std::thread([request = event.request]() {
		std::string ret;
		try {
			ret = HandleCommand(request.cmd, request.args);
		} catch (const std::exception& e) {
			g_rpc.sendReply(request.id, loader_common::ws::errorReply(e.what()));
			return;
		}
		g_rpc.sendReply(request.id, loader_common::ws::textReply(ret));
	}).detach();
}

/*
 * 换字体走 player_config + Font::ResetDefault，而不是自己造 FTFont：那条路已经处理好了
 * gothic/mincho 两套默认字体和位图 fallback。字号被 RangeConfigParam 限制在 6..16。
 */
void ApplyFont(const std::string& path, int sizeOffset) {
	auto& cfg = Player::player_config;
	if (g_baseFontSize <= 0) {
		g_baseFontSize = cfg.font1_size.Get();
	}

	cfg.font1.SetLocked(false);
	cfg.font2.SetLocked(false);
	cfg.font1_size.SetLocked(false);
	cfg.font2_size.SetLocked(false);

	if (!path.empty()) {
		if (!cfg.font1.Set(path) || !cfg.font2.Set(path)) {
			Output::Warning("MTool: font {} was rejected", path);
			return;
		}
	}

	int size = Utils::Clamp(g_baseFontSize + sizeOffset, cfg.font1_size.GetMin(), cfg.font1_size.GetMax());
	cfg.font1_size.Set(size);
	cfg.font2_size.Set(size);

	Font::ResetDefault();
	Output::Debug("MTool: font set to {} at size {}", cfg.font1.Get(), cfg.font1_size.Get());
}

} // namespace

void MtoolRpc::Update() {
	if (!g_enabled) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock(g_cmdMutex);
		while (!g_cmdQueue.empty()) {
			auto pending = std::move(g_cmdQueue.front());
			g_cmdQueue.pop_front();
			try {
				pending->result = HandleGameCommand(pending->cmd, pending->args);
			} catch (const std::exception& e) {
				pending->result = std::string("Error: ") + e.what();
			}
			pending->done = true;
		}
	}
	g_cmdCv.notify_all();

	ApplyStatLocks();

	if (g_reloadTitle.load()) {
		if (g_focusWait.load()) {
			// 回焦后的第一个主循环 Update 再刷
		} else if (Scene::instance && Scene::instance->type == Scene::Title) {
			// 只重建命令窗口：选项文字重新过翻译钩子，indices 也顺带重置。
			// 不能换场景——Push 时机不对会破坏 MainFunction 的 init 交接
			g_reloadTitle = false;
			static_cast<Scene_Title*>(Scene::instance.get())->CreateCommandWindow();
		} else {
			g_reloadTitle = false;
		}
	}

#ifdef _WIN32
	InstallKeyEvHook();
#endif

	std::string path;
	int sizeOffset = 0;
	{
		std::lock_guard<std::mutex> lock(g_fontMutex);
		if (!g_fontDirty) {
			return;
		}
		g_fontDirty = false;
		path = g_fontPath;
		sizeOffset = g_fontSizeOffset;
	}
	ApplyFont(path, sizeOffset);
}

bool MtoolRpc::Enabled() {
	return g_enabled;
}

bool MtoolRpc::ForceWordWrap() {
	return g_enabled && g_trsLoaded && g_wordWrap;
}

double MtoolRpc::SpeedFactor() {
	return g_enabled ? g_speedFactor.load() : 1.0;
}

void MtoolRpc::SetFocusWait(bool waiting) {
	g_focusWait = waiting;
}

bool MtoolRpc::ForceThrough() {
	return g_enabled && g_through.load();
}

bool MtoolRpc::NoEncounter() {
	return g_enabled && g_noEncounter.load();
}

void MtoolRpc::InitFromEnv() {
	const char* portEnv = std::getenv("MTOOL_PORT");
	if (!portEnv || !*portEnv) {
		return;
	}
	std::string portStr = portEnv;
	// MTOOL_PORT may hold the path of MTool's listenPort file instead of the number
	// itself, so a restarted tool listening elsewhere keeps generated launchers valid.
	if (portStr.find_first_not_of("0123456789") != std::string::npos) {
		std::ifstream portFile(portStr);
		if (!portFile) {
			return;
		}
		portStr.clear();
		portFile >> portStr;
	}
	int port = std::atoi(portStr.c_str());
	if (port <= 0 || port > 65535) {
		return;
	}

	g_transport = std::make_unique<loader_common::ws::MToolSocketTransport>("127.0.0.1", port, "/");
	g_transport->SetMessageCallback([](const std::string& message) {
		OnMessage(message);
	});
	// 不要在传输线程上调用 Output（非线程安全）；错误静默重连即可
	g_transport->SetErrorCallback([](const std::string&) {
	});
	g_rpc.setTextSender([](const std::string& text) {
		return g_transport && g_transport->SendTextMessage(text);
	});
	g_transport->ConnectAsync();
	g_enabled = true;
	Output::Info("MTool online translation enabled, connecting to 127.0.0.1:{}", port);

	// 字体也能在启动时就定下来，不必等 MTool 连上后再发 setFont
	const char* fontEnv = std::getenv("MTOOL_FONT");
	const char* fontOffsetEnv = std::getenv("MTOOL_FONT_SIZE_OFFSET");
	if ((fontEnv && *fontEnv) || (fontOffsetEnv && *fontOffsetEnv)) {
		std::lock_guard<std::mutex> lock(g_fontMutex);
		if (fontEnv) {
			g_fontPath = fontEnv;
		}
		if (fontOffsetEnv) {
			g_fontSizeOffset = std::atoi(fontOffsetEnv);
		}
		g_fontDirty = true;
	}
}

std::string MtoolRpc::Translate(const std::string& text) {
	if (!g_enabled || !g_trsLoaded || text.empty()) {
		return "";
	}
	if (!g_transport || !g_transport->IsConnected()) {
		return "";
	}
	if (g_breakerUntilMs.load() > NowMs()) {
		return "";
	}

	std::string cached = g_cache.get(text);
	if (!cached.empty()) {
		return cached;
	}

	try {
		nlohmann::json reply = g_rpc.doTask("trs", nlohmann::json::array({text}), kTrsTimeoutMs);
		g_failCount = 0;
		if (reply.contains("ret") && reply["ret"].is_string()) {
			std::string translated = reply["ret"].get<std::string>();
			if (!translated.empty()) {
				g_cache.put(text, translated);
			}
			return translated;
		}
	} catch (const std::exception&) {
		if (++g_failCount >= kMaxFailures) {
			g_breakerUntilMs = NowMs() + kBreakerCooldownMs;
			g_failCount = 0;
			Output::Debug("MTool translate: too many failures, pausing online lookup");
		}
	}
	return "";
}

#else

bool MtoolRpc::Enabled() {
	return false;
}

void MtoolRpc::InitFromEnv() {
}

void MtoolRpc::Update() {
}

bool MtoolRpc::ForceWordWrap() {
	return false;
}

double MtoolRpc::SpeedFactor() {
	return 1.0;
}

void MtoolRpc::SetFocusWait(bool) {
}

bool MtoolRpc::ForceThrough() {
	return false;
}

bool MtoolRpc::NoEncounter() {
	return false;
}

std::string MtoolRpc::Translate(const std::string&) {
	return "";
}

#endif

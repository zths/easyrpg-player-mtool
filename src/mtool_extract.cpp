/*
 * In-player extraction of translatable strings. See mtool_extract.h.
 * Extraction logic is a port of LcfTrans (MIT licensed, (c) 2020 LcfTrans
 * authors), trimmed to MTool's needs: no PO serialization, no update/match
 * modes, results are deduplicated and returned as JSON.
 */

#include "mtool_extract.h"

#if defined(PLAYER_WITH_MTOOL) && defined(HAVE_NLOHMANN_JSON)

#include <algorithm>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <lcf/context.h>
#include <lcf/rpg/eventcommand.h>
#include <lcf/rpg/database.h>
#include <lcf/rpg/map.h>
#include <lcf/rpg/mapinfo.h>
#include <lcf/ldb/reader.h>
#include <lcf/lmu/reader.h>
#include <lcf/lmt/reader.h>
#include <lcf/span.h>
#include <nlohmann/json.hpp>

#include "filefinder.h"
#include "output.h"
#include "player.h"
#include "utils.h"

namespace {

using Cmd = lcf::rpg::EventCommand::Code;
constexpr int lines_per_message = 4;

template <typename T>
using any_event_ctx = const lcf::Context<lcf::rpg::EventCommand, T>;

template <typename T>
using map_event_ctx = const lcf::Context<lcf::rpg::EventCommand,
	lcf::Context<lcf::rpg::EventPage,
		lcf::Context<lcf::rpg::Event, T>>>;

template <typename T>
using common_event_ctx = const lcf::Context<lcf::rpg::EventCommand,
	lcf::Context<lcf::rpg::CommonEvent, T>>;

template <typename T>
using battle_event_ctx = lcf::Context<lcf::rpg::EventCommand,
	lcf::Context<lcf::rpg::TroopPage,
		lcf::Context<lcf::rpg::Troop, T>>>;

struct OutEntry {
	std::string context;
	std::string text;
	bool choice = false;
};

// RPG_RT ignores raw control characters inside messages; strip them so
// extracted strings match what the runtime lookup receives.
std::string RemoveControlChars(std::string_view s) {
	std::string out(s);
	auto iter = std::remove_if(out.begin(), out.end(), [](const char ch) {
		return (ch >= 0x0 && ch <= 0x1F) || ch == 0x7F;
	});
	out.erase(iter, out.end());
	return out;
}

std::string JoinLines(const std::vector<std::string>& lines) {
	std::string ret;
	for (const auto& s : lines) {
		ret += s + "\n";
	}
	if (!ret.empty()) {
		ret.pop_back();
	}
	return ret;
}

std::vector<std::string> GetChoices(lcf::Span<lcf::rpg::EventCommand> list, int start_index) {
	constexpr int max_num_choices = 4;

	int current_indent = list[start_index].indent;
	std::vector<std::string> s_choices;
	for (int index_temp = start_index; index_temp < static_cast<int>(list.size()); ++index_temp) {
		const auto& com = list[index_temp];
		if (com.indent != current_indent) {
			continue;
		}

		if (static_cast<Cmd>(com.code) == Cmd::ShowChoiceOption && com.parameters.size() > 0 && com.parameters[0] < max_num_choices) {
			s_choices.push_back(RemoveControlChars(com.string));
		}

		if (static_cast<Cmd>(com.code) == Cmd::ShowChoiceEnd) {
			break;
		}
	}
	return s_choices;
}

class EntryCollector {
public:
	void Add(const std::string& context, const std::vector<std::string>& lines, bool choice) {
		if (lines.empty() || std::all_of(lines.begin(), lines.end(), [](const auto& e) { return e.empty(); })) {
			return;
		}
		OutEntry entry;
		entry.context = context;
		entry.text = JoinLines(lines);
		entry.choice = choice;
		std::string key = context + "\x01" + entry.text + (choice ? "\x01ch" : "");
		if (seen.insert(key).second) {
			entries.push_back(std::move(entry));
		}
	}

	void AddSingle(const std::string& context, const std::string& text) {
		Add(context, { text }, false);
	}

	std::vector<OutEntry> entries;

private:
	std::set<std::string> seen;
};

template <typename T> bool isEventCommandString(const lcf::ContextStructBase<T>&) { return false; }
bool isEventCommandString(const lcf::ContextStructBase<lcf::rpg::EventCommand>& ctx) { return ctx.name == "string"; }

template<typename T>
int getEventId(const map_event_ctx<T>& ctx) { return ctx.parent->parent->obj->ID; }
template<typename T>
int getEventId(const common_event_ctx<T>& ctx) { return ctx.parent->obj->ID; }
template<typename T>
int getEventId(const battle_event_ctx<T>& ctx) { return ctx.parent->parent->obj->ID; }

struct ParseEvent {
	EntryCollector& collector;

	std::vector<std::string> lines;
	std::string context;
	bool choice = false;
	int prev_evt_id = 0;
	int prev_line = 0;
	int prev_indent = 0;

	void add_evt_entry() {
		collector.Add(context, lines, choice);
		lines.clear();
		context.clear();
		choice = false;
	}

	template<typename T>
	void parse(const T&) {}

	template<typename T>
	void parse(const any_event_ctx<T>& ctx) {
		int evt_id = getEventId(ctx);
		int line = ctx.parent->index + 1;

		auto indent = ctx.obj->indent;
		auto code = ctx.obj->code;
		auto estring = ctx.obj->string;

		if (prev_evt_id != evt_id || prev_line != line - 1 || prev_indent != indent) {
			add_evt_entry();
		}

		switch (static_cast<Cmd>(code)) {
			case Cmd::ShowMessage:
				add_evt_entry();
				lines.push_back(RemoveControlChars(estring));
				break;
			case Cmd::ShowMessage_2:
				lines.push_back(RemoveControlChars(estring));
				break;
			case Cmd::ShowChoice: {
				auto choices = GetChoices(ctx.parent->obj->event_commands, line);
				add_evt_entry();
				context.clear();
				choice = true;
				lines = choices;
				add_evt_entry();
				break;
			}
			case Cmd::ChangeHeroName:
				add_evt_entry();
				lines.push_back(RemoveControlChars(estring));
				context = "actors.name";
				add_evt_entry();
				break;
			case Cmd::ChangeHeroTitle:
				add_evt_entry();
				lines.push_back(RemoveControlChars(estring));
				context = "actors.title";
				add_evt_entry();
				break;
			case Cmd::ConditionalBranch:
				// Condition: Actor Name is
				if (ctx.obj->parameters.size() > 2 && ctx.obj->parameters[0] == 5 && ctx.obj->parameters[2] == 1) {
					add_evt_entry();
					lines.push_back(RemoveControlChars(estring));
					context = "actors.name";
					add_evt_entry();
				}
				break;
			case Cmd::Maniac_ShowStringPicture: {
				add_evt_entry();
				auto tokens = Utils::Tokenize(lcf::ToString(estring), [](char32_t c) { return c == 0x01; });
				if (tokens.size() >= 4) {
					for (auto& tok : Utils::Tokenize(tokens[1], [](char32_t c) { return c == '\n'; })) {
						lines.push_back(RemoveControlChars(tok));
					}
					context = "strpic";
					add_evt_entry();
				}
				break;
			}
			default:
				break;
		}

		prev_evt_id = evt_id;
		prev_line = line;
		prev_indent = indent;
	}
};

template<typename ParentType, typename Root>
void parseEvents(EntryCollector& collector, Root& root) {
	ParseEvent p { collector };

	lcf::rpg::ForEachString(root, [&](const auto& val, const auto& ctx) {
		(void)val;
		if (!ctx.parent ||
				!std::is_same<decltype(ctx.parent->obj), std::add_pointer_t<ParentType>>::value ||
				!isEventCommandString(ctx)) {
			return;
		}

		p.parse(ctx);
	});
	p.add_evt_entry();
}

void ExtractLdb(Filesystem_Stream::InputStream& is, const std::string& encoding, EntryCollector& collector) {
	auto db = lcf::LDB_Reader::Load(is, encoding);
	if (!db) {
		return;
	}

	auto chunks = { "actors", "classes", "skills", "items", "enemies", "states", "terms", "switches", "variables" };

	std::vector<std::vector<std::string>> fields = {
		{ "actors", "name", "title", "skill_name" },
		{ "classes", "name" },
		{ "skills", "name", "description", "using_message1", "using_message2" },
		{ "items", "name", "description" },
		{ "enemies", "name" },
		{ "states", "name", "message_actor", "message_enemy", "message_already", "message_affected", "message_recovery" },
		{ "switches", "name" },
		{ "variables", "name" }
	};

	lcf::rpg::ForEachString(*db, [&](const auto& val, const auto& ctx) {
		if (!ctx.parent || ctx.parent->parent) {
			// Only entries exactly one level deep
			return;
		}

		if (!val.empty()) {
			std::string_view pname = ctx.parent->name;
			std::string_view name = ctx.name;

			if (std::find(chunks.begin(), chunks.end(), pname) == chunks.end()) {
				return;
			}

			if (pname != "terms") {
				for (auto& field : fields) {
					if (ctx.parent->name == field[0]) {
						if (std::find(field.begin(), field.end(), name) == field.end()) {
							return;
						}
					}
				}
			}

			collector.AddSingle(lcf::ToString(pname) + "." + lcf::ToString(name), lcf::ToString(val));
		}
	});

	parseEvents<lcf::rpg::CommonEvent>(collector, *db);
	parseEvents<lcf::rpg::TroopPage>(collector, *db);
}

template <typename T>
bool isMapType(const T&) { return false; }
bool isMapType(const lcf::rpg::MapInfo& info) { return info.type == lcf::rpg::TreeMap::MapType_map; }

void ExtractLmt(Filesystem_Stream::InputStream& is, const std::string& encoding, EntryCollector& collector) {
	auto tree = lcf::LMT_Reader::Load(is, encoding);
	if (!tree) {
		return;
	}

	lcf::rpg::ForEachString(*tree, [&](const auto& val, const auto& ctx) {
		if (!ctx.parent || ctx.name != "name") {
			return;
		}

		if (isMapType(*ctx.obj)) {
			collector.AddSingle("", lcf::ToString(val));
		}
	});
}

void ExtractLmu(Filesystem_Stream::InputStream& is, const std::string& encoding, EntryCollector& collector) {
	auto map = lcf::LMU_Reader::Load(is, encoding);
	if (!map) {
		return;
	}
	parseEvents<lcf::rpg::EventPage>(collector, *map);
}

} // namespace

std::string MtoolExtract::ExtractAllJson() {
	EntryCollector collector;
	std::string encoding = Player::GetEncoding();

	auto game_fs = FileFinder::Game();
	if (!game_fs) {
		return "[]";
	}

	int map_count = 0;
	for (const auto& entry : *game_fs.ListDirectory()) {
		if (entry.second.type != DirectoryTree::FileType::Regular) {
			continue;
		}
		const auto& lname = entry.first;
		bool is_ldb = lname == "rpg_rt.ldb";
		bool is_lmt = lname == "rpg_rt.lmt";
		bool is_lmu = EndsWith(lname, ".lmu");
		if (!is_ldb && !is_lmt && !is_lmu) {
			continue;
		}
		auto is = game_fs.OpenInputStream(entry.second.name);
		if (!is) {
			continue;
		}
		if (is_ldb) {
			ExtractLdb(is, encoding, collector);
		} else if (is_lmt) {
			ExtractLmt(is, encoding, collector);
		} else {
			ExtractLmu(is, encoding, collector);
			++map_count;
		}
	}

	nlohmann::json out = nlohmann::json::array();
	for (const auto& e : collector.entries) {
		nlohmann::json item;
		if (!e.context.empty()) {
			item["c"] = e.context;
		}
		item["t"] = e.text;
		if (e.choice) {
			item["ch"] = true;
		}
		out.push_back(std::move(item));
	}

	// 注意：本函数跑在传输线程上，不要调 Output
	return out.dump();
}

#else

std::string MtoolExtract::ExtractAllJson() {
	return "[]";
}

#endif

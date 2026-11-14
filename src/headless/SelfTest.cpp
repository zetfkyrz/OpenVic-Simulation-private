#include "openvic-simulation/ai/DiplomacyAI.hpp"
#include "SelfTest.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <filesystem>
#include <span>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <openvic-simulation/GameManager.hpp>
#include <openvic-simulation/DefinitionManager.hpp>
#include <openvic-simulation/InstanceManager.hpp>
#include <openvic-simulation/politics/LawDynamics.hpp>
#include <openvic-simulation/country/CountryInstance.hpp>
#include <openvic-simulation/economy/GoodInstance.hpp>
#include <openvic-simulation/economy/GoodDefinition.hpp>
#include <openvic-simulation/economy/production/ProductionType.hpp>
#include <openvic-simulation/defines/OpenVicDefines.hpp>
#include <openvic-simulation/economy/BuildingType.hpp>
#include <openvic-simulation/economy/credit/Credit.hpp>
#include <openvic-simulation/economy/production/FactoryInstance.hpp>
#include <openvic-simulation/economy/production/ResourceGatheringOperation.hpp>
#include <openvic-simulation/economy/trading/TradeNetwork.hpp>
#include <openvic-simulation/population/Pop.hpp>
#include <openvic-simulation/map/ProvinceInstance.hpp>
#include <openvic-simulation/military/ArmyGroup.hpp>
#include <openvic-simulation/military/Leader.hpp>
#include <openvic-simulation/military/Division.hpp>
#include <openvic-simulation/military/Equipment.hpp>
#include <openvic-simulation/military/Companies.hpp>
#include <openvic-simulation/economy/production/RgoBuilding.hpp>
#include <openvic-simulation/population/PopDynamics.hpp>
#include <openvic-simulation/population/PopulationGrowth.hpp>
#include <openvic-simulation/military/Unlocks.hpp>
#include <openvic-simulation/ai/CountryAI.hpp>
#include <openvic-simulation/population/PopType.hpp>
#include <openvic-simulation/research/ArmyResearch.hpp>
#include <openvic-simulation/military/UnitEquipment.hpp>
#include <openvic-simulation/military/UnitModifiers.hpp>
#include <openvic-simulation/modifier/ModifierEffectCache.hpp>
#include <openvic-simulation/military/BattleFormulas.hpp>
#include <openvic-simulation/military/DivisionStats.hpp>
#include <openvic-simulation/modifier/ModifierManager.hpp>
#include <openvic-simulation/dataloader/Dataloader.hpp>
#include <openvic-simulation/military/SupplyDelivery.hpp>
#include <openvic-simulation/military/LeaderSkills.hpp>
#include <openvic-simulation/military/SupplyTerritory.hpp>
#include <openvic-simulation/military/UnitInstance.hpp>
#include <openvic-simulation/military/War.hpp>
#include <openvic-simulation/country/CountryInstanceManager.hpp>
#include <openvic-simulation/military/UnitInstanceGroup.hpp>
#include <openvic-simulation/military/UnitType.hpp>
#include <openvic-simulation/map/State.hpp>
#include <openvic-simulation/misc/GameAction.hpp>
#include <openvic-simulation/misc/MoneyLedger.hpp>
#include <openvic-simulation/misc/Replay.hpp>
#include <openvic-simulation/misc/StateHash.hpp>
#include <openvic-simulation/utility/ThreadPool.hpp>

using namespace OpenVic;

namespace {
	struct Results {
		int passed = 0;
		int failed = 0;

		void check(const bool ok, std::string_view name, std::string const& detail = {}) {
			if (ok) {
				passed++;
				fmt::println("TEST PASS {}", name);
			} else {
				failed++;
				fmt::println("TEST FAIL {}: {}", name, detail);
			}
		}
		void summary() const {
			fmt::println("SELFTEST SUMMARY passed={} failed={}", passed, failed);
		}
	};

	// ---------------------------------------------------------------- pure tests

	void test_rate(Results& r) {
		bool exact = true;
		std::string detail;
		const std::int32_t numerators[] = { 1, 2, 3, 5, 7, 8, 15, 25, 35, 60, 80 };
		const std::int32_t denominators[] = { 2, 4, 5, 8, 10, 100, 1000 };
		for (std::int32_t n : numerators) {
			for (std::int32_t d : denominators) {
				if (n > d) {
					continue;
				}
				const Rate rate { n, d };
				for (std::int32_t v : { 1, 7, 100, 12345, 987654 }) {
					const fixed_point_t x = fixed_point_t(v) / 7;
					if (rate.of(x) != x * n / d) {
						exact = false;
						detail = fmt::format("{}/{} of {}", n, d, x);
					}
				}
				if (rate.fixed() != fixed_point_t(n) / d) {
					exact = false;
					detail = fmt::format("fixed {}/{}", n, d);
				}
			}
		}
		r.check(exact, "rate: of() and fixed() give the same numbers as x * n / d", detail);
	}

	void test_defines(Results& r) {
		OpenVicDefines defines;
		OpenVicDefines other;
		r.check(defines.get_hash() == other.get_hash(), "defines: the hash of the defaults is the same in two objects");

		const std::uint64_t default_hash = defines.get_hash();
		const std::int32_t default_rank = defines.ai.max_rank_for_wars;
		defines.ai.set_value("test.lua", "MAX_RANK_FOR_WARS", "12");
		r.check(defines.ai.max_rank_for_wars == 12, "defines: a valid whole number is read");
		defines.ai.set_value("test.lua", "MAX_RANK_FOR_WARS", "100000");
		r.check(defines.ai.max_rank_for_wars == 12, "defines: a value above the range keeps the old value");
		defines.ai.set_value("test.lua", "MAX_RANK_FOR_WARS", "-3");
		r.check(defines.ai.max_rank_for_wars == 12, "defines: a negative value outside the range keeps the old value");
		defines.ai.set_value("test.lua", "MAX_RANK_FOR_WARS", "abc");
		r.check(defines.ai.max_rank_for_wars == 12, "defines: text instead of a number keeps the old value");
		defines.ai.set_value("test.lua", "MAX_RANK_FOR_WARS", "2.5");
		r.check(defines.ai.max_rank_for_wars == 12, "defines: a fraction for a whole number keeps the old value");
		defines.ai.set_value("test.lua", "NO_SUCH_KEY", "1");
		r.check(defines.ai.max_rank_for_wars == 12, "defines: an unknown key changes nothing");
		defines.ai.set_value("test.lua", "WAR_CHANCE_PER_YEAR", "0.1");
		r.check(defines.ai.war_chance_per_year.numerator == 1000, "defines: a rate is read as a fraction (0.1 = 1000/10000)");
		defines.ai.set_value("test.lua", "WAR_CHANCE_PER_YEAR", "-0.1");
		r.check(defines.ai.war_chance_per_year.numerator == 1000, "defines: a negative rate keeps the old value");
		defines.politics.set_value("test.lua", "CLOSENESS_WIDTH", "0.5");
		r.check(defines.politics.closeness_width == 0.5, "defines: a double is read");

		// The hash follows the values and only them
		defines.update_hash();
		r.check(defines.get_hash() != default_hash, "defines: the hash changes when a value changes");
		defines.ai.max_rank_for_wars = default_rank;
		defines.ai.war_chance_per_year = OpenVicDefines {}.ai.war_chance_per_year;
		defines.politics.closeness_width = OpenVicDefines {}.politics.closeness_width;
		defines.update_hash();
		r.check(defines.get_hash() == default_hash, "defines: the hash is the same again when the values are the defaults again");
	}

	game_action_t make(auto&& action) {
		return game_action_t(std::forward<decltype(action)>(action));
	}

	void test_replay(Results& r) {
		std::vector<ReplayEntry> entries;
		entries.push_back({ 1, game_action_t(std::in_place_type<tick_argument_t>) });
		entries.push_back({ 1, game_action_t(std::in_place_type<set_tariff_rate_argument_t>, country_index_t(3), fixed_point_t(1) / 4) });
		entries.push_back({ 2, game_action_t(std::in_place_type<set_good_trade_order_argument_t>, country_index_t(1), good_index_t(2), true, fixed_point_t(5) / 3) });
		entries.push_back({ 2, game_action_t(std::in_place_type<move_army_argument_t>, unique_id_t(77), province_index_t(9)) });
		entries.push_back({ 3, game_action_t(std::in_place_type<create_leader_argument_t>, country_index_t(2), unit_branch_t::LAND) });
		entries.push_back({ 3, game_action_t(std::in_place_type<set_mobilise_argument_t>, country_index_t(4), false) });
		entries.push_back({ 4, game_action_t(std::in_place_type<stop_army_argument_t>, unique_id_t(1234567890123ULL)) });
		entries.push_back({ 4, game_action_t(std::in_place_type<take_credit_argument_t>, country_index_t(5), std::uint32_t(30)) });

		const std::string text = Replay::serialise(entries);
		std::vector<ReplayEntry> back;
		const bool parsed = Replay::deserialise(text, back);
		r.check(parsed, "replay: the text of a replay is read back");
		bool same = parsed && back.size() == entries.size();
		for (size_t i = 0; same && i < entries.size(); ++i) {
			same = back[i].batch == entries[i].batch && back[i].action.index() == entries[i].action.index();
		}
		r.check(same, "replay: batches and kinds of the actions are kept");
		r.check(parsed && Replay::serialise(back) == text, "replay: writing the read actions gives the same text");

		std::vector<ReplayEntry> broken;
		r.check(!Replay::deserialise("OVREPLAY 1\n1 no_such_action 1 2\n", broken), "replay: an unknown action is refused");
		broken.clear();
		r.check(!Replay::deserialise("OVREPLAY 1\n1 set_tariff_rate 3\n", broken), "replay: missing values are refused");
		broken.clear();
		r.check(!Replay::deserialise("not a replay\n", broken), "replay: a file without the header is refused");
		broken.clear();
		r.check(Replay::deserialise("OVREPLAY 1\n", broken) && broken.empty(), "replay: an empty replay is fine");

		r.check(
			!Replay::is_recorded(game_action_t(std::in_place_type<set_pause_argument_t>, true)) &&
				!Replay::is_recorded(game_action_t(std::in_place_type<set_speed_argument_t>, std::int64_t(2))) &&
				Replay::is_recorded(game_action_t(std::in_place_type<tick_argument_t>)),
			"replay: pause and speed are not recorded, ticks are"
		);
	}

	void test_save_header(Results& r) {
		const std::uint64_t hash = 0x1234567890abcdefULL;
		std::uint64_t parsed = 0;
		r.check(
			Replay::hash_from_string(Replay::hash_to_string(hash), parsed) && parsed == hash && Replay::hash_to_string(hash).size() == 16,
			"save header: the hash is written and read as 16 hex digits"
		);
		r.check(!Replay::hash_from_string("xyz", parsed) && !Replay::hash_from_string("", parsed), "save header: a wrong hash text is refused");

		Replay::Compatibility same = Replay::check_header(Replay::FORMAT_VERSION, Replay::SIMULATION_VERSION, true, hash, hash);
		r.check(same.loadable && same.warnings.empty(), "save header: the same version and defines load without a warning");

		Replay::Compatibility other_defines = Replay::check_header(Replay::FORMAT_VERSION, Replay::SIMULATION_VERSION, true, hash, hash + 1);
		r.check(other_defines.loadable && other_defines.warnings.size() == 1, "save header: other defines give one warning and can still be opened");

		Replay::Compatibility other_version = Replay::check_header(Replay::FORMAT_VERSION, Replay::SIMULATION_VERSION + 1, true, hash, hash);
		r.check(other_version.loadable && other_version.warnings.size() == 1, "save header: another simulation version gives a warning");

		Replay::Compatibility old_format = Replay::check_header(1, -1, false, 0, hash);
		r.check(old_format.loadable && !old_format.warnings.empty(), "save header: format 1 is migrated with a warning");

		Replay::Compatibility future = Replay::check_header(Replay::FORMAT_VERSION + 1, 1, true, hash, hash);
		r.check(!future.loadable && !future.blocked_reason.empty(), "save header: a newer format is refused with a reason (no crash)");

		Replay::Compatibility broken = Replay::check_header(0, 1, true, hash, hash);
		r.check(!broken.loadable && !broken.blocked_reason.empty(), "save header: an unreadable format is refused with a reason");
	}

	// ---------------------------------------------------------------- tests on a real game

	double dbl(fixed_point_t x);
	bool near(fixed_point_t a, fixed_point_t b, fixed_point_t tolerance);

	// ---- the weapons (docs/EQUIPMENT_MODDING.md)

	void write_test_file(std::filesystem::path const& path, std::string_view text) {
		std::filesystem::create_directories(path.parent_path());
		std::ofstream file { path, std::ios::binary };
		file << text;
	}

	// The weapons are read from files only: a new line and new weapons come with files, a mod replaces a weapon, a broken file is skipped
	void test_equipment_pure(Results& r) {
		namespace fs = std::filesystem;
		const fs::path temp = fs::temp_directory_path() / "openvic_selftest_equipment";
		std::error_code ec;
		fs::remove_all(temp, ec);
		const fs::path base = temp / "base";
		const fs::path mod = temp / "mod";

		write_test_file(base / "common/ov_equipment/lines.txt",
			"rifle = { icon = \"small_arms\" name = \"LINE_RIFLE\" slot = infantry unit_types = { infantry } }\n"
			"siege_artillery = { icon = \"artillery\" name = \"LINE_SIEGE\" slot = special unit_types = { siege } }\n");
		write_test_file(base / "common/ov_equipment/rifles/rifle_a.txt",
			"rifle_a = {\n line = rifle\n tier = 1\n attack_mult = 4\n defense_mult = 1\n breakthrough_mult = 1\n weight = 4.5\n"
			" flags = { smokeless breech_loading breech_loading }\n good = { base_price = 37 }\n}\n");
		write_test_file(base / "common/ov_equipment/rifles/rifle_b.txt",
			"rifle_b = {\n line = rifle\n tier = 2\n attack_mult = 6\n defense_mult = 1\n breakthrough_mult = 1\n weight = 4.2\n"
			" unlock_key = \"my_unlock:b\"\n mystery_key = 5\n good = { id = \"rifle_b_good\" base_price = 50 }\n}\n");
		// A new line with one weapon: no code is needed
		write_test_file(base / "common/ov_equipment/siege/mortar_1.txt",
			"mortar_1 = {\n line = siege_artillery\n tier = 1\n attack_mult = 8\n defense_mult = 1\n breakthrough_mult = 1\n weight = 2000\n"
			" good = { base_price = 90 }\n}\n");
		// Broken: an unknown line, a missing value, a value out of its range, a file that cannot be parsed
		write_test_file(base / "common/ov_equipment/rifles/bad_line.txt",
			"bad_line = {\n line = nothing\n tier = 1\n attack_mult = 1\n defense_mult = 1\n breakthrough_mult = 1\n weight = 1\n good = { base_price = 1 }\n}\n");
		write_test_file(base / "common/ov_equipment/rifles/bad_missing.txt",
			"bad_missing = {\n line = rifle\n tier = 1\n attack_mult = 1\n defense_mult = 1\n weight = 1\n good = { base_price = 1 }\n}\n");
		write_test_file(base / "common/ov_equipment/rifles/bad_range.txt",
			"bad_range = {\n line = rifle\n tier = 1\n attack_mult = -1\n defense_mult = 1\n breakthrough_mult = 1\n weight = 1\n good = { base_price = 1 }\n}\n");
		write_test_file(base / "common/ov_equipment/rifles/bad_syntax.txt", "bad_syntax = { line = rifle tier = \n");
		// The mod: replaces rifle_a with a file of the same path, adds rifle_c, replaces rifle_b by id from another file
		write_test_file(mod / "common/ov_equipment/rifles/rifle_a.txt",
			"rifle_a = {\n line = rifle\n tier = 1\n attack_mult = 5\n defense_mult = 1\n breakthrough_mult = 1\n weight = 4.4\n good = { base_price = 40 }\n}\n");
		write_test_file(mod / "common/ov_equipment/rifles/rifle_c.txt",
			"rifle_c = {\n line = rifle\n tier = 3\n attack_mult = 9\n defense_mult = 1\n breakthrough_mult = 1\n weight = 4\n good = { base_price = 70 }\n}\n"
			"rifle_b = {\n line = rifle\n tier = 2\n attack_mult = 7\n defense_mult = 1\n breakthrough_mult = 1\n weight = 4.2\n good = { base_price = 51 }\n}\n");

		Dataloader loader;
		const Dataloader::path_vector_t roots { base, mod };
		r.check(loader.set_roots(roots, {}, false), "equipment: the roots of a test (a base and a mod) are accepted");

		EquipmentManager manager;
		const std::size_t loaded = loader.load_equipment_files(manager);
		r.check(manager.get_lines().size() == 2, "equipment: the two lines of lines.txt are loaded", std::to_string(manager.get_lines().size()));
		r.check(
			manager.find_line("siege_artillery") != nullptr && manager.find_line("siege_artillery")->slot == "special",
			"equipment: a new line is described by a file only"
		);
		// rifle_a (replaced), rifle_b, rifle_c, rifle_b again (replaced), mortar_1; the four broken files are skipped
		r.check(loaded == 5 && manager.get_equipment().size() == 4, "equipment: the good files are loaded, the broken ones are skipped",
			fmt::format("loaded {} of {} weapons", loaded, manager.get_equipment().size()));
		r.check(
			manager.find_equipment("bad_line") == nullptr && manager.find_equipment("bad_missing") == nullptr
				&& manager.find_equipment("bad_range") == nullptr && manager.find_equipment("bad_syntax") == nullptr,
			"equipment: a weapon of an unknown line, with a missing or wrong value, or in a file with a syntax error is not loaded"
		);
		EquipmentDefinition const* const mortar = manager.find_equipment("mortar_1");
		r.check(mortar != nullptr && mortar->line == "siege_artillery", "equipment: a weapon of the new line is loaded without code");

		EquipmentDefinition const* const rifle_a = manager.find_equipment("rifle_a");
		r.check(
			rifle_a != nullptr && rifle_a->attack_mult == fixed_point_t(5) && rifle_a->good.base_price == fixed_point_t(40),
			"equipment: a file of a mod with the same path replaces the built-in one"
		);
		r.check(rifle_a != nullptr && rifle_a->unlock_key == "equipment:rifle_a", "equipment: the unlock key is equipment:<id> by default");
		r.check(
			rifle_a != nullptr && rifle_a->flags.empty() && !rifle_a->has_flag("breech_loading"),
			"equipment: the flags of a mod weapon are only the ones of its file"
		);
		EquipmentDefinition const* const rifle_b = manager.find_equipment("rifle_b");
		r.check(
			rifle_b != nullptr && rifle_b->attack_mult == fixed_point_t(7) && rifle_b->unlock_key == "equipment:rifle_b",
			"equipment: a weapon of a mod with the id of another one replaces it, whatever the file"
		);
		EquipmentDefinition const* const rifle_c = manager.find_equipment("rifle_c");
		r.check(rifle_c != nullptr && rifle_c->good.identifier == "rifle_c", "equipment: the id of the good is the id of the weapon by default");

		// The load order does not depend on the file system: the base, then the mod, the files of a root sorted
		memory::vector<std::string> order;
		for (fs::path const& file : loader.lookup_files_in_dir_load_order("common/ov_equipment", ".txt", true)) {
			order.push_back(fs::relative(file, temp).generic_string());
		}
		const std::size_t base_files = std::count_if(order.begin(), order.end(), [](std::string const& f) { return f.starts_with("base/"); });
		const bool sorted_per_root = std::is_sorted(order.begin(), order.begin() + base_files) && std::is_sorted(order.begin() + base_files, order.end());
		r.check(order.size() == 9 && sorted_per_root && order.back().starts_with("mod/"), "equipment: the files are loaded in a fixed order, the mod last",
			fmt::format("{} files", order.size()));

		auto const line_equipment = manager.get_line_equipment("rifle");
		r.check(
			line_equipment.size() == 3 && line_equipment[0]->tier == 1 && line_equipment[1]->tier == 2 && line_equipment[2]->tier == 3,
			"equipment: the weapons of a line come the lowest generation first"
		);

		fs::remove_all(temp, ec);
	}

	// The weapons that come with the game (ov_data), if the data folder is found
	void test_equipment_data(Results& r, GameManager& game_manager) {
		if (game_manager.get_builtin_data_path().empty()) {
			fmt::println("TEST INFO equipment: no ov_data folder, the weapons of the game are not checked");
			return;
		}
		EquipmentManager const& manager = game_manager.get_definition_manager().get_military_manager().get_equipment_manager();
		r.check(manager.get_lines().size() >= 2, "equipment: the lines of the game are loaded", std::to_string(manager.get_lines().size()));
		for (std::string_view line : { "rifle", "field_artillery" }) {
			auto const generations = manager.get_line_equipment(line);
			bool ok = generations.size() >= 5;
			for (std::size_t i = 0; ok && i < generations.size(); i++) {
				ok = generations[i]->tier == i;
			}
			r.check(ok, fmt::format("equipment: the line {} has the generations 0 to 4", line), std::to_string(generations.size()));
		}
		EquipmentDefinition const* const rifle_3 = manager.find_equipment("rifle_3");
		r.check(
			rifle_3 != nullptr && rifle_3->attack_mult == fixed_point_t(9) / 4 && rifle_3->defense_mult == fixed_point_t(8) / 5 && rifle_3->breakthrough_mult == fixed_point_t(3) / 2
				&& rifle_3->has_flag("breech_loading") && !rifle_3->has_flag("magazine"),
			"equipment: rifle_3 has the multipliers of the table (attack 2.25, defence 1.6, breakthrough 1.5, breech loading)"
		);
		EquipmentDefinition const* const rifle_1 = manager.find_equipment("rifle_1");
		EquipmentDefinition const* const rifle_0 = manager.find_equipment("rifle_0");
		EquipmentDefinition const* const gun_0 = manager.find_equipment("field_gun_0");
		r.check(
			rifle_0 != nullptr && rifle_0->good.replaces == "small_arms" && rifle_0->good.base_price == fixed_point_t(37)
				&& rifle_1 != nullptr && rifle_1->good.replaces.empty() && gun_0 != nullptr && gun_0->good.replaces == "artillery",
			"equipment: rifle_0 and field_gun_0 are the old small arms and artillery (they replace the goods of Vic2, price 37 and 60)"
		);
		EquipmentDefinition const* const gun_4 = manager.find_equipment("field_gun_4");
		r.check(
			gun_4 != nullptr && gun_4->has_flag("indirect_fire") && gun_4->attack_mult == fixed_point_t(4) && gun_4->good.base_price == fixed_point_t(185),
			"equipment: field_gun_4 is the quick-firing gun (attack 4, indirect fire, price 185)"
		);
		EquipmentDefinition const* const gun_1 = manager.find_equipment("field_gun_1");
		r.check(
			gun_1 != nullptr && gun_1->has_flag("canister") && gun_1->attack_mult == fixed_point_t::_1,
			"equipment: field_gun_1 has canister and the multipliers 1"
		);
		r.check(
			rifle_0 != nullptr && gun_0 != nullptr && rifle_0->tier == 0 && gun_0->tier == 0 && rifle_0->attack_mult == fixed_point_t(7) / 10
				&& rifle_0->defense_mult == fixed_point_t(7) / 10 && rifle_0->breakthrough_mult == fixed_point_t(7) / 10
				&& gun_0->attack_mult == fixed_point_t(7) / 10 && gun_0->breakthrough_mult == fixed_point_t(7) / 10
				&& rifle_0->good.base_price == fixed_point_t(37) && gun_0->good.base_price == fixed_point_t(60),
			"equipment: the weapons of tier 0 (rifle_0, field_gun_0) have the multipliers 0.7 and the prices of rifle_1 and field_gun_1"
		);
		bool prices_grow = true;
		for (std::string_view line : { "rifle", "field_artillery" }) {
			auto const generations = manager.get_line_equipment(line);
			for (std::size_t i = 1; i < generations.size(); i++) {
				prices_grow &= generations[i]->good.base_price >= generations[i - 1]->good.base_price; // rifle_0 costs as rifle_1
			}
		}
		r.check(prices_grow, "equipment: the base price grows with the generation");
		bool power_grows = true;
		for (std::string_view line : { "rifle", "field_artillery" }) {
			auto const generations = manager.get_line_equipment(line);
			for (std::size_t i = 1; i < generations.size(); i++) {
				power_grows &= generations[i]->attack_mult > generations[i - 1]->attack_mult && generations[i]->defense_mult > generations[i - 1]->defense_mult
					&& generations[i]->breakthrough_mult > generations[i - 1]->breakthrough_mult;
			}
		}
		r.check(power_grows, "equipment: the attack and the defence grow with the generation");

		// The weapons are goods (rifle_1..4, field_gun_1..4); the old names find the same goods
		GoodDefinitionManager const& goods = game_manager.get_definition_manager().get_economy_manager().get_good_definition_manager();
		bool all_goods = true, prices_match = true, in_category = true;
		for (EquipmentDefinition const& equipment : manager.get_equipment()) {
			GoodDefinition const* const good = goods.get_good_definition_by_identifier(equipment.good.identifier);
			all_goods &= good != nullptr;
			if (good != nullptr) {
				prices_match &= good->base_price == equipment.good.base_price;
				in_category &= good->category.get_identifier() == equipment.good.category;
			}
		}
		r.check(all_goods && manager.get_equipment().size() >= 10, "goods: every weapon is a good of the market (rifle_0..4, field_gun_0..4)");
		r.check(prices_match, "goods: the base price of a good is the one of the file of its weapon");
		r.check(in_category, "goods: the goods of the weapons are in the category of the file (military goods)");
		r.check(
			goods.get_good_definition_by_identifier("small_arms") != nullptr
				&& goods.get_good_definition_by_identifier("small_arms") == goods.get_good_definition_by_identifier("rifle_0")
				&& goods.get_good_definition_by_identifier("artillery") != nullptr
				&& goods.get_good_definition_by_identifier("artillery") == goods.get_good_definition_by_identifier("field_gun_0"),
			"goods: the old names small_arms and artillery are rifle_0 and field_gun_0 (the files of Vic2 keep working)"
		);
		UnitTypeManager const& unit_types = game_manager.get_definition_manager().get_military_manager().get_unit_type_manager();
		RegimentType const* const infantry_type = unit_types.get_regiment_type_by_identifier("line_infantry");
		GoodDefinition const* const rifle_good = goods.get_good_definition_by_identifier("rifle_0");
		bool builds_with_rifles = false, ammunition_used = false;
		if (infantry_type != nullptr) {
			for (auto const& [good, quantity] : infantry_type->get_build_cost()) {
				builds_with_rifles |= good == rifle_good && quantity > fixed_point_t::_0;
			}
			for (UnitType::supply_item_t const& item : infantry_type->get_supply_items()) {
				ammunition_used |= item.good->get_identifier() == "ammunition" && item.construction_amount > fixed_point_t::_0;
			}
		}
		r.check(builds_with_rifles && ammunition_used, "goods: an infantry regiment is built from rifle_0 (the old small arms of its file) and still uses ammunition");
	}

	// ---- the unlocks of a country (docs/EQUIPMENT_MODDING.md, section 6)

	void test_unlocks(Results& r, InstanceManager& im) {
		EquipmentManager const& equipment = im.definition_manager.get_military_manager().get_equipment_manager();
		ProvisionalUnlocks const& provisional = im.definition_manager.get_military_manager().get_provisional_unlocks();
		OpenVicWarDefines const& war = im.definition_manager.get_define_manager().get_openvic_defines().war;
		if (equipment.get_equipment().empty() || provisional.get_steps().empty()) {
			fmt::println("TEST INFO unlocks: no ov_data folder, the unlocks are not checked");
			return;
		}

		std::span<CountryInstance> countries = im.get_country_instance_manager().get_country_instances();
		CountryInstance* country = nullptr;
		CountryInstance* other = nullptr;
		for (std::size_t i = countries.size(); i-- > 0;) {
			if (countries[i].exists()) {
				(country == nullptr ? country : other) = &countries[i];
			}
			if (other != nullptr) {
				break;
			}
		}
		r.check(country != nullptr && other != nullptr, "unlocks: two countries to test with");
		if (country == nullptr || other == nullptr) {
			return;
		}
		{
			bool all_have_defaults = true;
			for (CountryInstance const& each : countries) {
				all_have_defaults &= each.has_unlock("equipment:rifle_0") && each.has_unlock("equipment:field_gun_0");
			}
			r.check(all_have_defaults, "unlocks: every country has the weapons of tier 0 from the start (default_unlocks.txt)");
		}
		StaticModifierCache const& static_modifier_cache = im.definition_manager.get_modifier_manager().get_static_modifier_cache();
		const auto refresh = [&]() {
			country->update_modifier_sum(im.get_today(), static_modifier_cache);
		};

		// apply_provisional_unlocks gives the keys to every country: all of them are put back at the end
		struct unlock_snapshot_t {
			std::vector<memory::string> keys;
			std::vector<Modifier const*> modifiers;
			std::uint32_t applied = 0;
		};
		std::vector<unlock_snapshot_t> snapshots;
		for (CountryInstance const& each : countries) {
			snapshots.push_back({
				{ each.get_unlock_keys().begin(), each.get_unlock_keys().end() },
				{ each.get_unlock_modifiers().begin(), each.get_unlock_modifiers().end() }, each.get_provisional_unlock_steps_applied()
			});
		}
		const auto restore_all = [&]() {
			for (std::size_t i = 0; i < countries.size(); ++i) {
				CountryInstance& each = countries[i];
				for (memory::string const& key : std::vector<memory::string> { each.get_unlock_keys().begin(), each.get_unlock_keys().end() }) {
					each.revoke_unlock(key);
				}
				for (memory::string const& key : snapshots[i].keys) {
					each.grant_unlock(key);
				}
				while (!each.get_unlock_modifiers().empty()) {
					each.remove_unlock_modifier(*each.get_unlock_modifiers().front());
				}
				for (Modifier const* const modifier : snapshots[i].modifiers) {
					each.add_unlock_modifier(*modifier);
				}
				each.set_provisional_unlock_steps_applied(snapshots[i].applied);
			}
		};

		EquipmentDefinition const* const rifle_2 = equipment.find_equipment("rifle_2");
		EquipmentDefinition const* const rifle_3 = equipment.find_equipment("rifle_3");
		EquipmentDefinition const* const rifle_1 = equipment.find_equipment("rifle_1");
		if (rifle_1 == nullptr || rifle_2 == nullptr || rifle_3 == nullptr) {
			r.check(false, "unlocks: the weapons rifle_1, rifle_2 and rifle_3 are loaded");
			return;
		}

		// ---- the layer itself: keys and the state hash
		const std::uint64_t hash_before = StateHash::compute(im);
		const bool had_key = country->has_unlock("equipment:rifle_3");
		r.check(!had_key && !is_equipment_unlocked(*country, *rifle_3), "unlocks: a weapon without its key is not available");
		r.check(country->grant_unlock("equipment:rifle_3"), "unlocks: grant_unlock gives a new key");
		r.check(country->has_unlock("equipment:rifle_3") && is_equipment_unlocked(*country, *rifle_3), "unlocks: with the key the weapon is available");
		r.check(!is_equipment_unlocked(*other, *rifle_3), "unlocks: the key of one country does not open the weapon of another");
		r.check(!country->grant_unlock("equipment:rifle_3"), "unlocks: a key is held once");
		r.check(!country->grant_unlock("no_colon") && !country->grant_unlock(":x") && !country->grant_unlock("x:") && !country->grant_unlock("a b:c"),
			"unlocks: a key must be <kind>:<id>");
		EquipmentDefinition const* const best = get_best_equipment(equipment, *country, "rifle");
		r.check(best != nullptr && best->tier >= 3, "unlocks: the best weapon of a line is the one of the highest generation the country has",
			best != nullptr ? std::string(best->identifier) : std::string("none"));
		country->grant_unlock("formation:test");
		country->grant_unlock("support_company:test");
		r.check(
			country->has_unlock("formation:test") && country->has_unlock("support_company:test"),
			"unlocks: the keys of formations and of companies are the same kind of thing"
		);
		const std::uint64_t hash_with_key = StateHash::compute(im);
		r.check(hash_with_key != hash_before, "unlocks: the set of keys is in the state hash");
		country->revoke_unlock("formation:test");
		country->revoke_unlock("support_company:test");
		r.check(country->revoke_unlock("equipment:rifle_3") && !country->has_unlock("equipment:rifle_3"), "unlocks: revoke_unlock takes the key away");
		r.check(!country->revoke_unlock("equipment:rifle_3"), "unlocks: a key that is not held cannot be taken away");
		r.check(StateHash::compute(im) == hash_before, "unlocks: the hash is back when the keys are");
		const auto sorted_keys = country->get_unlock_keys();
		r.check(std::is_sorted(sorted_keys.begin(), sorted_keys.end()), "unlocks: the keys are kept sorted (a fixed order for the hash)");

		// ---- the provisional layer by the year: the first day of 1836 gave the first weapons and no modifier
		r.check(
			country->has_unlock("equipment:rifle_1") && country->has_unlock("equipment:field_gun_1") && !country->has_unlock("equipment:rifle_2"),
			"unlocks: a civilised country at the start has rifle_1 and field_gun_1 from its technologies, not rifle_2"
		);
		refresh();
		DivisionTemplateLimits limits = get_template_limits(im, *country);
		r.check(
			limits.ranks == static_cast<std::uint32_t>(war.template_ranks_default) && limits.depth == static_cast<std::uint32_t>(war.template_depth_default)
				&& limits.company_slots == static_cast<std::uint32_t>(war.template_company_slots_default) && limits.row_weight_shift == 0,
			"unlocks: a country at the start has the default limits (3 ranks, 2 rows, 1 slot)",
			fmt::format("{} ranks, {} rows, {} slots", limits.ranks, limits.depth, limits.company_slots)
		);

		// ---- the later years (the same function the daily update calls, the year given)
		const std::uint32_t applied_before = country->get_provisional_unlock_steps_applied();
		const std::size_t keys_before = country->get_unlock_keys().size();
		const std::size_t modifiers_before = country->get_unlock_modifiers().size();
		// the layer is off by default (the technologies of the army give the unlocks): these checks turn it on
		OpenVicWarDefines& switch_war = const_cast<OpenVicWarDefines&>(war);
		const auto switch_before = switch_war.provisional_unlocks_enabled;
		switch_war.provisional_unlocks_enabled = 1;
		apply_provisional_unlocks(im, 1870);
		refresh();
		limits = get_template_limits(im, *country);
		r.check(
			country->has_unlock("equipment:rifle_3") && country->has_unlock("equipment:field_gun_3") && !country->has_unlock("equipment:rifle_4"),
			"unlocks: by 1870 the provisional layer has given the weapons of the third generation, not the fourth"
		);
		r.check(
			limits.ranks == 5 && limits.depth == 3 && limits.company_slots == 2,
			"unlocks: by 1870 the modifiers give 5 ranks, 3 rows and 2 company slots",
			fmt::format("{} ranks, {} rows, {} slots", limits.ranks, limits.depth, limits.company_slots)
		);
		r.check(!is_equipment_unlocked(*other, *rifle_2) || other->has_unlock("equipment:rifle_2"), "unlocks: the other countries are not touched by the one");
		apply_provisional_unlocks(im, 1900);
		refresh();
		limits = get_template_limits(im, *country);
		r.check(
			limits.ranks == 6 && limits.depth == 4 && limits.company_slots == 4 && limits.row_weight_shift == 1 && country->has_unlock("equipment:rifle_4"),
			"unlocks: by 1900 the limits are the maximum (6 ranks, 4 rows, 4 slots), the weights of the rows are shifted, rifle_4 is there",
			fmt::format("{} ranks, {} rows, {} slots, shift {}", limits.ranks, limits.depth, limits.company_slots, limits.row_weight_shift)
		);
		const auto best_late = get_best_equipment(equipment, *country, "rifle");
		r.check(best_late != nullptr && best_late->identifier == "rifle_4", "unlocks: the best rifle of the late years is rifle_4");

		// ---- the modifier max_columns changes the limit of the grid (one modifier of its own)
		{
			// Back to the start: no unlock modifiers
			while (!country->get_unlock_modifiers().empty()) {
				country->remove_unlock_modifier(*country->get_unlock_modifiers().front());
			}
			refresh();
			const std::uint32_t ranks_without = get_template_limits(im, *country).ranks;
			Modifier const* column_modifier = nullptr;
			for (ProvisionalUnlockStep const& step : provisional.get_steps()) {
				if (step.modifier.has_value() && step.year == 1845) {
					column_modifier = &*step.modifier;
				}
			}
			r.check(column_modifier != nullptr, "unlocks: the provisional file has the step of 1845 with max_columns");
			if (column_modifier != nullptr) {
				country->add_unlock_modifier(*column_modifier);
				refresh();
				r.check(
					get_template_limits(im, *country).ranks == ranks_without + 1,
					"unlocks: the modifier max_columns = 1 lets the grid have one more rank",
					fmt::format("{} -> {}", ranks_without, get_template_limits(im, *country).ranks)
				);
				country->remove_unlock_modifier(*column_modifier);
				refresh();
				r.check(get_template_limits(im, *country).ranks == ranks_without, "unlocks: without the modifier the limit is the old one");
			}
		}

		// ---- the player researches himself: a country that is not an AI one gets nothing from the layer (and catches up when the AI is switched on)
		{
			const std::uint32_t steps_before = other->get_provisional_unlock_steps_applied();
			other->set_provisional_unlock_steps_applied(0);
			other->set_ai(false);
			apply_provisional_unlocks(im, 1900);
			r.check(other->get_provisional_unlock_steps_applied() == 0, "unlocks: the provisional layer gives nothing to a country of the player");
			other->set_ai(true);
			apply_provisional_unlocks(im, 1900);
			r.check(other->get_provisional_unlock_steps_applied() > 0, "unlocks: the AI that plays for the player catches up with the steps of the layer");
			other->set_provisional_unlock_steps_applied(steps_before);
		}

		// ---- the switch: without the layer nothing is given
		{
			OpenVicWarDefines& mutable_war = const_cast<OpenVicWarDefines&>(war);
			const auto enabled_before = mutable_war.provisional_unlocks_enabled;
			mutable_war.provisional_unlocks_enabled = 0;
			country->revoke_unlock("equipment:rifle_4");
			country->set_provisional_unlock_steps_applied(0);
			apply_provisional_unlocks(im, 1900);
			r.check(
				!country->has_unlock("equipment:rifle_4") && country->get_provisional_unlock_steps_applied() == 0,
				"unlocks: with ov_war.PROVISIONAL_UNLOCKS_ENABLED = 0 the provisional layer gives nothing"
			);
			mutable_war.provisional_unlocks_enabled = enabled_before;
		}
		switch_war.provisional_unlocks_enabled = switch_before;

		// ---- put the country back
		for (memory::string const& key : std::vector<memory::string> { country->get_unlock_keys().begin(), country->get_unlock_keys().end() }) {
			country->revoke_unlock(key);
		}
		while (!country->get_unlock_modifiers().empty()) {
			country->remove_unlock_modifier(*country->get_unlock_modifiers().front());
		}
		country->grant_unlock("equipment:rifle_1");
		country->grant_unlock("equipment:field_gun_1");
		country->set_provisional_unlock_steps_applied(applied_before);
		(void)keys_before;
		(void)modifiers_before;
		restore_all();
		refresh();
	}

	// ---- what a template is worth (military/DivisionStats.cpp, military/BattleFormulas.cpp)

	void test_division_stats(Results& r, InstanceManager& im) {
		EquipmentManager const& equipment = im.definition_manager.get_military_manager().get_equipment_manager();
		ProvisionalUnlocks const& provisional = im.definition_manager.get_military_manager().get_provisional_unlocks();
		UnitTypeManager const& unit_types = im.definition_manager.get_military_manager().get_unit_type_manager();
		if (equipment.get_equipment().empty() || provisional.get_steps().empty()) {
			fmt::println("TEST INFO division stats: no ov_data folder, not checked");
			return;
		}
		OpenVicWarDefines const& war = im.definition_manager.get_define_manager().get_openvic_defines().war;

		// The weights of the rows
		using battle_formulas::row_weight;
		r.check(
			row_weight(0, 0) == fixed_point_t::_1 && row_weight(1, 0) == fixed_point_t::_0_50 && row_weight(2, 0) == fixed_point_t::_1 / 4
				&& row_weight(3, 0) == fixed_point_t::_1 / 8,
			"battle: the weights of the rows are 1, 1/2, 1/4, 1/8"
		);
		r.check(
			row_weight(0, 1) == fixed_point_t::_1 && row_weight(1, 1) == fixed_point_t::_1 && row_weight(2, 1) == fixed_point_t::_0_50
				&& row_weight(3, 1) == fixed_point_t::_1 / 4,
			"battle: with the formation of the technologies the weights of the rows are 1, 1, 1/2, 1/4"
		);

		CountryInstance* country = nullptr;
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (candidate.exists() && candidate.has_army_command()) {
				country = &candidate;
			}
		}
		RegimentType const* const infantry = unit_types.get_regiment_type_by_identifier("line_infantry");
		RegimentType const* const cavalry = unit_types.get_regiment_type_by_identifier("cavalry");
		RegimentType const* const artillery = unit_types.get_regiment_type_by_identifier("napoleonic_artillery");
		RegimentType const* const skirmishers = unit_types.get_regiment_type_by_identifier("skirmisher_infantry");
		RegimentType const* const rifled = unit_types.get_regiment_type_by_identifier("rifled_artillery");
		r.check(country != nullptr && infantry != nullptr && cavalry != nullptr && artillery != nullptr && skirmishers != nullptr && rifled != nullptr,
			"division stats: a country and the regiment types to test with");
		if (country == nullptr || infantry == nullptr || cavalry == nullptr || artillery == nullptr || skirmishers == nullptr || rifled == nullptr) {
			return;
		}
		StaticModifierCache const& static_modifier_cache = im.definition_manager.get_modifier_manager().get_static_modifier_cache();
		const auto refresh = [&]() {
			country->update_modifier_sum(im.get_today(), static_modifier_cache);
		};
		const auto cell = [](RegimentType const& regiment_type) -> std::uint8_t {
			return static_cast<std::uint8_t>(get_division_slot_kind(regiment_type) + 1);
		};
		const auto make_template = [](DivisionGrid const& grid) -> DivisionTemplate {
			DivisionTemplate division_template;
			division_template.cells = grid;
			division_template.rebuild_counts();
			return division_template;
		};
		const auto grid_with = [&cell](RegimentType const& regiment_type, const std::size_t ranks, const std::size_t rows) -> DivisionGrid {
			DivisionGrid grid {};
			for (std::size_t rank = 0; rank < ranks; ++rank) {
				for (std::size_t row = 0; row < rows; ++row) {
					grid[DivisionTemplate::cell_index(rank, row)] = cell(regiment_type);
				}
			}
			return grid;
		};

		// The state to put back
		std::vector<memory::string> keys_before { country->get_unlock_keys().begin(), country->get_unlock_keys().end() };
		const std::uint32_t applied_before = country->get_provisional_unlock_steps_applied();
		refresh();

		// ---- the standard template
		const DivisionTemplate standard = make_template(make_standard_division_grid(unit_types));
		const DivisionStats standard_stats = compute_division_stats(im, *country, standard);
		// The first row weighs 1 and the second 1/2: every regiment counts 1.5
		// the weapons the country gives to the types (line infantry: rifle_0; the cavalry: rifle_1 from the provisional unlocks)
		const auto weapon_of = [&](RegimentType const& regiment_type) {
			return choose_unit_weapon(equipment, *country, regiment_type, get_allowed_weapons(equipment, regiment_type), war.equipment_below_min_penalty.fixed());
		};
		const UnitWeapon infantry_weapon = weapon_of(*infantry);
		const UnitWeapon cavalry_weapon = weapon_of(*cavalry);
		const fixed_point_t expected_attack = (infantry->attack * infantry_weapon.effective->attack_mult * infantry_weapon.keep * 2
			+ cavalry->attack * cavalry_weapon.effective->attack_mult * cavalry_weapon.keep) * 3 / 2;
		const fixed_point_t expected_defence = (infantry->defence * infantry_weapon.effective->defense_mult * infantry_weapon.keep * 2
			+ cavalry->defence * cavalry_weapon.effective->defense_mult * cavalry_weapon.keep) * 3 / 2;
		r.check(
			standard_stats.regiments == 6 && standard_stats.width == 3 && standard_stats.depth == fixed_point_t(2),
			"division stats: the standard template has 6 regiments, a front of 3 ranks and a depth of 2",
			fmt::format("{} regiments, width {}, depth {}", standard_stats.regiments, standard_stats.width, dbl(standard_stats.depth))
		);
		// the density changes them (the standard template: 2 x 2 / 3 = 1.33, dense: the attack grows)
		const battle_formulas::DensityEffect standard_density = battle_formulas::density_effect(war, standard_stats.density);
		r.check(
			near(standard_stats.attack, expected_attack * standard_density.attack_mult, fixed_point_t(1) / 100)
				&& near(standard_stats.defence, expected_defence * standard_density.defence_mult, fixed_point_t(1) / 100),
			"division stats: the attack and the defence add up the regiments by the weights of their rows and the multipliers of their weapons (and the density)",
			fmt::format("attack {} of {}, defence {} of {}", dbl(standard_stats.attack), dbl(expected_attack), dbl(standard_stats.defence), dbl(expected_defence))
		);
		r.check(
			standard_stats.men == infantry->max_strength * 4000 + cavalry->max_strength * 2000
				|| near(standard_stats.men, (infantry->max_strength * 4 + cavalry->max_strength * 2) * war.template_men_per_strength, fixed_point_t(1)),
			"division stats: the men are the strength of the regiments times the men of a strength point"
		);
		r.check(
			standard_stats.speed == std::min(infantry->maximum_speed, cavalry->maximum_speed) && standard_stats.weight > fixed_point_t::_0
				&& standard_stats.organisation > fixed_point_t::_0,
			"division stats: the speed is the one of the slowest regiment, the weight and the organisation are there"
		);
		r.check(!standard_stats.cost.empty() && !standard_stats.supply_per_day.empty(), "division stats: the cost and the supplies of the regiments are listed");

		// ---- the width: a division twice as wide deals twice the damage at the same depth
		const DivisionStats narrow = compute_division_stats(im, *country, make_template(grid_with(*infantry, 1, 2)));
		const DivisionStats wide = compute_division_stats(im, *country, make_template(grid_with(*infantry, 2, 2)));
		r.check(wide.width == 2 * narrow.width && wide.depth == narrow.depth, "division stats: twice the ranks, the same depth");
		// without the density (which depends on the width) the attack and the defence are in proportion to the width
		const auto without_density = [&](DivisionStats const& stats, const bool attack) -> fixed_point_t {
			const battle_formulas::DensityEffect effect = battle_formulas::density_effect(war, stats.density);
			return attack ? stats.attack / effect.attack_mult : stats.defence / effect.defence_mult;
		};
		r.check(
			near(without_density(wide, true), without_density(narrow, true) * 2, fixed_point_t(1) / 100)
				&& near(without_density(wide, false), without_density(narrow, false) * 2, fixed_point_t(1) / 100),
			"division stats: the attack and the defence (before the density) are in proportion to the width at the same depth"
		);
		const DivisionStats target = compute_division_stats(im, *country, standard);
		// A protection the attack never reaches (a quarter of the attack goes through in both cases): the damage is in proportion to the attack
		const fixed_point_t damage_narrow = battle_formulas::battle_hit(war, narrow.attack, fixed_point_t::_1, fixed_point_t::_1, fixed_point_t(1000), fixed_point_t::_1).damage;
		const fixed_point_t damage_wide = battle_formulas::battle_hit(war, wide.attack, fixed_point_t::_1, fixed_point_t::_1, fixed_point_t(1000), fixed_point_t::_1).damage;
		r.check(
			damage_narrow > fixed_point_t::_0 && near(damage_wide / wide.attack, damage_narrow / narrow.attack, fixed_point_t(1) / 1000),
			"battle: the damage a division deals is in proportion to its attack (so to its width, its density counted)",
			fmt::format("{} and {}", dbl(damage_narrow), dbl(damage_wide))
		);

		// ---- the depth is the durability: more rows last longer, a rank of one row and a rank of two give a depth of 1.5
		const DivisionStats shallow = compute_division_stats(im, *country, make_template(grid_with(*infantry, 2, 1)));
		const DivisionStats deep = compute_division_stats(im, *country, make_template(grid_with(*infantry, 2, 2)));
		r.check(near(deep.durability, shallow.durability * 2, fixed_point_t(1) / 100), "division stats: twice the depth, twice the durability");
		DivisionGrid uneven = grid_with(*infantry, 1, 2);
		uneven[DivisionTemplate::cell_index(1, 0)] = cell(*infantry);
		const DivisionStats uneven_stats = compute_division_stats(im, *country, make_template(uneven));
		r.check(uneven_stats.depth == fixed_point_t(3) / 2 && uneven_stats.width == 2, "division stats: the depth is the average of the ranks (1.5 for 2 and 1)");
		// the density: 2 x depth / width (a width twice the depth is 1.0), the standard template 6 regiments in 3 ranks of depth 2 is 4/3
		r.check(
			near(standard_stats.raw_density, fixed_point_t(4) / 3, fixed_point_t(1) / 1000) && near(battle_formulas::raw_density(war, 8, 4), fixed_point_t::_1, fixed_point_t(1) / 1000)
				&& near(narrow.raw_density, fixed_point_t(4), fixed_point_t(1) / 1000),
			"division stats: the density is 2 x depth / width (a width twice the depth is 1.0)", fmt::format("{} {}", dbl(standard_stats.raw_density), dbl(narrow.raw_density))
		);

		// ---- the weapons of a better generation: the attack, the defence and the goods change
		const DivisionTemplate infantry_template = make_template(grid_with(*skirmishers, 2, 2)); // may have rifle_2 and rifle_3
		const DivisionStats first_generation = compute_division_stats(im, *country, infantry_template);
		country->grant_unlock("equipment:rifle_3");
		const DivisionStats third_generation = compute_division_stats(im, *country, infantry_template);
		EquipmentDefinition const* const rifle_1 = equipment.find_equipment("rifle_1");
		EquipmentDefinition const* const rifle_3 = equipment.find_equipment("rifle_3");
		if (rifle_1 != nullptr && rifle_3 != nullptr) {
			// before: no allowed rifle, rifle_1 with the penalty of one generation; after: rifle_3
			const fixed_point_t ratio = rifle_3->attack_mult / (rifle_1->attack_mult * (fixed_point_t::_1 - war.equipment_below_min_penalty.fixed()));
			r.check(
				near(third_generation.attack, first_generation.attack * ratio, fixed_point_t(1) / 50),
				"division stats: a rifle of the third generation gives the attack in proportion to its points",
				fmt::format("{} against {} x {}", dbl(third_generation.attack), dbl(first_generation.attack), dbl(ratio))
			);
			r.check(third_generation.defence > first_generation.defence && third_generation.durability > first_generation.durability,
				"division stats: a better rifle gives more defence and a more durable division");
			const auto has_good = [](memory::vector<DivisionGoodAmount> const& list, std::string_view good) -> bool {
				return std::any_of(list.begin(), list.end(), [good](DivisionGoodAmount const& entry) { return entry.good == good && entry.amount > fixed_point_t::_0; });
			};
			r.check(
				has_good(third_generation.cost, "rifle_3") && !has_good(third_generation.cost, "rifle_2")
					&& has_good(first_generation.cost, "rifle_2") && !has_good(first_generation.cost, "rifle_3"),
				"division stats: the cost lists the rifles the unit is built with, or the better ones of its list that the country has"
			);
			r.check(has_good(third_generation.supply_per_day, "rifle_3") && has_good(third_generation.ammo_per_day, "ammunition"),
				"division stats: the supplies of the regiments and the use of ammunition list the rifles of the generation");
			// A division that has the weapon in half: in between
			const DivisionStats half_equipped = compute_division_stats(im, *country, infantry_template, fixed_point_t::_0_50);
			r.check(
				half_equipped.attack > first_generation.attack && half_equipped.attack < third_generation.attack
					&& half_equipped.reliability == fixed_point_t::_0_50,
				"division stats: a division with half of the weapons has the attack between the two generations and a reliability of one half"
			);
		}
		country->revoke_unlock("equipment:rifle_3");

		// ---- the artillery has its supplies too (the guns of the generation, canned food)
		const DivisionStats artillery_first = compute_division_stats(im, *country, make_template(grid_with(*rifled, 1, 1)));
		country->grant_unlock("equipment:field_gun_3");
		const DivisionStats artillery_third = compute_division_stats(im, *country, make_template(grid_with(*rifled, 1, 1)));
		const auto amount_of = [](memory::vector<DivisionGoodAmount> const& list, std::string_view good) -> fixed_point_t {
			for (DivisionGoodAmount const& entry : list) {
				if (entry.good == good) {
					return entry.amount;
				}
			}
			return fixed_point_t::_0;
		};
		r.check(
			amount_of(artillery_first.supply_per_day, "field_gun_2") > fixed_point_t::_0 && amount_of(artillery_first.supply_per_day, "canned_food") > fixed_point_t::_0,
			"division stats: the supplies of artillery are listed (the guns and the food)",
			fmt::format("{} supply entries, field_gun_1 {}, canned_food {}, regiments {}", artillery_first.supply_per_day.size(),
				dbl(amount_of(artillery_first.supply_per_day, "field_gun_2")), dbl(amount_of(artillery_first.supply_per_day, "canned_food")), artillery_first.regiments)
		);
		r.check(
			amount_of(artillery_third.supply_per_day, "field_gun_3") == amount_of(artillery_first.supply_per_day, "field_gun_2")
				&& amount_of(artillery_third.supply_per_day, "field_gun_2") == fixed_point_t::_0
				&& artillery_third.attack > artillery_first.attack,
			"division stats: with a gun of the third generation the supplies are of this gun and the attack is more"
		);
		country->revoke_unlock("equipment:field_gun_3");

		// ---- the effects of the technologies: the shift of the rows and the durability
		Modifier const* shift_modifier = nullptr;
		for (ProvisionalUnlockStep const& step : provisional.get_steps()) {
			if (step.modifier.has_value() && step.year == 1895) {
				shift_modifier = &*step.modifier;
			}
		}
		if (shift_modifier != nullptr) {
			country->add_unlock_modifier(*shift_modifier);
			refresh();
			const DivisionStats shifted = compute_division_stats(im, *country, make_template(grid_with(*infantry, 1, 2)));
			// the second row weighs as the first: (1 + 1) against (1 + 1/2)
			r.check(
				near(shifted.attack * 3, narrow.attack * 4, fixed_point_t(1) / 50),
				"division stats: the formation of the technologies (row_weight_shift) makes the second row weigh as the first",
				fmt::format("{} against {}", dbl(shifted.attack), dbl(narrow.attack))
			);
			country->remove_unlock_modifier(*shift_modifier);
			refresh();
		}

		// ---- put the country back
		for (memory::string const& key : std::vector<memory::string> { country->get_unlock_keys().begin(), country->get_unlock_keys().end() }) {
			country->revoke_unlock(key);
		}
		for (memory::string const& key : keys_before) {
			country->grant_unlock(key);
		}
		country->set_provisional_unlock_steps_applied(applied_before);
		refresh();
	}

	// ---- the production types of the factories of weapons (ov_data/common/ov_production_types)

	void test_production_types(Results& r, InstanceManager& im) {
		BuildingTypeManager const& building_types = im.definition_manager.get_economy_manager().get_building_type_manager();
		EquipmentManager const& equipment = im.definition_manager.get_military_manager().get_equipment_manager();
		if (equipment.get_equipment().empty()) {
			fmt::println("TEST INFO production types: no ov_data folder, not checked");
			return;
		}
		BuildingType const* const rifle_building = building_types.get_building_type_by_identifier("small_arms_factory");
		BuildingType const* const gun_building = building_types.get_building_type_by_identifier("artillery_factory");
		r.check(rifle_building != nullptr && gun_building != nullptr, "production types: the factories of rifles and of guns exist");
		if (rifle_building == nullptr || gun_building == nullptr) {
			return;
		}

		// ---- the options of the buildings: a type for every generation, from the files
		const auto rifle_options = get_factory_production_options(im, *rifle_building);
		const auto gun_options = get_factory_production_options(im, *gun_building);
		bool ordered = rifle_options.size() == 5 && gun_options.size() == 5;
		for (std::size_t i = 0; ordered && i < 5; ++i) {
			ordered = rifle_options[i].tier == i && gun_options[i].tier == i
				&& rifle_options[i].production_type->output_good.get_identifier() == fmt::format("rifle_{}", i)
				&& gun_options[i].production_type->output_good.get_identifier() == fmt::format("field_gun_{}", i);
		}
		r.check(ordered, "production types: a factory of rifles and a factory of guns have a type for each of the five generations (0 to 4), the lowest first",
			fmt::format("{} and {} options", rifle_options.size(), gun_options.size()));
		r.check(
			rifle_options.size() == 5 && rifle_options[0].production_type == rifle_building->production_type,
			"production types: the first generation is the type of the building (the old small arms recipe)"
		);
		bool recipes_cost_more = rifle_options.size() == 5 && gun_options.size() == 5;
		for (std::size_t i = 1; recipes_cost_more && i < 5; ++i) {
			const auto input_value = [&im](ProductionType const& production_type) -> fixed_point_t {
				fixed_point_t value = fixed_point_t::_0;
				for (auto const& [good, quantity] : production_type.input_goods) {
					value += quantity * good->base_price;
				}
				return value;
			};
			recipes_cost_more = input_value(*rifle_options[i].production_type) > input_value(*rifle_options[i - 1].production_type)
				&& input_value(*gun_options[i].production_type) > input_value(*gun_options[i - 1].production_type);
		}
		r.check(recipes_cost_more, "production types: the recipe of a newer generation needs dearer inputs");

		// ---- what a country can choose follows its unlocks
		CountryInstance* const owner = [&im, rifle_building]() -> CountryInstance* {
			for (StateSet const& state_set : im.get_map_instance().get_state_manager().get_state_sets()) {
				for (State const& state : state_set.get_states()) {
					if (state.get_owner() != nullptr && state.get_capital() != nullptr && state.find_factory(*rifle_building) != nullptr) {
						return state.get_owner();
					}
				}
			}
			return nullptr;
		}();
		r.check(owner != nullptr, "production types: a country with a factory of rifles exists");
		if (owner == nullptr) {
			return;
		}
		State* state = nullptr;
		for (State* const candidate : owner->get_states()) {
			if (candidate->get_capital() != nullptr && candidate->find_factory(*rifle_building) != nullptr) {
				state = candidate;
				break;
			}
		}
		if (state == nullptr) {
			return;
		}
		const std::vector<memory::string> keys_before { owner->get_unlock_keys().begin(), owner->get_unlock_keys().end() };
		const std::uint32_t applied_before = owner->get_provisional_unlock_steps_applied();

		ProductionType const* best = get_best_production_type(im, *owner, *rifle_building);
		r.check(best == rifle_options[1].production_type, "production types: in 1836 the best type is the one of rifle_1 (the provisional unlocks), the tier 0 stays available",
			best != nullptr ? std::string(best->get_identifier()) : std::string("none"));
		r.check(
			is_production_option_available(*owner, rifle_options[0]) && is_production_option_available(*owner, rifle_options[1])
				&& !is_production_option_available(*owner, rifle_options[2]) && !is_production_option_available(*owner, rifle_options[4]),
			"production types: a type is available when the country has the unlock of its weapon"
		);
		owner->grant_unlock("equipment:rifle_3");
		best = get_best_production_type(im, *owner, *rifle_building);
		r.check(best == rifle_options[3].production_type, "production types: with the third generation the best type is the one of rifle_3");

		// ---- the command: switching a factory takes one tick and costs nothing
		const province_index_t province = state->get_capital()->index;
		const building_type_index_t type = rifle_building->index;
		FactoryInstance* factory = state->find_factory(*rifle_building);
		const fixed_point_t efficiency_before = factory->get_efficiency();
		ProductionType const* const type_before = factory->production_type; // the AI may have moved it to the first generation already
		const bool had_machine_parts_before = std::any_of(factory->get_inputs().begin(), factory->get_inputs().end(), [](FactoryInstance::Input const& input) {
			return input.good.get_identifier() == "machine_parts";
		});
		im.queue_game_action<set_factory_production_type_argument_t>(owner->index, province, type, std::uint32_t(4));
		im.force_tick_and_update();
		factory = state->find_factory(*rifle_building);
		r.check(factory->production_type == type_before, "production types: a type whose weapon the country does not have is refused");
		im.queue_game_action<set_factory_production_type_argument_t>(owner->index, province, type, std::uint32_t(3));
		im.force_tick_and_update();
		factory = state->find_factory(*rifle_building);
		r.check(
			factory->production_type == rifle_options[3].production_type && factory->get_pending_production_type() == nullptr,
			"production types: the factory makes the type that was chosen after one tick"
		);
		const bool machine_parts_now = std::any_of(factory->get_inputs().begin(), factory->get_inputs().end(), [](FactoryInstance::Input const& input) {
			return input.good.get_identifier() == "machine_parts";
		});
		(void)had_machine_parts_before;
		r.check(machine_parts_now && factory->get_inputs().size() == rifle_options[3].production_type->input_goods.size(),
			"production types: the inputs of the factory are the ones of the new recipe");
		r.check(factory->get_efficiency() >= efficiency_before * 9 / 10, "production types: the switch costs no efficiency (no retooling)");
		const std::uint64_t hash_switched = StateHash::compute(im);
		im.queue_game_action<set_factory_production_type_argument_t>(owner->index, province, type, std::uint32_t(0));
		im.force_tick_and_update();
		r.check(
			state->find_factory(*rifle_building)->production_type == rifle_options[0].production_type,
			"production types: an old generation stays available while the country has its unlock (it is switched back by hand)"
		);
		r.check(StateHash::compute(im) != hash_switched, "production types: the type of a factory is in the state hash");
		im.queue_game_action<set_factory_production_type_argument_t>(owner->index, province, type, std::uint32_t(99));
		im.force_tick_and_update();
		r.check(state->find_factory(*rifle_building)->production_type == rifle_options[0].production_type, "production types: an option that does not exist is refused");

		// ---- the simple choice of the AI: the newest generation the country has
		choose_ai_production_types(im, *owner);
		im.force_tick_and_update();
		r.check(
			state->find_factory(*rifle_building)->production_type == rifle_options[3].production_type,
			"production types: the AI puts the factories of weapons on the newest generation the country has"
		);

		// ---- put the country back
		for (memory::string const& key : std::vector<memory::string> { owner->get_unlock_keys().begin(), owner->get_unlock_keys().end() }) {
			owner->revoke_unlock(key);
		}
		for (memory::string const& key : keys_before) {
			owner->grant_unlock(key);
		}
		owner->set_provisional_unlock_steps_applied(applied_before);
		im.queue_game_action<set_factory_production_type_argument_t>(owner->index, province, type, std::uint32_t(0));
		im.force_tick_and_update();
	}

	// ---- the weapons of the regiments (CountryInstance::update_regiment_equipment, military/UnitEquipment.hpp)

	void test_regiment_equipment(Results& r, InstanceManager& im) {
		EquipmentManager const& equipment = im.definition_manager.get_military_manager().get_equipment_manager();
		GoodDefinitionManager const& goods = im.definition_manager.get_economy_manager().get_good_definition_manager();
		UnitTypeManager const& unit_types = im.definition_manager.get_military_manager().get_unit_type_manager();
		OpenVicWarDefines const& war = im.definition_manager.get_define_manager().get_openvic_defines().war;
		if (equipment.get_equipment().empty()) {
			fmt::println("TEST INFO regiment equipment: no ov_data folder, not checked");
			return;
		}
		GoodDefinition const* const rifle_0_good = goods.get_good_definition_by_identifier("rifle_0");
		GoodDefinition const* const rifle_1_good = goods.get_good_definition_by_identifier("rifle_1");
		EquipmentDefinition const* const rifle_0 = equipment.find_equipment("rifle_0");
		EquipmentDefinition const* const rifle_1 = equipment.find_equipment("rifle_1");
		EquipmentDefinition const* const rifle_2 = equipment.find_equipment("rifle_2");
		RegimentType const* const dragoons = unit_types.get_regiment_type_by_identifier("dragoons");
		RegimentType const* const line_infantry = unit_types.get_regiment_type_by_identifier("line_infantry");
		RegimentType const* const cavalry_type = unit_types.get_regiment_type_by_identifier("cavalry");
		if (rifle_0_good == nullptr || rifle_1_good == nullptr || rifle_0 == nullptr || rifle_1 == nullptr || rifle_2 == nullptr || dragoons == nullptr
			|| line_infantry == nullptr || cavalry_type == nullptr) {
			r.check(false, "regiment equipment: the rifles, their goods and the unit types exist");
			return;
		}

		// A country with a few regiments of cavalry (they may have rifle_0 and rifle_1)
		CountryInstance* country = nullptr;
		std::vector<RegimentInstance*> cavalry;
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (!candidate.exists() || candidate.is_at_war()) {
				continue;
			}
			std::vector<RegimentInstance*> found;
			for (ArmyInstance& army : candidate.get_armies()) {
				for (RegimentInstance& regiment : army.get_regiment_instances()) {
					if (&regiment.get_regiment_type() == cavalry_type) {
						found.push_back(&regiment);
					}
				}
			}
			if (found.size() >= 4) {
				country = &candidate;
				cavalry = std::move(found);
				break;
			}
		}
		r.check(country != nullptr, "regiment equipment: a country with four regiments of cavalry exists");
		if (country == nullptr) {
			return;
		}
		const fixed_point_t pieces = [&]() -> fixed_point_t {
			for (auto const& [good, quantity] : cavalry_type->get_build_cost()) {
				if (good == rifle_0_good) {
					return quantity;
				}
			}
			return fixed_point_t::_0;
		}();
		r.check(pieces > fixed_point_t::_0, "regiment equipment: a regiment of cavalry is built with rifles (the pieces of its weapon)");

		const std::vector<memory::string> keys_before { country->get_unlock_keys().begin(), country->get_unlock_keys().end() };
		const fixed_point_t stock_before = country->get_good_data(*rifle_1_good).stockpile_amount;
		const fixed_point_t penalty = war.equipment_below_min_penalty.fixed();

		// ---- the unit is built with the lowest weapon of its list; the country has rifle_1 too (the provisional unlocks)
		country->grant_unlock("equipment:rifle_1");
		country->get_good_data(*rifle_1_good).stockpile_amount = fixed_point_t::_0;
		im.force_tick_and_update();
		bool all_first = true;
		for (RegimentInstance* const regiment : cavalry) {
			all_first &= regiment->get_weapon() == nullptr && near(regiment->get_weapon_attack_ratio(), rifle_0->attack_mult, fixed_point_t(1) / 100)
				&& near(regiment->get_weapon_breakthrough_ratio(), rifle_0->breakthrough_mult, fixed_point_t(1) / 100);
		}
		r.check(all_first, "regiment equipment: a regiment has the weapon it was built with (rifle_0, multipliers 0.7) until it is given a better one");
		r.check(
			country->get_good_data(*rifle_1_good).government_needs >= pieces,
			"regiment equipment: without the rifles in the stockpile the government needs them (it buys them at the market)",
			fmt::format("needs {}", dbl(country->get_good_data(*rifle_1_good).government_needs))
		);

		// ---- the stock has the rifles: a day gives the best allowed weapon to as many regiments as ov_war.EQUIPMENT_UPGRADES_PER_DAY says (the regiments
		// that had rifle_1 before, from the stockpile the country bought in the days of the other tests, do not count)
		std::size_t had_rifle_1 = 0;
		for (ArmyInstance& army : country->get_armies()) {
			for (RegimentInstance& regiment : army.get_regiment_instances()) {
				had_rifle_1 += regiment.get_weapon() == rifle_1;
			}
		}
		country->get_good_data(*rifle_1_good).stockpile_amount = fixed_point_t(1000);
		im.force_tick_and_update();
		std::size_t with_rifle_1 = 0;
		RegimentInstance* equipped = nullptr;
		// the regiments of all the types of the country that may have rifle_1 (the day gives the weapon to the first ones)
		for (ArmyInstance& army : country->get_armies()) {
			for (RegimentInstance& regiment : army.get_regiment_instances()) {
				if (regiment.get_weapon() == rifle_1) {
					with_rifle_1++;
					equipped = &regiment;
				}
			}
		}
		r.check(
			with_rifle_1 - had_rifle_1 == static_cast<std::size_t>(war.equipment_upgrades_per_day) && equipped != nullptr && equipped->get_weapon_good() == rifle_1_good,
			"regiment equipment: a day gives the better weapon to as many regiments as ov_war.EQUIPMENT_UPGRADES_PER_DAY says",
			fmt::format("{} regiments, stock {}, needs {}, pieces {}, upgrades {}, best {}", with_rifle_1, dbl(country->get_good_data(*rifle_1_good).stockpile_amount),
				dbl(country->get_good_data(*rifle_1_good).government_needs), dbl(pieces), war.equipment_upgrades_per_day,
				choose_unit_weapon(equipment, *country, *cavalry_type, get_allowed_weapons(equipment, *cavalry_type), war.equipment_below_min_penalty.fixed()).best != nullptr ? "yes" : "no")
		);
		if (equipped != nullptr) {
			r.check(
				near(equipped->get_weapon_attack_ratio(), rifle_1->attack_mult, fixed_point_t(1) / 100)
					&& near(equipped->get_weapon_defence_ratio(), rifle_1->defense_mult, fixed_point_t(1) / 100)
					&& near(equipped->get_weapon_breakthrough_ratio(), rifle_1->breakthrough_mult, fixed_point_t(1) / 100),
				"regiment equipment: its attack, defence and breakthrough are the multipliers of the new weapon"
			);
			r.check(
				country->get_good_data(*rifle_1_good).army_needs > fixed_point_t::_0,
				"regiment equipment: the supplies of the regiment are of its weapon (the rifles of the first generation, not rifle_0)"
			);
		}
		r.check(
			country->get_good_data(*rifle_1_good).stockpile_amount < fixed_point_t(1000),
			"regiment equipment: the weapons come out of the national stockpile (they are not made from the air)"
		);
		// The weapon is never better than the list of the type allows: line infantry (rifle_0 only) is not given rifle_1
		{
			const auto allowed = get_allowed_weapons(equipment, *line_infantry);
			r.check(allowed.size() == 1 && allowed.front() == rifle_0, "regiment equipment: line infantry may have rifle_0 only (the list of its file)");
			const UnitWeapon choice = choose_unit_weapon(equipment, *country, *line_infantry, allowed, penalty);
			r.check(choice.best == rifle_0 && choice.effective == rifle_0 && choice.keep == fixed_point_t::_1, "regiment equipment: it uses rifle_0 even when the country has better rifles");
		}

		// ---- a type with none of the allowed weapons: the nearest lower generation, 25 % less for every generation below the lowest allowed one
		{
			const auto allowed = get_allowed_weapons(equipment, *dragoons); // rifle_2 and rifle_3
			r.check(allowed.size() == 2 && allowed.front() == rifle_2, "regiment equipment: dragoons may have rifle_2 and rifle_3");
			// the country has rifle_0 and rifle_1: one generation below rifle_2
			UnitWeapon choice = choose_unit_weapon(equipment, *country, *dragoons, allowed, penalty);
			r.check(
				choice.best == nullptr && choice.effective == rifle_1 && near(choice.keep, fixed_point_t(3) / 4, fixed_point_t(1) / 1000),
				"regiment equipment: dragoons with rifle_1 only use it with 25 % less (one generation below the lowest allowed)"
			);
			// only rifle_0: two generations below
			country->revoke_unlock("equipment:rifle_1");
			choice = choose_unit_weapon(equipment, *country, *dragoons, allowed, penalty);
			r.check(
				choice.effective == rifle_0 && near(choice.keep, fixed_point_t::_0_50, fixed_point_t(1) / 1000),
				"regiment equipment: with rifle_0 only the penalty is 25 % for each of the two generations (50 % left)"
			);
			// rifle_2: the lowest allowed one
			country->grant_unlock("equipment:rifle_2");
			choice = choose_unit_weapon(equipment, *country, *dragoons, allowed, penalty);
			r.check(choice.best == rifle_2 && choice.keep == fixed_point_t::_1, "regiment equipment: with rifle_2 the dragoons have no penalty");
			// rifle_3 is allowed too: the best
			country->grant_unlock("equipment:rifle_3");
			choice = choose_unit_weapon(equipment, *country, *dragoons, allowed, penalty);
			r.check(choice.best != nullptr && choice.best->tier == 3, "regiment equipment: with rifle_3 the dragoons use it (the best of the list)");
		}

		// ---- put the country back
		for (ArmyInstance& army : country->get_armies()) {
			for (RegimentInstance& regiment : army.get_regiment_instances()) {
				regiment.set_weapon(nullptr);
				regiment.set_weapon_base_good(nullptr);
				regiment.set_weapon_good(nullptr);
				regiment.set_weapon_attack_ratio(fixed_point_t::_1);
				regiment.set_weapon_defence_ratio(fixed_point_t::_1);
				regiment.set_weapon_breakthrough_ratio(fixed_point_t::_1);
			}
		}
		for (memory::string const& key : std::vector<memory::string> { country->get_unlock_keys().begin(), country->get_unlock_keys().end() }) {
			country->revoke_unlock(key);
		}
		for (memory::string const& key : keys_before) {
			country->grant_unlock(key);
		}
		country->get_good_data(*rifle_1_good).stockpile_amount = stock_before;
	}

	// ---- the unit types of ov_data/common/ov_units

	void test_unit_types(Results& r, InstanceManager& im) {
		namespace fs = std::filesystem;
		UnitTypeManager const& unit_types = im.definition_manager.get_military_manager().get_unit_type_manager();
		EquipmentManager const& equipment = im.definition_manager.get_military_manager().get_equipment_manager();
		GoodDefinitionManager const& goods = im.definition_manager.get_economy_manager().get_good_definition_manager();
		ModifierEffectCache const& cache = im.definition_manager.get_modifier_manager().get_modifier_effect_cache();
		const auto type = [&](std::string_view id) { return unit_types.get_regiment_type_by_identifier(id); };
		for (std::string_view const id : { "line_infantry", "infantry", "skirmisher_infantry", "modern_infantry", "cavalry", "hussars", "dragoons", "cuirassiers",
			"napoleonic_artillery", "artillery", "rifled_artillery", "fragmentation_artillery" }) {
			if (type(id) == nullptr) {
				fmt::println("TEST INFO unit types: no ov_data folder or a unit is missing ({}), not checked", id);
				return;
			}
		}
		r.check(true, "unit types: the twelve types of the starting set are loaded from files");
		r.check(type("infantry")->ov.unlock_key == "unit:infantry" && type("hussars")->ov.unlock_key == "unit:hussars"
				&& type("line_infantry")->ov.unlock_key.empty() && type("napoleonic_artillery")->ov.unlock_key.empty() && type("cavalry")->ov.unlock_key.empty(),
			"unit types: line infantry, cavalry and napoleonic artillery are open from the start, the others have a key");
		// The armies of the start name the units of Vic2: infantry is line infantry (the alias)
		// (the AI may recruit the new types its technologies open, so the line infantry of the start is only looked for)
		bool line_infantry_in_armies = false;
		for (CountryInstance& country : im.get_country_instance_manager().get_country_instances()) {
			for (ArmyInstance& army : country.get_armies()) {
				for (RegimentInstance const& regiment : army.get_regiment_instances()) {
					line_infantry_in_armies |= regiment.get_regiment_type().get_identifier() == "line_infantry";
				}
			}
		}
		r.check(line_infantry_in_armies, "unit types: the armies of the start have line infantry (the alias of the infantry of Vic2)");
		// The key opens the type
		CountryInstance* country = nullptr;
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (candidate.exists()) {
				country = &candidate;
			}
		}
		const std::vector<memory::string> keys_before { country->get_unlock_keys().begin(), country->get_unlock_keys().end() };
		country->revoke_unlock("unit:hussars");
		r.check(!country->is_unit_type_unlocked(*type("hussars")) && country->is_unit_type_unlocked(*type("line_infantry")),
			"unit types: without the key a type is not available (line infantry, with no key, is)");
		country->grant_unlock("unit:hussars");
		r.check(country->is_unit_type_unlocked(*type("hussars")), "unit types: with the key unit:hussars the type is available");
		// The dragoons get half of the bonuses of the infantry and half of the cavalry
		{
			const fs::path temp = fs::temp_directory_path() / "openvic_selftest_units";
			std::error_code ec;
			fs::remove_all(temp, ec);
			write_test_file(temp / "m.txt", "step = { year = 1836 modifier = { attack_mult = { infantry = 0.10 cavalry = 0.02 } } }\n");
			ProvisionalUnlocks modifiers;
			const ovdl::v2script::Parser parser = Dataloader::parse_defines(temp / "m.txt");
			modifiers.load_file("m.txt", parser.get_file_node(), im.definition_manager.get_modifier_manager());
			if (modifiers.get_steps().size() == 1) {
				country->add_unlock_modifier(*modifiers.get_steps()[0].modifier);
				country->update_modifier_sum(im.get_today(), im.definition_manager.get_modifier_manager().get_static_modifier_cache());
				const auto sum_for = [&](RegimentType const& regiment_type) {
					return get_scoped_sum(*country, cache, scoped_effect_t::ATTACK_MULT, get_scope_weights(cache, regiment_type));
				};
				r.check(
					near(sum_for(*type("dragoons")), fixed_point_t(6) / 100, fixed_point_t(1) / 1000)
						&& near(sum_for(*type("line_infantry")), fixed_point_t(10) / 100, fixed_point_t(1) / 1000)
						&& near(sum_for(*type("hussars")), fixed_point_t(2) / 100, fixed_point_t(1) / 1000),
					"unit types: the dragoons get half of the bonus of the infantry and half of the cavalry (0.05 + 0.01), the infantry 0.10, the hussars 0.02",
					fmt::format("{} {} {}", dbl(sum_for(*type("dragoons"))), dbl(sum_for(*type("line_infantry"))), dbl(sum_for(*type("hussars"))))
				);
				country->remove_unlock_modifier(*modifiers.get_steps()[0].modifier);
				country->update_modifier_sum(im.get_today(), im.definition_manager.get_modifier_manager().get_static_modifier_cache());
			}
			fs::remove_all(temp, ec);
		}
		// The stats of the types
		r.check(type("hussars")->maximum_speed > type("cavalry")->maximum_speed && type("hussars")->reconnaissance > type("cavalry")->reconnaissance
				&& type("hussars")->ov.occupation_weight > type("cavalry")->ov.occupation_weight,
			"unit types: the hussars are faster than the cavalry, with a better reconnaissance and a bigger occupation weight");
		r.check(type("cuirassiers")->maximum_speed < type("cavalry")->maximum_speed && type("cuirassiers")->get_breakthrough() > type("cavalry")->get_breakthrough()
				&& type("cuirassiers")->max_strength > type("cavalry")->max_strength && type("cuirassiers")->reconnaissance < type("cavalry")->reconnaissance
				&& type("cuirassiers")->ov.occupation_weight < type("cavalry")->ov.occupation_weight,
			"unit types: the cuirassiers are slower, more durable, with a better breakthrough and a worse reconnaissance and occupation than the cavalry");
		bool iron_in_cost = false;
		for (auto const& [good, quantity] : type("cuirassiers")->get_build_cost()) {
			iron_in_cost |= good->get_identifier() == "iron" && quantity > fixed_point_t::_0;
		}
		r.check(iron_in_cost, "unit types: the cuirassiers are built with iron");
		r.check(type("artillery")->ov.occupation_weight == fixed_point_t::_0_50 && type("line_infantry")->ov.occupation_weight == fixed_point_t::_1
				&& type("dragoons")->ov.occupation_weight == fixed_point_t(3) / 2,
			"unit types: the weights of the occupation of the artillery, the infantry and the dragoons are 0.5, 1, 1.5");
		r.check(type("cavalry")->ov.upgrades_to == "hussars" && !type("cavalry")->ov.upgrade_cost.empty(), "unit types: the cavalry can be upgraded to hussars for a price from the data");

		// A new type comes with a file only, a broken one is skipped
		{
			const fs::path temp = fs::temp_directory_path() / "openvic_selftest_unit_files";
			std::error_code ec;
			fs::remove_all(temp, ec);
			write_test_file(temp / "good.txt",
				"guard_test = { category = infantry equipment = { rifle_0 } icon = 1 sprite = Infantry occupation_weight = 1.2\n"
				" base_stats = { attack = 8 defense = 2 breakthrough = 9 organisation = 30 speed = 4 manpower = 3 cost = { rifle_0 = 10 } supply_cost = { rifle_0 = 0.02 ammunition = 0.1 } } }\n");
			write_test_file(temp / "bad.txt",
				"bad_weapon = { category = infantry equipment = { no_such_rifle } icon = 1 sprite = Infantry\n"
				" base_stats = { attack = 8 defense = 2 breakthrough = 9 organisation = 30 speed = 4 manpower = 3 cost = { rifle_0 = 10 } supply_cost = { rifle_0 = 0.02 } } }\n"
				"bad_category = { category = navy equipment = { rifle_0 } icon = 1 sprite = Infantry\n"
				" base_stats = { attack = 8 defense = 2 breakthrough = 9 organisation = 30 speed = 4 manpower = 3 cost = { rifle_0 = 10 } supply_cost = { rifle_0 = 0.02 } } }\n"
				"missing_stats = { category = infantry equipment = { rifle_0 } icon = 1 sprite = Infantry }\n");
			UnitTypeManager fresh;
			fresh.reserve_all_unit_types(4);
			const ovdl::v2script::Parser good = Dataloader::parse_defines(temp / "good.txt");
			const ovdl::v2script::Parser bad = Dataloader::parse_defines(temp / "bad.txt");
			const std::size_t good_count = fresh.load_ov_unit_file(goods, equipment, "good.txt", good.get_file_node());
			const std::size_t bad_count = fresh.load_ov_unit_file(goods, equipment, "bad.txt", bad.get_file_node());
			RegimentType const* const guard_test = fresh.get_regiment_type_by_identifier("guard_test");
			r.check(
				good_count == 1 && guard_test != nullptr && guard_test->attack == fixed_point_t(8) && guard_test->get_breakthrough() == fixed_point_t(9)
					&& guard_test->ov.occupation_weight == fixed_point_t(12) / 10 && guard_test->ov.equipment.size() == 1,
				"unit types: a new type of unit comes with one file, no code is needed"
			);
			r.check(bad_count == 0 && fresh.get_regiment_type_count() == 1, "unit types: a type with an unknown weapon, an unknown category or without stats is skipped, the game goes on");
			fs::remove_all(temp, ec);
		}

		// put the country back
		for (memory::string const& key : std::vector<memory::string> { country->get_unlock_keys().begin(), country->get_unlock_keys().end() }) {
			country->revoke_unlock(key);
		}
		for (memory::string const& key : keys_before) {
			country->grant_unlock(key);
		}
	}

	// ---- the army technologies (research/ArmyTechnology.*, ov_data/common/ov_technology)

	void test_army_technologies(Results& r, InstanceManager& im) {
		namespace fs = std::filesystem;
		ArmyTechnologyManager const& technologies = im.definition_manager.get_research_manager().get_army_technology_manager();
		if (technologies.size() == 0) {
			fmt::println("TEST INFO army technologies: no ov_data folder, not checked");
			return;
		}
		r.check(technologies.size() == 23 && technologies.validate() == 0, "army technologies: the 23 technologies of the start are loaded from files, the validator finds nothing", fmt::format("{}", technologies.size()));
		bool all_ok = true;
		for (ArmyTechnology const& technology : technologies.get_technologies()) {
			all_ok &= technology.research_days == 730 && technology.branch == "army" && technology.era >= 1 && technology.era <= 5 && technology.x >= 0 && technology.y >= 0;
		}
		r.check(all_ok, "army technologies: each has the branch army, an era 1-5, a cell and 730 days of research");
		// The civilised start
		std::vector<std::string> start;
		for (ArmyTechnology const& technology : technologies.get_technologies()) {
			if (technology.start == "civilised") {
				start.emplace_back(technology.identifier);
			}
		}
		std::sort(start.begin(), start.end());
		r.check(start == std::vector<std::string> { "arms_making", "percussion_cap", "standardized_army" }, "army technologies: the civilised countries start with arms making, the percussion cap and the standardized army");
		// AND between the blocks, OR inside a block
		ArmyTechnology const* const operational = technologies.find("operational_planning");
		r.check(
			operational != nullptr && operational->prerequisites.size() == 2 && operational->prerequisites[0].size() == 1 && operational->prerequisites[1].size() == 2
				&& technologies.get_prerequisite_indices(*operational).size() == 2,
			"army technologies: operational planning needs mobilisation planning AND (skirmish order OR breech loading) - two blocks, the second has two technologies"
		);
		ArmyTechnology const* const magazine = technologies.find("magazine_rifle");
		r.check(
			magazine != nullptr && magazine->modifier.has_value() && !magazine->modifier->get_values().empty()
				&& std::find(magazine->unlocks.begin(), magazine->unlocks.end(), "equipment:rifle_4") != magazine->unlocks.end(),
			"army technologies: the unlocks and the modifiers of a file are read (magazine rifle: rifle_4 and the breakthrough of the infantry)"
		);
		// The layout: the prerequisites are above the technologies
		bool above = true;
		for (ArmyTechnology const& technology : technologies.get_technologies()) {
			for (auto const& block : technology.prerequisites) {
				for (memory::string const& id : block) {
					ArmyTechnology const* const prerequisite = technologies.find(id);
					above &= prerequisite != nullptr && prerequisite->y < technology.y;
				}
			}
		}
		r.check(above, "army technologies: the prerequisites of every technology are in the rows above it");

		// A new technology comes with a file only, the broken ones are skipped, the validator finds the faults of the tree
		const fs::path temp = fs::temp_directory_path() / "openvic_selftest_techs";
		std::error_code ec;
		fs::remove_all(temp, ec);
		write_test_file(temp / "good.txt",
			"alpha = { era = 1 x = 0 y = 0 }\n"
			"beta = { era = 1 x = 0 y = 1 research_days = 100 prerequisite = { alpha } unlocks = { \"unit:guard_test\" } modifiers = { attack_mult = { all = 0.05 } } }\n"
			"gamma = { era = 2 x = 1 y = 2 prerequisite = { alpha beta } prerequisite = { beta } }\n");
		write_test_file(temp / "bad.txt",
			"no_cell = { era = 1 x = 0 }\n"
			"no_days = { era = 1 x = 3 y = 3 research_days = 0 }\n"
			"alpha = { era = 1 x = 5 y = 5 }\n"
			"bad_modifier = { era = 1 x = 6 y = 6 modifiers = { no_such_effect = 1 } }\n");
		write_test_file(temp / "faults.txt",
			"missing = { era = 1 x = 0 y = 0 prerequisite = { nowhere } }\n"
			"same_a = { era = 1 x = 2 y = 2 }\n"
			"same_b = { era = 1 x = 2 y = 2 }\n"
			"low = { era = 1 x = 4 y = 1 prerequisite = { high } }\n"
			"high = { era = 1 x = 4 y = 3 }\n"
			"cycle_a = { era = 1 x = 6 y = 1 prerequisite = { cycle_b } }\n"
			"cycle_b = { era = 1 x = 6 y = 0 prerequisite = { cycle_a } }\n");
		ModifierManager const& modifier_manager = im.definition_manager.get_modifier_manager();
		{
			ArmyTechnologyManager fresh;
			const ovdl::v2script::Parser good = Dataloader::parse_defines(temp / "good.txt");
			const ovdl::v2script::Parser bad = Dataloader::parse_defines(temp / "bad.txt");
			const std::size_t good_count = fresh.load_file("good.txt", good.get_file_node(), modifier_manager);
			const std::size_t bad_count = fresh.load_file("bad.txt", bad.get_file_node(), modifier_manager);
			ArmyTechnology const* const beta = fresh.find("beta");
			ArmyTechnology const* const gamma = fresh.find("gamma");
			r.check(
				good_count == 3 && beta != nullptr && beta->research_days == 100 && beta->prerequisites.size() == 1 && beta->unlocks.size() == 1 && beta->modifier.has_value()
					&& gamma != nullptr && gamma->prerequisites.size() == 2 && gamma->prerequisites[0].size() == 2 && fresh.find("alpha")->research_days == 730 && fresh.validate() == 0,
				"army technologies: a new technology comes with one file, no code is needed (the days default to 730, each prerequisite key is a block)"
			);
			r.check(
				fresh.find("no_cell") == nullptr && fresh.find("no_days") == nullptr && fresh.find("bad_modifier") == nullptr && fresh.find("alpha")->x == 0,
				"army technologies: a technology without a cell or days, with an unknown modifier or with an id that is taken is skipped, the game goes on"
			);
			r.check(bad_count == 0, "army technologies: the broken file gives no technologies");
		}
		{
			ArmyTechnologyManager faulty;
			const ovdl::v2script::Parser parser = Dataloader::parse_defines(temp / "faults.txt");
			faulty.load_file("faults.txt", parser.get_file_node(), modifier_manager);
			// missing prerequisite 1, two in a cell 1, a prerequisite that is not above 1 (low), a cycle (reported on the way back) 1
			const std::size_t problems = faulty.validate();
			r.check(faulty.size() == 7 && problems >= 4, "army technologies: the validator finds a missing prerequisite, two technologies in a cell, a prerequisite that is not above and a cycle", fmt::format("{} problems", problems));
		}
		fs::remove_all(temp, ec);
	}

	// ---- the research of the army technologies (research/ArmyResearch.*)

	void test_army_research(Results& r, InstanceManager& im) {
		ArmyTechnologyManager const& manager = im.definition_manager.get_research_manager().get_army_technology_manager();
		if (manager.size() == 0) {
			fmt::println("TEST INFO army research: no army technologies, not checked");
			return;
		}
		OpenVicResearchDefines& research_defines = const_cast<OpenVicResearchDefines&>(im.definition_manager.get_define_manager().get_openvic_defines().research);
		ModifierManager const& modifier_manager = im.definition_manager.get_modifier_manager();
		const auto index_of = [&](std::string_view id) -> std::size_t { return manager.find(id)->index; };
		const auto state_of = [&](CountryInstance const& country, std::string_view id) { return get_army_technology_state(manager, country, *manager.find(id)); };

		// A civilised AI country, another civilised one and an uncivilised one
		CountryInstance* civilised = nullptr;
		CountryInstance* other = nullptr;
		CountryInstance* uncivilised = nullptr;
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (!candidate.exists() || candidate.is_rebel_country()) {
				continue;
			}
			if (candidate.is_civilised() && candidate.is_ai()) {
				if (civilised == nullptr) {
					civilised = &candidate;
				} else if (other == nullptr) {
					other = &candidate;
				}
			} else if (!candidate.is_civilised() && uncivilised == nullptr) {
				uncivilised = &candidate;
			}
		}
		if (civilised == nullptr || other == nullptr || uncivilised == nullptr) {
			fmt::println("TEST INFO army research: no countries to test with, not checked");
			return;
		}
		struct Snapshot {
			CountryInstance* country;
			std::vector<memory::string> keys;
			std::vector<Modifier const*> modifiers;
			std::vector<std::uint32_t> researched;
			std::vector<ArmyResearchProgress> research;
			bool start_given;
		};
		std::vector<Snapshot> snapshots;
		for (CountryInstance* const country : { civilised, other, uncivilised }) {
			snapshots.push_back({
				country, { country->get_unlock_keys().begin(), country->get_unlock_keys().end() },
				{ country->get_unlock_modifiers().begin(), country->get_unlock_modifiers().end() },
				{ country->get_army_technologies_researched().begin(), country->get_army_technologies_researched().end() },
				{ country->get_army_research().begin(), country->get_army_research().end() }, country->are_army_start_technologies_given()
			});
		}
		const auto refresh = [&](CountryInstance& country) {
			country.update_modifier_sum(im.get_today(), modifier_manager.get_static_modifier_cache());
		};

		// The provisional layer of the year (the AI countries) gave the keys of the weapons: the three countries are put back to the start (only the start
		// technologies); the AI does not research by default, the test of the AI switches it on
		const std::int32_t ai_before = research_defines.army_research_ai_enabled;
		research_defines.army_research_ai_enabled = 0;
		for (CountryInstance* const country : { civilised, other, uncivilised }) {
			for (ArmyTechnology const& technology : manager.get_technologies()) {
				country->unmark_army_technology_researched(static_cast<std::uint32_t>(technology.index));
				for (memory::string const& key : technology.unlocks) {
					country->revoke_unlock(key);
				}
				if (technology.modifier.has_value()) {
					country->remove_unlock_modifier(*technology.modifier);
				}
			}
			country->get_army_research_mutable().clear();
			country->set_army_start_technologies_given(false);
		}
		update_army_research(im);
		refresh(*civilised);
		refresh(*other);

		// The start: the civilised have the technologies of the start with their keys, the uncivilised have nothing
		r.check(
			civilised->are_army_start_technologies_given() && civilised->has_army_technology(static_cast<std::uint32_t>(index_of("arms_making")))
				&& civilised->has_army_technology(static_cast<std::uint32_t>(index_of("percussion_cap"))) && civilised->has_unlock("equipment:rifle_1")
				&& civilised->has_unlock("unit:infantry") && civilised->has_unlock("equipment:field_gun_1") && !civilised->has_unlock("equipment:rifle_2"),
			"army research: a civilised country starts with arms making, the percussion cap and the standardized army and their keys (rifle_1, field_gun_1, unit:infantry)"
		);
		r.check(
			uncivilised->get_army_technologies_researched().empty() && !uncivilised->are_army_start_technologies_given() && !uncivilised->has_unlock("equipment:rifle_1"),
			"army research: an uncivilised country has no army technologies and no keys of them"
		);
		r.check(
			state_of(*civilised, "percussion_cap") == tech_state_t::RESEARCHED && state_of(*civilised, "rifling") == tech_state_t::AVAILABLE
				&& state_of(*civilised, "breech_loading") == tech_state_t::NOT_AVAILABLE,
			"army research: the states are researched, available (the prerequisites are done) and not available"
		);
		const std::int32_t civilised_only_before = research_defines.army_research_civilised_only;
		r.check(
			can_start_army_research(im, *uncivilised, index_of("arms_making")) == army_research_check_t::OK
				&& can_start_army_research(im, *uncivilised, index_of("percussion_cap")) == army_research_check_t::PREREQUISITES,
			"army research: by default the uncivilised research themselves, from the roots of the tree",
			fmt::format("arms_making {}, percussion_cap {}, civilised_only {}", static_cast<int>(can_start_army_research(im, *uncivilised, index_of("arms_making"))),
				static_cast<int>(can_start_army_research(im, *uncivilised, index_of("percussion_cap"))), research_defines.army_research_civilised_only)
		);
		research_defines.army_research_civilised_only = 1;
		r.check(
			can_start_army_research(im, *uncivilised, index_of("arms_making")) == army_research_check_t::NOT_ALLOWED
				&& can_start_army_research(im, *civilised, index_of("breech_loading")) == army_research_check_t::PREREQUISITES
				&& can_start_army_research(im, *civilised, index_of("percussion_cap")) == army_research_check_t::ALREADY_RESEARCHED
				&& can_start_army_research(im, *civilised, 100000) == army_research_check_t::NO_SUCH_TECHNOLOGY
				&& can_start_army_research(im, *civilised, index_of("rifling")) == army_research_check_t::OK,
			"army research: the uncivilised cannot research, nor can one start a technology with open prerequisites, a researched one or one that does not exist"
		);
		research_defines.army_research_civilised_only = civilised_only_before;

		// One slot per branch, the days, the unlocks and the modifier (without the days the literacy adds, version 89: checked below)
		const Rate literacy_research_before = research_defines.literacy_research;
		research_defines.literacy_research = Rate(0, 100);
		r.check(start_army_research(im, *civilised, index_of("rifling")), "army research: a research is started");
		r.check(
			state_of(*civilised, "rifling") == tech_state_t::RESEARCHING
				&& can_start_army_research(im, *civilised, index_of("post_napoleonic_tactics")) == army_research_check_t::SLOT_BUSY
				&& can_start_army_research(im, *civilised, index_of("rifling")) == army_research_check_t::ALREADY_RESEARCHING && !start_army_research(im, *civilised, index_of("post_napoleonic_tactics")),
			"army research: a branch has one slot, the second technology waits"
		);
		for (int day = 0; day < 100; ++day) {
			update_army_research(im);
		}
		r.check(
			get_army_research_days_left(im, *civilised, index_of("rifling")) == 630 && !civilised->has_unlock("equipment:rifle_2"),
			"army research: a day of research is a day, nothing is given before the end", fmt::format("{} days left", get_army_research_days_left(im, *civilised, index_of("rifling")))
		);
		// cancelling loses the progress
		r.check(cancel_army_research(im, *civilised, index_of("rifling")) && civilised->get_army_research().empty()
				&& get_army_research_days_left(im, *civilised, index_of("rifling")) == 730 && !cancel_army_research(im, *civilised, index_of("rifling")),
			"army research: cancelling frees the slot and the progress is lost");
		start_army_research(im, *civilised, index_of("rifling"));
		for (int day = 0; day < 729; ++day) {
			update_army_research(im);
		}
		r.check(state_of(*civilised, "rifling") == tech_state_t::RESEARCHING && !civilised->has_unlock("equipment:rifle_2"), "army research: after 729 days the technology is still being researched");
		update_army_research(im);
		r.check(
			state_of(*civilised, "rifling") == tech_state_t::RESEARCHED && civilised->get_army_research().empty() && civilised->has_unlock("equipment:rifle_2")
				&& civilised->has_unlock("equipment:field_gun_2") && civilised->has_unlock("unit:rifled_artillery") && state_of(*civilised, "breech_loading") == tech_state_t::AVAILABLE,
			"army research: after 730 days the technology is researched, its keys are given and the next one is available"
		);
		// A literate country researches faster (version 89, docs/LITERACY.md): 100 days give more than 100 days of research
		research_defines.literacy_research = literacy_research_before;
		if (civilised->get_average_literacy() > fixed_point_t::_0 && research_defines.literacy_research.numerator > 0
			&& start_army_research(im, *civilised, index_of("breech_loading"))) {
			const std::int32_t days_before = get_army_research_days_left(im, *civilised, index_of("breech_loading"));
			for (int day = 0; day < 100; ++day) {
				update_army_research(im);
			}
			const std::int32_t days_after = get_army_research_days_left(im, *civilised, index_of("breech_loading"));
			r.check(days_before - days_after > 100, "army research: a literate country researches faster",
				fmt::format("{} days done in 100 days, literacy {:.2f}", days_before - days_after, static_cast<double>(civilised->get_average_literacy())));
			cancel_army_research(im, *civilised, index_of("breech_loading"));
		}
		research_defines.army_research_ai_enabled = ai_before;

		// The modifier of a technology reaches the country
		{
			CountryInstance& country = *civilised;
			ModifierEffectCache const& cache = modifier_manager.get_modifier_effect_cache();
			(void)cache;
			refresh(country);
			const std::uint32_t ranks_before = get_template_limits(im, country).ranks;
			complete_army_technology(im, country, index_of("post_napoleonic_tactics"));
			refresh(country);
			r.check(get_template_limits(im, country).ranks == ranks_before + 1 && country.has_unlock("unit:hussars"),
				"army research: post-Napoleonic tactics gives a column to the templates (the modifier of the file) and the hussars");
			r.check(!complete_army_technology(im, country, index_of("post_napoleonic_tactics")), "army research: a technology is researched once");
		}

		// AND between the blocks, OR inside a block (operational planning: mobilisation planning AND (skirmish order OR breech loading))
		{
			complete_army_technology(im, *other, index_of("mobilisation_planning"));
			r.check(state_of(*other, "operational_planning") == tech_state_t::NOT_AVAILABLE, "army research: with one of the two blocks done the technology is not available");
			complete_army_technology(im, *other, index_of("skirmish_order"));
			r.check(state_of(*other, "operational_planning") == tech_state_t::AVAILABLE, "army research: one technology of a block of alternatives is enough");
		}

		// The AI: the first available technology in the order of the files, the slot of the branch free
		{
			const std::size_t choice = choose_ai_army_technology(im, *other, 0);
			r.check(choice != std::numeric_limits<std::size_t>::max() && state_of(*other, manager.at(choice).identifier) == tech_state_t::AVAILABLE, "army research: the AI chooses an available technology");
			bool first = choice != std::numeric_limits<std::size_t>::max();
			for (ArmyTechnology const& technology : manager.get_technologies()) {
				if (first && technology.index < choice && can_start_army_research(im, *other, technology.index) == army_research_check_t::OK) {
					first = false;
				}
			}
			r.check(first, "army research: the AI takes the first available technology in the order of the files");
			other->get_army_research_mutable().clear();
			research_defines.army_research_ai_enabled = 1;
			update_army_research(im);
			r.check(!other->get_army_research().empty(), "army research: a free slot of an AI country is filled by the daily update when ARMY_RESEARCH_AI_ENABLED = 1");
			// the player (not an AI country) is never given a research by the AI, and neither is the first call of the setup of the game
			other->get_army_research_mutable().clear();
			update_army_research(im, false);
			r.check(other->get_army_research().empty(), "army research: the setup of the game (the player is not known yet) does not start a research of the AI");
			other->set_ai(false);
			update_army_research(im);
			r.check(other->get_army_research().empty(), "army research: a country of the player does not get a research from the AI");
			other->set_ai(true);
			research_defines.army_research_ai_enabled = 0;
			other->get_army_research_mutable().clear();
			update_army_research(im);
			r.check(other->get_army_research().empty(), "army research: ov_research.ARMY_RESEARCH_AI_ENABLED = 0 stops the AI");
			research_defines.army_research_ai_enabled = ai_before;
		}

		// A civilised-only switch
		{
			const std::int32_t only_before = research_defines.army_research_civilised_only;
			research_defines.army_research_civilised_only = 0;
			uncivilised->get_army_research_mutable().clear(); // the AI test above may have started one for it
			r.check(can_start_army_research(im, *uncivilised, index_of("arms_making")) == army_research_check_t::OK, "army research: with ARMY_RESEARCH_CIVILISED_ONLY = 0 the uncivilised may research too");
			research_defines.army_research_civilised_only = only_before;
		}

		// The state is in the hash
		{
			other->get_army_research_mutable().clear();
			const auto hash_before = StateHash::compute(im);
			const std::size_t choice = choose_ai_army_technology(im, *other, 0);
			r.check(choice != std::numeric_limits<std::size_t>::max() && start_army_research(im, *other, choice) && StateHash::compute(im) != hash_before,
				"army research: the research is in the hash of the state");
		}

		// put the countries back
		for (Snapshot const& snapshot : snapshots) {
			CountryInstance& country = *snapshot.country;
			for (const std::uint32_t index : std::vector<std::uint32_t> { country.get_army_technologies_researched().begin(), country.get_army_technologies_researched().end() }) {
				if (std::find(snapshot.researched.begin(), snapshot.researched.end(), index) == snapshot.researched.end()) {
					country.unmark_army_technology_researched(index);
				}
			}
			for (memory::string const& key : std::vector<memory::string> { country.get_unlock_keys().begin(), country.get_unlock_keys().end() }) {
				country.revoke_unlock(key);
			}
			for (memory::string const& key : snapshot.keys) {
				country.grant_unlock(key);
			}
			while (!country.get_unlock_modifiers().empty()) {
				country.remove_unlock_modifier(*country.get_unlock_modifiers().front());
			}
			for (Modifier const* const modifier : snapshot.modifiers) {
				country.add_unlock_modifier(*modifier);
			}
			country.get_army_research_mutable().assign(snapshot.research.begin(), snapshot.research.end());
			country.set_army_start_technologies_given(snapshot.start_given);
			refresh(country);
		}
	}

	// ---- the buildings and the mechanics that the keys of the unlock layer open (BuildingType::requires_unlock, mechanic:*)

	void test_unlock_gates(Results& r, InstanceManager& im) {
		BuildingTypeManager const& buildings = im.definition_manager.get_economy_manager().get_building_type_manager();
		BuildingType const* const small_arms = buildings.get_building_type_by_identifier("small_arms_factory");
		BuildingType const* const radio_factory = buildings.get_building_type_by_identifier("radio_factory");
		BuildingType const* const steel = buildings.get_building_type_by_identifier("steel_factory");
		if (small_arms == nullptr || radio_factory == nullptr || steel == nullptr || small_arms->requires_unlock.empty()) {
			fmt::println("TEST INFO unlock gates: no ov_data folder, not checked");
			return;
		}
		CountryInstance* civilised = nullptr;
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (candidate.exists() && !candidate.is_rebel_country() && candidate.is_civilised()) {
				civilised = &candidate;
			}
		}
		if (civilised == nullptr) {
			return;
		}
		const std::vector<memory::string> keys_before { civilised->get_unlock_keys().begin(), civilised->get_unlock_keys().end() };
		for (std::string_view const key : { "building:small_arms_factory", "building:telegraph_factory", "building:radio_factory", "mechanic:division_templates", "mechanic:mobilisation" }) {
			civilised->revoke_unlock(key);
		}
		r.check(!civilised->is_building_type_unlocked(*small_arms), "unlock gates: without the key a building that needs it cannot be built, whatever the technologies of the base game say");
		civilised->grant_unlock("building:small_arms_factory");
		r.check(civilised->is_building_type_unlocked(*small_arms), "unlock gates: with the key the building can be built (the base game technology is not needed)");
		r.check(!civilised->is_building_type_unlocked(*radio_factory), "unlock gates: the communications factory needs the telegraph or the radio");
		civilised->grant_unlock("building:telegraph_factory");
		r.check(civilised->is_building_type_unlocked(*radio_factory), "unlock gates: any one of the keys of a building is enough (the telegraph)");
		civilised->revoke_unlock("building:telegraph_factory");
		civilised->grant_unlock("building:radio_factory");
		r.check(civilised->is_building_type_unlocked(*radio_factory), "unlock gates: any one of the keys of a building is enough (the radio)");
		r.check(steel->requires_unlock.empty(), "unlock gates: a building that is not in ov_buildings is unlocked by the technologies of the base game as before");
		// the factory production types: the telegraph and the radio, the old one stays
		const auto options = get_factory_production_options(im, *radio_factory);
		bool telegraph_option = false, radio_option = false;
		for (FactoryProductionOption const& option : options) {
			telegraph_option |= option.production_type->output_good.get_identifier() == "telegraph" && option.unlock_key == "building:telegraph_factory" && option.tier == 1;
			radio_option |= option.production_type->output_good.get_identifier() == "radio" && option.unlock_key == "building:radio_factory" && option.tier == 2;
		}
		r.check(telegraph_option && radio_option, "unlock gates: the communications factory makes the telegraph (tier 1) or the radio (tier 2), each by its key");
		ProductionType const* const with_both = get_best_production_type(im, *civilised, *radio_factory);
		civilised->grant_unlock("building:telegraph_factory");
		civilised->revoke_unlock("building:radio_factory");
		ProductionType const* const with_telegraph = get_best_production_type(im, *civilised, *radio_factory);
		r.check(with_both != nullptr && with_telegraph != nullptr && with_both->output_good.get_identifier() == "radio" && with_telegraph->output_good.get_identifier() == "telegraph",
			"unlock gates: the radio takes over from the telegraph when the country has both, the telegraph is the best before");

		for (memory::string const& key : std::vector<memory::string> { civilised->get_unlock_keys().begin(), civilised->get_unlock_keys().end() }) {
			civilised->revoke_unlock(key);
		}
		for (memory::string const& key : keys_before) {
			civilised->grant_unlock(key);
		}
	}

	// ---- the density and the breakthrough (military/BattleFormulas.*)

	void test_battle_rules(Results& r, InstanceManager& im) {
		OpenVicWarDefines const& war = im.definition_manager.get_define_manager().get_openvic_defines().war;
		using namespace battle_formulas;
		const fixed_point_t eps = fixed_point_t(1) / 10000;

		// The density: 1.00 is 100 %, 0.75 is 68.75 % (x^2 + x with x = 0.25)
		const DensityEffect even = density_effect(war, fixed_point_t::_1);
		const DensityEffect thin = density_effect(war, fixed_point_t(3) / 4);
		const DensityEffect thin_90 = density_effect(war, fixed_point_t(9) / 10);
		r.check(even.defence_mult == fixed_point_t::_1 && even.breakthrough_mult == fixed_point_t::_1 && even.casualties_mult == fixed_point_t::_1 && even.attack_mult == fixed_point_t::_1,
			"battle: a density of 1.00 changes nothing");
		r.check(near(thin.defence_mult, fixed_point_t(6875) / 10000, eps) && near(thin.breakthrough_mult, fixed_point_t(6875) / 10000, eps) && thin.casualties_mult == fixed_point_t::_1 && thin.attack_mult == fixed_point_t::_1,
			"battle: a density of 0.75 leaves 68.75 % of the defence and of the breakthrough (1 - (x^2 + x))", fmt::format("{}", dbl(thin.defence_mult)));
		r.check(near(thin_90.defence_mult, fixed_point_t(89) / 100, eps),
			"battle: a density of 0.90 leaves 89 % by the formula (x = 0.1: 1 - 0.11; the example 98 % of the owner does not follow from the formula, see the report)");
		const DensityEffect dense = density_effect(war, fixed_point_t(125) / 100);
		r.check(near(dense.casualties_mult, fixed_point_t(13125) / 10000, eps) && near(dense.breakthrough_mult, fixed_point_t(13125) / 10000, eps)
				&& near(dense.attack_mult, fixed_point_t(115625) / 100000, eps) && dense.defence_mult == fixed_point_t::_1,
			"battle: a density of 1.25 gives 131.25 % of the casualties and the breakthrough and 115.625 % of the attack (half the rate)",
			fmt::format("{} {} {}", dbl(dense.casualties_mult), dbl(dense.breakthrough_mult), dbl(dense.attack_mult)));
		const DensityEffect dense_110 = density_effect(war, fixed_point_t(11) / 10);
		r.check(near(dense_110.casualties_mult, fixed_point_t(111) / 100, eps), "battle: a density of 1.10 gives 111 % of the casualties by the formula (x = 0.1)");

		// The technologies of the scattered order: 1.2 x 0.9 = 1.08, the excess over 1.0 less by 15 %: 1.068; nothing under 1.0 is changed; not under 1.0 from above
		r.check(near(effective_density(fixed_point_t(12) / 10, fixed_point_t(9) / 10, fixed_point_t(15) / 100), fixed_point_t(1068) / 1000, eps), "battle: the density 1.2 with the coefficient 0.9 and the excess cut by 15 % is 1.068");
		r.check(effective_density(fixed_point_t(8) / 10, fixed_point_t(9) / 10, fixed_point_t(15) / 100) == fixed_point_t(8) / 10 && effective_density(fixed_point_t(105) / 100, fixed_point_t(8) / 10, fixed_point_t::_0) == fixed_point_t::_1,
			"battle: a density under 1.0 is not changed by the technologies and a density over 1.0 does not fall under it");

		// The breakthrough: R = the breakthrough of the attacker / the effective defence of the defender (the fortification counts)
		DivisionStats attacker, defender;
		attacker.breakthrough = fixed_point_t(30);
		defender.defence = fixed_point_t(10);
		r.check(breakthrough_ratio(war, attacker, defender) == fixed_point_t(3), "battle: R is the breakthrough over the defence");
		defender.fortification = fixed_point_t(2);
		r.check(near(breakthrough_ratio(war, attacker, defender), fixed_point_t(30) / (fixed_point_t(10) * (fixed_point_t::_1 + war.fortification_per_level.of(fixed_point_t(2)))), eps),
			"battle: the fortification (5 % of the defence a level) and the bonus of the terrain are in the effective defence");
		r.check(near(effective_defence(war, defender, fixed_point_t(11) / 10), defender.defence * (fixed_point_t::_1 + war.fortification_per_level.of(fixed_point_t(2))) * fixed_point_t(11) / 10, eps * 10),
			"battle: the bonus of the terrain multiplies the protection");

		BreakthroughCounter counter;
		r.check(breakthrough_day(war, fixed_point_t(3), counter), "battle: R of 3 in one day makes the defence retreat");
		counter = {};
		bool retreated = false;
		for (int day = 1; day <= 6; ++day) {
			retreated |= breakthrough_day(war, fixed_point_t(2), counter);
		}
		r.check(!retreated && counter.days == 6, "battle: R of 2 for six days is not enough");
		r.check(breakthrough_day(war, fixed_point_t(2), counter), "battle: R of 2 for the seventh day in a row makes the defence retreat");
		counter = {};
		for (int day = 1; day <= 6; ++day) {
			breakthrough_day(war, fixed_point_t(2), counter);
		}
		const bool after_drop = breakthrough_day(war, fixed_point_t(3) / 2, counter);
		r.check(!after_drop && counter.days == 0, "battle: R of 2 for six days and then R of 1.5 gives no retreat (the counter goes back)");
		bool later = false;
		for (int day = 1; day <= 6; ++day) {
			later |= breakthrough_day(war, fixed_point_t(2), counter);
		}
		r.check(!later, "battle: after the counter went back six more days of R = 2 are still not enough");
		r.check(!breakthrough_day(war, fixed_point_t(199) / 100, counter) && counter.days == 0, "battle: R just under 2 is nothing");

		// The damage of a day (battle_hit): attack x conditions x density x (1 - BATTLE_BLOCK_SHARE x min(1, protection / attack)); the men lost are
		// damage x BATTLE_STRENGTH_PER_HIT x casualties_taken_mult, the organisation damage x BATTLE_ORGANISATION_PER_HIT
		const fixed_point_t attack = fixed_point_t(20);
		const Hit full_block = battle_hit(war, attack, fixed_point_t::_1, fixed_point_t::_1, attack, fixed_point_t::_1);
		const Hit over_block = battle_hit(war, attack, fixed_point_t::_1, fixed_point_t::_1, attack * 3, fixed_point_t::_1);
		r.check(near(full_block.damage, attack / 4, eps) && near(over_block.damage, attack / 4, eps),
			"battle: a protection as big as the attack or bigger lets 25 % of the attack through", fmt::format("{} {}", dbl(full_block.damage), dbl(over_block.damage)));
		const Hit half_block = battle_hit(war, attack, fixed_point_t::_1, fixed_point_t::_1, attack / 2, fixed_point_t::_1);
		r.check(near(half_block.damage, attack * 5 / 8, eps), "battle: a protection of half the attack lets 62.5 % through", fmt::format("{}", dbl(half_block.damage)));
		const Hit no_block = battle_hit(war, attack, fixed_point_t::_1, fixed_point_t::_1, fixed_point_t::_0, fixed_point_t::_1);
		r.check(near(no_block.damage, attack, eps), "battle: no protection lets all of the attack through");
		const Hit dense_target = battle_hit(war, attack, fixed_point_t::_1, fixed_point_t(2), attack / 2, fixed_point_t::_1);
		r.check(near(dense_target.damage, half_block.damage * 2, eps), "battle: the density of the target multiplies the damage");
		const Hit hard_conditions = battle_hit(war, attack, fixed_point_t(3) / 4, fixed_point_t::_1, attack / 2, fixed_point_t::_1);
		r.check(near(hard_conditions.damage, half_block.damage * 3 / 4, eps), "battle: the conditions of the attacker (a river) multiply the damage");
		const Hit medicine = battle_hit(war, attack, fixed_point_t::_1, fixed_point_t::_1, attack / 2, fixed_point_t::_1 / 2);
		r.check(near(medicine.strength_loss, half_block.strength_loss / 2, eps) && medicine.organisation_loss == half_block.organisation_loss
				&& medicine.damage == half_block.damage,
			"battle: the medicine (casualties_taken_mult) lowers the men lost only, not the damage and not the organisation");
		r.check(near(half_block.strength_loss, half_block.damage * war.battle_strength_per_hit, eps) && near(half_block.organisation_loss, half_block.damage * war.battle_organisation_per_hit, eps),
			"battle: the men and the organisation lost are the damage times BATTLE_STRENGTH_PER_HIT and BATTLE_ORGANISATION_PER_HIT");
		r.check(near(defender_protection(war, fixed_point_t(10), fixed_point_t::_1, fixed_point_t(12) / 10), fixed_point_t(10) * (fixed_point_t::_1 + war.fortification_per_level.of(fixed_point_t::_1)) * fixed_point_t(12) / 10, eps),
			"battle: the protection of a defender is its defence x (1 + 0.05 x fortification) x the terrain");

		// The reconnaissance (section 12): the advantage 2 x own / sum - 1; 3 against 1 is +0.5: the attack x (1 + RECON_TACTICS_BONUS / 2), the dig in of the
		// enemy less by RECON_DIG_IN_CANCEL / 2; the side with -0.5 gets no bonus and sees the defence lasting (1 + RECON_ESTIMATE_CAUTION / 2) times longer
		const fixed_point_t ahead = recon_advantage(fixed_point_t(3), fixed_point_t::_1);
		r.check(ahead == fixed_point_t::_1 / 2 && recon_advantage(fixed_point_t::_1, fixed_point_t(3)) == -fixed_point_t::_1 / 2
				&& recon_advantage(fixed_point_t::_0, fixed_point_t::_0) == fixed_point_t::_0 && recon_advantage(fixed_point_t(2), fixed_point_t::_0) == fixed_point_t::_1
				&& recon_tactics(war, ahead) == fixed_point_t::_1 + war.recon_tactics_bonus.of(ahead) && recon_tactics(war, -ahead) == fixed_point_t::_1
				&& near(recon_dig_in(war, fixed_point_t(4), ahead), fixed_point_t(4) * (fixed_point_t::_1 - war.recon_dig_in_cancel.of(ahead)), eps)
				&& recon_dig_in(war, fixed_point_t(4), -ahead) == fixed_point_t(4)
				&& recon_caution(war, -ahead) == fixed_point_t::_1 + war.recon_estimate_caution.of(ahead) && recon_caution(war, ahead) == fixed_point_t::_1,
			"battle: the reconnaissance gives the side ahead the tactics bonus and cancels a part of the enemy dig in; the side behind estimates with caution",
			fmt::format("advantage {} tactics {} dig in {} caution {}", dbl(ahead), dbl(recon_tactics(war, ahead)), dbl(recon_dig_in(war, fixed_point_t(4), ahead)),
				dbl(recon_caution(war, -ahead))));

		// The modifiers of the country count in the attack, the defence and the breakthrough of a template (percents are summed)
		CountryInstance* country = nullptr;
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (candidate.exists() && !candidate.is_rebel_country() && candidate.has_army_command() && !candidate.get_division_templates().empty()) {
				country = &candidate;
			}
		}
		if (country == nullptr) {
			return;
		}
		namespace fs = std::filesystem;
		const fs::path temp = fs::temp_directory_path() / "openvic_selftest_battle";
		std::error_code ec;
		fs::remove_all(temp, ec);
		write_test_file(temp / "m.txt",
			"step = { year = 1836 modifier = { attack_mult = { all = 0.10 } defense_mult = { all = 0.20 } breakthrough_mult = { all = 0.30 } casualties_taken_mult = { all = 0.5 }\n"
			" density_coefficient_mult = 0.5 density_excess_reduction = 0.5 } }\n");
		ProvisionalUnlocks modifiers;
		const ovdl::v2script::Parser parser = Dataloader::parse_defines(temp / "m.txt");
		modifiers.load_file("m.txt", parser.get_file_node(), im.definition_manager.get_modifier_manager());
		fs::remove_all(temp, ec);
		if (modifiers.get_steps().size() != 1) {
			r.check(false, "battle: the test modifier is loaded");
			return;
		}
		const DivisionTemplate division_template = country->get_division_templates().front();
		const DivisionStats before = compute_division_stats(im, *country, division_template);
		country->add_unlock_modifier(*modifiers.get_steps()[0].modifier);
		country->update_modifier_sum(im.get_today(), im.definition_manager.get_modifier_manager().get_static_modifier_cache());
		const DivisionStats after = compute_division_stats(im, *country, division_template);
		const DensityEffect before_effect = density_effect(war, before.density);
		const DensityEffect after_effect = density_effect(war, after.density);
		const fixed_point_t raw_before = before.raw_density;
		r.check(near(after.density, effective_density(raw_before, fixed_point_t(1) / 2, fixed_point_t(1) / 2), eps) && after.raw_density == raw_before,
			"battle: the modifiers density_coefficient_mult and density_excess_reduction change the density of a template", fmt::format("{} -> {}", dbl(before.density), dbl(after.density)));
		r.check(
			near(after.attack / after_effect.attack_mult, before.attack / before_effect.attack_mult * fixed_point_t(11) / 10, fixed_point_t(1) / 100)
				&& near(after.defence / after_effect.defence_mult, before.defence / before_effect.defence_mult * fixed_point_t(12) / 10, fixed_point_t(1) / 100)
				&& near(after.breakthrough / after_effect.breakthrough_mult, before.breakthrough / before_effect.breakthrough_mult * fixed_point_t(13) / 10, fixed_point_t(1) / 100),
			"battle: attack_mult, defense_mult and breakthrough_mult of the modifiers (+10 %, +20 %, +30 %) count in the template"
		);
		r.check(near(after.casualties_mult / after_effect.casualties_mult, before.casualties_mult / before_effect.casualties_mult / 2, eps),
			"battle: casualties_taken_mult of the modifiers counts in the casualties of the template");
		country->remove_unlock_modifier(*modifiers.get_steps()[0].modifier);
		country->update_modifier_sum(im.get_today(), im.definition_manager.get_modifier_manager().get_static_modifier_cache());
	}

	// ---- the occupation of a province (military/BattleFormulas.*, DivisionStats)

	void test_occupation(Results& r, InstanceManager& im) {
		OpenVicWarDefines const& war = im.definition_manager.get_define_manager().get_openvic_defines().war;
		using namespace battle_formulas;
		const fixed_point_t eps = fixed_point_t(1) / 1000;
		r.check(near(occupation_power(war, fixed_point_t(6)), fixed_point_t::_1, eps) && near(occupation_power(war, fixed_point_t(3)), fixed_point_t(1) / 2, eps),
			"occupation: the power is the sum of the weights over 6 (six regiments of weight 1 are 6/6)");
		const fixed_point_t speed = occupation_speed(fixed_point_t::_1, fixed_point_t(25) / 100, fixed_point_t(2));
		r.check(near(speed, fixed_point_t(5) / 2, eps) && near(occupation_days(fixed_point_t(10), speed, false), fixed_point_t(4), eps),
			"occupation: the speed with general_staff (+25 %) and operational_planning (x2) is x1.25 x2, the days are the base over the speed");
		r.check(occupation_days(fixed_point_t(10), speed, true) == fixed_point_t::_0 && occupation_days(fixed_point_t(10), fixed_point_t::_0, false) > fixed_point_t(1000),
			"occupation: mechanic:instant_occupation overrides all (0 days), no power never takes a province");

		ModifierEffectCache const& cache = im.definition_manager.get_modifier_manager().get_modifier_effect_cache();
		r.check(cache.get_occupation_speed_leader() != nullptr, "occupation: a trait of a general can have occupation_speed_pct");

		ArmyTechnologyManager const& technologies = im.definition_manager.get_research_manager().get_army_technology_manager();
		UnitTypeManager const& unit_types = im.definition_manager.get_military_manager().get_unit_type_manager();
		RegimentType const* const line_infantry = unit_types.get_regiment_type_by_identifier("line_infantry");
		RegimentType const* const artillery = unit_types.get_regiment_type_by_identifier("napoleonic_artillery");
		ArmyTechnology const* const general_staff = technologies.find("general_staff");
		ArmyTechnology const* const operational_planning = technologies.find("operational_planning");
		CountryInstance* country = nullptr;
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (candidate.exists() && !candidate.is_rebel_country() && candidate.has_army_command()) {
				country = &candidate;
			}
		}
		if (line_infantry == nullptr || artillery == nullptr || general_staff == nullptr || operational_planning == nullptr || country == nullptr
			|| !general_staff->modifier.has_value() || !operational_planning->modifier.has_value()) {
			fmt::println("TEST INFO occupation: no ov_data folder, the templates are not checked");
			return;
		}
		const auto make = [&](DivisionGrid const& grid) {
			DivisionTemplate division_template;
			division_template.id = 999;
			division_template.cells = grid;
			division_template.rebuild_counts();
			return division_template;
		};
		const auto grid_of = [&](RegimentType const& regiment_type, const std::size_t ranks, const std::size_t rows) {
			DivisionGrid grid {};
			for (std::size_t rank = 0; rank < ranks; ++rank) {
				for (std::size_t row = 0; row < rows; ++row) {
					grid[DivisionTemplate::cell_index(rank, row)] = static_cast<std::uint8_t>(get_division_slot_kind(regiment_type) + 1);
				}
			}
			return grid;
		};
		const bool had_key = country->has_unlock(UNLOCK_MECHANIC_INSTANT_OCCUPATION);
		country->revoke_unlock(UNLOCK_MECHANIC_INSTANT_OCCUPATION);
		const auto has_modifier = [&](Modifier const& modifier) -> bool {
			for (Modifier const* const held : country->get_unlock_modifiers()) {
				if (held == &modifier) {
					return true;
				}
			}
			return false;
		};
		const bool had_general_staff = has_modifier(*general_staff->modifier);
		const bool had_operational = has_modifier(*operational_planning->modifier);
		country->remove_unlock_modifier(*general_staff->modifier);
		country->remove_unlock_modifier(*operational_planning->modifier);
		const auto refresh = [&]() { country->update_modifier_sum(im.get_today(), im.definition_manager.get_modifier_manager().get_static_modifier_cache()); };
		refresh();

		const DivisionStats six = compute_division_stats(im, *country, make(grid_of(*line_infantry, 3, 2)));
		r.check(six.regiments == 6 && near(six.occupation_power, fixed_point_t::_1, eps) && near(six.occupation_weight, fixed_point_t::_1, eps) && near(six.occupation_speed, fixed_point_t::_1, eps)
				&& near(six.occupation_days, fixed_point_t(war.occupation_base_days), eps) && !six.occupation_instant,
			"occupation: six regiments of infantry give the power 6/6 and the base days", fmt::format("power {} speed {} days {}", dbl(six.occupation_power), dbl(six.occupation_speed), dbl(six.occupation_days)));
		const DivisionStats guns = compute_division_stats(im, *country, make(grid_of(*artillery, 3, 2)));
		r.check(near(guns.occupation_power, fixed_point_t(1) / 2, eps), "occupation: artillery weighs 0.5, six guns are 3/6");

		country->add_unlock_modifier(*general_staff->modifier);
		country->add_unlock_modifier(*operational_planning->modifier);
		refresh();
		const DivisionStats fast = compute_division_stats(im, *country, make(grid_of(*line_infantry, 3, 2)));
		r.check(near(fast.occupation_speed, fixed_point_t(5) / 2, eps) && near(fast.occupation_days, fixed_point_t(war.occupation_base_days) / fixed_point_t(5) * 2, eps),
			"occupation: with general_staff and operational_planning the speed is x1.25 x2 and the days fall to 4", fmt::format("speed {} days {}", dbl(fast.occupation_speed), dbl(fast.occupation_days)));
		// the companies do not count
		DivisionTemplate with_company = make(grid_of(*line_infantry, 3, 2));
		with_company.companies[0] = 1;
		r.check(near(compute_division_stats(im, *country, with_company).occupation_power, fast.occupation_power, eps), "occupation: the companies do not change the power of capture");

		country->grant_unlock(UNLOCK_MECHANIC_INSTANT_OCCUPATION);
		const DivisionStats instant = compute_division_stats(im, *country, make(grid_of(*line_infantry, 3, 2)));
		r.check(instant.occupation_instant && instant.occupation_days == fixed_point_t::_0, "occupation: mechanic:instant_occupation makes the capture instant");

		country->revoke_unlock(UNLOCK_MECHANIC_INSTANT_OCCUPATION);
		if (had_key) {
			country->grant_unlock(UNLOCK_MECHANIC_INSTANT_OCCUPATION);
		}
		country->remove_unlock_modifier(*general_staff->modifier);
		country->remove_unlock_modifier(*operational_planning->modifier);
		if (had_general_staff) {
			country->add_unlock_modifier(*general_staff->modifier);
		}
		if (had_operational) {
			country->add_unlock_modifier(*operational_planning->modifier);
		}
		refresh();
	}

	// ---- the modifiers of the army with a scope (military/UnitModifiers.hpp)

	void test_scoped_modifiers(Results& r, InstanceManager& im) {
		namespace fs = std::filesystem;
		ModifierManager const& modifier_manager = im.definition_manager.get_modifier_manager();
		ModifierEffectCache const& cache = modifier_manager.get_modifier_effect_cache();
		UnitTypeManager const& unit_types = im.definition_manager.get_military_manager().get_unit_type_manager();
		RegimentType const* const infantry = unit_types.get_regiment_type_by_identifier("infantry");
		RegimentType const* const cavalry = unit_types.get_regiment_type_by_identifier("cavalry");
		RegimentType const* const artillery = unit_types.get_regiment_type_by_identifier("artillery");
		r.check(
			cache.find_unit_scope("all") != ModifierEffectCache::NO_SCOPE && cache.find_unit_scope("infantry") != ModifierEffectCache::NO_SCOPE
				&& cache.find_unit_scope("cavalry") != ModifierEffectCache::NO_SCOPE && cache.find_unit_scope("artillery") != ModifierEffectCache::NO_SCOPE
				&& infantry != nullptr && cavalry != nullptr && artillery != nullptr,
			"modifiers: the scopes all, infantry, cavalry, artillery and the ids of the unit types are registered"
		);
		if (infantry == nullptr || cavalry == nullptr || artillery == nullptr) {
			return;
		}

		// Two modifiers from a file (the form of the files of the technologies)
		const fs::path temp = fs::temp_directory_path() / "openvic_selftest_scoped";
		std::error_code ec;
		fs::remove_all(temp, ec);
		write_test_file(temp / "good.txt",
			"step = { year = 1836 modifier = { attack_mult = { all = 0.05 infantry = 0.10 } casualties_taken_mult = { all = 0.8 } occupation_speed_pct = 0.25\n"
			" occupation_speed_mult = 2.0 fortification = { infantry = 1 } } }\n"
			"step = { year = 1836 modifier = { attack_mult = { cavalry = 0.02 } casualties_taken_mult = { all = 0.9 } occupation_speed_pct = 0.10 occupation_speed_mult = 1.5 } }\n");
		write_test_file(temp / "bad.txt", "step = { year = 1836 modifier = { attack_mult = { no_such_scope = 0.5 } } }\n");
		ProvisionalUnlocks good, bad;
		{
			const ovdl::v2script::Parser parser = Dataloader::parse_defines(temp / "good.txt");
			good.load_file("good.txt", parser.get_file_node(), modifier_manager);
		}
		{
			const ovdl::v2script::Parser parser = Dataloader::parse_defines(temp / "bad.txt");
			bad.load_file("bad.txt", parser.get_file_node(), modifier_manager);
		}
		r.check(good.get_steps().size() == 2 && bad.get_steps().empty(), "modifiers: a scope that does not exist makes the step broken (it is skipped), the good file is loaded");
		if (good.get_steps().size() != 2) {
			fs::remove_all(temp, ec);
			return;
		}

		CountryInstance* country = nullptr;
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (candidate.exists()) {
				country = &candidate;
			}
		}
		StaticModifierCache const& static_modifier_cache = modifier_manager.get_static_modifier_cache();
		const auto refresh = [&]() {
			country->update_modifier_sum(im.get_today(), static_modifier_cache);
		};
		const auto weights_of = [&](RegimentType const& regiment_type) { return get_scope_weights(cache, regiment_type); };
		const std::size_t unlock_modifiers_before = country->get_unlock_modifiers().size();
		country->add_unlock_modifier(*good.get_steps()[0].modifier);
		country->add_unlock_modifier(*good.get_steps()[1].modifier);
		refresh();

		// The percents are summed, by the scope of the unit
		const auto infantry_weights = weights_of(*infantry);
		const auto cavalry_weights = weights_of(*cavalry);
		const auto artillery_weights = weights_of(*artillery);
		r.check(
			near(get_scoped_sum(*country, cache, scoped_effect_t::ATTACK_MULT, infantry_weights), fixed_point_t(15) / 100, fixed_point_t(1) / 1000)
				&& near(get_scoped_sum(*country, cache, scoped_effect_t::ATTACK_MULT, cavalry_weights), fixed_point_t(7) / 100, fixed_point_t(1) / 1000)
				&& near(get_scoped_sum(*country, cache, scoped_effect_t::ATTACK_MULT, artillery_weights), fixed_point_t(5) / 100, fixed_point_t(1) / 1000),
			"modifiers: the percents of the scopes are summed (infantry 5 + 10, cavalry 5 + 2, artillery 5)",
			fmt::format("{} {} {}", dbl(get_scoped_sum(*country, cache, scoped_effect_t::ATTACK_MULT, infantry_weights)),
				dbl(get_scoped_sum(*country, cache, scoped_effect_t::ATTACK_MULT, cavalry_weights)), dbl(get_scoped_sum(*country, cache, scoped_effect_t::ATTACK_MULT, artillery_weights)))
		);
		// The multipliers are multiplied
		r.check(
			near(get_scoped_product(*country, cache, scoped_effect_t::CASUALTIES_TAKEN_MULT, infantry_weights), fixed_point_t(72) / 100, fixed_point_t(1) / 1000),
			"modifiers: the multipliers are multiplied (0.8 x 0.9 = 0.72)",
			fmt::format("{}", dbl(get_scoped_product(*country, cache, scoped_effect_t::CASUALTIES_TAKEN_MULT, infantry_weights)))
		);
		r.check(
			near(country->get_modifier_effect_value(*cache.get_occupation_speed_pct()), fixed_point_t(35) / 100, fixed_point_t(1) / 1000)
				&& near(get_effect_product(*country, cache.get_occupation_speed_mult()), fixed_point_t(3), fixed_point_t(1) / 1000),
			"modifiers: the occupation speed adds the percents (0.25 + 0.10) and multiplies the multipliers (2 x 1.5)"
		);
		// A unit that counts half for two scopes (the dragoons) gets half of each
		memory::vector<ScopeWeight> half_weights;
		half_weights.push_back({ cache.find_unit_scope("infantry"), fixed_point_t::_0_50 });
		half_weights.push_back({ cache.find_unit_scope("cavalry"), fixed_point_t::_0_50 });
		r.check(
			near(get_scoped_sum(*country, cache, scoped_effect_t::FORTIFICATION, half_weights), fixed_point_t::_0_50, fixed_point_t(1) / 1000)
				&& near(get_scoped_sum(*country, cache, scoped_effect_t::ATTACK_MULT, half_weights), fixed_point_t(6) / 100, fixed_point_t(1) / 1000),
			"modifiers: a unit with the weights infantry 0.5 and cavalry 0.5 gets half of the bonuses of each (fortification 0.5)"
		);
		// The fortification of a template is the average of the regiments: 1 for the infantry, 0 for the cavalry gives 0.5
		DivisionTemplate division_template;
		division_template.cells[DivisionTemplate::cell_index(0, 0)] = static_cast<std::uint8_t>(get_division_slot_kind(*infantry) + 1);
		division_template.cells[DivisionTemplate::cell_index(1, 0)] = static_cast<std::uint8_t>(get_division_slot_kind(*cavalry) + 1);
		division_template.rebuild_counts();
		const DivisionStats stats = compute_division_stats(im, *country, division_template);
		r.check(near(stats.fortification, fixed_point_t::_0_50, fixed_point_t(1) / 1000), "modifiers: a template of half infantry (fortification 1) and half cavalry (0) has the fortification 0.5",
			fmt::format("{}", dbl(stats.fortification)));

		// put the country back
		country->remove_unlock_modifier(*good.get_steps()[0].modifier);
		country->remove_unlock_modifier(*good.get_steps()[1].modifier);
		(void)unlock_modifiers_before;
		refresh();
		fs::remove_all(temp, ec);
	}

	struct Game {
		GameManager& game_manager;
		Bookmark const& bookmark;

		InstanceManager* start() {
			if (!game_manager.setup_instance(bookmark) || !game_manager.start_game_session() || !game_manager.update_clock()) {
				return nullptr;
			}
			return game_manager.get_instance_manager();
		}
		void end() {
			game_manager.end_game_session();
		}
	};

	// The commands of a player on the first day: tariffs of the biggest countries, subsidies and a closed factory switch
	void give_orders(InstanceManager& instance_manager) {
		for (CountryInstance const& country : instance_manager.get_country_instance_manager().get_country_instances()) {
			if (country.get_total_rank() > 0 && country.get_total_rank() <= 12) {
				instance_manager.queue_game_action<set_tariff_rate_argument_t>(country.index, fixed_point_t(1) / 4);
				instance_manager.queue_game_action<set_all_factories_subsidised_argument_t>(country.index, true);
			}
		}
		// The country with the most money (the lowest index among equals) offers a credit to everybody
		CountryInstance const* richest = nullptr;
		for (CountryInstance const& country : instance_manager.get_country_instance_manager().get_country_instances()) {
			if (country.exists() && (richest == nullptr || country.get_cash_stockpile().load() > richest->get_cash_stockpile().load())) {
				richest = &country;
			}
		}
		if (richest != nullptr) {
			instance_manager.queue_game_action<offer_external_credit_argument_t>(
				richest->index, fixed_point_t(2000), fixed_point_t(6) / 100, std::uint32_t(1), true
			);
		}
		instance_manager.force_tick_and_update();
	}

	void play(InstanceManager& instance_manager, const int days) {
		for (int i = 0; i < days; ++i) {
			instance_manager.force_tick_and_update();
		}
	}

	// Plays the days and writes down the hash of every day (to find the first day two runs differ on)
	struct DayDetails {
		std::vector<std::int64_t> country_cash;
		std::vector<std::int64_t> country_free;
		std::int64_t pop_cash = 0, factory_budget = 0, capitalist_fund = 0, price_sum = 0;
	};

	DayDetails collect_details(InstanceManager& instance_manager) {
		DayDetails details;
		for (CountryInstance const& country : instance_manager.get_country_instance_manager().get_country_instances()) {
			details.country_cash.push_back(country.get_cash_stockpile().load().get_raw_value());
		}
		for (ProvinceInstance& province : instance_manager.get_map_instance().get_province_instances()) {
			for (Pop& pop : province.get_mutable_pops()) {
				details.pop_cash += pop.get_cash().get_copy_of_value().get_raw_value();
			}
		}
		for (StateSet const& state_set : instance_manager.get_map_instance().get_state_manager().get_state_sets()) {
			for (State const& state : state_set.get_states()) {
				details.capitalist_fund += state.get_capitalist_fund().get_raw_value();
				for (FactoryInstance const& factory : state.get_factories()) {
					details.factory_budget += factory.get_budget().get_raw_value();
				}
			}
		}
		for (GoodInstance const& good : instance_manager.get_good_instance_manager().get_good_instances()) {
			details.price_sum += good.get_price().get_raw_value();
		}
		return details;
	}

	std::vector<DayDetails> g_details_1, g_details_2;

	void play_recording(InstanceManager& instance_manager, const int days, std::vector<std::uint64_t>& hashes, std::vector<DayDetails>& details) {
		for (int i = 0; i < days; ++i) {
			instance_manager.force_tick_and_update();
			hashes.push_back(StateHash::compute(instance_manager));
			details.push_back(collect_details(instance_manager));
		}
	}

	std::string hex(const std::uint64_t value) {
		return Replay::hash_to_string(value);
	}

	// Credits: the formulas of docs/CREDITS.md on a credit that is not in a game
	Loan make_test_loan(Loan::term_t term, std::int32_t term_days, std::int32_t delay_days, fixed_point_t principal, fixed_point_t rate) {
		Loan loan;
		loan.kind = Loan::kind_t::EXTERNAL;
		loan.principal = loan.principal_left = principal;
		loan.annual_rate = rate;
		loan.term = term;
		loan.term_days = term_days;
		loan.delay_days = delay_days;
		return loan;
	}

	double dbl(fixed_point_t x) {
		return static_cast<double>(x);
	}

	std::int32_t installments_for_medium() {
		return Loan::calculate_installments(3650, 1825);
	}

	bool near(fixed_point_t a, fixed_point_t b, fixed_point_t tolerance) {
		return (a > b ? a - b : b - a) <= tolerance;
	}

	void test_credit_pure(Results& r) {
		const Date start { 1836, 1, 1 };
		const fixed_point_t principal = fixed_point_t(1000);
		const fixed_point_t rate = fixed_point_t(5) / 100;

		// Total interest: principal * rate * (delay + repayment / 2) years
		const fixed_point_t total_interest = Loan::calculate_total_interest(principal, rate, 913, 912);
		const fixed_point_t expected_interest = fixed_point_t(1000) * 5 / 100 * (913 + 456) / 365;
		r.check(near(total_interest, expected_interest, fixed_point_t(1) / 10), "credit: the total interest is principal * rate * (delay + repayment / 2)",
			fmt::format("{} != {}", dbl(total_interest), dbl(expected_interest)));

		// The whole life of a short credit (5 years, payments start after 2.5 years) paid on time
		Loan loan = make_test_loan(Loan::term_t::SHORT, 1825, 913, principal, rate);
		loan.start_schedule(start);
		const std::int32_t installments = loan.installments_left;
		r.check(installments == 31, "credit: a short credit has 31 monthly payments after the delay", std::to_string(installments));
		r.check(loan.get_installment_due(start + Timespan::from_days(912)) == fixed_point_t::_0, "credit: nothing is due before the delay ends");
		fixed_point_t first_due = loan.get_installment_due(start + Timespan::from_days(913));
		bool equal_installments = first_due > fixed_point_t::_0;
		fixed_point_t paid_total = 0;
		Date day = start + Timespan::from_days(913);
		for (int i = 0; i < installments && loan.installments_left > 0; i++) {
			const fixed_point_t due = loan.get_installment_due(day);
			if (!near(due, first_due, fixed_point_t(1) / 100)) {
				equal_installments = false;
			}
			loan.apply_installment_payment(due, due);
			paid_total += due;
			day = day + Timespan::from_days(30);
		}
		r.check(equal_installments, "credit: the payments are equal over the whole period of payments (interest spread evenly)");
		r.check(loan.get_remaining() == fixed_point_t::_0 && loan.installments_left == 0, "credit: the credit is paid off by the last installment",
			fmt::format("left {}", dbl(loan.get_remaining())));
		r.check(near(paid_total, principal + total_interest, fixed_point_t(1) / 10), "credit: in total the principal and the interest are paid",
			fmt::format("{} != {}", dbl(paid_total), dbl(principal + total_interest)));
		r.check(near(loan.interest_paid, total_interest, fixed_point_t(1) / 10), "credit: the interest paid equals the interest of the schedule");

		// A missed payment is not lost: the next one is bigger
		Loan late = make_test_loan(Loan::term_t::MEDIUM, 3650, 1825, principal, rate);
		late.start_schedule(start);
		const Date first_day = start + Timespan::from_days(1825);
		const fixed_point_t due_1 = late.get_installment_due(first_day);
		late.apply_installment_payment(due_1, fixed_point_t::_0);
		r.check(late.installments_left == installments_for_medium() - 1 && late.get_remaining() == principal + Loan::calculate_total_interest(principal, rate, 1825, 1825), "credit: a missed month passes, nothing of the debt is paid");
		const fixed_point_t due_2 = late.get_installment_due(first_day + Timespan::from_days(30));
		r.check(due_2 > due_1, "credit: what was not paid is added to the next payments");

		// Early repayment of everything during the delay: only the interest earned so far is owed
		Loan early = make_test_loan(Loan::term_t::SHORT, 1825, 913, principal, rate);
		early.start_schedule(start);
		const Date year_later = start + Timespan::from_days(365);
		const fixed_point_t cost = early.get_full_repayment_cost(year_later);
		r.check(near(cost, principal + principal * rate, fixed_point_t(1) / 100), "credit: early repayment in the delay costs the principal and the interest of that time",
			fmt::format("{}", dbl(cost)));
		const fixed_point_t paid_early = early.apply_early_repayment(year_later, cost);
		r.check(paid_early == cost && early.get_remaining() == fixed_point_t::_0, "credit: a full early repayment ends the credit with no future interest");
		r.check(near(early.interest_paid, principal * rate, fixed_point_t(1) / 100), "credit: the interest paid of an early repayment is the interest earned");

		// A part of the principal paid early lowers all the later payments
		Loan partial = make_test_loan(Loan::term_t::SHORT, 1825, 913, principal, rate);
		partial.start_schedule(start);
		const Date in_payments = start + Timespan::from_days(913 + 300);
		const fixed_point_t due_before = partial.get_installment_due(in_payments);
		partial.apply_early_repayment(in_payments, fixed_point_t(400));
		const fixed_point_t due_after = partial.get_installment_due(in_payments);
		r.check(partial.principal_left == fixed_point_t(600) && due_after < due_before, "credit: a partial early repayment lowers the payments",
			fmt::format("{} -> {}", dbl(due_before), dbl(due_after)));

		// Short credits only fund constructions of a year or less
		Loan short_loan = make_test_loan(Loan::term_t::SHORT, 1825, 913, principal, rate);
		Loan long_loan = make_test_loan(Loan::term_t::LONG, 7300, 3650, principal, rate);
		r.check(
			short_loan.can_fund(Loan::purpose_t::CONSTRUCTION, 365, 365) && !short_loan.can_fund(Loan::purpose_t::CONSTRUCTION, 366, 365)
				&& !short_loan.can_fund(Loan::purpose_t::LOSS_COVER, 100, 365) && long_loan.can_fund(Loan::purpose_t::LOSS_COVER, 5000, 365),
			"credit: a short credit funds only constructions of at most 365 days, the others anything"
		);

		// The limits of the AI are in the defines: it lends at 1% or more, it borrows at 25% or less
		OpenVicCreditDefines const defaults;
		r.check(
			defaults.interest_min.numerator == 100 && defaults.interest_max.numerator == 2500 && defaults.term_long_days == 7300,
			"credit: the defaults of the defines are 1% (lending), 25% (borrowing) and 20 years for long credits"
		);
	}

	void play_to_month_start(InstanceManager& instance_manager) {
		for (int i = 0; i < 35; ++i) {
			instance_manager.force_tick_and_update();
			if (instance_manager.get_today().is_month_start()) {
				return;
			}
		}
	}

	void test_credit_game(Results& r, InstanceManager& im) {
		CreditManager& credit = im.get_credit_manager();
		memory::vector<CountryInstance*> rich;
		for (CountryInstance& country : im.get_country_instance_manager().get_country_instances()) {
			if (country.exists() && !country.is_rebel_country() && country.get_total_rank() > 0) {
				country.set_ai(true);
				rich.push_back(&country);
			}
		}
		std::sort(rich.begin(), rich.end(), [](CountryInstance* a, CountryInstance* b) -> bool {
			return a->get_total_rank() < b->get_total_rank();
		});
		if (rich.size() < 4) {
			r.check(false, "credit: the game has at least four countries");
			return;
		}
		for (std::size_t i = 0; i < 4; i++) {
			rich[i]->set_ai(false);
		}
		CountryInstance& lender = *rich[0];
		CountryInstance& borrower = *rich[1];
		CountryInstance& stranger = *rich[2];
		CountryInstance& heir = *rich[3];
		lender.add_cash(fixed_point_t(5000));
		const fixed_point_t amount = fixed_point_t(1000);
		const fixed_point_t rate = fixed_point_t(5) / 100;

		// An offer to one country: the money is held, the others cannot take it
		const fixed_point_t before = lender.get_cash_stockpile().load();
		credit.stage_target_country(lender, borrower, true);
		Loan* offer = credit.offer_external_credit(im, lender, amount, rate, Loan::term_t::MEDIUM, false);
		r.check(offer != nullptr && lender.get_cash_stockpile().load() == before - amount, "credit: an offer holds the money of the lender");
		if (offer == nullptr) {
			return;
		}
		const std::uint32_t id = offer->id;
		r.check(!credit.take_credit(im, stranger, id), "credit: a country that is not named cannot take an offer made to a tag");
		const fixed_point_t borrower_before = borrower.get_cash_stockpile().load();
		r.check(credit.take_credit(im, borrower, id) && borrower.get_cash_stockpile().load() == borrower_before + amount,
			"credit: the named country takes the offer and gets the money");
		r.check(offer->status == Loan::status_t::ACTIVE && offer->payments_start == im.get_today() + Timespan::from_days(1825)
				&& offer->due_date == im.get_today() + Timespan::from_days(3650),
			"credit: a medium credit has 10 years and the payments start after 5");

		// An offer that is withdrawn gives the money back
		const fixed_point_t before_2 = lender.get_cash_stockpile().load();
		Loan* second = credit.offer_external_credit(im, lender, amount, rate, Loan::term_t::SHORT, true);
		r.check(second != nullptr && lender.get_cash_stockpile().load() == before_2 - amount, "credit: a second offer (to all) holds the money");
		const std::uint32_t second_id = second != nullptr ? second->id : 0;
		r.check(credit.withdraw_offer(im, lender, second_id) && lender.get_cash_stockpile().load() == before_2,
			"credit: a withdrawn offer returns the money");
		r.check(!credit.take_credit(im, stranger, second_id), "credit: a withdrawn offer cannot be taken");

		// The rate never changes after it is set
		r.check(offer->annual_rate == rate, "credit: the rate is set once when the credit is offered");

		// The borrower goes bankrupt (it loses all its provinces): the debt is written off
		memory::vector<ProvinceInstance*> provinces { borrower.get_owned_provinces().begin(), borrower.get_owned_provinces().end() };
		for (ProvinceInstance* province : provinces) {
			province->set_owner(&lender);
		}
		play_to_month_start(im);
		r.check(credit.get_loan_by_id(id) == nullptr, "credit: the credit of a bankrupt borrower is written off");

		// A region with capitalists changes its owner: the credit goes to the new owner
		State* region = nullptr;
		CountryInstance* old_owner = nullptr;
		for (CountryInstance* country : { &stranger, &heir }) {
			for (State* state : country->get_states()) {
				if (state->get_capitalist_fund() >= fixed_point_t::_0 && state->get_provinces().size() > 0 && region == nullptr) {
					region = state;
					old_owner = country;
				}
			}
		}
		if (region == nullptr || old_owner == nullptr) {
			r.check(false, "credit: found a region for the owner change test");
			return;
		}
		CountryInstance& new_owner = old_owner == &stranger ? heir : stranger;
		old_owner->add_cash(fixed_point_t(5000));
		// The lender offers to all regions, so no staging is needed
		Loan* internal = credit.offer_internal_credit(im, *old_owner, amount, rate, Loan::term_t::LONG, true);
		r.check(internal != nullptr, "credit: a country offers an internal credit to the capitalists of its regions");
		if (internal == nullptr) {
			return;
		}
		const std::uint32_t internal_id = internal->id;
		r.check(credit.take_internal_credit(im, internal_id, *region) && internal->unspent == amount,
			"credit: the capitalists of a region take the internal credit, the money is theirs to spend");
		for (ProvinceInstance& province : region->get_provinces()) {
			province.set_owner(&new_owner);
		}
		play_to_month_start(im);
		Loan const* moved = credit.get_loan_by_id(internal_id);
		r.check(moved != nullptr && moved->lender == &new_owner, "credit: when a region changes its owner the new owner is the lender");

		// The AI: it never lends below the minimum interest and never borrows above the maximum
		play(im, 150);
		OpenVicCreditDefines const& limits = credit.get_defines();
		bool lends_ok = true, borrows_ok = true;
		std::size_t ai_offers = 0, ai_taken = 0;
		for (auto const& loan : credit.get_loans()) {
			if (loan->lender->is_ai()) {
				ai_offers++;
				lends_ok = lends_ok && loan->annual_rate >= limits.interest_min.fixed();
			}
			if (loan->status == Loan::status_t::ACTIVE && loan->kind == Loan::kind_t::EXTERNAL && loan->borrower->is_ai()) {
				ai_taken++;
				borrows_ok = borrows_ok && loan->annual_rate <= limits.interest_max.fixed();
			}
		}
		r.check(lends_ok, "credit: the AI never offers a credit below the minimum interest");
		r.check(borrows_ok, "credit: the AI never takes a credit above the maximum interest");
		{
			std::size_t ai_count = 0, richest_ai_index = 0;
			fixed_point_t richest = 0;
			for (CountryInstance const& country : im.get_country_instance_manager().get_country_instances()) {
				if (country.exists() && country.is_ai()) {
					ai_count++;
					if (country.get_cash_stockpile().load() > richest) {
						richest = country.get_cash_stockpile().load();
						richest_ai_index = type_safe::get(country.index);
					}
				}
			}
			fmt::println("TEST INFO credit: {} AI countries, the richest ({}) has {}", ai_count, richest_ai_index, static_cast<double>(richest));
		}
		fmt::println("TEST INFO credit: after 150 days {} credits of AI lenders exist, {} external credits taken by AI countries", ai_offers, ai_taken);
	}

	void test_factory_actions(Results& r, InstanceManager& im) {
		State* found_state = nullptr;
		BuildingType const* found_type = nullptr;
		for (StateSet& state_set : im.get_map_instance().get_state_manager().get_mutable_state_sets()) {
			for (State& state : state_set.get_mutable_states()) {
				if (found_state == nullptr && state.get_owner() != nullptr && state.get_capital() != nullptr
					&& !state.get_factories().empty()) {
					found_state = &state;
					found_type = &state.get_factories().begin()->building_type;
				}
			}
		}
		r.check(found_state != nullptr, "factories: a state with a factory exists at the start");
		if (found_state == nullptr) {
			return;
		}
		CountryInstance& owner = *found_state->get_owner();
		const province_index_t province = found_state->get_capital()->index;
		const building_type_index_t type = found_type->index;

		FactoryInstance* factory = found_state->find_factory(*found_type);
		r.check(factory->get_priority() == 1, "factories: the default priority is normal");
		im.queue_game_action<set_factory_priority_argument_t>(owner.index, province, type, std::uint32_t(2));
		im.force_tick_and_update();
		factory = found_state->find_factory(*found_type);
		r.check(factory != nullptr && factory->get_priority() == 2, "factories: the priority of a factory can be raised");
		im.queue_game_action<set_factory_priority_argument_t>(owner.index, province, type, std::uint32_t(7));
		im.force_tick_and_update();
		r.check(found_state->find_factory(*found_type)->get_priority() == 2, "factories: a wrong priority is refused");

		const std::size_t before = found_state->get_factories().size();
		im.queue_game_action<delete_factory_argument_t>(owner.index, province, type);
		im.force_tick_and_update();
		r.check(found_state->get_factories().size() + 1 == before && found_state->find_factory(*found_type) == nullptr,
			"factories: a factory can be deleted by its owner");
		play(im, 5);
	}

	// Supply limit of the provinces (docs/PLAN_ARMY_SUPPLY_FRONTS.md, sections 1 and 2): the parts of the breakdown make the capacity, it
	// is never negative, the better terrain gives more, the railroad and the own land raise it, the load is the consumption of the
	// regiments, and the coefficient of the land that is not held depends on how far the held provinces are
	void test_supply_limit(Results& r, InstanceManager& im) {
		OpenVicSupplyDefines const& defines = im.definition_manager.get_define_manager().get_openvic_defines().supply;
		CountryInstanceManager const& countries = im.get_country_instance_manager();
		std::size_t land = 0, bad_sum = 0, negative = 0, bad_rail = 0, bad_home = 0, bad_load = 0, with_army = 0, bad_coefficient = 0;
		fixed_point_t best_local = fixed_point_t::_0, worst_local = fixed_point_t(100000);
		std::size_t railroads = 0;
		for (ProvinceInstance const& province : im.get_map_instance().get_province_instances()) {
			if (province.province_definition.is_water()) {
				continue;
			}
			CountryInstance const* const owner = province.get_owner();
			if (owner == nullptr) {
				continue;
			}
			land++;
			SupplyFriends const friends = SupplyFriends::build(*owner, countries, im.get_war_manager());
			SupplyTerritory const territory =
				calculate_supply_territory(province, im.get_map_instance(), *owner, friends, defines);
			bad_coefficient += territory.coefficient < fixed_point_t::_0 || territory.coefficient > fixed_point_t::_1;
			ProvinceInstance::supply_breakdown_t const own = province.get_supply_breakdown(owner, territory);
			const fixed_point_t expected = (own.local_limit + own.country_limit) * own.limit_multiplier * own.rail_multiplier
				* own.technology_multiplier * (fixed_point_t::_1 + own.home_bonus) * territory.coefficient;
			bad_sum += (own.capacity != std::max(fixed_point_t::_0, expected));
			negative += own.capacity < fixed_point_t::_0;
			bad_rail += own.rail_multiplier != fixed_point_t::_1 + defines.supply_rail_bonus_per_level.fixed() * own.rail_level;
			railroads += own.rail_level > fixed_point_t::_0;
			bad_home += !own.is_home || own.home_bonus != owner->get_supply_home_bonus();
			// another country: no bonus of the own land
			for (CountryInstance const& other : countries.get_country_instances()) {
				if (&other != owner && other.exists()) {
					ProvinceInstance::supply_breakdown_t const foreign = province.get_supply_breakdown(&other, SupplyTerritory {});
					bad_home += foreign.is_home || foreign.home_bonus != fixed_point_t::_0;
					break;
				}
			}
			if (own.local_limit > best_local) {
				best_local = own.local_limit;
			}
			if (own.local_limit < worst_local) {
				worst_local = own.local_limit;
			}
			fixed_point_t load = fixed_point_t::_0;
			for (ArmyInstance const& army : province.get_armies()) {
				if (&army.get_country() == owner) {
					for (RegimentInstance const& regiment : army.get_regiment_instances()) {
						load += regiment.unit_type.supply_consumption;
					}
				}
			}
			with_army += load > fixed_point_t::_0;
			bad_load += own.load != load;
		}
		r.check(land > 1000, "supply limit: the land provinces with an owner are counted", fmt::format("{}", land));
		r.check(bad_sum == 0, "supply limit: the capacity is (local + global) * base * railroad * technologies * (1 + own land) * territory", fmt::format("{} provinces", bad_sum));
		r.check(bad_coefficient == 0, "supply limit: the supply coefficient of the territory is always between 0 and 1", fmt::format("{} provinces", bad_coefficient));
		r.check(negative == 0, "supply limit: the capacity is never negative");
		r.check(bad_rail == 0, "supply limit: every level of the railroad adds the same share (the levels are added)", fmt::format("{} provinces", bad_rail));
		r.check(bad_home == 0, "supply limit: the bonus of the own land is only for the owner", fmt::format("{} provinces", bad_home));
		r.check(best_local > worst_local && worst_local > fixed_point_t::_0, "supply limit: the better terrain gives a higher limit than the worse one",
			fmt::format("{} .. {}", worst_local, best_local));
		r.check(with_army > 0, "supply limit: there are provinces with the regiments of their owner");
		r.check(bad_load == 0, "supply limit: the load is the supply consumption of the regiments of the country", fmt::format("{} provinces", bad_load));
		r.check(railroads > 0, "supply limit: there are provinces with a railroad at the start", fmt::format("{}", railroads));

		// ---- The friends: the country, an ally, a country that gave the passage, the same side of a war; not an enemy
		CountryInstance* first = nullptr;
		CountryInstance* ally = nullptr;
		CountryInstance* stranger = nullptr;
		CountryInstance* enemy = nullptr;
		for (CountryInstance& country : im.get_country_instance_manager().get_country_instances()) {
			if (!country.exists() || country.get_total_rank() == 0) {
				continue;
			}
			if (first == nullptr) {
				first = &country;
			} else if (ally == nullptr) {
				ally = &country;
			} else if (stranger == nullptr) {
				stranger = &country;
			} else if (enemy == nullptr) {
				enemy = &country;
			}
		}
		r.check(enemy != nullptr, "supply friends: four countries exist");
		if (enemy != nullptr) {
			SupplyFriends friends = SupplyFriends::build(*first, countries, im.get_war_manager());
			r.check(friends.contains(first), "supply friends: a country is its own friend");
			r.check(!friends.contains(ally) && !friends.contains(stranger) && !friends.contains(nullptr),
				"supply friends: nobody else is a friend without an alliance, a passage or a war on the same side");
			ally->set_alliance_with(*first, true);
			stranger->set_military_access_to(*first, false);
			first->set_military_access_to(*stranger, true);
			friends = SupplyFriends::build(*first, countries, im.get_war_manager());
			r.check(friends.contains(ally), "supply friends: an ally is a friend");
			r.check(friends.contains(stranger), "supply friends: a country that gave the passage of the troops is a friend");
			r.check(!friends.contains(enemy), "supply friends: the others are still not friends");
			// A country that took the passage from the first one does not give it supply (the passage is one way)
			SupplyFriends const other_side = SupplyFriends::build(*stranger, countries, im.get_war_manager());
			r.check(!other_side.contains(first), "supply friends: the passage works one way");
			first->set_military_access_to(*stranger, false);
			ally->set_alliance_with(*first, false);

			// War: the enemy is not a friend; the countries on the same side are
			if (im.get_war_manager().declare_war(im, *first, *enemy)) {
				friends = SupplyFriends::build(*first, countries, im.get_war_manager());
				r.check(!friends.contains(enemy), "supply friends: the enemy in a war is not a friend");
				bool all_side = true;
				for (War const* war : im.get_war_manager().get_wars_of(*first)) {
					for (CountryInstance const* member : war->get_side(war->is_attacker(*first))) {
						all_side &= friends.contains(member);
					}
					for (CountryInstance const* member : war->get_side(!war->is_attacker(*first))) {
						all_side &= !friends.contains(member);
					}
				}
				r.check(all_side, "supply friends: the countries on the same side of the war are friends, the other side is not");
			}
		}

		// ---- The coefficient of the land that is not held, checked by a different way (sets of the provinces) on every province
		if (first != nullptr) {
			const SupplyFriends friends = SupplyFriends::build(*first, countries, im.get_war_manager());
			MapInstance const& map = im.get_map_instance();
			const auto land_neighbours = [&map](ProvinceInstance const& province) {
				std::vector<ProvinceInstance const*> result;
				for (ProvinceDefinition::adjacency_t const& adjacency : province.province_definition.get_adjacencies()) {
					if (adjacency.get_type() == ProvinceDefinition::adjacency_t::type_t::LAND) {
						result.push_back(&map.get_province_instance_by_definition(adjacency.get_to()));
					}
				}
				return result;
			};
			std::size_t checked = 0, wrong = 0, held_wrong = 0;
			std::size_t kinds[6] = {};
			for (ProvinceInstance const& province : map.get_province_instances()) {
				if (province.province_definition.is_water()) {
					continue;
				}
				SupplyTerritory const got = calculate_supply_territory(province, map, *first, friends, defines);
				kinds[static_cast<std::size_t>(got.kind)]++;
				if (is_supply_held(province, friends)) {
					held_wrong += got.kind != SupplyTerritory::kind_t::HELD || got.coefficient != fixed_point_t::_1;
					continue;
				}
				checked++;
				// Level 1: the held neighbours
				std::vector<ProvinceInstance const*> level1 = land_neighbours(province);
				std::size_t held1 = 0;
				std::vector<ProvinceInstance const*> between;
				for (ProvinceInstance const* neighbour : level1) {
					if (is_supply_held(*neighbour, friends)) {
						held1++;
					} else {
						between.push_back(neighbour);
					}
				}
				SupplyTerritory::kind_t expected_kind = SupplyTerritory::kind_t::FAR;
				fixed_point_t expected_coefficient = defines.supply_far.fixed();
				if (held1 >= 2) {
					expected_kind = SupplyTerritory::kind_t::TWO_NEIGHBOURS;
					expected_coefficient = fixed_point_t::_1;
				} else if (held1 == 1) {
					expected_kind = SupplyTerritory::kind_t::ONE_NEIGHBOUR;
					expected_coefficient = defines.supply_neighbour_one.fixed();
				} else {
					// Level 2: the held provinces next to the provinces between (not the province itself, not those between)
					std::vector<ProvinceInstance const*> held2;
					std::vector<ProvinceInstance const*> between2;
					const auto contains = [](std::vector<ProvinceInstance const*> const& list, ProvinceInstance const* item) {
						return std::find(list.begin(), list.end(), item) != list.end();
					};
					for (ProvinceInstance const* first_step : between) {
						for (ProvinceInstance const* second_step : land_neighbours(*first_step)) {
							if (second_step == &province || contains(between, second_step)) {
								continue;
							}
							if (is_supply_held(*second_step, friends)) {
								if (!contains(held2, second_step)) {
									held2.push_back(second_step);
								}
							} else if (!contains(between2, second_step)) {
								between2.push_back(second_step);
							}
						}
					}
					if (!held2.empty()) {
						expected_kind = SupplyTerritory::kind_t::ONE_BETWEEN;
						expected_coefficient = defines.supply_gap1_base.fixed()
							+ defines.supply_gap1_step.fixed()
								* (std::min<std::int32_t>(static_cast<std::int32_t>(held2.size()), defines.supply_gap1_max_count) - 1);
					}
				}
				// The search through two provinces is not open at the start (no technology), so the rest is far away
				wrong += got.kind != expected_kind || got.coefficient != expected_coefficient;
			}
			r.check(checked > 1000, "supply territory: many provinces are not held by a country and its friends", fmt::format("{}", checked));
			r.check(held_wrong == 0, "supply territory: the provinces that are held give 100 percent");
			r.check(wrong == 0, "supply territory: the kind and the coefficient equal the ones counted another way on every province",
				fmt::format("{} provinces", wrong));
			r.check(
				kinds[static_cast<std::size_t>(SupplyTerritory::kind_t::ONE_NEIGHBOUR)] > 0
					&& kinds[static_cast<std::size_t>(SupplyTerritory::kind_t::ONE_BETWEEN)] > 0
					&& kinds[static_cast<std::size_t>(SupplyTerritory::kind_t::FAR)] > 0,
				"supply territory: provinces of every kind exist", fmt::format("{} {} {}",
					kinds[static_cast<std::size_t>(SupplyTerritory::kind_t::ONE_NEIGHBOUR)],
					kinds[static_cast<std::size_t>(SupplyTerritory::kind_t::ONE_BETWEEN)],
					kinds[static_cast<std::size_t>(SupplyTerritory::kind_t::FAR)])
			);

			// The search through two provinces between (opened by a technology, or by the define): only the provinces that were far away change
			OpenVicSupplyDefines& opened = const_cast<OpenVicSupplyDefines&>(defines);
			std::size_t changed = 0, bad_changed = 0;
			for (ProvinceInstance const& province : map.get_province_instances()) {
				if (province.province_definition.is_water()) {
					continue;
				}
				opened.supply_gap2_enabled = 0;
				SupplyTerritory const closed = calculate_supply_territory(province, map, *first, friends, defines);
				opened.supply_gap2_enabled = 1;
				SupplyTerritory const open = calculate_supply_territory(province, map, *first, friends, defines);
				opened.supply_gap2_enabled = 0;
				if (closed.kind != open.kind || closed.coefficient != open.coefficient) {
					changed++;
					bad_changed += closed.kind != SupplyTerritory::kind_t::FAR || open.kind != SupplyTerritory::kind_t::TWO_BETWEEN
						|| open.coefficient < opened.supply_gap2_base.fixed()
						|| open.coefficient > opened.supply_gap2_base.fixed() + opened.supply_gap2_step.fixed() * (opened.supply_gap2_max_count - 1);
				}
			}
			r.check(changed > 0, "supply territory: with two provinces between opened some far provinces get the 25 percent kind", fmt::format("{}", changed));
			r.check(bad_changed == 0, "supply territory: opening it only changes far provinces and gives 25 to 45 percent", fmt::format("{} provinces", bad_changed));
		}
	}

	// ---- the supply does not go through the provinces that an enemy controls (docs/WAR_ASSUMPTIONS.md, section 5)

	void test_supply_through_enemy(Results& r, InstanceManager& im) {
		OpenVicSupplyDefines const& defines = im.definition_manager.get_define_manager().get_openvic_defines().supply;
		MapInstance& map = im.get_map_instance();
		CountryInstance* attacker = nullptr;
		for (CountryInstance& country : im.get_country_instance_manager().get_country_instances()) {
			if (country.is_at_war() && !country.get_war_enemies().empty() && country.exists()) {
				attacker = &country;
				break;
			}
		}
		if (attacker == nullptr) {
			fmt::println("TEST INFO supply through the enemy: no country at war, not checked");
			return;
		}
		const SupplyFriends friends = SupplyFriends::build(*attacker, im.get_country_instance_manager(), im.get_war_manager());
		const auto land_neighbours = [&map](ProvinceInstance const& province) {
			std::vector<ProvinceInstance const*> result;
			for (ProvinceDefinition::adjacency_t const& adjacency : province.province_definition.get_adjacencies()) {
				if (adjacency.get_type() == ProvinceDefinition::adjacency_t::type_t::LAND) {
					result.push_back(&map.get_province_instance_by_definition(adjacency.get_to()));
				}
			}
			return result;
		};
		const auto contains = [](std::vector<ProvinceInstance const*> const& list, ProvinceInstance const* item) {
			return std::find(list.begin(), list.end(), item) != list.end();
		};
		// The coefficient counted another way (sets of provinces), with the provinces of the enemy closed (closed = true) or open as before
		const auto expected_for = [&](ProvinceInstance const& province, const bool closed) -> std::pair<SupplyTerritory::kind_t, fixed_point_t> {
			std::size_t held1 = 0;
			std::vector<ProvinceInstance const*> between;
			for (ProvinceInstance const* neighbour : land_neighbours(province)) {
				if (is_supply_held(*neighbour, friends)) {
					held1++;
				} else if (!closed || !is_enemy_controlled(*neighbour, *attacker)) {
					between.push_back(neighbour);
				}
			}
			if (held1 >= 2) {
				return { SupplyTerritory::kind_t::TWO_NEIGHBOURS, fixed_point_t::_1 };
			}
			if (held1 == 1) {
				return { SupplyTerritory::kind_t::ONE_NEIGHBOUR, defines.supply_neighbour_one.fixed() };
			}
			std::vector<ProvinceInstance const*> held2;
			for (ProvinceInstance const* first_step : between) {
				for (ProvinceInstance const* second_step : land_neighbours(*first_step)) {
					if (second_step != &province && !contains(between, second_step) && is_supply_held(*second_step, friends) && !contains(held2, second_step)) {
						held2.push_back(second_step);
					}
				}
			}
			if (!held2.empty()) {
				return { SupplyTerritory::kind_t::ONE_BETWEEN, defines.supply_gap1_base.fixed()
					+ defines.supply_gap1_step.fixed() * (std::min<std::int32_t>(static_cast<std::int32_t>(held2.size()), defines.supply_gap1_max_count) - 1) };
			}
			return { SupplyTerritory::kind_t::FAR, defines.supply_far.fixed() };
		};
		std::size_t checked = 0, wrong = 0, differs_from_open = 0, enemy_provinces = 0;
		for (ProvinceInstance const& province : map.get_province_instances()) {
			if (province.province_definition.is_water() || is_supply_held(province, friends)) {
				continue;
			}
			checked++;
			enemy_provinces += is_enemy_controlled(province, *attacker);
			const SupplyTerritory got = calculate_supply_territory(province, map, *attacker, friends, defines);
			const auto closed = expected_for(province, true);
			const auto open = expected_for(province, false);
			wrong += got.kind != closed.first || got.coefficient != closed.second;
			differs_from_open += open.first != closed.first || open.second != closed.second;
		}
		r.check(checked > 1000 && enemy_provinces > 0, "supply through the enemy: a country at war has provinces of its enemy around", fmt::format("{} provinces, {} of the enemy", checked, enemy_provinces));
		r.check(wrong == 0, "supply through the enemy: the kind and the coefficient equal the ones counted another way, the provinces of the enemy closed", fmt::format("{} provinces", wrong));
		(void)differs_from_open;

		// A province behind a province of an enemy: with the province between held by nobody the supply comes through it (one province between), with the
		// province between taken by the enemy it does not
		{
			CountryInstance* const enemy = const_cast<CountryInstance*>(&attacker->get_war_enemies().front().get());
			ProvinceInstance* behind = nullptr;
			ProvinceInstance* between_province = nullptr;
			for (ProvinceInstance& candidate : map.get_province_instances()) {
				if (candidate.province_definition.is_water() || is_supply_held(candidate, friends) || is_enemy_controlled(candidate, *attacker)
					|| calculate_supply_territory(candidate, map, *attacker, friends, defines).kind != SupplyTerritory::kind_t::ONE_BETWEEN) {
					continue;
				}
				// the only way to the held land goes through one neighbour that nobody held
				ProvinceInstance* only_between = nullptr;
				std::size_t ways = 0;
				for (ProvinceInstance const* neighbour : land_neighbours(candidate)) {
					for (ProvinceInstance const* second_step : land_neighbours(*neighbour)) {
						if (second_step != &candidate && is_supply_held(*second_step, friends) && !is_supply_held(*neighbour, friends)) {
							ways++;
							only_between = const_cast<ProvinceInstance*>(neighbour);
						}
					}
				}
				if (ways >= 1 && only_between != nullptr && only_between->get_owner() != nullptr && enemy != nullptr && enemy != attacker) {
					// every neighbour that leads to held land is this one
					bool single = true;
					for (ProvinceInstance const* neighbour : land_neighbours(candidate)) {
						if (neighbour == only_between || is_supply_held(*neighbour, friends)) {
							continue;
						}
						for (ProvinceInstance const* second_step : land_neighbours(*neighbour)) {
							single &= second_step == &candidate || !is_supply_held(*second_step, friends);
						}
					}
					if (single) {
						behind = &candidate;
						between_province = only_between;
						break;
					}
				}
			}
			if (behind == nullptr) {
				fmt::println("TEST INFO supply through the enemy: no province behind a single province between, not checked");
			} else {
				CountryInstance* const old_controller = between_province->get_controller();
				between_province->set_controller(enemy);
				const SupplyTerritory closed = calculate_supply_territory(*behind, map, *attacker, friends, defines);
				between_province->set_controller(old_controller);
				const SupplyTerritory open = calculate_supply_territory(*behind, map, *attacker, friends, defines);
				r.check(open.kind == SupplyTerritory::kind_t::ONE_BETWEEN && closed.kind == SupplyTerritory::kind_t::FAR && closed.coefficient < open.coefficient,
					"supply through the enemy: a province behind a province taken by the enemy loses the supply that went through it (one province between becomes far)",
					fmt::format("{} -> {}", dbl(open.coefficient), dbl(closed.coefficient)));
			}
		}

		// A province taken at once (the instant occupation): the control and the supply are worked out at once, in the same tick
		ProvinceInstance* taken = nullptr;
		for (ProvinceInstance& province : map.get_province_instances()) {
			if (!province.province_definition.is_water() && province.get_owner() != nullptr && province.get_owner() == province.get_controller() && attacker->is_at_war_with(*province.get_owner())
				&& calculate_supply_territory(province, map, *attacker, friends, defines).kind != SupplyTerritory::kind_t::HELD) {
				taken = &province;
				break;
			}
		}
		if (taken != nullptr) {
			CountryInstance* const old_controller = taken->get_controller();
			taken->set_controller(attacker);
			const SupplyTerritory now = calculate_supply_territory(*taken, map, *attacker, SupplyFriends::build(*attacker, im.get_country_instance_manager(), im.get_war_manager()), defines);
			r.check(now.kind == SupplyTerritory::kind_t::HELD && now.coefficient == fixed_point_t::_1,
				"supply through the enemy: a province taken at once is held by the country in the same tick (100 percent, nothing is kept from before)");
			taken->set_controller(old_controller);
			const SupplyTerritory back = calculate_supply_territory(*taken, map, *attacker, friends, defines);
			r.check(back.kind != SupplyTerritory::kind_t::HELD, "supply through the enemy: the province given back is not held again");
		}
	}

	// The delivery of the supply to the armies, the priorities and the reserves of the regiments (docs/PLAN_ARMY_SUPPLY_FRONTS.md, section 3)
	void test_supply_delivery(Results& r, InstanceManager& im) {
		OpenVicSupplyDefines const& defines = im.definition_manager.get_define_manager().get_openvic_defines().supply;
		const fixed_point_t standard = get_supply_priority_multiplier(defines, supply_priority_t::STANDARD);
		const fixed_point_t increased = get_supply_priority_multiplier(defines, supply_priority_t::INCREASED);
		const fixed_point_t maximum = get_supply_priority_multiplier(defines, supply_priority_t::MAXIMUM);
		r.check(standard == fixed_point_t::_1 && increased == fixed_point_t(5) / 2 && maximum == fixed_point_t(5),
			"supply delivery: the priorities send 1, 2.5 and 5 times the need at most");

		// A province that lets through the share d of the need: the standard priority sends the need and gets the share d
		bool bounded = true, standard_ok = true, increased_ok = true, never_more_sent = true;
		for (int step = 0; step <= 100; ++step) {
			const fixed_point_t d = fixed_point_t(step) / 100;
			for (const fixed_point_t multiplier : { standard, increased, maximum }) {
				SupplyDeliveryResult const result = calculate_supply_delivery(d, multiplier);
				bounded &= result.delivery >= fixed_point_t::_0 && result.delivery <= fixed_point_t::_1;
				bounded &= result.send_multiplier >= fixed_point_t::_1 && result.send_multiplier <= multiplier;
				// Never more than makes up for the losses: what arrives does not exceed the need
				never_more_sent &= result.delivery <= fixed_point_t::_1;
				if (multiplier == standard) {
					standard_ok &= result.delivery == d && result.send_multiplier == fixed_point_t::_1;
				}
			}
			SupplyDeliveryResult const result = calculate_supply_delivery(d, increased);
			// With the increased priority half of the need is already made up for at d = 0.4 (0.4 * 2.5 = 1)
			if (step >= 41) {
				increased_ok &= result.delivery == fixed_point_t::_1;
			} else if (step > 0 && step < 40) {
				increased_ok &= result.delivery == d * increased;
			}
		}
		r.check(bounded && never_more_sent, "supply delivery: what is sent is between 1 and the multiplier of the priority, what arrives is never above the need");
		r.check(standard_ok, "supply delivery: the standard priority sends the need and what arrives is the share the province lets through");
		r.check(increased_ok, "supply delivery: the increased priority makes up for the losses up to 2.5 times the need");
		r.check(calculate_supply_delivery(fixed_point_t::_0, maximum).delivery == fixed_point_t::_0
				&& calculate_supply_delivery(fixed_point_t::_0, maximum).send_multiplier == fixed_point_t::_1,
			"supply delivery: nothing gets through and nothing is sent (a province without a limit does not divide by zero)");
		r.check(calculate_supply_delivery(fixed_point_t(3), maximum).delivery == fixed_point_t::_1,
			"supply delivery: a share above 1 does not give more than the need");

		// The reserve: a week without supplies lasts exactly the days of the reserve, then the regiment is without
		const fixed_point_t week = fixed_point_t(7);
		fixed_point_t reserve = week;
		int days = 0;
		fixed_point_t deficit = fixed_point_t::_0;
		while (deficit == fixed_point_t::_0 && days < 30) {
			SupplyReserveResult const result = step_supply_reserve(reserve, fixed_point_t::_1, fixed_point_t::_0, week);
			reserve = result.reserve;
			deficit = result.deficit;
			days++;
		}
		r.check(days == 8 && reserve == fixed_point_t::_0, "supply reserve: a regiment with a reserve of 7 days stands 7 days without supplies, then it has a deficit",
			fmt::format("{} days", days));
		SupplyReserveResult half = step_supply_reserve(week, fixed_point_t(1) / 2, fixed_point_t::_0, week);
		r.check(half.reserve == week - fixed_point_t(1) / 2 && half.deficit == fixed_point_t::_0, "supply reserve: half of the need arriving costs half a day of the reserve");
		SupplyReserveResult refilled = step_supply_reserve(fixed_point_t(3), fixed_point_t::_0, fixed_point_t::_1, week);
		r.check(refilled.reserve == fixed_point_t(4), "supply reserve: with everything arriving the reserve rises by the refill");
		SupplyReserveResult capped = step_supply_reserve(fixed_point_t(7), fixed_point_t::_0, fixed_point_t::_1, week);
		r.check(capped.reserve == week, "supply reserve: the reserve never rises above its capacity");
		SupplyReserveResult none = step_supply_reserve(fixed_point_t::_0, fixed_point_t(3) / 10, fixed_point_t::_0, fixed_point_t::_0);
		r.check(none.reserve == fixed_point_t::_0 && none.deficit == fixed_point_t(3) / 10, "supply reserve: without a reserve (no technology) the whole shortfall is a deficit");

		// The armies of a real game after some days
		play(im, 3);
		std::size_t armies = 0, bad_level = 0, bad_delivery = 0, bad_send = 0, bad_reserve = 0, with_reserve = 0;
		for (CountryInstance const& country : im.get_country_instance_manager().get_country_instances()) {
			const fixed_point_t capacity = country.get_supply_reserve_capacity();
			for (ArmyInstance const& army : country.get_armies()) {
				armies++;
				bad_level += army.get_supply_level() < fixed_point_t::_0 || army.get_supply_level() > fixed_point_t::_1;
				bad_delivery += army.get_supply_delivery() < fixed_point_t::_0 || army.get_supply_delivery() > fixed_point_t::_1
					|| army.get_supply_ratio() < fixed_point_t::_0 || army.get_supply_ratio() > fixed_point_t::_1;
				bad_send += army.get_supply_send_multiplier() < fixed_point_t::_1 || army.get_supply_send_multiplier() > maximum;
				for (RegimentInstance const& regiment : army.get_regiment_instances()) {
					bad_reserve += regiment.get_supply_reserve() < fixed_point_t::_0 || regiment.get_supply_reserve() > capacity;
					with_reserve += regiment.get_supply_reserve() > fixed_point_t::_0;
				}
			}
		}
		r.check(armies > 50, "supply delivery: there are armies", fmt::format("{}", armies));
		r.check(bad_level == 0 && bad_delivery == 0, "supply delivery: the supply level and what arrives of every army are between 0 and 1",
			fmt::format("{} {}", bad_level, bad_delivery));
		r.check(bad_send == 0, "supply delivery: no army is sent more than the multiplier of the maximum priority", fmt::format("{}", bad_send));
		r.check(bad_reserve == 0, "supply delivery: the reserve of every regiment is between 0 and the capacity of its country", fmt::format("{}", bad_reserve));
		r.check(with_reserve > 0, "supply delivery: the regiments of the civilised countries have a reserve");

		// The actions: the priority of an army, the return to the priority of the country, the priority of the country
		ArmyInstance* army = nullptr;
		for (CountryInstance& country : im.get_country_instance_manager().get_country_instances()) {
			if (country.has_armies()) {
				army = &country.get_armies().front().get();
				break;
			}
		}
		r.check(army != nullptr, "supply priority: an army exists");
		if (army != nullptr) {
			CountryInstance& owner = army->get_country();
			r.check(!army->is_supply_priority_custom() && owner.get_effective_supply_priority(false, army->get_supply_priority()) == supply_priority_t::STANDARD,
				"supply priority: an army has the priority of its country at first");
			im.queue_game_action<set_global_supply_priority_argument_t>(owner.index, std::uint32_t(1));
			im.force_tick_and_update();
			r.check(owner.get_army_supply_priority() == supply_priority_t::INCREASED, "supply priority: the priority of the country is set");
			im.queue_game_action<set_army_supply_priority_argument_t>(army->unique_id, std::uint32_t(2));
			im.force_tick_and_update();
			r.check(army->is_supply_priority_custom() && army->get_supply_priority() == supply_priority_t::MAXIMUM,
				"supply priority: an army can have its own priority");
			im.queue_game_action<set_global_supply_priority_argument_t>(owner.index, std::uint32_t(0));
			im.force_tick_and_update();
			r.check(army->get_supply_priority() == supply_priority_t::MAXIMUM && army->is_supply_priority_custom(),
				"supply priority: the priority of the country does not change an army that has its own");
			im.queue_game_action<set_army_supply_priority_argument_t>(army->unique_id, std::uint32_t(3));
			im.force_tick_and_update();
			r.check(!army->is_supply_priority_custom(), "supply priority: an army can return to the priority of its country");
		}

		// ---- An army on the land of an enemy far from its own land: the limit lets little through, the reserve is used up in days, then the
		// supply level falls (the war between two countries was declared by the tests above)
		{
			OpenVicSupplyDefines& open_defines = const_cast<OpenVicSupplyDefines&>(defines);
			CountryInstanceManager const& all_countries = im.get_country_instance_manager();
			ArmyInstance* raider = nullptr;
			for (CountryInstance& country : im.get_country_instance_manager().get_country_instances()) {
				// (a country with a reserve of supplies: the ones without mechanic:division_templates have none)
				if (!country.is_at_war() || country.get_war_enemies().empty() || !country.has_armies() || country.get_supply_reserve_capacity() <= fixed_point_t::_0) {
					continue;
				}
				for (ArmyInstance& candidate : country.get_armies()) {
					if (candidate.get_unit_count() >= 4 && (raider == nullptr || candidate.get_unit_count() > raider->get_unit_count())) {
						raider = &candidate;
					}
				}
				if (raider != nullptr) {
					break;
				}
			}
			r.check(raider != nullptr, "supply on enemy land: an army of a country at war with four regiments exists");
			if (raider != nullptr) {
				CountryInstance& attacker = raider->get_country();
				const SupplyFriends raider_friends = SupplyFriends::build(attacker, all_countries, im.get_war_manager());
				ProvinceInstance* target = nullptr;
				for (ProvinceInstance& province : im.get_map_instance().get_province_instances()) {
					if (!province.province_definition.is_water() && province.get_owner() != nullptr && province.get_owner() == province.get_controller()
						&& attacker.is_at_war_with(*province.get_owner()) && province.get_armies().empty()
						&& calculate_supply_territory(province, im.get_map_instance(), attacker, raider_friends, defines).kind
							== SupplyTerritory::kind_t::FAR) {
						target = &province;
						break;
					}
				}
				r.check(target != nullptr, "supply on enemy land: an enemy province far from the land of the attacker exists");
				if (target != nullptr) {
					const fixed_point_t capacity = attacker.get_supply_reserve_capacity();
					raider->set_location(*target);
					raider->set_supply_level(fixed_point_t::_1);
					for (RegimentInstance& regiment : raider->get_regiment_instances()) {
						regiment.set_supply_reserve(capacity);
					}
					raider->set_supply_priority_custom(true);
					raider->set_supply_priority(supply_priority_t::STANDARD);
					play(im, 1);
					const fixed_point_t first_delivery = raider->get_supply_delivery();
					r.check(first_delivery < fixed_point_t::_1 && first_delivery > fixed_point_t::_0,
						"supply on enemy land: the limit of the enemy province lets through only a part of the need",
						fmt::format("{}", first_delivery));
					const fixed_point_t reserve_after_one = raider->get_supply_reserve_average();
					r.check(reserve_after_one < capacity, "supply on enemy land: the reserve is used on the first day",
						fmt::format("{} of {}", reserve_after_one, capacity));
					r.check(raider->get_supply_level() == fixed_point_t::_1, "supply on enemy land: the supply level does not fall while there is a reserve");
					// The army must stay where it is
					int days = 1;
					while (days < 30 && raider->get_supply_reserve_average() > fixed_point_t::_0) {
						raider->set_location(*target);
						play(im, 1);
						days++;
					}
					r.check(raider->get_supply_reserve_average() == fixed_point_t::_0, "supply on enemy land: the reserve is used up", fmt::format("{} days", days));
					const fixed_point_t shortfall = fixed_point_t::_1 - raider->get_supply_delivery();
					const int expected = shortfall > fixed_point_t::_0 ? static_cast<int>((capacity / shortfall).round<int32_t>()) : 0;
					r.check(days >= expected - 1 && days <= expected + 2, "supply on enemy land: the reserve lasts its days divided by the shortfall",
						fmt::format("{} days, expected about {}", days, expected));
					for (int i = 0; i < 10; ++i) {
						raider->set_location(*target);
						play(im, 1);
					}
					r.check(raider->get_supply_level() < fixed_point_t::_1, "supply on enemy land: the supply level falls when the reserve is used up",
						fmt::format("{}", raider->get_supply_level()));
					// The maximum priority sends more and makes up for the losses
					const fixed_point_t standard_ratio = raider->get_supply_ratio();
					raider->set_supply_priority(supply_priority_t::MAXIMUM);
					raider->set_location(*target);
					play(im, 1);
					// (an army this far on the land of the enemy is cut off from the supply, P8: the priority sends more, nothing arrives)
					r.check(raider->get_supply_send_multiplier() > fixed_point_t::_1
							&& (raider->is_cut_off() ? raider->get_supply_ratio() == fixed_point_t::_0 : raider->get_supply_ratio() > standard_ratio),
						"supply on enemy land: the maximum priority sends more (what arrives is more, nothing when the army is cut off)",
						fmt::format("{} -> {} (sent x{}, cut off {})", standard_ratio, raider->get_supply_ratio(), raider->get_supply_send_multiplier(), raider->is_cut_off()));
					r.check(raider->get_supply_ratio() <= fixed_point_t::_1, "supply on enemy land: what arrives never exceeds the need");
					(void)open_defines;
				}
			}
		}
	}

	// The kind (index) of a regiment type by its identifier, SIZE_MAX if the game has none
	std::size_t regiment_kind(InstanceManager& im, std::string_view identifier) {
		RegimentType const* const regiment_type =
			im.definition_manager.get_military_manager().get_unit_type_manager().get_regiment_type_by_identifier(identifier);
		return regiment_type != nullptr ? get_division_slot_kind(*regiment_type) : SIZE_MAX;
	}

	// A grid of the cells filled one after another (two rows to a rank): first_count of the first kind, then second_count of the second
	DivisionGrid make_test_grid(const std::size_t first_kind, const std::size_t first_count, const std::size_t second_kind, const std::size_t second_count) {
		DivisionGrid grid {};
		std::size_t cell = 0;
		for (std::size_t i = 0; i < first_count + second_count; ++i, ++cell) {
			const std::size_t kind = i < first_count ? first_kind : second_kind;
			grid[DivisionTemplate::cell_index(cell / 2, cell % 2)] = static_cast<std::uint8_t>(kind + 1);
		}
		return grid;
	}

	// Divisions and templates (docs/PLAN_ARMY_SUPPLY_FRONTS.md, section 4; docs/WAR_ASSUMPTIONS.md, section 6): every country has armies of divisions
	// of the default template (the regiments of the history are laid out by it: whole divisions, then what is left), generals only with
	// mechanic:command, templates of its own only with mechanic:division_templates; templates and reforming
	void test_divisions(Results& r, InstanceManager& im) {
		UnitTypeManager const& unit_types = im.definition_manager.get_military_manager().get_unit_type_manager();
		std::size_t armies = 0, plain_armies = 0, regiments_without_division = 0, bad_division = 0, out_of_template_at_start = 0, plain_leaders = 0;
		std::size_t without_default = 0, plain_with_more_templates = 0, too_few_whole = 0, not_laid_out = 0;
		for (CountryInstance const& country : im.get_country_instance_manager().get_country_instances()) {
			if (!country.exists() || !country.has_armies()) {
				continue;
			}
			without_default += country.get_division_templates().empty() || country.get_division_templates().front().cells != make_standard_division_grid(unit_types);
			plain_with_more_templates += !country.has_unlock(UNLOCK_MECHANIC_DIVISION_TEMPLATES) && country.get_division_templates().size() != 1;
			for (ArmyInstance const& army : country.get_armies()) {
				armies++;
				if (!country.has_army_command()) {
					plain_armies++;
					plain_leaders += army.get_leader() != nullptr;
				}
				for (DivisionInstance const& division : army.get_divisions()) {
					out_of_template_at_start += division.template_id == 0;
				}
				for (RegimentInstance const& regiment : army.get_regiment_instances()) {
					bool found = false;
					for (DivisionInstance const& division : army.get_divisions()) {
						found |= division.id == regiment.get_division_id();
					}
					regiments_without_division += regiment.get_division_id() == 0;
					bad_division += !found;
				}
				// Whole divisions: at least as many as the infantry and the cavalry of the army make by the template (4 + 2 of the categories)
				std::size_t infantry = 0, cavalry = 0, whole = 0;
				for (RegimentInstance const& regiment : army.get_regiment_instances()) {
					infantry += regiment.get_regiment_type().unit_category == UnitType::unit_category_t::INFANTRY;
					cavalry += regiment.get_regiment_type().unit_category == UnitType::unit_category_t::CAVALRY;
				}
				for (DivisionInstance const& division : army.get_divisions()) {
					const DivisionLayout layout = layout_division(unit_types, country, army, division, 2);
					std::size_t in_cells = 0;
					for (DivisionLayout::Entry const& entry : layout.entries) {
						in_cells += entry.row < 2;
					}
					whole += in_cells >= 6;
				}
				if (country.get_division_templates().size() == 1 && country.get_division_templates().front().get_total_slots() == 6) {
					too_few_whole += whole < std::min(infantry / 4, cavalry / 2);
				}
				not_laid_out += army.get_divisions().empty() && army.get_unit_count() > 0;
			}
		}
		r.check(armies > 50 && plain_armies > 0, "divisions: there are armies of countries with and without mechanic:command", fmt::format("{} {}", armies, plain_armies));
		r.check(regiments_without_division == 0 && bad_division == 0 && not_laid_out == 0, "divisions: every regiment of every country is in a division of its army",
			fmt::format("{} {} {}", regiments_without_division, bad_division, not_laid_out));
		r.check(out_of_template_at_start == 0, "divisions: the regiments of the history are laid out by the default template (no division out of it)",
			fmt::format("{}", out_of_template_at_start));
		r.check(too_few_whole == 0, "divisions: the regiments of the history make the whole divisions they can", fmt::format("{}", too_few_whole));
		r.check(without_default == 0, "divisions: every country has the default template of ov_data/common/ov_units/default_template.txt");
		r.check(plain_with_more_templates == 0, "divisions: a country without mechanic:division_templates has the default template only");
		r.check(plain_leaders == 0, "divisions: the armies of a country without mechanic:command have no general");

		// The standard template: four regiments of infantry (2 x 2) and two of cavalry (1 x 2)
		const std::size_t kind_infantry = regiment_kind(im, "line_infantry");
		const std::size_t kind_named_cavalry = regiment_kind(im, "cavalry");
		// The cavalry of the tests: the cavalry type that most regiments of the civilised countries are of
		std::size_t kind_cavalry = kind_named_cavalry;
		{
			std::map<std::size_t, std::size_t> cavalry_regiments;
			for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
				if (candidate.exists() && candidate.has_army_command()) {
					for (ArmyInstance& candidate_army : candidate.get_armies()) {
						for (RegimentInstance const& regiment : candidate_army.get_regiment_instances()) {
							if (regiment.get_regiment_type().unit_category == UnitType::unit_category_t::CAVALRY) {
								cavalry_regiments[get_division_slot_kind(regiment.get_regiment_type())]++;
							}
						}
					}
				}
			}
			std::size_t best = 0;
			for (auto const& [kind, count] : cavalry_regiments) {
				if (count > best) {
					best = count;
					kind_cavalry = kind;
				}
			}
		}
		CountryInstance* country = nullptr;
		ArmyInstance* army = nullptr;
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (!candidate.exists() || !candidate.has_unlock(UNLOCK_MECHANIC_DIVISION_TEMPLATES) || candidate.is_at_war()) {
				continue;
			}
			for (ArmyInstance& candidate_army : candidate.get_armies()) {
				std::size_t infantry = 0, cavalry = 0;
				for (RegimentInstance const& regiment : candidate_army.get_regiment_instances()) {
					infantry += get_division_slot_kind(regiment.get_regiment_type()) == kind_infantry;
					cavalry += get_division_slot_kind(regiment.get_regiment_type()) == kind_cavalry;
				}
				if (infantry >= 2 && cavalry >= 2 && !candidate_army.is_in_combat()) {
					country = &candidate;
					army = &candidate_army;
					break;
				}
			}
			if (army != nullptr) {
				break;
			}
		}
		r.check(army != nullptr, "divisions: an army with two regiments of infantry and two of cavalry exists");
		if (army == nullptr) {
			return;
		}
		DivisionTemplate const* const standard = country->find_division_template(1);
		r.check(standard != nullptr && kind_infantry < DIVISION_MAX_TYPES && kind_named_cavalry < DIVISION_MAX_TYPES
				&& standard->counts[kind_infantry] == 4 && standard->counts[kind_named_cavalry] == 2 && standard->get_total_slots() == 6,
			"divisions: the standard template has four regiments of infantry (2 x 2) and two of cavalry (1 x 2)");
		DivisionGrid expected_standard {};
		for (std::size_t row = 0; row < 2; ++row) {
			expected_standard[DivisionTemplate::cell_index(0, row)] = static_cast<std::uint8_t>(kind_infantry + 1);
			expected_standard[DivisionTemplate::cell_index(1, row)] = static_cast<std::uint8_t>(kind_infantry + 1);
			expected_standard[DivisionTemplate::cell_index(2, row)] = static_cast<std::uint8_t>(kind_named_cavalry + 1);
		}
		const bool standard_shape = standard != nullptr && standard->cells == expected_standard;
		r.check(standard_shape, "divisions: the standard template is 2 ranks of 2 rows of infantry and 1 rank of 2 rows of cavalry");

		const auto count_templated = [&army](const std::uint32_t template_id) -> std::size_t {
			std::size_t count = 0;
			for (DivisionInstance const& division : army->get_divisions()) {
				count += division.template_id == template_id;
			}
			return count;
		};
		const auto count_in_templates = [&army](const std::size_t kind) -> std::size_t {
			std::size_t count = 0;
			for (RegimentInstance const& regiment : army->get_regiment_instances()) {
				for (DivisionInstance const& division : army->get_divisions()) {
					if (division.id == regiment.get_division_id() && division.template_id != 0
						&& get_division_slot_kind(regiment.get_regiment_type()) == kind) {
						count++;
					}
				}
			}
			return count;
		};

		// The regiments of the history are all in divisions of the standard template: reforming has nothing to do
		const std::size_t standard_divisions = count_templated(1);
		const std::size_t standard_regiments = army->get_unit_count();
		im.queue_game_action<reform_army_to_template_argument_t>(army->unique_id, std::uint32_t(0), std::uint32_t(1));
		im.force_tick_and_update();
		r.check(standard_divisions > 0 && count_templated(1) == standard_divisions && army->get_unit_count() == standard_regiments,
			"divisions: the army is in divisions of the standard template from the start (reforming has no regiments out of the template)",
			fmt::format("{} {}", count_templated(1), standard_divisions));
		// The template is deleted: its divisions stay, out of the template (the regiments are free for the next template)
		im.queue_game_action<delete_division_template_argument_t>(country->index, std::uint32_t(1));
		im.force_tick_and_update();
		r.check(country->find_division_template(1) == nullptr && count_templated(1) == 0 && army->get_unit_count() == standard_regiments,
			"divisions: deleting a template leaves its divisions out of the template");

		// A new template: one regiment of infantry and one of cavalry
		const std::size_t templates_before = country->get_division_templates().size();
		im.queue_game_action<create_division_template_argument_t>(country->index, make_test_grid(kind_infantry, 1, kind_cavalry, 1));
		im.force_tick_and_update();
		r.check(country->get_division_templates().size() == templates_before + 1, "divisions: a template can be made");
		const std::uint32_t pair_id = country->get_division_templates().back().id;
		im.queue_game_action<create_division_template_argument_t>(country->index, DivisionGrid {});
		im.force_tick_and_update();
		r.check(country->get_division_templates().size() == templates_before + 1, "divisions: a template without cells is refused");
		// More ranks or rows than the country may use: refused
		DivisionGrid too_wide {};
		too_wide[DivisionTemplate::cell_index(5, 0)] = static_cast<std::uint8_t>(kind_infantry + 1);
		im.queue_game_action<create_division_template_argument_t>(country->index, too_wide);
		DivisionGrid too_deep {};
		too_deep[DivisionTemplate::cell_index(0, 3)] = static_cast<std::uint8_t>(kind_infantry + 1);
		im.queue_game_action<create_division_template_argument_t>(country->index, too_deep);
		DivisionGrid not_a_type {};
		not_a_type[DivisionTemplate::cell_index(0, 0)] = static_cast<std::uint8_t>(DIVISION_MAX_TYPES);
		im.queue_game_action<create_division_template_argument_t>(country->index, not_a_type);
		im.force_tick_and_update();
		r.check(country->get_division_templates().size() == templates_before + 1,
			"divisions: a template with more ranks or rows than the limits give, or with a cell that is no regiment type, is refused");

		std::size_t infantry = 0, cavalry = 0;
		for (RegimentInstance const& regiment : army->get_regiment_instances()) {
			infantry += get_division_slot_kind(regiment.get_regiment_type()) == kind_infantry;
			cavalry += get_division_slot_kind(regiment.get_regiment_type()) == kind_cavalry;
		}
		const std::size_t expected_divisions = std::min(infantry, cavalry);
		const std::size_t regiments = army->get_unit_count();
		im.queue_game_action<reform_army_to_template_argument_t>(army->unique_id, std::uint32_t(0), pair_id);
		im.force_tick_and_update();
		r.check(count_templated(pair_id) == expected_divisions && expected_divisions >= 2, "divisions: the regiments make as many whole divisions as they can",
			fmt::format("{} of {}", count_templated(pair_id), expected_divisions));
		r.check(count_in_templates(kind_infantry) == expected_divisions && count_in_templates(kind_cavalry) == expected_divisions,
			"divisions: every division of the template has one regiment of infantry and one of cavalry");
		r.check(army->get_unit_count() == regiments, "divisions: reforming does not lose regiments");

		// Changing the template changes the divisions: two regiments of infantry and none of cavalry (the cavalry goes to the reserve)
		im.queue_game_action<update_division_template_argument_t>(country->index, pair_id, make_test_grid(kind_infantry, 2, kind_cavalry, 0));
		im.force_tick_and_update();
		r.check(count_in_templates(kind_cavalry) == 0, "divisions: when the template changes the regiments that do not fit leave the divisions");
		std::size_t out_of_template = 0, in_divisions = 0;
		for (RegimentInstance const& regiment : army->get_regiment_instances()) {
			bool templated = false;
			for (DivisionInstance const& division : army->get_divisions()) {
				templated |= division.id == regiment.get_division_id() && division.template_id != 0;
			}
			out_of_template += !templated;
			in_divisions += templated;
		}
		r.check(in_divisions + out_of_template == regiments && out_of_template >= expected_divisions,
			"divisions: the regiments that left wait in the reserve of the army", fmt::format("{} {}", in_divisions, out_of_template));
		bad_division = 0;
		for (DivisionInstance const& division : army->get_divisions()) {
			if (division.template_id == pair_id) {
				std::size_t members = 0;
				for (RegimentInstance const& regiment : army->get_regiment_instances()) {
					members += regiment.get_division_id() == division.id;
				}
				bad_division += members > 2;
			}
		}
		r.check(bad_division == 0, "divisions: a division never has more regiments than its template");

		// The template is deleted: its divisions stay, out of the template
		im.queue_game_action<delete_division_template_argument_t>(country->index, pair_id);
		im.force_tick_and_update();
		r.check(country->find_division_template(pair_id) == nullptr && count_templated(pair_id) == 0 && army->get_unit_count() == regiments,
			"divisions: the divisions of a deleted template do not belong to any template");

		// Without the technology there are no templates, and generals command armies only with it
		CountryInstance* plain = nullptr;
		ArmyInstance* plain_army = nullptr;
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (candidate.exists() && !candidate.has_army_command() && !candidate.has_unlock(UNLOCK_MECHANIC_DIVISION_TEMPLATES) && candidate.has_armies()
				&& !candidate.get_leaders<unit_branch_t::LAND>().empty()) {
				plain = &candidate;
				plain_army = &candidate.get_armies().front().get();
				break;
			}
		}
		if (plain != nullptr) {
			im.queue_game_action<create_division_template_argument_t>(plain->index, make_test_grid(kind_infantry, 1, kind_infantry, 0));
			im.force_tick_and_update();
			r.check(plain->get_division_templates().size() == 1, "divisions: a country without mechanic:division_templates cannot make templates");
			LeaderInstance const& general = plain->get_leaders<unit_branch_t::LAND>().front();
			im.queue_game_action<set_army_leader_argument_t>(plain_army->unique_id, general.unique_id);
			im.force_tick_and_update();
			r.check(plain_army->get_leader() == nullptr, "divisions: a general cannot command the army of a country without mechanic:command");
		}
	}

	// Groups of armies and field marshals, the bonus of the generals to the supply (docs/PLAN_ARMY_SUPPLY_FRONTS.md, sections 1 and 4)
	// ---- the mechanics that the keys open (mechanic:division_templates, mechanic:mobilisation)

	void test_unlock_mechanics(Results& r, InstanceManager& im) {
		CountryInstance* civilised = nullptr;
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (candidate.exists() && !candidate.is_rebel_country() && candidate.is_civilised()) {
				civilised = &candidate;
			}
		}
		if (civilised == nullptr || im.definition_manager.get_military_manager().get_equipment_manager().get_equipment().empty()) {
			return;
		}
		const std::vector<memory::string> keys_before { civilised->get_unlock_keys().begin(), civilised->get_unlock_keys().end() };
		const bool mobilised_before = civilised->is_mobilised();
		civilised->revoke_unlock("mechanic:division_templates");
		civilised->revoke_unlock("mechanic:mobilisation");
		civilised->set_mobilised(false);
		const std::size_t templates_before = civilised->get_division_templates().size();
		const DivisionGrid grid = make_test_grid(regiment_kind(im, "line_infantry"), 1, regiment_kind(im, "cavalry"), 1);
		im.run_game_action_now<create_division_template_argument_t>(civilised->index, grid);
		r.check(civilised->get_division_templates().size() == templates_before, "unlock gates: without mechanic:division_templates a template cannot be created");
		civilised->grant_unlock("mechanic:division_templates");
		im.run_game_action_now<create_division_template_argument_t>(civilised->index, grid);
		r.check(civilised->get_division_templates().size() == templates_before + 1, "unlock gates: with mechanic:division_templates a template is created");
		civilised->revoke_unlock("mechanic:division_templates");
		const std::uint32_t new_id = civilised->get_division_templates().back().id;
		im.run_game_action_now<delete_division_template_argument_t>(civilised->index, new_id);
		r.check(civilised->get_division_templates().size() == templates_before + 1, "unlock gates: without the key a template cannot be deleted either (the templates are fixed)");
		civilised->grant_unlock("mechanic:division_templates");
		im.run_game_action_now<delete_division_template_argument_t>(civilised->index, new_id);
		r.check(civilised->get_division_templates().size() == templates_before, "unlock gates: with the key the template is deleted");

		im.run_game_action_now<set_mobilise_argument_t>(civilised->index, true);
		r.check(!civilised->is_mobilised(), "unlock gates: without mechanic:mobilisation a country cannot mobilise");
		civilised->grant_unlock("mechanic:mobilisation");
		im.run_game_action_now<set_mobilise_argument_t>(civilised->index, true);
		r.check(civilised->is_mobilised(), "unlock gates: with mechanic:mobilisation it can");
		civilised->revoke_unlock("mechanic:mobilisation");
		im.run_game_action_now<set_mobilise_argument_t>(civilised->index, false);
		r.check(!civilised->is_mobilised(), "unlock gates: demobilising is always possible");

		civilised->set_mobilised(mobilised_before);
		for (memory::string const& key : std::vector<memory::string> { civilised->get_unlock_keys().begin(), civilised->get_unlock_keys().end() }) {
			civilised->revoke_unlock(key);
		}
		for (memory::string const& key : keys_before) {
			civilised->grant_unlock(key);
		}
	}

	// ---- the paid upgrade of a regiment (cavalry to hussars)

	void test_regiment_upgrade(Results& r, InstanceManager& im) {
		UnitTypeManager const& unit_types = im.definition_manager.get_military_manager().get_unit_type_manager();
		RegimentType const* const cavalry = unit_types.get_regiment_type_by_identifier("cavalry");
		RegimentType const* const hussars = unit_types.get_regiment_type_by_identifier("hussars");
		if (cavalry == nullptr || hussars == nullptr || cavalry->ov.upgrades_to != "hussars" || cavalry->ov.upgrade_cost.empty()) {
			fmt::println("TEST INFO regiment upgrade: no ov_data folder, not checked");
			return;
		}
		ArmyInstance* army = nullptr;
		RegimentInstance* regiment = nullptr;
		for (CountryInstance& owner : im.get_country_instance_manager().get_country_instances()) {
			for (ArmyInstance& candidate : owner.get_armies()) {
				if (candidate.is_in_combat() || owner.is_rebel_country()) {
					continue;
				}
				for (RegimentInstance const& unit : candidate.get_regiment_instances()) {
					if (&unit.get_regiment_type() == cavalry) {
						army = &candidate;
						regiment = const_cast<RegimentInstance*>(&unit);
						break;
					}
				}
				if (regiment != nullptr) {
					break;
				}
			}
			if (regiment != nullptr) {
				break;
			}
		}
		if (regiment == nullptr) {
			fmt::println("TEST INFO regiment upgrade: no cavalry regiment, not checked");
			return;
		}
		CountryInstance& country = army->get_country();
		const unique_id_t id = regiment->unique_id;
		const std::uint32_t division_id = regiment->get_division_id();
		Pop* const pop = regiment->get_pop();
		const std::size_t units_before = army->get_unit_count();
		const std::vector<memory::string> keys_before { country.get_unlock_keys().begin(), country.get_unlock_keys().end() };
		std::vector<std::pair<GoodDefinition const*, fixed_point_t>> stock_before;
		for (auto const& [good, quantity] : cavalry->ov.upgrade_cost) {
			stock_before.emplace_back(good, country.get_good_data(*good).stockpile_amount);
			country.get_good_data(*good).stockpile_amount = quantity * 3;
		}

		country.revoke_unlock("unit:hussars");
		r.check(!im.run_game_action_now<upgrade_regiment_argument_t>(country.index, id) && &regiment->get_regiment_type() == cavalry, "regiment upgrade: without the key of the hussars the upgrade is refused");
		country.grant_unlock("unit:hussars");
		for (auto const& [good, quantity] : cavalry->ov.upgrade_cost) {
			country.get_good_data(*good).stockpile_amount = quantity / 2;
		}
		r.check(!im.run_game_action_now<upgrade_regiment_argument_t>(country.index, id) && &regiment->get_regiment_type() == cavalry, "regiment upgrade: without the goods in the stockpile the upgrade is refused");
		for (auto const& [good, quantity] : cavalry->ov.upgrade_cost) {
			country.get_good_data(*good).stockpile_amount = quantity * 3;
		}
		r.check(!im.run_game_action_now<upgrade_regiment_argument_t>(country_index_t(country.index == country_index_t(0) ? 1 : 0), id), "regiment upgrade: a regiment of another country cannot be upgraded");
		const bool upgraded = im.run_game_action_now<upgrade_regiment_argument_t>(country.index, id);
		RegimentInstance* const after = static_cast<RegimentInstance*>(im.get_unit_instance_manager().get_unit_instance_by_unique_id(id));
		r.check(
			upgraded && after != nullptr && &after->get_regiment_type() == hussars && after->get_division_id() == division_id && after->get_pop() == pop && army->get_unit_count() == units_before,
			"regiment upgrade: a paid upgrade makes the cavalry regiment hussars with the same id, pop and division"
		);
		bool paid = true;
		for (auto const& [good, quantity] : cavalry->ov.upgrade_cost) {
			paid &= country.get_good_data(*good).stockpile_amount == quantity * 2;
		}
		r.check(paid, "regiment upgrade: the price of the file is taken from the national stockpile");
		r.check(!im.run_game_action_now<upgrade_regiment_argument_t>(country.index, id), "regiment upgrade: hussars cannot be upgraded (no upgrades_to)");

		for (auto const& [good, quantity] : stock_before) {
			country.get_good_data(*good).stockpile_amount = quantity;
		}
		for (memory::string const& key : std::vector<memory::string> { country.get_unlock_keys().begin(), country.get_unlock_keys().end() }) {
			country.revoke_unlock(key);
		}
		for (memory::string const& key : keys_before) {
			country.grant_unlock(key);
		}
	}

	// ---- the support companies (military/Companies.*, ov_data/common/ov_companies)

	void test_companies(Results& r, InstanceManager& im) {
		namespace fs = std::filesystem;
		CompanyTypeManager const& companies = im.definition_manager.get_military_manager().get_company_type_manager();
		CompanyType const* const telegraph = companies.find("telegraph_company");
		CompanyType const* const radio = companies.find("radio_company");
		CompanyType const* const engineer = companies.find("engineer_company");
		if (telegraph == nullptr || radio == nullptr || engineer == nullptr) {
			fmt::println("TEST INFO companies: no ov_data folder, not checked");
			return;
		}
		r.check(
			telegraph->category == "signals" && radio->category == "signals" && engineer->category == "engineering" && radio->fallback_index == telegraph->index
				&& telegraph->unlock_key == "support_company:telegraph" && !telegraph->goods.empty() && !radio->goods.empty() && engineer->goods.size() == 2,
			"companies: the telegraph, radio and engineer companies are loaded from files (signals, signals with a fallback on the telegraph, engineering)"
		);

		// A new company comes with one file, a broken one is skipped
		{
			const fs::path temp = fs::temp_directory_path() / "openvic_selftest_companies";
			std::error_code ec;
			fs::remove_all(temp, ec);
			write_test_file(temp / "good.txt",
				"test_company = { category = machinegun goods = { ammunition = 0.2 } weight = 100 manpower = 0.1 buffs = { attack_mult = { infantry = 0.2 } fortification = 1 } }\n");
			write_test_file(temp / "bad.txt",
				"no_category = { goods = { ammunition = 0.2 } }\n"
				"bad_good = { category = x goods = { no_such_good = 1 } }\n"
				"bad_key = { category = x unlock_key = \"wrong\" }\n"
				"bad_buff = { category = x buffs = { luck = 1 } }\n"
				"test_company = { category = y }\n");
			CompanyTypeManager fresh;
			GoodDefinitionManager const& goods = im.definition_manager.get_economy_manager().get_good_definition_manager();
			const ovdl::v2script::Parser good = Dataloader::parse_defines(temp / "good.txt");
			const ovdl::v2script::Parser bad = Dataloader::parse_defines(temp / "bad.txt");
			const std::size_t good_count = fresh.load_file("good.txt", good.get_file_node(), goods);
			const std::size_t bad_count = fresh.load_file("bad.txt", bad.get_file_node(), goods);
			CompanyType const* const test_company = fresh.find("test_company");
			r.check(
				good_count == 1 && test_company != nullptr && test_company->category == "machinegun" && test_company->buffs.size() == 2 && test_company->buffs[1].scope == "all"
					&& fresh.get_categories().size() >= 1,
				"companies: a new company and a new category come with one file, no code is needed (a buff without a scope is for all)"
			);
			r.check(bad_count <= 1 && fresh.find("no_category") == nullptr && fresh.find("bad_good") == nullptr && fresh.find("bad_key") == nullptr
					&& fresh.find("test_company")->category == "machinegun",
				"companies: a company without a category, with an unknown good or a wrong key is skipped, so is a repeated id", fmt::format("{}", bad_count));
			fs::remove_all(temp, ec);
		}

		// A country with armies and a template (the regiments are put into the divisions of the template below)
		CountryInstance* country = nullptr;
		DivisionTemplate* division_template = nullptr;
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (!candidate.exists() || candidate.is_rebel_country() || !candidate.has_army_command()) {
				continue;
			}
			// an army with three regiments of line infantry (a template of three ranks of one line infantry regiment is made for the test)
			for (ArmyInstance const& army : candidate.get_armies()) {
				std::size_t line_infantry = 0;
				for (RegimentInstance const& regiment : army.get_regiment_instances()) {
					line_infantry += regiment.get_regiment_type().get_identifier() == "line_infantry";
				}
				if (line_infantry >= 3 && country == nullptr) {
					country = &candidate;
				}
			}
			if (country != nullptr) {
				break;
			}
		}
		if (country == nullptr) {
			fmt::println("TEST INFO companies: no country with an army and a template, not checked");
			return;
		}
		const std::vector<memory::string> keys_before { country->get_unlock_keys().begin(), country->get_unlock_keys().end() };
		const std::vector<CompanyState> states_before { country->get_company_states().begin(), country->get_company_states().end() };
		country->grant_unlock("mechanic:division_templates");
		DivisionGrid test_grid {};
		const std::size_t line_infantry_kind = regiment_kind(im, "line_infantry");
		for (std::size_t rank = 0; rank < 3; ++rank) {
			test_grid[DivisionTemplate::cell_index(rank, 0)] = static_cast<std::uint8_t>(line_infantry_kind + 1);
		}
		if (!im.run_game_action_now<create_division_template_argument_t>(country->index, test_grid)) {
			fmt::println("TEST INFO companies: the template for the test cannot be made, not checked");
			return;
		}
		division_template = &country->get_mutable_division_templates().back();
		const DivisionCompanies companies_before = division_template->companies;
		const std::uint32_t width = get_template_width(*division_template);
		const std::uint32_t template_id = division_template->id;
		for (std::string_view const key : { "support_company:telegraph", "support_company:radio", "support_company:engineer" }) {
			country->revoke_unlock(key);
		}
		country->grant_unlock("mechanic:division_templates");
		// the first three ranks that have a regiment
		std::vector<std::size_t> ranks;
		for (std::size_t rank = 0; rank < DIVISION_MAX_RANKS && ranks.size() < 3; ++rank) {
			for (std::size_t row = 0; row < DIVISION_MAX_DEPTH; ++row) {
				if (division_template->cells[DivisionTemplate::cell_index(rank, row)] != 0) {
					ranks.push_back(rank);
					break;
				}
			}
		}
		if (width != 3 || ranks.size() != 3) {
			fmt::println("TEST INFO companies: the standard template does not have 3 ranks, the scale is not checked");
			return;
		}
		const auto slot = [&](DivisionCompanies& layout, const std::size_t rank_number, const std::size_t slot_number, CompanyType const* company) {
			layout[ranks[rank_number] * DIVISION_MAX_COMPANY_SLOTS + slot_number] = company == nullptr ? 0 : static_cast<std::uint8_t>(company->index + 1);
		};
		DivisionCompanies layout {};
		slot(layout, 0, 0, radio);
		slot(layout, 1, 0, radio);
		slot(layout, 2, 0, telegraph);

		// The keys
		r.check(!is_valid_company_layout(im, *country, division_template->cells, layout), "companies: without the keys of the companies a layout is not allowed");
		country->grant_unlock("support_company:telegraph");
		country->grant_unlock("support_company:radio");
		country->grant_unlock("support_company:engineer");
		country->revoke_unlock("mechanic:division_templates");
		r.check(!im.run_game_action_now<set_division_template_companies_argument_t>(country->index, template_id, layout), "companies: without mechanic:division_templates the companies cannot be changed");
		country->grant_unlock("mechanic:division_templates");
		const DivisionTemplateLimits limits = get_template_limits(im, *country);
		r.check(is_valid_company_layout(im, *country, division_template->cells, layout) && limits.company_slots >= 1, "companies: with the keys the layout of three signal companies in three ranks is allowed");

		// A rank holds one company of a category; a slot over the limit and a rank without regiments are refused
		DivisionCompanies two_signals {};
		slot(two_signals, 0, 0, radio);
		slot(two_signals, 0, 1, telegraph);
		r.check(!is_valid_company_layout(im, *country, division_template->cells, two_signals), "companies: two companies of the same category (signals) cannot be in one rank");
		DivisionCompanies mixed {};
		slot(mixed, 0, 0, radio);
		slot(mixed, 0, 1, engineer);
		r.check(limits.company_slots < 2 || is_valid_company_layout(im, *country, division_template->cells, mixed), "companies: companies of different categories fit in one rank (when the country has two slots)");
		DivisionCompanies over_limit {};
		over_limit[ranks[0] * DIVISION_MAX_COMPANY_SLOTS + limits.company_slots] = static_cast<std::uint8_t>(telegraph->index + 1);
		r.check(!is_valid_company_layout(im, *country, division_template->cells, over_limit), "companies: a slot over the limit of the country is refused");
		DivisionCompanies empty_rank {};
		empty_rank[(DIVISION_MAX_RANKS - 1) * DIVISION_MAX_COMPANY_SLOTS] = static_cast<std::uint8_t>(telegraph->index + 1);
		r.check(!is_valid_company_layout(im, *country, division_template->cells, empty_rank), "companies: a rank without regiments has no company slots");

		r.check(im.run_game_action_now<set_division_template_companies_argument_t>(country->index, template_id, layout) && division_template->companies == layout,
			"companies: the command sets the companies of the template");
		const memory::vector<std::uint32_t> counts = count_template_companies(companies, *division_template);
		r.check(counts[radio->index] == 2 && counts[telegraph->index] == 1 && counts[engineer->index] == 0, "companies: the template has 2 radio companies and 1 telegraph company");

		// The scale: 2 radio + 1 telegraph in 3 ranks = 2/3 of the radio buff and 1/3 of the telegraph one
		memory::vector<CompanyState>& states = country->get_company_states_mutable();
		states.assign(companies.size(), CompanyState {});
		for (std::size_t index = 0; index < companies.size(); ++index) {
			states[index] = { index, fixed_point_t::_1 };
		}
		DivisionStats stats = compute_division_stats(im, *country, *division_template);
		const fixed_point_t expected = fixed_point_t(2) / 3 * (fixed_point_t(1) / 10) + fixed_point_t(1) / 3 * (fixed_point_t(5) / 100);
		r.check(stats.companies == 3 && near(stats.company_attack, expected, fixed_point_t(1) / 1000) && near(stats.company_defence, expected, fixed_point_t(1) / 1000)
				&& near(stats.company_breakthrough, expected, fixed_point_t(1) / 1000),
			"companies: 2 radio + 1 telegraph in 3 ranks give 2/3 x 10 % + 1/3 x 5 % to the attack, the defence and the breakthrough", fmt::format("{}", dbl(stats.company_attack)));
		DivisionTemplate without = *division_template;
		without.companies = DivisionCompanies {};
		const DivisionStats plain = compute_division_stats(im, *country, without);
		r.check(stats.attack > plain.attack && near(stats.attack / plain.attack, fixed_point_t::_1 + expected, fixed_point_t(1) / 100),
			"companies: the buff is in the attack of the template", fmt::format("{} / {}", dbl(stats.attack), dbl(plain.attack)));

		// Without radio in the stockpile the radio companies work as telegraph companies: 3 x 5 %
		states[radio->index] = { telegraph->index, fixed_point_t::_1 };
		stats = compute_division_stats(im, *country, *division_template);
		r.check(near(stats.company_attack, fixed_point_t(5) / 100, fixed_point_t(1) / 1000), "companies: radio companies without radio work as telegraph companies (3 of 3 ranks x 5 %)", fmt::format("{}", dbl(stats.company_attack)));
		// The buff is scaled by the share of the goods
		states[radio->index] = { radio->index, fixed_point_t(1) / 2 };
		states[telegraph->index] = { telegraph->index, fixed_point_t::_1 };
		stats = compute_division_stats(im, *country, *division_template);
		r.check(near(stats.company_attack, fixed_point_t(2) / 3 * (fixed_point_t(1) / 20) + fixed_point_t(1) / 3 * (fixed_point_t(5) / 100), fixed_point_t(1) / 1000),
			"companies: half of the goods of the radio gives half of its buff", fmt::format("{}", dbl(stats.company_attack)));
		// Companies cost goods and men
		bool cost_ok = false, supply_ok = false;
		for (DivisionGoodAmount const& entry : stats.cost) {
			cost_ok |= entry.good == "radio" && entry.amount >= fixed_point_t(10) - fixed_point_t(1) / 100;
		}
		for (DivisionGoodAmount const& entry : stats.supply_per_day) {
			supply_ok |= entry.good == "telegraph" && entry.amount >= fixed_point_t(1) / 10 - fixed_point_t(1) / 1000;
		}
		r.check(cost_ok && supply_ok && stats.men > plain.men && stats.weight > plain.weight, "companies: they are in the cost, the goods of a day, the men and the weight of the template");

		// The daily use of the goods: the companies of the divisions of the template use their goods from the national stockpile
		GoodDefinition const* const telegraph_good = telegraph->goods.front().first;
		GoodDefinition const* const radio_good = radio->goods.front().first;
		std::size_t division_count = 0;
		for (ArmyInstance const& army : country->get_armies()) {
			for (DivisionInstance const& division : army.get_divisions()) {
				division_count += division.template_id == template_id;
			}
		}
		if (division_count == 0) {
			// The regiments of the history are in divisions of the default template: they go to the reserve of their army first
			for (ArmyInstance& army : country->get_armies()) {
				if (army.is_in_combat()) {
					continue;
				}
				const std::uint32_t reserve_id = im.get_unit_instance_manager().next_division_id++;
				army.get_mutable_divisions().clear();
				army.get_mutable_divisions().push_back({ reserve_id, 0 });
				for (RegimentInstance& regiment : army.get_regiment_instances()) {
					regiment.set_division_id(reserve_id);
				}
				reform_army_to_template(im, army, 0, template_id);
			}
			for (ArmyInstance const& army : country->get_armies()) {
				for (DivisionInstance const& division : army.get_divisions()) {
					division_count += division.template_id == template_id;
				}
			}
		}
		const fixed_point_t telegraph_before = country->get_good_data(*telegraph_good).stockpile_amount;
		const fixed_point_t radio_before = country->get_good_data(*radio_good).stockpile_amount;
		country->get_good_data(*telegraph_good).stockpile_amount = fixed_point_t(1000);
		country->get_good_data(*radio_good).stockpile_amount = fixed_point_t::_0;
		im.force_tick_and_update();
		const CompanyState radio_state = country->get_company_states()[radio->index];
		r.check(radio_state.works_as == telegraph->index && radio_state.supplied > fixed_point_t(1) / 2,
			"companies: without radio in the stockpile the radio companies fall back to the telegraph and are supplied by it");
		const fixed_point_t used = fixed_point_t(1000) - country->get_good_data(*telegraph_good).stockpile_amount;
		r.check(division_count > 0 && used >= fixed_point_t(3) / 10 * static_cast<std::int32_t>(division_count) / 2,
			"companies: the telegraph companies of the divisions use their goods from the national stockpile in a day", fmt::format("used {} in {} divisions", dbl(used), division_count));
		country->get_good_data(*radio_good).stockpile_amount = fixed_point_t(1000);
		im.force_tick_and_update();
		r.check(country->get_company_states()[radio->index].works_as == radio->index, "companies: with radio in the stockpile the radio companies work as radio companies");
		country->get_good_data(*telegraph_good).stockpile_amount = telegraph_before;
		country->get_good_data(*radio_good).stockpile_amount = radio_before;

		// put the country back
		division_template->companies = companies_before;
		delete_division_template(*country, template_id);
		country->get_company_states_mutable().assign(states_before.begin(), states_before.end());
		for (memory::string const& key : std::vector<memory::string> { country->get_unlock_keys().begin(), country->get_unlock_keys().end() }) {
			country->revoke_unlock(key);
		}
		for (memory::string const& key : keys_before) {
			country->grant_unlock(key);
		}
	}

	// ---- the commanders: the limit of the armies of a group and the switch of the orders

	void test_group_limit(Results& r, InstanceManager& im) {
		OpenVicWarDefines& war = const_cast<OpenVicWarDefines&>(im.definition_manager.get_define_manager().get_openvic_defines().war);
		CountryInstance* country = nullptr;
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (candidate.exists() && !candidate.is_rebel_country() && candidate.has_army_command() && candidate.get_armies().size() >= 2
				&& candidate.get_army_groups().size() < MAX_ARMY_GROUPS) {
				country = &candidate;
				break;
			}
		}
		r.check(war.marshal_max_attached == 12 && war.marshal_max_attached_limit == 48, "commanders: a field marshal holds 12 armies by default, 48 at most with the technologies");
		if (country == nullptr) {
			fmt::println("TEST INFO commanders: no country with two armies, not checked");
			return;
		}
		const std::int32_t max_before = war.marshal_max_attached;
		war.marshal_max_attached = 1;
		create_army_group(*country, im.definition_manager.get_military_manager().get_command_palette().size());
		const std::uint32_t group_id = country->get_army_groups().back().id;
		ArmyInstance& first = country->get_armies()[0].get();
		ArmyInstance& second = country->get_armies()[1].get();
		const std::uint32_t first_before = first.get_army_group_id();
		const std::uint32_t second_before = second.get_army_group_id();
		first.set_army_group_id(0);
		second.set_army_group_id(0);
		r.check(set_army_group(*country, first, group_id, 1) && !set_army_group(*country, second, group_id, 1), "commanders: a group holds no more armies than the limit");
		r.check(set_army_group_orders(*country, group_id, false) && !country->get_army_groups().back().execute_orders && !set_army_group_orders(*country, group_id, false)
				&& set_army_group_orders(*country, group_id, true) && country->get_army_groups().back().execute_orders,
			"commanders: the orders of a group are switched on and off");
		first.set_army_group_id(first_before);
		second.set_army_group_id(second_before);
		delete_army_group(im, *country, group_id);
		war.marshal_max_attached = max_before;
	}

	// ---- the command of a field marshal (B1): the palette, the colours of the groups, the limit of the generals

	void test_command_colours(Results& r, InstanceManager& im) {
		MilitaryManager const& military = im.definition_manager.get_military_manager();
		CommandPalette const& palette = military.get_command_palette();
		r.check(palette.size() >= 12, "command: the palette of the field marshals has at least 12 colours", fmt::format("{}", palette.size()));
		bool distinct = true;
		for (std::size_t a = 0; a < palette.size(); ++a) {
			for (std::size_t b = a + 1; b < palette.size(); ++b) {
				distinct &= palette.get_colours()[a] != palette.get_colours()[b];
			}
		}
		r.check(distinct, "command: the colours of the palette are all different");

		// A palette file: the colours are read, a broken one gives what could be read
		{
			CommandPalette loaded;
			r.check(loaded.size() == 0, "command: a palette is empty before it is loaded");
			loaded.use_builtin_colours();
			r.check(loaded.size() == 12, "command: the built-in palette has 12 colours");
		}

		CountryInstance* country = nullptr;
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (candidate.exists() && !candidate.is_rebel_country() && candidate.has_army_command() && candidate.get_army_groups().empty()) {
				country = &candidate;
				break;
			}
		}
		if (country == nullptr) {
			fmt::println("TEST INFO command: no country without groups, the colours of the groups are not checked");
		} else {
			const std::size_t size = palette.size();
			r.check(create_army_group(*country, size) && create_army_group(*country, size) && create_army_group(*country, size), "command: a country creates three groups");
			auto const groups = country->get_army_groups();
			r.check(groups[0].colour_index == 0 && groups[1].colour_index == 1 && groups[2].colour_index == 2,
				"command: the groups take the first free colours of the palette in order (the same in every run)");
			r.check(set_army_group_colour(*country, groups[0].id, 5, size) && country->get_army_groups()[0].colour_index == 5
					&& !set_army_group_colour(*country, groups[0].id, 5, size) && !set_army_group_colour(*country, groups[0].id, static_cast<std::uint32_t>(size), size)
					&& !set_army_group_colour(*country, 9999, 1, size),
				"command: the player recolours a marshal (a colour of the palette, a known group, a change)");
			// the colour freed by the recolouring is the first free one again
			r.check(create_army_group(*country, size) && country->get_army_groups().back().colour_index == 0, "command: a new group takes the colour that was freed");
			// a colour out of the palette (another palette, an old save) is put back into it by the daily update
			country->get_mutable_army_groups().back().colour_index = static_cast<std::uint32_t>(size) + 7;
			update_army_groups(im, *country);
			r.check(country->get_army_groups().back().colour_index < size, "command: a colour out of the palette is replaced by the daily update");
			const std::vector<std::uint32_t> ids { country->get_army_groups()[0].id, country->get_army_groups()[1].id, country->get_army_groups()[2].id, country->get_army_groups()[3].id };
			for (const std::uint32_t id : ids) {
				delete_army_group(im, *country, id);
			}
			r.check(country->get_army_groups().empty(), "command: the test groups are deleted");
		}

		// The limit of the generals: the base of the defines, the modifier, never above the limit
		OpenVicWarDefines& war = const_cast<OpenVicWarDefines&>(im.definition_manager.get_define_manager().get_openvic_defines().war);
		ModifierEffectCache const& cache = im.definition_manager.get_modifier_manager().get_modifier_effect_cache();
		r.check(cache.get_attached_generals_add() != nullptr, "command: the modifier attached_generals_add exists");
		CountryInstance* any = nullptr;
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (candidate.exists() && !candidate.is_rebel_country()) {
				any = &candidate;
				break;
			}
		}
		if (any != nullptr) {
			const std::int32_t base_before = war.marshal_max_attached;
			const std::int32_t limit_before = war.marshal_max_attached_limit;
			r.check(get_marshal_max_attached(im, *any) == static_cast<std::uint32_t>(base_before), "command: without the modifier a marshal holds the base number of armies");
			war.marshal_max_attached = 60;
			war.marshal_max_attached_limit = 48;
			r.check(get_marshal_max_attached(im, *any) == 48, "command: a marshal never holds more than the limit");
			war.marshal_max_attached = 12;
			war.marshal_max_attached_limit = 10;
			r.check(get_marshal_max_attached(im, *any) == 10, "command: the limit holds the base too");
			war.marshal_max_attached = base_before;
			war.marshal_max_attached_limit = limit_before;
		}
	}

	// ---- the plan of a field marshal (docs/WAR_ASSUMPTIONS.md, section 6): the fronts he holds, the button of the execution that starts and stops it

	void test_command_plan(Results& r, InstanceManager& im) {
		WarManager& war_manager = im.get_war_manager();
		CountryInstance* country = nullptr;
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (candidate.exists() && !candidate.is_rebel_country() && candidate.has_army_command() && candidate.get_armies().size() >= 2
				&& candidate.get_army_groups().empty()) {
				country = &candidate;
				break;
			}
		}
		if (country == nullptr) {
			fmt::println("TEST INFO command plan: no country with two armies and without groups, not checked");
			return;
		}
		const std::size_t palette_size = im.definition_manager.get_military_manager().get_command_palette().size();
		r.check(create_army_group(*country, palette_size), "plan: a group is created");
		const std::uint32_t group_id = country->get_army_groups().back().id;
		const std::uint32_t front_id = war_manager.create_front(im, *country, nullptr);
		r.check(front_id != 0 && war_manager.get_front_by_id(front_id)->group_id == 0, "plan: a front made without a group belongs to nobody");
		r.check(war_manager.is_front_commanded(*war_manager.get_front_by_id(front_id)), "plan: a front without a group always gets orders (the automatic plan)");

		r.check(!set_army_group_plan_active(im, *country, group_id, true), "plan: the execution needs a front of the group");
		r.check(set_army_group_front(im, *country, group_id, front_id, true) && war_manager.get_front_by_id(front_id)->group_id == group_id
				&& !set_army_group_front(im, *country, group_id, front_id, true) && !set_army_group_front(im, *country, 9999, front_id, true),
			"plan: a group holds a front (once, a known group)");
		r.check(war_manager.is_front_commanded(*war_manager.get_front_by_id(front_id)), "plan: a front of a group gets orders before the execution (its divisions hold the line)");
		r.check(!set_army_group_plan_active(im, *country, group_id, true), "plan: the execution does not start without a field marshal");

		// The modes of the execution
		ArmyGroup const& group = country->get_army_groups().back();
		r.check(group.aggression == 2 && group.integrity == 1, "plan: a new group executes balanced (2) with the medium integrity (1)");
		r.check(set_army_group_modes(*country, group_id, 3, 0) && group.aggression == 3 && group.integrity == 0
				&& !set_army_group_modes(*country, group_id, 3, 0) && !set_army_group_modes(*country, group_id, 0, 1)
				&& !set_army_group_modes(*country, group_id, 4, 1) && !set_army_group_modes(*country, group_id, 2, 3),
			"plan: the modes are set (aggression 1..3, integrity 0..2, a change is needed)");

		// A field marshal
		unique_id_t leader_id = 0;
		for (LeaderInstance const& leader : country->get_leaders<unit_branch_t::LAND>()) {
			if (!leader.is_marshal() && leader.get_can_be_used()) {
				leader_id = leader.unique_id;
				break;
			}
		}
		ArmyInstance& first = country->get_armies()[0].get();
		const std::uint32_t first_group_before = first.get_army_group_id();
		if (leader_id == 0) {
			fmt::println("TEST INFO command plan: no free general, the start of the plan is not checked");
		} else {
			r.check(set_army_group_marshal(im, *country, group_id, leader_id), "plan: a marshal is set");
			first.clear_path();
			first.set_army_group_id(group_id);
			im.force_tick_and_update();
			Front const* const front = war_manager.get_front_by_id(front_id);
			r.check(front != nullptr && std::find(front->army_ids.begin(), front->army_ids.end(), first.unique_id) != front->army_ids.end()
					&& !country->get_army_groups().back().plan_active,
				"plan: an army attached to the marshal goes on his front by itself, before the execution");
			r.check(set_army_group_plan_active(im, *country, group_id, true) && country->get_army_groups().back().plan_active, "plan: the button starts the execution");
			r.check(!set_army_group_plan_active(im, *country, group_id, true), "plan: a plan that runs is not started again");
			r.check(set_army_group_plan_active(im, *country, group_id, false) && war_manager.is_front_commanded(*war_manager.get_front_by_id(front_id))
					&& !country->get_army_groups().back().plan_active,
				"plan: the button stops the execution, the front still holds its line");
			// the marshal leaves while the plan runs: the daily update stops it
			set_army_group_plan_active(im, *country, group_id, true);
			set_army_group_marshal(im, *country, group_id, 0);
			update_army_groups(im, *country);
			r.check(!country->get_army_groups().back().plan_active, "plan: a plan without a field marshal is stopped by the daily update");
		}

		// Letting go and deleting
		r.check(set_army_group_front(im, *country, group_id, front_id, false) && war_manager.get_front_by_id(front_id)->group_id == 0,
			"plan: a front that is let go is nobody's");
		set_army_group_front(im, *country, group_id, front_id, true);
		delete_army_group(im, *country, group_id);
		r.check(war_manager.get_front_by_id(front_id) == nullptr, "plan: the fronts of a group that is deleted go with it");
		first.set_army_group_id(first_group_before);
	}

	// ---- the lines of a field marshal (docs/WAR_ASSUMPTIONS.md, section 6, P2): a front along the border with a neighbour, a coastal front, the edit
	// mode, the offensive and its zone, the defence, the naval invasion, the deletion

	void test_plan_lines(Results& r, InstanceManager& im) {
		WarManager& war_manager = im.get_war_manager();
		MapInstance& map_instance = im.get_map_instance();
		// A country with mechanic:command, a land neighbour and a coast
		CountryInstance* country = nullptr;
		CountryInstance* neighbour = nullptr;
		ProvinceInstance* sea = nullptr;
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (!candidate.exists() || candidate.is_rebel_country() || !candidate.has_army_command() || !candidate.get_army_groups().empty()) {
				continue;
			}
			CountryInstance* found_neighbour = nullptr;
			ProvinceInstance* found_sea = nullptr;
			for (ProvinceInstance* province : candidate.get_owned_provinces()) {
				if (province->get_controller() != &candidate) {
					continue;
				}
				for (ProvinceDefinition::adjacency_t const& adjacency : province->province_definition.get_adjacencies()) {
					ProvinceInstance& other = map_instance.get_province_instance_by_definition(adjacency.get_to());
					if (adjacency.get_type() == ProvinceDefinition::adjacency_t::type_t::LAND && !other.province_definition.is_water()
						&& other.get_controller() != nullptr && other.get_controller() != &candidate && found_neighbour == nullptr) {
						found_neighbour = other.get_controller();
					}
					if (adjacency.get_type() == ProvinceDefinition::adjacency_t::type_t::COASTAL && other.province_definition.is_water() && found_sea == nullptr) {
						found_sea = &other;
					}
				}
			}
			if (found_neighbour != nullptr && found_sea != nullptr) {
				country = &candidate;
				neighbour = found_neighbour;
				sea = found_sea;
				break;
			}
		}
		if (country == nullptr) {
			fmt::println("TEST INFO plan lines: no country with mechanic:command, a neighbour and a coast, not checked");
			return;
		}
		const std::size_t palette_size = im.definition_manager.get_military_manager().get_command_palette().size();
		create_army_group(*country, palette_size);
		const std::uint32_t group_id = country->get_army_groups().back().id;

		// A border front
		r.check(war_manager.create_border_front(im, *country, 9999, *neighbour) == 0, "lines: a front needs a group of the country (or none)");
		r.check(war_manager.create_border_front(im, *country, group_id, *country) == 0, "lines: no front against the country itself");
		const std::uint32_t front_id = war_manager.create_border_front(im, *country, group_id, *neighbour);
		Front const* front = war_manager.get_front_by_id(front_id);
		bool edges_valid = front != nullptr && !front->edges.empty();
		if (front != nullptr) {
			for (FrontEdge const& edge : front->edges) {
				edges_valid &= edge.own->get_controller() == country && edge.other->get_controller() == neighbour
					&& std::find(front->provinces.begin(), front->provinces.end(), edge.own) != front->provinces.end();
			}
		}
		r.check(edges_valid && front->group_id == group_id && front->kind == Front::kind_t::BORDER && front->enemy == neighbour,
			"lines: a border front runs along the whole border with the neighbour (our land, his land), held by the marshal");
		std::size_t border_pieces = 0;
		for (ProvinceInstance& province : map_instance.get_province_instances()) {
			if (province.get_controller() != country || province.province_definition.is_water()) {
				continue;
			}
			for (ProvinceDefinition::adjacency_t const& adjacency : province.province_definition.get_adjacencies()) {
				ProvinceInstance const& other = map_instance.get_province_instance_by_definition(adjacency.get_to());
				border_pieces += adjacency.get_type() == ProvinceDefinition::adjacency_t::type_t::LAND && other.get_controller() == neighbour;
			}
		}
		r.check(front != nullptr && front->edges.size() == border_pieces, "lines: every piece of the border with the neighbour is on the line");

		// The edit mode
		if (front != nullptr && front->edges.size() >= 2) {
			const FrontEdge piece = front->edges.front();
			const std::size_t before = front->edges.size();
			r.check(war_manager.toggle_front_edge(im, *country, front_id, *piece.own, *piece.other) && war_manager.get_front_by_id(front_id)->edges.size() == before - 1
					&& war_manager.get_front_by_id(front_id)->edited,
				"lines: the edit mode takes a piece of the border away (the front is edited)");
			r.check(war_manager.toggle_front_edge(im, *country, front_id, *piece.own, *piece.other) && war_manager.get_front_by_id(front_id)->edges.size() == before,
				"lines: and puts it back");
			r.check(!war_manager.toggle_front_edge(im, *country, front_id, *piece.other, *piece.own), "lines: a piece has our land on our side");
		}

		// An offensive from the front: its targets are land we do not control, the zone holds the targets and the land between
		front = war_manager.get_front_by_id(front_id);
		ProvinceInstance* const target = front->edges.front().other;
		r.check(war_manager.create_plan_line(im, *country, group_id, PlanLine::type_t::OFFENSIVE, *front->provinces.front()) == 0,
			"lines: the target of an offensive is not our land");
		const std::uint32_t offensive_id = war_manager.create_plan_line(im, *country, group_id, PlanLine::type_t::OFFENSIVE, *target);
		PlanLine const* offensive = war_manager.get_plan_line_by_id(offensive_id);
		r.check(offensive != nullptr && offensive->front_id == front_id && offensive->provinces.size() == 1, "lines: an offensive starts from the nearest front of the marshal");
		const memory::vector<ProvinceInstance*> zone = offensive != nullptr ? war_manager.get_offensive_zone(im, *offensive) : memory::vector<ProvinceInstance*> {};
		bool zone_valid = std::find(zone.begin(), zone.end(), target) != zone.end();
		for (ProvinceInstance const* const province : zone) {
			zone_valid &= province->get_controller() != country;
		}
		r.check(zone_valid, "lines: the zone of an offensive holds its targets and no land of ours");
		// A target farther away: the zone reaches it over the land between
		ProvinceInstance* far_target = nullptr;
		for (ProvinceDefinition::adjacency_t const& adjacency : target->province_definition.get_adjacencies()) {
			ProvinceInstance& other = map_instance.get_province_instance_by_definition(adjacency.get_to());
			bool next_to_us = false;
			for (ProvinceDefinition::adjacency_t const& second : other.province_definition.get_adjacencies()) {
				next_to_us |= map_instance.get_province_instance_by_definition(second.get_to()).get_controller() == country;
			}
			if (adjacency.get_type() == ProvinceDefinition::adjacency_t::type_t::LAND && !other.province_definition.is_water()
				&& other.get_controller() != country && !next_to_us) {
				far_target = &other;
				break;
			}
		}
		if (far_target != nullptr && offensive != nullptr) {
			r.check(war_manager.toggle_plan_line_province(im, *country, offensive_id, *far_target)
					&& war_manager.toggle_plan_line_province(im, *country, offensive_id, *target), "lines: a target is added and one taken away");
			offensive = war_manager.get_plan_line_by_id(offensive_id);
			const memory::vector<ProvinceInstance*> far_zone = war_manager.get_offensive_zone(im, *offensive);
			r.check(std::find(far_zone.begin(), far_zone.end(), far_target) != far_zone.end() && far_zone.size() >= 2,
				"lines: the zone of a farther target holds the land between the front and it");
			r.check(!war_manager.toggle_plan_line_province(im, *country, offensive_id, *far_target), "lines: the last target cannot be taken away");
		}

		// One order: a second offensive takes the place of the first, the front stays
		const std::uint32_t second_offensive = war_manager.create_plan_line(im, *country, group_id, PlanLine::type_t::OFFENSIVE, *target);
		r.check(second_offensive != 0 && war_manager.get_plan_line_by_id(offensive_id) == nullptr && war_manager.get_front_by_id(front_id) != nullptr,
			"lines: a new offensive takes the place of the earlier one, the front stays");

		// A line of defence and a naval invasion: each takes the place of the whole earlier order
		front = war_manager.get_front_by_id(front_id);
		ProvinceInstance* const own = front->provinces.front();
		r.check(war_manager.create_plan_line(im, *country, group_id, PlanLine::type_t::DEFENCE, *target) == 0, "lines: a line of defence holds our land");
		const std::uint32_t defence_id = war_manager.create_plan_line(im, *country, group_id, PlanLine::type_t::DEFENCE, *own);
		r.check(defence_id != 0 && war_manager.get_front_by_id(front_id) == nullptr && war_manager.get_plan_line_by_id(second_offensive) == nullptr,
			"lines: a line of defence takes the place of the front and its offensive (one order)");
		ProvinceInstance* port = nullptr;
		for (ProvinceInstance* province : country->get_owned_provinces()) {
			if (province->get_controller() == country && province->province_definition.has_port()) {
				port = province;
				break;
			}
		}
		if (port != nullptr) {
			r.check(war_manager.create_plan_line(im, *country, group_id, PlanLine::type_t::NAVAL, *own) == 0 || own->province_definition.has_port(),
				"lines: a naval invasion starts from a port");
			const std::uint32_t naval_id = war_manager.create_plan_line(im, *country, group_id, PlanLine::type_t::NAVAL, *port);
			r.check(naval_id != 0 && war_manager.get_plan_line_by_id(naval_id)->port == port && war_manager.get_plan_line_by_id(naval_id)->provinces.empty()
					&& war_manager.get_plan_line_by_id(defence_id) == nullptr,
				"lines: a naval invasion has its port, the coast is added afterwards; it takes the place of the line of defence");
		}

		// A coastal front
		const std::uint32_t coast_id = war_manager.create_coastal_front(im, *country, group_id, *sea);
		Front const* const coast = war_manager.get_front_by_id(coast_id);
		bool coast_valid = coast != nullptr && !coast->edges.empty() && coast->kind == Front::kind_t::COAST && coast->sea == sea;
		if (coast != nullptr) {
			for (FrontEdge const& edge : coast->edges) {
				coast_valid &= edge.other == sea && edge.own->get_controller() == country;
			}
		}
		r.check(coast_valid, "lines: a coastal front runs along our coast next to the sea province");
		r.check(war_manager.create_coastal_front(im, *country, group_id, *own) == 0, "lines: a coastal front needs a sea province");
		r.check(coast != nullptr && war_manager.get_plan_lines().end() == std::find_if(war_manager.get_plan_lines().begin(), war_manager.get_plan_lines().end(),
				[group_id](PlanLine const& line) -> bool { return line.group_id == group_id; }),
			"lines: a coastal front takes the place of the earlier order");

		// The plan is in the state hash, the trash deletes all the lines of the marshal
		const std::uint64_t hash_before = StateHash::compute(im);
		set_army_group_modes(*country, group_id, 1, 2);
		r.check(StateHash::compute(im) != hash_before, "lines: the modes of the plan are in the state hash");
		r.check(war_manager.delete_group_lines(im, *country, group_id) && war_manager.find_front_of_group(*country, group_id) == nullptr
				&& std::none_of(war_manager.get_plan_lines().begin(), war_manager.get_plan_lines().end(), [group_id](PlanLine const& line) -> bool {
					return line.group_id == group_id;
				}),
			"lines: the trash deletes the fronts and the lines of the marshal");
		delete_army_group(im, *country, group_id);
	}

	// ---- the theatres of the field marshals (B4)

	void test_theatres(Results& r, InstanceManager& im) {
		CountryInstance* country = nullptr;
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (candidate.exists() && !candidate.is_rebel_country() && candidate.has_army_command() && candidate.get_army_groups().empty() && candidate.get_theatres().size() <= 1
				&& !candidate.get_owned_provinces().empty()) {
				country = &candidate;
				break;
			}
		}
		if (country == nullptr) {
			fmt::println("TEST INFO theatres: no country without groups, not checked");
			return;
		}
		const std::size_t palette_size = im.definition_manager.get_military_manager().get_command_palette().size();
		const std::size_t max_theatres = static_cast<std::size_t>(im.definition_manager.get_define_manager().get_openvic_defines().war.theatres_max);
		r.check(max_theatres == 8, "theatres: a country has 8 theatres at most by default");

		// The main theatre is made by itself with the first group, and takes it
		r.check(create_army_group(*country, palette_size), "theatres: a group is created");
		const std::uint32_t main_id = get_main_theatre_id(*country);
		r.check(country->get_theatres().size() == 1 && main_id != 0 && country->get_army_groups().back().theatre_id == main_id,
			"theatres: a country with the organisation has the main theatre (made by the daily update or by the first group) and its first group belongs to it");
		const std::uint32_t group_id = country->get_army_groups().back().id;

		// A second theatre, its colour, its name, its focus
		r.check(create_theatre(*country, palette_size, max_theatres) && country->get_theatres().size() == 2, "theatres: a second theatre is created");
		const std::uint32_t second_id = country->get_theatres().back().id;
		r.check(country->get_theatres()[0].colour_index != country->get_theatres()[1].colour_index, "theatres: the theatres take different colours of the palette");
		r.check(set_theatre_colour(*country, second_id, 9, palette_size) && country->find_theatre(second_id)->colour_index == 9 && !set_theatre_colour(*country, second_id, 9, palette_size)
				&& !set_theatre_colour(*country, second_id, static_cast<std::uint32_t>(palette_size), palette_size) && !set_theatre_colour(*country, 9999, 1, palette_size),
			"theatres: the colour of the frame is changed (a colour of the palette, a known theatre, a change)");
		theatre_name_t name {};
		const std::string_view text = "Северный театр"; // UTF-8
		std::copy(text.begin(), text.end(), name.begin());
		r.check(rename_theatre(*country, second_id, name) && country->find_theatre(second_id)->name == name && !rename_theatre(*country, second_id, name) && !rename_theatre(*country, 9999, name),
			"theatres: a theatre is renamed (UTF-8 in 48 bytes)");
		theatre_name_t full_name;
		full_name.fill(0x41);
		rename_theatre(*country, second_id, full_name);
		r.check(country->find_theatre(second_id)->name.back() == 0, "theatres: a name always ends with a zero");
		rename_theatre(*country, second_id, name);
		ProvinceInstance const* land = nullptr;
		for (ProvinceInstance const* province : country->get_owned_provinces()) {
			if (!province->province_definition.is_water()) {
				land = province;
				break;
			}
		}
		const std::uint32_t number = land != nullptr ? static_cast<std::uint32_t>(land->province_definition.get_province_number()) : 0;
		r.check(number != 0 && set_theatre_focus(im, *country, second_id, number) && country->find_theatre(second_id)->focus_province == number && !set_theatre_focus(im, *country, second_id, number),
			"theatres: the camera focus is a land province of the map");
		r.check(!set_theatre_focus(im, *country, second_id, 99999) && !set_theatre_focus(im, *country, second_id, 70000) && set_theatre_focus(im, *country, second_id, 0) && country->find_theatre(second_id)->focus_province == 0,
			"theatres: a province that does not exist is refused, 0 takes the focus away");

		// A marshal moves to another theatre
		r.check(set_army_group_theatre(*country, group_id, second_id) && country->find_army_group(group_id)->theatre_id == second_id && !set_army_group_theatre(*country, group_id, second_id)
				&& !set_army_group_theatre(*country, group_id, 9999) && !set_army_group_theatre(*country, 9999, second_id),
			"theatres: a marshal goes to another theatre (once, a known theatre and group)");
		// The theatre is deleted: the main one takes its marshals; the main one stays
		r.check(!delete_theatre(*country, main_id) && !delete_theatre(*country, 9999), "theatres: the main theatre and an unknown one are not deleted");
		r.check(delete_theatre(*country, second_id) && country->find_army_group(group_id)->theatre_id == main_id && country->get_theatres().size() == 1,
			"theatres: a deleted theatre gives its marshals to the main one");
		// A group of a theatre that is gone belongs to the main one after the daily update
		country->get_mutable_army_groups().back().theatre_id = 9999;
		update_army_groups(im, *country);
		r.check(country->find_army_group(group_id)->theatre_id == main_id, "theatres: a group of a theatre that does not exist goes to the main theatre by the daily update");
		// The limit
		std::size_t made = 0;
		while (create_theatre(*country, palette_size, max_theatres)) {
			++made;
		}
		r.check(country->get_theatres().size() == max_theatres && made == max_theatres - 1, "theatres: no more than the limit");

		// The name goes through the replay (the commands are saved as text)
		{
			rename_theatre_argument_t::underlying_type value {};
			std::get<1>(value) = 1;
			std::get<2>(value) = name;
			std::vector<ReplayEntry> entries;
			entries.push_back({ 1, game_action_t(std::in_place_type<rename_theatre_argument_t>, std::as_const(value)) });
			std::vector<ReplayEntry> loaded;
			const bool parsed = Replay::deserialise(Replay::serialise(entries), loaded);
			r.check(parsed && loaded.size() == 1 && std::holds_alternative<rename_theatre_argument_t>(loaded[0].action)
					&& std::get<2>(type_safe::get(std::get<rename_theatre_argument_t>(loaded[0].action))) == name,
				"theatres: the name of a theatre goes through the text of the replay unchanged");
		}

		// clean up: the groups and the theatres of the test
		const std::vector<std::uint32_t> theatre_ids = [&]() {
			std::vector<std::uint32_t> ids;
			for (Theatre const& theatre : country->get_theatres()) {
				ids.push_back(theatre.id);
			}
			return ids;
		}();
		for (const std::uint32_t id : theatre_ids) {
			delete_theatre(*country, id);
		}
		delete_army_group(im, *country, group_id);
	}

	// ---- B5: the technologies that raise the number of generals a field marshal commands (+12 each: 12, 24, 36, 48)

	void test_attached_generals_technologies(Results& r, InstanceManager& im) {
		ArmyTechnologyManager const& manager = im.definition_manager.get_research_manager().get_army_technology_manager();
		ModifierManager const& modifier_manager = im.definition_manager.get_modifier_manager();
		CountryInstance* country = nullptr;
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (candidate.exists() && !candidate.is_rebel_country() && candidate.is_civilised() && candidate.is_ai()) {
				country = &candidate;
				break;
			}
		}
		const std::array<std::string_view, 3> ids { "general_staff", "mobilisation_planning", "operational_planning" };
		if (country == nullptr || manager.size() == 0) {
			fmt::println("TEST INFO attached generals: no country or no technologies, not checked");
			return;
		}
		const auto refresh = [&]() { country->update_modifier_sum(im.get_today(), modifier_manager.get_static_modifier_cache()); };
		bool all_have_effect = true;
		std::vector<std::size_t> done;
		refresh();
		const std::uint32_t base = get_marshal_max_attached(im, *country);
		r.check(base == 12, "attached generals: without the technologies a marshal commands 12");
		std::vector<std::uint32_t> limits;
		for (const std::string_view id : ids) {
			ArmyTechnology const* const technology = manager.find(id);
			bool has_effect = false;
			if (technology != nullptr && technology->modifier.has_value()) {
				for (auto const& [effect, value] : technology->modifier->get_values()) {
					has_effect |= effect->get_identifier() == "attached_generals_add" && value == fixed_point_t(12);
				}
			}
			all_have_effect &= has_effect;
			if (technology != nullptr && !country->has_army_technology(static_cast<std::uint32_t>(technology->index))) {
				complete_army_technology(im, *country, technology->index);
				done.push_back(technology->index);
			}
			refresh();
			limits.push_back(get_marshal_max_attached(im, *country));
		}
		r.check(all_have_effect, "attached generals: general_staff, mobilisation_planning and operational_planning give attached_generals_add = 12 in their files");
		r.check(limits.size() == 3 && limits[0] == 24 && limits[1] == 36 && limits[2] == 48, "attached generals: with the three technologies a marshal commands 24, 36 and 48 generals",
			fmt::format("{} {} {}", limits.size() > 0 ? limits[0] : 0, limits.size() > 1 ? limits[1] : 0, limits.size() > 2 ? limits[2] : 0));
		// put the country back
		for (const std::size_t index : done) {
			ArmyTechnology const& technology = manager.at(index);
			country->unmark_army_technology_researched(static_cast<std::uint32_t>(index));
			for (memory::string const& key : technology.unlocks) {
				country->revoke_unlock(key);
			}
			if (technology.modifier.has_value()) {
				country->remove_unlock_modifier(*technology.modifier);
			}
		}
		refresh();
		r.check(get_marshal_max_attached(im, *country) == base, "attached generals: the country is back to the base limit");
	}

	// ---- a fleet goes into a port of its country

	void test_fleet_to_port(Results& r, InstanceManager& im) {
		NavyInstance* navy = nullptr;
		for (CountryInstance& country : im.get_country_instance_manager().get_country_instances()) {
			if (country.exists() && !country.is_rebel_country() && !country.get_navies().empty()) {
				navy = &country.get_navies().front().get();
				break;
			}
		}
		if (navy == nullptr) {
			fmt::println("TEST INFO fleet to port: no fleet, not checked");
			return;
		}
		CountryInstance& owner = navy->get_country();
		std::size_t ports = 0, ordered = 0, failed_without_path = 0;
		std::string example;
		for (ProvinceInstance& province : im.get_map_instance().get_province_instances()) {
			if (province.province_definition.is_water() || !province.province_definition.has_port() || province.get_owner() != &owner) {
				continue;
			}
			ports++;
			navy->clear_path();
			if (im.run_game_action_now<move_army_argument_t>(navy->unique_id, province.index)) {
				ordered++;
				failed_without_path += navy->get_path().empty() && &navy->get_location() != &province;
			} else if (example.empty()) {
				example = std::string { province.get_identifier() };
			}
		}
		navy->clear_path();
		fmt::println("TEST INFO fleet to port: {} of {} ports of {} accepted, the fleet is in {} (water: {})", ordered, ports, owner.get_identifier(), navy->get_location().get_identifier(), navy->get_location().province_definition.is_water());
		r.check(ports > 0 && ordered > 0, "fleet to port: a fleet can be ordered to the ports of its country", fmt::format("{} of {} ports ordered, e.g. refused: {}", ordered, ports, example));
		r.check(failed_without_path == 0, "fleet to port: an accepted order has a route");
	}

	// ---- the population of the start: the pops of the instance are the pops of the history of the start date; the numbers of the top bar come from the sum of them

	void test_population_start(Results& r, InstanceManager& im) {
		DefinitionManager const& definitions = im.definition_manager;
		std::int64_t history_total = 0, instance_total = 0, countries_total = 0, unowned_total = 0;
		for (ProvinceInstance const& province : im.get_map_instance().get_province_instances()) {
			if (province.province_definition.is_water()) {
				continue;
			}
			const std::int64_t size = type_safe::get(province.get_total_population());
			instance_total += size;
			unowned_total += province.get_owner() == nullptr ? size : 0;
			ProvinceHistoryMap const* const history_map = definitions.get_history_manager().get_province_manager().get_province_history(&province.province_definition);
			if (history_map == nullptr) {
				continue;
			}
			ProvinceHistoryEntry const* pop_entry = nullptr;
			for (auto const& [entry_date, entry] : history_map->get_entries()) {
				if (entry_date > im.get_today() && pop_entry != nullptr) {
					break;
				}
				if (!entry->get_pops().empty()) {
					pop_entry = entry.get();
				}
			}
			if (pop_entry != nullptr) {
				for (PopBase const& pop : pop_entry->get_pops()) {
					history_total += type_safe::get(pop.get_size());
				}
			}
		}
		bool changes_ok = true;
		for (CountryInstance const& country : im.get_country_instance_manager().get_country_instances()) {
			countries_total += type_safe::get(country.get_total_population());
			// at the start the month has been rolled: no change yet, the month started with the whole population
			changes_ok &= type_safe::get(country.get_population_last_monthly_change()) == 0
				&& country.get_population_month_start() == country.get_total_population();
		}
		r.check(instance_total > 0 && instance_total == history_total, "population: the pops of the start are the pops of the history of the start date (every province)", fmt::format("{} against {}", instance_total, history_total));
		r.check(countries_total + unowned_total == instance_total, "population: the totals of the countries and of the provinces nobody owns make the world", fmt::format("{} + {} against {}", countries_total, unowned_total, instance_total));
		r.check(changes_ok, "population: on the first day the change of the month is 0 and the month starts with the whole population");
	}

	// ---- the natural growth of the population (population/PopulationGrowth.*, ov_data/common/ov_population/growth.txt)

	void test_population_growth(Results& r, InstanceManager& im) {
		namespace fs = std::filesystem;
		OpenVicPopulationDefines const& defines = im.definition_manager.get_define_manager().get_openvic_defines().population;
		PopManager const& pop_manager = im.definition_manager.get_pop_manager();
		PopulationGrowthData const& data = pop_manager.get_growth_data();
		const fixed_point_t eps = fixed_point_t(1) / 10000;
		const fixed_point_t fallback = defines.growth_per_year.fixed();
		const auto rate = [&](std::string_view tag, std::string_view group, std::string_view culture, const std::int32_t year) {
			return data.get_yearly_rate(tag, group, culture, "", year, fallback);
		};
		if (!data.is_loaded()) {
			fmt::println("TEST INFO population growth: no ov_data folder, the rules are not checked");
		} else {
			r.check(near(rate("ENG", "british", "english", 1836), fixed_point_t(12) / 1000, eps) && near(rate("RUS", "east_slavic", "russian", 1836), fixed_point_t(13) / 1000, eps)
					&& near(rate("USA", "american_cultures", "yankee", 1850), fixed_point_t(25) / 1000, eps) && near(rate("FRA", "french", "french", 1836), fixed_point_t(4) / 1000, eps),
				"population growth: the countries of the data grow by their yearly rates (UK 1.2 %, Russia 1.3 %, the USA 2.5 %, France 0.4 %)");
			r.check(near(rate("PRU", "germanic", "prussian", 1836), fixed_point_t(1) / 100, eps) && near(rate("AUS", "germanic", "austrian", 1836), fixed_point_t(8) / 1000, eps),
				"population growth: a culture group gives its rate, a tag is more specific than a group (Prussia 1.0 %, Austria 0.8 % although its Germans are of the group)");
			r.check(near(rate("JAP", "east_asian", "japanese", 1850), fixed_point_t(15) / 10000, eps) && near(rate("JAP", "east_asian", "japanese", 1868), fixed_point_t(7) / 1000, eps),
				"population growth: Japan grows by 0.15 % until 1868 and by 0.7 % from 1868 (the periods of a rule)");
			r.check(near(rate("ENG", "british", "irish", 1840), fixed_point_t(12) / 1000, eps) && rate("ENG", "british", "irish", 1848) < fixed_point_t::_0
					&& near(rate("ENG", "british", "irish", 1860), fixed_point_t(3) / 1000, eps) && near(rate("ENG", "british", "english", 1848), fixed_point_t(12) / 1000, eps),
				"population growth: the Irish of the UK grow until 1845, shrink in 1846-1851 and grow slowly after, the English do not (a culture inside a country)");
			r.check(near(rate("XXX", "nowhere", "nobody", 1900), fixed_point_t(7) / 1000, eps), "population growth: a pop that no rule is for grows by the default of the file (0.7 %)");
		}

		// The rate of the month: the life needs, the famine of the data, the modifiers of the country
		using population_growth::monthly_yearly_rate;
		const fixed_point_t base = fixed_point_t(12) / 1000;
		const fixed_point_t breakeven = defines.growth_breakeven_life_needs.fixed();
		r.check(near(monthly_yearly_rate(defines, base, fixed_point_t::_1, fixed_point_t::_1), base, eps), "population growth: a pop with all its life needs met grows by the whole base");
		r.check(monthly_yearly_rate(defines, base, breakeven, fixed_point_t::_1) == fixed_point_t::_0 && monthly_yearly_rate(defines, base, (breakeven + fixed_point_t::_1) / 2, fixed_point_t::_1) > fixed_point_t::_0,
			"population growth: at the breakeven of the life needs the growth stops, above it grows with the life needs");
		r.check(monthly_yearly_rate(defines, base, fixed_point_t::_0, fixed_point_t::_1) < fixed_point_t::_0 && near(monthly_yearly_rate(defines, base, fixed_point_t::_0, fixed_point_t(5)), -defines.decline_per_year.fixed(), eps),
			"population growth: a hungry pop shrinks, and the modifiers do not make the decline bigger");
		r.check(monthly_yearly_rate(defines, fixed_point_t(-2) / 100, fixed_point_t::_1, fixed_point_t::_1) == fixed_point_t(-2) / 100 && monthly_yearly_rate(defines, fixed_point_t(-2) / 100, fixed_point_t::_0, fixed_point_t::_1) < fixed_point_t(-2) / 100,
			"population growth: a negative base of the data (a famine) always counts, a hunger comes on top of it");
		r.check(near(monthly_yearly_rate(defines, base, fixed_point_t::_1, fixed_point_t(3) / 2), base * 3 / 2, eps), "population growth: the multiplier of the country multiplies the growth");
		r.check(near(population_growth::growth_multiplier(defines, fixed_point_t(1) / 10, fixed_point_t::_1), fixed_point_t(11) / 10, eps)
				&& population_growth::growth_multiplier(defines, fixed_point_t::_0, fixed_point_t(8) / 10) > fixed_point_t::_1
				&& near(population_growth::growth_multiplier(defines, fixed_point_t::_0, fixed_point_t::_1), fixed_point_t::_1, eps),
			"population growth: the sanitation (population_growth_pct) and the medicine (the casualties taken under 1) raise the growth, nothing changes without them");

		// A file: a new rule comes with the file, the broken ones are skipped
		const fs::path temp = fs::temp_directory_path() / "openvic_selftest_growth";
		std::error_code ec;
		fs::remove_all(temp, ec);
		write_test_file(temp / "good.txt", "default = 0.01\nrule = { tag = AAA yearly = 0.02 }\nrule = { tag = BBB periods = { 1900 = 0.03 1800 = 0.01 } }\n");
		write_test_file(temp / "bad.txt",
			"rule = { yearly = 0.02 }\nrule = { tag = AAA }\nrule = { tag = AAA yearly = 0.02 periods = { 1900 = 0.01 } }\nrule = { group = no_such_group_at_all yearly = 0.01 }\nrule = { tag = AAA periods = { year = 0.01 } }\n");
		PopulationGrowthData fresh;
		const ovdl::v2script::Parser good = Dataloader::parse_defines(temp / "good.txt");
		const ovdl::v2script::Parser bad = Dataloader::parse_defines(temp / "bad.txt");
		const std::size_t good_count = fresh.load_file("good.txt", good.get_file_node(), pop_manager.get_culture_manager());
		const std::size_t bad_count = fresh.load_file("bad.txt", bad.get_file_node(), pop_manager.get_culture_manager());
		r.check(good_count == 2 && near(fresh.get_yearly_rate("AAA", "", "", "", 1836, fallback), fixed_point_t(2) / 100, eps) && near(fresh.get_yearly_rate("ZZZ", "", "", "", 1836, fallback), fixed_point_t(1) / 100, eps)
				&& near(fresh.get_yearly_rate("BBB", "", "", "", 1950, fallback), fixed_point_t(3) / 100, eps),
			"population growth: a new rule and the default come with a file, no code is needed (the periods are sorted)");
		r.check(bad_count == 0, "population growth: a rule with no condition, with both yearly and periods or neither, a group that does not exist or a period that is not a year is skipped");
		fs::remove_all(temp, ec);
	}

	// The budget of the AI countries (CountryAI::balance_budget): after some months a country that spent more than it earned has raised its taxes, none goes over the top
	void test_ai_budget(Results& r, InstanceManager& im) {
		OpenVicAIDefines const& ai = im.definition_manager.get_define_manager().get_openvic_defines().ai;
		PopManager const& pop_manager = im.definition_manager.get_pop_manager();
		std::size_t raised = 0, over_the_top = 0, player_changed = 0;
		for (CountryInstance const& country : im.get_country_instance_manager().get_country_instances()) {
			if (!country.exists() || country.get_owned_provinces().empty()) {
				continue;
			}
			for (Strata const& strata : pop_manager.get_stratas()) {
				const fixed_point_t rate = country.get_tax_rate_slider_value_by_strata(strata).get_value_untracked();
				if (country.is_ai()) {
					raised += rate > fixed_point_t::_0_50;
					over_the_top += rate > ai.budget_max_tax.fixed();
				} else {
					player_changed += rate != fixed_point_t::_0_50;
				}
			}
		}
		r.check(ai.budget_enabled == 0 || raised > 0, "AI budget: a country that spends more than it earns raises its taxes", fmt::format("{} strata raised", raised));
		r.check(over_the_top == 0 && player_changed == 0, "AI budget: the taxes stay under BUDGET_MAX_TAX and the sliders of a player are not touched");
	}

	// The weapons of the artisans (ProductionTypeManager::set_artisan_weapon_unlocks): the generations up to ARTISAN_WEAPON_FREE_TIER need the unlock of the
	// weapon itself, a higher one the unlock of the weapon ARTISAN_WEAPON_TIER_LAG generations newer (rifle_2 with rifle_4)
	void test_artisan_weapons(Results& r, InstanceManager& im) {
		OpenVicEconomyDefines const& economy = im.definition_manager.get_define_manager().get_openvic_defines().economy;
		EquipmentManager const& equipment = im.definition_manager.get_military_manager().get_equipment_manager();
		std::size_t checked = 0, wrong = 0;
		for (ProductionType const& production_type : im.definition_manager.get_economy_manager().get_production_type_manager().get_production_types()) {
			if (production_type.template_type != ProductionType::template_type_t::ARTISAN) {
				continue;
			}
			EquipmentDefinition const* made = nullptr;
			for (EquipmentDefinition const& weapon : equipment.get_equipment()) {
				if (weapon.good.identifier == production_type.output_good.get_identifier()) {
					made = &weapon;
				}
			}
			if (made == nullptr) {
				wrong += !production_type.artisan_unlock_key.empty();
				continue;
			}
			checked++;
			memory::string expected = "artisan:never";
			if (static_cast<std::int32_t>(made->tier) <= economy.artisan_weapon_free_tier) {
				expected = made->unlock_key;
			} else {
				for (EquipmentDefinition const& weapon : equipment.get_equipment()) {
					if (weapon.line == made->line && static_cast<std::int32_t>(weapon.tier) == static_cast<std::int32_t>(made->tier) + economy.artisan_weapon_tier_lag) {
						expected = weapon.unlock_key;
					}
				}
			}
			wrong += production_type.artisan_unlock_key != expected;
		}
		r.check(checked >= 3 && wrong == 0, "artisans: the weapons they make need the unlocks of their generation (rifle_2 with rifle_4)",
			fmt::format("{} artisan weapon types, {} wrong", checked, wrong));
	}

	// The buildings of the RGOs (RgoBuildingData, ov_data/common/ov_rgo_buildings): the goods, the files, the levels, the payment, the staff
	void test_rgo_buildings(Results& r, InstanceManager& im) {
		namespace fs = std::filesystem;
		DefinitionManager const& definitions = im.definition_manager;
		OpenVicEconomyDefines const& defines = definitions.get_define_manager().get_openvic_defines().economy;
		GoodDefinitionManager const& goods = definitions.get_economy_manager().get_good_definition_manager();
		RgoBuildingData const& data = definitions.get_economy_manager().get_production_type_manager().get_rgo_buildings();
		const auto building_of = [&](std::string_view good_identifier) -> std::string {
			GoodDefinition const* const good = goods.get_good_definition_by_identifier(good_identifier);
			RgoBuildingType const* const type = good == nullptr ? nullptr : data.get_type_for_good(*good);
			return type == nullptr ? std::string {} : std::string { type->identifier };
		};
		if (data.get_types().empty()) {
			fmt::println("TEST INFO RGO buildings: no ov_data folder, the buildings are not checked");
		} else {
			r.check(building_of("fish") == "fishing_harbour" && building_of("cattle") == "farm" && building_of("wool") == "farm" && building_of("grain") == "harvest_field"
					&& building_of("tea") == "plantation" && building_of("coffee") == "plantation" && building_of("fruit") == "plantation",
				"RGO buildings: the fishing harbour is for fish, the farm for cattle and wool, the harvest field for grain, the plantation for tea, coffee and fruit");
			std::size_t rgo_goods = 0, without_building = 0;
			for (ProductionType const& production_type : definitions.get_economy_manager().get_production_type_manager().get_production_types()) {
				if (production_type.template_type == ProductionType::template_type_t::RGO) {
					rgo_goods++;
					without_building += building_of(production_type.output_good.get_identifier()).empty();
				}
			}
			r.check(rgo_goods > 0 && without_building == 0, "RGO buildings: every good that an RGO makes has a building", fmt::format("{} RGO goods, {} without a building", rgo_goods, without_building));
		}

		// The files: a new building and the good of a later file, the broken ones are skipped
		const fs::path temp = fs::temp_directory_path() / "openvic_selftest_rgo_buildings";
		std::error_code ec;
		fs::remove_all(temp, ec);
		write_test_file(temp / "good.txt", "tea_house = { goods = { tea } max_level = 4 build_days = 30 lumber = 3 }\nsalt_pan = { goods = { fish no_such_good } }\n");
		write_test_file(temp / "bad.txt", "nothing = { }\nnothing_either = { goods = { no_such_good } }\nwrong = { goods = { tea } max_level = -1 }\n");
		RgoBuildingData fresh;
		const ovdl::v2script::Parser good = Dataloader::parse_defines(temp / "good.txt");
		const ovdl::v2script::Parser bad = Dataloader::parse_defines(temp / "bad.txt");
		const std::size_t good_count = fresh.load_file("good.txt", good.get_file_node(), goods);
		const std::size_t bad_count = fresh.load_file("bad.txt", bad.get_file_node(), goods);
		GoodDefinition const* const tea = goods.get_good_definition_by_identifier("tea");
		RgoBuildingType const* const tea_house = tea == nullptr ? nullptr : fresh.get_type_for_good(*tea);
		r.check(good_count == 2 && bad_count == 0 && tea_house != nullptr && tea_house->max_level == 4 && tea_house->build_days == 30 && tea_house->lumber == fixed_point_t(3),
			"RGO buildings: a new building comes with a file with its own level, days and lumber; buildings with no goods or errors are skipped");
		fs::remove_all(temp, ec);

		// The levels: the maximum is 2 and the technologies open the next ones
		CountryInstance* player_like = nullptr;
		ProvinceInstance* target = nullptr;
		for (ProvinceInstance& province : im.get_map_instance().get_province_instances()) {
			ResourceGatheringOperation const& rgo = province.get_mutable_rgo();
			if (province.get_owner() != nullptr && rgo.is_valid() && rgo.get_building_level() == 0 && rgo.get_building_days_left() == 0
				&& rgo.get_max_employee_count_cache() >= 100 && !building_of(rgo.get_production_type_nullable()->output_good.get_identifier()).empty()) {
				target = &province;
				player_like = province.get_owner();
				break;
			}
		}
		if (target == nullptr || player_like == nullptr) {
			r.check(false, "RGO buildings: a province with an RGO was found for the checks");
			return;
		}
		ResourceGatheringOperation& rgo = target->get_mutable_rgo();
		RgoBuildingType const* const type = data.get_type_for_good(rgo.get_production_type_nullable()->output_good);
		const std::int32_t default_max = RgoBuildingData::get_max_level(*type, defines, *player_like);
		const std::string key = "rgo_level:" + std::string { type->identifier } + ":" + std::to_string(default_max + 1);
		const bool granted = player_like->grant_unlock(key);
		const std::int32_t opened_max = RgoBuildingData::get_max_level(*type, defines, *player_like);
		player_like->revoke_unlock(key);
		r.check(default_max == defines.rgo_building_max_level && granted && opened_max == default_max + 1 && RgoBuildingData::get_max_level(*type, defines, *player_like) == default_max,
			"RGO buildings: the maximum level is RGO_BUILDING_MAX_LEVEL (2), a technology key opens the next level, taking it away closes it");

		// A start: no lumber in the stockpile - no building; with it the state or the pops pay, a second start waits for the first
		GoodDefinition const* const lumber = goods.get_good_definition_by_identifier("lumber");
		CountryInstance::good_data_t& lumber_data = player_like->get_good_data(*lumber);
		lumber_data.stockpile_amount = fixed_point_t::_0;
		const rgo_build_result_t without_lumber = RgoBuildingData::start_building(im, *target, rgo_builder_t::STATE);
		lumber_data.stockpile_amount = fixed_point_t(100);
		const pop_size_t cap_before = rgo.get_max_employee_count_cache();
		fixed_point_t pops_cash_before = fixed_point_t::_0;
		for (Pop const& pop : target->get_pops()) {
			pops_cash_before += pop.get_cash().get_copy_of_value();
		}
		const fixed_point_t treasury_before = player_like->get_cash_stockpile().load();
		const rgo_build_result_t started = RgoBuildingData::start_building(im, *target, rgo_builder_t::POPS);
		fixed_point_t pops_cash_after = fixed_point_t::_0;
		for (Pop const& pop : target->get_pops()) {
			pops_cash_after += pop.get_cash().get_copy_of_value();
		}
		const fixed_point_t paid = pops_cash_before - pops_cash_after;
		r.check(without_lumber == rgo_build_result_t::NO_LUMBER && (started == rgo_build_result_t::OK || started == rgo_build_result_t::NO_MONEY),
			"RGO buildings: without lumber in the national stockpile nothing is built");
		if (started != rgo_build_result_t::OK) {
			// the pops of this province are too poor: the state pays
			r.check(RgoBuildingData::start_building(im, *target, rgo_builder_t::STATE) == rgo_build_result_t::OK, "RGO buildings: the state can start a level when the pops are too poor");
		} else {
			r.check(paid > fixed_point_t::_0 && player_like->get_cash_stockpile().load() - treasury_before == paid && lumber_data.stockpile_amount == fixed_point_t(100) - RgoBuildingData::get_lumber(*type, defines),
				"RGO buildings: the pops pay the price of the lumber to the treasury and the lumber leaves the stockpile",
				fmt::format("paid {} treasury +{} lumber left {}", static_cast<double>(paid), static_cast<double>(player_like->get_cash_stockpile().load() - treasury_before), static_cast<double>(lumber_data.stockpile_amount)));
		}
		r.check(RgoBuildingData::start_building(im, *target, rgo_builder_t::STATE) == rgo_build_result_t::BUSY && rgo.get_building_days_left() == RgoBuildingData::get_build_days(*type, defines),
			"RGO buildings: a level takes RGO_BUILDING_BUILD_DAYS (90) days and the next one waits for it");
		for (int day = 0; day < defines.rgo_building_build_days + 2; ++day) {
			im.force_tick_and_update();
		}
		const pop_size_t cap_after = rgo.get_max_employee_count_cache();
		const double expected_cap = static_cast<double>(type_safe::get(cap_before)) * (1.0 + static_cast<double>(defines.rgo_building_staff_per_level.fixed()));
		r.check(rgo.get_building_level() >= 1 && type_safe::get(cap_after) > type_safe::get(cap_before) && static_cast<double>(type_safe::get(cap_after)) >= expected_cap - 1.0,
			"RGO buildings: after the days the level is built and the RGO has 5 % more workplaces", fmt::format("level {} cap {} -> {} (expected {})", rgo.get_building_level(), type_safe::get(cap_before), type_safe::get(cap_after), expected_cap));
	}

	// The troops use less wool, clothes and cattle (ARMY_CLOTH_CATTLE_SHARE): the cavalry of ov_data has 20 wool in its cost and 0.2 in its supply
	void test_army_cloth_share(Results& r, InstanceManager& im) {
		DefinitionManager const& definitions = im.definition_manager;
		const fixed_point_t share = definitions.get_define_manager().get_openvic_defines().economy.army_cloth_cattle_share.fixed();
		GoodDefinition const* const wool = definitions.get_economy_manager().get_good_definition_manager().get_good_definition_by_identifier("wool");
		RegimentType const* const cavalry = definitions.get_military_manager().get_unit_type_manager().get_regiment_type_by_identifier("cavalry");
		if (wool == nullptr || cavalry == nullptr) {
			fmt::println("TEST INFO army cloth share: no cavalry of ov_data, not checked");
			return;
		}
		const auto cost = cavalry->get_build_cost().find(wool);
		const auto supply = cavalry->get_supply_cost().find(wool);
		r.check(cost != cavalry->get_build_cost().end() && supply != cavalry->get_supply_cost().end() && near(cost->second, fixed_point_t(20) * share, fixed_point_t(1) / 100)
				&& near(supply->second, fixed_point_t(2) / 10 * share, fixed_point_t(1) / 100),
			"army cloth share: the wool in the cost and the supply of the cavalry is ARMY_CLOTH_CATTLE_SHARE of the file (half by default)");
	}

	// The lumber of the ships (SHIP_LUMBER_PER_PIECE): in the build cost of every ship, and 1 % of it (the navy durable share) is the supply of a month
	void test_ship_lumber(Results& r, InstanceManager& im) {
		DefinitionManager const& definitions = im.definition_manager;
		GoodDefinition const* const lumber = definitions.get_economy_manager().get_good_definition_manager().get_good_definition_by_identifier("lumber");
		const fixed_point_t per_piece = definitions.get_define_manager().get_openvic_defines().economy.ship_lumber_per_piece;
		std::size_t ships = 0, with_lumber = 0, right_amount = 0, in_supply = 0;
		for (ShipType const& ship_type : definitions.get_military_manager().get_unit_type_manager().get_ship_types()) {
			ships++;
			fixed_point_t pieces = fixed_point_t::_0, lumber_cost = fixed_point_t::_0;
			for (auto const& [good, quantity] : ship_type.get_build_cost()) {
				if (good == lumber) {
					lumber_cost = quantity;
				} else {
					pieces += quantity;
				}
			}
			with_lumber += lumber_cost > fixed_point_t::_0;
			right_amount += near(lumber_cost, pieces * per_piece, fixed_point_t(1) / 100);
			for (auto const& item : ship_type.get_supply_items()) {
				in_supply += item.good == lumber && !item.consumable && near(item.construction_amount, lumber_cost, fixed_point_t(1) / 100);
			}
		}
		r.check(lumber != nullptr && ships > 0 && with_lumber == ships && right_amount == ships, "ship lumber: every ship has the lumber of SHIP_LUMBER_PER_PIECE for each piece of its cost in the build cost", fmt::format("{} ships, {} with lumber, {} right", ships, with_lumber, right_amount));
		r.check(in_supply == ships, "ship lumber: the lumber is a durable supply item of the ship (its monthly share is NAVY_SUPPLY_DURABLE_SHARE of the build amount)");
	}

	// The migration to free jobs (PopDynamics::job_migration_take, JOB_MIGRATION_*): 5 % of the unemployed, a few of the employed when the place pays more
	void test_job_migration(Results& r, InstanceManager& im) {
		OpenVicPopulationDefines const& defines = im.definition_manager.get_define_manager().get_openvic_defines().population;
		using PopDynamics::job_migration_take;
		r.check(job_migration_take(defines, 1000, 9000, false, 10000, 1000000) == 50, "job migration: 5 % of 1000 unemployed go, the employed stay when the place does not pay more");
		r.check(job_migration_take(defines, 1000, 9000, true, 10000, 1000000) == 50 + 18, "job migration: 0.2 % of the 9000 employed go as well when the place pays much more");
		r.check(job_migration_take(defines, 3, 0, false, 10, 1000000) == 1 && job_migration_take(defines, 0, 1000, false, 1000, 1000) == 0,
			"job migration: at least one of a few unemployed goes, nobody goes when none is unemployed and the place does not pay more");
		r.check(job_migration_take(defines, 1000, 0, false, 10000, 20) == 20 && job_migration_take(defines, 500, 0, false, 30, 1000) == 25 && job_migration_take(defines, 5, 0, false, 1, 1000) == 0,
			"job migration: not more than the room of the place and never the last person of the pop");
		PopManager const& pop_manager = im.definition_manager.get_pop_manager();
		r.check(pop_manager.job_migrated_total >= pop_manager.job_retrained_total && pop_manager.job_retrained_total >= 0,
			"job migration: the people who changed the type are some of those who moved", fmt::format("{} moved, {} changed the type", pop_manager.job_migrated_total, pop_manager.job_retrained_total));
	}

	// The fish of the whole world: the output of the fishing grounds is multiplied by FISH_OUTPUT_MULTIPLIER once after the loading (the files give 2.2)
	void test_fish_output(Results& r, InstanceManager& im) {
		DefinitionManager const& definitions = im.definition_manager;
		GoodDefinition const* const fish = definitions.get_economy_manager().get_good_definition_manager().get_good_definition_by_identifier("fish");
		ProductionType const* const wharf = fish == nullptr ? nullptr : definitions.get_economy_manager().get_production_type_manager().get_good_to_rgo_production_type(*fish);
		const fixed_point_t multiplier = definitions.get_define_manager().get_openvic_defines().economy.fish_output_multiplier;
		r.check(wharf != nullptr && near(wharf->base_output_quantity, fixed_point_t(22) / 10 * multiplier, fixed_point_t(1) / 1000),
			"fish: the output of the fishing grounds is the 2.2 of the files times FISH_OUTPUT_MULTIPLIER (2.2 by default)");
		GoodDefinition const* const grain = definitions.get_economy_manager().get_good_definition_manager().get_good_definition_by_identifier("grain");
		ProductionType const* const farm = grain == nullptr ? nullptr : definitions.get_economy_manager().get_production_type_manager().get_good_to_rgo_production_type(*grain);
		const fixed_point_t grain_multiplier = definitions.get_define_manager().get_openvic_defines().economy.grain_output_multiplier;
		r.check(farm != nullptr && near(farm->base_output_quantity, fixed_point_t(18) / 10 * grain_multiplier, fixed_point_t(1) / 1000),
			"fish: the grain is the 1.8 of the files times GRAIN_OUTPUT_MULTIPLIER (1.1 by default)");
		GoodDefinition const* const iron = definitions.get_economy_manager().get_good_definition_manager().get_good_definition_by_identifier("iron");
		ProductionType const* const mine = iron == nullptr ? nullptr : definitions.get_economy_manager().get_production_type_manager().get_good_to_rgo_production_type(*iron);
		r.check(mine != nullptr && mine->base_output_quantity > fixed_point_t::_0, "fish: the other goods (iron) are not scaled");
	}

	// The growth comes once a month: the world total (the pops, with the men who died in the armies added back: they die every day) changes on the
	// first days of the months only
	void test_population_monthly(Results& r, InstanceManager& im) {
		const auto world = [&im]() -> std::int64_t {
			std::int64_t total = im.get_unit_instance_manager().men_killed_total;
			for (ProvinceInstance const& province : im.get_map_instance().get_province_instances()) {
				for (Pop const& pop : province.get_pops()) {
					total += type_safe::get(pop.get_size());
				}
			}
			return total;
		};
		std::int64_t before = world();
		std::size_t changes_off_month_start = 0, changes = 0;
		for (int day = 0; day < 70; ++day) {
			im.force_tick_and_update();
			const std::int64_t total = world();
			if (total != before) {
				changes++;
				const auto day_of_month = im.get_today().get_day();
				// the monthly update runs on the first day, the totals of the countries are worked out the day after
				changes_off_month_start += !(day_of_month == 1 || day_of_month == 2);
			}
			before = total;
		}
		r.check(changes >= 1 && changes_off_month_start == 0, "population growth: the population changes once a month, on the first days of the months only", fmt::format("{} changes, {} off the month start", changes, changes_off_month_start));
	}

	void test_army_groups(Results& r, InstanceManager& im) {
		OpenVicSupplyDefines& defines = const_cast<OpenVicSupplyDefines&>(im.definition_manager.get_define_manager().get_openvic_defines().supply);
		CountryInstance* country = nullptr;
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (candidate.exists() && candidate.has_army_command() && candidate.get_armies().size() >= 3
				&& candidate.get_leaders<unit_branch_t::LAND>().size() >= 3) {
				country = &candidate;
				break;
			}
		}
		r.check(country != nullptr, "army groups: a civilised country with three armies and three generals exists");
		if (country == nullptr) {
			return;
		}
		const bool auto_assign_before = country->get_auto_assign_leaders();
		country->set_auto_assign_leaders(false);
		// Two armies with a general each and a general that leads nobody (he becomes the marshal)
		ArmyInstance* first = &country->get_armies()[0].get();
		ArmyInstance* second = &country->get_armies()[1].get();
		for (ArmyInstance& army : country->get_armies()) {
			army.set_leader(nullptr);
		}
		LeaderInstance* lead_first = nullptr;
		LeaderInstance* lead_second = nullptr;
		LeaderInstance* marshal = nullptr;
		for (LeaderInstance& leader : country->get_leaders<unit_branch_t::LAND>()) {
			if (leader.get_unit_instance_group() != nullptr || leader.is_marshal() || !leader.get_can_be_used()) {
				continue;
			}
			if (lead_first == nullptr) {
				lead_first = &leader;
			} else if (lead_second == nullptr) {
				lead_second = &leader;
			} else if (marshal == nullptr) {
				marshal = &leader;
			}
		}
		r.check(marshal != nullptr, "army groups: three free generals exist");
		if (marshal == nullptr) {
			return;
		}
		first->set_leader(lead_first);
		second->set_leader(lead_second);

		// The group
		const std::size_t groups_before = country->get_army_groups().size();
		im.queue_game_action<create_army_group_argument_t>(country->index);
		im.force_tick_and_update();
		r.check(country->get_army_groups().size() == groups_before + 1, "army groups: a group can be made");
		const std::uint32_t group_id = country->get_army_groups().back().id;
		im.queue_game_action<set_army_group_argument_t>(first->unique_id, group_id);
		im.queue_game_action<set_army_group_argument_t>(second->unique_id, group_id);
		im.queue_game_action<set_army_group_argument_t>(second->unique_id, std::uint32_t(9999));
		im.force_tick_and_update();
		r.check(first->get_army_group_id() == group_id && second->get_army_group_id() == group_id, "army groups: armies can join a group, an unknown group is refused");

		// The field marshal: a general that leads nobody
		// A general that leads an army can be made the marshal: he leaves the army
		im.queue_game_action<set_army_group_marshal_argument_t>(country->index, group_id, lead_first->unique_id);
		im.force_tick_and_update();
		r.check(country->find_army_group(group_id)->marshal_id == lead_first->unique_id && first->get_leader() == nullptr && lead_first->is_marshal(),
			"army groups: a general that leads an army can become the marshal, he leaves the army");
		im.queue_game_action<set_army_group_marshal_argument_t>(country->index, group_id, std::uint32_t(0));
		im.force_tick_and_update();
		r.check(country->find_army_group(group_id)->marshal_id == 0 && !lead_first->is_marshal(), "army groups: the marshal can be taken off");
		first->set_leader(lead_first);
		im.queue_game_action<set_army_group_marshal_argument_t>(country->index, group_id, marshal->unique_id);
		im.force_tick_and_update();
		r.check(country->find_army_group(group_id)->marshal_id == marshal->unique_id && marshal->is_marshal(), "army groups: a free general becomes the marshal");
		im.queue_game_action<set_army_leader_argument_t>(second->unique_id, marshal->unique_id);
		im.force_tick_and_update();
		r.check(second->get_leader() == lead_second, "army groups: a marshal cannot lead an army");
		LeaderInstance* const spare = marshal;
		// The auto assignment does not take the marshal (the armies without a general get the other free generals)
		country->set_auto_assign_leaders(true);
		for (int day = 0; day < 3; ++day) {
			im.force_tick_and_update();
		}
		country->set_auto_assign_leaders(false);
		r.check(spare->get_unit_instance_group() == nullptr && spare->is_marshal(), "army groups: the marshal is not given to an army by the auto assignment");
		first->set_leader(lead_first);
		second->set_leader(lead_second);

		// The bonus: the prestige of the general (and the half of the one of the marshal for the army of the group); 1 percent a point of prestige
		defines.supply_leader_prestige_bonus.numerator = 100;
		play(im, 1);
		const fixed_point_t expected_first = lead_first->get_prestige() / 100 + marshal->get_prestige() / 100 / 2;
		const fixed_point_t expected_second = lead_second->get_prestige() / 100 + marshal->get_prestige() / 100 / 2;
		const auto near_enough = [](fixed_point_t a, fixed_point_t b) -> bool {
			return a - b < fixed_point_t(1) / 1000 && b - a < fixed_point_t(1) / 1000;
		};
		r.check(near_enough(first->get_supply_leader_bonus(), expected_first) && near_enough(second->get_supply_leader_bonus(), expected_second),
			"army groups: the army gets the bonus of its general and half of the bonus of the marshal of its group",
			fmt::format("{} / {} and {} / {}", first->get_supply_leader_bonus(), expected_first, second->get_supply_leader_bonus(), expected_second));
		// An army outside the group gets no bonus of the marshal
		ArmyInstance* outside = &country->get_armies()[2].get();
		r.check(near_enough(outside->get_supply_leader_bonus(), outside->get_leader() != nullptr ? outside->get_leader()->get_prestige() / 100 : fixed_point_t::_0),
			"army groups: an army outside the group gets no bonus of the marshal");
		defines.supply_leader_prestige_bonus.numerator = 0;
		play(im, 1);
		r.check(first->get_supply_leader_bonus() == fixed_point_t::_0, "army groups: without the prestige bonus and the traits the bonus of the generals is 0 (the balance is not touched)");

		// Deleting a group frees the marshal and the armies
		im.queue_game_action<delete_army_group_argument_t>(country->index, group_id);
		im.force_tick_and_update();
		r.check(first->get_army_group_id() == 0 && !marshal->is_marshal() && country->find_army_group(group_id) == nullptr,
			"army groups: deleting a group frees its armies and its marshal");
		country->set_auto_assign_leaders(auto_assign_before);

		// Without the technology
		CountryInstance* plain = nullptr;
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (candidate.exists() && !candidate.has_army_command() && candidate.has_armies()) {
				plain = &candidate;
				break;
			}
		}
		if (plain != nullptr) {
			im.queue_game_action<create_army_group_argument_t>(plain->index);
			im.force_tick_and_update();
			r.check(plain->get_army_groups().empty(), "army groups: a country without the technology cannot make groups");
		}
	}

	// A division of the standard template with at least four regiments in two ranks or more (the armies of Vic2 seldom make a whole one), nullptr if none
	std::pair<ArmyInstance*, DivisionInstance const*> find_full_division(InstanceManager& im, CountryInstance& country) {
		for (ArmyInstance& army : country.get_armies()) {
			if (army.is_in_combat() || army.is_moving() || army.get_transport_navy() != nullptr) {
				continue;
			}
			for (DivisionInstance const& division : army.get_divisions()) {
				std::size_t count = 0;
				for (RegimentInstance const& regiment : army.get_regiment_instances()) {
					count += regiment.get_division_id() == division.id;
				}
				const DivisionLayout layout = layout_division(
					im.definition_manager.get_military_manager().get_unit_type_manager(), country, army, division, 2
				);
				if (division.template_id == 1 && count >= 4 && layout.width >= 2 && layout.entries.size() == count) {
					return { &army, &division };
				}
			}
		}
		return { nullptr, nullptr };
	}

	// The live division: the cache of its numbers is computed again when what they depend on changes, a line that lost its regiment is taken by the next one
	void test_live_divisions(Results& r, InstanceManager& im) {
		WarManager& war_manager = im.get_war_manager();
		UnitTypeManager const& unit_types = im.definition_manager.get_military_manager().get_unit_type_manager();
		CountryInstance* country = im.get_country_instance_manager().get_country_instance_by_identifier("FRA");
		r.check(country != nullptr, "live division: France exists");
		if (country == nullptr) {
			return;
		}
		const std::pair<ArmyInstance*, DivisionInstance const*> found = find_full_division(im, *country);
		ArmyInstance* const army = found.first;
		DivisionInstance const* const division = found.second;
		r.check(army != nullptr, "live division: France has a division of the standard template with four regiments");
		if (army == nullptr) {
			return;
		}
		const std::uint32_t division_id = division->id;
		const auto stats = [&]() -> DivisionStats const& {
			DivisionInstance const* current = nullptr;
			for (DivisionInstance const& candidate : army->get_divisions()) {
				if (candidate.id == division_id) {
					current = &candidate;
				}
			}
			return war_manager.get_live_division_stats(im, *army, *current);
		};
		const DivisionStats full = stats();
		const DivisionStats template_stats = compute_division_stats(im, *country, *country->find_division_template(1));
		r.check(full.regiments >= 4 && full.width >= 2 && full.attack > fixed_point_t::_0 && full.strength > fixed_point_t::_0,
			"live division: the division has its regiments, its width, an attack and its men", fmt::format("{} (template {})", dbl(full.attack), dbl(template_stats.attack)));
		std::uint64_t computed = war_manager.get_live_stats_computed();
		(void) stats();
		r.check(war_manager.get_live_stats_computed() == computed, "live division: the numbers come from the cache when nothing changed");

		// The first line of the first rank loses its men: the second line takes its place with the weight of the first
		std::vector<RegimentInstance*> members;
		for (RegimentInstance& regiment : army->get_regiment_instances()) {
			if (regiment.get_division_id() == division_id) {
				members.push_back(&regiment);
			}
		}
		DivisionInstance const* current_division = nullptr;
		for (DivisionInstance const& candidate : army->get_divisions()) {
			if (candidate.id == division_id) {
				current_division = &candidate;
			}
		}
		const DivisionLayout before = layout_division(unit_types, *country, *army, *current_division, 2);
		RegimentInstance* first_line = nullptr;
		RegimentInstance* second_line = nullptr;
		for (DivisionLayout::Entry const& entry : before.entries) {
			RegimentInstance& regiment = army->get_regiment_instances()[entry.regiment];
			if (entry.rank == 0 && entry.line == 0) {
				first_line = &regiment;
			} else if (entry.rank == 0 && entry.line == 1) {
				second_line = &regiment;
			}
		}
		r.check(first_line != nullptr && second_line != nullptr, "live division: the first rank has two lines");
		if (first_line == nullptr || second_line == nullptr) {
			return;
		}
		const fixed_point_t first_strength = first_line->get_strength();
		first_line->take_damage(fixed_point_t::_0, first_line->get_strength());
		const DivisionLayout after = layout_division(unit_types, *country, *army, *current_division, 2);
		bool second_now_first = false;
		for (DivisionLayout::Entry const& entry : after.entries) {
			if (&army->get_regiment_instances()[entry.regiment].get() == second_line) {
				second_now_first = entry.line == 0;
			}
		}
		r.check(after.entries.size() + 1 == before.entries.size() && second_now_first, "live division: the regiment of the second line takes the place of the first one that lost its men (its weight is 1)");
		computed = war_manager.get_live_stats_computed();
		const DivisionStats damaged = stats();
		r.check(war_manager.get_live_stats_computed() == computed + 1 && damaged.attack < full.attack && damaged.regiments + 1 == full.regiments,
			"live division: the losses compute the numbers again", fmt::format("{} {}", dbl(damaged.attack), dbl(full.attack)));
		first_line->recover(fixed_point_t::_0, first_strength);
		computed = war_manager.get_live_stats_computed();
		r.check(near(stats().attack, full.attack, fixed_point_t(1) / 1000) && war_manager.get_live_stats_computed() == computed + 1,
			"live division: the reinforcement computes them again");
		// Half the men of a regiment: its cell is worth half
		second_line->take_damage(fixed_point_t::_0, second_line->get_strength() / 2);
		const DivisionStats half = stats();
		r.check(half.attack < full.attack && half.regiments == full.regiments, "live division: a regiment with half its men is worth less");
		second_line->recover(fixed_point_t::_0, second_line->get_max_strength());
		// The weapon and the general
		const fixed_point_t ratio_before = second_line->get_weapon_attack_ratio();
		computed = war_manager.get_live_stats_computed();
		second_line->set_weapon_attack_ratio(ratio_before * 2);
		const DivisionStats armed = stats();
		r.check(war_manager.get_live_stats_computed() == computed + 1 && armed.attack > full.attack, "live division: a new weapon computes the numbers again");
		second_line->set_weapon_attack_ratio(ratio_before);
		(void) stats();
		computed = war_manager.get_live_stats_computed();
		LeaderInstance* const leader_before = army->get_leader();
		LeaderInstance* other = nullptr;
		for (LeaderInstance& leader : country->get_leaders<unit_branch_t::LAND>()) {
			if (&leader != leader_before && leader.get_unit_instance_group() == nullptr && leader.get_can_be_used() && !leader.is_marshal()) {
				other = &leader;
			}
		}
		if (other != nullptr) {
			army->set_leader(other);
			(void) stats();
			r.check(war_manager.get_live_stats_computed() == computed + 1, "live division: a new general computes the numbers again");
			army->set_leader(leader_before);
		}
		// The next day (the modifiers and the technologies of the country are summed every day)
		(void) stats();
		computed = war_manager.get_live_stats_computed();
		im.force_tick_and_update();
		bool still_there = false;
		for (DivisionInstance const& candidate : army->get_divisions()) {
			still_there |= candidate.id == division_id;
		}
		if (still_there) {
			(void) stats();
			r.check(war_manager.get_live_stats_computed() == computed + 1, "live division: a new day (the modifiers and the technologies) computes the numbers again");
		}
	}

	// A battle set up by hand: France attacks Prussia on a plain; the width of the battle limits the divisions of the front, the others wait in the reserve
	void test_live_battle(Results& r, InstanceManager& im) {
		WarManager& war_manager = im.get_war_manager();
		CountryInstance* attacker = im.get_country_instance_manager().get_country_instance_by_identifier("FRA");
		CountryInstance* defender = im.get_country_instance_manager().get_country_instance_by_identifier("PRU");
		r.check(attacker != nullptr && defender != nullptr && war_manager.declare_war(im, *attacker, *defender), "live battle: France declares war on Prussia");
		if (attacker == nullptr || defender == nullptr || !attacker->is_at_war_with(*defender)) {
			return;
		}
		// A plain of Prussia with no army
		ProvinceInstance* field = nullptr;
		for (ProvinceInstance* province : defender->get_owned_provinces()) {
			if (province->get_controller() == defender && province->get_armies().empty() && province->get_terrain_type() != nullptr
				&& province->get_terrain_type()->get_defence_bonus() == fixed_point_t::_0
			&& province->get_terrain_type()->get_combat_width_percentage_change() == fixed_point_t::_0 && !province->province_definition.is_water()) {
				field = province;
				break;
			}
		}
		r.check(field != nullptr, "live battle: Prussia has a plain with no army");
		if (field == nullptr) {
			return;
		}
		// All the divisions of France in one army there, one division of Prussia
		std::vector<ArmyInstance*> french;
		for (ArmyInstance& army : attacker->get_armies()) {
			if (army.get_transport_navy() == nullptr && !army.is_in_combat()) {
				french.push_back(&army);
			}
		}
		std::size_t french_divisions = 0;
		for (ArmyInstance* army : french) {
			army->clear_path();
			army->set_location(*field);
			army->set_previous_location(nullptr);
			french_divisions += army->get_divisions().size();
		}
		const std::pair<ArmyInstance*, DivisionInstance const*> prussian_found = find_full_division(im, *defender);
		ArmyInstance* const prussian = prussian_found.first;
		DivisionInstance const* const prussian_division = prussian_found.second;
		r.check(prussian != nullptr && french_divisions > 6, "live battle: a division of Prussia and more than six divisions of France",
			fmt::format("{}", french_divisions));
		if (prussian == nullptr) {
			return;
		}
		ArmyInstance* defending = prussian;
		if (prussian->get_divisions().size() > 1) {
			defending = split_division(im.get_unit_instance_manager(), *prussian, prussian_division->id);
		}
		if (defending == nullptr) {
			return;
		}
		defending->clear_path();
		defending->set_location(*field);
		// The estimate (no change of the game): one French army, then all of them (the support of several armies)
		std::vector<ArmyInstance const*> one_army { french.front() };
		std::vector<ArmyInstance const*> all_armies { french.begin(), french.end() };
		const std::uint64_t battles_before = war_manager.get_land_battle_count();
		const BattleEstimate one_estimate = war_manager.estimate_battle(im, one_army, *field);
		const BattleEstimate estimate = war_manager.estimate_battle(im, all_armies, *field);
		r.check(estimate.valid && one_estimate.valid && estimate.attacker_attack >= one_estimate.attacker_attack && estimate.ratio >= one_estimate.ratio
				&& war_manager.get_land_battle_count() == battles_before,
			"live battle: the estimate counts the support of all the armies and changes nothing",
			fmt::format("R {} with one army, {} with all", dbl(one_estimate.ratio), dbl(estimate.ratio)));
		im.force_tick_and_update();
		LandBattleReport const* report = nullptr;
		for (LandBattleReport const& candidate : war_manager.get_battle_reports()) {
			if (candidate.province == field) {
				report = &candidate;
			}
		}
		r.check(report != nullptr, "live battle: the battle is fought");
		if (report == nullptr) {
			return;
		}
		OpenVicWarDefines const& war = im.definition_manager.get_define_manager().get_openvic_defines().war;
		r.check(report->attacker == attacker && report->defender == defender, "live battle: the country that came attacks, the controller of the province defends");
		r.check(report->width == static_cast<std::uint32_t>(war.battle_width_base) && report->attacker_ranks <= report->width && report->attacker_reserve > 0
				&& report->attacker_front + report->attacker_reserve == french_divisions,
			"live battle: the width of the battle (a plain: BATTLE_WIDTH_BASE ranks) limits the divisions of the front, the others wait in the reserve",
			fmt::format("width {} ranks {} front {} reserve {}", report->width, report->attacker_ranks, report->attacker_front, report->attacker_reserve));
		r.check(report->defender_front == 1 && report->defender_strength_lost > fixed_point_t::_0 && report->attacker_strength_lost > fixed_point_t::_0,
			"live battle: both sides lose men");
		r.check(report->ratio > fixed_point_t::_0, "live battle: R is counted", fmt::format("{}", dbl(report->ratio)));
		r.check(near(estimate.ratio, report->ratio, report->ratio / 5) && estimate.attacker_wins == (report->ended && report->attacker_won),
			"live battle: the estimate of the day before gives about the R of the battle and its winner (the supply of a day may differ)",
			fmt::format("estimate R {} wins {}, battle R {} won {}", dbl(estimate.ratio), estimate.attacker_wins, dbl(report->ratio), report->attacker_won));
		r.check(estimate.recon_advantage == report->recon_advantage && report->recon_advantage >= -fixed_point_t::_1 && report->recon_advantage <= fixed_point_t::_1,
			"live battle: the battle and the estimate see the same advantage of the reconnaissance (section 12)", fmt::format("{}", dbl(report->recon_advantage)));
		// The divisions of a new day: the front keeps its divisions while they stay
		std::uint32_t day = 1;
		while (day < 30) {
			bool going = false;
			for (LandBattleReport const& candidate : war_manager.get_battle_reports()) {
				going |= candidate.province == field && !candidate.ended;
			}
			if (!going) {
				break;
			}
			im.force_tick_and_update();
			day++;
		}
		bool prussian_left = true;
		for (ArmyInstance const& army : field->get_armies()) {
			prussian_left &= &army.get_country() != defender;
		}
		r.check(prussian_left, "live battle: the one division of Prussia leaves the field (retreat or destroyed)", fmt::format("{} days", day));
		for (CountryInstance* country : { attacker, defender }) {
			r.check(war_manager.get_battle_stats(*country).battle_days > 0, "live battle: the statistics count the days of the battle");
		}
	}


	// ---- the skills of the leaders (docs/WAR_ASSUMPTIONS.md, section 7): the levels by the experience, the battle teaches the generals (the terrain
	// half as fast), a new level gives the country prestige, the attack and the defence work in a battle, the field marshal learns the supply and the
	// planning in a war and they work; the role decides which skills work

	void test_leader_skills(Results& r, InstanceManager& im) {
		using namespace leader_skills;
		OpenVicWarDefines& war = const_cast<OpenVicWarDefines&>(im.definition_manager.get_define_manager().get_openvic_defines().war);
		const std::int32_t general_per_level = war.general_xp_per_level;
		const std::int32_t marshal_per_level = war.marshal_xp_per_level;
		r.check(level_of(fixed_point_t(general_per_level) - fixed_point_t::_1, general_per_level, 5) == 0
				&& level_of(fixed_point_t(general_per_level), general_per_level, 5) == 1
				&& level_of(fixed_point_t(general_per_level * 3), general_per_level, 5) == 2
				&& level_of(fixed_point_t(general_per_level * 15), general_per_level, 5) == 5
				&& level_of(fixed_point_t(general_per_level * 100), general_per_level, 5) == 5
				&& level_of(fixed_point_t(marshal_per_level * 6), marshal_per_level, 3) == 3,
			"leader skills: level n needs the experience of a level x (1 + ... + n), no more than the maximum");
		r.check(scale_terrain_effect(fixed_point_t(1) / 10, fixed_point_t(5) / 100, 2) == fixed_point_t(1) / 10 * (fixed_point_t(110) / 100)
				&& scale_terrain_effect(fixed_point_t(-2) / 10, fixed_point_t(5) / 100, 2) == fixed_point_t(-2) / 10 * (fixed_point_t(90) / 100)
				&& scale_terrain_effect(fixed_point_t(-2) / 10, fixed_point_t(5) / 100, 0) == fixed_point_t(-2) / 10,
			"leader skills: a level of a terrain makes its bonus 5 % bigger and its penalty 5 % smaller");

		// The battles of the war of France and Prussia (test_live_battle) taught the generals: the terrain half of the attack and the defence
		std::size_t taught = 0;
		bool halves = true;
		for (CountryInstance* country : { im.get_country_instance_manager().get_country_instance_by_identifier("FRA"),
				im.get_country_instance_manager().get_country_instance_by_identifier("PRU") }) {
			if (country == nullptr) {
				continue;
			}
			for (LeaderInstance const& leader : country->get_leaders<unit_branch_t::LAND>()) {
				fixed_point_t terrain_total = fixed_point_t::_0;
				for (auto const& [terrain, experience] : leader.get_terrain_experience()) {
					terrain_total += experience;
				}
				const fixed_point_t battle = leader.get_attack_experience() + leader.get_defence_experience();
				taught += battle > fixed_point_t::_0;
				halves &= near(terrain_total, war.general_terrain_xp_share.of(battle), fixed_point_t(1) / 100);
			}
		}
		r.check(taught > 0 && halves, "leader skills: the generals of a battle learn the attack or the defence, the terrain half of it",
			fmt::format("{} generals learned", taught));

		// A new level: the prestige of the country, the message; the levels of a marshal are worth more
		CountryInstance* france = im.get_country_instance_manager().get_country_instance_by_identifier("FRA");
		LeaderInstance* general = nullptr;
		ArmyInstance* general_army = nullptr;
		for (ArmyInstance& army : france->get_armies()) {
			if (army.get_leader() != nullptr && !army.get_leader()->is_marshal() && army.get_transport_navy() == nullptr && !army.get_divisions().empty()) {
				general = army.get_leader();
				general_army = &army;
				break;
			}
		}
		r.check(general != nullptr, "leader skills: France has an army with a general");
		if (general == nullptr) {
			return;
		}
		const fixed_point_t saved_attack = general->get_attack_experience();
		const fixed_point_t saved_defence = general->get_defence_experience();
		general->set_attack_experience(fixed_point_t::_0);
		const fixed_point_t prestige_before = france->get_prestige_untracked();
		const std::size_t messages_before = im.get_notification_log().get_entries().size();
		add_experience(im, *general, skill_t::ATTACK, experience_for_level(3, general_per_level));
		r.check(get_level(war, *general, skill_t::ATTACK) == 3 && france->get_prestige_untracked() == prestige_before + war.general_level_prestige * 6
				&& im.get_notification_log().get_entries().size() == messages_before + 1
				&& im.get_notification_log().get_entries().back().kind == "leader_skill_up",
			"leader skills: three levels at once give the country 1 + 2 + 3 prestige and a message",
			fmt::format("prestige {} -> {}", france->get_prestige_untracked() - prestige_before, france->get_prestige_untracked()));

		// The attack and the defence of the general in a battle: the estimate against a Prussian army
		CountryInstance* prussia = im.get_country_instance_manager().get_country_instance_by_identifier("PRU");
		ProvinceInstance const* target = nullptr;
		for (ArmyInstance const& army : prussia->get_armies()) {
			if (army.get_transport_navy() == nullptr && !army.empty() && !army.get_location().province_definition.is_water()) {
				target = &army.get_location();
				break;
			}
		}
		if (target != nullptr && france->is_at_war_with(*prussia)) {
			std::vector<ArmyInstance const*> attacking { general_army };
			general->set_attack_experience(fixed_point_t::_0);
			general->set_defence_experience(fixed_point_t::_0);
			const BattleEstimate plain = im.get_war_manager().estimate_battle(im, attacking, *target);
			general->set_attack_experience(experience_for_level(5, general_per_level));
			const BattleEstimate skilled = im.get_war_manager().estimate_battle(im, attacking, *target);
			r.check(plain.valid && skilled.valid && near(skilled.attacker_attack, plain.attacker_attack * fixed_point_t(125) / 100, plain.attacker_attack / 100),
				"leader skills: 5 levels of the attack give the divisions of the general 25 % more attack",
				fmt::format("{} -> {}", dbl(plain.attacker_attack), dbl(skilled.attacker_attack)));
			// The role: the same general made a marshal does not use the skills of a general
			general->set_marshal(true);
			r.check(get_active_level(war, general, skill_t::ATTACK) == 0 && get_level(war, *general, skill_t::ATTACK) == 5,
				"leader skills: a general that is a marshal keeps his levels of a general but does not use them");
			general->set_marshal(false);
		} else {
			fmt::println("TEST INFO leader skills: no Prussian army to estimate against");
		}
		general->set_attack_experience(saved_attack);
		general->set_defence_experience(saved_defence);

		// The field marshal: a war teaches him, his levels work on the planning and the supply
		CountryInstance* country = nullptr;
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (candidate.exists() && !candidate.is_rebel_country() && candidate.has_army_command() && candidate.is_at_war() && !candidate.get_armies().empty()) {
				country = &candidate;
				break;
			}
		}
		if (country == nullptr) {
			fmt::println("TEST INFO leader skills: no country with the command at war, the marshal not checked");
			return;
		}
		const std::size_t palette_size = im.definition_manager.get_military_manager().get_command_palette().size();
		create_army_group(*country, palette_size);
		const std::uint32_t group_id = country->get_army_groups().back().id;
		LeaderInstance* marshal = nullptr;
		for (LeaderInstance& leader : country->get_leaders<unit_branch_t::LAND>()) {
			if (!leader.is_marshal() && leader.get_can_be_used() && leader.get_unit_instance_group() == nullptr
				&& set_army_group_marshal(im, *country, group_id, leader.unique_id)) {
				marshal = &leader;
				break;
			}
		}
		if (marshal == nullptr) {
			fmt::println("TEST INFO leader skills: no free general for a field marshal, not checked");
			delete_army_group(im, *country, group_id);
			return;
		}
		ArmyInstance* army = nullptr;
		for (ArmyInstance& candidate : country->get_armies()) {
			if (candidate.get_army_group_id() == 0 && candidate.get_parent_army_id() == 0 && !candidate.empty()) {
				army = &candidate;
				break;
			}
		}
		if (army == nullptr || !set_army_group(*country, *army, group_id, get_marshal_max_attached(im, *country))) {
			fmt::println("TEST INFO leader skills: no army for the marshal, not checked");
			delete_army_group(im, *country, group_id);
			return;
		}
		const fixed_point_t supply_before = marshal->get_supply_experience();
		const fixed_point_t planning_before = marshal->get_planning_experience();
		update_army_groups(im, *country);
		r.check(marshal->get_supply_experience() == supply_before + war.marshal_xp_per_day && marshal->get_planning_experience() == planning_before,
			"leader skills: a marshal at war learns the supply while his group has armies, not the planning without an order");
		const fixed_point_t base_need = get_army_supply_need(im, *army);
		marshal->set_supply_experience(experience_for_level(2, marshal_per_level));
		const fixed_point_t skilled_need = get_army_supply_need(im, *army);
		r.check(near(skilled_need, base_need * fixed_point_t(90) / 100, base_need / 1000 + fixed_point_t(1) / 1000),
			"leader skills: 2 levels of the supply of the marshal: his armies need 10 % less", fmt::format("{} -> {}", dbl(base_need), dbl(skilled_need)));
		marshal->set_planning_experience(experience_for_level(3, marshal_per_level));
		update_army_groups(im, *country);
		ArmyGroup const* const group = country->find_army_group(group_id);
		r.check(group != nullptr && group->planning_effect == fixed_point_t::_1 + war.marshal_skill_step.fixed() * 3,
			"leader skills: 3 levels of the planning: the bonus is worth 15 % more");
		marshal->set_supply_experience(supply_before);
		marshal->set_planning_experience(planning_before);
		set_army_group(*country, *army, 0, get_marshal_max_attached(im, *country));
		delete_army_group(im, *country, group_id);
	}

	// ---- the armies cross the straits (the Bosporus, the Dardanelles...) like in Vic2; an army sent by hand stays off the orders of the marshal until the
	// player gives it back (a right click on the marshal: set_army_group to the same group, or a new order of the marshal)

	void test_straits_and_manual_orders(Results& r, InstanceManager& im) {
		MapInstance& map_instance = im.get_map_instance();
		UnitInstanceManager& unit_manager = im.get_unit_instance_manager();
		// A strait between two land provinces of one country that has an army
		bool crossed = false, found = false;
		for (ProvinceInstance& province : map_instance.get_province_instances()) {
			if (found || province.province_definition.is_water() || province.get_controller() == nullptr) {
				continue;
			}
			CountryInstance* const country = province.get_controller();
			for (ProvinceDefinition::adjacency_t const& adjacency : province.province_definition.get_adjacencies()) {
				if (adjacency.get_type() != ProvinceDefinition::adjacency_t::type_t::STRAIT || adjacency.get_to().is_water()) {
					continue;
				}
				ProvinceInstance& other = map_instance.get_province_instance_by_definition(adjacency.get_to());
				if (other.get_controller() != country) {
					continue;
				}
				ArmyInstance* army = nullptr;
				for (ArmyInstance& candidate : country->get_armies()) {
					if (!candidate.empty() && candidate.get_transport_navy() == nullptr && !candidate.is_moving() && !candidate.is_in_combat()) {
						army = &candidate;
						break;
					}
				}
				if (army == nullptr) {
					continue;
				}
				found = true;
				ProvinceInstance& home = army->get_location();
				army->set_location(province);
				crossed = unit_manager.order_move(*army, other, map_instance) && army->get_path().size() >= 1
					&& &army->get_path().back().get() == &other;
				fmt::println("TEST INFO straits: {} from {} to {}, path {}", country->get_identifier(), province.get_identifier(), other.get_identifier(), army->get_path().size());
				army->clear_path();
				army->set_location(home);
				break;
			}
		}
		r.check(found && crossed, "straits: an army goes over a strait between two provinces of its country");

		// The manual order
		CountryInstance* country = nullptr;
		CountryInstance* neighbour = nullptr;
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (!candidate.exists() || candidate.is_rebel_country() || !candidate.has_army_command() || candidate.get_armies().empty()) {
				continue;
			}
			for (ProvinceInstance* province : candidate.get_owned_provinces()) {
				for (ProvinceDefinition::adjacency_t const& adjacency : province->province_definition.get_adjacencies()) {
					CountryInstance* const other = map_instance.get_province_instance_by_definition(adjacency.get_to()).get_controller();
					if (neighbour == nullptr && adjacency.get_type() == ProvinceDefinition::adjacency_t::type_t::LAND && other != nullptr && other != &candidate) {
						neighbour = other;
					}
				}
			}
			if (neighbour != nullptr) {
				country = &candidate;
				break;
			}
		}
		if (country == nullptr) {
			fmt::println("TEST INFO manual orders: no country with the command and a neighbour, not checked");
			return;
		}
		const std::size_t palette_size = im.definition_manager.get_military_manager().get_command_palette().size();
		create_army_group(*country, palette_size);
		const std::uint32_t group_id = country->get_army_groups().back().id;
		ArmyInstance* army = nullptr;
		for (ArmyInstance& candidate : country->get_armies()) {
			if (candidate.get_army_group_id() == 0 && candidate.get_parent_army_id() == 0 && !candidate.empty() && candidate.get_transport_navy() == nullptr) {
				army = &candidate;
				break;
			}
		}
		if (army == nullptr || !set_army_group(*country, *army, group_id, get_marshal_max_attached(im, *country))) {
			fmt::println("TEST INFO manual orders: no army for the group, not checked");
			delete_army_group(im, *country, group_id);
			return;
		}
		const unique_id_t army_id = army->unique_id;
		const auto find_army_by_id = [](CountryInstance& owner, const unique_id_t id) -> ArmyInstance* {
			for (ArmyInstance& candidate : owner.get_armies()) {
				if (candidate.unique_id == id) {
					return &candidate;
				}
			}
			return nullptr;
		};
		WarManager& war_manager = im.get_war_manager();
		const std::uint32_t front_id = war_manager.create_border_front(im, *country, group_id, *neighbour);
		const auto on_front = [&]() -> bool {
			Front const* const front = war_manager.get_front_by_id(front_id);
			return front != nullptr && std::find(front->army_ids.begin(), front->army_ids.end(), army_id) != front->army_ids.end();
		};
		// Sent by hand and stopped: it stays off the front
		army->clear_path();
		army->set_manual_order(true);
		im.force_tick_and_update();
		army = find_army_by_id(*country, army_id);
		r.check(army != nullptr && army->is_manual_order() && !on_front(), "manual orders: an army sent by hand that stopped stays off the front of its marshal");
		if (army == nullptr) {
			return;
		}
		// The right click on the marshal (the same group again): back to the orders
		r.check(set_army_group(*country, *army, group_id, get_marshal_max_attached(im, *country)) && !army->is_manual_order(),
			"manual orders: attached to its marshal again it goes back to his orders");
		im.force_tick_and_update();
		r.check(on_front(), "manual orders: back on the front of the marshal");
		// A new order of the marshal (the game action) takes it back too
		army = find_army_by_id(*country, army_id);
		if (army == nullptr) {
			return;
		}
		army->clear_path();
		army->set_manual_order(true);
		im.force_tick_and_update();
		im.queue_game_action<create_border_front_argument_t>(country->index, group_id, neighbour->index);
		im.force_tick_and_update();
		army = find_army_by_id(*country, army_id);
		r.check(army != nullptr && !army->is_manual_order(), "manual orders: a new order of the marshal takes back the armies sent by hand");
		war_manager.delete_group_lines(im, *country, group_id);
		delete_army_group(im, *country, group_id);
	}

	// ---- the recruitment of a division by a template (docs/RECRUITMENT.md): the manpower of the pops of soldiers (a regiment is POP_SIZE_PER_REGIMENT
	// men), all the regiments of the template at once, one division of the template when they are ready, the cancel of the whole division

	// The manpower in men (docs/RECRUITMENT.md): the losses die (the pops of soldiers shrink), an army over the maximum fights weaker and gets no
	// new men, with free men again it does
	void test_manpower_losses(Results& r, InstanceManager& im) {
		UnitInstanceManager& unit_manager = im.get_unit_instance_manager();
		CountryInstance* country = nullptr;
		RegimentInstance* regiment = nullptr;
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (!candidate.exists() || candidate.is_rebel_country() || candidate.is_at_war() || unit_manager.get_manpower(candidate).free_men <= 3000) {
				continue;
			}
			for (ArmyInstance& army : candidate.get_armies()) {
				ProvinceInstance const& location = army.get_location();
				if (location.get_owner() != &candidate || army.is_in_combat() || army.get_transport_navy() != nullptr) {
					continue;
				}
				for (RegimentInstance& candidate_regiment : army.get_regiment_instances()) {
					if (candidate_regiment.get_pop() != nullptr && candidate_regiment.get_strength() >= candidate_regiment.get_max_strength()) {
						regiment = &candidate_regiment;
						break;
					}
				}
				if (regiment != nullptr) {
					break;
				}
			}
			if (regiment != nullptr) {
				country = &candidate;
				break;
			}
		}
		if (country == nullptr) {
			fmt::println("TEST INFO manpower: no country at peace with free men and a full regiment at home, not checked");
			return;
		}
		const Manpower before = unit_manager.get_manpower(*country);
		const std::int64_t pop_before = type_safe::get(regiment->get_pop()->get_size());
		regiment->take_damage(fixed_point_t::_0, fixed_point_t::_1);
		unit_manager.apply_losses(*regiment, *country);
		const Manpower after = unit_manager.get_manpower(*country);
		r.check(type_safe::get(regiment->get_pop()->get_size()) == pop_before - 1000 && after.pop_men == before.pop_men - 1000
				&& after.army_men == before.army_men - 1000 && before.max_men - after.max_men >= 1000 && before.max_men - after.max_men <= 1200,
			"manpower: a strength point lost is 1000 men who die (the pop of the regiment and the maximum shrink with its bonus)",
			fmt::format("pop {} -> {}, max {} -> {}, army {} -> {}", pop_before, type_safe::get(regiment->get_pop()->get_size()), before.max_men,
				after.max_men, before.army_men, after.army_men));

		// The pops of soldiers lose 40 % of the army (put back at the end): the army is over the maximum
		std::vector<std::pair<Pop*, std::int32_t>> taken;
		std::int64_t to_take = after.pop_men - after.army_men * 6 / 10;
		for (ProvinceInstance* province : country->get_owned_provinces()) {
			for (Pop const& pop : province->get_pops()) {
				if (to_take <= 0 || !pop.get_type().can_be_recruited) {
					continue;
				}
				Pop* const mutable_pop = province->find_pop_by_id(pop.id_in_province);
				const std::int32_t take = static_cast<std::int32_t>(std::min<std::int64_t>(to_take, type_safe::get(pop.get_size()) - 1));
				if (take > 0) {
					mutable_pop->change_size(-take, 0);
					taken.emplace_back(mutable_pop, take);
					to_take -= take;
				}
			}
		}
		const Manpower over = unit_manager.get_manpower(*country);
		OpenVicWarDefines const& wd = im.definition_manager.get_define_manager().get_openvic_defines().war;
		const fixed_point_t expected = std::max(wd.over_manpower_min_mult.fixed(), fixed_point_t::_1 - wd.over_manpower_penalty.of(over.over_share));
		r.check(over.over_share > fixed_point_t::_0 && over.combat_multiplier == expected && over.combat_multiplier < fixed_point_t::_1,
			"manpower: an army over the maximum fights with the attack and the defence times 1 - the share over it",
			fmt::format("over {:.3f}, multiplier {:.3f}", static_cast<double>(over.over_share), static_cast<double>(over.combat_multiplier)));
		for (GoodDefinition const& good : im.definition_manager.get_economy_manager().get_good_definition_manager().get_good_definitions()) {
			country->get_good_data(good).stockpile_amount += 1000;
		}
		// The AI may disband its weakest regiments over the maximum on the first day of a month: the regiment is looked up by its id
		const unique_id_t regiment_id = regiment->unique_id;
		const auto strength_now = [&unit_manager, regiment_id]() -> fixed_point_t {
			UnitInstance* const unit = unit_manager.get_unit_instance_by_unique_id(regiment_id);
			return unit != nullptr ? unit->get_strength() : fixed_point_t(-1);
		};
		const fixed_point_t strength_before = strength_now();
		im.force_tick_and_update();
		const fixed_point_t strength_over = strength_now();
		r.check(country->manpower_combat_multiplier < fixed_point_t::_1 && strength_over <= strength_before,
			"manpower: over the maximum the penalty is on the country and the regiment gets no new men",
			fmt::format("multiplier {:.3f}, strength {:.3f} -> {:.3f}", static_cast<double>(country->manpower_combat_multiplier),
				static_cast<double>(strength_before), static_cast<double>(strength_over)));
		for (auto const& [pop, take] : taken) {
			pop->change_size(take, 0);
		}
		im.force_tick_and_update();
		im.force_tick_and_update();
		const fixed_point_t strength_after = strength_now();
		if (strength_over < fixed_point_t::_0 || strength_after < fixed_point_t::_0) {
			fmt::println("TEST INFO manpower: the regiment was disbanded by the AI, the new men are not checked");
			return;
		}
		r.check(country->manpower_combat_multiplier == fixed_point_t::_1 && strength_after > strength_over,
			"manpower: with free men again the penalty is gone and the regiment gets new men",
			fmt::format("multiplier {:.3f}, strength {:.3f} -> {:.3f}", static_cast<double>(country->manpower_combat_multiplier),
				static_cast<double>(strength_over), static_cast<double>(strength_after)));
	}

	// The mobilisation pool: the land that is not national gives a hundredth of the share, no region gives more than its cap (docs/RECRUITMENT.md)
	void test_mobilisation_regions(Results& r, InstanceManager& im) {
		UnitInstanceManager const& unit_manager = im.get_unit_instance_manager();
		OpenVicWarDefines const& wd = im.definition_manager.get_define_manager().get_openvic_defines().war;
		std::size_t countries = 0, over_cap = 0, non_national_pops = 0, non_national_too_big = 0;
		const fixed_point_t share = fixed_point_t(1) / 10;
		for (CountryInstance const& country : im.get_country_instance_manager().get_country_instances()) {
			if (!country.exists() || country.is_rebel_country() || country.get_owned_provinces().empty()) {
				continue;
			}
			++countries;
			std::vector<std::pair<State const*, std::int64_t>> regions;
			for (auto const& [pop, men] : unit_manager.get_mobilisation_men(country, share)) {
				ProvinceInstance const& province = pop->get_location();
				auto it = std::find_if(regions.begin(), regions.end(), [&province](auto const& entry) -> bool { return entry.first == province.get_state(); });
				if (it == regions.end()) {
					regions.emplace_back(province.get_state(), men);
				} else {
					it->second += men;
				}
				if (!province.is_owner_core()) {
					++non_national_pops;
					non_national_too_big += men > type_safe::get(pop->get_size()) / 1000 + 1; // the share 0.1 times the hundredth
				}
			}
			for (auto const& [state, men] : regions) {
				std::int64_t cap = 0;
				if (state != nullptr) {
					for (ProvinceInstance const& province : state->get_provinces()) {
						if (province.get_owner() == &country && province.get_controller() == &country) {
							cap += (province.is_owner_core() ? wd.mobilisation_region_max : wd.mobilisation_region_max_non_national)
								.of_int(static_cast<std::int64_t>(type_safe::get(province.get_total_population())));
						}
					}
				}
				over_cap += state != nullptr && men > cap + 1;
			}
		}
		r.check(countries > 0 && over_cap == 0 && non_national_too_big == 0,
			"mobilisation: the land that is not national gives a hundredth, no region gives more than its cap",
			fmt::format("{} countries, {} regions over the cap, {} of {} pops of the land that is not national give too many", countries, over_cap,
				non_national_too_big, non_national_pops));
	}

	// The allies in a war that goes on and the separate peace (docs/WAR_ASSUMPTIONS.md, "Союзники и отдельный мир")
	void test_allies_and_separate_peace(Results& r, InstanceManager& im) {
		WarManager& war_manager = im.get_war_manager();
		CountryRelationManager& relations = im.get_country_relation_manager();
		std::vector<CountryInstance*> free;
		for (CountryInstance& country : im.get_country_instance_manager().get_country_instances()) {
			if (country.exists() && !country.is_rebel_country() && !country.is_at_war() && !country.get_owned_provinces().empty()) {
				free.push_back(&country);
			}
		}
		CountryInstance* attacker = nullptr;
		CountryInstance* defender = nullptr;
		CountryInstance* ally = nullptr;
		for (std::size_t i = 0; i + 2 < free.size() && ally == nullptr; ++i) {
			for (std::size_t j = i + 1; j < free.size() && ally == nullptr; ++j) {
				if (free[i]->has_alliance_with(*free[j]) || war_manager.has_truce(*free[i], *free[j], im.get_today())) {
					continue;
				}
				for (std::size_t k = 0; k < free.size(); ++k) {
					if (k != i && k != j && !free[k]->has_alliance_with(*free[j]) && !free[k]->has_alliance_with(*free[i])
						&& !war_manager.has_truce(*free[k], *free[j], im.get_today())) {
						attacker = free[i];
						defender = free[j];
						ally = free[k];
						break;
					}
				}
			}
		}
		if (ally == nullptr) {
			fmt::println("TEST INFO allies: no three free countries, not checked");
			return;
		}
		// The justification: a casus belli and infamy at its end, the war with it uses it up (docs/WAR_ASSUMPTIONS.md, "Обоснование войны")
		OpenVicWarDefines const& wd = im.definition_manager.get_define_manager().get_openvic_defines().war;
		const bool started = war_manager.start_justification(*attacker, *defender, im.get_today());
		r.check(started && attacker->justify_target == defender && attacker->justify_days_left == wd.justify_war_days
				&& !war_manager.get_justify_block_reason(*attacker, *ally, im.get_today()).empty(),
			"justification: it starts against a country, one at a time");
		const fixed_point_t infamy_before = attacker->get_infamy_untracked();
		attacker->justify_days_left = 1;
		im.force_tick_and_update();
		const bool has_cb = attacker->has_casus_belli_against(*defender, im.get_today());
		r.check(has_cb && attacker->justify_target != defender && attacker->get_infamy_untracked() >= infamy_before + wd.justify_war_infamy - fixed_point_t(1) / 2,
			"justification: at its end the country has a casus belli and the infamy of it",
			fmt::format("infamy {:.2f} -> {:.2f}", static_cast<double>(infamy_before), static_cast<double>(attacker->get_infamy_untracked())));
		const fixed_point_t exhaustion_before = attacker->get_war_exhaustion();
		if (attacker->is_at_war() || defender->is_at_war() || ally->is_at_war() || !war_manager.declare_war_as_country(im, *attacker, *defender, WarGoal {})) {
			fmt::println("TEST INFO allies: the countries went to war meanwhile, not checked");
			return;
		}
		r.check(!attacker->has_casus_belli_against(*defender, im.get_today()) && attacker->get_war_exhaustion() == exhaustion_before,
			"justification: a war with the casus belli uses it up and gives no war exhaustion");
		War* war = war_manager.find_war_between(*attacker, *defender);
		const std::uint32_t war_id = war != nullptr ? war->id : 0;
		relations.set_country_alliance(attacker, ally, true);
		const bool joined = war != nullptr && war_manager.join_war(im, war_id, *ally, true);
		r.check(joined && ally->is_at_war_with(*defender) && war_manager.get_war_by_id(war_id)->is_attacker(*ally),
			"allies: a called ally joins the war that goes on, on the side of the caller");
		const std::string_view leader_reason = war_manager.get_separate_peace_block_reason(im, *war_manager.get_war_by_id(war_id), *attacker);
		const std::string_view ally_reason = war_manager.get_separate_peace_block_reason(im, *war_manager.get_war_by_id(war_id), *ally);
		const bool left = ally_reason.empty() && war_manager.separate_peace(im, war_id, *ally);
		War const* const after = war_manager.get_war_by_id(war_id);
		r.check(!leader_reason.empty() && left && !ally->is_at_war_with(*defender) && after != nullptr && !after->has_participant(*ally)
				&& attacker->is_at_war_with(*defender) && war_manager.has_truce(*ally, *defender, im.get_today()),
			"allies: an ally that does not lead its side leaves the war with a separate peace (the leader cannot), the war goes on",
			fmt::format("leader: \"{}\", ally: \"{}\"", leader_reason, ally_reason));
		war_manager.end_war(im, war_id);
		relations.set_country_alliance(attacker, ally, false);
	}

	// The orders of the armies (docs/WAR_ASSUMPTIONS.md, section 10): an entrenched army digs in faster, an army follows another one, to the port
	void test_army_orders(Results& r, InstanceManager& im) {
		UnitInstanceManager& unit_manager = im.get_unit_instance_manager();
		// The second army must be reachable over the land of the country (not on an island or in a colony across the sea)
		const auto land_connected = [&im](ProvinceInstance const& from, ProvinceInstance const& to, CountryInstance const& owner) -> bool {
			std::vector<std::uint8_t> seen(im.get_map_instance().get_province_instances().size(), 0);
			std::deque<ProvinceInstance const*> queue { &from };
			seen[type_safe::get(from.index)] = 1;
			while (!queue.empty()) {
				ProvinceInstance const* const current = queue.front();
				queue.pop_front();
				if (current == &to) {
					return true;
				}
				for (ProvinceDefinition::adjacency_t const& adjacency : current->province_definition.get_adjacencies()) {
					if (adjacency.get_type() != ProvinceDefinition::adjacency_t::type_t::LAND || adjacency.get_to().is_water()) {
						continue;
					}
					ProvinceInstance const& next = im.get_map_instance().get_province_instance_by_definition(adjacency.get_to());
					if (seen[type_safe::get(next.index)] == 0 && next.get_owner() == &owner) {
						seen[type_safe::get(next.index)] = 1;
						queue.push_back(&next);
					}
				}
			}
			return false;
		};
		CountryInstance* country = nullptr;
		ArmyInstance* first = nullptr;
		ArmyInstance* second = nullptr;
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (!candidate.exists() || candidate.is_rebel_country() || candidate.is_at_war()) {
				continue;
			}
			first = second = nullptr;
			for (ArmyInstance& army : candidate.get_armies()) {
				if (army.empty() || army.is_moving() || army.is_in_combat() || army.get_transport_navy() != nullptr || army.is_at_sea()
					|| army.get_location().get_owner() != &candidate) {
					continue;
				}
				if (first == nullptr) {
					first = &army;
				} else if (&army.get_location() != &first->get_location() && land_connected(first->get_location(), army.get_location(), candidate)) {
					second = &army;
					break;
				}
			}
			if (second != nullptr) {
				country = &candidate;
				break;
			}
		}
		if (country == nullptr) {
			fmt::println("TEST INFO army orders: no country with two armies at home in two provinces, not checked");
			return;
		}
		const std::int32_t days_per_level = static_cast<std::int32_t>(
			im.definition_manager.get_define_manager().get_military_defines().get_dig_in_increase_each_days().to_int()
		);
		first->set_stance(ArmyInstance::STANCE_ENTRENCH);
		first->reset_dig_in();
		const unique_id_t first_id = first->unique_id;
		const unique_id_t second_id = second->unique_id;
		const std::int32_t days = std::max(1, days_per_level / 2);
		for (std::int32_t day = 0; day < days; ++day) {
			im.force_tick_and_update();
		}
		first = static_cast<ArmyInstance*>(unit_manager.get_unit_instance_group_by_unique_id(first_id));
		second = static_cast<ArmyInstance*>(unit_manager.get_unit_instance_group_by_unique_id(second_id));
		if (first == nullptr || second == nullptr) {
			fmt::println("TEST INFO army orders: the armies are gone, not checked");
			return;
		}
		r.check(first->get_dig_in_level() >= 1 || country->get_dig_in_cap() + 1 <= 0,
			"army orders: an entrenched army digs in twice as fast",
			fmt::format("{} days, level {}, days a level {}", days, first->get_dig_in_level(), days_per_level));
		first->set_stance(ArmyInstance::STANCE_NORMAL);
		first->set_follow_army_id(second->unique_id);
		unit_manager.update_follows(im.get_map_instance());
		r.check(first->is_moving() && &first->get_path().back().get() == &second->get_location(),
			"army orders: an army that follows another goes where it stands");
		first->set_follow_army_id(0);
		first->clear_path();
		ProvinceInstance* const port = unit_manager.find_nearest_army_port(*first, im.get_map_instance());
		r.check(port == nullptr || (port->get_owner() == country && port->province_definition.has_port()),
			"army orders: the nearest port of an army is a port of its country");
	}

	// The canned food the stockpile does not have is replaced by grain, twice as much (docs/MILITARY_SUPPLY.md)
	void test_canned_food_grain(Results& r, InstanceManager& im) {
		GoodDefinitionManager const& goods = im.definition_manager.get_economy_manager().get_good_definition_manager();
		GoodDefinition const* const canned_food = goods.get_good_definition_by_identifier("canned_food");
		GoodDefinition const* const grain = goods.get_good_definition_by_identifier("grain");
		RegimentType const* type = im.definition_manager.get_military_manager().get_unit_type_manager().get_regiment_type_by_identifier("skirmisher_infantry");
		CountryInstance* country = nullptr;
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (candidate.exists() && !candidate.is_rebel_country() && !candidate.is_at_war() && candidate.get_capital() != nullptr && !candidate.get_armies().empty()) {
				country = &candidate;
				break;
			}
		}
		if (canned_food == nullptr || grain == nullptr || type == nullptr || country == nullptr) {
			fmt::println("TEST INFO canned food and grain: no goods, unit type or country, not checked");
			return;
		}
		UnitInstanceManager& unit_manager = im.get_unit_instance_manager();
		ArmyInstance* const army = unit_manager.create_army_with_regiments(*country, *country->get_capital(), *type, 4, "canned food test");
		if (army == nullptr) {
			fmt::println("TEST INFO canned food and grain: no army made, not checked");
			return;
		}
		const unique_id_t army_id = army->unique_id;
		im.force_tick_and_update(); // the needs of the new regiments are known
		country->get_good_data(*canned_food).stockpile_amount = fixed_point_t::_0;
		const fixed_point_t cans_due = country->get_good_data(*canned_food).army_due;
		country->get_good_data(*grain).stockpile_amount += fixed_point_t(1000);
		const fixed_point_t grain_before = country->get_good_data(*grain).stockpile_amount;
		const fixed_point_t grain_due = country->get_good_data(*grain).army_due + country->get_good_data(*grain).navy_due;
		im.force_tick_and_update();
		const fixed_point_t from_grain = country->get_canned_food_from_grain_yesterday();
		const fixed_point_t grain_used = grain_before - country->get_good_data(*grain).stockpile_amount;
		r.check(cans_due > fixed_point_t::_0 && from_grain > fixed_point_t::_0 && grain_used >= from_grain * 2 - fixed_point_t::_0_01,
			"canned food and grain: without canned food the stockpile gives grain, two for a can",
			fmt::format("cans due {:.3f}, cans from grain {:.3f}, grain used {:.3f} (its own need {:.3f})", static_cast<double>(cans_due),
				static_cast<double>(from_grain), static_cast<double>(grain_used), static_cast<double>(grain_due)));

		// The rifles with the post-Napoleonic tactics (supply:rifle_1_first): rifle_1 goes first, for the regiments with muskets too; what the regiments
		// armed with rifle_1 lack is taken as rifle_0, and they lose a share of their attack
		GoodDefinition const* const musket = goods.get_good_definition_by_identifier("rifle_0");
		GoodDefinition const* const rifle = goods.get_good_definition_by_identifier("rifle_1");
		RegimentType const* const rifle_type =
			im.definition_manager.get_military_manager().get_unit_type_manager().get_regiment_type_by_identifier("infantry");
		if (musket != nullptr && rifle != nullptr) {
			const bool had_unlock = country->has_unlock(UNLOCK_SUPPLY_RIFLE_1_FIRST);
			country->revoke_unlock(UNLOCK_SUPPLY_RIFLE_1_FIRST);
			country->get_good_data(*musket).stockpile_amount = fixed_point_t(1000);
			country->get_good_data(*rifle).stockpile_amount = fixed_point_t(1000);
			im.force_tick_and_update();
			const fixed_point_t without_unlock = country->get_rifle_1_for_rifle_0_yesterday();
			country->grant_unlock(UNLOCK_SUPPLY_RIFLE_1_FIRST);
			country->get_good_data(*musket).stockpile_amount = fixed_point_t(1000);
			country->get_good_data(*rifle).stockpile_amount = fixed_point_t(1000);
			im.force_tick_and_update();
			const fixed_point_t with_unlock = country->get_rifle_1_for_rifle_0_yesterday();
			const fixed_point_t muskets_due = country->get_good_data(*musket).army_due;
			r.check(without_unlock == fixed_point_t::_0 && with_unlock > fixed_point_t::_0 && with_unlock <= muskets_due + fixed_point_t::_0_01,
				"muskets and rifles: with supply:rifle_1_first the regiments with muskets get rifle_1 first, even with rifle_0 in the stockpile",
				fmt::format("without {:.4f}, with {:.4f}, muskets due {:.4f}", static_cast<double>(without_unlock), static_cast<double>(with_unlock),
					static_cast<double>(muskets_due)));

			ArmyInstance* const rifle_army = rifle_type != nullptr
				? unit_manager.create_army_with_regiments(*country, *country->get_capital(), *rifle_type, 2, "rifle test") : nullptr;
			if (rifle_army != nullptr) {
				const unique_id_t rifle_army_id = rifle_army->unique_id;
				country->get_good_data(*rifle).stockpile_amount = fixed_point_t(1000);
				im.force_tick_and_update();
				ArmyInstance* army_now = static_cast<ArmyInstance*>(unit_manager.get_unit_instance_group_by_unique_id(rifle_army_id));
				const fixed_point_t ratio_with_rifles = army_now != nullptr && !army_now->get_regiment_instances().empty()
					? army_now->get_regiment_instances().front().get().get_weapon_attack_ratio() : fixed_point_t::_0;
				country->get_good_data(*rifle).stockpile_amount = fixed_point_t::_0;
				country->get_good_data(*musket).stockpile_amount = fixed_point_t(1000);
				im.force_tick_and_update();
				army_now = static_cast<ArmyInstance*>(unit_manager.get_unit_instance_group_by_unique_id(rifle_army_id));
				const fixed_point_t ratio_with_muskets = army_now != nullptr && !army_now->get_regiment_instances().empty()
					? army_now->get_regiment_instances().front().get().get_weapon_attack_ratio() : fixed_point_t::_0;
				const fixed_point_t muskets_for_rifles = country->get_rifle_0_for_rifle_1_yesterday();
				r.check(muskets_for_rifles > fixed_point_t::_0 && ratio_with_muskets < ratio_with_rifles
						&& ratio_with_muskets >= ratio_with_rifles * fixed_point_t::_0_50,
					"muskets and rifles: without rifle_1 the regiments armed with it take rifle_0 and lose a share of their attack",
					fmt::format("rifle_0 for rifle_1 {:.4f}, attack ratio {:.4f} -> {:.4f}", static_cast<double>(muskets_for_rifles),
						static_cast<double>(ratio_with_rifles), static_cast<double>(ratio_with_muskets)));
				if (army_now != nullptr) {
					unit_manager.destroy_group(*army_now);
				}
			}
			if (!had_unlock) {
				country->revoke_unlock(UNLOCK_SUPPLY_RIFLE_1_FIRST);
			}
		}
		if (UnitInstanceGroup* const made = unit_manager.get_unit_instance_group_by_unique_id(army_id)) {
			unit_manager.destroy_group(*made);
		}
	}

	void test_division_recruitment(Results& r, InstanceManager& im) {
		UnitInstanceManager& unit_manager = im.get_unit_instance_manager();
		UnitTypeManager const& unit_type_manager = im.definition_manager.get_military_manager().get_unit_type_manager();
		CountryInstance* country = nullptr;
		ProvinceInstance* province = nullptr;
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (!candidate.exists() || candidate.is_rebel_country() || candidate.get_division_templates().empty() || candidate.is_at_war()
				|| unit_manager.get_manpower(candidate).available_regiments < 12 || candidate.get_capital() == nullptr) {
				continue;
			}
			country = &candidate;
			province = candidate.get_capital();
			break;
		}
		if (country == nullptr) {
			fmt::println("TEST INFO division recruitment: no country with a template and 12 free regiments, not checked");
			return;
		}
		DivisionTemplate const& division_template = country->get_division_templates().front();
		const std::uint32_t regiments = division_template.get_total_slots();
		const Manpower before = unit_manager.get_manpower(*country);
		r.check(before.men_per_regiment == 3000 && before.available_regiments <= before.max_regiments,
			"division recruitment: the manpower counts the regiments of the pops of soldiers, 3000 men a regiment",
			fmt::format("{} of {} regiments", before.available_regiments, before.max_regiments));
		// The goods of the regiments in the stockpile
		for (const std::uint8_t cell : division_template.cells) {
			if (RegimentType const* const type = cell != 0 ? unit_type_manager.get_regiment_type_by_index(regiment_type_index_t(cell - 1)) : nullptr) {
				for (auto const& [good, quantity] : type->get_build_cost()) {
					country->get_good_data(*good).stockpile_amount += quantity;
				}
			}
		}
		const std::string reason = unit_manager.get_division_recruitment_block_reason(*country, unit_type_manager, division_template, *province);
		const std::size_t orders_before = unit_manager.get_recruitment_orders().size();
		const bool started = reason.empty() && unit_manager.start_division_recruitment(*country, unit_type_manager, division_template, *province);
		r.check(started && unit_manager.get_recruitment_orders().size() == orders_before + regiments
				&& unit_manager.get_manpower(*country).available_regiments == before.available_regiments - regiments,
			"division recruitment: all the regiments of the template are ordered and take the manpower", reason);
		if (!started) {
			return;
		}
		const std::uint32_t division_id = unit_manager.get_recruitment_orders().back().division_id;
		std::int32_t days = 0;
		for (RecruitmentOrder const& order : unit_manager.get_recruitment_orders()) {
			days = std::max(days, order.division_id == division_id ? order.days_left : 0);
		}
		for (std::int32_t day = 0; day < days + 1; ++day) {
			im.force_tick_and_update();
		}
		ArmyInstance const* recruited = nullptr;
		for (ArmyInstance const& army : country->get_armies()) {
			for (DivisionInstance const& division : army.get_divisions()) {
				if (division.id == division_id) {
					recruited = &army;
				}
			}
		}
		std::size_t in_division = 0;
		if (recruited != nullptr) {
			for (RegimentInstance const& regiment : recruited->get_regiment_instances()) {
				in_division += regiment.get_division_id() == division_id;
			}
		}
		r.check(recruited != nullptr && in_division == regiments && &recruited->get_location() == province,
			"division recruitment: the regiments form one division of the template in the province",
			fmt::format("{} days, {} of {} regiments", days, in_division, regiments));
		// The cancel of one regiment cancels the division: the orders, the manpower and the goods come back
		for (const std::uint8_t cell : division_template.cells) {
			if (RegimentType const* const type = cell != 0 ? unit_type_manager.get_regiment_type_by_index(regiment_type_index_t(cell - 1)) : nullptr) {
				for (auto const& [good, quantity] : type->get_build_cost()) {
					country->get_good_data(*good).stockpile_amount += quantity;
				}
			}
		}
		const Manpower before_second = unit_manager.get_manpower(*country);
		const std::size_t orders_second = unit_manager.get_recruitment_orders().size();
		if (unit_manager.start_division_recruitment(*country, unit_type_manager, division_template, *province)) {
			const unique_id_t one = unit_manager.get_recruitment_orders().back().id;
			r.check(unit_manager.cancel_recruitment(*country, one) && unit_manager.get_recruitment_orders().size() == orders_second
					&& unit_manager.get_manpower(*country).available_regiments == before_second.available_regiments,
				"division recruitment: cancelling a regiment cancels the whole division and gives the manpower back");
		}
	}

	// ---- the mobilisation (docs/RECRUITMENT.md, "Мобилизация"): the poor pops raise regiments out of the manpower of the soldiers, the losses of
	// a mobilised regiment die in its pop, the demobilisation gives the men back (by hand, and by itself after the war), the AI mobilises when weaker

	// The men of the poor pops of the mobilisation in the country (the pool base)
	std::int64_t mobilisation_pop_men(CountryInstance const& country) {
		std::int64_t men = 0;
		for (ProvinceInstance const* province : country.get_owned_provinces()) {
			for (Pop const& pop : province->get_pops()) {
				if (UnitInstanceManager::is_mobilisation_pop_type(pop.get_type())) {
					men += type_safe::get(pop.get_size());
				}
			}
		}
		return men;
	}

	std::size_t count_mobilised_regiments(CountryInstance const& country) {
		std::size_t count = 0;
		for (ArmyInstance const& army : country.get_armies()) {
			for (RegimentInstance const& regiment : army.get_regiment_instances()) {
				count += regiment.is_mobilised();
			}
		}
		return count;
	}

	void test_mobilisation(Results& r, InstanceManager& im) {
		UnitInstanceManager& unit_manager = im.get_unit_instance_manager();
		CountryInstance* country = nullptr;
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (!candidate.exists() || candidate.is_rebel_country() || candidate.is_at_war() || candidate.get_armies().empty()
				|| candidate.get_owned_provinces().size() < 5) {
				continue;
			}
			candidate.grant_unlock(UNLOCK_MECHANIC_MOBILISATION);
			const Mobilisation pool = unit_manager.get_mobilisation(candidate);
			if (pool.pool_regiments >= 3 && pool.pool_regiments <= 200) {
				country = &candidate;
				break;
			}
		}
		if (country == nullptr) {
			fmt::println("TEST INFO mobilisation: no country at peace with a pool of 3 to 200 regiments, not checked");
			return;
		}
		const Manpower soldiers_before = unit_manager.get_manpower(*country);
		const Mobilisation pool = unit_manager.get_mobilisation(*country);
		fmt::println(
			"TEST INFO mobilisation: {} poor {} men, share {:.4f}, pool {} men = {} regiments, {}-{} days, impact {:.2f}, planned output {:.3f}",
			country->get_identifier(), pool.pop_men, static_cast<double>(pool.share), pool.pool_men, pool.pool_regiments, pool.days_min,
			pool.days_max, static_cast<double>(pool.economy_impact), static_cast<double>(pool.planned_output_multiplier)
		);
		const std::size_t orders_before = unit_manager.get_recruitment_orders().size();
		r.check(unit_manager.get_mobilise_block_reason(*country).empty() && unit_manager.mobilise(*country) && country->is_mobilised(),
			"mobilisation: a country with the mechanic and poor pops mobilises", std::string { unit_manager.get_mobilise_block_reason(*country) });
		std::size_t poor_orders = 0;
		for (RecruitmentOrder const& order : unit_manager.get_recruitment_orders()) {
			poor_orders += order.country == country && order.mobilisation && order.pop != nullptr
				&& UnitInstanceManager::is_mobilisation_pop_type(order.pop->get_type()) && &order.pop->get_location() == order.province;
		}
		const Manpower soldiers_ordered = unit_manager.get_manpower(*country);
		r.check(unit_manager.get_recruitment_orders().size() == orders_before + pool.pool_regiments && poor_orders == pool.pool_regiments
				&& soldiers_ordered.queued_men == soldiers_before.queued_men && soldiers_ordered.free_men == soldiers_before.free_men,
			"mobilisation: the regiments of the pool are ordered from the poor pops in their provinces, the manpower of the soldiers does not change",
			fmt::format("{} orders of {} regiments, {} from poor pops; free soldiers {} -> {}", unit_manager.get_recruitment_orders().size() - orders_before,
				pool.pool_regiments, poor_orders, soldiers_before.free_men, soldiers_ordered.free_men));
		for (std::int32_t day = 0; day <= pool.days_max; ++day) {
			im.force_tick_and_update();
		}
		const Mobilisation raised = unit_manager.get_mobilisation(*country);
		bool linked = true;
		for (ArmyInstance const& army : country->get_armies()) {
			for (RegimentInstance const& regiment : army.get_regiment_instances()) {
				if (regiment.is_mobilised()) {
					linked &= regiment.get_pop() != nullptr && UnitInstanceManager::is_mobilisation_pop_type(regiment.get_pop()->get_type());
				}
			}
		}
		const Manpower soldiers_raised = unit_manager.get_manpower(*country);
		std::uint32_t regular = 0;
		for (ArmyInstance const& army : country->get_armies()) {
			for (RegimentInstance const& regiment : army.get_regiment_instances()) {
				regular += !regiment.is_mobilised();
			}
		}
		r.check(raised.regiments == pool.pool_regiments && raised.pending_regiments == 0 && linked && soldiers_raised.regiments == regular,
			"mobilisation: after the days of the provinces the mobilised regiments stand, linked to poor pops, out of the regiments of the soldiers",
			fmt::format("{} of {} regiments ({} waiting), {} regular regiments of {} counted", raised.regiments, pool.pool_regiments,
				raised.pending_regiments, soldiers_raised.regiments, regular));
		r.check(country->manpower_combat_multiplier == fixed_point_t::_1 || soldiers_before.combat_multiplier < fixed_point_t::_1,
			"mobilisation: the mobilised men do not put the army over the manpower of the soldiers",
			fmt::format("multiplier {:.3f}", static_cast<double>(country->manpower_combat_multiplier)));
		r.check(country->mobilisation_output_multiplier == raised.output_multiplier
				&& (raised.economy_impact <= fixed_point_t::_0 || raised.output_multiplier < fixed_point_t::_1),
			"mobilisation: while the men are away the RGOs and the factories of the country make less",
			fmt::format("impact {:.2f}, taken {:.4f}, output {:.4f}", static_cast<double>(raised.economy_impact),
				static_cast<double>(raised.taken_share), static_cast<double>(country->mobilisation_output_multiplier)));

		// The losses of a mobilised regiment die in its poor pop, the soldiers do not lose a man
		RegimentInstance* mobilised = nullptr;
		for (ArmyInstance& army : country->get_armies()) {
			for (RegimentInstance& regiment : army.get_regiment_instances()) {
				if (mobilised == nullptr && regiment.is_mobilised() && regiment.get_pop() != nullptr && type_safe::get(regiment.get_pop()->get_size()) > 2000) {
					mobilised = &regiment;
				}
			}
		}
		if (mobilised != nullptr) {
			const std::int64_t pop_before = type_safe::get(mobilised->get_pop()->get_size());
			const std::int64_t soldiers_men = unit_manager.get_manpower(*country).pop_men;
			mobilised->take_damage(fixed_point_t::_0, fixed_point_t::_1);
			unit_manager.apply_losses(*mobilised, *country);
			r.check(type_safe::get(mobilised->get_pop()->get_size()) == pop_before - 1000 && unit_manager.get_manpower(*country).pop_men == soldiers_men,
				"mobilisation: the losses of a mobilised regiment die in its poor pop, not in the soldiers",
				fmt::format("pop {} -> {}, soldiers {} -> {}", pop_before, type_safe::get(mobilised->get_pop()->get_size()), soldiers_men,
					unit_manager.get_manpower(*country).pop_men));
		}

		// The demobilisation: no mobilised regiment is left, the men are back (nobody dies), the economy is whole again
		const std::int64_t poor_before = mobilisation_pop_men(*country);
		std::size_t linked_regiments = 0;
		for (ProvinceInstance const* province : country->get_owned_provinces()) {
			for (Pop const& pop : province->get_pops()) {
				if (UnitInstanceManager::is_mobilisation_pop_type(pop.get_type())) {
					linked_regiments += pop.get_regiment_count();
				}
			}
		}
		const bool demobilised = unit_manager.demobilise(*country);
		std::size_t linked_after = 0;
		for (ProvinceInstance const* province : country->get_owned_provinces()) {
			for (Pop const& pop : province->get_pops()) {
				if (UnitInstanceManager::is_mobilisation_pop_type(pop.get_type())) {
					linked_after += pop.get_regiment_count();
				}
			}
		}
		r.check(demobilised && !country->is_mobilised() && count_mobilised_regiments(*country) == 0 && mobilisation_pop_men(*country) == poor_before
				&& linked_after + raised.regiments == linked_regiments && country->mobilisation_output_multiplier == fixed_point_t::_1,
			"mobilisation: the demobilisation disbands the mobilised regiments, the men go back to their pops and nobody dies",
			fmt::format("{} mobilised regiments left, poor {} -> {}, regiments linked to poor pops {} -> {}", count_mobilised_regiments(*country),
				poor_before, mobilisation_pop_men(*country), linked_regiments, linked_after));

		// The game action, and the demobilisation by itself DEMOBILISE_DAYS after a war
		im.queue_game_action<set_mobilise_argument_t>(country->index, true);
		im.force_tick_and_update();
		r.check(country->is_mobilised(), "mobilisation: the game action set_mobilise mobilises");
		country->mobilisation_war_seen = true; // as if a war had ended
		const std::int32_t demobilise_days = im.definition_manager.get_define_manager().get_openvic_defines().war.demobilise_days;
		for (std::int32_t day = 0; day <= demobilise_days; ++day) {
			im.force_tick_and_update();
		}
		r.check(!country->is_mobilised() && count_mobilised_regiments(*country) == 0,
			"mobilisation: DEMOBILISE_DAYS after the last war the country demobilises by itself",
			fmt::format("{} days, mobilised {}", demobilise_days, country->is_mobilised()));
	}

	// The AI at war with fewer regiments than its enemy mobilises
	void test_ai_mobilisation(Results& r, InstanceManager& im) {
		WarManager& war_manager = im.get_war_manager();
		CountryInstance* const russia = im.get_country_instance_manager().get_country_instance_by_identifier("RUS");
		CountryInstance* const turkey = im.get_country_instance_manager().get_country_instance_by_identifier("TUR");
		if (russia == nullptr || turkey == nullptr) {
			fmt::println("TEST INFO AI mobilisation: no Russia or Turkey, not checked");
			return;
		}
		CountryInstance* weak = russia->get_regiment_count() < turkey->get_regiment_count() ? russia : turkey;
		CountryInstance* strong = weak == russia ? turkey : russia;
		if (!weak->is_at_war_with(*strong) && !war_manager.declare_war(im, *strong, *weak)) {
			fmt::println("TEST INFO AI mobilisation: no war between Russia and Turkey, not checked");
			return;
		}
		weak->grant_unlock(UNLOCK_MECHANIC_MOBILISATION);
		strong->grant_unlock(UNLOCK_MECHANIC_MOBILISATION);
		im.force_tick_and_update();
		CountryAI::monthly_update(im);
		r.check(!weak->is_ai() || weak->is_mobilised(), "AI mobilisation: the AI country at war with fewer regiments than its enemy mobilises",
			fmt::format("{} {} regiments against {} {}", weak->get_identifier(), weak->get_regiment_count(), strong->get_identifier(),
				strong->get_regiment_count()));
		r.check(!strong->is_mobilised(), "AI mobilisation: the stronger side does not mobilise");
	}

	// The monthly limit of the recruitment (RECRUIT_MAX_REGIMENTS_PER_MONTH) and the priorities of the waiting orders
	void test_recruitment_limit(Results& r, InstanceManager& im) {
		UnitInstanceManager& unit_manager = im.get_unit_instance_manager();
		OpenVicWarDefines& war = const_cast<OpenVicWarDefines&>(im.definition_manager.get_define_manager().get_openvic_defines().war);
		const std::int32_t limit_before = war.recruit_max_regiments_per_month;
		// The first day of the game fits the armies of the start to the manpower and gives the stockpile: the country is chosen after it (the day also
		// sets the month of the limit)
		im.force_tick_and_update();
		CountryInstance* country = nullptr;
		RegimentType const* type = nullptr;
		Pop* pop = nullptr;
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (!candidate.exists() || candidate.is_rebel_country() || candidate.is_at_war() || candidate.get_capital() == nullptr
				|| unit_manager.get_manpower(candidate).available_regiments < 6) {
				continue;
			}
			for (RegimentType const& regiment_type : im.definition_manager.get_military_manager().get_unit_type_manager().get_regiment_types()) {
				if (candidate.is_unit_type_unlocked(regiment_type)) {
					type = &regiment_type;
					break;
				}
			}
			if (type == nullptr) {
				continue;
			}
			for (auto const& [good, quantity] : type->get_build_cost()) {
				candidate.get_good_data(*good).stockpile_amount += quantity * 1000; // the armies eat these goods every day too
			}
			for (Pop const& candidate_pop : candidate.get_capital()->get_pops()) {
				if (unit_manager.get_recruitment_block_reason(candidate, *type, *candidate.get_capital(), candidate_pop).empty()) {
					pop = candidate.get_capital()->find_pop_by_id(candidate_pop.id_in_province);
					break;
				}
			}
			if (pop != nullptr) {
				country = &candidate;
				break;
			}
			type = nullptr;
		}
		if (country == nullptr) {
			fmt::println("TEST INFO recruitment limit: no country at peace with 6 free regiments and a pop of soldiers in its capital, not checked");
			return;
		}
		war.recruit_max_regiments_per_month = 2;
		ProvinceInstance& capital = *country->get_capital();
		const auto order_of = [&unit_manager](const unique_id_t id) -> RecruitmentOrder const* {
			for (RecruitmentOrder const& order : unit_manager.get_recruitment_orders()) {
				if (order.id == id) {
					return &order;
				}
			}
			return nullptr;
		};
		const std::int64_t left = unit_manager.get_recruitment_limit_left(*country);
		std::vector<unique_id_t> ids;
		std::string refused;
		for (int i = 0; i < 5; ++i) {
			if (unit_manager.start_recruitment(*country, *type, capital, *pop)) {
				ids.push_back(unit_manager.get_recruitment_orders().back().id);
			} else if (refused.empty()) {
				refused = std::string { unit_manager.get_recruitment_block_reason(*country, *type, capital, *pop) };
			}
		}
		std::size_t started = 0;
		std::size_t waiting = 0;
		for (const unique_id_t id : ids) {
			RecruitmentOrder const* const order = order_of(id);
			started += order != nullptr && !order->waiting;
			waiting += order != nullptr && order->waiting;
		}
		r.check(ids.size() == 5 && left >= 0 && started == static_cast<std::size_t>(left) && waiting == 5 - static_cast<std::size_t>(left),
			"recruitment limit: over RECRUIT_MAX_REGIMENTS_PER_MONTH the orders wait in the queue and take nothing",
			fmt::format("{} {} orders, {} started, {} waiting, {} left of the month; refused: {}", country->get_identifier(), ids.size(), started,
				waiting, left, refused));
		// The newest waiting order gets the urgent priority: in the next month it starts first, then the oldest one; the third one waits
		std::vector<unique_id_t> waiting_ids;
		for (const unique_id_t id : ids) {
			if (RecruitmentOrder const* const order = order_of(id); order != nullptr && order->waiting) {
				waiting_ids.push_back(id);
			}
		}
		if (waiting_ids.size() >= 3) {
			im.queue_game_action<set_recruitment_priority_argument_t>(country->index, waiting_ids.back(), std::uint32_t { 2 });
			const std::int32_t month = unit_manager.current_month;
			// The stockpile sells what the country does not need: the goods of the regiments are put back before every day
			for (int day = 0; day < 40 && unit_manager.current_month == month; ++day) {
				for (auto const& [good, quantity] : type->get_build_cost()) {
					fixed_point_t& stockpile = country->get_good_data(*good).stockpile_amount;
					stockpile = std::max(stockpile, quantity * 10);
				}
				im.force_tick_and_update();
			}
			RecruitmentOrder const* const urgent = order_of(waiting_ids.back());
			RecruitmentOrder const* const oldest = order_of(waiting_ids.front());
			RecruitmentOrder const* const middle = order_of(waiting_ids[1]);
			r.check(urgent != nullptr && !urgent->waiting && urgent->priority == 2 && oldest != nullptr && !oldest->waiting && middle != nullptr
					&& middle->waiting,
				"recruitment limit: in the next month the waiting orders start by their priority, then the older ones",
				fmt::format("urgent waits {}, oldest waits {}, middle waits {}; month {} -> {}, left {}, reason now: {}", urgent != nullptr && urgent->waiting,
					oldest != nullptr && oldest->waiting, middle != nullptr && middle->waiting, month, unit_manager.current_month,
					unit_manager.get_recruitment_limit_left(*country), unit_manager.get_recruitment_block_reason(*country, *type, capital, *pop)));
		}
		for (const unique_id_t id : ids) {
			unit_manager.cancel_recruitment(*country, id);
		}
		war.recruit_max_regiments_per_month = limit_before;
	}

	// ---- the state of the executed plan (P5c): no war with the countries of the front is told to the player, with the war it is something else

	void test_plan_status(Results& r, InstanceManager& im) {
		WarManager& war_manager = im.get_war_manager();
		MapInstance& map_instance = im.get_map_instance();
		CountryInstance* country = nullptr;
		CountryInstance* neighbour = nullptr;
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (!candidate.exists() || candidate.is_rebel_country() || !candidate.has_army_command() || candidate.is_at_war() || candidate.get_armies().empty()) {
				continue;
			}
			for (ProvinceInstance* province : candidate.get_owned_provinces()) {
				for (ProvinceDefinition::adjacency_t const& adjacency : province->province_definition.get_adjacencies()) {
					CountryInstance* const other = map_instance.get_province_instance_by_definition(adjacency.get_to()).get_controller();
					if (neighbour == nullptr && adjacency.get_type() == ProvinceDefinition::adjacency_t::type_t::LAND && other != nullptr && other != &candidate
						&& !other->is_at_war() && war_manager.can_declare_war(candidate, *other, im.get_today())) {
						neighbour = other;
					}
				}
			}
			if (neighbour != nullptr) {
				country = &candidate;
				break;
			}
		}
		if (country == nullptr) {
			fmt::println("TEST INFO plan status: no country at peace with the command and a neighbour, not checked");
			return;
		}
		const std::size_t palette_size = im.definition_manager.get_military_manager().get_command_palette().size();
		create_army_group(*country, palette_size);
		const std::uint32_t group_id = country->get_army_groups().back().id;
		bool has_marshal = false;
		for (LeaderInstance const& leader : country->get_leaders<unit_branch_t::LAND>()) {
			if (!leader.is_marshal() && leader.get_can_be_used() && set_army_group_marshal(im, *country, group_id, leader.unique_id)) {
				has_marshal = true;
				break;
			}
		}
		const std::uint32_t front_id = war_manager.create_border_front(im, *country, group_id, *neighbour);
		if (!has_marshal || front_id == 0 || !set_army_group_plan_active(im, *country, group_id, true)) {
			fmt::println("TEST INFO plan status: no marshal or no front, not checked");
			delete_army_group(im, *country, group_id);
			return;
		}
		war_manager.update_plan_status(im, *country, group_id, true);
		r.check(country->find_army_group(group_id)->plan_status == 3, "plan status: executed at peace with the country of the front: no war");
		if (war_manager.declare_war(im, *country, *neighbour)) {
			war_manager.update_plan_status(im, *country, group_id, true);
			r.check(country->find_army_group(group_id)->plan_status != 3, "plan status: at war the check finds the enemy",
				fmt::format("status {}", country->find_army_group(group_id)->plan_status));
			if (War* const war = war_manager.find_war_between(*country, *neighbour)) {
				war_manager.end_war(im, war->id);
			}
		}
		war_manager.delete_group_lines(im, *country, group_id);
		delete_army_group(im, *country, group_id);
	}

	// ---- the way by sea (P4d, docs/WAR_ASSUMPTIONS.md, section 6): an army with no way over the land (Turkey: Tripolitania behind Egypt) goes to a port
	// of its country, sails and lands in the port nearest to its destination, then goes on over the land

	// The laws (docs/LAWS_DESIGN.md): the start from the history, a law in force changes the effects, the model of the army and the kind of the
	// mobilisation change the pool of the mobilisation
	// The administration (version 88, docs/ADMINISTRATION.md): a colony needs more bureaucrats than a core state, the collection of every state is
	// between ADMIN_MIN_COLLECTION and 1, and the priority of the administration is set and cleared by the action
	void test_administration(Results& r, InstanceManager& im) {
		CountryInstance* const england = im.get_country_instance_manager().get_country_instance_by_identifier("ENG");
		if (england == nullptr || england->get_states().empty()) {
			fmt::println("TEST INFO administration: no England, not checked");
			return;
		}
		OpenVicEconomyDefines const& defines = im.definition_manager.get_define_manager().get_openvic_defines().economy;
		State const* colony = nullptr;
		State const* core = nullptr;
		bool collection_in_range = true;
		for (State const* const state : england->get_states()) {
			if (state == nullptr) {
				continue;
			}
			if (state->get_admin_collection() < defines.admin_min_collection.fixed() || state->get_admin_collection() > fixed_point_t::_1) {
				collection_in_range = false;
			}
			if (state->is_colonial_state()) {
				if (colony == nullptr) {
					colony = state;
				}
			} else if (core == nullptr) {
				bool all_core = true;
				for (ProvinceInstance const& province : state->get_provinces()) {
					all_core &= province.is_owner_core();
				}
				if (all_core) {
					core = state;
				}
			}
		}
		r.check(collection_in_range, "administration: the taxes a state collects are between ADMIN_MIN_COLLECTION and all of them");
		if (colony == nullptr || core == nullptr || colony->get_capital() == nullptr) {
			fmt::println("TEST INFO administration: England has no colony or no core state, the rest not checked");
			return;
		}
		r.check(colony->get_admin_needed_share() > core->get_admin_needed_share() * 2 - fixed_point_t::epsilon * 4,
			"administration: a colony needs at least twice the bureaucrats of a core state",
			fmt::format("colony {:.4f}, core {:.4f}", static_cast<double>(colony->get_admin_needed_share()), static_cast<double>(core->get_admin_needed_share())));

		ProvinceInstance const* const focus_before = england->get_admin_focus();
		im.queue_game_action<set_admin_focus_argument_t>(england->index, colony->get_capital()->index, true);
		im.force_tick_and_update();
		const bool set = england->get_admin_focus_state() == colony;
		im.queue_game_action<set_admin_focus_argument_t>(england->index, colony->get_capital()->index, false);
		im.force_tick_and_update();
		r.check(set && england->get_admin_focus() == nullptr, "administration: the priority of the administration is set and cleared by the action");
		england->set_admin_focus(focus_before);
	}

	// The literacy (version 89, docs/LITERACY.md): the maximum organisation of a regiment is the one of its type x (1 + LITERACY_ORGANISATION x the
	// literacy of its pop), and the bonus of the output of a country without the technologies of the production line is LITERACY_OUTPUT_BASE
	void test_literacy(Results& r, InstanceManager& im) {
		OpenVicDefines const& defines = im.definition_manager.get_define_manager().get_openvic_defines();
		UnitInstanceManager& units = im.get_unit_instance_manager();
		units.update_literacy_organisation(defines.war.literacy_organisation);
		bool organisation_ok = true;
		std::size_t checked = 0;
		for (ArmyInstance const& army : units.get_armies()) {
			for (RegimentInstance const& regiment : army.get_regiment_instances()) {
				if (regiment.get_pop() == nullptr || checked >= 50) {
					continue;
				}
				const fixed_point_t literacy = std::clamp(regiment.get_pop()->get_literacy(), fixed_point_t::_0, fixed_point_t::_1);
				const fixed_point_t expected = regiment.unit_type.default_organisation * (fixed_point_t::_1 + defines.war.literacy_organisation.of(literacy));
				organisation_ok &= regiment.get_max_organisation() == expected && regiment.get_organisation() <= regiment.get_max_organisation();
				++checked;
			}
		}
		r.check(checked > 0 && organisation_ok, "literacy: the maximum organisation of a regiment grows with the literacy of its soldiers",
			fmt::format("{} regiments checked", checked));
		CountryInstance const* const england = im.get_country_instance_manager().get_country_instance_by_identifier("ENG");
		if (england != nullptr) {
			const fixed_point_t cap = england->get_literacy_output_cap();
			r.check(cap >= defines.economy.literacy_output_base.fixed()
					&& cap <= defines.economy.literacy_output_base.fixed() + defines.economy.literacy_output_step.fixed() * 3,
				"literacy: the bonus of the output of the literate is the base plus a step per technology of the production line",
				fmt::format("England {:.2f}", static_cast<double>(cap)));
		}
	}

	// The relations (version 90, docs/DIPLOMACY.md): the buttons are monthly processes (+20 / -30 for a diplomatic point, to the limit), the
	// relation without an alliance stays in -100..100, a war gives a scar of -200 that heals after the peace, the points come by the rank
	void test_relations(Results& r, InstanceManager& im) {
		CountryInstanceManager& countries = im.get_country_instance_manager();
		CountryRelationManager& relations = im.get_country_relation_manager();
		OpenVicDiplomacyDefines const& defines = im.definition_manager.get_define_manager().get_openvic_defines().diplomacy;
		CountryInstance* const sweden = countries.get_country_instance_by_identifier("SWE");
		CountryInstance* const portugal = countries.get_country_instance_by_identifier("POR");
		CountryInstance* const uncivilised = countries.get_country_instance_by_identifier("CHI");
		if (sweden == nullptr || portugal == nullptr || uncivilised == nullptr || relations.get_at_war_with(sweden, portugal)
			|| relations.get_country_alliance(sweden, portugal)) {
			fmt::println("TEST INFO relations: no Sweden, Portugal or China, or they are at war or allied, not checked");
			return;
		}
		const CountryRelationManager::relation_value_type relation_before = relations.get_country_relation(sweden, portugal);
		const fixed_point_t points_before = sweden->get_diplomatic_points();
		const fixed_point_t china_before = uncivilised->get_diplomatic_points();

		// Up by 20 for a point, stopped at 100
		relations.set_country_relation(sweden, portugal, 90);
		sweden->set_diplomatic_points(fixed_point_t(5));
		relations.set_relation_process(sweden, portugal, 1);
		relations.monthly_update(im);
		const fixed_point_t points_after_up = sweden->get_diplomatic_points();
		r.check(relations.get_country_relation(sweden, portugal) == defines.relation_max && relations.get_relation_process(sweden, portugal) == 0,
			"relations: improving gives +20 a month up to 100, then the process ends",
			fmt::format("relation {}, process {}", relations.get_country_relation(sweden, portugal), relations.get_relation_process(sweden, portugal)));
		// Down by 30 for a point
		relations.set_relation_process(sweden, portugal, -1);
		relations.monthly_update(im);
		r.check(relations.get_country_relation(sweden, portugal) == defines.relation_max - defines.relation_worsen_per_month
				&& relations.get_relation_process(sweden, portugal) == -1,
			"relations: worsening gives -30 a month and goes on",
			fmt::format("relation {}", relations.get_country_relation(sweden, portugal)));
		relations.set_relation_process(sweden, portugal, 0);
		// Without an alliance the relation does not stay above 100
		relations.set_country_relation(sweden, portugal, 150);
		relations.monthly_update(im);
		r.check(relations.get_country_relation(sweden, portugal) == defines.relation_max, "relations: without an alliance the relation is at most 100");
		// The points: a month gives the points of the rank (a point was spent on each month of a process above)
		r.check(points_after_up < fixed_point_t(5) + defines.diplomatic_points_great_power
				&& uncivilised->get_diplomatic_points() >= std::min(defines.diplomatic_points_max, china_before + defines.diplomatic_points_uncivilised) - fixed_point_t::epsilon,
			"relations: the diplomatic points come every month (the uncivilised 0.5) and a process takes one");
		// The scar of a war heals after the peace
		relations.add_war_scar(sweden, portugal);
		const fixed_point_t scar = relations.get_war_scar(sweden, portugal);
		relations.monthly_update(im);
		const fixed_point_t healed = relations.get_war_scar(sweden, portugal) - scar;
		const fixed_point_t expected = fixed_point_t(defines.war_relation_heal_per_month);
		r.check(scar == fixed_point_t(-defines.war_relation_penalty) && healed == expected
				&& static_cast<std::int32_t>(relations.get_effective_relation(sweden, portugal)) == std::clamp(static_cast<std::int32_t>(relations.get_country_relation(sweden, portugal))
					+ relations.get_war_scar(sweden, portugal).floor<std::int32_t>(), -200, 200),
			"relations: a war gives a scar of -200 that heals by 1 a month",
			fmt::format("scar {:.2f}, healed {:.4f} a month (expected {:.4f})", static_cast<double>(scar), static_cast<double>(healed), static_cast<double>(expected)));
		// back as it was (the scar heals in the game on its own)
		relations.set_country_relation(sweden, portugal, relation_before);
		sweden->set_diplomatic_points(points_before);
	}

	// The diplomacy of the AI and the help of the AI (version 91, docs/DIPLOMACY.md): after a month every great power of the AI has a goal for the
	// years ahead, no two countries at war are allied; the help of the AI buys again a good the government lacks and does not buy
	void test_diplomacy_ai(Results& r, InstanceManager& im) {
		OpenVicDiplomacyDefines& defines = const_cast<OpenVicDiplomacyDefines&>(im.definition_manager.get_define_manager().get_openvic_defines().diplomacy);
		const std::int32_t enabled_before = defines.ai_diplomacy_enabled;
		defines.ai_diplomacy_enabled = 1;
		DiplomacyAI::monthly_update(im);
		defines.ai_diplomacy_enabled = enabled_before;
		bool goals = true;
		std::size_t great_powers = 0;
		for (CountryInstance const& country : im.get_country_instance_manager().get_country_instances()) {
			const std::size_t rank = country.get_total_rank();
			if (!country.exists() || !country.is_ai() || rank == 0 || rank > static_cast<std::size_t>(defines.great_power_rank) || country.get_owned_provinces().empty()) {
				continue;
			}
			++great_powers;
			goals &= country.get_ai_goal() != CountryInstance::ai_goal_t::NONE && country.get_ai_goal_until() > im.get_today();
		}
		r.check(great_powers > 0 && goals, "diplomacy AI: every great power of the AI has a goal for the years ahead", fmt::format("{} great powers", great_powers));
		bool allied_at_war = false;
		for (CountryInstance const& a : im.get_country_instance_manager().get_country_instances()) {
			for (CountryInstance const& b : im.get_country_instance_manager().get_country_instances()) {
				if (&a != &b && a.has_alliance_with(b) && a.is_at_war_with(b)) {
					allied_at_war = true;
				}
			}
		}
		r.check(!allied_at_war, "diplomacy AI: no two countries at war with each other are allied");

		CountryInstance* const country = im.get_country_instance_manager().get_country_instance_by_identifier("SWE");
		GoodInstance* const good = im.get_good_instance_manager().get_good_instance_by_identifier("grain");
		if (country == nullptr || good == nullptr) {
			fmt::println("TEST INFO diplomacy AI: no Sweden or no grain, the help of the AI not checked");
			return;
		}
		CountryInstance::good_data_t& good_data = country->get_good_data(*good);
		const bool automated_before = good_data.is_automated;
		const bool selling_before = good_data.is_selling;
		const fixed_point_t stockpile_before = good_data.stockpile_amount;
		const fixed_point_t needs_before = good_data.government_needs;
		const bool was_ai = country->is_ai();
		country->set_ai(false);
		country->set_ai_assist(true);
		good_data.is_automated = false;
		good_data.is_selling = true;
		good_data.stockpile_amount = fixed_point_t::_0;
		good_data.government_needs = fixed_point_t(10);
		DiplomacyAI::weekly_assist(im);
		r.check(good_data.is_automated, "diplomacy AI: the help of the AI buys again a good the government lacks");
		good_data.is_automated = automated_before;
		good_data.is_selling = selling_before;
		good_data.stockpile_amount = stockpile_before;
		good_data.government_needs = needs_before;
		country->set_ai_assist(false);
		country->set_ai(was_ai);
	}

	// The spheres of influence (version 92, docs/PLAN_SPHERES.md): the spheres of the history at the start, the relation bonus and its floor,
	// leaving and joining, who may found one; the AI members of the history stay though the influence is small
	void test_spheres(Results& r, InstanceManager& im) {
		CountryInstanceManager& countries = im.get_country_instance_manager();
		CountryRelationManager& relations = im.get_country_relation_manager();
		SphereManager& spheres = im.get_sphere_manager();
		OpenVicDiplomacyDefines const& defines = im.definition_manager.get_define_manager().get_openvic_defines().diplomacy;
		CountryInstance* const prussia = countries.get_country_instance_by_identifier("PRU");
		CountryInstance* const bavaria = countries.get_country_instance_by_identifier("BAV");
		CountryInstance* const saxony = countries.get_country_instance_by_identifier("SAX");
		CountryInstance* const sweden = countries.get_country_instance_by_identifier("SWE");
		if (prussia == nullptr || bavaria == nullptr || saxony == nullptr || sweden == nullptr || im.definition_manager.get_start_sphere_definitions().get_entries().empty()) {
			fmt::println("TEST INFO spheres: no Prussia, Bavaria, Saxony or Sweden, or no start spheres, not checked");
			return;
		}
		Sphere const* const zollverein = spheres.find_led_by(prussia);
		r.check(zollverein != nullptr && zollverein->kind == sphere_kind_t::CUSTOMS_UNION && spheres.find_of(bavaria) == zollverein
				&& bavaria->get_sphere_owner_untracked() == prussia && prussia->get_leads_sphere(),
			"spheres: Prussia starts with the customs union of the history (Bavaria is a member)",
			fmt::format("{} spheres", spheres.get_spheres().size()));
		if (zollverein == nullptr || spheres.find_of(bavaria) != zollverein || spheres.find_of(saxony) != zollverein) {
			return;
		}
		// the relation: +25 with the leader, never under -175 (a war scar of -200 stops there)
		const CountryRelationManager::relation_value_type base = relations.get_country_relation(prussia, bavaria);
		r.check(static_cast<std::int32_t>(relations.get_effective_relation(prussia, bavaria))
				== std::clamp(static_cast<std::int32_t>(base) + relations.get_war_scar(prussia, bavaria).floor<std::int32_t>() + defines.sphere_leader_relation, -200, 200),
			"spheres: the leader has +25 relation with a member");
		relations.set_country_relation(prussia, saxony, -100);
		relations.add_war_scar(prussia, saxony);
		r.check(static_cast<std::int32_t>(relations.get_effective_relation(prussia, saxony)) == defines.sphere_relation_floor,
			"spheres: the relation of the leader and a member never goes under -175",
			fmt::format("relation {}", relations.get_effective_relation(prussia, saxony)));
		// the AI members of the history stay a month though the influence is far under the threshold
		spheres.monthly_update(im);
		r.check(spheres.find_of(bavaria) == spheres.find_led_by(prussia) && spheres.find_of(saxony) == spheres.find_led_by(prussia),
			"spheres: an AI member of the history stays, whatever the influence and the relation");
		// leaving, and coming back by the invitation (a player country is not asked)
		const bool bavaria_ai = bavaria->is_ai();
		r.check(spheres.leave(im, *bavaria) && spheres.find_of(bavaria) == nullptr && bavaria->get_sphere_owner_untracked() == nullptr
				&& relations.get_effective_relation(prussia, bavaria) == std::clamp<std::int32_t>(base + relations.get_war_scar(prussia, bavaria).floor<std::int32_t>(), -200, 200),
			"spheres: a member leaves, the bonus of the relation goes");
		bavaria->set_ai(false);
		r.check(spheres.invite(im, *prussia, *bavaria) && spheres.find_of(bavaria) == spheres.find_led_by(prussia), "spheres: the leader invites a country back");
		bavaria->set_ai(bavaria_ai);
		r.check(!spheres.invite_block_reason(im, *prussia, *bavaria).empty(), "spheres: a member is not invited twice");
		// who founds: only a great power out of the spheres, no Alliance without the technology
		r.check(!spheres.found_block_reason(im, *sweden, sphere_kind_t::IMPERIAL).empty(), "spheres: a country that is no great power founds no sphere");
		r.check(!spheres.found_block_reason(im, *prussia, sphere_kind_t::MILITARY).empty(), "spheres: a leader founds no second sphere");
		CountryInstance* founder = nullptr;
		for (CountryInstance& country : countries.get_country_instances()) {
			const std::size_t rank = country.get_total_rank();
			if (founder == nullptr && country.exists() && !country.get_owned_provinces().empty() && rank > 0
				&& rank <= static_cast<std::size_t>(defines.great_power_rank) && spheres.find_of(&country) == nullptr) {
				founder = &country;
			}
		}
		if (founder == nullptr) {
			fmt::println("TEST INFO spheres: every great power is in a sphere, founding not checked");
			return;
		}
		r.check(!spheres.found_block_reason(im, *founder, sphere_kind_t::ALLIANCE).empty() && spheres.found(im, *founder, sphere_kind_t::IDEOLOGICAL)
				&& spheres.find_led_by(founder) != nullptr && founder->get_leads_sphere(),
			"spheres: a great power out of the spheres founds one (the Alliance needs the technology)", fmt::format("{}", founder->get_identifier()));
	}

	// The kinds of the spheres (version 93, docs/PLAN_SPHERES.md): the customs union and its single market, the military union (no wars inside,
	// the leader draws the members into his war, the land of a partner), the imperial sphere (the tribute, the relations between the members,
	// the free diplomacy), the pause before the next change of the kind
	void test_sphere_kinds(Results& r, InstanceManager& im) {
		CountryInstanceManager& countries = im.get_country_instance_manager();
		CountryRelationManager& relations = im.get_country_relation_manager();
		SphereManager& spheres = im.get_sphere_manager();
		WarManager& war_manager = im.get_war_manager();
		OpenVicDiplomacyDefines const& defines = im.definition_manager.get_define_manager().get_openvic_defines().diplomacy;
		CountryInstance* const prussia = countries.get_country_instance_by_identifier("PRU");
		CountryInstance* const bavaria = countries.get_country_instance_by_identifier("BAV");
		CountryInstance* const saxony = countries.get_country_instance_by_identifier("SAX");
		Sphere const* const zollverein = prussia != nullptr ? spheres.find_led_by(prussia) : nullptr;
		if (zollverein == nullptr || bavaria == nullptr || saxony == nullptr || spheres.find_of(bavaria) != zollverein || spheres.find_of(saxony) != zollverein
			|| zollverein->kind != sphere_kind_t::CUSTOMS_UNION) {
			fmt::println("TEST INFO sphere kinds: no customs union of Prussia with Bavaria and Saxony, not checked");
			return;
		}
		// (the tests do not wait SPHERE_KIND_CHANGE_MONTHS: the date of the last change is moved back)
		const auto allow_change = [&]() {
			const_cast<Sphere*>(spheres.find_led_by(prussia))->kind_changed = Date {};
		};

		// ---- the customs union: Saxony borders Prussia, so it is in the single market; the tariffs do not fall inside
		TradeNetwork& network = im.get_trade_network();
		if (network.is_enabled()) {
			network.update(im);
			const std::size_t pru = type_safe::get(prussia->index), sax = type_safe::get(saxony->index), bav = type_safe::get(bavaria->index);
			r.check(network.same_customs_union(pru, bav) && network.same_customs_union(sax, pru) && network.same_single_market(pru, sax)
					&& network.has_single_markets(),
				"sphere kinds: the members of a customs union trade without tariffs, the land neighbours of the leader are its single market");
		}
		r.check(!spheres.change_kind_block_reason(im, *prussia, sphere_kind_t::MILITARY).empty(),
			"sphere kinds: the kind is not changed again before SPHERE_KIND_CHANGE_MONTHS");

		// ---- the military union
		allow_change();
		r.check(spheres.change_kind(im, *prussia, sphere_kind_t::MILITARY) && bavaria->get_sphere_kind() == sphere_kind_t::MILITARY
				&& prussia->get_sphere_kind() == sphere_kind_t::MILITARY, "sphere kinds: the leader changes the kind, the members get it");
		r.check(bavaria->is_military_partner(*saxony) && prussia->is_military_partner(*bavaria) && !war_manager.can_declare_war(*bavaria, *saxony, im.get_today())
				&& !war_manager.get_justify_block_reason(*prussia, *bavaria, im.get_today()).empty(),
			"sphere kinds: no war between the members of a military union");
		ProvinceInstance* const saxon_land = saxony->get_capital();
		r.check(saxon_land != nullptr && UnitInstanceGroup::can_country_enter(*bavaria, *saxon_land),
			"sphere kinds: the armies of a military union go on the land of the other members");
		// a war of the leader draws the members in
		CountryInstance* target = nullptr;
		for (CountryInstance& candidate : countries.get_country_instances()) {
			if (target == nullptr && candidate.exists() && !candidate.is_rebel_country() && !candidate.get_owned_provinces().empty()
				&& spheres.find_of(&candidate) == nullptr && !candidate.is_at_war() && war_manager.can_declare_war(*prussia, candidate, im.get_today())) {
				bool allied_inside = prussia->has_alliance_with(candidate);
				for (CountryInstance const* member : zollverein->members) {
					allied_inside |= member->has_alliance_with(candidate);
				}
				if (!allied_inside) {
					target = &candidate;
				}
			}
		}
		if (target == nullptr) {
			fmt::println("TEST INFO sphere kinds: Prussia has no country to attack, the call of the members not checked");
		} else {
			std::size_t free_members = 0;
			for (CountryInstance const* member : spheres.find_led_by(prussia)->members) {
				free_members += member->is_at_war() ? 0 : 1;
			}
			const bool declared = war_manager.declare_war(im, *prussia, *target);
			War const* const war = war_manager.find_war_between(*prussia, *target);
			std::size_t joined = 0;
			bool wrong_side = false;
			if (war != nullptr) {
				for (CountryInstance const* member : spheres.find_led_by(prussia)->members) {
					joined += war->has_participant(*member) ? 1 : 0;
					wrong_side |= war->has_participant(*member) && !war->is_attacker(*member);
				}
			}
			r.check(declared && war != nullptr && joined > 0 && !wrong_side,
				"sphere kinds: the leader of a military union draws its members into its war",
				fmt::format("{} of {} free members joined the war against {}", joined, free_members, target->get_identifier()));
			if (war != nullptr) {
				war_manager.end_war(im, war->id);
			}
		}

		// ---- the imperial sphere: the relations between the members, the tribute, the free diplomacy
		allow_change();
		r.check(spheres.change_kind(im, *prussia, sphere_kind_t::IMPERIAL) && !bavaria->is_military_partner(*saxony),
			"sphere kinds: the military union ends with the change of the kind");
		const std::int32_t members_base = static_cast<std::int32_t>(relations.get_country_relation(bavaria, saxony))
			+ relations.get_war_scar(bavaria, saxony).floor<std::int32_t>();
		r.check(static_cast<std::int32_t>(relations.get_effective_relation(bavaria, saxony)) == std::clamp(members_base + defines.sphere_member_relation, -200, 200),
			"sphere kinds: the members of an imperial sphere have +25 relation with each other");
		const fixed_point_t cash_before = prussia->get_cash_stockpile().load();
		spheres.daily_update(im);
		const fixed_point_t tribute = spheres.find_led_by(prussia)->tribute_yesterday;
		r.check(tribute > fixed_point_t::_0 && prussia->get_cash_stockpile().load() == cash_before + tribute,
			"sphere kinds: the members of an imperial sphere pay a share of their income to the leader",
			fmt::format("tribute {:.2f}", static_cast<double>(tribute)));
		relations.set_country_relation(bavaria, saxony, 0);
		const fixed_point_t points_gain = CountryRelationManager::get_diplomatic_points_gain(*bavaria);
		bavaria->set_diplomatic_points(fixed_point_t::_0);
		// (the AI of Bavaria has processes of its own that would take the points)
		for (auto const& [other, direction] : relations.get_relation_processes_of(bavaria)) {
			relations.set_relation_process(bavaria, other, 0);
		}
		relations.set_relation_process(bavaria, saxony, 1);
		relations.monthly_update(im);
		r.check(relations.get_country_relation(bavaria, saxony) == defines.relation_improve_per_month
				&& bavaria->get_diplomatic_points() == std::min(defines.diplomatic_points_max, points_gain),
			"sphere kinds: the processes of the relations between the members of an imperial sphere cost no diplomatic points",
			fmt::format("relation {}, points {:.2f}", relations.get_country_relation(bavaria, saxony), static_cast<double>(bavaria->get_diplomatic_points())));
		relations.set_relation_process(bavaria, saxony, 0);

		// back to the customs union of the history
		allow_change();
		spheres.change_kind(im, *prussia, sphere_kind_t::CUSTOMS_UNION);
	}

	// The principles of the spheres (version 94, docs/PLAN_SPHERES.md): the data, the slots by the countries, the bonus for the leader and the
	// members, the level by the years, the output of the chosen goods only, the freight of the trading posts
	void test_sphere_principles(Results& r, InstanceManager& im) {
		CountryInstanceManager& countries = im.get_country_instance_manager();
		SphereManager& spheres = im.get_sphere_manager();
		SpherePrincipleDefinitions const& definitions = im.definition_manager.get_sphere_principle_definitions();
		GoodDefinitionManager const& goods = im.definition_manager.get_economy_manager().get_good_definition_manager();
		CountryInstance* const prussia = countries.get_country_instance_by_identifier("PRU");
		CountryInstance* const bavaria = countries.get_country_instance_by_identifier("BAV");
		const std::uint32_t specialists = definitions.find("specialists_exchange");
		const std::uint32_t industry = definitions.find("military_industry");
		const std::uint32_t posts = definitions.find("trading_posts");
		GoodDefinition const* const small_arms = goods.get_good_definition_by_identifier("small_arms");
		GoodDefinition const* const oil = goods.get_good_definition_by_identifier("oil");
		GoodDefinition const* const grain = goods.get_good_definition_by_identifier("grain");
		r.check(definitions.get_principles().size() >= 6 && specialists != Sphere::NO_PRINCIPLE && industry != Sphere::NO_PRINCIPLE
				&& posts != Sphere::NO_PRINCIPLE,
			"sphere principles: the principles are read from ov_data/common/ov_sphere_principles",
			fmt::format("{} principles", definitions.get_principles().size()));
		Sphere const* const zollverein = prussia != nullptr ? spheres.find_led_by(prussia) : nullptr;
		if (zollverein == nullptr || bavaria == nullptr || spheres.find_of(bavaria) != zollverein || specialists == Sphere::NO_PRINCIPLE
			|| industry == Sphere::NO_PRINCIPLE || posts == Sphere::NO_PRINCIPLE || small_arms == nullptr || oil == nullptr || grain == nullptr) {
			fmt::println("TEST INFO sphere principles: no customs union of Prussia with Bavaria, or no principles or goods, not checked");
			return;
		}
		// (the AI of Prussia may have filled the slots already)
		for (std::size_t slot = 0; slot < Sphere::MAX_PRINCIPLE_SLOTS; ++slot) {
			spheres.set_principle(im, *prussia, slot, Sphere::NO_PRINCIPLE);
		}
		r.check(spheres.get_principle_slots(im, *zollverein) == 3 && bavaria->get_sphere_principle_bonus(principle_effect_t::RESEARCH) == fixed_point_t::_0,
			"sphere principles: a sphere of more than 4 countries has 3 slots",
			fmt::format("{} slots for {} countries", spheres.get_principle_slots(im, *zollverein), zollverein->members.size() + 1));

		const fixed_point_t five = fixed_point_t::_1 / 20;
		r.check(spheres.set_principle(im, *prussia, 0, specialists) && bavaria->get_sphere_principle_bonus(principle_effect_t::RESEARCH) == five
				&& prussia->get_sphere_principle_bonus(principle_effect_t::RESEARCH) == five,
			"sphere principles: a principle gives its bonus to the leader and the members");
		r.check(!spheres.set_principle_block_reason(im, *prussia, 1, specialists).empty() && !spheres.set_principle_block_reason(im, *bavaria, 1, industry).empty(),
			"sphere principles: a principle is in one slot only, and only the leader sets them");

		// the level grows with the years in the slot: x1.5 after 5 years, x2 after 15
		const auto age = [&](const std::int32_t years) {
			const_cast<Sphere*>(spheres.find_led_by(prussia))->principles[0].since = im.get_today() - Timespan::from_years(years);
			spheres.refresh_principles(im);
		};
		age(6);
		const fixed_point_t level2 = bavaria->get_sphere_principle_bonus(principle_effect_t::RESEARCH);
		age(16);
		const fixed_point_t level3 = bavaria->get_sphere_principle_bonus(principle_effect_t::RESEARCH);
		r.check(level2 == five * 3 / 2 && level3 == five * 2 && spheres.get_principle_level(im, spheres.find_led_by(prussia)->principles[0]) == 3,
			"sphere principles: the level grows with the years (x1.5 after 5, x2 after 15)",
			fmt::format("{:.4f}, {:.4f}", static_cast<double>(level2), static_cast<double>(level3)));
		spheres.set_principle(im, *prussia, 0, Sphere::NO_PRINCIPLE);
		spheres.set_principle(im, *prussia, 0, specialists);
		r.check(bavaria->get_sphere_principle_bonus(principle_effect_t::RESEARCH) == five, "sphere principles: a principle put in again starts at the level 1");

		// the military industry: the military goods and the oil, not the grain
		spheres.set_principle(im, *prussia, 1, industry);
		r.check(bavaria->get_sphere_output_bonus(type_safe::get(small_arms->index)) == five && bavaria->get_sphere_output_bonus(type_safe::get(oil->index)) == five
				&& bavaria->get_sphere_output_bonus(type_safe::get(grain->index)) == fixed_point_t::_0,
			"sphere principles: the military industry raises the output of the military goods and the oil only");

		// the trading posts: the freight between the countries of the sphere is lower
		spheres.set_principle(im, *prussia, 2, posts);
		TradeNetwork& network = im.get_trade_network();
		if (network.is_enabled()) {
			network.update(im);
			r.check(network.get_sphere_freight_factor(type_safe::get(bavaria->index), type_safe::get(prussia->index)) == fixed_point_t::_1 - five,
				"sphere principles: the trading posts lower the freight between the countries of the sphere");
		}

		for (std::size_t slot = 0; slot < Sphere::MAX_PRINCIPLE_SLOTS; ++slot) {
			spheres.set_principle(im, *prussia, slot, Sphere::NO_PRINCIPLE);
		}
		r.check(bavaria->get_sphere_output_bonus(type_safe::get(small_arms->index)) == fixed_point_t::_0
				&& bavaria->get_sphere_principle_bonus(principle_effect_t::RESEARCH) == fixed_point_t::_0,
			"sphere principles: the bonuses go with the principles");
	}

	void test_laws(Results& r, InstanceManager& im) {
		LawManager const& law_manager = im.definition_manager.get_politics_manager().get_law_manager();
		CountryInstance* const russia = im.get_country_instance_manager().get_country_instance_by_identifier("RUS");
		LawVariant const* const serfdom = law_manager.get_variant("serfdom");
		LawVariant const* const professional = law_manager.get_variant("professional_army");
		LawVariant const* const national = law_manager.get_variant("national_army");
		if (law_manager.empty() || russia == nullptr || serfdom == nullptr || professional == nullptr || national == nullptr) {
			fmt::println("TEST INFO laws: no laws or no Russia, not checked");
			return;
		}
		r.check(russia->get_law(serfdom->group_index) == serfdom, "laws: the start laws come from the history (Russia starts with serfdom)");
		UnitInstanceManager& unit_manager = im.get_unit_instance_manager();
		LawVariant const* const before = russia->get_law(professional->group_index);
		const std::uint8_t level_before = russia->get_mobilisation_level();
		russia->set_law(*national);
		russia->set_mobilisation_level(3);
		const fixed_point_t share_national = unit_manager.get_mobilisation(*russia).share;
		russia->set_law(*professional);
		const fixed_point_t share_professional = unit_manager.get_mobilisation(*russia).share;
		russia->set_mobilisation_level(0);
		const fixed_point_t share_limited = unit_manager.get_mobilisation(*russia).share;
		r.check(russia->get_law_effect(law_effect_t::army_attack) > fixed_point_t::_0_10 && russia->get_law_effect(law_effect_t::army_attack) < fixed_point_t::_0_50 && share_national > fixed_point_t::_0
				&& share_professional * 2 <= share_national + fixed_point_t::epsilon * 4 && share_professional * 2 >= share_national - fixed_point_t::epsilon * 4
				&& share_limited * 4 <= share_professional + fixed_point_t::epsilon * 4 && share_limited * 4 >= share_professional - fixed_point_t::epsilon * 4,
			"laws: the professional army gives +20 % attack and halves the mobilisation, the limited mobilisation raises a quarter of the pool",
			fmt::format("attack {:.2f}, share national {:.5f}, professional {:.5f}, limited {:.5f}", static_cast<double>(russia->get_law_effect(law_effect_t::army_attack)),
				static_cast<double>(share_national), static_cast<double>(share_professional), static_cast<double>(share_limited)));
		russia->set_mobilisation_level(level_before);
		if (before != nullptr) {
			russia->set_law(*before);
		}

		// Back towards the reaction only under a reactionary or fascist government (England: not reactionary, slavery banned)
		CountryInstance* const england = im.get_country_instance_manager().get_country_instance_by_identifier("ENG");
		LawVariant const* const banned = law_manager.get_variant("slavery_banned");
		LawVariant const* const debt = law_manager.get_variant("debt_slavery");
		if (england != nullptr && banned != nullptr && debt != nullptr && england->get_law(banned->group_index) == banned) {
			CountryParty const* const ruling = england->get_ruling_party_untracked();
			const std::string_view ideology = ruling != nullptr && ruling->ideology != nullptr ? std::string_view { ruling->ideology->get_identifier() } : "";
			if (ideology != "reactionary" && ideology != "fascist") {
				r.check(!LawDynamics::may_pass(*england, *debt, banned) && LawDynamics::may_pass(*england, *banned, debt),
					"laws: a law back towards the reaction does not pass under a government that is not reactionary");
			}
		}
	}

	void test_sea_transfer(Results& r, InstanceManager& im) {
		CountryInstance* turkey = im.get_country_instance_manager().get_country_instance_by_identifier("TUR");
		if (turkey == nullptr || turkey->get_capital() == nullptr) {
			fmt::println("TEST INFO sea transfer: no Turkey, not checked");
			return;
		}
		ArmyInstance* army = nullptr;
		for (ArmyInstance& candidate : turkey->get_armies()) {
			if (candidate.get_location().get_identifier() == "1733" && candidate.get_transport_navy() == nullptr && !candidate.is_moving()) {
				army = &candidate;
			}
		}
		if (army == nullptr) {
			fmt::println("TEST INFO sea transfer: no Turkish army in Tripolitania, not checked");
			return;
		}
		const unique_id_t army_id = army->unique_id;
		ProvinceInstance& capital = *turkey->get_capital();
		const bool ordered = im.get_unit_instance_manager().order_move(*army, capital, im.get_map_instance());
		r.check(ordered && army->has_sea_transfer() && army->get_sea_transfer_from() != nullptr && army->get_sea_transfer_to() != nullptr
				&& army->get_sea_transfer_days_total() > 0,
			"sea transfer: an army with no way over the land gets a way by sea through the ports of its country",
			army->has_sea_transfer() ? fmt::format("{} -> {} by sea, {} days", army->get_sea_transfer_from()->get_identifier(),
				army->get_sea_transfer_to()->get_identifier(), army->get_sea_transfer_days_total()) : std::string {});
		bool was_at_sea = false;
		ArmyInstance* found = nullptr;
		for (int day = 0; day < 200; ++day) {
			im.force_tick_and_update();
			found = nullptr;
			for (ArmyInstance& candidate : turkey->get_armies()) {
				if (candidate.unique_id == army_id) {
					found = &candidate;
				}
			}
			if (found == nullptr) {
				break;
			}
			// At sea alone (SEA_TRANSFER_NEEDS_SHIPS 0) or on board of the ships that carry it
			was_at_sea |= found->is_at_sea() || found->get_transport_navy() != nullptr;
			if (&found->get_location() == &capital && !found->is_moving()) {
				break;
			}
		}
		r.check(found != nullptr && was_at_sea && &found->get_location() == &capital,
			"sea transfer: the army sails, lands and reaches its destination over the land",
			found != nullptr ? fmt::format("at {}, at sea {}, way by sea {}, on board {}", found->get_location().get_identifier(), was_at_sea,
				found->has_sea_transfer(), found->get_transport_navy() != nullptr) : std::string { "gone" });
	}

	// The AI countries go on taking provinces in a war (the new battles must not stop them): Russia and Turkey, 60 days
	// ---- the fronts follow the border (docs/WAR_ASSUMPTIONS.md, section 6, P3): a taken province moves the line, a war brings the allies of the
	// enemy into the sector, an edited front keeps its pieces, a front without a border stays empty

	void test_front_dynamics(Results& r, InstanceManager& im) {
		WarManager& war_manager = im.get_war_manager();
		MapInstance& map_instance = im.get_map_instance();
		// A country with mechanic:command that is at peace, two neighbours of it that are at peace (the second one becomes the ally of the first)
		CountryInstance* country = nullptr;
		CountryInstance* neighbour = nullptr;
		CountryInstance* ally = nullptr;
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (!candidate.exists() || candidate.is_rebel_country() || !candidate.has_army_command() || candidate.is_at_war() || !candidate.get_army_groups().empty()) {
				continue;
			}
			std::vector<CountryInstance*> found;
			for (ProvinceInstance* province : candidate.get_owned_provinces()) {
				for (ProvinceDefinition::adjacency_t const& adjacency : province->province_definition.get_adjacencies()) {
					CountryInstance* const other = map_instance.get_province_instance_by_definition(adjacency.get_to()).get_controller();
					if (adjacency.get_type() == ProvinceDefinition::adjacency_t::type_t::LAND && other != nullptr && other != &candidate && !other->is_at_war()
						&& !other->is_rebel_country() && !other->has_alliance_with(candidate) && std::find(found.begin(), found.end(), other) == found.end()) {
						found.push_back(other);
					}
				}
			}
			if (found.size() >= 2) {
				country = &candidate;
				neighbour = found[0];
				ally = found[1];
				break;
			}
		}
		if (country == nullptr) {
			fmt::println("TEST INFO front dynamics: no country with two neighbours at peace, not checked");
			return;
		}
		const std::size_t palette_size = im.definition_manager.get_military_manager().get_command_palette().size();
		create_army_group(*country, palette_size);
		const std::uint32_t group_id = country->get_army_groups().back().id;
		const std::uint32_t front_id = war_manager.create_border_front(im, *country, group_id, *neighbour);
		im.force_tick_and_update();
		Front const* front = war_manager.get_front_by_id(front_id);
		r.check(front != nullptr && !front->edges.empty(), "front dynamics: the front of the marshal is there");
		if (front == nullptr || front->edges.empty()) {
			return;
		}
		const memory::vector<FrontEdge> before = front->edges;
		const auto only_country = [&](Front const& checked, CountryInstance const* other_country) -> bool {
			for (FrontEdge const& edge : checked.edges) {
				if (edge.other->get_controller() != other_country || edge.own->get_controller() != country) {
					return false;
				}
			}
			return true;
		};
		r.check(only_country(*front, neighbour), "front dynamics: at peace the front runs along the neighbour only");

		// A province of the neighbour is taken: the line moves to its far side
		ProvinceInstance* const taken = before.front().other;
		CountryInstance* const taken_from = taken->get_controller();
		taken->set_controller(country);
		im.force_tick_and_update();
		front = war_manager.get_front_by_id(front_id);
		bool moved = true;
		bool from_taken = false;
		for (FrontEdge const& edge : front->edges) {
			moved &= edge.other != taken;
			from_taken |= edge.own == taken;
		}
		bool taken_has_border = false;
		for (ProvinceDefinition::adjacency_t const& adjacency : taken->province_definition.get_adjacencies()) {
			taken_has_border |= adjacency.get_type() == ProvinceDefinition::adjacency_t::type_t::LAND
				&& map_instance.get_province_instance_by_definition(adjacency.get_to()).get_controller() == neighbour;
		}
		r.check(moved && (from_taken || !taken_has_border), "front dynamics: a taken province is no longer across the line, its border with the neighbour is on it");
		taken->set_controller(taken_from);
		im.force_tick_and_update();
		r.check(war_manager.get_front_by_id(front_id)->edges == before, "front dynamics: the province given back puts the line back as it was");

		// A war: the ally of the neighbour joins him, his border becomes part of the sector
		ally->set_alliance_with(*neighbour, true);
		neighbour->set_alliance_with(*ally, true);
		const bool declared = war_manager.declare_war(im, *country, *neighbour);
		im.force_tick_and_update();
		front = war_manager.get_front_by_id(front_id);
		bool has_ally = false;
		for (FrontEdge const& edge : front->edges) {
			has_ally |= edge.other->get_controller() == ally;
		}
		r.check(declared && country->is_at_war_with(*ally) && has_ally, "front dynamics: at war the allies of the enemy in the war are in the sector (their border is on the line)");

		// An edited front keeps its pieces: one piece taken away stays away when the border changes
		const FrontEdge piece = front->edges.back();
		war_manager.toggle_front_edge(im, *country, front_id, *piece.own, *piece.other);
		ProvinceInstance* const second_taken = war_manager.get_front_by_id(front_id)->edges.front().other;
		CountryInstance* const second_from = second_taken->get_controller();
		second_taken->set_controller(country);
		im.force_tick_and_update();
		front = war_manager.get_front_by_id(front_id);
		r.check(front->edited && std::find(front->edges.begin(), front->edges.end(), piece) == front->edges.end(),
			"front dynamics: an edited front does not get back the piece the player took away");
		second_taken->set_controller(second_from);

		// The war is over and the neighbour gone from the border: an empty front stays (its divisions stand)
		if (War* const war = war_manager.find_war_between(*country, *neighbour); war != nullptr) {
			war_manager.end_war(im, war->id);
		}
		ally->set_alliance_with(*neighbour, false);
		neighbour->set_alliance_with(*ally, false);
		im.force_tick_and_update();
		front = war_manager.get_front_by_id(front_id);
		r.check(front != nullptr && only_country(*front, neighbour), "front dynamics: after the war the front runs along the neighbour only again");
		war_manager.delete_group_lines(im, *country, group_id);
		delete_army_group(im, *country, group_id);
	}

	// ---- the divisions of a field marshal go to his order by themselves (docs/WAR_ASSUMPTIONS.md, section 6): the front, the line of defence; a division
	// split from the army of a general stays his; a front on a part of the border

	void test_order_moves(Results& r, InstanceManager& im) {
		WarManager& war_manager = im.get_war_manager();
		MapInstance& map_instance = im.get_map_instance();
		CountryInstance* country = nullptr;
		CountryInstance* neighbour = nullptr;
		ArmyInstance* army = nullptr;
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (!candidate.exists() || candidate.is_rebel_country() || !candidate.has_army_command() || candidate.is_at_war() || !candidate.get_army_groups().empty()) {
				continue;
			}
			// An army that stands on its own land at most two provinces from the land of a neighbour (it reaches the front over land)
			ArmyInstance* found_army = nullptr;
			CountryInstance* found_neighbour = nullptr;
			const auto foreign_land_near = [&](ProvinceInstance& start) -> CountryInstance* {
				for (ProvinceDefinition::adjacency_t const& first : start.province_definition.get_adjacencies()) {
					if (first.get_type() != ProvinceDefinition::adjacency_t::type_t::LAND) {
						continue;
					}
					ProvinceInstance& step = map_instance.get_province_instance_by_definition(first.get_to());
					if (step.get_controller() != nullptr && step.get_controller() != &candidate) {
						return step.get_controller();
					}
					for (ProvinceDefinition::adjacency_t const& second : step.province_definition.get_adjacencies()) {
						CountryInstance* const other = map_instance.get_province_instance_by_definition(second.get_to()).get_controller();
						if (second.get_type() == ProvinceDefinition::adjacency_t::type_t::LAND && other != nullptr && other != &candidate) {
							return other;
						}
					}
				}
				return nullptr;
			};
			for (ArmyInstance& candidate_army : candidate.get_armies()) {
				if (candidate_army.get_transport_navy() == nullptr && !candidate_army.is_at_sea() && !candidate_army.is_moving() && !candidate_army.is_in_combat() && candidate_army.get_divisions().size() >= 2
					&& candidate_army.get_location().get_controller() == &candidate) {
					found_neighbour = foreign_land_near(candidate_army.get_location());
					if (found_neighbour != nullptr) {
						found_army = &candidate_army;
						break;
					}
				}
			}
			if (found_army != nullptr && found_neighbour != nullptr) {
				country = &candidate;
				neighbour = found_neighbour;
				army = found_army;
				break;
			}
		}
		if (country == nullptr) {
			fmt::println("TEST INFO order moves: no country at peace with a neighbour and an army of two divisions, not checked");
			return;
		}
		const std::size_t palette_size = im.definition_manager.get_military_manager().get_command_palette().size();
		create_army_group(*country, palette_size);
		const std::uint32_t group_id = country->get_army_groups().back().id;
		const unique_id_t army_id = army->unique_id;
		const std::uint32_t group_before = army->get_army_group_id();
		r.check(set_army_group(*country, *army, group_id, get_marshal_max_attached(im, *country)), "order moves: the army is attached to the marshal");

		// A part of the border: between the two ends of the line
		const memory::vector<FrontEdge> whole = war_manager.get_border_part(im, *country, *neighbour, nullptr, nullptr);
		const memory::vector<FrontEdge> part = whole.size() >= 2
			? war_manager.get_border_part(im, *country, *neighbour, whole.front().own, whole.front().own) : memory::vector<FrontEdge> {};
		r.check(whole.size() < 2 || (!part.empty() && part.size() < whole.size() && part.front().own == whole.front().own),
			"order moves: a part of the border between one province and itself is the pieces of that province only");

		const std::uint32_t front_id = war_manager.create_border_front(im, *country, group_id, *neighbour);
		im.force_tick_and_update();
		Front const* front = war_manager.get_front_by_id(front_id);
		bool on_front = front != nullptr && std::find(front->army_ids.begin(), front->army_ids.end(), army_id) != front->army_ids.end();
		bool heading = false;
		std::size_t divisions_of_group = 0;
		bool children_valid = true;
		for (ArmyInstance const& other : country->get_armies()) {
			if (other.get_army_group_id() != group_id) {
				children_valid &= other.get_parent_army_id() != army_id;
				continue;
			}
			divisions_of_group += other.get_divisions().size();
			children_valid &= other.unique_id == army_id || other.get_parent_army_id() == army_id;
			ProvinceInstance const* const at = other.is_moving() ? &static_cast<ProvinceInstance const&>(other.get_path().back()) : &other.get_location();
			heading |= front != nullptr && std::find(front->provinces.begin(), front->provinces.end(), at) != front->provinces.end();
		}
		r.check(on_front && heading && !country->get_army_groups().back().plan_active,
			"order moves: the divisions of the marshal go to his front at once, without the execution of the plan",
			fmt::format("on front {} heading {} front armies {} provinces {} country {}", on_front, heading, front != nullptr ? front->army_ids.size() : 0,
				front != nullptr ? front->provinces.size() : 0, country->get_identifier()));
		r.check(children_valid && divisions_of_group >= 2, "order moves: the divisions split along the line stay in the group, under the army of their general");
		std::size_t attached = 0;
		for (ArmyInstance const& other : country->get_armies()) {
			attached += other.get_army_group_id() == group_id && other.get_parent_army_id() == 0;
		}
		r.check(attached == 1, "order moves: the limit of the marshal counts the armies of the generals only (the divisions are not generals)");

		// A few days: the divisions are split and spread along the line (as many provinces as there are divisions, up to the length of the line)
		for (int day = 0; day < 3; ++day) {
			im.force_tick_and_update();
		}
		front = war_manager.get_front_by_id(front_id);
		std::vector<ProvinceInstance const*> held;
		std::size_t group_armies = 0;
		for (ArmyInstance const& other : country->get_armies()) {
			if (other.get_army_group_id() != group_id) {
				continue;
			}
			group_armies++;
			ProvinceInstance const* const at = other.is_moving() ? &static_cast<ProvinceInstance const&>(other.get_path().back()) : &other.get_location();
			if (std::find(front->provinces.begin(), front->provinces.end(), at) != front->provinces.end() && std::find(held.begin(), held.end(), at) == held.end()) {
				held.push_back(at);
			}
		}
		r.check(held.size() >= std::min(group_armies, front->provinces.size()),
			"order moves: the divisions are spread along the front, not in one stack", fmt::format("{} provinces held by {} divisions, the line has {}", held.size(),
				group_armies, front->provinces.size()));

		// A line of defence takes the place of the front: the divisions go to its provinces; it is a chain of neighbours (a gap is filled)
		ProvinceInstance* const own = front->provinces.back();
		ProvinceInstance* const far_end = front->provinces.front();
		const std::uint32_t defence_id = war_manager.create_plan_line(im, *country, group_id, PlanLine::type_t::DEFENCE, *own);
		const bool extended = own == far_end || war_manager.extend_plan_line(im, *country, defence_id, true, *far_end);
		PlanLine const* const defence = war_manager.get_plan_line_by_id(defence_id);
		bool chained = defence != nullptr && defence->provinces.back() == far_end;
		for (std::size_t index = 1; defence != nullptr && index < defence->provinces.size(); ++index) {
			bool neighbours = false;
			for (ProvinceDefinition::adjacency_t const& adjacency : defence->provinces[index - 1]->province_definition.get_adjacencies()) {
				neighbours |= &adjacency.get_to() == &defence->provinces[index]->province_definition;
			}
			chained &= neighbours;
		}
		r.check(extended && chained, "order moves: a line of defence drawn to a far province is a chain of neighbouring provinces up to it");
		if (defence != nullptr && defence->provinces.size() >= 3) {
			ProvinceInstance* const middle = defence->provinces[1];
			r.check(war_manager.extend_plan_line(im, *country, defence_id, true, *middle) && war_manager.get_plan_line_by_id(defence_id)->provinces.size() == 2,
				"order moves: the end of the line dragged onto a province of it cuts the line there");
		}
		im.force_tick_and_update();
		bool to_line = false;
		for (ArmyInstance const& other : country->get_armies()) {
			if (other.get_army_group_id() == group_id) {
				ProvinceInstance const* const at = other.is_moving() ? &static_cast<ProvinceInstance const&>(other.get_path().back()) : &other.get_location();
				to_line |= at == own;
			}
		}
		r.check(war_manager.find_front_of_group(*country, group_id) == nullptr && to_line, "order moves: the divisions go to the line of defence that took the place of the front");

		// Taken out of the group: the divisions follow their army
		set_army_group(*country, *army, group_before, get_marshal_max_attached(im, *country));
		bool all_out = true;
		for (ArmyInstance const& other : country->get_armies()) {
			all_out &= other.get_parent_army_id() != army_id || other.get_army_group_id() == group_before;
		}
		r.check(all_out, "order moves: the divisions of an army leave the group with it");
		war_manager.delete_group_lines(im, *country, group_id);
		delete_army_group(im, *country, group_id);
	}

	// ---- the executor of an offensive (P5, docs/WAR_ASSUMPTIONS.md, section 6): the thresholds of the modes; a war with a plan takes no land beyond its
	// zone, the divisions do not attack while the plan is not executed

	void test_offensive_modes(Results& r, InstanceManager& im) {
		OpenVicWarDefines const& defines = im.definition_manager.get_define_manager().get_openvic_defines().war;
		const auto estimate = [](fixed_point_t chance) -> BattleEstimate {
			BattleEstimate result;
			result.valid = true;
			result.win_chance = chance;
			return result;
		};
		const fixed_point_t c45 = fixed_point_t(45) / 100, c55 = fixed_point_t(55) / 100, c70 = fixed_point_t(70) / 100, c85 = fixed_point_t(85) / 100;
		r.check(!offensive_attacks(1, estimate(c70), defines) && offensive_attacks(1, estimate(c85), defines), "offensive: the careful mode attacks at a chance of 80 % and more");
		r.check(!offensive_attacks(2, estimate(c55), defines) && offensive_attacks(2, estimate(c70), defines), "offensive: the balanced mode attacks at 65 % and more");
		r.check(!offensive_attacks(3, estimate(c45), defines) && offensive_attacks(3, estimate(c55), defines), "offensive: the aggressive mode attacks at 50 % and more");
		r.check(!offensive_attacks(3, BattleEstimate {}, defines), "offensive: no attackers, no attack");
		// The chance: 50 % when both sides last as long, 65 % at 1.23 times, 80 % at 1.59 times (the steepness 3), 1 when the defence leaves at once
		const auto chance = [&](fixed_point_t q) -> double {
			return static_cast<double>(battle_formulas::win_chance(q * 10, fixed_point_t(10), defines.battle_win_steepness));
		};
		r.check(std::abs(chance(fixed_point_t::_1) - 0.5) < 0.001 && std::abs(chance(fixed_point_t(123) / 100) - 0.65) < 0.01
				&& std::abs(chance(fixed_point_t(159) / 100) - 0.80) < 0.01 && battle_formulas::win_chance(fixed_point_t(5), fixed_point_t::_0, 3) == fixed_point_t::_1,
			"offensive: the chance of the victory is 50 % at equal days, 65 % at 1.23 times, 80 % at 1.59 times",
			fmt::format("{:.3f} {:.3f} {:.3f}", chance(fixed_point_t::_1), chance(fixed_point_t(123) / 100), chance(fixed_point_t(159) / 100)));
	}

	void test_offensive_execution(Results& r, InstanceManager& im) {
		WarManager& war_manager = im.get_war_manager();
		MapInstance& map_instance = im.get_map_instance();
		// A country with mechanic:command at peace with an army next to a neighbour at peace
		CountryInstance* country = nullptr;
		CountryInstance* neighbour = nullptr;
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (!candidate.exists() || candidate.is_rebel_country() || !candidate.has_army_command() || candidate.is_at_war() || !candidate.get_army_groups().empty()) {
				continue;
			}
			for (ArmyInstance& army : candidate.get_armies()) {
				if (army.get_transport_navy() != nullptr || army.is_moving() || army.get_location().get_controller() != &candidate || army.get_divisions().size() < 2) {
					continue;
				}
				for (ProvinceDefinition::adjacency_t const& adjacency : army.get_location().province_definition.get_adjacencies()) {
					CountryInstance* const other = map_instance.get_province_instance_by_definition(adjacency.get_to()).get_controller();
					if (neighbour == nullptr && adjacency.get_type() == ProvinceDefinition::adjacency_t::type_t::LAND && other != nullptr && other != &candidate
						&& !other->is_at_war() && !other->is_rebel_country() && !other->has_alliance_with(candidate)) {
						neighbour = other;
					}
				}
				if (neighbour != nullptr) {
					break;
				}
			}
			if (neighbour != nullptr) {
				country = &candidate;
				break;
			}
		}
		if (country == nullptr) {
			fmt::println("TEST INFO offensive execution: no country with an army next to a neighbour at peace, not checked");
			return;
		}
		const std::size_t palette_size = im.definition_manager.get_military_manager().get_command_palette().size();
		create_army_group(*country, palette_size);
		const std::uint32_t group_id = country->get_army_groups().back().id;
		const std::uint32_t max_attached = get_marshal_max_attached(im, *country);
		for (ArmyInstance& army : country->get_armies()) {
			if (army.get_transport_navy() == nullptr) {
				set_army_group(*country, army, group_id, max_attached);
			}
		}
		const bool declared = war_manager.declare_war(im, *country, *neighbour);
		const std::uint32_t front_id = war_manager.create_border_front(im, *country, group_id, *neighbour);
		Front const* front = war_manager.get_front_by_id(front_id);
		if (!declared || front == nullptr || front->edges.empty()) {
			fmt::println("TEST INFO offensive execution: no war or no front, not checked");
			return;
		}
		ProvinceInstance* const target = front->edges.front().other;
		const std::uint32_t offensive_id = war_manager.create_plan_line(im, *country, group_id, PlanLine::type_t::OFFENSIVE, *target);
		const memory::vector<ProvinceInstance*> zone = war_manager.get_offensive_zone(im, *war_manager.get_plan_line_by_id(offensive_id));
		std::vector<ProvinceInstance const*> ours_before;
		for (ProvinceInstance const& province : map_instance.get_province_instances()) {
			if (province.get_controller() == country) {
				ours_before.push_back(&province);
			}
		}
		const auto gained_outside_zone = [&]() -> std::size_t {
			std::size_t outside = 0;
			for (ProvinceInstance const& province : map_instance.get_province_instances()) {
				// The land of another country (taking back an own province somewhere else is not the plan)
				if (province.get_controller() == country && province.get_owner() != country
					&& std::find(ours_before.begin(), ours_before.end(), &province) == ours_before.end()
					&& std::find(zone.begin(), zone.end(), &province) == zone.end()) {
					outside++;
				}
			}
			return outside;
		};
		// Not executed: the divisions stand on the line, nothing is taken
		for (int day = 0; day < 15; ++day) {
			im.force_tick_and_update();
		}
		r.check(target->get_controller() != country, "offensive: while the plan is not executed nothing of the zone is attacked");
		// Executed, aggressive: the target is taken, nothing beyond the zone (the execution needs a field marshal)
		for (LeaderInstance const& leader : country->get_leaders<unit_branch_t::LAND>()) {
			if (!leader.is_marshal() && leader.get_can_be_used() && set_army_group_marshal(im, *country, group_id, leader.unique_id)) {
				break;
			}
		}
		if (!set_army_group_plan_active(im, *country, group_id, true)) {
			fmt::println("TEST INFO offensive execution: no field marshal for the execution, not checked");
			war_manager.delete_group_lines(im, *country, group_id);
			delete_army_group(im, *country, group_id);
			return;
		}
		set_army_group_modes(*country, group_id, 3, 1);
		bool attacked = false;
		for (int day = 0; day < 45 && target->get_controller() != country; ++day) {
			// The test checks the executor, not the supply: the divisions of the group are kept supplied (a small country at war, its divisions
			// gathered at the border, ran out of supply in some runs and could not win anything)
			for (ArmyInstance& army : country->get_armies()) {
				if (army.get_army_group_id() == group_id) {
					army.set_supply_level(fixed_point_t::_1);
				}
			}
			im.force_tick_and_update();
			for (ArmyInstance const& army : country->get_armies()) {
				attacked |= army.get_army_group_id() == group_id && (&army.get_location() == target
					|| (army.is_moving() && &static_cast<ProvinceInstance const&>(army.get_path().back()) == target));
			}
		}
		std::size_t next_to_target = 0;
		front = war_manager.get_front_by_id(front_id);
		if (front != nullptr) {
			for (ArmyInstance const& army : country->get_armies()) {
				for (FrontEdge const& edge : front->edges) {
					if (army.get_army_group_id() == group_id && edge.other == target && edge.own == &army.get_location()) {
						next_to_target++;
						break;
					}
				}
			}
		}
		// The regiments that defend the target and the ones of the group next to it (an attack needs a chance of victory)
		std::size_t defenders = 0, group_regiments = 0;
		for (ArmyInstance const& army : target->get_armies()) {
			defenders += &army.get_country() != country ? army.get_unit_count() : 0;
		}
		std::size_t waiting_for_ships = 0, moving = 0;
		std::string state_of_group;
		for (ArmyInstance const& army : country->get_armies()) {
			if (army.get_army_group_id() == group_id) {
				fixed_point_t organisation = fixed_point_t::_0, max_organisation = fixed_point_t::_0, attack = fixed_point_t::_0;
				for (RegimentInstance const& regiment : army.get_regiment_instances()) {
					organisation += regiment.get_organisation();
					max_organisation += regiment.get_max_organisation();
					attack += regiment.get_weapon_attack_ratio();
				}
				state_of_group += fmt::format(" [org {:.1f}/{:.1f} supply {:.2f} weapons {:.2f} cut {}]", static_cast<double>(organisation),
					static_cast<double>(max_organisation), static_cast<double>(army.get_supply_level()), static_cast<double>(attack), army.is_cut_off());
			}
		}
		for (ArmyInstance const& army : country->get_armies()) {
			group_regiments += army.get_army_group_id() == group_id ? army.get_unit_count() : 0;
			waiting_for_ships += army.get_army_group_id() == group_id && army.has_sea_transfer();
			moving += army.get_army_group_id() == group_id && army.is_moving();
		}
		r.check(attacked, "offensive: the executed plan sends divisions into its target",
			fmt::format("{} defenders {} regiments of the group {} (way by sea {}, moving {}); at war {} front {} armies {} next to the target {} mode {} country {} target controller {}",
				state_of_group, defenders, group_regiments, waiting_for_ships, moving, country->is_at_war_with(*neighbour), front != nullptr,
				front != nullptr ? front->army_ids.size() : 0, next_to_target, front != nullptr ? static_cast<int>(front->mode) : -1, country->get_identifier(),
				target->get_controller() != nullptr ? target->get_controller()->get_identifier() : "none"));
		std::string outside_names;
		for (ProvinceInstance const& province : map_instance.get_province_instances()) {
			if (province.get_controller() == country && province.get_owner() != country
				&& std::find(ours_before.begin(), ours_before.end(), &province) == ours_before.end()
				&& std::find(zone.begin(), zone.end(), &province) == zone.end()) {
				outside_names += fmt::format(" {} (owner {}, armies of the country there:", province.get_identifier(),
					province.get_owner() != nullptr ? province.get_owner()->get_identifier() : "none");
				for (ArmyInstance const& army : province.get_armies()) {
					if (&army.get_country() == country) {
						outside_names += fmt::format(" {} group {} manual {}", army.unique_id, army.get_army_group_id(), army.is_manual_order());
					}
				}
				outside_names += ")";
			}
		}
		r.check(gained_outside_zone() == 0, "offensive: no land beyond the zone of the plan is taken",
			fmt::format("{} provinces{} country {}", gained_outside_zone(), outside_names, country->get_identifier()));
		fmt::println("TEST INFO offensive execution: target taken {}", target->get_controller() == country);
		if (War* const war = war_manager.find_war_between(*country, *neighbour); war != nullptr) {
			war_manager.end_war(im, war->id);
		}
		war_manager.delete_group_lines(im, *country, group_id);
		delete_army_group(im, *country, group_id);
	}

	// ---- the concentration (docs/WAR_ASSUMPTIONS.md, section 12): the division next to the target does not win alone, the free divisions of the line do:
	// they gather in the province of the line next to the target

	void test_concentration(Results& r, InstanceManager& im) {
		WarManager& war_manager = im.get_war_manager();
		MapInstance& map_instance = im.get_map_instance();
		UnitInstanceManager& units = im.get_unit_instance_manager();
		CountryInstance* country = nullptr;
		CountryInstance* neighbour = nullptr;
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (!candidate.exists() || candidate.is_rebel_country() || !candidate.has_army_command() || candidate.is_at_war() || !candidate.get_army_groups().empty()) {
				continue;
			}
			std::size_t divisions = 0;
			for (ArmyInstance const& army : candidate.get_armies()) {
				divisions += army.get_transport_navy() == nullptr ? army.get_divisions().size() : 0;
			}
			if (divisions < 4) {
				continue;
			}
			for (ProvinceInstance* province : candidate.get_owned_provinces()) {
				for (ProvinceDefinition::adjacency_t const& adjacency : province->province_definition.get_adjacencies()) {
					CountryInstance* const other = map_instance.get_province_instance_by_definition(adjacency.get_to()).get_controller();
					if (neighbour == nullptr && adjacency.get_type() == ProvinceDefinition::adjacency_t::type_t::LAND && other != nullptr && other != &candidate
						&& !other->is_at_war() && !other->is_rebel_country() && !other->has_alliance_with(candidate) && other->has_armies()) {
						neighbour = other;
					}
				}
			}
			if (neighbour != nullptr) {
				country = &candidate;
				break;
			}
		}
		if (country == nullptr || !war_manager.declare_war(im, *country, *neighbour)) {
			fmt::println("TEST INFO concentration: no country with four divisions next to a neighbour with an army, not checked");
			return;
		}
		const std::size_t palette_size = im.definition_manager.get_military_manager().get_command_palette().size();
		create_army_group(*country, palette_size);
		const std::uint32_t group_id = country->get_army_groups().back().id;
		const std::uint32_t front_id = war_manager.create_border_front(im, *country, group_id, *neighbour);
		Front const* front = war_manager.get_front_by_id(front_id);
		ProvinceInstance* target = nullptr;
		ProvinceInstance* near = nullptr;
		ProvinceInstance* far = nullptr;
		if (front != nullptr && !front->edges.empty()) {
			target = front->edges.front().other;
			near = front->edges.front().own;
			for (ProvinceInstance* province : front->provinces) {
				bool next_to = false;
				for (FrontEdge const& edge : front->edges) {
					next_to |= edge.own == province && edge.other == target;
				}
				if (far == nullptr && !next_to) {
					far = province;
				}
			}
		}
		ArmyInstance* defender = nullptr;
		for (ArmyInstance& army : neighbour->get_armies()) {
			if (defender == nullptr && army.get_transport_navy() == nullptr && !army.empty()) {
				defender = &army;
			}
		}
		bool marshal = false;
		for (LeaderInstance const& leader : country->get_leaders<unit_branch_t::LAND>()) {
			marshal = marshal || (!leader.is_marshal() && leader.get_can_be_used() && set_army_group_marshal(im, *country, group_id, leader.unique_id));
		}
		if (far == nullptr || defender == nullptr || !marshal) {
			fmt::println("TEST INFO concentration: no province of the line away from the target, no enemy army or no field marshal, not checked");
		} else {
			// The enemy army defends the target; one division of ours stands next to it, the others on the line away from it
			defender->clear_path();
			defender->set_location(*target);
			std::vector<ArmyInstance*> ours;
			for (ArmyInstance& army : country->get_armies()) {
				if (army.get_transport_navy() == nullptr && !army.empty()) {
					ours.push_back(&army);
				}
			}
			for (std::size_t index = 0; index < ours.size(); ++index) {
				while (ours[index]->get_divisions().size() > 1) {
					ArmyInstance* const split = split_division_unit(units, *ours[index]);
					if (split == nullptr) {
						break;
					}
					ours.push_back(split);
				}
			}
			const std::uint32_t max_attached = get_marshal_max_attached(im, *country);
			for (std::size_t index = 0; index < ours.size(); ++index) {
				ours[index]->clear_path();
				ours[index]->set_location(index == 0 ? *near : *far);
				set_army_group(*country, *ours[index], group_id, max_attached);
			}
			war_manager.create_plan_line(im, *country, group_id, PlanLine::type_t::OFFENSIVE, *target);
			// The careful mode: PLAN_MIN_HOLD keeps the only division next to the target in its province
			set_army_group_modes(*country, group_id, 1, 0);
			set_army_group_plan_active(im, *country, group_id, true);
			im.force_tick_and_update();
			ArmyGroup const* const group = country->find_army_group(group_id);
			front = war_manager.get_front_by_id(front_id);
			std::size_t to_focus = 0;
			for (ArmyInstance const* army : ours) {
				to_focus += front != nullptr && front->focus != nullptr && army->is_moving() && &static_cast<ProvinceInstance const&>(army->get_path().back()) == front->focus;
			}
			const std::uint32_t status = group != nullptr ? group->plan_status : 0;
			if (front != nullptr && front->focus_target == target) {
				r.check(to_focus > 0 && to_focus < ours.size(),
					"concentration: the division next to the target does not win alone, the divisions from afar go to the province of the line next to it",
					fmt::format("{} of {} go to the focus", to_focus, ours.size()));
			} else {
				fmt::println("TEST INFO concentration: no gathering against the target, the plan status is {} (1: attacks, 6 or 8: all of them lose), not checked", status);
			}
		}
		if (War* const war = war_manager.find_war_between(*country, *neighbour); war != nullptr) {
			war_manager.end_war(im, war->id);
		}
		war_manager.delete_group_lines(im, *country, group_id);
		delete_army_group(im, *country, group_id);
	}

	// ---- the bonus of the planning (P6, docs/WAR_ASSUMPTIONS.md, section 6): it grows while the order waits, falls while it is executed, is gone without
	// an order; a division moving by a manual order has none; the modifiers exist

	void test_planning(Results& r, InstanceManager& im) {
		WarManager& war_manager = im.get_war_manager();
		MapInstance& map_instance = im.get_map_instance();
		ModifierEffectCache const& cache = im.definition_manager.get_modifier_manager().get_modifier_effect_cache();
		r.check(cache.get_planning_max_add() != nullptr && cache.get_planning_speed_mult() != nullptr, "planning: the modifiers planning_max_add and planning_speed_mult exist");
		CountryInstance* country = nullptr;
		CountryInstance* neighbour = nullptr;
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (!candidate.exists() || candidate.is_rebel_country() || !candidate.has_army_command() || !candidate.get_army_groups().empty() || candidate.get_armies().empty()) {
				continue;
			}
			for (ProvinceInstance* province : candidate.get_owned_provinces()) {
				for (ProvinceDefinition::adjacency_t const& adjacency : province->province_definition.get_adjacencies()) {
					CountryInstance* const other = map_instance.get_province_instance_by_definition(adjacency.get_to()).get_controller();
					if (neighbour == nullptr && adjacency.get_type() == ProvinceDefinition::adjacency_t::type_t::LAND && other != nullptr && other != &candidate) {
						neighbour = other;
					}
				}
			}
			if (neighbour != nullptr) {
				country = &candidate;
				break;
			}
		}
		if (country == nullptr) {
			fmt::println("TEST INFO planning: no country with a neighbour, not checked");
			return;
		}
		const std::size_t palette_size = im.definition_manager.get_military_manager().get_command_palette().size();
		create_army_group(*country, palette_size);
		const std::uint32_t group_id = country->get_army_groups().back().id;
		bool has_marshal = false;
		for (LeaderInstance const& leader : country->get_leaders<unit_branch_t::LAND>()) {
			if (!leader.is_marshal() && leader.get_can_be_used() && set_army_group_marshal(im, *country, group_id, leader.unique_id)) {
				has_marshal = true;
				break;
			}
		}
		if (!has_marshal) {
			fmt::println("TEST INFO planning: no free general for a field marshal, not checked");
			delete_army_group(im, *country, group_id);
			return;
		}
		const auto planning = [&]() -> fixed_point_t {
			return country->find_army_group(group_id)->planning;
		};
		const fixed_point_t max = get_planning_max(im, *country);
		const fixed_point_t gain = get_planning_gain(im, *country);
		update_army_groups(im, *country);
		r.check(planning() == fixed_point_t::_0, "planning: no order, no bonus");
		war_manager.create_border_front(im, *country, group_id, *neighbour);
		for (int day = 0; day < 5; ++day) {
			update_army_groups(im, *country);
		}
		r.check(planning() == gain * 5, "planning: the bonus grows by the gain a day while the order waits", fmt::format("{} after 5 days, the gain {}", planning(), gain));
		for (int day = 0; day < 60; ++day) {
			update_army_groups(im, *country);
		}
		r.check(planning() == max && max >= fixed_point_t(3) / 10, "planning: the bonus stops at its maximum (30 % and the modifier)", fmt::format("{} of {}", planning(), max));
		// An army of the group has it, not while it moves by a manual order
		ArmyInstance& army = country->get_armies()[0].get();
		const std::uint32_t group_before = army.get_army_group_id();
		set_army_group(*country, army, group_id, get_marshal_max_attached(im, *country));
		// (the skill of the planning of the marshal, section 7: the bonus is worth planning_effect, it falls slower; an earlier test war may have taught him)
		const fixed_point_t effect = country->find_army_group(group_id)->planning_effect;
		r.check(get_army_planning(army) == max * effect, "planning: the divisions of the marshal have the bonus in a battle",
			fmt::format("{} of {} x {}", get_army_planning(army), max, effect));
		army.set_manual_order(true);
		r.check(get_army_planning(army) == fixed_point_t::_0, "planning: a division moving by a manual order has no bonus");
		army.set_manual_order(false);
		set_army_group(*country, army, group_before, get_marshal_max_attached(im, *country));
		// Executed: it falls
		set_army_group_plan_active(im, *country, group_id, true);
		for (int day = 0; day < 5; ++day) {
			update_army_groups(im, *country);
		}
		const fixed_point_t decay = im.definition_manager.get_define_manager().get_openvic_defines().war.planning_decay_per_day.of(fixed_point_t::_1);
		const fixed_point_t slower = std::max(fixed_point_t::_0, fixed_point_t(2) - effect);
		r.check(planning() == max - decay * slower * 5, "planning: the bonus falls by the decay a day while the plan is executed",
			fmt::format("{}, the effect {}", planning(), effect));
		war_manager.delete_group_lines(im, *country, group_id);
		update_army_groups(im, *country);
		r.check(planning() == fixed_point_t::_0, "planning: the order deleted, the bonus is gone");
		delete_army_group(im, *country, group_id);
	}

	// ---- the breakthrough (P7, docs/WAR_ASSUMPTIONS.md, section 6): a chain into the enemy land from the front, the strongest share of the divisions,
	// they gather at its start and go along it while the plan is executed; it lives next to the offensive

	void test_breakthrough(Results& r, InstanceManager& im) {
		WarManager& war_manager = im.get_war_manager();
		MapInstance& map_instance = im.get_map_instance();
		CountryInstance* country = nullptr;
		CountryInstance* neighbour = nullptr;
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (!candidate.exists() || candidate.is_rebel_country() || !candidate.has_army_command() || candidate.is_at_war() || !candidate.get_army_groups().empty()) {
				continue;
			}
			for (ArmyInstance& army : candidate.get_armies()) {
				if (army.get_transport_navy() != nullptr || army.is_moving() || army.get_location().get_controller() != &candidate || army.get_divisions().size() < 2) {
					continue;
				}
				for (ProvinceDefinition::adjacency_t const& adjacency : army.get_location().province_definition.get_adjacencies()) {
					CountryInstance* const other = map_instance.get_province_instance_by_definition(adjacency.get_to()).get_controller();
					if (neighbour == nullptr && adjacency.get_type() == ProvinceDefinition::adjacency_t::type_t::LAND && other != nullptr && other != &candidate
						&& !other->is_at_war() && !other->is_rebel_country() && !other->has_alliance_with(candidate)) {
						neighbour = other;
					}
				}
				if (neighbour != nullptr) {
					break;
				}
			}
			if (neighbour != nullptr) {
				country = &candidate;
				break;
			}
		}
		if (country == nullptr) {
			fmt::println("TEST INFO breakthrough: no country with an army next to a neighbour at peace, not checked");
			return;
		}
		const std::size_t palette_size = im.definition_manager.get_military_manager().get_command_palette().size();
		create_army_group(*country, palette_size);
		const std::uint32_t group_id = country->get_army_groups().back().id;
		const std::uint32_t max_attached = get_marshal_max_attached(im, *country);
		for (ArmyInstance& army : country->get_armies()) {
			if (army.get_transport_navy() == nullptr) {
				set_army_group(*country, army, group_id, max_attached);
			}
		}
		const std::uint32_t front_id = war_manager.create_border_front(im, *country, group_id, *neighbour);
		Front const* const front = war_manager.get_front_by_id(front_id);
		if (front == nullptr || front->edges.empty()) {
			fmt::println("TEST INFO breakthrough: no front, not checked");
			delete_army_group(im, *country, group_id);
			return;
		}
		ProvinceInstance* const first = front->edges.front().other;
		r.check(war_manager.create_plan_line(im, *country, group_id, PlanLine::type_t::BREAKTHROUGH, *front->provinces.front()) == 0,
			"breakthrough: its way goes over the land of the others");
		const std::uint32_t offensive_id = war_manager.create_plan_line(im, *country, group_id, PlanLine::type_t::OFFENSIVE, *first);
		const std::uint32_t line_id = war_manager.create_plan_line(im, *country, group_id, PlanLine::type_t::BREAKTHROUGH, *first);
		r.check(line_id != 0 && war_manager.get_plan_line_by_id(offensive_id) != nullptr, "breakthrough: it lives next to the offensive of the front (one order)");
		// The way two provinces deep: a gap is filled over the land of the others
		ProvinceInstance* deep = nullptr;
		for (ProvinceDefinition::adjacency_t const& adjacency : first->province_definition.get_adjacencies()) {
			ProvinceInstance& step = map_instance.get_province_instance_by_definition(adjacency.get_to());
			if (adjacency.get_type() != ProvinceDefinition::adjacency_t::type_t::LAND || step.get_controller() == country || step.province_definition.is_water()) {
				continue;
			}
			for (ProvinceDefinition::adjacency_t const& second : step.province_definition.get_adjacencies()) {
				ProvinceInstance& far = map_instance.get_province_instance_by_definition(second.get_to());
				if (deep == nullptr && second.get_type() == ProvinceDefinition::adjacency_t::type_t::LAND && &far != first && far.get_controller() != country
					&& !far.province_definition.is_water() && !first->province_definition.is_adjacent_to(far.province_definition)) {
					deep = &far;
				}
			}
		}
		if (deep != nullptr) {
			r.check(war_manager.extend_plan_line(im, *country, line_id, true, *deep) && war_manager.get_plan_line_by_id(line_id)->provinces.size() == 3,
				"breakthrough: the way to a far province is a chain of neighbours");
		}
		// (the first day splits the armies along the front, the second gives the breakthrough its divisions)
		im.force_tick_and_update();
		im.force_tick_and_update();
		PlanLine const* const line = war_manager.get_plan_line_by_id(line_id);
		std::size_t divisions = 0;
		for (ArmyInstance const& army : country->get_armies()) {
			divisions += army.get_army_group_id() == group_id ? army.get_divisions().size() : 0;
		}
		bool off_front = line != nullptr && !line->army_ids.empty();
		Front const* const after = war_manager.get_front_by_id(front_id);
		for (const unique_id_t army_id : line != nullptr ? line->army_ids : memory::vector<unique_id_t> {}) {
			off_front &= std::find(after->army_ids.begin(), after->army_ids.end(), army_id) == after->army_ids.end();
		}
		r.check(off_front && line->army_ids.size() <= divisions, "breakthrough: it takes divisions of the marshal, they are not on the front",
			fmt::format("{} of {} divisions", line != nullptr ? line->army_ids.size() : 0, divisions));
		war_manager.delete_group_lines(im, *country, group_id);
		delete_army_group(im, *country, group_id);
	}

	// ---- the encirclements (P8, docs/WAR_ASSUMPTIONS.md, section 6): an army deep in the land of its enemy is cut off, gets no supply, loses its
	// organisation and gives itself up; at peace nobody is cut off

	void test_encirclement(Results& r, InstanceManager& im) {
		WarManager& war_manager = im.get_war_manager();
		MapInstance& map_instance = im.get_map_instance();
		OpenVicWarDefines const& defines = im.definition_manager.get_define_manager().get_openvic_defines().war;
		// Two neighbours at peace; an army of the second one and a province of the first one whose land neighbours are all its own (deep in its land)
		CountryInstance* country = nullptr;
		CountryInstance* enemy = nullptr;
		ProvinceInstance* deep = nullptr;
		const auto interior = [&](CountryInstance const& owner, ProvinceInstance const& province) -> bool {
			if (province.get_controller() != &owner || province.province_definition.is_water() || &province == owner.get_capital() || !province.get_armies().empty()) {
				return false;
			}
			std::int32_t land = 0;
			for (ProvinceDefinition::adjacency_t const& adjacency : province.province_definition.get_adjacencies()) {
				ProvinceInstance const& next = map_instance.get_province_instance_by_definition(adjacency.get_to());
				if (adjacency.get_type() == ProvinceDefinition::adjacency_t::type_t::LAND && !next.province_definition.is_water()) {
					land++;
					if (next.get_controller() != &owner) {
						return false;
					}
					for (ProvinceDefinition::adjacency_t const& second : next.province_definition.get_adjacencies()) {
						ProvinceInstance const& far = map_instance.get_province_instance_by_definition(second.get_to());
						if (second.get_type() == ProvinceDefinition::adjacency_t::type_t::LAND && !far.province_definition.is_water() && far.get_controller() != &owner) {
							return false;
						}
					}
				}
			}
			return land >= 2;
		};
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (!candidate.exists() || candidate.is_rebel_country() || candidate.is_at_war()) {
				continue;
			}
			ProvinceInstance* found_deep = nullptr;
			for (ProvinceInstance* province : candidate.get_owned_provinces()) {
				if (interior(candidate, *province)) {
					found_deep = province;
					break;
				}
			}
			if (found_deep == nullptr) {
				continue;
			}
			// A neighbour with an army that can declare the war (no truce)
			for (ProvinceInstance* province : candidate.get_owned_provinces()) {
				for (ProvinceDefinition::adjacency_t const& adjacency : province->province_definition.get_adjacencies()) {
					CountryInstance* const other = map_instance.get_province_instance_by_definition(adjacency.get_to()).get_controller();
					if (enemy == nullptr && adjacency.get_type() == ProvinceDefinition::adjacency_t::type_t::LAND && other != nullptr && other != &candidate
						&& !other->is_at_war() && !other->is_rebel_country() && !other->has_alliance_with(candidate) && other->has_armies()
						&& war_manager.declare_war(im, *other, candidate)) {
						enemy = other;
					}
				}
			}
			if (enemy != nullptr) {
				country = &candidate;
				deep = found_deep;
				break;
			}
		}
		if (country == nullptr) {
			fmt::println("TEST INFO encirclement: no country with a province deep in its land and a neighbour that can make war on it, not checked");
			return;
		}
		ArmyInstance* army = nullptr;
		for (ArmyInstance& candidate : enemy->get_armies()) {
			if (candidate.get_transport_navy() == nullptr && !candidate.is_in_combat()) {
				army = &candidate;
				break;
			}
		}
		if (army == nullptr) {
			fmt::println("TEST INFO encirclement: no army, not checked");
			return;
		}
		const unique_id_t army_id = army->unique_id;
		army->clear_path();
		army->set_location(*deep);
		im.force_tick_and_update();
		army = static_cast<ArmyInstance*>(im.get_unit_instance_manager().get_unit_instance_group_by_unique_id(army_id));
		r.check(army != nullptr && army->is_encircled() && army->is_cut_off(), "encirclement: an army deep in the land of its enemy is cut off from the supply, in a pocket");
		if (army != nullptr) {
			im.force_tick_and_update();
			r.check(army->get_supply_delivery() == fixed_point_t::_0, "encirclement: nothing of the supply arrives to it");
			// Without organisation it gives itself up after ENCIRCLED_SURRENDER_DAYS days
			for (RegimentInstance& regiment : army->get_regiment_instances()) {
				regiment.take_damage(regiment.get_organisation(), fixed_point_t::_0);
			}
			bool gone = false;
			for (std::int32_t day = 0; day <= defines.encircled_surrender_days + 1 && !gone; ++day) {
				im.force_tick_and_update();
				gone = im.get_unit_instance_manager().get_unit_instance_group_by_unique_id(army_id) == nullptr;
			}
			r.check(gone, "encirclement: without organisation the army cut off gives itself up");
		}
		if (War* const war = war_manager.find_war_between(*country, *enemy); war != nullptr) {
			war_manager.end_war(im, war->id);
		}
		im.force_tick_and_update();
		bool none_cut_off = true;
		for (ArmyInstance const& other : enemy->get_armies()) {
			none_cut_off &= !other.is_encircled() || enemy->is_at_war();
		}
		r.check(none_cut_off, "encirclement: at peace nobody is cut off");
	}

	// ---- the fallback (P9, docs/WAR_ASSUMPTIONS.md, section 6): a worn out division on the line next to the enemy falls back behind it, recovers and goes
	// back to the line

	void test_fallback(Results& r, InstanceManager& im) {
		WarManager& war_manager = im.get_war_manager();
		MapInstance& map_instance = im.get_map_instance();
		CountryInstance* country = nullptr;
		CountryInstance* neighbour = nullptr;
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (!candidate.exists() || candidate.is_rebel_country() || !candidate.has_army_command() || candidate.is_at_war() || !candidate.get_army_groups().empty()) {
				continue;
			}
			for (ArmyInstance& army : candidate.get_armies()) {
				if (army.get_transport_navy() != nullptr || army.is_moving() || army.get_location().get_controller() != &candidate || army.get_divisions().size() < 2) {
					continue;
				}
				for (ProvinceDefinition::adjacency_t const& adjacency : army.get_location().province_definition.get_adjacencies()) {
					CountryInstance* const other = map_instance.get_province_instance_by_definition(adjacency.get_to()).get_controller();
					if (neighbour == nullptr && adjacency.get_type() == ProvinceDefinition::adjacency_t::type_t::LAND && other != nullptr && other != &candidate
						&& !other->is_at_war() && !other->is_rebel_country() && !other->has_alliance_with(candidate) && war_manager.declare_war(im, candidate, *other)) {
						neighbour = other;
					}
				}
				if (neighbour != nullptr) {
					break;
				}
			}
			if (neighbour != nullptr) {
				country = &candidate;
				break;
			}
		}
		if (country == nullptr) {
			fmt::println("TEST INFO fallback: no war of a country with an army next to its neighbour, not checked");
			return;
		}
		const std::size_t palette_size = im.definition_manager.get_military_manager().get_command_palette().size();
		create_army_group(*country, palette_size);
		const std::uint32_t group_id = country->get_army_groups().back().id;
		const std::uint32_t max_attached = get_marshal_max_attached(im, *country);
		for (ArmyInstance& army : country->get_armies()) {
			if (army.get_transport_navy() == nullptr) {
				set_army_group(*country, army, group_id, max_attached);
			}
		}
		const std::uint32_t front_id = war_manager.create_border_front(im, *country, group_id, *neighbour);
		for (int day = 0; day < 6; ++day) {
			im.force_tick_and_update();
		}
		Front const* front = war_manager.get_front_by_id(front_id);
		// A division standing on the line, worn out
		ArmyInstance* worn = nullptr;
		for (ArmyInstance& army : country->get_armies()) {
			if (front != nullptr && army.get_army_group_id() == group_id && !army.is_moving() && !army.is_in_combat() && !army.is_at_sea()
				&& std::find(front->provinces.begin(), front->provinces.end(), &army.get_location()) != front->provinces.end()) {
				worn = &army;
				break;
			}
		}
		if (worn == nullptr) {
			fmt::println("TEST INFO fallback: no division standing on the line, not checked");
		} else {
			const unique_id_t worn_id = worn->unique_id;
			for (RegimentInstance& regiment : worn->get_regiment_instances()) {
				regiment.take_damage(regiment.get_organisation() * 9 / 10, fixed_point_t::_0);
			}
			im.force_tick_and_update();
			worn = static_cast<ArmyInstance*>(im.get_unit_instance_manager().get_unit_instance_group_by_unique_id(worn_id));
			front = war_manager.get_front_by_id(front_id);
			const auto heading = [](ArmyInstance const& army) -> ProvinceInstance const* {
				return army.is_moving() ? &static_cast<ProvinceInstance const&>(army.get_path().back()) : &army.get_location();
			};
			const bool fell_back = worn != nullptr && heading(*worn)->get_controller() == country
				&& std::find(front->provinces.begin(), front->provinces.end(), heading(*worn)) == front->provinces.end();
			// A province behind the line: a land neighbour of the country off the line with no enemy next to it
			bool has_rear = false;
			bool enemy_by_land = false;
			if (worn != nullptr) {
				for (ProvinceDefinition::adjacency_t const& adjacency : worn->get_location().province_definition.get_adjacencies()) {
					CountryInstance const* const controller = map_instance.get_province_instance_by_definition(adjacency.get_to()).get_controller();
					enemy_by_land |= adjacency.get_type() == ProvinceDefinition::adjacency_t::type_t::LAND && controller != nullptr
						&& controller != country && country->is_at_war_with(*controller);
				}
				for (ProvinceDefinition::adjacency_t const& adjacency : worn->get_location().province_definition.get_adjacencies()) {
					ProvinceInstance const& next = map_instance.get_province_instance_by_definition(adjacency.get_to());
					if (adjacency.get_type() != ProvinceDefinition::adjacency_t::type_t::LAND || next.get_controller() != country
						|| std::find(front->provinces.begin(), front->provinces.end(), &next) != front->provinces.end()) {
						continue;
					}
					bool enemy_next = false;
					for (ProvinceDefinition::adjacency_t const& further : next.province_definition.get_adjacencies()) {
						CountryInstance const* const controller = map_instance.get_province_instance_by_definition(further.get_to()).get_controller();
						enemy_next |= further.get_type() == ProvinceDefinition::adjacency_t::type_t::LAND && controller != nullptr
							&& controller != country && country->is_at_war_with(*controller);
					}
					has_rear |= !enemy_next;
				}
			}
			// Attacked that day the division is in a battle: it leaves it by the retreat of the battle, not by the fallback
			if (worn != nullptr && worn->is_in_combat()) {
				fmt::println("TEST INFO fallback: the worn out division was attacked on the line (in a battle), the fallback is not checked");
			} else if (worn != nullptr && !fell_back && (!has_rear || !enemy_by_land)) {
				fmt::println("TEST INFO fallback: the line of {} has no province of its own behind it or no enemy next to it by land there, the fallback is not checked",
					country->get_identifier());
			} else r.check(fell_back, "fallback: a worn out division on the line next to the enemy falls back behind it",
				worn == nullptr ? std::string { "the division is gone" } : fmt::format(
					"{} at {} heading {} (controller {}), moving {}, in combat {}, on the line {}, organisation {:.2f} of {:.2f}, on the front list {}, manual {}, cut off {}", country->get_identifier(),
					worn->get_location().get_identifier(), heading(*worn)->get_identifier(),
					heading(*worn)->get_controller() != nullptr ? heading(*worn)->get_controller()->get_identifier() : std::string_view { "-" },
					worn->is_moving(), worn->is_in_combat(),
					std::find(front->provinces.begin(), front->provinces.end(), heading(*worn)) != front->provinces.end(),
					static_cast<double>(worn->get_total_organisation()), static_cast<double>(worn->get_total_max_organisation()),
					std::find(front->army_ids.begin(), front->army_ids.end(), worn->unique_id) != front->army_ids.end(),
					worn->is_manual_order(), worn->is_cut_off()));
			if (worn != nullptr && fell_back) {
				for (int day = 0; day < 10 && worn->is_moving(); ++day) {
					im.force_tick_and_update();
				}
				// Recovered: back to the line
				for (RegimentInstance& regiment : worn->get_regiment_instances()) {
					regiment.recover(regiment.get_max_organisation(), fixed_point_t::_0);
				}
				im.force_tick_and_update();
				front = war_manager.get_front_by_id(front_id);
				r.check(std::find(front->provinces.begin(), front->provinces.end(), heading(*worn)) != front->provinces.end() || worn->is_in_combat(),
					"fallback: a recovered division goes back to the line");
			}
		}
		if (War* const war = war_manager.find_war_between(*country, *neighbour); war != nullptr) {
			war_manager.end_war(im, war->id);
		}
		war_manager.delete_group_lines(im, *country, group_id);
		delete_army_group(im, *country, group_id);
	}

	// ---- the naval invasion (P10, docs/WAR_ASSUMPTIONS.md, section 6): the divisions gather in the port, the preparation counts the days, executed and
	// prepared the divisions board the fleet in the port and it sails; a landing of a plan takes less organisation

	void test_naval_invasion(Results& r, InstanceManager& im) {
		WarManager& war_manager = im.get_war_manager();
		MapInstance& map_instance = im.get_map_instance();
		OpenVicWarDefines const& defines = im.definition_manager.get_define_manager().get_openvic_defines().war;
		// A country with mechanic:command, a fleet with transports in a port of its own, and a foreign coast at the sea of the port or at a sea next to it
		// that the country can make war on
		CountryInstance* country = nullptr;
		NavyInstance* fleet = nullptr;
		ProvinceInstance* port = nullptr;
		ProvinceInstance* target = nullptr;
		CountryInstance* enemy = nullptr;
		const auto coast_near = [&](CountryInstance& owner, ProvinceDefinition const& sea) -> ProvinceInstance* {
			std::vector<ProvinceDefinition const*> seas { &sea };
			for (ProvinceDefinition::adjacency_t const& adjacency : sea.get_adjacencies()) {
				if (adjacency.get_to().is_water()) {
					seas.push_back(&adjacency.get_to());
				}
			}
			for (ProvinceDefinition const* const water : seas) {
				for (ProvinceDefinition::adjacency_t const& adjacency : water->get_adjacencies()) {
					ProvinceInstance& land = map_instance.get_province_instance_by_definition(adjacency.get_to());
					CountryInstance* const controller = land.get_controller();
					if (!land.province_definition.is_water() && controller != nullptr && controller != &owner && !controller->is_at_war() && !controller->is_rebel_country()
						&& !controller->has_alliance_with(owner) && war_manager.declare_war(im, owner, *controller)) {
						enemy = controller;
						return &land;
					}
				}
			}
			return nullptr;
		};
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (!candidate.exists() || candidate.is_rebel_country() || !candidate.has_army_command() || candidate.is_at_war() || !candidate.get_army_groups().empty()
				|| !candidate.has_armies()) {
				continue;
			}
			for (NavyInstance& navy : candidate.get_navies()) {
				ProvinceInstance& at = navy.get_location();
				if (at.province_definition.is_water() || !at.province_definition.has_port() || at.province_definition.get_port_adjacent_province() == nullptr
					|| at.get_controller() != &candidate || navy.get_transport_capacity() < 2 || navy.is_moving() || !navy.get_carried_armies().empty()) {
					continue;
				}
				target = coast_near(candidate, *at.province_definition.get_port_adjacent_province());
				if (target != nullptr) {
					fleet = &navy;
					port = &at;
					break;
				}
			}
			if (fleet != nullptr) {
				country = &candidate;
				break;
			}
		}
		if (country == nullptr) {
			fmt::println("TEST INFO naval invasion: no country with a fleet of transports in its port and a coast to make war on, not checked");
			return;
		}
		// The country plays as a player for the test: the AI of the fleets at war would sail the fleet out of the port
		const bool was_ai = country->is_ai();
		country->set_ai(false);
		const std::size_t palette_size = im.definition_manager.get_military_manager().get_command_palette().size();
		create_army_group(*country, palette_size);
		const std::uint32_t group_id = country->get_army_groups().back().id;
		for (LeaderInstance const& leader : country->get_leaders<unit_branch_t::LAND>()) {
			if (!leader.is_marshal() && leader.get_can_be_used() && set_army_group_marshal(im, *country, group_id, leader.unique_id)) {
				break;
			}
		}
		// One army of the country into the group, standing in the port already (the gathering is checked by the days below)
		ArmyInstance* army = nullptr;
		for (ArmyInstance& candidate : country->get_armies()) {
			if (candidate.get_transport_navy() == nullptr && !candidate.is_in_combat() && candidate.get_unit_count() <= fleet->get_transport_capacity()) {
				army = &candidate;
				break;
			}
		}
		if (army == nullptr) {
			fmt::println("TEST INFO naval invasion: no army small enough for the fleet, not checked");
			delete_army_group(im, *country, group_id);
			country->set_ai(was_ai);
			return;
		}
		const unique_id_t army_id = army->unique_id;
		const std::uint32_t group_before = army->get_army_group_id();
		set_army_group(*country, *army, group_id, get_marshal_max_attached(im, *country));
		army->clear_path();
		army->set_location(*port);
		const std::uint32_t line_id = war_manager.create_plan_line(im, *country, group_id, PlanLine::type_t::NAVAL, *port);
		war_manager.toggle_plan_line_province(im, *country, line_id, *target);
		for (int day = 0; day < 3; ++day) {
			im.force_tick_and_update();
		}
		PlanLine const* line = war_manager.get_plan_line_by_id(line_id);
		r.check(line != nullptr && line->prepared_days >= 3, "naval invasion: the days of the preparation are counted while it waits");
		const bool started = set_army_group_plan_active(im, *country, group_id, true);
		r.check(started, "naval invasion: it can be executed (an order of the marshal)");
		// Not prepared yet: nobody boards; prepared: the divisions in the port board the fleet
		im.force_tick_and_update();
		army = static_cast<ArmyInstance*>(im.get_unit_instance_manager().get_unit_instance_group_by_unique_id(army_id));
		r.check(army != nullptr && army->get_transport_navy() == nullptr, "naval invasion: before NAVAL_PLAN_PREP_DAYS of the preparation nobody boards");
		const_cast<PlanLine*>(war_manager.get_plan_line_by_id(line_id))->prepared_days = static_cast<std::uint32_t>(defines.naval_plan_prep_days);
		bool boarded = false, sailed = false, landed = false;
		int days_with_group = 0;
		for (int day = 0; day < 40 && !landed; ++day) {
			im.force_tick_and_update();
			bool any = false;
			for (ArmyInstance const& other : country->get_armies()) {
				if (other.get_army_group_id() != group_id) {
					continue;
				}
				any = true;
				boarded |= other.get_transport_navy() != nullptr;
				sailed |= other.get_transport_navy() != nullptr && &other.get_transport_navy()->get_location() != port;
				landed |= &other.get_location() == target && other.get_transport_navy() == nullptr;
			}
			if (!any) {
				break;
			}
			days_with_group++;
		}
		r.check(boarded, "naval invasion: executed and prepared, the divisions in the port board the fleet", fmt::format("{} from {}, {} days with armies in the group, fleet in {} with room {}, the army in {}",
			country->get_identifier(), port->get_identifier(), days_with_group, fleet->get_location().get_identifier(), fleet->get_transport_capacity(),
			im.get_unit_instance_manager().get_unit_instance_group_by_unique_id(army_id) != nullptr
				? im.get_unit_instance_manager().get_unit_instance_group_by_unique_id(army_id)->get_location().get_identifier() : std::string_view { "-" }));
		fmt::println("TEST INFO naval invasion: sailed {} landed {}", sailed, landed);
		if (War* const war = war_manager.find_war_between(*country, *enemy); war != nullptr) {
			war_manager.end_war(im, war->id);
		}
		war_manager.delete_group_lines(im, *country, group_id);
		if (ArmyInstance* const left = static_cast<ArmyInstance*>(im.get_unit_instance_manager().get_unit_instance_group_by_unique_id(army_id)); left != nullptr) {
			set_army_group(*country, *left, group_before, get_marshal_max_attached(im, *country));
		}
		delete_army_group(im, *country, group_id);
		country->set_ai(was_ai);
	}

	// ---- the field marshals of the AI countries (P11, docs/WAR_ASSUMPTIONS.md, section 6): on by default; an AI country at war makes a group, a
	// front and an offensive against the enemy it borders and executes it

	void test_ai_marshals(Results& r, InstanceManager& im) {
		WarManager& war_manager = im.get_war_manager();
		MapInstance& map_instance = im.get_map_instance();
		OpenVicWarDefines& defines = const_cast<OpenVicWarDefines&>(im.definition_manager.get_define_manager().get_openvic_defines().war);
		r.check(defines.plan_ai_enabled == 1, "ai marshals: on by default (the AI countries with the command fight by the plans of their marshals)");
		CountryInstance* country = nullptr;
		CountryInstance* enemy = nullptr;
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (!candidate.exists() || !candidate.is_ai() || candidate.is_rebel_country() || !candidate.has_army_command() || candidate.is_at_war()
				|| !candidate.get_army_groups().empty() || !candidate.has_armies()) {
				continue;
			}
			for (ProvinceInstance* province : candidate.get_owned_provinces()) {
				for (ProvinceDefinition::adjacency_t const& adjacency : province->province_definition.get_adjacencies()) {
					CountryInstance* const other = map_instance.get_province_instance_by_definition(adjacency.get_to()).get_controller();
					if (enemy == nullptr && adjacency.get_type() == ProvinceDefinition::adjacency_t::type_t::LAND && other != nullptr && other != &candidate
						&& !other->is_at_war() && !other->is_rebel_country() && !other->has_alliance_with(candidate) && war_manager.declare_war(im, candidate, *other)) {
						enemy = other;
					}
				}
			}
			if (enemy != nullptr) {
				country = &candidate;
				break;
			}
		}
		if (country == nullptr) {
			fmt::println("TEST INFO ai marshals: no AI country with command and a neighbour to make war on, not checked");
			return;
		}
		for (int day = 0; day < 12; ++day) {
			im.force_tick_and_update();
		}
		Front const* const front = !country->get_army_groups().empty() ? war_manager.find_front_of_group(*country, country->get_army_groups().front().id) : nullptr;
		bool offensive = false;
		for (PlanLine const& line : war_manager.get_plan_lines()) {
			offensive |= line.country == country && line.type == PlanLine::type_t::OFFENSIVE;
		}
		// Much weaker than the enemy (AI_MARSHAL_DEFEND_RATIO): no offensive, the front holds the line (section 12)
		const auto regiments_of = [](CountryInstance const& owner) -> std::int32_t {
			std::int32_t regiments = 0;
			for (ArmyInstance const& army : owner.get_armies()) {
				regiments += static_cast<std::int32_t>(army.get_unit_count());
			}
			return regiments;
		};
		const bool weak = fixed_point_t(regiments_of(*country)) / std::max(1, regiments_of(*enemy)) < defines.ai_marshal_defend_ratio.fixed();
		r.check(!country->get_army_groups().empty() && country->get_army_groups().front().ai_made && front != nullptr && front->enemy == enemy && offensive != weak,
			"ai marshals: on, an AI country at war makes a group, a front against its enemy and an offensive (none when much weaker)",
			fmt::format("{} groups, weak {}", country->get_army_groups().size(), weak));
		// The stances (section 12): every army of the group has one of the AI (entrench, raid or rest)
		std::size_t in_group = 0, with_stance = 0;
		for (ArmyInstance const& army : country->get_armies()) {
			if (!country->get_army_groups().empty() && army.get_army_group_id() == country->get_army_groups().front().id && !army.empty()) {
				in_group++;
				with_stance += army.get_stance() != ArmyInstance::STANCE_NORMAL;
			}
		}
		r.check(in_group > 0 && with_stance == in_group, "ai marshals: the armies of the group of the AI get stances", fmt::format("{} of {}", with_stance, in_group));
		// The breakthrough (section 13): with the thresholds open the marshal draws one from his front to the capital of the enemy; one that stands is
		// given up and the next one waits for the pause
		const Rate kept_ratio = defines.ai_breakthrough_ratio;
		const std::int32_t kept_divisions = defines.ai_breakthrough_min_divisions;
		const auto breakthrough_of = [&]() -> PlanLine* {
			for (PlanLine const& line : war_manager.get_plan_lines()) {
				if (line.country == country && line.type == PlanLine::type_t::BREAKTHROUGH) {
					return const_cast<PlanLine*>(&line);
				}
			}
			return nullptr;
		};
		const auto to_marshals = [&]() -> void {
			do {
				im.force_tick_and_update();
			} while (im.get_today().get_day() % 10 != 0);
		};
		defines.ai_breakthrough_ratio = Rate(0, 1);
		defines.ai_breakthrough_min_divisions = 1;
		const memory::vector<ProvinceInstance*> way = front != nullptr && enemy->get_capital() != nullptr
			? war_manager.get_ai_breakthrough_way(im, *country, *war_manager.find_front_of_group(*country, country->get_army_groups().front().id), *enemy)
			: memory::vector<ProvinceInstance*> {};
		if (way.empty()) {
			fmt::println("TEST INFO ai marshals: the capital of {} is not reachable over its land, the breakthrough not checked", enemy->get_identifier());
		} else {
			bool chain = way.back() == enemy->get_capital();
			for (std::size_t index = 1; index < way.size(); ++index) {
				chain &= way[index - 1]->province_definition.is_adjacent_to(way[index]->province_definition);
			}
			to_marshals();
			PlanLine* const line = breakthrough_of();
			r.check(chain && line != nullptr && !line->provinces.empty() && line->provinces.back() == enemy->get_capital(),
				"ai marshals: a strong marshal draws a breakthrough, a chain of neighbours from his front to the capital of the enemy",
				fmt::format("way {} line {}", way.size(), line != nullptr ? line->provinces.size() : 0));
			if (line != nullptr) {
				line->stalled_days = static_cast<std::uint32_t>(defines.ai_breakthrough_stall_days);
				line->progress = static_cast<std::uint32_t>(line->provinces.size());
			}
			to_marshals();
			ArmyGroup const* const group = country->get_army_groups().empty() ? nullptr : &country->get_army_groups().front();
			r.check(breakthrough_of() == nullptr && group != nullptr && group->ai_breakthrough_after > im.get_today(),
				"ai marshals: a breakthrough that stands is given up, the next one waits for the pause");
		}
		defines.ai_breakthrough_ratio = kept_ratio;
		defines.ai_breakthrough_min_divisions = kept_divisions;
		if (War* const war = war_manager.find_war_between(*country, *enemy); war != nullptr) {
			war_manager.end_war(im, war->id);
		}
		std::vector<std::uint32_t> group_ids;
		for (ArmyGroup const& group : country->get_army_groups()) {
			group_ids.push_back(group.id);
		}
		for (const std::uint32_t group_id : group_ids) {
			delete_army_group(im, *country, group_id);
		}
	}

	void test_ai_war_captures(Results& r, InstanceManager& im) {
		WarManager& war_manager = im.get_war_manager();
		std::size_t held = 0;
		for (auto const& [first, second] : { std::pair { "RUS", "TUR" } }) {
			CountryInstance* a = im.get_country_instance_manager().get_country_instance_by_identifier(first);
			CountryInstance* b = im.get_country_instance_manager().get_country_instance_by_identifier(second);
			if (a == nullptr || b == nullptr || a->is_at_war_with(*b) || !war_manager.declare_war(im, *a, *b)) {
				continue;
			}
			// Plenty of the goods of its first template in the stockpile of the first: the AI recruits a whole division (docs/RECRUITMENT.md)
			UnitTypeManager const& unit_type_manager = im.definition_manager.get_military_manager().get_unit_type_manager();
			if (!a->get_division_templates().empty()) {
				for (const std::uint8_t cell : a->get_division_templates().front().cells) {
					if (RegimentType const* const type = cell != 0 ? unit_type_manager.get_regiment_type_by_index(regiment_type_index_t(cell - 1)) : nullptr) {
						for (auto const& [good, quantity] : type->get_build_cost()) {
							a->get_good_data(*good).stockpile_amount += quantity * 1000; // the army eats some of the goods every day
						}
					}
				}
			}
			// The armies of the start take all the manpower (they were fitted to it): a pop of soldiers gets the men of a few divisions
			for (ProvinceInstance* province : a->get_owned_provinces()) {
				Pop* soldiers = nullptr;
				for (Pop const& pop : province->get_pops()) {
					if (pop.get_type().can_be_recruited && Pop::is_culture_status_allowed(a->get_allowed_regiment_cultures(), pop.get_culture_status())) {
						soldiers = province->find_pop_by_id(pop.id_in_province);
						break;
					}
				}
				if (soldiers != nullptr) {
					soldiers->change_size(90000, 0);
					break;
				}
			}
			const bool had_manpower = im.get_unit_instance_manager().get_manpower(*a).available_regiments >= 12;
			const std::string ai_reason = a->get_division_templates().empty() || a->get_capital() == nullptr ? std::string { "no template or capital" }
				: im.get_unit_instance_manager().get_division_recruitment_block_reason(*a, unit_type_manager, a->get_division_templates().front(), *a->get_capital());
			// The AI recruits only below the regiments it wants: it wants more for the test
			OpenVicAIDefines& ai_defines = const_cast<OpenVicAIDefines&>(im.definition_manager.get_define_manager().get_openvic_defines().ai);
			const auto wanted_before = ai_defines.min_war_regiments;
			ai_defines.min_war_regiments = 100000;
			for (int day = 0; day < 60; ++day) {
				im.force_tick_and_update();
			}
			ai_defines.min_war_regiments = wanted_before;
			std::size_t division_orders = 0;
			for (RecruitmentOrder const& order : im.get_unit_instance_manager().get_recruitment_orders()) {
				division_orders += order.country == a && order.division_id != 0;
			}
			if (had_manpower) {
				r.check(division_orders > 0, "AI recruitment: an AI country at war with the goods and the manpower recruits a whole division by its template",
					fmt::format("{} regiments of divisions ordered; at the start: {}; ai {}", division_orders, ai_reason, a->is_ai()));
			} else {
				fmt::println("TEST INFO AI recruitment: {} has no manpower for a division, not checked", a->get_identifier());
			}
			for (ProvinceInstance const* province : a->get_owned_provinces()) {
				held += province->get_controller() == b;
			}
			for (ProvinceInstance const* province : b->get_owned_provinces()) {
				held += province->get_controller() == a;
			}
		}
		r.check(held > 0, "live battles: the AI countries at war take provinces of each other (Russia and Turkey, 60 days)", fmt::format("{} provinces", held));
	}

	// World trade: the routes by land and sea exist and are the same both ways, the freight of the sellers reaches the treasuries, the
	// workers keep part of the output, and the people who are hungry for a month and have no job emigrate or die
	// Fronts (docs/PLAN_ARMY_SUPPLY_FRONTS.md, section 5): the divisions of the armies of a front spread over its line, one army for every province
	// of the line, and a division keeps its template when it leaves its army
	void test_fronts(Results& r, InstanceManager& im) {
		OpenVicWarDefines& war_defines = const_cast<OpenVicWarDefines&>(im.definition_manager.get_define_manager().get_openvic_defines().war);
		const auto splits_before = war_defines.front_spread_max_splits_per_day;
		war_defines.front_spread_max_splits_per_day = 10;

		CountryInstance* country = nullptr;
		ArmyInstance* army = nullptr;
		std::vector<ProvinceInstance*> line;
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (!candidate.exists() || !candidate.has_army_command() || candidate.is_at_war()) {
				continue;
			}
			line.clear();
			for (ProvinceInstance* province : candidate.get_owned_provinces()) {
				if (!province->province_definition.is_water() && province->get_controller() == &candidate && line.size() < 3) {
					line.push_back(province);
				}
			}
			if (line.size() < 3) {
				continue;
			}
			for (ArmyInstance& candidate_army : candidate.get_armies()) {
				if (count_division_units(candidate_army) >= 3 && !candidate_army.is_in_combat() && !candidate_army.is_moving()) {
					country = &candidate;
					army = &candidate_army;
					break;
				}
			}
			if (army != nullptr) {
				break;
			}
		}
		r.check(army != nullptr, "fronts: a country with mechanic:command, an army of three divisions and three provinces exists");
		if (army == nullptr) {
			war_defines.front_spread_max_splits_per_day = splits_before;
			return;
		}

		std::size_t templated_before = 0;
		for (DivisionInstance const& division : army->get_divisions()) {
			templated_before += division.template_id == 1;
		}
		const std::size_t regiments_before = army->get_unit_count();
		const unique_id_t army_id = army->unique_id;

		WarManager& war_manager = im.get_war_manager();
		const std::uint32_t front_id = war_manager.create_front(im, *country, nullptr);
		r.check(front_id != 0, "fronts: an empty front can be made");
		// The line of the front by hand (the player draws lines along the border, the test wants three provinces of its own choice)
		for (ProvinceInstance* province : line) {
			const_cast<Front*>(war_manager.get_front_by_id(front_id))->provinces.push_back(province);
		}
		r.check(war_manager.set_front_army(im, *country, front_id, army_id, true), "fronts: an army can be assigned to the front");
		r.check(war_manager.get_front_by_id(front_id) != nullptr && war_manager.get_front_by_id(front_id)->provinces.size() == 3,
			"fronts: the line has the three provinces");

		im.force_tick_and_update();
		Front const* const front = war_manager.get_front_by_id(front_id);
		r.check(front != nullptr && front->army_ids.size() == 3, "fronts: the divisions spread over the line, one army for every province",
			fmt::format("{}", front != nullptr ? front->army_ids.size() : 0));
		std::size_t regiments_after = 0, templated_after = 0, unknown_division = 0;
		bool same_country = true;
		if (front != nullptr) {
			for (const unique_id_t id : front->army_ids) {
				ArmyInstance* const spread_army = static_cast<ArmyInstance*>(im.get_unit_instance_manager().get_unit_instance_group_by_unique_id(id));
				if (spread_army == nullptr) {
					continue;
				}
				same_country = same_country && &spread_army->get_country() == country;
				regiments_after += spread_army->get_unit_count();
				for (DivisionInstance const& division : spread_army->get_divisions()) {
					templated_after += division.template_id == 1;
				}
				for (RegimentInstance const& regiment : spread_army->get_regiment_instances()) {
					bool found = regiment.get_division_id() == 0;
					for (DivisionInstance const& division : spread_army->get_divisions()) {
						found = found || division.id == regiment.get_division_id();
					}
					unknown_division += !found;
				}
			}
		}
		r.check(same_country && regiments_after == regiments_before, "fronts: spreading loses no regiments",
			fmt::format("{} of {}", regiments_after, regiments_before));
		r.check(templated_after == templated_before, "fronts: a division keeps its template when it leaves its army",
			fmt::format("{} of {}", templated_after, templated_before));
		r.check(unknown_division == 0, "fronts: every regiment is in a division of its own army");

		// A day later nothing more is split (an army for every province already), the armies still exist
		im.force_tick_and_update();
		r.check(war_manager.get_front_by_id(front_id) != nullptr && war_manager.get_front_by_id(front_id)->army_ids.size() == 3,
			"fronts: no more armies are split when every province has one");
		r.check(war_manager.delete_front(*country, front_id), "fronts: a front can be deleted");

		// A country without mechanic:command has no fronts
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (candidate.exists() && !candidate.has_army_command() && candidate.has_armies()) {
				r.check(war_manager.create_front(im, candidate, nullptr) == 0, "fronts: a country without mechanic:command cannot make a front");
				break;
			}
		}

		war_defines.front_spread_max_splits_per_day = splits_before;
	}

	void test_trade_and_hunger(Results& r, InstanceManager& im) {
		MoneyLedger& ledger = im.get_money_ledger();
		ledger.enable(im);

		std::int64_t starved = 0;
		for (int day = 0; day < 130; ++day) {
			im.force_tick_and_update();
			if (im.get_today().is_month_start()) {
				for (ProvinceInstance& province : im.get_map_instance().get_province_instances()) {
					for (Pop& pop : province.get_mutable_pops()) {
						starved += type_safe::get(pop.get_num_starved());
					}
				}
			}
		}

		TradeNetwork const& network = im.get_trade_network();
		r.check(network.is_ready(), "trade: the network of the routes is built");
		const std::size_t countries = network.get_node_count() - 1;
		std::size_t routes = 0, land = 0, sea = 0, asymmetric = 0, bad_cost = 0;
		for (std::size_t from = 0; from < countries; ++from) {
			for (std::size_t to = 0; to < countries; ++to) {
				TradeNetwork::route_t const& route = network.get_route(country_index_t(from), country_index_t(to));
				TradeNetwork::route_t const& back = network.get_route(country_index_t(to), country_index_t(from));
				if (from == to) {
					bad_cost += route.mode != TradeNetwork::mode_t::NONE;
					continue;
				}
				if ((route.mode == TradeNetwork::mode_t::NONE) != (back.mode == TradeNetwork::mode_t::NONE)) {
					asymmetric++;
				}
				if (route.mode == TradeNetwork::mode_t::NONE) {
					continue;
				}
				routes++;
				land += route.mode == TradeNetwork::mode_t::LAND;
				sea += route.mode == TradeNetwork::mode_t::SEA;
				bad_cost += route.cost < fixed_point_t::_0 || route.cost > fixed_point_t::_1;
			}
		}
		r.check(routes > 0 && land > 0 && sea > 0, "trade: there are routes by land and by sea", fmt::format("{} routes, {} land, {} sea", routes, land, sea));
		r.check(asymmetric == 0, "trade: a route exists both ways", fmt::format("{} pairs", asymmetric));
		r.check(bad_cost == 0, "trade: the cost of a route is a share of the price, a country has no route to itself", fmt::format("{}", bad_cost));

		// The freight went to the treasuries (the ledger, which is closed in the other runs, counts it with a hook of its own)
		fixed_point_t fee_sum;
		for (CountryInstance const& country : im.get_country_instance_manager().get_country_instances()) {
			fee_sum += country.get_transport_income();
		}
		r.check(fee_sum > fixed_point_t::_0, "trade: the freight of the sellers reaches the treasuries", fmt::format("{}", fee_sum));

		// Subsistence: the pool never is more than the output
		std::int32_t pools = 0, pools_too_big = 0;
		for (ProvinceInstance const& province : im.get_map_instance().get_province_instances()) {
			const fixed_point_t pool = province.get_rgo().get_subsistence_pool();
			if (pool > fixed_point_t::_0) {
				pools++;
				pools_too_big += pool > province.get_rgo().get_output_quantity_yesterday();
			}
		}
		r.check(pools > 0, "trade: the workers keep part of the output of their RGO");
		r.check(pools_too_big == 0, "trade: the workers never keep more than the RGO made", fmt::format("{} of {}", pools_too_big, pools));

		fmt::println("TEST INFO trade: {} routes ({} land, {} sea), {} people died of hunger in the first months, {} RGOs kept part of their output", routes, land, sea, starved, pools);
		ledger.disable();
	}

	// The investment fund: the construction slider sets money aside, the AI keeps its budget out of a loss, the money of the
	// fund pays the construction of profitable enterprises, and no money appears or disappears without a hook of the ledger
	void test_investment_fund(Results& r, InstanceManager& im) {
		CountryInstance* player = nullptr;
		for (CountryInstance& country : im.get_country_instance_manager().get_country_instances()) {
			if (country.exists() && (player == nullptr || country.get_states().size() > player->get_states().size())) {
				player = &country;
			}
		}
		r.check(player != nullptr, "investment fund: a country with states exists");
		if (player == nullptr) {
			return;
		}

		MoneyLedger& ledger = im.get_money_ledger();
		ledger.enable(im);

		// A country played by a human keeps the whole allocation (the AI sets its own)
		player->set_ai(false);
		r.check(
			player->get_construction_spending_slider_value().get_value_untracked() == fixed_point_t::_0,
			"investment fund: the slider of the allocation starts at nothing"
		);
		im.queue_game_action<set_construction_spending_argument_t>(player->index, fixed_point_t::_1);
		im.force_tick_and_update();
		r.check(
			player->get_construction_spending_slider_value().get_value_untracked() == fixed_point_t::_1,
			"investment fund: the slider of the player can be raised to 100 percent"
		);

		constexpr int DAYS = 240;
		play(im, DAYS);

		const fixed_point_t income = player->get_income_yesterday();
		const fixed_point_t allocation = player->get_investment_allocation();
		r.check(
			player->get_investment_fund() + player->get_investment_spent_total() > fixed_point_t::_0,
			"investment fund: the slider sets money aside every day",
			fmt::format("fund {}, spent {}", player->get_investment_fund(), player->get_investment_spent_total())
		);
		r.check(
			income > fixed_point_t::_0 && allocation * 2 >= income - fixed_point_t(1) / 100 && allocation * 2 <= income + fixed_point_t(1) / 100,
			"investment fund: 100 percent of the slider is half of the income",
			fmt::format("income {}, allocation {}", income, allocation)
		);

		// The AI sets money aside only out of a surplus of its budget, and its treasury never goes below zero
		std::int32_t ai_investing = 0, ai_wrong = 0, ai_negative = 0, ai_total = 0, ai_surplus = 0, ai_deficit = 0;
		fixed_point_t total_fund, total_spent;
		for (CountryInstance& country : im.get_country_instance_manager().get_country_instances()) {
			if (!country.exists() || country.get_states().empty()) {
				continue;
			}
			total_fund += country.get_investment_fund();
			total_spent += country.get_investment_spent_total();
			if (country.get_cash_stockpile().load() < fixed_point_t::_0) {
				ai_negative++;
			}
			if (country.is_ai()) {
				ai_total++;
				if (country.get_balance_before_investment_average() > fixed_point_t::_0) {
					ai_surplus++;
				} else {
					ai_deficit++;
				}
			}
			if (country.is_ai() && country.get_construction_spending_slider_value().get_value_untracked() > fixed_point_t::_0) {
				ai_investing++;
				if (country.get_balance_before_investment_average() <= fixed_point_t::_0) {
					ai_wrong++;
				}
			}
		}
		r.check(ai_wrong == 0, "investment fund: an AI country sets money aside only while its budget has a surplus", fmt::format("{} of {}", ai_wrong, ai_investing));
		r.check(ai_negative == 0, "investment fund: no treasury goes below zero", fmt::format("{} countries", ai_negative));

		// Factories built with the money of the fund belong to the pops that were given it
		std::int32_t owned_by_pops = 0, deficit_projects = 0, projects = 0;
		for (CountryInstance const& country : im.get_country_instance_manager().get_country_instances()) {
			for (auto const& record : country.get_recent_investments()) {
				projects++;
				deficit_projects += record.deficit ? 1 : 0;
			}
		}
		for (StateSet const& state_set : im.get_map_instance().get_state_manager().get_state_sets()) {
			for (State const& state : state_set.get_states()) {
				for (FactoryInstance const& factory : state.get_factories()) {
					owned_by_pops += factory.get_owner_pop_type_index().has_value() ? 1 : 0;
				}
			}
		}
		fmt::println(
			"TEST INFO investment fund: after {} days the funds hold {}, {} was paid for projects ({} of the last projects, {} for a good the country lacks), {} factories belong to the pops that built them, {} of {} AI countries set money aside ({} have a surplus, {} a deficit)",
			DAYS, total_fund, total_spent, projects, deficit_projects, owned_by_pops, ai_investing, ai_total, ai_surplus, ai_deficit
		);
		r.check(total_spent > fixed_point_t::_0, "investment fund: the money of the funds builds profitable enterprises");

		r.check(
			ledger.get_measured_change_raw(im) == ledger.get_holder_sum_raw(),
			"investment fund: the money stays accounted for (nothing appears or disappears without a payment)",
			fmt::format("measured {}, explained {}", ledger.get_measured_change_raw(im), ledger.get_holder_sum_raw())
		);
		ledger.disable();
	}

	void test_game(Results& r, Game& game) {
		test_equipment_data(r, game.game_manager);
		constexpr int FIRST_DAYS = 40;
		constexpr int MORE_DAYS = 20;

		// Every day of every run the provinces also add up the political support of their pops and compare it with the sums that they kept
		const std::size_t kept_sums_differences_before = ProvinceInstance::get_political_support_cache_mismatch_count();
		ProvinceInstance::set_verify_political_support_cache(true);
		// The same for the consumption of the pops that the countries record: the flat sums against the map of the old way
		const std::size_t consumption_differences_before = CountryInstance::get_need_consumption_mismatch_count();
		CountryInstance::set_verify_need_consumption(true);

		// ---- run 1: the game that the others are compared with
		InstanceManager* im = game.start();
		r.check(im != nullptr, "game: the first instance starts");
		if (im == nullptr) {
			return;
		}
		test_population_start(r, *im);
		test_population_growth(r, *im);
		test_fish_output(r, *im);
		test_ship_lumber(r, *im);
		test_army_cloth_share(r, *im);
		const std::uint64_t defines_hash = im->definition_manager.get_define_manager().get_openvic_defines().get_hash();
		const std::uint64_t start_hash = StateHash::compute(*im);
		give_orders(*im);
		std::vector<std::uint64_t> days_1, days_2;
		play_recording(*im, FIRST_DAYS, days_1, g_details_1);
		const std::uint64_t hash_1 = StateHash::compute(*im);
		const std::string save_1 = Replay::serialise(im->get_action_history());
		play(*im, MORE_DAYS);
		const std::uint64_t hash_1_more = StateHash::compute(*im);
		r.check(hash_1 != start_hash && hash_1_more != hash_1, "game: the hash follows the game (it changes from day to day)");
		game.end();

		// ---- run 2: the same game again, deterministic
		im = game.start();
		r.check(im != nullptr && StateHash::compute(*im) == start_hash, "determinism: a new instance starts in the same state", im ? hex(StateHash::compute(*im)) + " != " + hex(start_hash) : "no instance");
		if (im == nullptr) {
			return;
		}
		give_orders(*im);
		play_recording(*im, FIRST_DAYS, days_2, g_details_2);
		const std::uint64_t hash_2 = StateHash::compute(*im);
		for (std::size_t day = 0; day < days_1.size() && day < days_2.size(); ++day) {
			if (days_1[day] != days_2[day]) {
				fmt::println("TEST INFO determinism: the two runs first differ on day {} after the orders", day + 1);
				DayDetails const& a = g_details_1[day];
				DayDetails const& b = g_details_2[day];
				fmt::println("TEST INFO determinism: pop cash {} vs {}, factory budgets {} vs {}, capitalist funds {} vs {}, prices {} vs {}",
					a.pop_cash, b.pop_cash, a.factory_budget, b.factory_budget, a.capitalist_fund, b.capitalist_fund, a.price_sum, b.price_sum);
				int shown = 0;
				for (std::size_t c = 0; c < a.country_cash.size() && shown < 6; ++c) {
					if (a.country_cash[c] != b.country_cash[c]) {
						fmt::println("TEST INFO determinism: country {} cash {} vs {}", c, a.country_cash[c], b.country_cash[c]);
						shown++;
					}
				}
				break;
			}
		}
		const std::string save_2 = Replay::serialise(im->get_action_history());
		r.check(hash_2 == hash_1, "determinism: two runs with the same actions end in the same state", hex(hash_1) + " != " + hex(hash_2));
		r.check(save_2 == save_1, "determinism: two runs write the same action history");
		game.end();

		// ---- run 2b: the same game with one worker thread, the result must not depend on the number of threads
		ThreadPool::set_worker_thread_count(1);
		im = game.start();
		ThreadPool::set_worker_thread_count(0);
		r.check(im != nullptr, "determinism: the instance with one thread starts");
		if (im == nullptr) {
			return;
		}
		give_orders(*im);
		std::vector<std::uint64_t> days_one_thread;
		std::vector<DayDetails> details_one_thread;
		play_recording(*im, FIRST_DAYS, days_one_thread, details_one_thread);
		const std::uint64_t hash_one_thread = StateHash::compute(*im);
		for (std::size_t day = 0; day < days_1.size() && day < days_one_thread.size(); ++day) {
			if (days_1[day] != days_one_thread[day]) {
				fmt::println("TEST INFO determinism: with one thread the game first differs on day {} after the orders", day + 1);
				break;
			}
		}
		r.check(
			hash_one_thread == hash_1, "determinism: the result does not depend on the number of threads (one thread, all threads)",
			hex(hash_1) + " != " + hex(hash_one_thread)
		);
		game.end();

		// ---- run 3: a save game: the state after loading equals the state before saving
		im = game.start();
		r.check(im != nullptr, "save game: the instance for loading starts");
		if (im == nullptr) {
			return;
		}
		std::vector<ReplayEntry> entries;
		r.check(Replay::deserialise(save_1, entries), "save game: the saved history is read");
		r.check(im->replay_actions(entries), "save game: the saved history is replayed");
		const std::uint64_t loaded_hash = StateHash::compute(*im);
		r.check(loaded_hash == hash_1, "save game: the state after loading equals the state before saving", hex(hash_1) + " != " + hex(loaded_hash));
		play(*im, MORE_DAYS);
		const std::uint64_t loaded_more = StateHash::compute(*im);
		r.check(loaded_more == hash_1_more, "save game: the game goes on after loading as it would have without saving", hex(hash_1_more) + " != " + hex(loaded_more));
		r.check(
			Replay::serialise(im->get_action_history()).size() > save_1.size(), "save game: the history of a loaded game goes on (it can be saved again)"
		);
		game.end();

		// ---- run 4: the money: it is created only by the gold
		im = game.start();
		r.check(im != nullptr, "money: the instance starts");
		if (im == nullptr) {
			return;
		}
		MoneyLedger& ledger = im->get_money_ledger();
		ledger.enable(*im);
		give_orders(*im);
		play(*im, FIRST_DAYS);
		const std::int64_t measured = ledger.get_measured_change_raw(*im);
		const std::int64_t holders = ledger.get_holder_sum_raw();
		const double days = static_cast<double>(ledger.get_days());
		const auto flow = [&](MoneyLedger::label_t label) -> std::int64_t { return ledger.get_flow_total_raw(label); };
		const auto pounds = [](const std::int64_t raw) -> double { return static_cast<double>(raw) / static_cast<double>(1 << fixed_point_t::PRECISION); };

		r.check(measured == holders, "money: every change of the total money is explained by the payments (nothing unaccounted)",
			fmt::format("measured {:.3f}, explained {:.3f} over {} days", pounds(measured), pounds(holders), days));
		r.check(flow(MoneyLedger::label_t::factory_budget_floor) == 0, "money: no factory spends money it does not have",
			fmt::format("{:.3f}", pounds(flow(MoneyLedger::label_t::factory_budget_floor))));

		// What is created: the gold (the rules of Victoria 2), the starting budget of new factories, the rounding up of the sellers.
		// The total must not grow by anything else. (It may grow by less: money is in flight in the factories for a day, and
		// the money of slaves leaves the economy; the money of the constructions goes to the treasuries and the builders since version 73.)
		const std::int64_t created = flow(MoneyLedger::label_t::gold_worker_pay) + flow(MoneyLedger::label_t::gold_income)
			+ flow(MoneyLedger::label_t::factory_starting_budget) + flow(MoneyLedger::label_t::market_sellers_get)
			+ flow(MoneyLedger::label_t::market_buyers_pay) + flow(MoneyLedger::label_t::transport_fee_income)
			+ flow(MoneyLedger::label_t::factory_budget_floor);
		r.check(
			measured <= created + (1 << fixed_point_t::PRECISION),
			"money: the total grows only by the gold, the starting budgets of new factories and the rounding of the market",
			fmt::format("the total changed by {:.3f}, the allowed sources gave {:.3f}", pounds(measured), pounds(created))
		);
		fmt::println("TEST INFO money: the total changed by {:.1f}, created by the allowed sources {:.1f} (gold {:.1f}, starting budgets {:.1f})",
			pounds(measured), pounds(created), pounds(flow(MoneyLedger::label_t::gold_worker_pay) + flow(MoneyLedger::label_t::gold_income)),
			pounds(flow(MoneyLedger::label_t::factory_starting_budget)));
		const double rounding = pounds(flow(MoneyLedger::label_t::market_sellers_get) + flow(MoneyLedger::label_t::market_buyers_pay) + flow(MoneyLedger::label_t::transport_fee_income));
		const double volume = -pounds(flow(MoneyLedger::label_t::market_buyers_pay));
		// (the shares of the orders are rounded, either way, by the smallest amount per order)
		r.check(rounding >= -volume * 0.0001 - 1.0 && rounding <= volume * 0.001 + 1.0, "money: the rounding of the market is below 0.1% of the trade",
			fmt::format("rounding {:.3f} of {:.3f}", rounding, volume));
		ledger.disable();
		game.end();


		// ---- run 5: the credits in a game: offers, taking, withdrawing, bankruptcy, a region that changes its owner
		im = game.start();
		r.check(im != nullptr, "credit: the instance starts");
		if (im == nullptr) {
			return;
		}
		test_credit_game(r, *im);
		game.end();

		// ---- run 6: the factory actions (priority, deleting)
		im = game.start();
		r.check(im != nullptr, "factories: the instance starts");
		if (im == nullptr) {
			return;
		}
		test_factory_actions(r, *im);
		game.end();

		// ---- run 7: the investment fund
		im = game.start();
		r.check(im != nullptr, "investment fund: the instance starts");
		if (im == nullptr) {
			return;
		}
		test_investment_fund(r, *im);
		game.end();

		// ---- run 8: the world trade and the hunger of the unemployed
		im = game.start();
		r.check(im != nullptr, "trade: the instance starts");
		if (im == nullptr) {
			return;
		}
		test_supply_limit(r, *im);
		test_divisions(r, *im);
		test_unlock_mechanics(r, *im);
		test_regiment_upgrade(r, *im);
		test_army_groups(r, *im);
		test_group_limit(r, *im);
		test_command_colours(r, *im);
		test_command_plan(r, *im);
		test_plan_lines(r, *im);
		test_theatres(r, *im);
		test_attached_generals_technologies(r, *im);
		test_fleet_to_port(r, *im);
		test_supply_delivery(r, *im);
		test_supply_through_enemy(r, *im);
		test_fronts(r, *im);
		test_unlocks(r, *im);
		test_division_stats(r, *im);
		test_production_types(r, *im);
		test_regiment_equipment(r, *im);
		test_unit_types(r, *im);
		test_army_technologies(r, *im);
		test_army_research(r, *im);
		test_battle_rules(r, *im);
		test_occupation(r, *im);
		test_unlock_gates(r, *im);
		test_scoped_modifiers(r, *im);
		test_trade_and_hunger(r, *im);
		test_population_monthly(r, *im); // ticks days too
		test_ai_budget(r, *im);
		test_artisan_weapons(r, *im);
		test_job_migration(r, *im);
		test_rgo_buildings(r, *im); // ticks days too
		test_companies(r, *im); // ticks days, so it is the last of the tests that use the game
		game.end();

		// ---- run 9: the live divisions and the land battles (docs/WAR_ASSUMPTIONS.md, section 6). The diplomacy of the AI is off in runs 9
		// and 10 (version 91): its alliances would call other countries into the wars the tests make (test_diplomacy_ai turns it on for itself,
		// at the end of the run)
		OpenVicDiplomacyDefines& diplomacy_defines =
			const_cast<OpenVicDiplomacyDefines&>(game.game_manager.get_definition_manager().get_define_manager().get_openvic_defines().diplomacy);
		const std::int32_t diplomacy_ai_before = diplomacy_defines.ai_diplomacy_enabled;
		diplomacy_defines.ai_diplomacy_enabled = 0;
		im = game.start();
		r.check(im != nullptr, "live battles: the instance starts");
		if (im == nullptr) {
			return;
		}
		test_live_divisions(r, *im);
		test_live_battle(r, *im);
		test_leader_skills(r, *im);
		test_straits_and_manual_orders(r, *im);
		test_division_recruitment(r, *im);
		test_manpower_losses(r, *im);
		test_plan_status(r, *im);
		test_laws(r, *im);
		test_administration(r, *im);
		test_literacy(r, *im);
		test_relations(r, *im);
		test_sea_transfer(r, *im);
		test_ai_war_captures(r, *im); // ticks 60 days
		test_front_dynamics(r, *im); // ticks days, a war of its own
		test_order_moves(r, *im); // ticks days
		test_offensive_modes(r, *im);
		test_offensive_execution(r, *im); // ticks days, a war of its own
		test_concentration(r, *im); // ticks a day, a war of its own
		test_planning(r, *im);
		test_breakthrough(r, *im); // ticks a day
		test_encirclement(r, *im); // ticks days, a war of its own
		test_fallback(r, *im); // ticks days, a war of its own
		test_naval_invasion(r, *im); // ticks days, a war of its own
		test_ai_marshals(r, *im); // ticks days, a war of its own
		test_mobilisation_regions(r, *im);
		test_allies_and_separate_peace(r, *im);
		test_army_orders(r, *im);
		test_canned_food_grain(r, *im); // it changes the stockpile and the armies of a country
		test_diplomacy_ai(r, *im); // last: it makes alliances and goals
		test_spheres(r, *im); // after it: it founds a sphere and changes the members
		test_sphere_kinds(r, *im); // a war of its own
		test_sphere_principles(r, *im);
		game.end();

		// ---- run 10: the mobilisation, the monthly limit of the recruitment and the priorities (docs/RECRUITMENT.md)
		im = game.start();
		r.check(im != nullptr, "mobilisation: the instance starts");
		if (im == nullptr) {
			return;
		}
		test_recruitment_limit(r, *im); // ticks up to a month
		test_mobilisation(r, *im); // ticks the days of the mobilisation and DEMOBILISE_DAYS
		test_ai_mobilisation(r, *im); // a war of its own
		game.end();
		diplomacy_defines.ai_diplomacy_enabled = diplomacy_ai_before;

		// ---- the defines hash does not change while playing
		r.check(defines_hash != 0, "defines: the instance sees the hash of the defines");

		// ---- the sums that the provinces keep from one day to the next
		ProvinceInstance::set_verify_political_support_cache(false);
		r.check(
			ProvinceInstance::get_political_support_cache_mismatch_count() == kept_sums_differences_before,
			"pops: the political support that a province keeps is always the sum of the political support of its pops",
			fmt::format("{} differences", ProvinceInstance::get_political_support_cache_mismatch_count() - kept_sums_differences_before)
		);
		CountryInstance::set_verify_need_consumption(false);
		r.check(
			CountryInstance::get_need_consumption_mismatch_count() == consumption_differences_before,
			"pops: the consumption that a country records by type of pop is the sum of the reports",
			fmt::format("{} differences", CountryInstance::get_need_consumption_mismatch_count() - consumption_differences_before)
		);
	}
}

bool OpenVic::run_pure_selftests() {
	Results r;
	test_rate(r);
	test_defines(r);
	test_replay(r);
	test_save_header(r);
	test_credit_pure(r);
	test_equipment_pure(r);
	r.summary();
	return r.failed == 0;
}

bool OpenVic::run_determinism_loop(GameManager& game_manager, Bookmark const& bookmark, const int runs) {
	constexpr int DAYS = 40;
	Game game { game_manager, bookmark };
	std::vector<std::uint64_t> first_hashes;
	std::vector<DayDetails> first_details;
	int differing = 0;
	for (int run = 0; run < runs; ++run) {
		InstanceManager* const im = game.start();
		if (im == nullptr) {
			fmt::println("DETERMINISM run {}: the instance does not start", run);
			return false;
		}
		const std::uint64_t start_hash = StateHash::compute(*im);
		give_orders(*im);
		std::vector<std::uint64_t> hashes;
		std::vector<DayDetails> details;
		play_recording(*im, DAYS, hashes, details);
		game.end();
		if (run == 0) {
			first_hashes = std::move(hashes);
			first_details = std::move(details);
			fmt::println("DETERMINISM run 0: start {} end {}", hex(start_hash), hex(first_hashes.back()));
			continue;
		}
		std::size_t day = 0;
		while (day < hashes.size() && day < first_hashes.size() && hashes[day] == first_hashes[day]) {
			day++;
		}
		if (day == hashes.size()) {
			fmt::println("DETERMINISM run {}: same", run);
			std::fflush(stdout);
			continue;
		}
		differing++;
		DayDetails const& a = first_details[day];
		DayDetails const& b = details[day];
		fmt::println("DETERMINISM run {}: differs from day {} (start {}): pop cash {} vs {}, factory budgets {} vs {}, capitalist funds {} vs {}, prices {} vs {}",
			run, day + 1, hex(start_hash), a.pop_cash, b.pop_cash, a.factory_budget, b.factory_budget, a.capitalist_fund, b.capitalist_fund, a.price_sum, b.price_sum);
		for (std::size_t c = 0; c < a.country_cash.size() && c < b.country_cash.size(); ++c) {
			if (a.country_cash[c] != b.country_cash[c]) {
				fmt::println("DETERMINISM run {}: country {} cash {} vs {}", run, c, a.country_cash[c], b.country_cash[c]);
			}
		}
		std::fflush(stdout);
	}
	fmt::println("DETERMINISM SUMMARY runs={} differing={}", runs, differing);
	return differing == 0;
}

bool OpenVic::run_data_selftests(GameManager& game_manager, Bookmark const& bookmark) {
	Results r;
	Game game { game_manager, bookmark };
	test_game(r, game);
	r.summary();
	return r.failed == 0;
}

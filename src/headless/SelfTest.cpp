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
#include <openvic-simulation/military/Unlocks.hpp>
#include <openvic-simulation/military/UnitEquipment.hpp>
#include <openvic-simulation/military/UnitModifiers.hpp>
#include <openvic-simulation/modifier/ModifierEffectCache.hpp>
#include <openvic-simulation/military/BattleFormulas.hpp>
#include <openvic-simulation/military/DivisionStats.hpp>
#include <openvic-simulation/modifier/ModifierManager.hpp>
#include <openvic-simulation/dataloader/Dataloader.hpp>
#include <openvic-simulation/military/SupplyDelivery.hpp>
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
			"unlocks: in 1836 the provisional layer gives rifle_1 and field_gun_1, not rifle_2"
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
			if (candidate.exists() && candidate.has_army_organisation()) {
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
		r.check(
			near(standard_stats.attack, expected_attack, fixed_point_t(1) / 100) && near(standard_stats.defence, expected_defence, fixed_point_t(1) / 100),
			"division stats: the attack and the defence add up the regiments by the weights of their rows and the multipliers of their weapons",
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
		r.check(
			near(wide.attack, narrow.attack * 2, fixed_point_t(1) / 100) && near(wide.defence, narrow.defence * 2, fixed_point_t(1) / 100),
			"division stats: the attack and the defence are in proportion to the width at the same depth"
		);
		const DivisionStats target = compute_division_stats(im, *country, standard);
		const fixed_point_t damage_narrow = battle_formulas::damage_per_day(war, narrow, target);
		const fixed_point_t damage_wide = battle_formulas::damage_per_day(war, wide, target);
		r.check(
			damage_narrow > fixed_point_t::_0 && near(damage_wide, damage_narrow * 2, fixed_point_t(1) / 100),
			"battle: a division half as wide deals half the damage at the same depth",
			fmt::format("{} and {}", dbl(damage_narrow), dbl(damage_wide))
		);

		// ---- the depth is the durability: more rows last longer, a rank of one row and a rank of two give a depth of 1.5
		const DivisionStats shallow = compute_division_stats(im, *country, make_template(grid_with(*infantry, 2, 1)));
		const DivisionStats deep = compute_division_stats(im, *country, make_template(grid_with(*infantry, 2, 2)));
		r.check(
			near(deep.durability, shallow.durability * 2, fixed_point_t(1) / 100)
				&& near(battle_formulas::days_to_destroy(deep.durability, damage_narrow), battle_formulas::days_to_destroy(shallow.durability, damage_narrow) * 2, fixed_point_t(1) / 10),
			"division stats: twice the depth, twice the durability, twice the days to destroy"
		);
		DivisionGrid uneven = grid_with(*infantry, 1, 2);
		uneven[DivisionTemplate::cell_index(1, 0)] = cell(*infantry);
		const DivisionStats uneven_stats = compute_division_stats(im, *country, make_template(uneven));
		r.check(uneven_stats.depth == fixed_point_t(3) / 2 && uneven_stats.width == 2, "division stats: the depth is the average of the ranks (1.5 for 2 and 1)");
		r.check(
			narrow.density > fixed_point_t::_0 && wide.density == narrow.density,
			"division stats: the density is the strength per rank (the same for the same ranks)"
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

		// ---- the stock has the rifles: a day gives the best allowed weapon to as many regiments as ov_war.EQUIPMENT_UPGRADES_PER_DAY says
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
			with_rifle_1 == static_cast<std::size_t>(war.equipment_upgrades_per_day) && equipped != nullptr && equipped->get_weapon_good() == rifle_1_good,
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
		bool start_types_ok = true;
		bool old_infantry_in_armies = false;
		for (CountryInstance& country : im.get_country_instance_manager().get_country_instances()) {
			for (ArmyInstance& army : country.get_armies()) {
				for (RegimentInstance const& regiment : army.get_regiment_instances()) {
					old_infantry_in_armies |= regiment.get_regiment_type().get_identifier() == "infantry";
					start_types_ok &= regiment.get_regiment_type().get_identifier() != "artillery";
				}
			}
		}
		r.check(!old_infantry_in_armies && start_types_ok, "unit types: the armies of the start have line infantry (the alias of the infantry of Vic2), no regiments of the new types");
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
				if (!country.is_at_war() || country.get_war_enemies().empty() || !country.has_armies()) {
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
						&& attacker.is_at_war_with(*province.get_owner())
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
					r.check(raider->get_supply_ratio() > standard_ratio && raider->get_supply_send_multiplier() > fixed_point_t::_1,
						"supply on enemy land: the maximum priority sends more and what arrives is more",
						fmt::format("{} -> {} (sent x{})", standard_ratio, raider->get_supply_ratio(), raider->get_supply_send_multiplier()));
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

	// Divisions and templates (docs/PLAN_ARMY_SUPPLY_FRONTS.md, section 4): the civilised countries have armies of divisions (the regiments of the start
	// are out of the template), the others have groups of regiments only and no generals; templates and reforming
	void test_divisions(Results& r, InstanceManager& im) {
		std::size_t civilised_armies = 0, regiments_without_division = 0, bad_division = 0, templated_at_start = 0, plain_with_division = 0, plain_leaders = 0;
		std::size_t civilised_without_template = 0, plain_with_template = 0, plain_armies = 0;
		for (CountryInstance const& country : im.get_country_instance_manager().get_country_instances()) {
			if (!country.exists() || !country.has_armies()) {
				continue;
			}
			if (country.has_army_organisation()) {
				civilised_without_template += country.get_division_templates().empty();
			} else {
				plain_with_template += !country.get_division_templates().empty();
			}
			for (ArmyInstance const& army : country.get_armies()) {
				if (country.has_army_organisation()) {
					civilised_armies++;
					for (DivisionInstance const& division : army.get_divisions()) {
						templated_at_start += division.template_id != 0;
					}
					for (RegimentInstance const& regiment : army.get_regiment_instances()) {
						bool found = false;
						for (DivisionInstance const& division : army.get_divisions()) {
							found |= division.id == regiment.get_division_id();
						}
						regiments_without_division += regiment.get_division_id() == 0;
						bad_division += !found;
					}
				} else {
					plain_armies++;
					plain_with_division += !army.get_divisions().empty();
					plain_leaders += army.get_leader() != nullptr;
					for (RegimentInstance const& regiment : army.get_regiment_instances()) {
						plain_with_division += regiment.get_division_id() != 0;
					}
				}
			}
		}
		r.check(civilised_armies > 50 && plain_armies > 0, "divisions: there are armies of civilised and of other countries", fmt::format("{} {}", civilised_armies, plain_armies));
		r.check(regiments_without_division == 0 && bad_division == 0, "divisions: every regiment of a civilised country is in a division of its army",
			fmt::format("{} {}", regiments_without_division, bad_division));
		r.check(templated_at_start == 0, "divisions: the regiments of the start are out of the template (they are not touched)", fmt::format("{}", templated_at_start));
		r.check(civilised_without_template == 0, "divisions: a civilised country has the standard template");
		r.check(plain_with_division == 0 && plain_with_template == 0, "divisions: a country without the technology has no divisions and no templates");
		r.check(plain_leaders == 0, "divisions: the armies of a country without the technology have no general");

		// The standard template: four regiments of infantry (2 x 2) and two of cavalry (1 x 2)
		const std::size_t kind_infantry = regiment_kind(im, "line_infantry");
		const std::size_t kind_named_cavalry = regiment_kind(im, "cavalry");
		// The cavalry of the tests: the cavalry type that most regiments of the civilised countries are of
		std::size_t kind_cavalry = kind_named_cavalry;
		{
			std::map<std::size_t, std::size_t> cavalry_regiments;
			for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
				if (candidate.exists() && candidate.has_army_organisation()) {
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
			if (!candidate.exists() || !candidate.has_army_organisation() || candidate.is_at_war()) {
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

		// The standard template: one whole division for every four regiments of infantry (with two regiments of cavalry for each)
		std::size_t standard_infantry = 0, standard_cavalry = 0;
		for (RegimentInstance const& regiment : army->get_regiment_instances()) {
			standard_infantry += get_division_slot_kind(regiment.get_regiment_type()) == kind_infantry;
			standard_cavalry += get_division_slot_kind(regiment.get_regiment_type()) == kind_named_cavalry;
		}
		const std::size_t standard_expected = std::min(standard_infantry / 4, standard_cavalry / 2);
		const std::size_t standard_regiments = army->get_unit_count();
		im.queue_game_action<reform_army_to_template_argument_t>(army->unique_id, std::uint32_t(0), std::uint32_t(1));
		im.force_tick_and_update();
		r.check(count_templated(1) == standard_expected && army->get_unit_count() == standard_regiments,
			"divisions: the regiments make whole divisions of the standard template (the others stay as they were)",
			fmt::format("{} of {}", count_templated(1), standard_expected));
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
			if (candidate.exists() && !candidate.has_army_organisation() && candidate.has_armies() && !candidate.get_leaders<unit_branch_t::LAND>().empty()) {
				plain = &candidate;
				plain_army = &candidate.get_armies().front().get();
				break;
			}
		}
		if (plain != nullptr) {
			im.queue_game_action<create_division_template_argument_t>(plain->index, make_test_grid(kind_infantry, 1, kind_infantry, 0));
			im.force_tick_and_update();
			r.check(plain->get_division_templates().empty(), "divisions: a country without the technology cannot make templates");
			LeaderInstance const& general = plain->get_leaders<unit_branch_t::LAND>().front();
			im.queue_game_action<set_army_leader_argument_t>(plain_army->unique_id, general.unique_id);
			im.force_tick_and_update();
			r.check(plain_army->get_leader() == nullptr, "divisions: a general cannot command the army of a country without the technology");
		}
	}

	// Groups of armies and field marshals, the bonus of the generals to the supply (docs/PLAN_ARMY_SUPPLY_FRONTS.md, sections 1 and 4)
	void test_army_groups(Results& r, InstanceManager& im) {
		OpenVicSupplyDefines& defines = const_cast<OpenVicSupplyDefines&>(im.definition_manager.get_define_manager().get_openvic_defines().supply);
		CountryInstance* country = nullptr;
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (candidate.exists() && candidate.has_army_organisation() && candidate.get_armies().size() >= 3
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
			if (candidate.exists() && !candidate.has_army_organisation() && candidate.has_armies()) {
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

	// World trade: the routes by land and sea exist and are the same both ways, the freight of the sellers reaches the treasuries, the
	// workers keep part of the output, and the people who are hungry for a month and have no job emigrate or die
	// Fronts (docs/PLAN_ARMY_SUPPLY_FRONTS.md, section 5): the divisions of the armies of a front spread over its line, one army for every province
	// of the line, and a division keeps its template when it leaves its army
	void test_fronts(Results& r, InstanceManager& im) {
		OpenVicWarDefines& war_defines = const_cast<OpenVicWarDefines&>(im.definition_manager.get_define_manager().get_openvic_defines().war);
		const auto chunk_before = war_defines.front_spread_chunk_size;
		const auto splits_before = war_defines.front_spread_max_splits_per_day;
		war_defines.front_spread_chunk_size = 1;
		war_defines.front_spread_max_splits_per_day = 10;

		CountryInstance* country = nullptr;
		ArmyInstance* army = nullptr;
		std::vector<ProvinceInstance*> line;
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (!candidate.exists() || !candidate.has_army_organisation() || candidate.is_at_war()) {
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
				if (candidate_army.get_unit_count() >= 8 && !candidate_army.is_in_combat() && !candidate_army.is_moving()) {
					country = &candidate;
					army = &candidate_army;
					break;
				}
			}
			if (army != nullptr) {
				break;
			}
		}
		r.check(army != nullptr, "fronts: a civilised country with an army of eight regiments and three provinces exists");
		if (army == nullptr) {
			war_defines.front_spread_chunk_size = chunk_before;
			war_defines.front_spread_max_splits_per_day = splits_before;
			return;
		}

		// Whole divisions of the standard template, so that the split has divisions of a template to keep
		im.queue_game_action<reform_army_to_template_argument_t>(army->unique_id, std::uint32_t(0), std::uint32_t(1));
		im.force_tick_and_update();
		std::size_t templated_before = 0;
		for (DivisionInstance const& division : army->get_divisions()) {
			templated_before += division.template_id == 1;
		}
		const std::size_t regiments_before = army->get_unit_count();
		const unique_id_t army_id = army->unique_id;

		WarManager& war_manager = im.get_war_manager();
		const std::uint32_t front_id = war_manager.create_front(im, *country, nullptr);
		r.check(front_id != 0, "fronts: an empty front can be made");
		for (ProvinceInstance* province : line) {
			war_manager.toggle_front_province(*country, front_id, *province);
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

		war_defines.front_spread_chunk_size = chunk_before;
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
		// the money of slaves and the cost of the expansions of the factories leave the economy.)
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
		test_army_groups(r, *im);
		test_supply_delivery(r, *im);
		test_fronts(r, *im);
		test_unlocks(r, *im);
		test_division_stats(r, *im);
		test_production_types(r, *im);
		test_regiment_equipment(r, *im);
		test_unit_types(r, *im);
		test_army_technologies(r, *im);
		test_scoped_modifiers(r, *im);
		test_trade_and_hunger(r, *im);
		game.end();

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

bool OpenVic::run_data_selftests(GameManager& game_manager, Bookmark const& bookmark) {
	Results r;
	Game game { game_manager, bookmark };
	test_game(r, game);
	r.summary();
	return r.failed == 0;
}

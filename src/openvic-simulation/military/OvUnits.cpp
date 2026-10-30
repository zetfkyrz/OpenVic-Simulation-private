#include "UnitType.hpp"

#include <fstream>
#include <string>

#include "openvic-simulation/dataloader/NodeTools.hpp"
#include "openvic-simulation/economy/GoodDefinition.hpp"
#include "openvic-simulation/military/Equipment.hpp"
#include "openvic-simulation/military/Unlocks.hpp"
#include "openvic-simulation/utility/Logger.hpp"

using namespace OpenVic;
using namespace OpenVic::NodeTools;

// The types of units of OpenVic: ov_data/common/ov_units/<id>.txt, one type a file (docs/WAR_ASSUMPTIONS.md, section 5).

namespace {
	std::size_t line_of_key(std::string_view source, std::string_view key) {
		std::ifstream file { std::string { source } };
		std::string text;
		std::size_t line_number = 0;
		while (std::getline(file, text)) {
			line_number++;
			if (!key.empty() && text.find(key) != std::string::npos) {
				return line_number;
			}
		}
		return 0;
	}

	// One buff: attack_mult, defense_mult, breakthrough_mult (shares: 0.1 is 10 %)
	node_callback_t expect_buff(RegimentType::OvBuff& buff) {
		return expect_dictionary_keys(
			"attack_mult", ZERO_OR_ONE, expect_fixed_point(assign_variable_callback(buff.attack_mult)),
			"defense_mult", ZERO_OR_ONE, expect_fixed_point(assign_variable_callback(buff.defense_mult)),
			"breakthrough_mult", ZERO_OR_ONE, expect_fixed_point(assign_variable_callback(buff.breakthrough_mult))
		);
	}

	node_callback_t expect_buff_map(memory::vector<std::pair<memory::string, RegimentType::OvBuff>>& buffs) {
		return expect_dictionary(
			[&buffs](std::string_view key, ast::NodeCPtr value) -> bool {
				RegimentType::OvBuff buff;
				const bool ok = expect_buff(buff)(value);
				buffs.emplace_back(memory::string { key }, buff);
				return ok;
			}
		);
	}
}

bool UnitTypeManager::load_vic2_aliases(std::string_view source, ast::NodeCPtr root) {
	vic2_aliases.clear();
	const bool parsed = expect_dictionary(
		[this](std::string_view vic2_id, ast::NodeCPtr value) -> bool {
			return expect_identifier_or_string(
				[this, vic2_id](std::string_view target) -> bool {
					vic2_aliases.emplace(memory::string { vic2_id }, memory::string { target });
					return true;
				}
			)(value);
		}
	)(root);
	if (!parsed) {
		spdlog::warn_s("The aliases of the units of Vic2 {} are broken, what could be read is loaded", source);
	}
	return parsed;
}

RegimentType const* UnitTypeManager::resolve_vic2_regiment_type(std::string_view identifier) const {
	if (const auto it = vic2_aliases.find(identifier); it != vic2_aliases.end()) {
		if (RegimentType const* const target = regiment_types.get_item_by_identifier(it->second); target != nullptr) {
			return target;
		}
	}
	return regiment_types.get_item_by_identifier(identifier);
}

std::size_t UnitTypeManager::load_ov_unit_file(
	GoodDefinitionManager const& good_definition_manager, EquipmentManager const& equipment_manager, std::string_view source, ast::NodeCPtr root
) {
	std::size_t loaded = 0;

	const bool parsed = expect_dictionary(
		[&](std::string_view key, ast::NodeCPtr value) -> bool {
			const auto skip = [source, key](std::string_view reason) -> bool {
				const std::size_t line = line_of_key(source, key);
				spdlog::warn_s("Unit type \"{}\" skipped: {} ({}{})", key, reason, source, line > 0 ? ":" + std::to_string(line) : std::string {});
				return true; // the other types of the file are still loaded
			};

			UnitType::unit_type_args_t unit_args {};
			RegimentType::regiment_type_args_t regiment_args {};
			RegimentType::OvUnitData ov;
			ov.is_ov = true;
			memory::string category;
			fixed_point_t occupation_weight = fixed_point_t(-1);
			fixed_point_t attack, defense, breakthrough, organisation, speed, reconnaissance, weight, manpower, supply_consumption = fixed_point_t::_1;
			bool has_manpower = false;
			int build_days = 90;
			std::uint32_t icon = 0, priority = 0;
			std::string_view sprite;
			memory::vector<std::pair<memory::string, fixed_point_t>> unused;

			const bool ok = expect_dictionary_keys_and_default(
				[source, key](std::string_view unknown_key, ast::NodeCPtr) -> bool {
					spdlog::warn_s("Unknown key \"{}\" in the unit type \"{}\" ({})", unknown_key, key, source);
					return true;
				},
				"category", ONE_EXACTLY, expect_identifier_or_string(assign_variable_callback_string(category)),
				"unlock_key", ZERO_OR_ONE, expect_identifier_or_string(assign_variable_callback_string(ov.unlock_key)),
				"equipment", ONE_EXACTLY, expect_list(expect_identifier_or_string(
					[&ov](std::string_view weapon) -> bool {
						ov.equipment.emplace_back(weapon);
						return true;
					}
				)),
				"icon", ONE_EXACTLY, expect_uint(assign_variable_callback(icon)),
				"sprite", ONE_EXACTLY, expect_identifier_or_string(assign_variable_callback(sprite)),
				"sprite_override", ZERO_OR_ONE, expect_identifier(assign_variable_callback(regiment_args.sprite_override)),
				"sprite_mount", ZERO_OR_ONE, expect_identifier(assign_variable_callback(regiment_args.sprite_mount)),
				"sprite_mount_attach_node", ZERO_OR_ONE, expect_identifier(assign_variable_callback(regiment_args.sprite_mount_attach_node)),
				"priority", ZERO_OR_ONE, expect_uint(assign_variable_callback(priority)),
				"build_time", ZERO_OR_ONE, expect_int(assign_variable_callback(build_days)),
				"occupation_weight", ZERO_OR_ONE, expect_fixed_point(assign_variable_callback(occupation_weight)),
				"scope_weights", ZERO_OR_ONE, expect_dictionary(
					[&ov](std::string_view scope, ast::NodeCPtr scope_value) -> bool {
						fixed_point_t scope_weight = fixed_point_t::_1;
						const bool scope_ok = expect_fixed_point(assign_variable_callback(scope_weight))(scope_value);
						ov.scope_weights.emplace_back(memory::string { scope }, scope_weight);
						return scope_ok;
					}
				),
				"upgrades_to", ZERO_OR_ONE, expect_identifier_or_string(assign_variable_callback_string(ov.upgrades_to)),
				"upgrade_cost", ZERO_OR_ONE,
					good_definition_manager.expect_good_definition_decimal_map(move_variable_callback(ov.upgrade_cost)),
				"buffs", ZERO_OR_ONE, expect_dictionary_keys(
					"general", ZERO_OR_ONE, expect_buff(ov.general),
					"vs_unit", ZERO_OR_ONE, expect_buff_map(ov.vs_unit),
					"terrain", ZERO_OR_ONE, expect_buff_map(ov.terrain)
				),
				"base_stats", ONE_EXACTLY, expect_dictionary_keys(
					"attack", ONE_EXACTLY, expect_fixed_point(assign_variable_callback(attack)),
					"defense", ONE_EXACTLY, expect_fixed_point(assign_variable_callback(defense)),
					"breakthrough", ONE_EXACTLY, expect_fixed_point(assign_variable_callback(breakthrough)),
					"organisation", ONE_EXACTLY, expect_fixed_point(assign_variable_callback(organisation)),
					"speed", ONE_EXACTLY, expect_fixed_point(assign_variable_callback(speed)),
					"reconnaissance", ZERO_OR_ONE, expect_fixed_point(assign_variable_callback(reconnaissance)),
					"weight", ZERO_OR_ONE, expect_fixed_point(assign_variable_callback(weight)),
					"manpower", ONE_EXACTLY, expect_fixed_point([&manpower, &has_manpower](fixed_point_t value_manpower) -> bool {
						manpower = value_manpower;
						has_manpower = true;
						return true;
					}),
					"cost", ONE_EXACTLY, good_definition_manager.expect_good_definition_decimal_map(move_variable_callback(unit_args.build_cost)),
					"supply_cost", ONE_EXACTLY, good_definition_manager.expect_good_definition_decimal_map(move_variable_callback(unit_args.supply_cost)),
					"supply_consumption", ZERO_OR_ONE, expect_fixed_point(assign_variable_callback(supply_consumption))
				)
			)(value);

			if (!ok) {
				return skip("the file could not be read as a unit type, see the messages above");
			}
			if (regiment_types.has_identifier(key) || ship_types.has_identifier(key)) {
				return skip("a unit type with this id exists");
			}
			if (category == "infantry") {
				unit_args.unit_category = UnitType::unit_category_t::INFANTRY;
			} else if (category == "cavalry") {
				unit_args.unit_category = UnitType::unit_category_t::CAVALRY;
			} else if (category == "artillery") {
				unit_args.unit_category = UnitType::unit_category_t::SUPPORT;
			} else {
				return skip(memory::fmt::format("the category \"{}\" is none of infantry, cavalry, artillery", category));
			}
			ov.category_scope = category;
			if (ov.equipment.empty()) {
				return skip("the list of weapons (equipment) is empty");
			}
			for (memory::string const& weapon : ov.equipment) {
				if (equipment_manager.find_equipment(weapon) == nullptr) {
					return skip(memory::fmt::format("the weapon \"{}\" does not exist", weapon));
				}
			}
			if (!has_manpower || manpower <= fixed_point_t::_0 || organisation <= fixed_point_t::_0 || speed <= fixed_point_t::_0) {
				return skip("manpower, organisation and speed must be above 0");
			}
			if (!ov.unlock_key.empty() && !is_valid_unlock_key(ov.unlock_key)) {
				return skip(memory::fmt::format("the unlock key \"{}\" is not <kind>:<id>", ov.unlock_key));
			}

			// The rest of the arguments of a regiment type of the game
			unit_args.icon = icon;
			unit_args.sprite = sprite;
			unit_args.starts_unlocked = ov.unlock_key.empty();
			unit_args.floating_flag = false;
			unit_args.priority = priority > 0 ? priority
				: unit_args.unit_category == UnitType::unit_category_t::INFANTRY ? 10
				: unit_args.unit_category == UnitType::unit_category_t::CAVALRY ? 25 : 20;
			unit_args.max_strength = manpower;
			unit_args.default_organisation = organisation;
			unit_args.maximum_speed = speed;
			unit_args.weighted_value = fixed_point_t(2);
			unit_args.supply_consumption = supply_consumption;
			unit_args.build_time = Timespan::from_days(build_days);
			regiment_args.attack = attack;
			regiment_args.defence = defense;
			regiment_args.reconnaissance = reconnaissance;
			regiment_args.discipline = fixed_point_t::_1;
			regiment_args.maneuver = fixed_point_t::_1;
			regiment_args.support = unit_args.unit_category == UnitType::unit_category_t::SUPPORT ? fixed_point_t(2) : fixed_point_t::_0;

			ov.breakthrough = breakthrough;
			ov.weight = weight;
			ov.occupation_weight = occupation_weight >= fixed_point_t::_0 ? occupation_weight
				: unit_args.unit_category == UnitType::unit_category_t::CAVALRY ? fixed_point_t(2)
				: unit_args.unit_category == UnitType::unit_category_t::SUPPORT ? fixed_point_t::_0_50 : fixed_point_t::_1;

			if (!add_regiment_type(key, unit_args, regiment_args)) {
				return skip("the type could not be registered, see the messages above");
			}
			regiment_types.get_item_by_identifier(key)->ov = std::move(ov);
			loaded++;
			return true;
		}
	)(root);

	if (!parsed) {
		spdlog::warn_s("The file of unit types {} is broken, what could be read is loaded", source);
	}
	return loaded;
}

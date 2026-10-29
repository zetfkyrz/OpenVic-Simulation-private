#include "Equipment.hpp"

#include <algorithm>
#include <fstream>
#include <string>

#include "openvic-simulation/dataloader/NodeTools.hpp"
#include "openvic-simulation/utility/Logger.hpp"

using namespace OpenVic;
using namespace OpenVic::NodeTools;

namespace {
	// The line of the file where the key is first written (0 if it cannot be found): for the messages about the files
	std::size_t find_key_line(std::string_view source, std::string_view key) {
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

	memory::string where(std::string_view source, std::string_view key) {
		const std::size_t line = find_key_line(source, key);
		return line > 0 ? memory::fmt::format("{}:{}", source, line) : memory::string { source };
	}

	bool is_known_slot(std::string_view slot) {
		return slot == "infantry" || slot == "cavalry" || slot == "support" || slot == "special";
	}
}

bool EquipmentDefinition::has_flag(std::string_view flag) const {
	return std::binary_search(flags.begin(), flags.end(), flag, [](std::string_view a, std::string_view b) { return a < b; });
}

std::size_t EquipmentManager::load_lines_file(std::string_view source, ast::NodeCPtr root) {
	std::size_t loaded = 0;

	const bool parsed = expect_dictionary(
		[this, source, &loaded](std::string_view key, ast::NodeCPtr value) -> bool {
			EquipmentLine line;
			line.identifier = key;
			line.source = source;

			const bool ok = expect_dictionary_keys_and_default(
				[source, key](std::string_view unknown_key, ast::NodeCPtr) -> bool {
					spdlog::warn_s("Unknown key \"{}\" in the equipment line \"{}\" ({})", unknown_key, key, where(source, unknown_key));
					return true;
				},
				"icon", ONE_EXACTLY, expect_identifier_or_string(assign_variable_callback_string(line.icon)),
				"name", ONE_EXACTLY, expect_identifier_or_string(assign_variable_callback_string(line.name_key)),
				"slot", ONE_EXACTLY, expect_identifier_or_string(assign_variable_callback_string(line.slot)),
				"unit_types", ZERO_OR_ONE, expect_list(expect_identifier_or_string(
					[&line](std::string_view unit_type) -> bool {
						line.unit_types.emplace_back(unit_type);
						return true;
					}
				))
			)(value);

			if (!ok) {
				spdlog::warn_s("Equipment line \"{}\" skipped, it is broken ({})", key, where(source, key));
				return true;
			}
			if (!is_known_slot(line.slot)) {
				spdlog::warn_s(
					"Equipment line \"{}\" skipped, the slot \"{}\" is none of infantry, cavalry, support, special ({})",
					key, line.slot, where(source, key)
				);
				return true;
			}

			if (const auto it = line_index_by_identifier.find(line.identifier); it != line_index_by_identifier.end()) {
				spdlog::warn_s(
					"Equipment line \"{}\" of {} replaces the one of {}", key, source, lines[it->second].source
				);
				lines[it->second] = std::move(line);
			} else {
				line_index_by_identifier.emplace(line.identifier, lines.size());
				lines.push_back(std::move(line));
			}
			loaded++;
			return true;
		}
	)(root);

	if (!parsed) {
		spdlog::warn_s("The equipment lines file {} is broken, what could be read is loaded", source);
	}
	return loaded;
}

std::size_t EquipmentManager::load_equipment_file(std::string_view source, ast::NodeCPtr root) {
	std::size_t loaded = 0;

	const bool parsed = expect_dictionary(
		[this, source, &loaded](std::string_view key, ast::NodeCPtr value) -> bool {
			EquipmentDefinition definition;
			definition.identifier = key;
			definition.source = source;
			definition.good.identifier = key;

			const bool ok = expect_dictionary_keys_and_default(
				[source, key](std::string_view unknown_key, ast::NodeCPtr) -> bool {
					spdlog::warn_s("Unknown key \"{}\" in the weapon \"{}\" ({})", unknown_key, key, where(source, unknown_key));
					return true;
				},
				"line", ONE_EXACTLY, expect_identifier_or_string(assign_variable_callback_string(definition.line)),
				"tier", ONE_EXACTLY, expect_uint(assign_variable_callback(definition.tier)),
				"unlock_key", ZERO_OR_ONE, expect_identifier_or_string(assign_variable_callback_string(definition.unlock_key)),
				"attack", ONE_EXACTLY, expect_fixed_point(assign_variable_callback(definition.attack)),
				"defence", ONE_EXACTLY, expect_fixed_point(assign_variable_callback(definition.defence)),
				"ammo_per_day", ZERO_OR_ONE, expect_fixed_point(assign_variable_callback(definition.ammo_per_day)),
				"ammo_good", ZERO_OR_ONE, expect_identifier_or_string(assign_variable_callback_string(definition.ammo_good)),
				"weight", ONE_EXACTLY, expect_fixed_point(assign_variable_callback(definition.weight)),
				"cost", ZERO_OR_ONE, expect_dictionary(
					[&definition](std::string_view good, ast::NodeCPtr amount) -> bool {
						fixed_point_t quantity = fixed_point_t::_0;
						if (!expect_fixed_point(assign_variable_callback(quantity))(amount)) {
							return false;
						}
						definition.cost.emplace_back(good, quantity);
						return true;
					}
				),
				"flags", ZERO_OR_ONE, expect_list(expect_identifier_or_string(
					[&definition](std::string_view flag) -> bool {
						definition.flags.emplace_back(flag);
						return true;
					}
				)),
				"good", ONE_EXACTLY, expect_dictionary_keys_and_default(
					[source, key](std::string_view unknown_key, ast::NodeCPtr) -> bool {
						spdlog::warn_s(
							"Unknown key \"{}\" in the good of the weapon \"{}\" ({})", unknown_key, key, where(source, unknown_key)
						);
						return true;
					},
					"id", ZERO_OR_ONE, expect_identifier_or_string(assign_variable_callback_string(definition.good.identifier)),
					"replaces", ZERO_OR_ONE, expect_identifier_or_string(assign_variable_callback_string(definition.good.replaces)),
					"base_price", ONE_EXACTLY, expect_fixed_point(assign_variable_callback(definition.good.base_price)),
					"icon", ZERO_OR_ONE, expect_identifier_or_string(assign_variable_callback_string(definition.good.icon)),
					"name", ZERO_OR_ONE, expect_identifier_or_string(assign_variable_callback_string(definition.good.name_key)),
					"colour", ZERO_OR_ONE, expect_colour(assign_variable_callback(definition.good.colour))
				)
			)(value);

			const auto skip = [source, key](std::string_view reason) -> bool {
				spdlog::warn_s("Weapon \"{}\" skipped: {} ({})", key, reason, where(source, key));
				return true; // the other weapons of the file are still loaded
			};

			if (!ok) {
				return skip("the file could not be read as a weapon, see the messages above");
			}
			if (find_line(definition.line) == nullptr) {
				return skip(memory::fmt::format("the line \"{}\" is not described in lines.txt", definition.line));
			}
			if (definition.tier < 1) {
				return skip("tier must be 1 or more");
			}
			if (definition.attack <= fixed_point_t::_0 || definition.defence <= fixed_point_t::_0) {
				return skip("attack and defence must be above 0");
			}
			if (definition.weight < fixed_point_t::_0 || definition.ammo_per_day < fixed_point_t::_0) {
				return skip("weight and ammo_per_day cannot be negative");
			}
			if (definition.good.base_price <= fixed_point_t::_0) {
				return skip("the base_price of the good must be above 0");
			}
			if (definition.good.identifier.empty()) {
				return skip("the good has no id");
			}

			if (definition.unlock_key.empty()) {
				definition.unlock_key = memory::fmt::format("equipment:{}", definition.identifier);
			}
			if (definition.good.icon.empty()) {
				definition.good.icon = definition.good.identifier;
			}
			if (definition.good.name_key.empty()) {
				definition.good.name_key = definition.good.identifier;
			}
			if (definition.good.colour == colour_t::null()) {
				definition.good.colour = colour_t::from_integer(0x808080);
			}
			std::sort(definition.flags.begin(), definition.flags.end());
			definition.flags.erase(std::unique(definition.flags.begin(), definition.flags.end()), definition.flags.end());

			if (const auto it = equipment_index_by_identifier.find(definition.identifier); it != equipment_index_by_identifier.end()) {
				spdlog::warn_s(
					"Weapon \"{}\" of {} replaces the one of {}", key, source, equipment[it->second].source
				);
				definition.index = it->second;
				equipment[it->second] = std::move(definition);
			} else {
				definition.index = equipment.size();
				equipment_index_by_identifier.emplace(definition.identifier, definition.index);
				equipment.push_back(std::move(definition));
			}
			loaded++;
			return true;
		}
	)(root);

	if (!parsed) {
		spdlog::warn_s("The weapon file {} is broken, what could be read is loaded", source);
	}
	return loaded;
}

EquipmentLine const* EquipmentManager::find_line(std::string_view identifier) const {
	const auto it = line_index_by_identifier.find(identifier);
	return it == line_index_by_identifier.end() ? nullptr : &lines[it->second];
}

EquipmentDefinition const* EquipmentManager::find_equipment(std::string_view identifier) const {
	const auto it = equipment_index_by_identifier.find(identifier);
	return it == equipment_index_by_identifier.end() ? nullptr : &equipment[it->second];
}

memory::vector<EquipmentDefinition const*> EquipmentManager::get_line_equipment(std::string_view line_identifier) const {
	memory::vector<EquipmentDefinition const*> result;
	for (EquipmentDefinition const& definition : equipment) {
		if (definition.line == line_identifier) {
			result.push_back(&definition);
		}
	}
	std::stable_sort(result.begin(), result.end(), [](EquipmentDefinition const* a, EquipmentDefinition const* b) {
		return a->tier < b->tier;
	});
	return result;
}

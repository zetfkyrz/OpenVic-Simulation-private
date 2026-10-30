#include "ArmyTechnology.hpp"

#include <algorithm>
#include <fstream>
#include <string>

#include "openvic-simulation/modifier/ModifierManager.hpp"
#include "openvic-simulation/utility/Logger.hpp"

using namespace OpenVic;
using namespace OpenVic::NodeTools;

namespace {
	std::size_t find_line(std::string_view source, std::string_view key) {
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
		const std::size_t line = find_line(source, key);
		return line > 0 ? memory::fmt::format("{}:{}", source, line) : memory::string { source };
	}
}

std::size_t ArmyTechnologyManager::load_file(std::string_view source, ast::NodeCPtr root, ModifierManager const& modifier_manager) {
	std::size_t loaded = 0;

	const bool parsed = expect_dictionary(
		[&](std::string_view key, ast::NodeCPtr value) -> bool {
			ArmyTechnology technology;
			technology.identifier = key;
			technology.source = source;
			technology.branch = "army";
			ModifierValue modifier_value;
			bool has_modifier = false;
			std::size_t modifier_keys = 0;
			bool has_x = false, has_y = false;

			const bool ok = expect_dictionary_keys_and_default(
				[source, key](std::string_view unknown_key, ast::NodeCPtr) -> bool {
					spdlog::warn_s("Unknown key \"{}\" in the technology \"{}\" ({})", unknown_key, key, where(source, unknown_key));
					return true;
				},
				"era", ONE_EXACTLY, expect_int(assign_variable_callback(technology.era)),
				"branch", ZERO_OR_ONE, expect_identifier_or_string(assign_variable_callback_string(technology.branch)),
				"x", ONE_EXACTLY, expect_int<std::int32_t>([&technology, &has_x](std::int32_t value_x) -> bool {
					technology.x = value_x;
					has_x = true;
					return true;
				}),
				"y", ONE_EXACTLY, expect_int<std::int32_t>([&technology, &has_y](std::int32_t value_y) -> bool {
					technology.y = value_y;
					has_y = true;
					return true;
				}),
				"research_days", ZERO_OR_ONE, expect_int(assign_variable_callback(technology.research_days)),
				"year_hint", ZERO_OR_ONE, expect_int(assign_variable_callback(technology.year_hint)),
				"start", ZERO_OR_ONE, expect_identifier_or_string(assign_variable_callback_string(technology.start)),
				"cost", ZERO_OR_ONE, success_callback, // reserved
				"icon", ZERO_OR_ONE, success_callback, // reserved (the icon of the branch is used)
				// Every "prerequisite = { ... }" is a block of its own: any technology of the block is enough, all the blocks are needed
				"prerequisite", ZERO_OR_MORE, [&technology](ast::NodeCPtr node) -> bool {
					technology.prerequisites.emplace_back();
					return expect_list(expect_identifier_or_string(
						[&technology](std::string_view prerequisite) -> bool {
							technology.prerequisites.back().emplace_back(prerequisite);
							return true;
						}
					))(node);
				},
				"unlocks", ZERO_OR_ONE, expect_list(expect_identifier_or_string(
					[&technology](std::string_view unlock) -> bool {
						if (!is_valid_unlock_key(unlock)) {
							spdlog::warn_s("The unlock key \"{}\" of the technology {} is not <kind>:<id>, skipped", unlock, technology.identifier);
							return true;
						}
						technology.unlocks.emplace_back(unlock);
						return true;
					}
				)),
				"modifiers", ZERO_OR_ONE, [&modifier_value, &has_modifier, &modifier_keys, &modifier_manager](ast::NodeCPtr modifier_node) -> bool {
					has_modifier = true;
					return expect_dictionary([&modifier_value, &modifier_keys, &modifier_manager](std::string_view modifier_key, ast::NodeCPtr node) -> bool {
						modifier_keys++;
						return modifier_manager.expect_technology_modifier(modifier_value)(modifier_key, node);
					})(modifier_node);
				}
			)(value);

			const auto skip = [&](std::string_view reason) -> bool {
				spdlog::warn_s("Technology \"{}\" skipped: {} ({})", key, reason, where(source, key));
				return true;
			};
			if (!ok) {
				return skip("the file could not be read as a technology, see the messages above");
			}
			if (!has_x || !has_y) {
				return skip("x and y (the cell of the grid) are needed");
			}
			// A key that is not a modifier is only warned about by the modifier manager, but the technology would work without it: skipped
			if (modifier_value.get_values().size() < modifier_keys) {
				return skip("a modifier is not known, see the messages above");
			}
			if (technology.research_days < 1) {
				return skip("research_days must be 1 or more");
			}
			if (index_by_identifier.contains(technology.identifier)) {
				return skip("a technology with this id was loaded already");
			}
			if (has_modifier) {
				technology.modifier.emplace(memory::fmt::format("army_tech_{}", technology.identifier), std::move(modifier_value));
			}
			technology.index = technologies.size();
			index_by_identifier.emplace(technology.identifier, technology.index);
			technologies.push_back(std::move(technology));
			loaded++;
			return true;
		}
	)(root);

	if (!parsed) {
		spdlog::warn_s("The technology file {} is broken, what could be read is loaded", source);
	}
	return loaded;
}

ArmyTechnology const* ArmyTechnologyManager::find(std::string_view identifier) const {
	const auto it = index_by_identifier.find(identifier);
	return it == index_by_identifier.end() ? nullptr : &technologies[it->second];
}

memory::vector<memory::string> ArmyTechnologyManager::get_branches() const {
	memory::vector<memory::string> branches;
	for (ArmyTechnology const& technology : technologies) {
		if (std::find(branches.begin(), branches.end(), technology.branch) == branches.end()) {
			branches.push_back(technology.branch);
		}
	}
	return branches;
}

memory::vector<memory::vector<std::size_t>> ArmyTechnologyManager::get_prerequisite_indices(ArmyTechnology const& technology) const {
	memory::vector<memory::vector<std::size_t>> blocks;
	for (memory::vector<memory::string> const& block : technology.prerequisites) {
		memory::vector<std::size_t>& indices = blocks.emplace_back();
		for (memory::string const& id : block) {
			if (const auto it = index_by_identifier.find(id); it != index_by_identifier.end()) {
				indices.push_back(it->second);
			}
		}
	}
	return blocks;
}

std::size_t ArmyTechnologyManager::validate() const {
	std::size_t problems = 0;

	// Missing prerequisites, a prerequisite that is not above the technology
	for (ArmyTechnology const& technology : technologies) {
		for (memory::vector<memory::string> const& block : technology.prerequisites) {
			for (memory::string const& id : block) {
				ArmyTechnology const* const prerequisite = find(id);
				if (prerequisite == nullptr) {
					spdlog::warn_s("The prerequisite \"{}\" of the technology {} does not exist ({})", id, technology.identifier, where(technology.source, id));
					problems++;
				} else if (prerequisite->branch == technology.branch && prerequisite->y >= technology.y) {
					spdlog::warn_s(
						"The prerequisite {} of the technology {} is not above it (y {} against {}) ({})", id, technology.identifier, prerequisite->y, technology.y,
						where(technology.source, technology.identifier)
					);
					problems++;
				}
			}
		}
	}

	// Two technologies in one cell of a branch
	for (std::size_t i = 0; i < technologies.size(); ++i) {
		for (std::size_t j = i + 1; j < technologies.size(); ++j) {
			if (technologies[i].branch == technologies[j].branch && technologies[i].x == technologies[j].x && technologies[i].y == technologies[j].y) {
				spdlog::warn_s(
					"The technologies {} and {} are in the same cell ({}, {}) of the branch {} ({})", technologies[i].identifier, technologies[j].identifier,
					technologies[i].x, technologies[i].y, technologies[i].branch, where(technologies[j].source, technologies[j].identifier)
				);
				problems++;
			}
		}
	}

	// Cycles: a technology that can be reached from itself by the prerequisites (depth-first, 0 new, 1 on the path, 2 done)
	std::vector<std::uint8_t> mark(technologies.size(), 0);
	std::vector<std::size_t> stack;
	const auto visit = [&](auto&& self, const std::size_t index) -> void {
		mark[index] = 1;
		stack.push_back(index);
		for (memory::vector<std::size_t> const& block : get_prerequisite_indices(technologies[index])) {
			for (const std::size_t next : block) {
				if (mark[next] == 1) {
					spdlog::warn_s(
						"The prerequisites of the technologies {} and {} make a cycle ({})", technologies[index].identifier, technologies[next].identifier,
						where(technologies[index].source, technologies[index].identifier)
					);
					problems++;
				} else if (mark[next] == 0) {
					self(self, next);
				}
			}
		}
		stack.pop_back();
		mark[index] = 2;
	};
	for (std::size_t i = 0; i < technologies.size(); ++i) {
		if (mark[i] == 0) {
			visit(visit, i);
		}
	}
	return problems;
}

#include "Unlocks.hpp"

#include <algorithm>

#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/economy/BuildingType.hpp"
#include "openvic-simulation/economy/production/FactoryInstance.hpp"
#include "openvic-simulation/economy/production/ProductionType.hpp"
#include "openvic-simulation/map/State.hpp"
#include "openvic-simulation/country/CountryInstanceManager.hpp"
#include "openvic-simulation/DefinitionManager.hpp"
#include "openvic-simulation/InstanceManager.hpp"
#include "openvic-simulation/military/Equipment.hpp"
#include "openvic-simulation/modifier/ModifierEffectCache.hpp"
#include "openvic-simulation/modifier/ModifierManager.hpp"
#include "openvic-simulation/utility/Logger.hpp"

using namespace OpenVic;
using namespace OpenVic::NodeTools;

bool OpenVic::is_valid_unlock_key(std::string_view key) {
	const std::size_t colon = key.find(':');
	if (colon == std::string_view::npos || colon == 0 || colon + 1 >= key.size()) {
		return false;
	}
	return std::none_of(key.begin(), key.end(), [](const char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; });
}

std::size_t ProvisionalUnlocks::load_file(std::string_view source, ast::NodeCPtr root, ModifierManager const& modifier_manager) {
	// What the file says, in the order of the file; the steps are made after sorting them by the year
	struct pending_step_t {
		int year = 0;
		memory::vector<memory::string> keys;
		ModifierValue modifier_value;
		bool has_modifier = false;
	};
	memory::vector<pending_step_t> pending;

	const bool parsed = expect_dictionary_keys_and_default(
		[source](std::string_view key, ast::NodeCPtr) -> bool {
			spdlog::warn_s("Unknown key \"{}\" in the provisional unlocks {}", key, source);
			return true;
		},
		"step", ZERO_OR_MORE, [&pending, source, &modifier_manager](ast::NodeCPtr value) -> bool {
			pending_step_t step;

			const bool ok = expect_dictionary_keys(
				"year", ONE_EXACTLY, expect_int(assign_variable_callback(step.year)),
				"unlock", ZERO_OR_ONE, expect_list(expect_identifier_or_string(
					[&step](std::string_view key) -> bool {
						if (!is_valid_unlock_key(key)) {
							spdlog::warn_s("The unlock key \"{}\" is not <kind>:<id>, skipped", key);
							return true;
						}
						step.keys.emplace_back(key);
						return true;
					}
				)),
				"modifier", ZERO_OR_ONE, [&step, &modifier_manager](ast::NodeCPtr modifier_node) -> bool {
					step.has_modifier = true;
					return expect_dictionary(modifier_manager.expect_technology_modifier(step.modifier_value))(modifier_node);
				}
			)(value);

			if (!ok) {
				spdlog::warn_s("A step of the provisional unlocks is skipped, it is broken ({})", source);
				return true;
			}
			pending.push_back(std::move(step));
			return true;
		}
	)(root);

	if (!parsed) {
		spdlog::warn_s("The provisional unlocks file {} is broken, what could be read is loaded", source);
	}

	// By the year (the order of the file for the same year); the steps do not move after this: the countries point to their modifiers
	memory::vector<std::size_t> order(pending.size());
	for (std::size_t i = 0; i < order.size(); i++) {
		order[i] = i;
	}
	std::stable_sort(order.begin(), order.end(), [&pending](std::size_t a, std::size_t b) { return pending[a].year < pending[b].year; });

	steps.clear();
	steps.reserve(pending.size());
	for (const std::size_t index : order) {
		pending_step_t& source_step = pending[index];
		ProvisionalUnlockStep& step = steps.emplace_back();
		step.year = source_step.year;
		step.keys = std::move(source_step.keys);
		if (source_step.has_modifier) {
			step.modifier.emplace(
				memory::fmt::format("provisional_unlock_{}_{}", step.year, steps.size() - 1), std::move(source_step.modifier_value)
			);
		}
	}
	return steps.size();
}

std::size_t DefaultUnlocks::load_file(std::string_view source, ast::NodeCPtr root) {
	keys.clear();
	const bool parsed = expect_dictionary_keys_and_default(
		[source](std::string_view key, ast::NodeCPtr) -> bool {
			spdlog::warn_s("Unknown key \"{}\" in the default unlocks {}", key, source);
			return true;
		},
		"unlock", ZERO_OR_MORE, expect_list(expect_identifier_or_string(
			[this, source](std::string_view key) -> bool {
				if (!is_valid_unlock_key(key)) {
					spdlog::warn_s("The unlock key \"{}\" of {} is not <kind>:<id>, skipped", key, source);
					return true;
				}
				keys.emplace_back(key);
				return true;
			}
		))
	)(root);
	if (!parsed) {
		spdlog::warn_s("The default unlocks file {} is broken, what could be read is loaded", source);
	}
	return keys.size();
}

void OpenVic::apply_default_unlocks(InstanceManager& instance_manager) {
	DefaultUnlocks const& defaults = instance_manager.definition_manager.get_military_manager().get_default_unlocks();
	if (defaults.get_keys().empty()) {
		return;
	}
	for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		for (memory::string const& key : defaults.get_keys()) {
			country.grant_unlock(key);
		}
	}
}

void OpenVic::update_provisional_unlocks(InstanceManager& instance_manager) {
	apply_default_unlocks(instance_manager);
	apply_provisional_unlocks(instance_manager, static_cast<int>(instance_manager.get_today().get_year()));
}

void OpenVic::apply_provisional_unlocks(InstanceManager& instance_manager, const int year) {
	OpenVicDefines const& defines = instance_manager.definition_manager.get_define_manager().get_openvic_defines();
	if (defines.war.provisional_unlocks_enabled == 0) {
		return;
	}
	ProvisionalUnlocks const& provisional = instance_manager.definition_manager.get_military_manager().get_provisional_unlocks();
	const std::span<const ProvisionalUnlockStep> steps = provisional.get_steps();

	for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		std::uint32_t applied = country.get_provisional_unlock_steps_applied();
		while (applied < steps.size() && steps[applied].year <= year) {
			ProvisionalUnlockStep const& step = steps[applied];
			for (memory::string const& key : step.keys) {
				country.grant_unlock(key);
			}
			if (step.modifier.has_value()) {
				country.add_unlock_modifier(*step.modifier);
			}
			applied++;
		}
		country.set_provisional_unlock_steps_applied(applied);
	}
}

DivisionTemplateLimits OpenVic::get_template_limits(InstanceManager const& instance_manager, CountryInstance const& country) {
	OpenVicWarDefines const& war = instance_manager.definition_manager.get_define_manager().get_openvic_defines().war;
	ModifierEffectCache const& cache = instance_manager.definition_manager.get_modifier_manager().get_modifier_effect_cache();

	const auto effect_value = [&country](ModifierEffect const* effect) -> fixed_point_t {
		return effect != nullptr ? country.get_modifier_effect_value(*effect) : fixed_point_t::_0;
	};
	const auto limited = [](const std::int32_t value, const std::int32_t min, const std::int32_t max) -> std::uint32_t {
		return static_cast<std::uint32_t>(std::clamp(value, min, std::max(min, max)));
	};

	DivisionTemplateLimits limits;
	limits.ranks = limited(
		war.template_ranks_default + effect_value(cache.get_max_columns_add()).floor<std::int32_t>(), 1, war.template_ranks_max
	);
	limits.depth = limited(
		war.template_depth_default + effect_value(cache.get_max_rows()).floor<std::int32_t>(), 1, war.template_depth_max
	);
	limits.company_slots = limited(
		war.template_company_slots_default + effect_value(cache.get_max_company_slots()).floor<std::int32_t>(), 0,
		war.template_company_slots_max
	);
	limits.row_weight_shift = std::clamp(effect_value(cache.get_row_weight_shift()).floor<std::int32_t>(), 0, 3);
	limits.density_reduction = std::clamp(effect_value(cache.get_density_reduction()), fixed_point_t::_0, fixed_point_t::_1);
	return limits;
}

bool OpenVic::is_equipment_unlocked(CountryInstance const& country, EquipmentDefinition const& equipment) {
	return country.has_unlock(equipment.unlock_key);
}

EquipmentDefinition const* OpenVic::get_best_equipment(
	EquipmentManager const& manager, CountryInstance const& country, std::string_view line
) {
	EquipmentDefinition const* best = nullptr;
	for (EquipmentDefinition const& equipment : manager.get_equipment()) {
		if (equipment.line == line && is_equipment_unlocked(country, equipment)
			&& (best == nullptr || equipment.tier > best->tier)) {
			best = &equipment;
		}
	}
	return best;
}

memory::vector<FactoryProductionOption> OpenVic::get_factory_production_options(
	InstanceManager const& instance_manager, BuildingType const& building_type
) {
	memory::vector<FactoryProductionOption> options;
	if (building_type.production_type == nullptr) {
		return options;
	}
	DefinitionManager const& definitions = instance_manager.definition_manager;
	EquipmentManager const& equipment_manager = definitions.get_military_manager().get_equipment_manager();

	const auto make_option = [&equipment_manager](ProductionType const& production_type) -> FactoryProductionOption {
		FactoryProductionOption option;
		option.production_type = &production_type;
		for (EquipmentDefinition const& equipment : equipment_manager.get_equipment()) {
			if (equipment.good.identifier == production_type.output_good.get_identifier()) {
				option.unlock_key = equipment.unlock_key;
				option.tier = equipment.tier;
				break;
			}
		}
		return option;
	};

	options.push_back(make_option(*building_type.production_type));
	for (FactoryRecipe const& recipe : definitions.get_economy_manager().get_production_type_manager().get_extra_recipes()) {
		if (recipe.building == building_type.get_identifier() && recipe.production_type != building_type.production_type) {
			options.push_back(make_option(*recipe.production_type));
		}
	}
	// The lowest generation first, the order of loading for the same generation
	std::stable_sort(options.begin(), options.end(), [](FactoryProductionOption const& a, FactoryProductionOption const& b) {
		return a.tier < b.tier;
	});
	return options;
}

bool OpenVic::is_production_option_available(CountryInstance const& country, FactoryProductionOption const& option) {
	return option.unlock_key.empty() || country.has_unlock(option.unlock_key);
}

ProductionType const* OpenVic::get_best_production_type(
	InstanceManager const& instance_manager, CountryInstance const& country, BuildingType const& building_type
) {
	ProductionType const* best = building_type.production_type;
	std::uint32_t best_tier = 0;
	for (FactoryProductionOption const& option : get_factory_production_options(instance_manager, building_type)) {
		if (is_production_option_available(country, option) && option.tier >= best_tier) {
			best = option.production_type;
			best_tier = option.tier;
		}
	}
	return best;
}

void OpenVic::choose_ai_production_types(InstanceManager& instance_manager, CountryInstance& country) {
	for (State* const state : country.get_states()) {
		for (FactoryInstance& factory : state->get_mutable_factories()) {
			if (factory.building_type.production_type == nullptr) {
				continue;
			}
			ProductionType const* const best = get_best_production_type(instance_manager, country, factory.building_type);
			ProductionType const* const current = factory.get_pending_production_type() != nullptr
				? factory.get_pending_production_type() : factory.production_type;
			if (best != nullptr && best != current) {
				factory.request_production_type(*best);
			}
		}
	}
}

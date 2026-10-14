#include "CountryAI.hpp"

#include <algorithm>
#include <unordered_set>
#include <vector>

#include "openvic-simulation/DefinitionManager.hpp"
#include "openvic-simulation/InstanceManager.hpp"
#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/economy/BuildingType.hpp"
#include "openvic-simulation/economy/GoodInstance.hpp"
#include "openvic-simulation/economy/production/FactoryInstance.hpp"
#include "openvic-simulation/economy/production/ProductionType.hpp"
#include "openvic-simulation/map/MapInstance.hpp"
#include "openvic-simulation/map/ProvinceInstance.hpp"
#include "openvic-simulation/map/State.hpp"
#include "openvic-simulation/military/UnitInstanceGroup.hpp"
#include "openvic-simulation/military/War.hpp"
#include "openvic-simulation/misc/GameAction.hpp"
#include "openvic-simulation/utility/Logger.hpp"

using namespace OpenVic;

namespace {
	// Deterministic "dice" in [0, 100) from the country, the date and a salt
	int dice(CountryInstance const& country, const Date date, const std::uint64_t salt) {
		std::uint64_t x = (static_cast<std::uint64_t>(type_safe::get(country.index)) + 1) * 0x9E3779B97F4A7C15ULL;
		x ^= static_cast<std::uint64_t>((date - Date {}).to_int()) * 0xBF58476D1CE4E5B9ULL;
		x ^= salt * 0x94D049BB133111EBULL;
		x ^= x >> 29;
		x *= 0xBF58476D1CE4E5B9ULL;
		x ^= x >> 32;
		return static_cast<int>(x % 100);
	}

	// Expected daily profit of one level of the factory at the current prices (the wages take about half of the value added)
	fixed_point_t expected_profit_per_level(InstanceManager& instance_manager, ProductionType const& production_type) {
		GoodInstanceManager const& goods = instance_manager.get_good_instance_manager();
		fixed_point_t value = goods.get_good_instance_by_definition(production_type.output_good).get_price()
			* production_type.base_output_quantity;
		for (auto const& [good, quantity] : production_type.input_goods) {
			value -= goods.get_good_instance_by_definition(*good).get_price() * quantity;
		}
		return value / 2;
	}

	void build_factory(InstanceManager& instance_manager, CountryInstance& country) {
		const fixed_point_t cash = country.get_cash_stockpile().load();
		BuildingTypeManager const& building_types = instance_manager.definition_manager.get_economy_manager().get_building_type_manager();

		struct Candidate {
			State* state;
			BuildingType const* building_type;
			fixed_point_t return_per_year;
		};
		Candidate best { nullptr, nullptr, fixed_point_t::_0 };

		std::unordered_set<State*> seen;
		for (ProvinceInstance* province : country.get_owned_provinces()) {
			State* const state = province->get_state();
			if (state == nullptr || state->get_owner() != &country || !seen.insert(state).second) {
				continue;
			}
			for (BuildingType const& building_type : building_types.get_building_types()) {
				ProductionType const* const production_type = building_type.production_type;
				if (production_type == nullptr || production_type->template_type != ProductionType::template_type_t::FACTORY
					|| building_type.cost <= fixed_point_t::_0 || cash < building_type.cost * 3) {
					continue;
				}
				if (!country.is_building_type_unlocked(building_type) || !production_type->is_valid_for_factory_in(*state)) {
					continue;
				}
				FactoryInstance* const existing = state->find_factory(building_type);
				if (existing != nullptr ? !existing->can_be_expanded() : !state->has_free_factory_slot()) {
					continue;
				}
				// One new project at a time per state
				const fixed_point_t return_per_year = expected_profit_per_level(instance_manager, *production_type) * 365 / building_type.cost;
				if (return_per_year > best.return_per_year) {
					best = { state, &building_type, return_per_year };
				}
			}
		}

		// Worth it if the factory pays back in time
		if (best.state != nullptr && best.return_per_year * CountryAI::FACTORY_PAYBACK_YEARS >= fixed_point_t::_1) {
			ProvinceInstance const* key = best.state->get_capital() != nullptr
				? best.state->get_capital() : &best.state->get_provinces().front().get();
			SPDLOG_INFO("AI: {} builds {} (return {:.2f} a year)", country.get_identifier(), best.building_type->get_identifier(),
				static_cast<double>(best.return_per_year));
			instance_manager.run_game_action_now<build_factory_argument_t>(country.index, key->index, best.building_type->index);
		}
	}

	void colonise(InstanceManager& instance_manager, CountryInstance& country) {
		ColonisationManager& colonisation = instance_manager.get_colonisation_manager();
		if (colonisation.get_total_points(instance_manager, country) - colonisation.get_used_points(instance_manager, country)
			< instance_manager.definition_manager.get_define_manager().get_country_defines().get_colonization_interest_cost_initial() / 2) {
			return;
		}
		// The nearest candidate to the capital
		ProvinceInstance const* capital = country.get_capital();
		ProvinceInstance* best = nullptr;
		fixed_point_t best_distance = fixed_point_t::_0;
		for (ProvinceInstance* candidate : colonisation.get_candidates(instance_manager, country)) {
			if (!colonisation.get_block_reason(instance_manager, country, *candidate).empty()) {
				continue;
			}
			// How far in map units, a rough measure
			fixed_point_t distance = fixed_point_t::_0;
			if (capital != nullptr) {
				const auto delta = candidate->province_definition.get_unit_position() - capital->province_definition.get_unit_position();
				distance = fixed_point_t::parse_unsafe(static_cast<float>(delta.x * delta.x + delta.y * delta.y));
			}
			if (best == nullptr || distance < best_distance) {
				best = candidate;
				best_distance = distance;
			}
		}
		if (best != nullptr) {
			instance_manager.run_game_action_now<start_colonisation_argument_t>(country.index, best->index);
		}
	}

	void recruit(InstanceManager& instance_manager, CountryInstance& country) {
		size_t regiments = 0;
		for (ArmyInstance const& army : country.get_armies()) {
			regiments += army.get_unit_count();
		}
		const size_t wanted = std::max<size_t>(
			3, country.get_owned_provinces().size() * CountryAI::WAR_REGIMENTS_PER_PROVINCE_PERMILLE / 1000
		);
		if (regiments >= wanted) {
			return;
		}

		RegimentType const* regiment_type = nullptr;
		for (RegimentType const& type : instance_manager.definition_manager.get_military_manager().get_unit_type_manager().get_regiment_types()) {
			if (country.is_unit_type_unlocked(type) && (regiment_type == nullptr || type.build_time < regiment_type->build_time)) {
				regiment_type = &type;
			}
		}
		if (regiment_type == nullptr) {
			return;
		}

		UnitInstanceManager& unit_manager = instance_manager.get_unit_instance_manager();
		int started = 0;
		for (ProvinceInstance* province : country.get_owned_provinces()) {
			if (started >= CountryAI::MAX_RECRUITS_PER_MONTH) {
				break;
			}
			if (province->get_controller() != &country) {
				continue;
			}
			for (Pop& pop : province->get_mutable_pops()) {
				if (UnitInstanceManager::get_recruitment_block_reason(country, *regiment_type, *province, pop).empty()
					&& unit_manager.start_recruitment(country, *regiment_type, *province, pop)) {
					++started;
					break;
				}
			}
		}
	}

	// A land neighbour country with its border province next to ours, nullptr if none can be attacked
	void look_for_war(InstanceManager& instance_manager, CountryInstance& country) {
		const Date today = instance_manager.get_today();
		WarManager& war_manager = instance_manager.get_war_manager();

		CountryInstance* target = nullptr;
		ProvinceInstance* border_province = nullptr;
		fixed_point_t target_power = fixed_point_t::_0;
		const fixed_point_t own_power = country.military_power.get_untracked();

		std::unordered_set<CountryInstance*> seen;
		for (ProvinceInstance* province : country.get_owned_provinces()) {
			for (ProvinceDefinition::adjacency_t const& adjacency : province->province_definition.get_adjacencies()) {
				if (adjacency.get_type() != ProvinceDefinition::adjacency_t::type_t::LAND || adjacency.get_to().is_water()) {
					continue;
				}
				ProvinceInstance& other = instance_manager.get_map_instance().get_province_instance_by_definition(adjacency.get_to());
				CountryInstance* const owner = other.get_owner();
				if (owner == nullptr || owner == &country || !seen.insert(owner).second) {
					continue;
				}
				if (owner->is_rebel_country() || !war_manager.can_declare_war(country, *owner, today)) {
					continue;
				}
				const int limit = owner->is_ai() ? CountryAI::MAX_TARGET_POWER_PERCENT : CountryAI::MAX_TARGET_POWER_PERCENT_PLAYER;
				const fixed_point_t power = owner->military_power.get_untracked();
				if (power * 100 > own_power * limit) {
					continue;
				}
				// An ally of the target that is stronger than us stops the plan
				bool dangerous = false;
				for (CountryInstance& candidate : instance_manager.get_country_instance_manager().get_country_instances()) {
					if (candidate.exists() && candidate.has_alliance_with(*owner) && candidate.military_power.get_untracked() > own_power) {
						dangerous = true;
						break;
					}
				}
				if (dangerous) {
					continue;
				}
				if (target == nullptr || power < target_power) {
					target = owner;
					target_power = power;
					border_province = &other;
				}
			}
		}

		if (target != nullptr && border_province != nullptr) {
			WarGoal goal;
			goal.type = WarGoal::type_t::CONQUER_STATE;
			goal.state_province = border_province->get_state() != nullptr && border_province->get_state()->get_capital() != nullptr
				? border_province->get_state()->get_capital() : border_province;
			if (war_manager.declare_war(instance_manager, country, *target, goal)) {
				SPDLOG_INFO("AI: {} declares war on {}", country.get_identifier(), target->get_identifier());
			}
		}
	}
}

void CountryAI::monthly_update(InstanceManager& instance_manager) {
	const Date today = instance_manager.get_today();

	for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		if (!country.exists() || country.is_rebel_country() || !country.is_ai() || country.get_owned_provinces().empty()) {
			continue;
		}

		build_factory(instance_manager, country);
		colonise(instance_manager, country);

		if (country.is_at_war()) {
			recruit(instance_manager, country);
		} else if (WAR_CHANCE_PERCENT_PER_YEAR > 0 && country.get_total_rank() > 0
			&& static_cast<int>(country.get_total_rank()) <= MAX_RANK_FOR_WARS
			// no wars in the first half year of the game
			&& today.get_year() * 12 + today.get_month() > 1836 * 12 + 7
			&& dice(country, today, 7) * 12 < WAR_CHANCE_PERCENT_PER_YEAR) {
			look_for_war(instance_manager, country);
		}
	}
}

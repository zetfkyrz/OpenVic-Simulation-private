#include "PopDynamics.hpp"

#include <algorithm>
#include <unordered_map>
#include <vector>

#include "openvic-simulation/DefinitionManager.hpp"
#include "openvic-simulation/InstanceManager.hpp"
#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/economy/production/FactoryInstance.hpp"
#include "openvic-simulation/map/MapInstance.hpp"
#include "openvic-simulation/map/ProvinceInstance.hpp"
#include "openvic-simulation/map/State.hpp"
#include "openvic-simulation/population/Culture.hpp"
#include "openvic-simulation/population/Pop.hpp"
#include "openvic-simulation/population/PopManager.hpp"
#include "openvic-simulation/population/PopType.hpp"
#include "openvic-simulation/utility/Logger.hpp"

using namespace OpenVic;

namespace {
	struct ProvinceScore {
		fixed_point_t unemployment;
		fixed_point_t life_needs;
		fixed_point_t score; // 0..1, higher is better
		bool has_pops = false;
	};

	// Whole people from a fraction: the whole part is returned and removed from the accumulator
	std::int32_t take_whole(fixed_point_t& accumulator) {
		const std::int32_t whole = accumulator.floor<std::int32_t>();
		if (whole > 0) {
			accumulator -= whole;
		}
		return std::max(whole, 0);
	}

	// Pop of the same type, culture and religion in the province, a new pop if there is none
	Pop* find_or_create_pop(
		ProvinceInstance& province, PopType const& type, Culture const& culture, Pop const& template_pop, PopDeps const& deps
	) {
		for (Pop& pop : province.get_mutable_pops()) {
			if (&pop.get_type() == &type && &pop.culture == &culture && &pop.religion == &template_pop.religion
				&& pop.get_rebel_type() == nullptr) {
				return &pop;
			}
		}
		return province.add_pop(static_cast<PopBase const&>(template_pop).with_changes(type, culture, pop_size_t(0)), deps);
	}

	ProvinceScore compute_score(ProvinceInstance& province) {
		ProvinceScore result;
		fixed_point_t total = fixed_point_t::_0;
		fixed_point_t unemployable_total = fixed_point_t::_0;
		fixed_point_t unemployed = fixed_point_t::_0;
		fixed_point_t life_sum = fixed_point_t::_0;

		for (Pop& pop : province.get_mutable_pops()) {
			if (pop.get_rebel_type() != nullptr || type_safe::get(pop.get_size()) <= 0) {
				continue;
			}
			const fixed_point_t size = type_safe::get(pop.get_size());
			total += size;
			life_sum += pop.get_life_needs_fulfilled() * size;
			if (pop.get_type().can_be_unemployed) {
				unemployable_total += size;
				unemployed += size * pop.get_unemployment_fraction();
			}
		}

		if (total > fixed_point_t::_0) {
			result.has_pops = true;
			result.life_needs = life_sum / total;
			result.unemployment = unemployable_total > fixed_point_t::_0 ? unemployed / unemployable_total : fixed_point_t::_0;
			result.score = (fixed_point_t::_1 - result.unemployment) / 2 + result.life_needs / 2;
		}
		return result;
	}
}

void PopDynamics::monthly_update(InstanceManager& instance_manager, PopDeps const& pop_deps) {
	MapInstance& map_instance = instance_manager.get_map_instance();
	PopManager const& pop_manager = instance_manager.definition_manager.get_pop_manager();

	PopType const* const farmers = pop_manager.get_pop_type_by_identifier("farmers");
	PopType const* const labourers = pop_manager.get_pop_type_by_identifier("labourers");
	PopType const* const craftsmen = pop_manager.get_pop_type_by_identifier("craftsmen");

	// Scores of the provinces before anything moves
	std::unordered_map<ProvinceInstance const*, ProvinceScore> scores;
	for (ProvinceInstance& province : map_instance.get_province_instances()) {
		if (!province.province_definition.is_water() && province.get_pop_count() > 0) {
			scores[&province] = compute_score(province);
		}
	}

	// Free jobs of the open factories of each state
	std::unordered_map<State const*, std::int32_t> vacancies;
	for (ProvinceInstance& province : map_instance.get_province_instances()) {
		State* const state = province.get_state();
		if (state == nullptr || vacancies.contains(state)) {
			continue;
		}
		std::int32_t free_jobs = 0;
		for (FactoryInstance const& factory : state->get_factories()) {
			if (factory.is_open() && !factory.is_under_construction()) {
				free_jobs += std::max(0, type_safe::get(factory.get_max_employee_count()) - type_safe::get(factory.get_employee_count()));
			}
		}
		vacancies[state] = free_jobs;
	}

	std::int32_t grown_total = 0;
	std::int32_t migrated_total = 0;
	std::int32_t assimilated_total = 0;
	std::int32_t promoted_total = 0;

	for (ProvinceInstance& province : map_instance.get_province_instances()) {
		if (province.province_definition.is_water() || province.get_pop_count() == 0) {
			continue;
		}
		CountryInstance const* const owner = province.get_owner();
		State* const state = province.get_state();

		// The pops are collected first, new pops added below must not be processed this month
		std::vector<Pop*> pops;
		for (Pop& pop : province.get_mutable_pops()) {
			pop.clear_dynamics_record();
			if (pop.get_rebel_type() == nullptr && type_safe::get(pop.get_size()) > 0) {
				pops.push_back(&pop);
			}
		}

		// Possible destinations of migrants: the other provinces of the state and the land neighbours of the same owner
		std::vector<ProvinceInstance*> destinations;
		if (owner != nullptr) {
			if (state != nullptr) {
				for (ProvinceInstance& other : state->get_provinces()) {
					if (&other != &province) {
						destinations.push_back(&other);
					}
				}
			}
			for (ProvinceDefinition::adjacency_t const& adjacency : province.province_definition.get_adjacencies()) {
				if (adjacency.get_type() != ProvinceDefinition::adjacency_t::type_t::LAND || adjacency.get_to().is_water()) {
					continue;
				}
				ProvinceInstance& other = map_instance.get_province_instance_by_definition(adjacency.get_to());
				if (other.get_owner() == owner && std::find(destinations.begin(), destinations.end(), &other) == destinations.end()) {
					destinations.push_back(&other);
				}
			}
		}
		ProvinceInstance* best_destination = nullptr;
		const ProvinceScore own_score = scores[&province];
		fixed_point_t best_score = own_score.score + fixed_point_t(MIGRATION_SCORE_MARGIN_PERCENT) / 100;
		for (ProvinceInstance* destination : destinations) {
			const auto it = scores.find(destination);
			if (it == scores.end() || !it->second.has_pops) {
				continue;
			}
			if (it->second.score > best_score) {
				best_score = it->second.score;
				best_destination = destination;
			}
		}

		for (Pop* pop : pops) {
			const std::int32_t size = type_safe::get(pop->get_size());
			if (size <= 0) {
				continue;
			}

			// ---- Growth: positive from half met life needs, starvation shrinks the pop faster
			const fixed_point_t life = pop->get_life_needs_fulfilled();
			const fixed_point_t breakeven = fixed_point_t(GROWTH_BREAKEVEN_PERCENT) / 100;
			fixed_point_t yearly_rate;
			if (life >= breakeven) {
				yearly_rate = (life - breakeven) / (fixed_point_t::_1 - breakeven) * GROWTH_PERMILLE_PER_YEAR / 1000;
			} else {
				yearly_rate = -(breakeven - life) / breakeven * DECLINE_PERMILLE_PER_YEAR / 1000;
			}
			pop->growth_remainder += yearly_rate * size / 12;
			if (pop->growth_remainder >= fixed_point_t::_1) {
				const std::int32_t whole = take_whole(pop->growth_remainder);
				pop->change_size(whole, 0);
				grown_total += whole;
			} else if (pop->growth_remainder <= -fixed_point_t::_1) {
				// Negative accumulator: take the whole people away
				fixed_point_t positive = -pop->growth_remainder;
				const std::int32_t whole = take_whole(positive);
				pop->growth_remainder = -positive;
				pop->change_size(-whole, 0);
				grown_total -= whole;
			}

			const PopType& type = pop->get_type();

			// ---- Assimilation of unaccepted cultures into the primary culture
			if (owner != nullptr && owner->get_primary_culture() != nullptr
				&& pop->get_culture_status() == Pop::culture_status_t::UNACCEPTED && !type.is_slave) {
				Culture const& primary = *owner->get_primary_culture();
				const int rate = &pop->culture.group == &primary.group ? ASSIMILATION_PERMILLE_PER_MONTH * 2 : ASSIMILATION_PERMILLE_PER_MONTH;
				pop->assimilation_remainder += fixed_point_t(rate) * type_safe::get(pop->get_size()) / 1000;
				const std::int32_t whole = std::min(take_whole(pop->assimilation_remainder), type_safe::get(pop->get_size()));
				if (whole > 0) {
					if (Pop* target = find_or_create_pop(province, type, primary, *pop, pop_deps)) {
						pop->change_size(-whole, 3);
						target->change_size(whole, 3);
						assimilated_total += whole;
					}
				}
			}

			// ---- Demotion of unemployed craftsmen / promotion of unemployed farmers and labourers
			const fixed_point_t unemployment = pop->get_unemployment_fraction();
			if (craftsmen != nullptr && farmers != nullptr && labourers != nullptr) {
				if (&type == craftsmen && unemployment > fixed_point_t(1) / 4) {
					PopType const* target_type = labourers;
					// The RGO decides which of the two is needed: the type with more people in the province
					fixed_point_t farmers_size = fixed_point_t::_0, labourers_size = fixed_point_t::_0;
					for (Pop& other : province.get_mutable_pops()) {
						if (&other.get_type() == farmers) {
							farmers_size += type_safe::get(other.get_size());
						} else if (&other.get_type() == labourers) {
							labourers_size += type_safe::get(other.get_size());
						}
					}
					if (farmers_size > labourers_size) {
						target_type = farmers;
					}
					pop->migration_remainder += fixed_point_t(type_safe::get(pop->get_unemployed())) * PROMOTION_PERCENT_PER_MONTH / 100;
					const std::int32_t whole = std::min(take_whole(pop->migration_remainder), type_safe::get(pop->get_size()));
					if (whole > 0) {
						if (Pop* target = find_or_create_pop(province, *target_type, pop->culture, *pop, pop_deps)) {
							pop->change_size(-whole, 2);
							target->change_size(whole, 2);
						}
					}
				} else if ((&type == farmers || &type == labourers) && unemployment > fixed_point_t(15) / 100
					&& state != nullptr && vacancies[state] > 0) {
					pop->migration_remainder += fixed_point_t(type_safe::get(pop->get_unemployed())) * PROMOTION_PERCENT_PER_MONTH / 100;
					const std::int32_t whole = std::min({
						take_whole(pop->migration_remainder), type_safe::get(pop->get_size()), vacancies[state]
					});
					if (whole > 0) {
						if (Pop* target = find_or_create_pop(province, *craftsmen, pop->culture, *pop, pop_deps)) {
							pop->change_size(-whole, 1);
							target->change_size(whole, 1);
							vacancies[state] -= whole;
							promoted_total += whole;
						}
					}
				}
			}

			// ---- Internal migration to a better province: the unemployed and the hungry leave
			if (best_destination != nullptr && !type.is_slave && type_safe::get(pop->get_size()) > 0
				&& (unemployment > fixed_point_t(1) / 5 || pop->get_life_needs_fulfilled() < fixed_point_t::_0_50)) {
				pop->migration_remainder += fixed_point_t(MIGRATION_PERMILLE_PER_MONTH) * type_safe::get(pop->get_size()) / 1000;
				const std::int32_t whole = std::min(take_whole(pop->migration_remainder), type_safe::get(pop->get_size()));
				if (whole > 0) {
					if (Pop* target = find_or_create_pop(*best_destination, type, pop->culture, *pop, pop_deps)) {
						pop->change_size(-whole, 3);
						target->change_size(whole, 3);
						migrated_total += whole;
					}
				}
			}
		}
	}

	SPDLOG_INFO(
		"Population changes: grown {}, migrated {}, assimilated {}, promoted to craftsmen {}",
		grown_total, migrated_total, assimilated_total, promoted_total
	);
}

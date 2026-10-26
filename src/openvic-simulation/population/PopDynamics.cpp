#include "PopDynamics.hpp"

#include <algorithm>
#include <unordered_map>
#include <vector>

#include "openvic-simulation/DefinitionManager.hpp"
#include "openvic-simulation/InstanceManager.hpp"
#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/economy/production/FactoryInstance.hpp"
#include "openvic-simulation/economy/production/ProductionType.hpp"
#include "openvic-simulation/economy/production/ResourceGatheringOperation.hpp"
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
		// The sums the score of the region (state) is made of
		fixed_point_t total, unemployable_total, unemployed, life_sum;
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

	ProvinceScore compute_score(ProvinceInstance& province, OpenVicPopulationDefines const& defines) {
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

		result.total = total;
		result.unemployable_total = unemployable_total;
		result.unemployed = unemployed;
		result.life_sum = life_sum;
		if (total > fixed_point_t::_0) {
			result.has_pops = true;
			result.life_needs = life_sum / total;
			result.unemployment = unemployable_total > fixed_point_t::_0 ? unemployed / unemployable_total : fixed_point_t::_0;
			result.score = defines.score_unemployment_weight.of(fixed_point_t::_1 - result.unemployment) + defines.score_life_needs_weight.of(result.life_needs);
		}
		return result;
	}
}

void PopDynamics::monthly_update(InstanceManager& instance_manager, PopDeps const& pop_deps) {
	OpenVicPopulationDefines const& defines =
		instance_manager.definition_manager.get_define_manager().get_openvic_defines().population;
	MapInstance& map_instance = instance_manager.get_map_instance();
	PopManager const& pop_manager = instance_manager.definition_manager.get_pop_manager();

	PopType const* const farmers = pop_manager.get_pop_type_by_identifier("farmers");
	PopType const* const labourers = pop_manager.get_pop_type_by_identifier("labourers");
	PopType const* const craftsmen = pop_manager.get_pop_type_by_identifier("craftsmen");

	// Scores of the provinces before anything moves
	std::unordered_map<ProvinceInstance const*, ProvinceScore> scores;
	for (ProvinceInstance& province : map_instance.get_province_instances()) {
		if (!province.province_definition.is_water() && province.get_pop_count() > 0) {
			scores[&province] = compute_score(province, defines);
		}
	}

	// The scores of the regions (states): the people move between regions, a move between the provinces of one region means nothing
	std::unordered_map<State const*, ProvinceScore> state_scores;
	for (auto const& [province_ptr, province_score] : scores) {
		State const* const score_state = province_ptr->get_state();
		if (score_state == nullptr) {
			continue;
		}
		ProvinceScore& sum = state_scores[score_state];
		sum.total += province_score.total;
		sum.unemployable_total += province_score.unemployable_total;
		sum.unemployed += province_score.unemployed;
		sum.life_sum += province_score.life_sum;
	}
	for (auto& [score_state, sum] : state_scores) {
		if (sum.total > fixed_point_t::_0) {
			sum.has_pops = true;
			sum.life_needs = sum.life_sum / sum.total;
			sum.unemployment = sum.unemployable_total > fixed_point_t::_0 ? sum.unemployed / sum.unemployable_total : fixed_point_t::_0;
			sum.score = defines.score_unemployment_weight.of(fixed_point_t::_1 - sum.unemployment)
				+ defines.score_life_needs_weight.of(sum.life_needs);
		}
	}
	const auto region_score = [&scores, &state_scores](ProvinceInstance const* province) -> ProvinceScore {
		if (province->get_state() != nullptr) {
			const auto it = state_scores.find(province->get_state());
			if (it != state_scores.end()) {
				return it->second;
			}
		}
		const auto it = scores.find(province);
		return it != scores.end() ? it->second : ProvinceScore {};
	};

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

	// Free jobs of the resource gathering operations by country and pop type: where the surplus people can go
	struct Vacancy {
		ProvinceInstance* province;
		std::int32_t free_jobs;
	};
	const std::size_t country_count = instance_manager.get_country_instance_manager().get_country_instances().size();
	const std::size_t type_count = static_cast<std::size_t>(pop_manager.get_pop_type_count());
	std::vector<std::vector<Vacancy>> job_vacancies(country_count * type_count);
	std::vector<std::int64_t> free_jobs_total(country_count * type_count, 0);
	for (ProvinceInstance& province : map_instance.get_province_instances()) {
		CountryInstance const* const rgo_owner = province.get_owner();
		if (rgo_owner == nullptr || province.province_definition.is_water()) {
			continue;
		}
		ProductionType const* const production_type = province.get_rgo().get_production_type_nullable();
		if (production_type == nullptr) {
			continue;
		}
		const std::int32_t free_jobs = type_safe::get(province.get_rgo().get_max_employee_count_cache())
			- type_safe::get(province.get_rgo().get_total_employees_count_cache());
		if (free_jobs <= 0) {
			continue;
		}
		for (Job const& job : production_type->get_jobs()) {
			const std::size_t slot = type_safe::get(rgo_owner->index) * type_count + type_safe::get(job.pop_type_index);
			job_vacancies[slot].push_back({ &province, free_jobs });
			free_jobs_total[slot] += free_jobs;
		}
	}
	for (std::vector<Vacancy>& list : job_vacancies) {
		std::sort(list.begin(), list.end(), [](Vacancy const& lhs, Vacancy const& rhs) -> bool {
			if (lhs.free_jobs != rhs.free_jobs) {
				return lhs.free_jobs > rhs.free_jobs;
			}
			return type_safe::get(lhs.province->province_definition.index) < type_safe::get(rhs.province->province_definition.index);
		});
	}

	// Moves people of a pop to the free jobs of a country (best places first), returns how many moved
	const auto move_to_vacancies = [&](
		Pop& source, ProvinceInstance& source_province, const std::size_t slot, std::int32_t count, const std::int32_t kind
	) -> std::int32_t {
		std::int32_t moved = 0;
		for (Vacancy& vacancy : job_vacancies[slot]) {
			if (count <= 0) {
				break;
			}
			if (vacancy.free_jobs <= 0 || vacancy.province == &source_province) {
				continue;
			}
			// Not more than a share of the free jobs of a place in a month
			const std::int32_t room = std::max<std::int32_t>(1, static_cast<std::int32_t>(defines.immigration_fill_share.of_int(vacancy.free_jobs)));
			const std::int32_t take = std::min({ count, room, vacancy.free_jobs });
			Pop* const target = find_or_create_pop(*vacancy.province, source.get_type(), source.culture, source, pop_deps);
			if (target == nullptr) {
				continue;
			}
			source.change_size(-take, kind);
			target->change_size(take, kind);
			vacancy.free_jobs -= take;
			free_jobs_total[slot] -= take;
			count -= take;
			moved += take;
		}
		return moved;
	};

	std::int32_t starved_total = 0;
	std::int32_t emigrated_internal_total = 0;
	std::int32_t emigrated_external_total = 0;
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

		// Possible destinations of migrants: the land neighbours of the same owner in other regions (the better region wins)
		std::vector<ProvinceInstance*> destinations;
		if (owner != nullptr) {
			for (ProvinceDefinition::adjacency_t const& adjacency : province.province_definition.get_adjacencies()) {
				if (adjacency.get_type() != ProvinceDefinition::adjacency_t::type_t::LAND || adjacency.get_to().is_water()) {
					continue;
				}
				ProvinceInstance& other = map_instance.get_province_instance_by_definition(adjacency.get_to());
				if (other.get_owner() == owner && other.get_state() != state
					&& std::find(destinations.begin(), destinations.end(), &other) == destinations.end()) {
					destinations.push_back(&other);
				}
			}
		}
		ProvinceInstance* best_destination = nullptr;
		const ProvinceScore own_score = region_score(&province);
		fixed_point_t best_score = own_score.score + defines.migration_score_margin.fixed();
		for (ProvinceInstance* destination : destinations) {
			const ProvinceScore destination_score = region_score(destination);
			if (!destination_score.has_pops) {
				continue;
			}
			if (destination_score.score > best_score) {
				best_score = destination_score.score;
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
			const fixed_point_t breakeven = defines.growth_breakeven_life_needs.fixed();
			fixed_point_t yearly_rate;
			if (life >= breakeven) {
				yearly_rate = defines.growth_per_year.of((life - breakeven) / (fixed_point_t::_1 - breakeven));
			} else {
				yearly_rate = defines.decline_per_year.of(-(breakeven - life) / breakeven);
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
				// Hunger never empties a pop (code that divides by the size of a pop would break on an empty one)
				const std::int32_t can_go = std::max(0, type_safe::get(pop->get_size()) - 1);
				pop->change_size(-std::min(whole, can_go), 0);
				grown_total -= std::min(whole, can_go);
			}

			const PopType& type = pop->get_type();

			// ---- The surplus: the unemployed of a pop that was hungry for a month. A small share of them emigrates (first inside the country
			// to the places with free jobs, abroad only when the whole country has none), the rest dies
			if (pop->hungry_days >= defines.hunger_days_before_death && type.can_be_unemployed && !type.is_slave && owner != nullptr) {
				const std::int32_t surplus = type_safe::get(pop->get_unemployed());
				if (surplus > 0 && surplus >= defines.starvation_min_surplus) {
					const std::size_t own_slot = type_safe::get(owner->index) * type_count + type_safe::get(type.index);
					// TODO the technologies raise the share of the emigrants (the research system adds a bonus here)
					// A pop never disappears from hunger or emigration: at least one person stays
					const std::int32_t people_that_may_go = std::max(0, type_safe::get(pop->get_size()) - 1);
					const std::int32_t emigrants = std::min(
						static_cast<std::int32_t>(defines.emigration_share.of_int(surplus)), people_that_may_go
					);
					std::int32_t placed = 0;
					if (emigrants > 0) {
						if (free_jobs_total[own_slot] > 0) {
							placed = move_to_vacancies(*pop, province, own_slot, emigrants, 3);
							emigrated_internal_total += placed;
						} else {
							// Abroad: the country with the most free jobs for this kind of people
							std::size_t best_slot = own_slot;
							std::int64_t best_free = 0;
							for (std::size_t country = 0; country < country_count; ++country) {
								const std::size_t slot = country * type_count + type_safe::get(type.index);
								if (slot != own_slot && free_jobs_total[slot] > best_free) {
									best_free = free_jobs_total[slot];
									best_slot = slot;
								}
							}
							if (best_slot != own_slot) {
								placed = move_to_vacancies(*pop, province, best_slot, emigrants, 4);
								emigrated_external_total += placed;
							}
						}
					}
					// The rest of the surplus dies (the emigrants that found no place stay and are counted with the rest)
					const std::int32_t staying = surplus - placed;
					const std::int32_t dying = std::min(
						static_cast<std::int32_t>(defines.surplus_death_share.of_int(std::max(0, staying - (emigrants - placed)))),
						std::max(0, people_that_may_go - placed)
					);
					if (dying > 0) {
						pop->change_size(-dying, 5);
						starved_total += dying;
					}
				}
			}
			pop->hungry_days = 0;

			// ---- Assimilation of unaccepted cultures into the primary culture
			if (owner != nullptr && owner->get_primary_culture() != nullptr
				&& pop->get_culture_status() == Pop::culture_status_t::UNACCEPTED && !type.is_slave) {
				Culture const& primary = *owner->get_primary_culture();
				// Inside a culture group the assimilation is faster
				const std::int32_t multiplier = &pop->culture.group == &primary.group ? defines.assimilation_same_group_multiplier : 1;
				pop->assimilation_remainder += defines.assimilation_per_month.of(
					fixed_point_t(type_safe::get(pop->get_size())) * multiplier
				);
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
				if (&type == craftsmen && unemployment > defines.craftsmen_demotion_unemployment.fixed()) {
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
					pop->migration_remainder += defines.promotion_per_month.of(fixed_point_t(type_safe::get(pop->get_unemployed())));
					const std::int32_t whole = std::min(take_whole(pop->migration_remainder), type_safe::get(pop->get_size()));
					if (whole > 0) {
						if (Pop* target = find_or_create_pop(province, *target_type, pop->culture, *pop, pop_deps)) {
							pop->change_size(-whole, 2);
							target->change_size(whole, 2);
						}
					}
				} else if ((&type == farmers || &type == labourers) && unemployment > defines.peasants_promotion_unemployment.fixed()
					&& state != nullptr && vacancies[state] > 0) {
					pop->migration_remainder += defines.promotion_per_month.of(fixed_point_t(type_safe::get(pop->get_unemployed())));
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
				&& (unemployment > defines.migration_unemployment_threshold.fixed()
					|| pop->get_life_needs_fulfilled() < defines.migration_life_needs_threshold.fixed())) {
				pop->migration_remainder += defines.migration_per_month.of(fixed_point_t(type_safe::get(pop->get_size())));
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
		"Population changes: grown {}, migrated {}, assimilated {}, promoted to craftsmen {}, died of hunger {}, emigrated inside the country {} and abroad {}",
		grown_total, migrated_total, assimilated_total, promoted_total, starved_total, emigrated_internal_total, emigrated_external_total
	);
}

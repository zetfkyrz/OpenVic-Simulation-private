#include "CountryAI.hpp"

#include "openvic-simulation/military/Unlocks.hpp"

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
#include "openvic-simulation/economy/production/RgoBuilding.hpp"
#include "openvic-simulation/map/MapInstance.hpp"
#include "openvic-simulation/population/PopManager.hpp"
#include "openvic-simulation/map/ProvinceInstance.hpp"
#include "openvic-simulation/map/State.hpp"
#include "openvic-simulation/military/Division.hpp"
#include "openvic-simulation/military/UnitInstanceGroup.hpp"
#include "openvic-simulation/military/War.hpp"
#include "openvic-simulation/misc/GameAction.hpp"
#include "openvic-simulation/utility/Logger.hpp"

using namespace OpenVic;

namespace {
	OpenVicAIDefines const& get_ai_defines(InstanceManager const& instance_manager) {
		return instance_manager.definition_manager.get_define_manager().get_openvic_defines().ai;
	}

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
		return value / get_ai_defines(instance_manager).expected_wage_divisor;
	}

	void build_factory(InstanceManager& instance_manager, CountryInstance& country) {
		OpenVicAIDefines const& ai = get_ai_defines(instance_manager);
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
					|| building_type.cost <= fixed_point_t::_0 || cash < building_type.cost * ai.factory_cash_cost_multiple) {
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
		if (best.state != nullptr && best.return_per_year * ai.factory_payback_years >= fixed_point_t::_1) {
			ProvinceInstance const* key = best.state->get_capital() != nullptr
				? best.state->get_capital() : &best.state->get_provinces().front().get();
			SPDLOG_INFO("AI: {} builds {} (return {:.2f} a year)", country.get_identifier(), best.building_type->get_identifier(),
				static_cast<double>(best.return_per_year));
			instance_manager.run_game_action_now<build_factory_argument_t>(country.index, key->index, best.building_type->index);
		}
	}

	// The budget of the AI country (OV_AI_DEFINES BUDGET_*): the sliders of the AI used to stay at 0.5 for ever, so a poor country spent twice its income,
	// the treasury was empty and the budgets (the army pay too) were cut every day. Deterministic: only the state of the country is read.
	void balance_budget(InstanceManager& instance_manager, CountryInstance& country) {
		OpenVicAIDefines const& ai = get_ai_defines(instance_manager);
		if (ai.budget_enabled == 0) {
			return;
		}
		const fixed_point_t spending = country.projected_spending.get_untracked();
		const fixed_point_t income = country.get_income_yesterday();
		if (spending <= fixed_point_t::_0 || income <= fixed_point_t::_0) {
			return; // the first days: nothing was collected yet
		}
		const fixed_point_t cash = country.get_cash_stockpile().load();
		const fixed_point_t reserve = spending * ai.budget_reserve_days;
		const fixed_point_t step = ai.budget_step.fixed();
		PopManager const& pop_manager = instance_manager.definition_manager.get_pop_manager();

		if (cash < reserve && income < spending) {
			bool taxes_at_top = true;
			for (Strata const& strata : pop_manager.get_stratas()) {
				const fixed_point_t rate = country.get_tax_rate_slider_value_by_strata(strata).get_value_untracked();
				const fixed_point_t top = std::min(ai.budget_max_tax.fixed(), country.get_tax_rate_slider_value_by_strata(strata).get_max());
				if (rate < top) {
					country.set_strata_tax_rate_slider_value(strata, std::min(top, rate + step));
					taxes_at_top = false;
				}
			}
			if (taxes_at_top) {
				const fixed_point_t floor = ai.budget_min_spending.fixed();
				country.set_military_spending_slider_value(std::max(floor, country.get_military_spending_slider_value().get_value_untracked() - step));
				country.set_administration_spending_slider_value(std::max(floor, country.get_administration_spending_slider_value().get_value_untracked() - step));
				country.set_education_spending_slider_value(std::max(floor, country.get_education_spending_slider_value().get_value_untracked() - step));
			}
		} else if (cash > reserve * 2 && income * Rate::DENOMINATOR > spending * ai.budget_surplus_ratio.numerator) {
			const fixed_point_t middle = fixed_point_t::_0_50;
			for (Strata const& strata : pop_manager.get_stratas()) {
				const fixed_point_t rate = country.get_tax_rate_slider_value_by_strata(strata).get_value_untracked();
				if (rate > middle) {
					country.set_strata_tax_rate_slider_value(strata, std::max(middle, rate - step));
				}
			}
			const auto restore = [&](fixed_point_t value) { return value < middle ? std::min(middle, value + step) : value; };
			country.set_military_spending_slider_value(restore(country.get_military_spending_slider_value().get_value_untracked()));
			country.set_administration_spending_slider_value(restore(country.get_administration_spending_slider_value().get_value_untracked()));
			country.set_education_spending_slider_value(restore(country.get_education_spending_slider_value().get_value_untracked()));
		}
	}

	void colonise(InstanceManager& instance_manager, CountryInstance& country) {
		OpenVicAIDefines const& ai = get_ai_defines(instance_manager);
		ColonisationManager& colonisation = instance_manager.get_colonisation_manager();
		if (colonisation.get_total_points(instance_manager, country) - colonisation.get_used_points(instance_manager, country)
			< instance_manager.definition_manager.get_define_manager().get_country_defines().get_colonization_interest_cost_initial() / ai.colonise_min_points_divisor) {
			return;
		}
		// The nearest candidate to the capital
		ProvinceInstance const* capital = country.get_capital();
		ProvinceInstance* best = nullptr;
		fixed_point_t best_distance = fixed_point_t::_0;
		for (ProvinceInstance* candidate : colonisation.get_startable_candidates(instance_manager, country)) {
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

	// The regiments of the country that are not mobilised (the mobilised ones go home after the war)
	size_t regular_regiment_count(CountryInstance const& country) {
		size_t regiments = 0;
		for (ArmyInstance const& army : country.get_armies()) {
			for (RegimentInstance const& regiment : army.get_regiment_instances()) {
				regiments += !regiment.is_mobilised();
			}
		}
		return regiments;
	}

	void recruit(InstanceManager& instance_manager, CountryInstance& country) {
		OpenVicAIDefines const& ai = get_ai_defines(instance_manager);
		const size_t regiments = regular_regiment_count(country);
		const size_t wanted = std::max<size_t>(
			static_cast<size_t>(ai.min_war_regiments),
			static_cast<size_t>(ai.war_regiments_per_province.of_int(static_cast<std::int64_t>(country.get_owned_provinces().size())))
		);
		if (regiments >= wanted) {
			return;
		}

		UnitInstanceManager& unit_manager = instance_manager.get_unit_instance_manager();
		// Over the monthly limit of the recruitment (ov_war.RECRUIT_MAX_REGIMENTS_PER_MONTH) the AI does not queue orders that would wait
		if (unit_manager.get_recruitment_limit_left(country) == 0) {
			return;
		}
		// Whole divisions by the first template: the capital first, then the other provinces of the country (the soldiers come from the whole
		// country, the province is where the division appears)
		if (ai.ai_divisions_per_month > 0 && !country.get_division_templates().empty()) {
			UnitTypeManager const& unit_type_manager = instance_manager.definition_manager.get_military_manager().get_unit_type_manager();
			DivisionTemplate const& division_template = country.get_division_templates().front();
			std::vector<ProvinceInstance*> places;
			if (country.get_capital() != nullptr && country.get_capital()->get_controller() == &country) {
				places.push_back(country.get_capital());
			}
			for (ProvinceInstance* province : country.get_owned_provinces()) {
				if (province != country.get_capital() && province->get_controller() == &country) {
					places.push_back(province);
				}
			}
			int divisions = 0;
			for (ProvinceInstance* province : places) {
				if (divisions >= ai.ai_divisions_per_month) {
					break;
				}
				if (unit_manager.get_division_recruitment_block_reason(country, unit_type_manager, division_template, *province).empty()
					&& unit_manager.start_division_recruitment(country, unit_type_manager, division_template, *province)) {
					++divisions;
				}
				// The reason is the same in every province but the first (the manpower and the goods are of the country)
				if (divisions == 0) {
					break;
				}
			}
			if (divisions > 0) {
				return;
			}
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

		int started = 0;
		for (ProvinceInstance* province : country.get_owned_provinces()) {
			if (started >= ai.max_recruits_per_month) {
				break;
			}
			if (province->get_controller() != &country) {
				continue;
			}
			for (Pop& pop : province->get_mutable_pops()) {
				if (unit_manager.get_recruitment_block_reason(country, *regiment_type, *province, pop).empty()
					&& unit_manager.start_recruitment(country, *regiment_type, *province, pop)) {
					++started;
					break;
				}
			}
		}
	}

	// The allies in a war (docs/WAR_ASSUMPTIONS.md, "Союзники и отдельный мир"): the leader of a side calls its AI allies that are free into its war;
	// a tired country that does not lead its side asks for a separate peace
	void war_allies(InstanceManager& instance_manager, CountryInstance& country) {
		WarManager& war_manager = instance_manager.get_war_manager();
		if (War const* const war = war_manager.get_war_to_call_into(country); war != nullptr) {
			const bool attacker_side = war->is_attacker(country);
			const std::uint32_t war_id = war->id;
			if (war->get_side(attacker_side).front() == &country) {
				for (CountryInstance& ally : instance_manager.get_country_instance_manager().get_country_instances()) {
					War const* const current = war_manager.get_war_by_id(war_id);
					if (current == nullptr) {
						break;
					}
					if (&ally != &country && ally.is_ai() && country.has_alliance_with(ally) && war_manager.can_join_war(*current, attacker_side, ally)
						&& war_manager.would_join_war(*current, ally)) {
						war_manager.join_war(instance_manager, war_id, ally, attacker_side);
					}
				}
			}
		}
		if (country.get_war_exhaustion_max() <= fixed_point_t::_0
			|| country.get_war_exhaustion() < war_manager.get_war_defines().separate_peace_ai_tiredness.of(country.get_war_exhaustion_max())) {
			return;
		}
		std::vector<std::uint32_t> war_ids;
		for (War const* war : war_manager.get_wars_of(country)) {
			war_ids.push_back(war->id);
		}
		for (const std::uint32_t war_id : war_ids) {
			War const* const war = war_manager.get_war_by_id(war_id);
			if (war != nullptr && war_manager.get_separate_peace_block_reason(instance_manager, *war, country).empty()) {
				war_manager.separate_peace(instance_manager, war_id, country);
			}
		}
	}

	// At peace an army over the manpower (docs/RECRUITMENT.md) fights weaker: the weakest regiments are disbanded down to the maximum
	void disband_over_manpower(InstanceManager& instance_manager, CountryInstance& country) {
		if (UnitInstanceGroup::war_defines == nullptr || UnitInstanceGroup::war_defines->ai_disband_over_manpower == 0) {
			return;
		}
		UnitInstanceManager& unit_manager = instance_manager.get_unit_instance_manager();
		const Manpower manpower = unit_manager.get_manpower(country);
		std::int64_t over = manpower.army_men - manpower.max_men;
		if (over <= 0) {
			return;
		}
		std::vector<std::pair<RegimentInstance*, ArmyInstance*>> regiments;
		for (ArmyInstance& army : country.get_armies()) {
			if (army.is_in_combat()) {
				continue;
			}
			for (RegimentInstance& regiment : army.get_regiment_instances()) {
				// The mobilised regiments are out of the manpower of the soldiers (they go home with the demobilisation)
				if (!regiment.is_mobilised()) {
					regiments.emplace_back(&regiment, &army);
				}
			}
		}
		std::stable_sort(regiments.begin(), regiments.end(), [](auto const& a, auto const& b) -> bool {
			return a.first->get_strength() != b.first->get_strength() ? a.first->get_strength() < b.first->get_strength()
				: a.first->unique_id < b.first->unique_id;
		});
		for (auto const& [regiment, army] : regiments) {
			if (over <= 0) {
				break;
			}
			over -= (regiment->get_strength() * UnitInstanceGroup::war_defines->template_men_per_strength).floor<std::int64_t>();
			unit_manager.destroy_unit(*regiment, *army);
		}
	}

	// At war the AI mobilises when its regiments (not mobilised) are fewer than AI_MOBILISE_RATIO x the regiments of its enemies; the peace demobilises
	// it by itself (UnitInstanceManager::update_mobilisation, DEMOBILISE_DAYS after the last war)
	void mobilise_if_weaker(InstanceManager& instance_manager, CountryInstance& country) {
		OpenVicWarDefines const* const wd = UnitInstanceGroup::war_defines;
		if (wd == nullptr || wd->ai_mobilise == 0 || country.is_mobilised() || !country.is_at_war()) {
			return;
		}
		UnitInstanceManager& unit_manager = instance_manager.get_unit_instance_manager();
		if (!unit_manager.get_mobilise_block_reason(country).empty()) {
			return;
		}
		std::int64_t enemy_regiments = 0;
		for (CountryInstance const& enemy : country.get_war_enemies()) {
			enemy_regiments += static_cast<std::int64_t>(enemy.get_regiment_count());
		}
		const std::int64_t own = static_cast<std::int64_t>(regular_regiment_count(country));
		if (own * Rate::DENOMINATOR < enemy_regiments * wd->ai_mobilise_ratio.numerator && unit_manager.mobilise(country)) {
			SPDLOG_INFO("AI: {} mobilises ({} regiments against {})", country.get_identifier(), own, enemy_regiments);
		}
	}

	// A land neighbour country with its border province next to ours, nullptr if none can be attacked
	void look_for_war(InstanceManager& instance_manager, CountryInstance& country) {
		OpenVicAIDefines const& ai = get_ai_defines(instance_manager);
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
				const std::int32_t limit =
					(owner->is_ai() ? ai.max_target_power_ratio : ai.max_target_power_ratio_player).numerator;
				const fixed_point_t power = owner->military_power.get_untracked();
				if (power * Rate::DENOMINATOR > own_power * limit) {
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
	RgoBuildingData::monthly_update(instance_manager); // the pops of every country, the player's too
	OpenVicAIDefines const& ai = get_ai_defines(instance_manager);
	const Date today = instance_manager.get_today();

	for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		if (!country.exists() || country.is_rebel_country() || !country.is_ai() || country.get_owned_provinces().empty()) {
			continue;
		}

		balance_budget(instance_manager, country);
		build_factory(instance_manager, country);
		choose_ai_production_types(instance_manager, country);
		colonise(instance_manager, country);

		if (country.is_at_war()) {
			war_allies(instance_manager, country);
		}
		if (country.is_at_war()) {
			mobilise_if_weaker(instance_manager, country);
			recruit(instance_manager, country);
		} else {
			disband_over_manpower(instance_manager, country);
		}
		if (!country.is_at_war() && ai.war_chance_per_year.numerator > 0 && country.get_total_rank() > 0
			&& static_cast<int>(country.get_total_rank()) <= ai.max_rank_for_wars
			// no wars in the first half year of the game
			&& today.get_year() * 12 + today.get_month() > ai.war_free_until_year * 12 + ai.war_free_until_month
			// the chance of a month: the yearly chance spread over 12 months, the dice has 100 sides
			&& dice(country, today, static_cast<std::uint64_t>(ai.war_decision_salt)) * 12 * (Rate::DENOMINATOR / 100)
				< ai.war_chance_per_year.numerator) {
			look_for_war(instance_manager, country);
		}
	}
}

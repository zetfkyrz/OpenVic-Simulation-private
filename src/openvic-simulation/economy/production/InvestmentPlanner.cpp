#include "InvestmentPlanner.hpp"

#include <algorithm>
#include <cstdint>
#include <string_view>

#include "openvic-simulation/DefinitionManager.hpp"
#include "openvic-simulation/InstanceManager.hpp"
#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/defines/OpenVicDefines.hpp"
#include "openvic-simulation/economy/BuildingInstance.hpp"
#include "openvic-simulation/economy/BuildingType.hpp"
#include "openvic-simulation/economy/GoodDefinition.hpp"
#include "openvic-simulation/economy/GoodInstance.hpp"
#include "openvic-simulation/economy/production/FactoryInstance.hpp"
#include "openvic-simulation/economy/production/ProductionType.hpp"
#include "openvic-simulation/economy/trading/MarketInstance.hpp"
#include "openvic-simulation/map/MapInstance.hpp"
#include "openvic-simulation/map/ProvinceInstance.hpp"
#include "openvic-simulation/map/State.hpp"
#include "openvic-simulation/modifier/ModifierEffectCache.hpp"
#include "openvic-simulation/modifier/ModifierManager.hpp"
#include "openvic-simulation/population/Pop.hpp"
#include "openvic-simulation/population/PopManager.hpp"
#include "openvic-simulation/population/PopType.hpp"

using namespace OpenVic;

namespace {
	// A deterministic "dice": the same date and place always give the same number
	std::uint64_t mix(std::uint64_t z) {
		z += 0x9E3779B97F4A7C15ULL;
		z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
		z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
		return z ^ (z >> 31);
	}

	// The chance (weight) of a pop type to be given the money of the fund, 0 for the other types
	std::int32_t builder_weight(OpenVicEconomyDefines const& defines, std::string_view identifier) {
		if (identifier == "capitalists") {
			return defines.investment_weight_capitalists;
		} else if (identifier == "aristocrats") {
			return defines.investment_weight_aristocrats;
		} else if (identifier == "artisans") {
			return defines.investment_weight_artisans;
		} else if (identifier == "clergymen") {
			return defines.investment_weight_clergymen;
		} else if (identifier == "clerks") {
			return defines.investment_weight_clerks;
		} else if (identifier == "officers") {
			return defines.investment_weight_officers;
		} else if (identifier == "bureaucrats") {
			return defines.investment_weight_bureaucrats;
		}
		return 0;
	}

	std::int32_t pops_of_type(State const& state, const pop_type_index_t type_index) {
		std::int32_t total = 0;
		for (Pop const& pop : state.get_pops_cache_by_type()[type_index]) {
			total += type_safe::get(pop.get_size());
		}
		return total;
	}

	// Chooses by chance which pops build the project: only the types that live in the state take part
	PopType const* choose_builder(
		PopManager const& pop_manager, OpenVicEconomyDefines const& defines, State const& state, const std::uint64_t seed
	) {
		std::int64_t total_weight = 0;
		for (PopType const& pop_type : pop_manager.get_pop_types()) {
			const std::int32_t weight = builder_weight(defines, pop_type.get_identifier());
			if (weight > 0 && pops_of_type(state, pop_type.index) > 0) {
				total_weight += weight;
			}
		}
		if (total_weight <= 0) {
			return nullptr;
		}

		std::int64_t roll = static_cast<std::int64_t>(mix(seed) % static_cast<std::uint64_t>(total_weight));
		for (PopType const& pop_type : pop_manager.get_pop_types()) {
			const std::int32_t weight = builder_weight(defines, pop_type.get_identifier());
			if (weight <= 0 || pops_of_type(state, pop_type.index) <= 0) {
				continue;
			}
			if (roll < weight) {
				return &pop_type;
			}
			roll -= weight;
		}
		return nullptr;
	}

	// The part of the jobs of one level of the factory that the unemployed of the state can fill, 0..1
	fixed_point_t estimate_staffing(State const& state, ProductionType const& production_type) {
		const std::int32_t workforce = static_cast<std::int32_t>(type_safe::get(production_type.base_workforce_size));
		std::int32_t jobs = 0;
		std::int32_t fillable = 0;
		for (Job const& job : production_type.get_jobs()) {
			const std::int32_t max_for_job = (job.amount * workforce).floor<std::int32_t>();
			if (max_for_job <= 0) {
				continue;
			}
			std::int32_t available = 0;
			for (Pop const& pop : state.get_pops_cache_by_type()[job.pop_type_index]) {
				available += type_safe::get(pop.get_unemployed());
			}
			jobs += max_for_job;
			fillable += std::min(max_for_job, available);
		}
		return jobs > 0 ? fp::from_fraction(fillable, jobs) : fixed_point_t::_0;
	}

	// The goods the country lacks, the first priority of the planner: the country needs the good, makes less of it than
	// it needs and cannot buy it, because the market does not have enough of it (or nobody has it yet)
	memory::vector<char> find_deficit_goods(
		CountryInstance const& country, MarketInstance const& market, OpenVicEconomyDefines const& defines,
		const std::size_t good_count
	) {
		memory::vector<char> deficit;
		deficit.resize(good_count, 0);

		for (auto const& [good_instance, good_data] : country.get_goods_data()) {
			const good_index_t index = good_instance.index;
			if (!market.get_is_available(index)) {
				continue;
			}

			const fixed_point_t needed =
				good_data.pop_demand + good_data.factory_demand + good_data.army_needs + good_data.navy_needs;
			if (needed <= fixed_point_t::_0) {
				continue;
			}

			fixed_point_t produced = 0;
			for (auto const& [production_type, quantity] : good_data.production_per_production_type) {
				produced += quantity;
			}
			if (produced >= defines.investment_deficit_coverage.of(needed)) {
				continue;
			}

			// The market cannot supply what is demanded
			const fixed_point_t supply = good_instance.get_total_supply_yesterday();
			if (good_instance.get_total_demand_yesterday() > supply + defines.investment_deficit_margin.of(supply)) {
				deficit[static_cast<std::size_t>(type_safe::get(index))] = 1;
			}
		}
		return deficit;
	}

	struct Candidate {
		State* state = nullptr;
		ProvinceInstance* province = nullptr; // railroad
		BuildingType const* building_type = nullptr;
		FactoryInstance* factory = nullptr; // the factory to expand, nullptr for a new one
		fixed_point_t cost;
		fixed_point_t profit;
		fixed_point_t score;
		// 0: makes a good the country lacks; 1: the rest, the most profitable first
		std::int32_t priority = 1;
		std::uint32_t order = 0;
	};

	// The AI chooses how much of the budget goes to the fund so that the budget does not go into a loss: it invests a part of the
	// average daily surplus, and nothing while the treasury is below a reserve of some days of spending
	void adjust_ai_allocation(CountryInstance& country, OpenVicEconomyDefines const& defines) {
		const fixed_point_t income = country.get_income_yesterday();
		const fixed_point_t share = CountryInstance::get_investment_budget_share();
		fixed_point_t slider = fixed_point_t::_0;

		if (income > fixed_point_t::_0 && share > fixed_point_t::_0) {
			const fixed_point_t surplus = country.get_balance_before_investment_average();
			const fixed_point_t reserve = country.projected_spending.get_untracked() * defines.investment_ai_reserve_days;
			const bool fund_is_full = country.get_investment_fund() >= income * defines.investment_ai_max_fund_days;
			if (surplus > fixed_point_t::_0 && country.get_cash_stockpile().load() >= reserve && !fund_is_full) {
				const fixed_point_t allocation = defines.investment_ai_surplus_share.of(surplus);
				slider = std::clamp(allocation / (share * income), fixed_point_t::_0, fixed_point_t::_1);
				// Whole percents of the slider, rounded down
				slider = fixed_point_t((slider * 100).floor<std::int32_t>()) / 100;
			}
		}
		if (country.get_construction_spending_slider_value().get_value_untracked() != slider) {
			country.set_construction_spending_slider_value(slider);
		}
	}
}

void InvestmentPlanner::tick(InstanceManager& instance_manager) {
	OpenVicEconomyDefines const& defines =
		instance_manager.definition_manager.get_define_manager().get_openvic_defines().economy;
	const std::int64_t period = std::max<std::int32_t>(1, defines.investment_period_days);
	const std::int64_t day = (instance_manager.get_today() - Date {}).to_int();

	for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		if (country.get_states().empty()) {
			continue;
		}
		// The AI watches its budget every day, so the fund never makes it go into a loss
		if (country.is_ai()) {
			adjust_ai_allocation(country, defines);
		}
		// The countries do their review of the projects on different days of the week
		if ((day + static_cast<std::int64_t>(type_safe::get(country.index))) % period != 0) {
			continue;
		}
		if (country.get_investment_fund() > fixed_point_t::_0) {
			review(instance_manager, country);
		}
	}
}

void InvestmentPlanner::review(InstanceManager& instance_manager, CountryInstance& country) {
	DefinitionManager const& definition_manager = instance_manager.definition_manager;
	OpenVicEconomyDefines const& defines = definition_manager.get_define_manager().get_openvic_defines().economy;
	MarketInstance const& market = instance_manager.get_market_instance();
	BuildingTypeManager const& building_type_manager = definition_manager.get_economy_manager().get_building_type_manager();
	const Date today = instance_manager.get_today();
	const std::int64_t day = (today - Date {}).to_int();

	const fixed_point_t fund = country.get_investment_fund();
	const std::int32_t profit_days = defines.investment_profit_days;
	const memory::vector<char> deficit_goods =
		find_deficit_goods(country, market, defines, instance_manager.get_good_instance_manager().get_good_instances().size());

	memory::vector<Candidate> candidates;
	std::uint32_t order = 0;

	// Enterprises: a new factory in a state or one more level of an existing one
	for (State* state : country.get_states()) {
		if (state->is_colonial_state()) {
			continue;
		}
		for (BuildingType const& building_type : building_type_manager.get_building_types()) {
			ProductionType const* const production_type = building_type.production_type;
			if (production_type == nullptr || production_type->template_type != ProductionType::template_type_t::FACTORY
				|| !country.is_building_type_unlocked(building_type)) {
				continue;
			}

			FactoryInstance* const existing = state->find_factory(building_type);
			if (existing != nullptr) {
				if (!existing->can_be_expanded() || !existing->is_open() || existing->get_level() <= building_level_t(0)) {
					continue;
				}
			} else if (!state->has_free_factory_slot()) {
				continue;
			}

			const fixed_point_t cost = FactoryInstance::calculate_construction_cost(building_type, market);
			if (cost <= fixed_point_t::_0 || cost > fund) {
				continue;
			}

			// A factory whose jobs were all filled for months wants to grow: first of all, without the profit test
			if (existing != nullptr && existing->is_expansion_wanted()) {
				Candidate wanted;
				wanted.state = state;
				wanted.building_type = &building_type;
				wanted.factory = existing;
				wanted.cost = cost;
				wanted.score = fixed_point_t::_100;
				wanted.priority = -1;
				wanted.order = order++;
				candidates.push_back(wanted);
				continue;
			}

			// What a level earns per day: the money from selling its goods minus the raw materials it uses
			fixed_point_t per_day =
				production_type->base_output_quantity * market.get_good_instance(production_type->output_good.index).get_price();
			for (auto const& [good, quantity] : production_type->input_goods) {
				per_day -= quantity * market.get_good_instance(good->index).get_price();
			}
			if (per_day <= fixed_point_t::_0) {
				continue;
			}

			const fixed_point_t staffing = estimate_staffing(*state, *production_type);
			if (staffing < defines.investment_min_staffing.fixed()) {
				continue;
			}

			const fixed_point_t profit = per_day * staffing * profit_days;
			if (profit < defines.investment_min_return.of(cost)) {
				continue;
			}
			if (existing == nullptr && !production_type->is_valid_for_factory_in(*state)) {
				continue;
			}

			Candidate candidate;
			candidate.state = state;
			candidate.building_type = &building_type;
			candidate.factory = existing;
			candidate.cost = cost;
			candidate.profit = profit;
			candidate.score = profit / cost;
			candidate.priority = deficit_goods[static_cast<std::size_t>(type_safe::get(production_type->output_good.index))] != 0 ? 0 : 1;
			candidate.order = order++;
			candidates.push_back(candidate);
		}
	}

	// Railroads: a level of the railroad in a province whose resource gathering would earn more with it
	BuildingType const* const railroad = building_type_manager.get_infrastructure_building_type();
	if (railroad != nullptr && railroad->province_building_index.has_value() && country.get_owned_provinces().size() > 0) {
		const province_building_index_t building_index = *railroad->province_building_index;
		ModifierEffectCache const& modifier_effect_cache = definition_manager.get_modifier_manager().get_modifier_effect_cache();
		const fixed_point_t cost = FactoryInstance::calculate_construction_cost(*railroad, market);

		if (cost > fixed_point_t::_0 && cost <= fund) {
			for (ProvinceInstance* const province : country.get_owned_provinces()) {
				BuildingInstance const& building = province->get_buildings()[building_index];
				if (building.get_expansion_state() != BuildingInstance::ExpansionState::CanExpand || province->get_state() == nullptr) {
					continue;
				}
				const fixed_point_t rgo_revenue = province->get_rgo().get_revenue_yesterday();
				if (rgo_revenue <= fixed_point_t::_0) {
					continue;
				}
				const fixed_point_t profit = defines.investment_railroad_bonus.of(rgo_revenue) * profit_days;
				if (profit < defines.investment_min_return.of(cost)) {
					continue;
				}
				if (!railroad->can_be_built_in(modifier_effect_cache, building.get_level() + 1, country, *province)) {
					continue;
				}

				Candidate candidate;
				candidate.state = province->get_state();
				candidate.province = province;
				candidate.building_type = railroad;
				candidate.cost = cost;
				candidate.profit = profit;
				candidate.score = profit / cost;
				candidate.order = order++;
				candidates.push_back(candidate);
			}
		}
	}

	if (candidates.empty()) {
		return;
	}

	// The priorities: what the country lacks comes first, then the most profitable, then the less profitable; the order of the
	// search decides between the equal ones
	std::sort(candidates.begin(), candidates.end(), [](Candidate const& lhs, Candidate const& rhs) -> bool {
		if (lhs.priority != rhs.priority) {
			return lhs.priority < rhs.priority;
		}
		if (lhs.score != rhs.score) {
			return lhs.score > rhs.score;
		}
		return lhs.order < rhs.order;
	});

	PopManager const& pop_manager = definition_manager.get_pop_manager();
	std::int32_t started = 0;
	for (Candidate const& candidate : candidates) {
		if (started >= defines.investment_projects_per_review) {
			break;
		}
		if (candidate.cost > country.get_investment_fund()) {
			continue;
		}

		State& state = *candidate.state;
		const std::uint64_t seed = static_cast<std::uint64_t>(day) * 1000003ULL
			+ static_cast<std::uint64_t>(type_safe::get(country.index)) * 7919ULL
			+ (state.get_capital() != nullptr ? static_cast<std::uint64_t>(type_safe::get(state.get_capital()->index)) : 0ULL) * 104729ULL
			+ static_cast<std::uint64_t>(type_safe::get(candidate.building_type->index)) * 15485863ULL;
		PopType const* const builder = choose_builder(pop_manager, defines, state, seed);

		CountryInstance::InvestmentRecord record;
		record.date = today;
		record.building_type = candidate.building_type;
		record.state = &state;
		record.province = candidate.province;
		record.builder = builder;
		record.cost = candidate.cost;
		record.deficit = candidate.priority == 0;

		if (candidate.province != nullptr) {
			// A railroad: no pops own it, it only makes the province richer
			record.kind = CountryInstance::InvestmentRecord::kind_t::RAILROAD;
			if (!candidate.province->expand_building(
					definition_manager.get_modifier_manager().get_modifier_effect_cache(),
					*candidate.building_type->province_building_index, country
				)) {
				continue;
			}
		} else if (candidate.factory != nullptr) {
			if (builder == nullptr || !candidate.factory->can_be_expanded()) {
				continue;
			}
			// The new level belongs to the pops who built it, so the share of the state of the factory shrinks
			const std::int32_t level = static_cast<std::int32_t>(type_safe::get(candidate.factory->get_level()));
			candidate.factory->set_state_share(candidate.factory->get_state_share() * level / (level + 1));
			candidate.factory->start_construction(today);
			candidate.factory->clear_expansion_wanted();
			record.kind = CountryInstance::InvestmentRecord::kind_t::EXPANDED_FACTORY;
		} else {
			if (builder == nullptr || !state.has_free_factory_slot()) {
				continue;
			}
			FactoryInstance* const factory = state.add_factory(*candidate.building_type, building_level_t(0));
			if (factory == nullptr) {
				continue;
			}
			factory->set_state_share(fixed_point_t::_0);
			factory->set_built_by_state(false);
			factory->set_owner_pop_type_index(builder->index);
			factory->start_construction(today);
			record.kind = CountryInstance::InvestmentRecord::kind_t::NEW_FACTORY;
		}

		country.spend_investment_fund(candidate.cost, std::move(record));
		started++;
	}
}

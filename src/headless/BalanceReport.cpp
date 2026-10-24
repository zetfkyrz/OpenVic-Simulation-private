#include <cstdlib>
#include "BalanceReport.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <tuple>

#include <fmt/format.h>

#include <openvic-simulation/InstanceManager.hpp>
#include <openvic-simulation/country/CountryInstance.hpp>
#include <openvic-simulation/economy/GoodInstance.hpp>
#include <openvic-simulation/economy/production/FactoryInstance.hpp>
#include <openvic-simulation/map/MapInstance.hpp>
#include <openvic-simulation/map/ProvinceInstance.hpp>
#include <openvic-simulation/map/State.hpp>
#include <openvic-simulation/misc/MoneyLedger.hpp>
#include <openvic-simulation/population/Pop.hpp>
#include <openvic-simulation/economy/BuildingType.hpp>
#include <openvic-simulation/economy/production/ArtisanalProducer.hpp>
#include <openvic-simulation/economy/production/ProductionType.hpp>
#include <openvic-simulation/population/PopType.hpp>

using namespace OpenVic;

namespace {
	double to_double(fixed_point_t const value) {
		return static_cast<double>(value);
	}
}

void BalanceReport::update(InstanceManager& instance_manager) {
	auto goods = instance_manager.get_good_instance_manager().get_good_instances();
	if (unchanged_days.size() != goods.size()) {
		unchanged_days.assign(goods.size(), 0);
		last_price.assign(goods.size(), -1);
	}
	for (GoodInstance const& good : goods) {
		const size_t i = static_cast<size_t>(type_safe::get(good.index));
		const long long raw = good.get_price().get_raw_value();
		unchanged_days[i] = raw == last_price[i] ? unchanged_days[i] + 1 : 0;
		last_price[i] = raw;
	}
}

void BalanceReport::print(InstanceManager& instance_manager, const bool with_goods) {
	// ---- population: life needs by strata, the hungry, unemployment
	std::map<std::string, std::pair<double, double>> life_by_strata; // sum(life * size), sum(size)
	std::map<std::string, std::tuple<double, double, double>> by_type; // sum(life * size), sum(size), sum(cash)
	double population = 0.0, hungry = 0.0, unemployed = 0.0, employable = 0.0;
	double life_cost = 0.0, everyday_cost = 0.0, luxury_cost = 0.0, income_total = 0.0, cash_total = 0.0;
	auto const& good_instances = instance_manager.get_good_instance_manager();
	for (ProvinceInstance& province : instance_manager.get_map_instance().get_province_instances()) {
		for (Pop& pop : province.get_mutable_pops()) {
			const double size = static_cast<double>(type_safe::get(pop.get_size()));
			if (size <= 0.0) {
				continue;
			}
			const double life = to_double(pop.get_life_needs_fulfilled());
			auto& entry = life_by_strata[std::string(pop.get_type().strata.get_identifier())];
			entry.first += life * size;
			entry.second += size;
			auto& type_entry = by_type[std::string(pop.get_type().get_identifier())];
			std::get<0>(type_entry) += life * size;
			std::get<1>(type_entry) += size;
			std::get<2>(type_entry) += to_double(pop.get_cash().get_copy_of_value());
			// What the pop asked for today at the prices of today (minus what it took from its own produce)
			for (auto const& [good_index, quantity] : pop.get_life_needs()) {
				life_cost += to_double(quantity) * to_double(good_instances.get_good_instance_by_index(good_index)->get_price());
			}
			for (auto const& [good_index, quantity] : pop.get_everyday_needs()) {
				everyday_cost += to_double(quantity) * to_double(good_instances.get_good_instance_by_index(good_index)->get_price());
			}
			for (auto const& [good_index, quantity] : pop.get_luxury_needs()) {
				luxury_cost += to_double(quantity) * to_double(good_instances.get_good_instance_by_index(good_index)->get_price());
			}
			income_total += to_double(pop.get_income());
			cash_total += to_double(pop.get_cash().get_copy_of_value());
			population += size;
			if (life < 0.5) {
				hungry += size;
			}
			if (pop.get_type().can_be_unemployed) {
				employable += size;
				unemployed += static_cast<double>(type_safe::get(pop.get_unemployed()));
			}
		}
	}
	if (start_day < 0) {
		start_day = (instance_manager.get_today() - Date {}).to_int();
		start_population = population;
	}
	const double years = static_cast<double>((instance_manager.get_today() - Date {}).to_int() - start_day) / 365.0;

	// ---- factories
	size_t factories = 0, open = 0, levels = 0, building = 0;
	for (StateSet const& state_set : instance_manager.get_map_instance().get_state_manager().get_state_sets()) {
		for (State const& state : state_set.get_states()) {
			for (FactoryInstance const& factory : state.get_factories()) {
				factories++;
				open += factory.is_open();
				levels += static_cast<size_t>(type_safe::get(factory.get_level()));
				building += factory.is_under_construction();
			}
		}
	}

	// ---- prices
	auto goods = instance_manager.get_good_instance_manager().get_good_instances();
	double min_ratio = 1e9, max_ratio = 0.0;
	size_t tradeable = 0, stuck = 0;
	for (GoodInstance const& good : goods) {
		if (!good.is_trading_good()) {
			continue;
		}
		tradeable++;
		const double base = to_double(good.good_definition.base_price);
		const double ratio = base > 0 ? to_double(good.get_price()) / base : 1.0;
		min_ratio = std::min(min_ratio, ratio);
		max_ratio = std::max(max_ratio, ratio);
		const size_t i = static_cast<size_t>(type_safe::get(good.index));
		stuck += i < unchanged_days.size() && unchanged_days[i] >= 90;
	}

	// ---- AI countries: how many can pay for a factory
	std::vector<double> treasuries;
	for (CountryInstance const& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		if (country.exists() && country.is_ai() && country.get_total_rank() > 0 && country.get_total_rank() <= 20) {
			treasuries.push_back(to_double(country.get_cash_stockpile().load()));
		}
	}
	std::sort(treasuries.begin(), treasuries.end());
	const double median_treasury = treasuries.empty() ? 0.0 : treasuries[treasuries.size() / 2];

	std::string line = fmt::format(
		"BALANCE {} pop={:.0f} growth_per_year={:+.2f}% hungry={:.1f}% unemployed={:.1f}% life:", instance_manager.get_today(),
		population, years > 0.05 && start_population > 0 ? (population / start_population - 1.0) * 100.0 / years : 0.0,
		population > 0 ? 100.0 * hungry / population : 0.0, employable > 0 ? 100.0 * unemployed / employable : 0.0
	);
	for (auto const& [name, entry] : life_by_strata) {
		line += fmt::format(" {}={:.2f}", name, entry.second > 0 ? entry.first / entry.second : 0.0);
	}
	line += fmt::format(
		" | factories={} open={:.0f}% levels={} building={} | prices min={:.2f}x max={:.2f}x stuck90={}/{} | ai_top20_median_treasury={:.0f}",
		factories, factories > 0 ? 100.0 * static_cast<double>(open) / static_cast<double>(factories) : 100.0, levels, building,
		min_ratio, max_ratio, stuck, tradeable, median_treasury
	);
	line += fmt::format(" | per day: pop_income={:.0f} life_needs_cost={:.0f} everyday_cost={:.0f} luxury_cost={:.0f} pop_cash={:.0f}", income_total, life_cost, everyday_cost, luxury_cost, cash_total);
	fmt::println("{}", line);
	std::string types_line = fmt::format("BALANCE_TYPES {} (life needs, people, cash per 1000 people):", instance_manager.get_today());
	for (auto const& [name, entry] : by_type) {
		const double people = std::get<1>(entry);
		types_line += fmt::format(
			" {}={:.2f}/{:.0f}/{:.2f}", name, people > 0 ? std::get<0>(entry) / people : 0.0, people, people > 0 ? 1000.0 * std::get<2>(entry) / people : 0.0
		);
	}
	fmt::println("{}", types_line);

	if (with_goods) {
		struct ArtisanGroup {
			size_t pops = 0;
			double people = 0, output = 0, revenue = 0, cost = 0;
		};
		std::map<std::string, ArtisanGroup> artisan_groups;
		for (ProvinceInstance& province : instance_manager.get_map_instance().get_province_instances()) {
			for (Pop& pop : province.get_mutable_pops()) {
				ArtisanalProducer const* const artisan = pop.get_artisanal_producer();
				if (artisan == nullptr || artisan->get_production_type_nullable() == nullptr) {
					continue;
				}
				ProductionType const& production_type = *artisan->get_production_type_nullable();
				if (std::getenv("BAL_ART") != nullptr && production_type.get_identifier() == std::getenv("BAL_ART")) {
					fmt::println("ARTPOP {} {} size={} cash={} rev={} cost={} prod={}", province.get_identifier(),
						province.get_country_to_report_economy() != nullptr ? province.get_country_to_report_economy()->get_identifier() : "-",
						type_safe::get(pop.get_size()), to_double(pop.get_cash().get_copy_of_value()), to_double(pop.get_artisanal_revenue()),
						to_double(artisan->get_costs_of_production()), to_double(artisan->get_current_production()));
				}
				ArtisanGroup& group = artisan_groups[std::string(production_type.get_identifier())];
				group.pops++;
				group.people += static_cast<double>(type_safe::get(pop.get_size()));
				group.output += to_double(artisan->get_current_production());
				group.revenue += to_double(artisan->get_current_production())
					* to_double(good_instances.get_good_instance_by_definition(production_type.output_good).get_price());
				group.cost += to_double(artisan->get_costs_of_production());
			}
		}
		for (StateSet const& state_set : instance_manager.get_map_instance().get_state_manager().get_state_sets()) {
			for (State const& state : state_set.get_states()) {
				for (FactoryInstance const& factory : state.get_factories()) {
					fmt::println(
						"FACTORY {} {:<22} {} open={} level={} employment={}/{} output={:.1f}(sold {:.1f}) revenue={:.1f} inputs={:.1f} wages={:.1f} profit={:.1f} eff={:.2f} throttle={:.2f} budget={:.0f} unprofitable_days={}",
						instance_manager.get_today(), factory.building_type.get_identifier(), state.get_identifier(),
						factory.is_open() ? 1 : 0, type_safe::get(factory.get_level()), type_safe::get(factory.get_employee_count()),
						type_safe::get(factory.get_max_employee_count()), to_double(factory.get_output_quantity_yesterday()),
						to_double(factory.get_sold_quantity_yesterday()), to_double(factory.get_revenue_yesterday()),
						to_double(factory.get_input_cost_yesterday()), to_double(factory.get_wages_yesterday()),
						to_double(factory.get_profit_yesterday()), to_double(factory.get_efficiency()), to_double(factory.get_throttle()),
						to_double(factory.get_budget()), factory.get_unprofitable_days()
					);
				}
			}
		}
		for (auto const& [name, group] : artisan_groups) {
			fmt::println(
				"ARTISANS {} {:<24} pops={} people={:.0f} output={:.1f} revenue={:.1f} cost={:.1f}", instance_manager.get_today(), name,
				group.pops, group.people, group.output, group.revenue, group.cost
			);
		}
		for (GoodInstance const& good : goods) {
			if (!good.is_trading_good()) {
				continue;
			}
			const double demand = to_double(good.get_total_demand_yesterday());
			const double traded = to_double(good.get_quantity_traded_yesterday());
			const double base = to_double(good.good_definition.base_price);
			fmt::println(
				"GOODS {} {:<22} price={:.2f} ({:.2f}x) demand={:.1f} supply={:.1f} traded={:.1f} bought={:.0f}% stuck={}",
				instance_manager.get_today(), good.get_identifier(), to_double(good.get_price()),
				base > 0 ? to_double(good.get_price()) / base : 1.0, demand, to_double(good.get_total_supply_yesterday()), traded,
				demand > 0 ? 100.0 * traded / demand : 100.0, unchanged_days[static_cast<size_t>(type_safe::get(good.index))]
			);
		}
	}
}

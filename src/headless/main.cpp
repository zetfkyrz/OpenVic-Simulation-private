#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <map>
#include <memory>
#include <random>
#include <unordered_set>

#include <fmt/base.h>
#include <fmt/chrono.h>

#include <range/v3/view/enumerate.hpp>

#include <spdlog/common.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/callback_sink.h>

#include <openvic-simulation/misc/StateHash.hpp>
#include <openvic-simulation/core/memory/MemoryTracker.hpp>
#include <openvic-simulation/core/memory/Vector.hpp>
#include <openvic-simulation/GameManager.hpp>
#include <openvic-simulation/country/CountryInstance.hpp>
#include <openvic-simulation/InstanceManager.hpp>
#include <openvic-simulation/research/ResearchManager.hpp>
#include <openvic-simulation/research/Technology.hpp>
#include <openvic-simulation/politics/PoliticsDynamics.hpp>
#include <openvic-simulation/politics/PoliticsManager.hpp>
#include <openvic-simulation/country/CountryParty.hpp>
#include <openvic-simulation/military/UnitInstanceGroup.hpp>
#include <openvic-simulation/economy/GoodInstance.hpp>
#include <openvic-simulation/military/Equipment.hpp>
#include <openvic-simulation/military/BattleFormulas.hpp>
#include <openvic-simulation/military/DivisionStats.hpp>
#include <openvic-simulation/military/UnitModifiers.hpp>
#include <openvic-simulation/modifier/ModifierEffectCache.hpp>
#include <openvic-simulation/defines/OpenVicDefines.hpp>
#include <openvic-simulation/map/ProvinceInstance.hpp>
#include <openvic-simulation/population/PopType.hpp>
#include <openvic-simulation/misc/GameAction.hpp>
#include <openvic-simulation/misc/Replay.hpp>
#include <openvic-simulation/economy/production/FactoryInstance.hpp>
#include <openvic-simulation/map/State.hpp>
#include <openvic-simulation/population/Pop.hpp>
#include <openvic-simulation/dataloader/Dataloader.hpp>
#include <openvic-simulation/economy/GoodDefinition.hpp>
#include <openvic-simulation/economy/production/ProductionType.hpp>
#include <openvic-simulation/economy/production/ResourceGatheringOperation.hpp>
#include <openvic-simulation/pathfinding/AStarPathing.hpp>
#include <openvic-simulation/testing/Testing.hpp>
#include <openvic-simulation/utility/Logger.hpp>
#include <openvic-simulation/utility/ThreadPool.hpp>

#include "BalanceReport.hpp"
#include "SelfTest.hpp"

using namespace OpenVic;

inline static void print_memory_usage( //
	std::string_view prefix, std::source_location const& location = std::source_location::current()
) {
#ifdef DEBUG_ENABLED // memory tracking will return 0 without DEBUG_ENABLED
#if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_INFO
	spdlog::log(
#ifndef SPDLOG_NO_SOURCE_LOC
		spdlog::source_loc { location.file_name(), static_cast<int>(location.line()), location.function_name() },
#else
		spdlog::source_loc {},
#endif
		spdlog::level::info, "{} Memory Usage: {} Bytes", prefix, OpenVic::memory::MemoryTracker::get_memory_usage()
	);
#endif
#endif
}

// Days to simulate for the economy report (-e), 0 = no report
static size_t economy_report_days = 0;
static bool profile_ticks = false;
static bool run_army_test = false;
static bool tariff_test = false;
static bool recommended_rules = false;
static bool politics_report = false;
static bool research_test = false;
// Replay save test: -W <file> writes the action history after the economy run, -L <file> replays it before the run
static std::string replay_save_path, replay_load_path;
// -D <file>: write an example common/openvic_defines.lua with all the keys and their current values
static std::string defines_dump_path;
// -M: follow the money of the economy (MoneyLedger), -N: close all the factories before the economy report
static bool money_ledger_report = false, close_all_factories = false;
// --selftest: the self tests (SelfTest.hpp), --selftest-pure: only those that need no game files
static bool run_selftest = false, run_selftest_pure = false;
// -C: print the CHECKSUM after every day, also of the first ticks before the economy report (to find the first day two runs differ)
static bool checksum_every_day = false;
// -o group.KEY=value (repeated): overrides a value of openvic_defines.lua for this run (for calibration experiments)
static std::vector<std::string> defines_overrides;
// -B: the BALANCE report (every 30 days), -G: also the line of every good
static bool balance_report = false, balance_goods = false;
// Index of the bookmark (start date) to simulate, 0 = 1836, 1 = 1861 (-k)
static size_t bookmark_index = 0;
// Subsidise all factories of all countries at the start of the economy report (-S)
static bool subsidise_all = false;

// Summary of the factories and of the money in the economy, printed with the "ECON" prefix
static void print_economy_report(InstanceManager& instance_manager) {
	size_t factories = 0, open_factories = 0, building = 0, levels = 0;
	fixed_point_t budgets = 0, revenue = 0, wages = 0, owner_income = 0, profit = 0;
	double workforce_fraction_sum = 0;
	size_t staffed_factories = 0, over_threshold = 0;
	fixed_point_t expansion_funds = 0, total_cost = 0;

	for (StateSet const& state_set : instance_manager.get_map_instance().get_state_manager().get_state_sets()) {
		for (State const& state : state_set.get_states()) {
			for (FactoryInstance const& factory : state.get_factories()) {
				factories++;
				levels += static_cast<size_t>(type_safe::get(factory.get_level()));
				open_factories += factory.is_open();
				building += factory.is_under_construction();
				budgets += factory.get_budget();
				expansion_funds += factory.get_expansion_fund();
				total_cost += FactoryInstance::calculate_construction_cost(factory.building_type, instance_manager.get_market_instance());
				revenue += factory.get_revenue_yesterday();
				wages += factory.get_wages_yesterday();
				owner_income += factory.get_owner_income_yesterday();
				profit += factory.get_profit_yesterday();
				if (type_safe::get(factory.get_max_employee_count()) > 0) {
					staffed_factories++;
					over_threshold += type_safe::get(factory.get_employee_count()) * 100 >= type_safe::get(factory.get_max_employee_count()) * 80;
					workforce_fraction_sum += static_cast<double>(type_safe::get(factory.get_employee_count())) /
						static_cast<double>(type_safe::get(factory.get_max_employee_count()));
				}
			}
		}
	}

	size_t external_loans = 0, internal_loans = 0;
	fixed_point_t external_debt = 0, internal_debt = 0, internal_unspent = 0, capitalist_funds = 0;
	for (auto const& loan : instance_manager.get_credit_manager().get_loans()) {
		if (loan->status != Loan::status_t::ACTIVE) {
			continue;
		}
		if (loan->kind == Loan::kind_t::EXTERNAL) {
			external_loans++;
			external_debt += loan->get_remaining();
		} else {
			internal_loans++;
			internal_debt += loan->get_remaining();
			internal_unspent += loan->unspent;
		}
	}
	for (StateSet const& state_set : instance_manager.get_map_instance().get_state_manager().get_state_sets()) {
		for (State const& state : state_set.get_states()) {
			capitalist_funds += state.get_capitalist_fund();
		}
	}

	fixed_point_t country_cash = 0, gold_income = 0;
	for (CountryInstance const& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		country_cash += country.get_cash_stockpile().load();
		gold_income += country.get_gold_income_untracked();
	}
	fixed_point_t pop_cash = 0;
	for (ProvinceInstance const& province : instance_manager.get_map_instance().get_province_instances()) {
		for (Pop const& pop : province.get_pops()) {
			pop_cash += pop.get_cash().get_copy_of_value();
		}
	}

	fmt::println(
		"CREDIT loans ext={} int={} | debt ext={:.0f} int={:.0f} | unspent={:.0f} capitalist_savings={:.0f} | building={}",
		external_loans, internal_loans, static_cast<double>(external_debt), static_cast<double>(internal_debt),
		static_cast<double>(internal_unspent), static_cast<double>(capitalist_funds), building
	);
	fmt::println(
		"ECON {} factories={} open={} building={} levels={} employment={:.2f} revenue={:.1f} wages={:.1f} owners={:.1f} "
		"profit={:.1f} over80%={} funds={:.0f} avg_cost={:.0f} | money: countries={:.0f} pops={:.0f} factories={:.0f} total={:.0f}",
		instance_manager.get_today(), factories, open_factories, building, levels,
		staffed_factories > 0 ? workforce_fraction_sum / staffed_factories : 0.0, static_cast<double>(revenue),
		static_cast<double>(wages), static_cast<double>(owner_income), static_cast<double>(profit),
		over_threshold, static_cast<double>(expansion_funds), factories > 0 ? static_cast<double>(total_cost) / factories : 0.0,
		static_cast<double>(country_cash), static_cast<double>(pop_cash), static_cast<double>(budgets),
		static_cast<double>(country_cash + pop_cash + budgets)
	);

	if (CountryInstance const* const eng = instance_manager.get_country_instance_manager().get_country_instance_by_identifier("ENG")) {
		fixed_point_t needs_value = 0, spending_value = 0;
		for (auto const& [good_instance, good_data] : eng->get_goods_data()) {
			needs_value += (good_data.army_needs + good_data.navy_needs) * good_instance.get_price();
		}
		fmt::println(
			"ARMYCOST {} ENG army supply/day={:.1f} navy={:.1f} | needs of the day at prices={:.1f} | actual stockpile spending={:.1f} regiments={} | level army={:.2f} navy={:.2f}",
			instance_manager.get_today(), static_cast<double>(eng->calculate_army_supply_cost_per_day()),
			static_cast<double>(eng->calculate_navy_supply_cost_per_day()), static_cast<double>(needs_value),
			static_cast<double>(eng->get_actual_national_stockpile_spending().load()), eng->get_regiment_count(),
			static_cast<double>(eng->get_army_supply_level()), static_cast<double>(eng->get_navy_supply_level())
		);
	}
	{
		// The investment funds: how much money sits in them and where
		fixed_point_t total_funds = 0;
		std::vector<std::pair<double, CountryInstance const*>> funds;
		for (CountryInstance const& country : instance_manager.get_country_instance_manager().get_country_instances()) {
			total_funds += country.get_investment_fund();
			funds.push_back({ static_cast<double>(country.get_investment_fund()), &country });
		}
		std::sort(funds.begin(), funds.end(), [](auto const& lhs, auto const& rhs) { return lhs.first > rhs.first; });
		fmt::print("FUNDS {} total={:.0f} | top:", instance_manager.get_today(), static_cast<double>(total_funds));
		for (size_t i = 0; i < funds.size() && i < 6; ++i) {
			fmt::print(
				" {} fund={:.0f} income/day={:.0f} cash={:.0f} slider={:.2f} ai={}", funds[i].second->get_identifier(), funds[i].first,
				static_cast<double>(funds[i].second->get_income_yesterday()), static_cast<double>(funds[i].second->get_cash_stockpile().load()),
				static_cast<double>(funds[i].second->get_construction_spending_slider_value().get_value_untracked()), funds[i].second->is_ai()
			);
		}
		fmt::println("");
	}

	// The world trade of yesterday (docs/TRADE.md)
	TradeNetwork const& trade_network = instance_manager.get_trade_network();
	if (trade_network.is_ready()) {
		fixed_point_t exports = 0, land = 0, sea = 0, internal_fees = 0, export_fees = 0, unmet = 0;
		size_t routes = 0, land_routes = 0, sea_routes = 0, mixed_routes = 0;
		double route_cost_sum = 0;
		const size_t country_count = trade_network.get_node_count() - 1;
		for (size_t from = 0; from < country_count; ++from) {
			TradeNetwork::country_stats_t const& stats = trade_network.get_stats_yesterday(country_index_t(from));
			exports += stats.export_value;
			land += stats.land_export_value;
			sea += stats.sea_export_value;
			internal_fees += stats.internal_fees;
			export_fees += stats.export_fees;
			unmet += stats.unmet_import_value;
			for (size_t to = 0; to < country_count; ++to) {
				TradeNetwork::route_t const& route = trade_network.get_route(country_index_t(from), country_index_t(to));
				if (route.mode == TradeNetwork::mode_t::NONE) {
					continue;
				}
				routes++;
				route_cost_sum += static_cast<double>(route.cost);
				land_routes += route.mode == TradeNetwork::mode_t::LAND;
				sea_routes += route.mode == TradeNetwork::mode_t::SEA;
				mixed_routes += route.mode == TradeNetwork::mode_t::MIXED;
			}
		}
		fmt::println(
			"TRADE {} exports/day={:.0f} (land {:.0f} sea {:.0f}) fees/day: internal={:.0f} export={:.0f} cut off by the routes/day={:.0f} | routes={} (land {} sea {} mixed {}) average cost={:.3f}",
			instance_manager.get_today(), static_cast<double>(exports), static_cast<double>(land), static_cast<double>(sea),
			static_cast<double>(internal_fees), static_cast<double>(export_fees), static_cast<double>(unmet),
			routes, land_routes, sea_routes, mixed_routes, routes > 0 ? route_cost_sum / routes : 0.0
		);
		// The flows between the countries (cargo value of yesterday): how big they are, to set the capacities of the routes
		// The goods that were on offer in one country and wanted in another, but the routes had no room (kept at the outside node)
		unmet += trade_network.get_stats_yesterday(trade_network.get_outside_node()).unmet_import_value;
		std::vector<std::pair<double, std::pair<size_t, size_t>>> flows;
		for (size_t from = 0; from < country_count; ++from) {
			for (size_t to = 0; to < country_count; ++to) {
				const double value = static_cast<double>(trade_network.get_pair_flow_yesterday(country_index_t(from), country_index_t(to)));
				if (value > 0) {
					flows.push_back({ value, { from, to } });
				}
			}
		}
		std::sort(flows.begin(), flows.end(), [](auto const& lhs, auto const& rhs) { return lhs.first > rhs.first; });
		if (!flows.empty()) {
			const auto quantile = [&flows](const double share) -> double {
				return flows[static_cast<size_t>(static_cast<double>(flows.size() - 1) * (1.0 - share))].first;
			};
			fmt::print(
				"TRADEFLOWS pairs={} median={:.1f} p90={:.1f} p99={:.1f} max={:.1f} | top:",
				flows.size(), quantile(0.5), quantile(0.9), quantile(0.99), flows.front().first
			);
			for (size_t i = 0; i < flows.size() && i < 8; ++i) {
				fmt::print(
					" {}>{} {:.0f}",
					instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index_t(flows[i].second.first)).country_definition.get_identifier(),
					instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index_t(flows[i].second.second)).country_definition.get_identifier(),
					flows[i].first
				);
			}
			fmt::println("");
		}
	}
}

// Exercises the army actions (split, merge, dig in, recruitment, disband) and prints the results with the "ARMY" prefix
static bool test_armies(InstanceManager& instance_manager, DefinitionManager const& definition_manager) {
	UnitInstanceManager& unit_manager = instance_manager.get_unit_instance_manager();
	bool ok = true;
	const auto check = [&ok](bool condition, std::string_view what) -> void {
		fmt::println("ARMY {} : {}", condition ? "ok  " : "FAIL", what);
		ok &= condition;
	};

	ArmyInstance* army = nullptr;
	for (ArmyInstance const& candidate : unit_manager.get_armies()) {
		if (candidate.get_unit_count() >= 3 && !candidate.get_country().is_rebel_country()) {
			army = static_cast<ArmyInstance*>(unit_manager.get_unit_instance_group_by_unique_id(candidate.unique_id));
			break;
		}
	}
	if (army == nullptr) {
		fmt::println("ARMY FAIL : no army with 3 regiments");
		return false;
	}
	CountryInstance& country = army->get_country();
	const size_t count_before = army->get_unit_count();
	fmt::println("ARMY using {} army \"{}\" with {} regiments", country.get_identifier(), army->get_name(), count_before);

	// Split and merge
	RegimentInstance& regiment = static_cast<RegimentInstance&>(army->get_units().back().get());
	ArmyInstance* const split = unit_manager.split_regiment(*army, regiment);
	check(split != nullptr && split->get_unit_count() == 1 && army->get_unit_count() == count_before - 1, "split_regiment");
	check(split != nullptr && &split->get_location() == &army->get_location(), "split army is in the same province");
	if (split != nullptr) {
		check(unit_manager.merge_armies(*army, *split) && army->get_unit_count() == count_before, "merge_armies");
	}

	// Dig in grows while the army stays put
	army->clear_path();
	for (int day = 0; day < 40; ++day) {
		instance_manager.force_tick_and_update();
	}
	fmt::println("ARMY dig_in_level after 40 days: {} (cap {})", static_cast<int>(army->get_dig_in_level()), country.get_dig_in_cap());
	check(army->get_dig_in_level() > 0 || country.get_dig_in_cap() == 0, "dig in grows");

	// Recruitment
	RegimentType const* regiment_type = nullptr;
	for (RegimentType const& type : definition_manager.get_military_manager().get_unit_type_manager().get_regiment_types()) {
		if (country.is_unit_type_unlocked(type)) {
			regiment_type = &type;
			break;
		}
	}
	Pop* recruit_pop = nullptr;
	ProvinceInstance* recruit_province = nullptr;
	if (regiment_type != nullptr) {
		for (ProvinceInstance& province : instance_manager.get_map_instance().get_province_instances()) {
			if (province.get_owner() != &country || province.get_controller() != &country) {
				continue;
			}
			for (Pop& pop : province.get_mutable_pops()) {
				if (pop.get_type().can_be_recruited && pop.get_regiment_count() < pop.get_max_supported_regiments() &&
					Pop::is_culture_status_allowed(country.get_allowed_regiment_cultures(), pop.get_culture_status())) {
					recruit_pop = &pop;
					recruit_province = &province;
					break;
				}
			}
			if (recruit_pop != nullptr) {
				break;
			}
		}
	}
	check(regiment_type != nullptr && recruit_pop != nullptr, "found a pop that can recruit");
	if (regiment_type != nullptr && recruit_pop != nullptr) {
		// Without goods the order is refused
		for (auto const& [good, quantity] : regiment_type->get_build_cost()) {
			country.get_good_data(*good).stockpile_amount = 0;
		}
		if (!regiment_type->get_build_cost().empty()) {
			check(!unit_manager.start_recruitment(country, *regiment_type, *recruit_province, *recruit_pop), "recruitment refused without goods");
		}
		for (auto const& [good, quantity] : regiment_type->get_build_cost()) {
			country.get_good_data(*good).stockpile_amount = quantity * 2;
		}
		const size_t pop_regiments = recruit_pop->get_regiment_count();
		const size_t orders_before = unit_manager.get_recruitment_orders().size();
		check(unit_manager.start_recruitment(country, *regiment_type, *recruit_province, *recruit_pop), "start_recruitment");
		check(unit_manager.get_recruitment_orders().size() == orders_before + 1, "order queued");
		check(recruit_pop->get_regiment_count() == pop_regiments + 1, "pop slot taken");

		// Cancel returns the goods and the slot
		const unique_id_t order_id = unit_manager.get_recruitment_orders().back().id;
		check(unit_manager.cancel_recruitment(country, order_id) && recruit_pop->get_regiment_count() == pop_regiments, "cancel_recruitment");
		for (auto const& [good, quantity] : regiment_type->get_build_cost()) {
			check(country.get_good_data(*good).stockpile_amount == quantity * 2, "goods refunded");
			break;
		}

		check(unit_manager.start_recruitment(country, *regiment_type, *recruit_province, *recruit_pop), "start_recruitment again");
		const int32_t days = UnitInstanceManager::get_recruitment_days(country, *regiment_type);
		size_t regiments_in_province_before = 0;
		for (ArmyInstance const& a : recruit_province->get_armies()) {
			if (&a.get_country() == &country) {
				regiments_in_province_before += a.get_unit_count();
			}
		}
		for (int32_t day = 0; day <= days; ++day) {
			instance_manager.force_tick_and_update();
		}
		size_t regiments_in_province_after = 0;
		for (ArmyInstance const& a : recruit_province->get_armies()) {
			if (&a.get_country() == &country) {
				regiments_in_province_after += a.get_unit_count();
			}
		}
		fmt::println("ARMY recruit days={} regiments in province {} -> {}", days, regiments_in_province_before, regiments_in_province_after);
		check(regiments_in_province_after >= regiments_in_province_before + 1, "regiment appeared in the province");

		// Disband the new regiment: the pop slot is free again
		for (ArmyInstance& a : recruit_province->get_armies()) {
			if (&a.get_country() == &country) {
				for (UnitInstance& unit : a.get_units()) {
					RegimentInstance& r = static_cast<RegimentInstance&>(unit);
					if (r.get_pop() == recruit_pop) {
						unit_manager.disband_regiment(a, r);
						check(recruit_pop->get_regiment_count() == pop_regiments, "disband frees the pop slot");
						return ok;
					}
				}
			}
		}
		check(false, "recruited regiment found for disband");
	}
	return ok;
}

// A battle between two armies and the siege of a province after it, printed with the "BATTLE" prefix
static bool test_battle(InstanceManager& instance_manager) {
	UnitInstanceManager& unit_manager = instance_manager.get_unit_instance_manager();
	bool ok = true;
	const auto check = [&ok](bool condition, std::string_view what) -> void {
		fmt::println("BATTLE {} : {}", condition ? "ok  " : "FAIL", what);
		ok &= condition;
	};

	ArmyInstance* army_a = nullptr;
	ArmyInstance* army_b = nullptr;
	for (ArmyInstance const& candidate : unit_manager.get_armies()) {
		if (candidate.get_country().is_rebel_country() || candidate.get_unit_count() < 2) {
			continue;
		}
		ArmyInstance* mutable_army = static_cast<ArmyInstance*>(unit_manager.get_unit_instance_group_by_unique_id(candidate.unique_id));
		if (army_a == nullptr) {
			army_a = mutable_army;
		} else if (&mutable_army->get_country() != &army_a->get_country()
			&& !mutable_army->get_country().has_alliance_with(army_a->get_country())) {
			army_b = mutable_army;
			break;
		}
	}
	if (army_a == nullptr || army_b == nullptr) {
		fmt::println("BATTLE FAIL : no two armies of different countries");
		return false;
	}

	CountryInstance& country_a = army_a->get_country();
	CountryInstance& country_b = army_b->get_country();
	check(instance_manager.get_war_manager().declare_war(instance_manager, country_a, country_b), "declare war");
	fmt::println("BATTLE {} (leader: {}) vs {} (leader: {})", country_a.get_identifier(),
		army_a->get_leader() != nullptr ? army_a->get_leader()->get_name() : "none",
		country_b.get_identifier(), army_b->get_leader() != nullptr ? army_b->get_leader()->get_name() : "none");

	// B attacks the province of A
	army_a->clear_path();
	army_b->clear_path();
	army_b->set_location(army_a->get_location());
	ProvinceInstance& province = army_a->get_location();
	const unique_id_t id_a = army_a->unique_id;
	const unique_id_t id_b = army_b->unique_id;
	fmt::println("BATTLE regiments A={} B={} in province {}", army_a->get_unit_count(), army_b->get_unit_count(), province.get_identifier());

	int day = 0;
	for (; day < 90; ++day) {
		instance_manager.force_tick_and_update();
		UnitInstanceGroup* const a = unit_manager.get_unit_instance_group_by_unique_id(id_a);
		UnitInstanceGroup* const b = unit_manager.get_unit_instance_group_by_unique_id(id_b);
		if (day % 5 == 0 || a == nullptr || b == nullptr) {
			fmt::println("BATTLE day {:2}: A {} org {:.2f} str {:.2f} | B {} org {:.2f} str {:.2f}", day + 1,
				a != nullptr ? a->get_unit_count() : 0, a != nullptr ? static_cast<double>(a->get_organisation_proportion()) : 0.0,
				a != nullptr ? static_cast<double>(a->get_strength_proportion()) : 0.0,
				b != nullptr ? b->get_unit_count() : 0, b != nullptr ? static_cast<double>(b->get_organisation_proportion()) : 0.0,
				b != nullptr ? static_cast<double>(b->get_strength_proportion()) : 0.0);
		}
		const bool a_here = a != nullptr && &a->get_location() == &province;
		const bool b_here = b != nullptr && &b->get_location() == &province;
		if (!(a_here && b_here)) {
			fmt::println("BATTLE ended after {} days: A {} B {}", day + 1, a == nullptr ? "destroyed" : (a_here ? "holds" : "retreated"),
				b == nullptr ? "destroyed" : (b_here ? "holds" : "retreated"));
			break;
		}
	}
	check(day < 90, "battle is decided within 90 days");

	// Peace: the winner occupies a state of the loser and demands it
	{
		WarManager& war_manager = instance_manager.get_war_manager();
		War const* war = nullptr;
		for (War const* candidate : war_manager.get_wars_of(country_a)) {
			if (candidate->has_participant(country_b)) {
				war = candidate;
				break;
			}
		}
		check(war != nullptr, "the war exists");
		if (war != nullptr) {
			ProvinceInstance* taken = nullptr;
			for (ProvinceInstance* candidate : country_b.get_owned_provinces()) {
				if (candidate->get_state() != nullptr) {
					taken = candidate;
					break;
				}
			}
			check(taken != nullptr, "the loser owns a state");
			if (taken != nullptr) {
				for (ProvinceInstance& province : taken->get_state()->get_provinces()) {
					province.set_controller(&country_a);
				}
				instance_manager.force_tick_and_update();
				const bool a_attacker = war->is_attacker(country_a);
				const auto candidates = war_manager.get_peace_candidates(*war, country_a, false);
				fmt::println("PEACE war #{} score A {:.1f} B {:.1f}, candidates: {}", war->id,
					static_cast<double>(war->get_score(a_attacker)), static_cast<double>(war->get_score(!a_attacker)), candidates.size());
				check(!candidates.empty(), "the occupied state is a candidate");
				PeaceTerms terms;
				terms.war_id = war->id;
				if (!candidates.empty()) {
					terms.states.push_back(candidates.front());
				}
				const fixed_point_t cost = war_manager.get_terms_cost(*war, country_a, terms);
				memory::string reason;
				const bool accepts = war_manager.evaluate_offer(instance_manager, *war, a_attacker, terms, &reason);
				fmt::println("PEACE terms cost {:.1f}, available {:.1f}, the other side {} ({})", static_cast<double>(cost),
					static_cast<double>(war->get_score(a_attacker)), accepts ? "accepts" : "refuses", reason);
				const bool signed_peace = war_manager.send_peace_offer(instance_manager, country_a, terms);
				auto const* response = war_manager.get_last_response(country_a);
				fmt::println("PEACE sent: {} - {}", signed_peace ? "signed" : "not signed", response != nullptr ? response->text : "");
				check(response != nullptr && !response->text.empty(), "the answer has a text");
				if (signed_peace) {
					check(taken->get_owner() == &country_a || taken->get_owner() != &country_b, "the state changed hands");
					check(!country_a.is_at_war_with(country_b), "the war is over");
				}
			}
		}
	}
	return ok;
}

// Fleets: movement between a port and the sea, a battle at sea, carrying an army and landing, building ships ("NAVY" prefix)
static bool test_navy(InstanceManager& instance_manager, DefinitionManager const& definition_manager) {
	UnitInstanceManager& unit_manager = instance_manager.get_unit_instance_manager();
	MapInstance& map_instance = instance_manager.get_map_instance();
	bool ok = true;
	const auto check = [&ok](bool condition, std::string_view what) -> void {
		fmt::println("NAVY {} : {}", condition ? "ok  " : "FAIL", what);
		ok &= condition;
	};

	size_t docked = 0, at_sea = 0;
	for (NavyInstance const& navy : unit_manager.get_navies()) {
		(navy.get_location().province_definition.is_water() ? at_sea : docked)++;
	}
	fmt::println("NAVY fleets: {} docked in ports, {} at sea", docked, at_sea);

	// A fleet with some ships and a port to go from
	NavyInstance* fleet = nullptr;
	for (NavyInstance const& candidate : unit_manager.get_navies()) {
		if (candidate.get_unit_count() >= 2 && !candidate.get_country().is_rebel_country()
			&& candidate.get_location().province_definition.has_port()) {
			fleet = static_cast<NavyInstance*>(unit_manager.get_unit_instance_group_by_unique_id(candidate.unique_id));
			break;
		}
	}
	if (fleet == nullptr) {
		fmt::println("NAVY FAIL : no docked fleet");
		return false;
	}
	CountryInstance& country = fleet->get_country();
	ProvinceInstance& port = fleet->get_location();
	ProvinceDefinition const* const sea_definition = port.province_definition.get_port_adjacent_province();
	check(sea_definition != nullptr, "the port has a sea next to it");
	if (sea_definition == nullptr) {
		return false;
	}
	fmt::println("NAVY {} fleet \"{}\" with {} ships in {} (sea {})", country.get_identifier(), fleet->get_name(), fleet->get_unit_count(),
		port.get_identifier(), sea_definition->get_identifier());

	// Sail to a sea some provinces away: find a sea about 3 adjacencies away
	ProvinceInstance* far_sea = nullptr;
	{
		std::vector<ProvinceDefinition const*> frontier { sea_definition };
		std::unordered_set<ProvinceDefinition const*> seen { sea_definition };
		for (int depth = 0; depth < 3; ++depth) {
			std::vector<ProvinceDefinition const*> next;
			for (ProvinceDefinition const* current : frontier) {
				for (ProvinceDefinition::adjacency_t const& adjacency : current->get_adjacencies()) {
					if (adjacency.get_to().is_water() && seen.insert(&adjacency.get_to()).second) {
						next.push_back(&adjacency.get_to());
					}
				}
			}
			frontier = next;
		}
		if (!frontier.empty()) {
			far_sea = &map_instance.get_province_instance_by_definition(*frontier.front());
		}
	}
	check(far_sea != nullptr, "found a sea 3 steps away");
	if (far_sea != nullptr) {
		check(unit_manager.order_move(*fleet, *far_sea, map_instance), "order_move to the sea");
		check(fleet->is_moving(), "the fleet moves");
		fmt::println("NAVY route of {} provinces, ETA {} days", fleet->get_path().size(), fleet->get_movement_days_left());
		int day = 0;
		while (fleet->is_moving() && day < 120) {
			instance_manager.force_tick_and_update();
			++day;
		}
		check(&fleet->get_location() == far_sea, "the fleet arrived at the sea");
		fmt::println("NAVY sailed for {} days", day);

		// Back to the port
		check(unit_manager.order_move(*fleet, port, map_instance), "order_move back into the port");
		day = 0;
		while (fleet->is_moving() && day < 120) {
			instance_manager.force_tick_and_update();
			++day;
		}
		check(&fleet->get_location() == &port, "the fleet is back in the port");
	}

	// Battle: an enemy fleet attacks the fleet in the sea next to the port
	NavyInstance* enemy = nullptr;
	for (NavyInstance const& candidate : unit_manager.get_navies()) {
		// An enemy the country is at war with or can declare a war on (not one it has just made peace with: a truce)
		if (&candidate.get_country() != &country && !candidate.get_country().is_rebel_country() && candidate.get_unit_count() >= 1
			&& !country.has_alliance_with(candidate.get_country())
			&& (country.is_at_war_with(candidate.get_country())
				|| instance_manager.get_war_manager().can_declare_war(country, candidate.get_country(), instance_manager.get_today()))) {
			enemy = static_cast<NavyInstance*>(unit_manager.get_unit_instance_group_by_unique_id(candidate.unique_id));
			break;
		}
	}
	if (enemy != nullptr) {
		CountryInstance& enemy_country = enemy->get_country();
		if (!country.is_at_war_with(enemy_country)) {
			check(instance_manager.get_war_manager().declare_war(instance_manager, country, enemy_country), "declare war for the naval battle");
		}
		ProvinceInstance& sea = map_instance.get_province_instance_by_definition(*sea_definition);
		fleet->clear_path();
		enemy->clear_path();
		const size_t ships_before = fleet->get_unit_count() + enemy->get_unit_count();
		unit_manager.order_move(*fleet, sea, map_instance);
		for (int d = 0; d < 20 && &fleet->get_location() != &sea; ++d) {
			instance_manager.force_tick_and_update();
		}
		enemy->set_location(sea);
		const unique_id_t fleet_id = fleet->unique_id;
		const unique_id_t enemy_id = enemy->unique_id;
		fmt::println("NAVY battle {} ({} ships) vs {} ({} ships) at {}", country.get_identifier(), fleet->get_unit_count(),
			enemy_country.get_identifier(), enemy->get_unit_count(), sea.get_identifier());
		int day = 0;
		for (; day < 120; ++day) {
			instance_manager.force_tick_and_update();
			UnitInstanceGroup* const a = unit_manager.get_unit_instance_group_by_unique_id(fleet_id);
			UnitInstanceGroup* const b = unit_manager.get_unit_instance_group_by_unique_id(enemy_id);
			if (day % 5 == 0) {
				fmt::println("NAVY day {:3}: A {} ships org {:.2f} str {:.2f} | B {} ships org {:.2f} str {:.2f}", day + 1,
					a ? a->get_unit_count() : 0, a ? static_cast<double>(a->get_organisation_proportion()) : 0.0, a ? static_cast<double>(a->get_strength_proportion()) : 0.0,
					b ? b->get_unit_count() : 0, b ? static_cast<double>(b->get_organisation_proportion()) : 0.0, b ? static_cast<double>(b->get_strength_proportion()) : 0.0);
			}
			const bool a_here = a != nullptr && &a->get_location() == &sea;
			const bool b_here = b != nullptr && &b->get_location() == &sea;
			if (!(a_here && b_here)) {
				fmt::println("NAVY battle over after {} days: A {} B {}", day + 1, a == nullptr ? "destroyed" : (a_here ? "holds" : "retreated"),
					b == nullptr ? "destroyed" : (b_here ? "holds" : "retreated"));
				break;
			}
		}
		check(day < 120, "naval battle is decided");
		(void)ships_before;
	}

	// Building a transport in the port
	ProvinceInstance& dock = port;
	ShipType const* transport = nullptr;
	for (ShipType const& type : definition_manager.get_military_manager().get_unit_type_manager().get_ship_types()) {
		if (type.is_transport && country.is_unit_type_unlocked(type)) {
			transport = &type;
			break;
		}
	}
	check(transport != nullptr, "found an unlocked transport type");
	if (transport != nullptr) {
		for (auto const& [good, quantity] : transport->get_build_cost()) {
			country.get_good_data(*good).stockpile_amount = quantity * 4;
		}
		const std::string_view reason = unit_manager.get_ship_block_reason(country, *transport, dock);
		fmt::println("NAVY transport build reason: \"{}\"", reason);
		check(reason.empty(), "a transport can be built in the port");
		for (int copy = 0; copy < 2; ++copy) {
			check(unit_manager.start_ship_building(country, *transport, dock), "start_ship_building");
		}
		const int32_t days = UnitInstanceManager::get_recruitment_days(country, *transport);
		for (int32_t d = 0; d <= days; ++d) {
			instance_manager.force_tick_and_update();
		}
		size_t transports_here = 0;
		NavyInstance* carrier = nullptr;
		for (NavyInstance& navy : dock.get_navies()) {
			if (&navy.get_country() == &country && navy.get_transport_capacity() > 0) {
				carrier = &navy;
			}
			for (ShipInstance const& ship : navy.get_ship_instances()) {
				transports_here += ship.get_ship_type().is_transport ? 1 : 0;
			}
		}
		fmt::println("NAVY transports in the port: {} after {} days", transports_here, days);
		check(carrier != nullptr, "a fleet with transport capacity is in the port");
		if (carrier != nullptr) {
			// An army from anywhere is brought to the port
			ArmyInstance* army = nullptr;
			for (ArmyInstance const& candidate : unit_manager.get_armies()) {
				if (&candidate.get_country() == &country && candidate.get_unit_count() >= 1 && candidate.get_unit_count() <= carrier->get_transport_capacity()) {
					army = static_cast<ArmyInstance*>(unit_manager.get_unit_instance_group_by_unique_id(candidate.unique_id));
					break;
				}
			}
			check(army != nullptr, "found an army to carry");
			if (army != nullptr) {
				army->clear_path();
				army->set_location(dock);
				check(carrier->load_army(*army), "load_army");
				check(!unit_manager.order_move(*army, dock, map_instance, false) || true, "a carried army ignores orders");
				ProvinceInstance& sea = map_instance.get_province_instance_by_definition(*sea_definition);
				check(unit_manager.order_move(*carrier, sea, map_instance), "the carrier sails out of the port");
				int d = 0;
				while (carrier->is_moving() && d < 60) {
					instance_manager.force_tick_and_update();
					++d;
				}
				check(&army->get_location() == &sea, "the army travels with the ships");
				carrier->set_in_combat(false);
				check(carrier->land_army(*army, dock), "land_army on the coast");
				check(&army->get_location() == &dock && army->get_transport_navy() == nullptr, "the army stands on land");
			}
		}
	}
	return ok;
}

// Total population by pop type, printed with the "POP" prefix
// A checksum of the state that the economy and the population drive, to compare a replay with the original game
static void print_state_checksum(InstanceManager& instance_manager) {
	std::int64_t pop_total = 0, cash_raw = 0, army_count = 0, province_owner_hash = 0;
	for (ProvinceInstance& province : instance_manager.get_map_instance().get_province_instances()) {
		for (Pop& pop : province.get_mutable_pops()) {
			pop_total += type_safe::get(pop.get_size());
			cash_raw += pop.get_cash().get_copy_of_value().get_raw_value();
		}
		if (province.get_owner() != nullptr) {
			province_owner_hash = province_owner_hash * 31 + type_safe::get(province.get_owner()->index) + 1;
		}
	}
	std::int64_t treasury_raw = 0;
	for (CountryInstance const& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		treasury_raw += country.get_cash_stockpile().load().get_raw_value();
	}
	army_count = static_cast<std::int64_t>(instance_manager.get_unit_instance_manager().get_armies().size());
	SPDLOG_INFO(
		"CHECKSUM {} pops={} popcash={} treasury={} owners={} armies={} actions={} state={:016x}", instance_manager.get_today(), pop_total,
		cash_raw, treasury_raw, province_owner_hash, army_count, instance_manager.get_action_history().size(), StateHash::compute(instance_manager)
	);
}

static void print_population_report(InstanceManager& instance_manager) {
	std::map<std::string, std::int64_t> by_type;
	std::map<std::string, double> life_by_type, cash_by_type, income_by_type, expenses_by_type;
	std::int64_t total = 0, unemployed = 0, employable = 0;
	for (ProvinceInstance& province : instance_manager.get_map_instance().get_province_instances()) {
		for (Pop& pop : province.get_mutable_pops()) {
			const std::int64_t size = type_safe::get(pop.get_size());
			by_type[std::string(pop.get_type().get_identifier())] += size;
			life_by_type[std::string(pop.get_type().get_identifier())] += static_cast<double>(pop.get_life_needs_fulfilled()) * static_cast<double>(size);
			cash_by_type[std::string(pop.get_type().get_identifier())] += static_cast<double>(pop.get_cash().get_copy_of_value());
			income_by_type[std::string(pop.get_type().get_identifier())] += static_cast<double>(pop.get_income());
			expenses_by_type[std::string(pop.get_type().get_identifier())] += static_cast<double>(pop.get_expenses().get_copy_of_value());
			total += size;
			if (pop.get_type().can_be_unemployed) {
				employable += size;
				unemployed += type_safe::get(pop.get_unemployed());
			}
		}
	}
	std::string line = fmt::format("POP {} total={} unemployed={:.1f}%", instance_manager.get_today(), total,
		employable > 0 ? 100.0 * static_cast<double>(unemployed) / static_cast<double>(employable) : 0.0);
	for (std::string_view name : { "farmers", "labourers", "craftsmen", "clerks", "soldiers", "aristocrats", "capitalists", "bureaucrats" }) {
		line += fmt::format(" {}={}", name, by_type[std::string(name)]);
	}
	line += " | life needs:";
	for (std::string_view name : { "farmers", "labourers", "craftsmen", "clerks", "soldiers", "aristocrats" }) {
		const std::int64_t count = by_type[std::string(name)];
		line += fmt::format(" {}={:.2f}", name, count > 0 ? life_by_type[std::string(name)] / static_cast<double>(count) : 0.0);
	}
	// What the needs of a farmer pop cost per 1000 people a day at the current prices
	{
		GoodInstanceManager const& goods = instance_manager.get_good_instance_manager();
		for (std::string_view name : { "farmers", "labourers", "craftsmen", "clerks" }) {
			double life_cost = 0.0, everyday_cost = 0.0;
			bool found = false;
			for (ProvinceInstance& province : instance_manager.get_map_instance().get_province_instances()) {
				for (Pop& pop : province.get_mutable_pops()) {
					if (pop.get_type().get_identifier() != name) {
						continue;
					}
					for (auto const& [good_index, quantity] : pop.get_life_needs()) {
						life_cost += static_cast<double>(quantity) * static_cast<double>(goods.get_good_instance_by_index(good_index)->get_price()) * 1000.0 / 200000.0;
					}
					for (auto const& [good_index, quantity] : pop.get_everyday_needs()) {
						everyday_cost += static_cast<double>(quantity) * static_cast<double>(goods.get_good_instance_by_index(good_index)->get_price()) * 1000.0 / 200000.0;
					}
					found = true;
					break;
				}
				if (found) {
					break;
				}
			}
			line += fmt::format(" | {} needs cost/1000/day: life {:.3f} everyday {:.3f}", name, life_cost, everyday_cost);
		}
	}
	line += " | price/base:";
	for (GoodInstance const& good : instance_manager.get_good_instance_manager().get_good_instances()) {
		const std::string_view name = good.good_definition.get_identifier();
		if (name == "grain" || name == "fruit" || name == "fish" || name == "cattle" || name == "clothes" || name == "furniture"
			|| name == "liquor" || name == "wine" || name == "cotton" || name == "coal" || name == "iron") {
			line += fmt::format(" {}={:.2f}/{:.2f}(s{:.0f} d{:.0f} t{:.0f})", name, static_cast<double>(good.get_price()), static_cast<double>(good.good_definition.base_price),
				static_cast<double>(good.get_total_supply_yesterday()), static_cast<double>(good.get_total_demand_yesterday()), static_cast<double>(good.get_quantity_traded_yesterday()));
		}
	}
	line += " | per 1000 people (cash/income/expenses):";
	for (std::string_view name : { "farmers", "labourers", "craftsmen", "clerks", "soldiers" }) {
		const double count = static_cast<double>(by_type[std::string(name)]) / 1000.0;
		if (count > 0) {
			line += fmt::format(" {}={:.1f}/{:.2f}/{:.2f}", name, cash_by_type[std::string(name)] / count,
				income_by_type[std::string(name)] / count, expenses_by_type[std::string(name)] / count);
		}
	}
	fmt::println("{}", line);
}

// Money flows of the pops by type (per 1000 people and day), printed with the "FLOWS" prefix
static void print_pop_flows(InstanceManager& instance_manager) {
	struct Flow {
		double size = 0.0;
		std::map<std::string, double> values;
	};
	std::map<std::string, Flow> flows;
	for (ProvinceInstance& province : instance_manager.get_map_instance().get_province_instances()) {
		for (Pop& pop : province.get_mutable_pops()) {
			Flow& flow = flows[std::string(pop.get_type().get_identifier())];
			flow.size += static_cast<double>(type_safe::get(pop.get_size()));
#define ADD_FLOW(name) flow.values[#name] += static_cast<double>(pop.get_##name());
			OV_DO_FOR_ALL_TYPES_OF_POP_INCOME(ADD_FLOW)
			OV_DO_FOR_ALL_TYPES_OF_POP_EXPENSES(ADD_FLOW)
#undef ADD_FLOW
		}
	}
	fmt::println("FLOWS {}", instance_manager.get_today());
	for (auto const& [name, flow] : flows) {
		if (flow.size < 1000.0) {
			continue;
		}
		std::string line = fmt::format("FLOWS   {:12}", name);
		for (auto const& [key, value] : flow.values) {
			const double per_1000 = value / (flow.size / 1000.0);
			if (std::abs(per_1000) >= 0.0005) {
				line += fmt::format(" {}={:.3f}", key, per_1000);
			}
		}
		fmt::println("{}", line);
	}
}

// Parties, ideologies and reforms of the biggest countries, printed with the "POLITICS" prefix
static void print_politics_report(InstanceManager& instance_manager) {
	PoliticsManager const& politics = instance_manager.definition_manager.get_politics_manager();
	fmt::println("POLITICS {}", instance_manager.get_today());
	for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		if (!country.exists() || country.get_total_rank() == 0 || country.get_total_rank() > 4) {
			continue;
		}
		std::string line = fmt::format("POLITICS   {} ruling={} last_election={} |", country.get_identifier(),
			country.get_ruling_party_untracked() != nullptr ? country.get_ruling_party_untracked()->get_identifier() : "none",
			country.get_last_election());
		const double total = static_cast<double>(type_safe::get(country.get_total_population()));
		for (Ideology const& ideology : politics.get_ideology_manager().get_ideologies()) {
			const double share = total > 0 ? 100.0 * static_cast<double>(country.get_supporter_equivalents_by_ideology()[ideology.index]) / total : 0.0;
			line += fmt::format(" {}={:.0f}%", ideology.get_identifier(), share);
		}
		{
			ModifierEffectCache const& cache = instance_manager.definition_manager.get_modifier_manager().get_modifier_effect_cache();
			line += " | vote effects:";
			for (Strata const& strata : instance_manager.definition_manager.get_pop_manager().get_stratas()) {
				line += fmt::format(" {}={:.2f}", strata.get_identifier(),
					static_cast<double>(country.get_modifier_effect_value(*cache.get_strata_effects(strata).get_vote())));
			}
			line += fmt::format(" culture rules: primary={} culture={} all={}", country.get_rule_set().is_primary_culture_voting_set(),
				country.get_rule_set().is_culture_voting_set(), country.get_rule_set().is_all_voting_set());
		}
		line += " | votes:";
		double votes_total = 0.0;
		for (CountryParty const& party : country.country_definition.get_parties()) {
			votes_total += static_cast<double>(country.get_vote_equivalents_by_party(party));
		}
		line += fmt::format(" voters={:.0f}%", total > 0 ? 100.0 * votes_total / total : 0.0);
		for (CountryParty const& party : country.country_definition.get_parties()) {
			const double votes = static_cast<double>(country.get_vote_equivalents_by_party(party));
			if (votes_total > 0 && votes / votes_total > 0.03) {
				line += fmt::format(" {}={:.0f}%", party.get_identifier(), 100.0 * votes / votes_total);
			}
		}
		fmt::println("{}", line);
		// Reforms that the country could enact now
		size_t shown = 0;
		for (Reform const& reform : politics.get_issue_manager().get_reforms()) {
			auto status = PoliticsDynamics::get_reform_status(instance_manager, country, reform);
			if (status.can_enact && shown < 6) {
				++shown;
				fmt::println("POLITICS     can enact {} (support {:.0f}% of the group, upper house {:.0f}%)", reform.get_identifier(),
					100.0 * static_cast<double>(status.support), 100.0 * static_cast<double>(status.upper_house));
			}
		}
	}
}

// Research of the biggest countries: they always research the cheapest available technology ("RESEARCH" prefix)
static void run_research_step(InstanceManager& instance_manager, const bool report) {
	TechnologyManager const& technologies = instance_manager.definition_manager.get_research_manager().get_technology_manager();
	const Date today = instance_manager.get_today();
	for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		if (!country.exists() || country.get_total_rank() == 0 || country.get_total_rank() > 8) {
			continue;
		}
		if (country.get_current_research_untracked() == nullptr) {
			Technology const* best = nullptr;
			for (Technology const& technology : technologies.get_technologies()) {
				if (country.can_research_tech(technology, today) && (best == nullptr || technology.cost < best->cost)) {
					best = &technology;
				}
			}
			if (best != nullptr) {
				instance_manager.queue_game_action<start_research_argument_t>(country.index, best->index);
			}
		}
		if (report && country.get_total_rank() == 1) {
			size_t linked = 0, techs_with = 0;
			for (Technology const& technology : technologies.get_technologies()) {
				linked += technology.get_inventions().size();
				techs_with += technology.get_inventions().empty() ? 0 : 1;
			}
			fmt::println("RESEARCH links: {} invention links on {} technologies, {} inventions in total", linked, techs_with,
				instance_manager.definition_manager.get_research_manager().get_invention_manager().get_inventions().size());
		}
		if (report) {
			size_t unlocked = 0;
			for (Technology const& technology : technologies.get_technologies()) {
				unlocked += country.is_technology_unlocked(technology) ? 1 : 0;
			}
			fmt::println("RESEARCH   #{} {} techs={} inventions={} daily_points={:.2f} researching={}", country.get_total_rank(),
				country.get_identifier(), unlocked, country.get_inventions_count_untracked(),
				static_cast<double>(country.get_daily_research_points_untracked()),
				country.get_current_research_untracked() != nullptr ? country.get_current_research_untracked()->get_identifier() : "-");
		}
	}
}

// Tariffs of the biggest countries: slider, effective rate, imports and the income, printed with the "TARIFF" prefix
static void print_tariff_report(InstanceManager& instance_manager) {
	fmt::println("TARIFF {}", instance_manager.get_today());
	size_t shown = 0;
	for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		if (!country.exists() || country.is_rebel_country() || shown >= 12) {
			continue;
		}
		if (country.get_total_rank() == 0 || country.get_total_rank() > 12) {
			continue;
		}
		++shown;
		fmt::println(
			"TARIFF   #{:2} {} slider {:.2f} effective {:.2f} efficiency {:.2f} imports {:.1f} income {:.1f} subsidies {:.1f}",
			country.get_total_rank(), country.get_identifier(),
			static_cast<double>(country.get_tariff_rate_slider_value().get_value_untracked()),
			static_cast<double>(country.effective_tariff_rate.get_untracked()),
			static_cast<double>(country.tariff_efficiency.get_untracked()),
			static_cast<double>(country.get_yesterdays_import_value_untracked()),
			static_cast<double>(country.get_tariff_income()),
			static_cast<double>(country.get_import_subsidies_spending())
		);
	}
}

// -V: every day the sums that the provinces keep from the day before (the political support of the pops) are also worked out again from
// the pops and compared with the kept ones, and the sums of the consumption of the pops that the countries record are compared with the
// map of the old way; a difference is logged and makes the run fail (the result of the run is the same)
static bool verify_kept_sums = false;

static void print_help(FILE* file, std::string_view program_name) {
	fmt::println(file, "Usage: {} [-h] [-t] [-e <days>] [-b <path>] [path]+", program_name);
	fmt::println(file, "    -e : Simulate the given number of days and print an economy report ('ECON') every 30 days.");
	fmt::println(file, "    -C : Print the CHECKSUM of the state after every day (two runs must print the same lines).");
	fmt::println(file, "    -V : Verify every day that the sums kept by the provinces and by the countries are the sums of the pops (slower, a difference fails the run).");
	fmt::println(file, "    -F : With -e: print the times of the parts of the tick ('PROFILE' lines, the same scopes as the F12 debugger).");
	fmt::println(file, "    -j : Use the given number of worker threads (the result must be the same for any number).");
	fmt::println(file, "    --ov-data <folder> : The folder with the data of OpenVic (default: OPENVIC_DATA_PATH, else ov_data found near the executable or in the current folder).");
	fmt::println(file, "    --dump-template : Print the TEMPLATE lines: what the division templates of the first countries are worth (attack, defence, durability, cost, supplies...).");
	fmt::println(file, "    --dump-unit : Print the UNIT lines: the types of land units (the data of OpenVic first) with the stats, the weapons, the scopes and the cost.");
	fmt::println(file, "    --dump-battle : Print the BATTLE lines: the damage a day and the days to destroy of divisions against each other (the formulas of military/BattleFormulas.cpp).");
	fmt::println(file, "    -h : Print this help message and exit the program.");
	fmt::println(file, "    -t : Run tests after loading defines.");
	fmt::println(file, "    -b : Use the following path as the base directory (instead of searching for one).");
	fmt::println(file, "    -s : Use the following path as a hint to search for a base directory.");
	fmt::println(
		file,
		"Any following paths are read as mods (/path/to/my/MODNAME.mod), with priority starting at one above the base "
		"directory."
	);
	fmt::println(file, "(Paths with spaces need to be enclosed in \"quotes\").");
}

static void print_rgo(ProvinceInstance const& province) {
	ResourceGatheringOperation const& rgo = province.get_rgo();
	ProductionType const* const production_type_nullable = rgo.get_production_type_nullable();
	if (production_type_nullable == nullptr) {
		spdlog::error_s("\t{} - production_type: nullptr", province);
	}
#if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_INFO
	else {
		ProductionType const& production_type = *production_type_nullable;
		GoodDefinition const& output_good = production_type.output_good;

		SPDLOG_INFO("{}:", province);
		SPDLOG_INFO(
			"\tgood: {}, "
			"production_type: {}, "
			"size_multiplier: {:.3}, "
			"output_quantity_yesterday: {:.3}, "
			"revenue_yesterday: {:.3}, "
			"total owner income: {:.3}, "
			"total employee income: {:.3}",
			production_type.output_good, //
			production_type, //
			rgo.get_size_multiplier(), //
			rgo.get_output_quantity_yesterday(), //
			rgo.get_revenue_yesterday(), //
			rgo.get_total_owner_income_cache(), //
			rgo.get_total_employee_income_cache()
		);

		bool logged_employees = false;
		{
			pop_type_index_t pop_type_index {};
			for (const pop_size_t employees_of_type : rgo.get_employee_count_per_type_cache()) {
				if (employees_of_type > 0) {
					if (!logged_employees) {
						SPDLOG_INFO("\temployess:");
						logged_employees = true;
					}
					SPDLOG_INFO("\t\t{} {}", employees_of_type, pop_type_index);
				}
				++pop_type_index;
			}
		}
	}
#endif
}

using pathing_rng_t = std::mt19937;
using pathing_rng_seed_t = pathing_rng_t::result_type;

using testing_clock_t = std::chrono::high_resolution_clock;
using test_time_point_t = testing_clock_t::time_point;
using test_duration_t = testing_clock_t::duration;

template<size_t TestCount>
static test_duration_t run_pathing_test(AStarPathing& pathing, std::optional<pathing_rng_seed_t> seed) {
	PointMap const& points = pathing.get_point_map();

	if (!seed.has_value()) {
		thread_local std::random_device rd;
		seed = rd();

		SPDLOG_INFO("Running {} pathing tests with randomly generated seed: {}", TestCount, seed.value());
	} else {
		SPDLOG_INFO("Running {} pathing tests with fixed seed: {}", TestCount, seed.value());
	}

	thread_local pathing_rng_t gen { seed.value() };

	using it = decltype(points.points_map().end());
	std::array<std::pair<it, it>, TestCount> from_to_tests;
	std::array<memory::vector<PointMap::points_key_type>, TestCount> test_results;

	for (size_t index = 0; index < TestCount; index++) {
		it from = points.points_map().begin();
		it to = points.points_map().begin();
		std::uniform_int_distribution<> dis(0, points.get_point_count() - 1);
		std::advance(from, dis(gen));
		std::advance(to, dis(gen));
		from_to_tests[index] = { from, to };
	}

	const test_time_point_t start_time = testing_clock_t::now();
	for (auto [index, pair] : from_to_tests | ranges::views::enumerate) {
		test_results[index] = pathing.get_id_path(pair.first.key(), pair.second.key());
	}
	const test_time_point_t end_time = testing_clock_t::now();

	for (auto [index, test] : test_results | ranges::views::enumerate) {
		SPDLOG_INFO("-Start Test {}-", index + 1);
		SPDLOG_INFO("\tFrom: Id {}", from_to_tests[index].first.key());
		for (PointMap::points_key_type const& id : test) {
			if (id == test.back()) {
				break;
			}
			if (id == test.front()) {
				continue;
			}
			SPDLOG_INFO("\t\tId {}", id);
		}
		if (test.empty()) {
			SPDLOG_INFO("\t\tFailed to find path");
		}
		SPDLOG_INFO("\tTo: Id {}", from_to_tests[index].second.key());
		SPDLOG_INFO("-End Test {}-", index + 1);
	}

	return end_time - start_time;
}

struct SinkGuard {
	std::shared_ptr<spdlog::logger> logger;
	spdlog::sink_ptr sink;

	SinkGuard(std::shared_ptr<spdlog::logger> const& l, spdlog::sink_ptr s) : logger(l), sink(s) {
		logger->sinks().push_back(sink);
	}

	~SinkGuard() {
		std::vector<spdlog::sink_ptr>& sinks = logger->sinks();
		sinks.erase(std::remove(sinks.begin(), sinks.end(), sink), sinks.end());
	}
};

// The data of OpenVic itself (the folder ov_data of the repository): --ov-data <folder>, else the environment variable OPENVIC_DATA_PATH, else
// the folder ov_data found in the current folder, next to the executable or in one of their parents
static fs::path g_builtin_data_path;

static fs::path find_builtin_data_path(char const* program) {
	if (char const* const from_environment = std::getenv("OPENVIC_DATA_PATH"); from_environment != nullptr && *from_environment != 0) {
		return from_environment;
	}
	std::error_code ec;
	for (fs::path start : { fs::current_path(ec), fs::absolute(fs::path { program }, ec).parent_path() }) {
		for (int depth = 0; depth < 8 && !start.empty(); depth++) {
			if (fs::is_directory(start / "ov_data", ec)) {
				return start / "ov_data";
			}
			fs::path parent = start.parent_path();
			if (parent == start) {
				break;
			}
			start = std::move(parent);
		}
	}
	return {};
}

static size_t info_count = 0, warning_count = 0, error_count = 0, critical_count = 0;

// WEAPONS (at the end of -e): for every weapon its good at the market (price, base price, stockpiles of the countries, what the factories made
// yesterday) and how many factories make it; the regiments by weapon (docs/WAR_ASSUMPTIONS.md)
static void print_weapon_report(InstanceManager& instance_manager) {
	EquipmentManager const& equipment_manager = instance_manager.definition_manager.get_military_manager().get_equipment_manager();
	GoodDefinitionManager const& goods = instance_manager.definition_manager.get_economy_manager().get_good_definition_manager();
	for (EquipmentDefinition const& equipment : equipment_manager.get_equipment()) {
		GoodDefinition const* const good = goods.get_good_definition_by_identifier(equipment.good.identifier);
		if (good == nullptr) {
			continue;
		}
		std::size_t factories = 0, open_factories = 0, regiments = 0;
		fixed_point_t produced = fixed_point_t::_0, stockpiles = fixed_point_t::_0;
		for (StateSet const& state_set : instance_manager.get_map_instance().get_state_manager().get_state_sets()) {
			for (State const& state : state_set.get_states()) {
				for (FactoryInstance const& factory : state.get_factories()) {
					if (factory.production_type->output_good.index == good->index) {
						factories++;
						open_factories += factory.is_open() && factory.get_level() > building_level_t(0);
						produced += factory.get_output_quantity_yesterday();
					}
				}
			}
		}
		for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
			if (!country.exists()) {
				continue;
			}
			stockpiles += country.get_good_data(*good).stockpile_amount;
			for (ArmyInstance& army : country.get_armies()) {
				for (RegimentInstance const& regiment : army.get_regiment_instances()) {
					regiments += regiment.get_weapon() == &equipment || (regiment.get_weapon() == nullptr && equipment.tier == 1
						&& equipment_manager.find_line_of_unit_type(regiment.get_regiment_type().get_identifier()) == equipment_manager.find_line(equipment.line));
				}
			}
		}
		GoodInstance const& good_instance = instance_manager.get_good_instance_manager().get_good_instance_by_definition(*good);
		fmt::println(
			"WEAPONS {:<12} tier={} price={:.2f} base={:.2f} factories={} open={} produced_yesterday={:.2f} stockpiles={:.1f} regiments={}",
			equipment.identifier, equipment.tier, static_cast<double>(good_instance.get_price()), static_cast<double>(good->base_price), factories,
			open_factories, static_cast<double>(produced), static_cast<double>(stockpiles), regiments
		);
	}
}


// --dump-template / --dump-battle (docs/EQUIPMENT_MODDING.md): the numbers of the templates and of a battle of divisions without the interface
static bool dump_templates = false, dump_battles = false, dump_units = false;

static std::string format_goods(memory::vector<DivisionGoodAmount> const& goods) {
	std::string text;
	for (DivisionGoodAmount const& entry : goods) {
		text += fmt::format("{}{}={:.3f}", text.empty() ? "" : ",", entry.good, static_cast<double>(entry.amount));
	}
	return text.empty() ? "-" : text;
}

static void print_template_line(std::string_view tag, CountryInstance const& country, DivisionTemplate const& division_template, DivisionStats const& stats) {
	fmt::println(
		"{} country={} template={} regiments={} width={} depth={:.2f} density={:.2f} attack={:.2f} defence={:.2f} durability={:.2f} organisation={:.1f} "
		"fortification={:.2f} weight={:.0f} speed={:.2f} reconnaissance={:.2f} occupation_speed={:.2f}{} reliability={:.2f} men={:.0f}",
		tag, country.get_identifier(), division_template.id, stats.regiments, stats.width, static_cast<double>(stats.depth), static_cast<double>(stats.density),
		static_cast<double>(stats.attack), static_cast<double>(stats.defence), static_cast<double>(stats.durability), static_cast<double>(stats.organisation),
		static_cast<double>(stats.fortification), static_cast<double>(stats.weight), static_cast<double>(stats.speed), static_cast<double>(stats.reconnaissance),
		static_cast<double>(stats.occupation_speed), stats.occupation_instant ? "(instant)" : "", static_cast<double>(stats.reliability), static_cast<double>(stats.men)
	);
	fmt::println("{}_COST country={} template={} {}", tag, country.get_identifier(), division_template.id, format_goods(stats.cost));
	fmt::println("{}_SUPPLY_PER_DAY country={} template={} {}", tag, country.get_identifier(), division_template.id, format_goods(stats.supply_per_day));
	fmt::println("{}_AMMO_PER_DAY country={} template={} {}", tag, country.get_identifier(), division_template.id, format_goods(stats.ammo_per_day));
}

// --dump-unit: the types of land units (UNIT lines), the data of OpenVic first
static void run_unit_dump(InstanceManager& instance_manager) {
	UnitTypeManager const& unit_types = instance_manager.definition_manager.get_military_manager().get_unit_type_manager();
	ModifierEffectCache const& cache = instance_manager.definition_manager.get_modifier_manager().get_modifier_effect_cache();
	for (RegimentType const& type : unit_types.get_regiment_types()) {
		std::string weapons;
		for (memory::string const& weapon : type.ov.equipment) {
			weapons += (weapons.empty() ? "" : ",") + std::string { weapon };
		}
		std::string scopes;
		for (ScopeWeight const& weight : get_scope_weights(cache, type)) {
			scopes += fmt::format("{}{}={:.2f}", scopes.empty() ? "" : ",", cache.get_unit_scopes()[weight.scope], static_cast<double>(weight.weight));
		}
		std::string cost;
		for (auto const& [good, quantity] : type.get_build_cost()) {
			cost += fmt::format("{}{}={:.1f}", cost.empty() ? "" : ",", good->get_identifier(), static_cast<double>(quantity));
		}
		fmt::println(
			"UNIT {:<24} source={} category={} key={} equipment={} attack={:.2f} defence={:.2f} breakthrough={:.2f} organisation={:.1f} speed={:.2f} reconnaissance={:.1f} "
			"strength={:.1f} occupation_weight={:.2f} scopes={} cost={} upgrades_to={}",
			type.get_identifier(), type.ov.is_ov ? "ov" : "vic2", type.ov.is_ov ? type.ov.category_scope : get_unit_category_scope(type),
			type.ov.unlock_key.empty() ? "-" : type.ov.unlock_key, weapons.empty() ? "-" : weapons, static_cast<double>(type.attack), static_cast<double>(type.defence),
			static_cast<double>(type.get_breakthrough()), static_cast<double>(type.default_organisation), static_cast<double>(type.maximum_speed),
			static_cast<double>(type.reconnaissance), static_cast<double>(type.max_strength), static_cast<double>(type.ov.occupation_weight), scopes, cost,
			type.ov.upgrades_to.empty() ? "-" : type.ov.upgrades_to
		);
	}
}

static void run_template_battle_dump(InstanceManager& instance_manager) {
	OpenVicWarDefines const& war = instance_manager.definition_manager.get_define_manager().get_openvic_defines().war;
	std::size_t printed = 0;
	for (CountryInstance const& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		if (!country.exists() || !country.has_army_organisation() || country.get_division_templates().empty() || printed >= 3) {
			continue;
		}
		printed++;
		for (DivisionTemplate const& division_template : country.get_division_templates()) {
			const DivisionStats stats = compute_division_stats(instance_manager, country, division_template);
			if (dump_templates) {
				print_template_line("TEMPLATE", country, division_template, stats);
			}
			if (dump_battles) {
				// The template against a division that is a rank of the infantry-like cells of it: the damage a day each way and the days to destroy
				DivisionTemplate narrow = division_template;
				for (std::size_t rank = 1; rank < DIVISION_MAX_RANKS; ++rank) {
					for (std::size_t row = 0; row < DIVISION_MAX_DEPTH; ++row) {
						narrow.cells[DivisionTemplate::cell_index(rank, row)] = 0;
					}
				}
				narrow.rebuild_counts();
				const DivisionStats narrow_stats = compute_division_stats(instance_manager, country, narrow);
				const auto line = [&](std::string_view label, DivisionStats const& a, DivisionStats const& b) {
					const fixed_point_t damage = battle_formulas::damage_per_day(war, a, b);
					fmt::println(
						"BATTLE country={} template={} {} attacker_width={} defender_width={} damage_per_day={:.3f} defender_durability={:.2f} days_to_destroy={:.1f}",
						country.get_identifier(), division_template.id, label, a.width, b.width, static_cast<double>(damage), static_cast<double>(b.durability),
						static_cast<double>(battle_formulas::days_to_destroy(b.durability, damage))
					);
				};
				line("full_vs_full", stats, stats);
				line("full_vs_one_rank", stats, narrow_stats);
				line("one_rank_vs_full", narrow_stats, stats);
			}
		}
	}
}

static bool run_headless(fs::path const& root, memory::vector<memory::string>& mods, bool run_tests) {
	bool ret = true;
	Dataloader::path_vector_t roots = { root };
	Dataloader::path_vector_t replace_paths = {};

	spdlog::sink_ptr counter_sink = std::make_shared<spdlog::sinks::callback_sink_st>([](spdlog::details::log_msg const& msg) {
		switch (msg.level) {
			using namespace spdlog::level;
		case info:	   info_count++; break;
		case warn:	   warning_count++; break;
		case err:	   error_count++; break;
		case critical: critical_count++; break;
		default:	   break;
		}
	});

	SinkGuard guard(spdlog::default_logger(), counter_sink);

	GameManager game_manager {
		[] {
			SPDLOG_INFO("State updated");
		},
		nullptr, nullptr //
	};

	SPDLOG_INFO("Commit hash: {}", GameManager::get_commit_hash());

	SPDLOG_INFO("===== Setting base path... =====");
	game_manager.set_builtin_data_path(g_builtin_data_path);
	ret &= game_manager.set_base_path(roots);

	SPDLOG_INFO("===== Loading mod descriptors... =====");
	ret &= game_manager.load_mod_descriptors();

	SPDLOG_INFO("===== Loading mods... =====");
	ret &= game_manager.load_mods(mods);

	SPDLOG_INFO("===== Loading definitions... =====");
	ret &= game_manager.load_definitions(
		[](std::string_view key, Dataloader::locale_t locale, std::string_view localisation) -> bool {
			return true;
		}
	);

	print_memory_usage("Definition Setup");

	if (run_tests) {
		// std::shared_ptr<spdlog::logger> testing_logger = spdlog::basic_logger_st(
		// 	"testing",
		// 	"testing/test_results/results.txt",
		// 	true
		// );
		// testing_logger->set_pattern("%v");
		std::shared_ptr<spdlog::logger> testing_logger = spdlog::default_logger();
		Testing testing { game_manager.get_definition_manager(), testing_logger };
		SPDLOG_INFO("Testing Loaded");
		testing.execute_all_scripts();
		testing.report_results();
		SPDLOG_INFO("Testing Executed");
	}

	if (!defines_overrides.empty()) {
		OpenVicDefines& openvic_defines = const_cast<DefineManager&>(game_manager.get_definition_manager().get_define_manager()).get_mutable_openvic_defines();
		for (std::string const& override_text : defines_overrides) {
			const size_t dot = override_text.find('.');
			const size_t equals = override_text.find('=');
			if (dot == std::string::npos || equals == std::string::npos || dot > equals) {
				spdlog::error_s("Wrong -o \"{}\": use group.KEY=value", override_text);
				return false;
			}
			bool found = false;
			for (OpenVicDefineGroup* group : openvic_defines.get_groups()) {
				if (group->get_name() == std::string_view { override_text }.substr(0, dot)) {
					group->set_value("command line", std::string_view { override_text }.substr(dot + 1, equals - dot - 1), std::string_view { override_text }.substr(equals + 1));
					found = true;
				}
			}
			if (!found) {
				spdlog::error_s("Unknown group in -o \"{}\"", override_text);
				return false;
			}
		}
		openvic_defines.update_hash();
	}

	if (run_selftest) {
		auto const& test_bookmarks = game_manager.get_definition_manager().get_history_manager().get_bookmark_manager().get_bookmarks();
		if (bookmark_index >= test_bookmarks.size()) {
			spdlog::error_s("Invalid bookmark index {}", bookmark_index);
			return false;
		}
		return ret & run_data_selftests(game_manager, test_bookmarks[bookmark_index]);
	}

	SPDLOG_INFO("===== Setting up instance... =====");
	auto const& bookmarks = game_manager.get_definition_manager().get_history_manager().get_bookmark_manager().get_bookmarks();
	if (bookmark_index >= bookmarks.size()) {
		spdlog::error_s("Invalid bookmark index {}", bookmark_index);
		return false;
	}
	Bookmark const* const bookmark = &bookmarks[bookmark_index];
	if (recommended_rules) {
		SPDLOG_INFO("Using the recommended game rules (price changes that clear the market)");
		const_cast<GameRulesManager&>(game_manager.get_game_rules_manager()).use_recommended_rules();
	}
	ret &= game_manager.setup_instance(*bookmark);

	print_memory_usage("Instance Setup");

	SPDLOG_INFO("===== Starting game session... =====");
	ret &= game_manager.start_game_session();

	// This triggers a gamestate update
	ret &= game_manager.update_clock();

	print_memory_usage("Game Session Post-Start");

	SPDLOG_INFO(
		"OpenVic defines hash {:016x}", game_manager.get_definition_manager().get_define_manager().get_openvic_defines().get_hash()
	);
	if (!defines_dump_path.empty()) {
		std::ofstream file(defines_dump_path);
		file << game_manager.get_definition_manager().get_define_manager().get_openvic_defines().to_lua_text();
		SPDLOG_INFO("Wrote the OpenVic defines to {}", defines_dump_path);
	}

	if (!replay_load_path.empty()) {
		std::ifstream file(replay_load_path);
		std::stringstream buffer;
		buffer << file.rdbuf();
		std::vector<ReplayEntry> entries;
		if (!Replay::deserialise(buffer.str(), entries)) {
			spdlog::error_s("Could not read the replay {}", replay_load_path);
			ret = false;
		} else {
			ret &= game_manager.get_instance_manager()->replay_actions(entries, [](size_t done, size_t total) {
				if (done == total) {
					SPDLOG_INFO("Replayed {} actions", done);
				}
			});
			print_state_checksum(*game_manager.get_instance_manager());
		}
	}

	// TODO - REMOVE TEST CODE
	SPDLOG_INFO("===== Ranking system test... =====");
	if (game_manager.get_instance_manager()) {
		const auto print_ranking_list = [ //
		](std::string_view title, OpenVic::forwardable_span<const std::reference_wrapper<CountryInstance>> countries) -> void {
			memory::string countries_str;
			for (CountryInstance& country : countries) {
				countries_str += fmt::format(
					"\n\t{} - Total #{} ({:.1}), Prestige #{} ({:.1}), Industry #{} ({:.1}), Military #{} ({:.1})", //
					country, //
					country.get_total_rank(), country.total_score.get_untracked(), //
					country.get_prestige_rank(), country.get_prestige_untracked(),
					country.get_industrial_rank(), country.get_industrial_power_untracked(),
					country.get_military_rank(), country.military_power.get_untracked()
				);
			}
			SPDLOG_INFO("{}:{}", title, countries_str);
		};

		CountryInstanceManager const& country_instance_manager = game_manager.get_instance_manager()->get_country_instance_manager();

		OpenVic::forwardable_span<const std::reference_wrapper<CountryInstance>> great_powers = country_instance_manager.get_great_powers();
		print_ranking_list("Great Powers", great_powers);
		print_ranking_list("Secondary Powers", country_instance_manager.get_secondary_powers());
		print_ranking_list("All countries", country_instance_manager.get_total_ranking());

		SPDLOG_INFO("===== RGO test... =====");
		for (size_t i = 0; i < std::min<size_t>(3, great_powers.size()); ++i) {
			CountryInstance const& great_power = great_powers[i];
			ProvinceInstance const* const capital_province = great_power.get_capital();
			if (capital_province == nullptr) {
				spdlog::warn_s("{} has no capital ProvinceInstance set.", great_power);
			} else {
				print_rgo(*capital_province);
			}
		}
	} else {
		spdlog::error_s("Instance manager not available!");
		ret = false;
	}

	if (ret) {
		static constexpr size_t TESTS = 10;
		// Set to std::nullopt to use a different random seed on each run
		static constexpr std::optional<pathing_rng_seed_t> LAND_SEED = 1836, SEA_SEED = 1861;

		using test_time_units_t = std::chrono::milliseconds;

		MapInstance& map_instance = game_manager.get_instance_manager()->get_map_instance();

		SPDLOG_INFO("===== Land Pathfinding test... =====");
		test_duration_t duration = std::chrono::duration_cast<test_time_units_t>( //
			run_pathing_test<TESTS>(map_instance.get_land_pathing(), LAND_SEED)
		);
		SPDLOG_INFO("Ran {} land pathing tests in {}", TESTS, duration);

		SPDLOG_INFO("===== Sea Pathfinding test... =====");
		duration = std::chrono::duration_cast<test_time_units_t>( //
			run_pathing_test<TESTS>(map_instance.get_sea_pathing(), SEA_SEED)
		);
		SPDLOG_INFO("Ran {} sea pathing tests in {}", TESTS, duration);
	}

	if (ret) {
		static constexpr size_t TICK_COUNT = 10;

		SPDLOG_INFO("===== Game Tick test... =====");
		size_t ticks_passed = 0;
		test_duration_t min_tick_duration = test_duration_t::max(), //
			max_tick_duration = test_duration_t::min(), //
			total_tick_duration {};
		const test_time_point_t start_time = testing_clock_t::now();
		while (++ticks_passed < TICK_COUNT) {
			const test_time_point_t tick_start = testing_clock_t::now();
			game_manager.get_instance_manager()->force_tick_and_update();
			if (checksum_every_day) {
				print_state_checksum(*game_manager.get_instance_manager());
			}
			const test_time_point_t tick_end = testing_clock_t::now();

			const test_duration_t tick_duration = tick_end - tick_start;

			min_tick_duration = std::min(min_tick_duration, tick_duration);
			max_tick_duration = std::max(max_tick_duration, tick_duration);

			total_tick_duration += tick_duration;

			print_memory_usage(fmt::format("Tick {} finished", ticks_passed));
		}
		const test_time_point_t end_time = testing_clock_t::now();
		const test_duration_t duration = end_time - start_time;

		if (ticks_passed != 0 && duration != test_duration_t::zero()) {
			const test_duration_t tick_tps = total_tick_duration / ticks_passed;
			const test_duration_t total_tps = duration / ticks_passed;
			spdlog::info(
				"Ran {} / {} ticks, total time {} at {} per tick, tick time only {} at {} per tick. "
				"Tick lengths ranged from {} to {}.",
				ticks_passed, TICK_COUNT, duration, total_tps, total_tick_duration, tick_tps, //
				min_tick_duration, max_tick_duration
			);
		} else {
			spdlog::error_s(
				"No ticks passed ({}, expected {}) or zero duration measured ({})!", ticks_passed, TICK_COUNT, duration
			);
			ret = false;
		}
	}

	if (ret && dump_units) {
		run_unit_dump(*game_manager.get_instance_manager());
	}
	if (ret && (dump_templates || dump_battles)) {
		run_template_battle_dump(*game_manager.get_instance_manager());
	}

	if (ret && run_army_test) {
		SPDLOG_INFO("===== Army test... =====");
		ret &= test_armies(*game_manager.get_instance_manager(), game_manager.get_definition_manager());
		ret &= test_battle(*game_manager.get_instance_manager());
		ret &= test_navy(*game_manager.get_instance_manager(), game_manager.get_definition_manager());
	}

	if (ret && tariff_test && economy_report_days == 0) {
		economy_report_days = 90;
	}

	if (ret && economy_report_days > 0) {
		InstanceManager& instance_manager = *game_manager.get_instance_manager();
		if (tariff_test) {
			// The biggest countries put a tariff of 25% (the slider is clamped to the limits of the country)
			for (CountryInstance const& country : instance_manager.get_country_instance_manager().get_country_instances()) {
				if (country.get_total_rank() > 0 && country.get_total_rank() <= 12) {
					instance_manager.queue_game_action<set_tariff_rate_argument_t>(country.index, fixed_point_t(1) / 4);
				}
			}
			instance_manager.force_tick_and_update();
			print_tariff_report(instance_manager);
		}
		if (close_all_factories) {
			for (CountryInstance const& country : instance_manager.get_country_instance_manager().get_country_instances()) {
				instance_manager.queue_game_action<set_all_factories_open_argument_t>(country.index, false);
			}
			instance_manager.force_tick_and_update();
		}
		if (money_ledger_report) {
			instance_manager.get_money_ledger().enable(instance_manager);
		}
		BalanceReport balance;
		if (subsidise_all) {
			for (CountryInstance const& country : instance_manager.get_country_instance_manager().get_country_instances()) {
				instance_manager.queue_game_action<set_all_factories_subsidised_argument_t>(country.index, true);
			}
			instance_manager.force_tick_and_update();
		}
		print_economy_report(instance_manager);
		print_population_report(instance_manager);
		if (profile_ticks) {
			instance_manager.get_tick_profiler().set_enabled(true);
			instance_manager.get_tick_profiler().reset();
		}
		for (size_t day = 1; day <= economy_report_days; ++day) {
			instance_manager.force_tick_and_update();
			if (checksum_every_day) {
				print_state_checksum(instance_manager);
			}
			if (balance_report) {
				balance.update(instance_manager);
			}
			if (research_test && day % 5 == 0) {
				run_research_step(instance_manager, day % 90 == 0);
			}
			if (day % 30 == 0 || (balance_report && std::getenv("BAL_DAILY") != nullptr && day > 60)) {
				if (money_ledger_report) {
					fmt::println("{}", instance_manager.get_money_ledger().report(instance_manager));
				}
				if (balance_report) {
					balance.print(instance_manager, balance_goods);
				}
				print_economy_report(instance_manager);
				print_population_report(instance_manager);
				if (politics_report) {
					print_politics_report(instance_manager);
				}
				if (day % 90 == 0) {
					print_pop_flows(instance_manager);
				}
				if (tariff_test) {
					print_tariff_report(instance_manager);
				}
			}
		}
	}

	if (ret && economy_report_days > 0 && profile_ticks) {
		// Times of the parts of the tick and of the update of the game state (the same scopes as the F12 debugger of the game)
		fmt::println("PROFILE {} days: part | average ms | maximum ms | calls | total ms", economy_report_days);
		for (TickProfiler::Entry const& entry : game_manager.get_instance_manager()->get_tick_profiler().get_entries()) {
			fmt::println(
				"PROFILE {:<52} {:>9.3f} {:>9.3f} {:>6} {:>10.1f}", std::string(static_cast<size_t>(entry.depth) * 2, ' ') + entry.name,
				entry.average_ns / 1.0e6, static_cast<double>(entry.max_ns) / 1.0e6, entry.calls, static_cast<double>(entry.total_ns) / 1.0e6
			);
		}
	}

	if (ret && economy_report_days > 0) {
		print_weapon_report(*game_manager.get_instance_manager());
		print_state_checksum(*game_manager.get_instance_manager());
		if (verify_kept_sums) {
			const size_t differences = ProvinceInstance::get_political_support_cache_mismatch_count();
			fmt::println("VERIFY the political support that the provinces kept: {} differences", differences);
			const size_t consumption_differences = CountryInstance::get_need_consumption_mismatch_count();
			fmt::println("VERIFY the consumption of the pops that the countries recorded: {} differences", consumption_differences);
			ret &= differences == 0 && consumption_differences == 0;
		}
		if (!replay_save_path.empty()) {
			std::ofstream file(replay_save_path);
			file << Replay::serialise(game_manager.get_instance_manager()->get_action_history());
		}
	}

	SPDLOG_INFO("===== Ending game session... =====");
	ret &= game_manager.end_game_session();

	SPDLOG_INFO("Max Memory Usage: {} Bytes", OpenVic::memory::MemoryTracker::get_max_memory_usage());

	return ret;
}

/*
	$ program [-h] [-t] [-b] [path]+
*/

int main(int argc, char const* argv[]) {
	// The tests that need no game files run before anything is loaded
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--selftest-pure") == 0) {
			return run_pure_selftests() ? 0 : 1;
		}
	}

	std::string_view program_name = get_filename(argc > 0 ? argv[0] : "", "<program>");
	fs::path root;
	memory::vector<memory::string> mods;
	mods.reserve(argc);
	bool run_tests = false;
	int argn = 0;

	/* Reads the next argument and converts it to a path via path_transform. If reading or converting fails, an error
	 * message and the help text are displayed, along with returning false to signify the program should exit.
	 */
	const auto _read = [&root, &argn, argc, argv, &program_name //
	](std::string_view command, std::string_view path_use, std::invocable<fs::path> auto path_transform) -> bool {
		if (root.empty()) {
			if (++argn < argc) {
				char const* path = argv[argn];
				root = path_transform(path);
				if (!root.empty()) {
					return true;
				} else {
					fmt::println(
						stderr, "Empty path after giving \"{}\" to {} command line argument \"{}\".", path, path_use, command
					);
				}
			} else {
				fmt::println(stderr, "Missing path after {} command line argument \"{}\".", path_use, command);
			}
		} else {
			fmt::println(stderr, "Duplicate {} command line argument \"-b\".", path_use);
		}
		print_help(stderr, program_name);
		return false;
	};

	while (++argn < argc) {
		char const* arg = argv[argn];
		if (strcmp(arg, "-h") == 0) {
			print_help(stdout, program_name);
			return 0;
		} else if (strcmp(arg, "-t") == 0) {
			run_tests = true;
		} else if (strcmp(arg, "-A") == 0) {
			run_army_test = true;
		} else if (strcmp(arg, "-D") == 0 && argn + 1 < argc) {
			defines_dump_path = argv[++argn];
		} else if ((strcmp(arg, "-W") == 0 || strcmp(arg, "-L") == 0) && argn + 1 < argc) {
			(arg[1] == 'W' ? replay_save_path : replay_load_path) = argv[++argn];
		} else if (strcmp(arg, "-X") == 0) {
			research_test = true;
		} else if (strcmp(arg, "-P") == 0) {
			politics_report = true;
		} else if (strcmp(arg, "-o") == 0 && argn + 1 < argc) {
			defines_overrides.emplace_back(argv[++argn]);
		} else if (strcmp(arg, "-B") == 0) {
			balance_report = true;
		} else if (strcmp(arg, "-G") == 0) {
			balance_report = balance_goods = true;
		} else if (strcmp(arg, "-C") == 0) {
			checksum_every_day = true;
		} else if (strcmp(arg, "-V") == 0) {
			verify_kept_sums = true;
			ProvinceInstance::set_verify_political_support_cache(true);
			CountryInstance::set_verify_need_consumption(true);
		} else if (strcmp(arg, "--selftest") == 0) {
			run_selftest = true;
		} else if (strcmp(arg, "--selftest-pure") == 0) {
			run_selftest_pure = true;
		} else if (strcmp(arg, "-M") == 0) {
			money_ledger_report = true;
		} else if (strcmp(arg, "-N") == 0) {
			close_all_factories = true;
		} else if (strcmp(arg, "-R") == 0) {
			recommended_rules = true;
		} else if (strcmp(arg, "-T") == 0) {
			tariff_test = true;
		} else if (strcmp(arg, "-S") == 0) {
			subsidise_all = true;
		} else if (strcmp(arg, "-k") == 0) {
			if (++argn >= argc) {
				fmt::println(stderr, "Missing bookmark index after \"-k\".");
				return -1;
			}
			bookmark_index = static_cast<size_t>(std::max(0, std::atoi(argv[argn])));
		} else if (strcmp(arg, "-j") == 0) {
			if (++argn >= argc) {
				fmt::println(stderr, "Missing number of threads after \"-j\".");
				return -1;
			}
			ThreadPool::set_worker_thread_count(static_cast<size_t>(std::max(0, std::atoi(argv[argn]))));
		} else if (strcmp(arg, "-F") == 0) {
			profile_ticks = true;
		} else if (strcmp(arg, "-e") == 0) {
			if (++argn >= argc) {
				fmt::println(stderr, "Missing number of days after \"-e\".");
				return -1;
			}
			economy_report_days = static_cast<size_t>(std::max(0, std::atoi(argv[argn])));
		} else if (strcmp(arg, "--dump-template") == 0) {
			dump_templates = true;
		} else if (strcmp(arg, "--dump-battle") == 0) {
			dump_battles = true;
		} else if (strcmp(arg, "--dump-unit") == 0) {
			dump_units = true;
		} else if (strcmp(arg, "--ov-data") == 0) {
			if (++argn >= argc) {
				fmt::println(stderr, "Missing folder after \"--ov-data\".");
				return -1;
			}
			g_builtin_data_path = argv[argn];
		} else if (strcmp(arg, "-b") == 0) {
			if (!_read("-b", "base directory", std::identity {})) {
				return -1;
			}
		} else if (strcmp(arg, "-s") == 0) {
			if (!_read("-s", "search hint", Dataloader::search_for_game_path)) {
				return -1;
			}
		} else {
			break;
		}
	}
	if (g_builtin_data_path.empty()) {
		g_builtin_data_path = find_builtin_data_path(argc > 0 ? argv[0] : "");
	}
	if (root.empty()) {
		root = Dataloader::search_for_game_path();
		if (root.empty()) {
			fmt::println(stderr, "Search for base directory path failed!");
			print_help(stderr, program_name);
			return -1;
		}
	}
	while (argn < argc) {
		mods.emplace_back(argv[argn++]);
	}

	SPDLOG_INFO("!!! HEADLESS SIMULATION START !!!");

	bool pure_tests_ok = true;
	if (run_selftest || run_selftest_pure) {
		pure_tests_ok = run_pure_selftests();
		if (run_selftest_pure) {
			return pure_tests_ok ? 0 : -1;
		}
	}

	const bool ret = run_headless(root, mods, run_tests) && pure_tests_ok;

	SPDLOG_INFO("!!! HEADLESS SIMULATION END !!!");

	spdlog::info("Load returned: {}", ret ? "SUCCESS" : "FAILURE");

	spdlog::info(
		"Logger Summary: "
#if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_INFO
		"Info = {}, "
#endif
		"Warning = {}, "
		"Error = {}, "
		"Critical = {}",
#if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_INFO
		info_count,
#endif
		warning_count, error_count, critical_count
	);

	return ret ? 0 : -1;
}

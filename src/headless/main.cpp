#include <chrono>
#include <filesystem>
#include <memory>
#include <random>
#include <unordered_set>

#include <fmt/base.h>
#include <fmt/chrono.h>

#include <range/v3/view/enumerate.hpp>

#include <spdlog/common.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/callback_sink.h>

#include <openvic-simulation/core/memory/MemoryTracker.hpp>
#include <openvic-simulation/core/memory/Vector.hpp>
#include <openvic-simulation/GameManager.hpp>
#include <openvic-simulation/country/CountryInstance.hpp>
#include <openvic-simulation/InstanceManager.hpp>
#include <openvic-simulation/military/UnitInstanceGroup.hpp>
#include <openvic-simulation/map/ProvinceInstance.hpp>
#include <openvic-simulation/population/PopType.hpp>
#include <openvic-simulation/misc/GameAction.hpp>
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
static bool run_army_test = false;
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
		if (!loan->active) {
			continue;
		}
		if (loan->kind == Loan::kind_t::EXTERNAL) {
			external_loans++;
			external_debt += loan->remaining;
		} else {
			internal_loans++;
			internal_debt += loan->remaining;
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
		if (&candidate.get_country() != &country && !candidate.get_country().is_rebel_country() && candidate.get_unit_count() >= 1
			&& !country.has_alliance_with(candidate.get_country())) {
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

static void print_help(FILE* file, std::string_view program_name) {
	fmt::println(file, "Usage: {} [-h] [-t] [-e <days>] [-b <path>] [path]+", program_name);
	fmt::println(file, "    -e : Simulate the given number of days and print an economy report ('ECON') every 30 days.");
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

static size_t info_count = 0, warning_count = 0, error_count = 0, critical_count = 0;
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

	SPDLOG_INFO("===== Setting up instance... =====");
	auto const& bookmarks = game_manager.get_definition_manager().get_history_manager().get_bookmark_manager().get_bookmarks();
	if (bookmark_index >= bookmarks.size()) {
		spdlog::error_s("Invalid bookmark index {}", bookmark_index);
		return false;
	}
	Bookmark const* const bookmark = &bookmarks[bookmark_index];
	ret &= game_manager.setup_instance(*bookmark);

	print_memory_usage("Instance Setup");

	SPDLOG_INFO("===== Starting game session... =====");
	ret &= game_manager.start_game_session();

	// This triggers a gamestate update
	ret &= game_manager.update_clock();

	print_memory_usage("Game Session Post-Start");

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

	if (ret && run_army_test) {
		SPDLOG_INFO("===== Army test... =====");
		ret &= test_armies(*game_manager.get_instance_manager(), game_manager.get_definition_manager());
		ret &= test_battle(*game_manager.get_instance_manager());
		ret &= test_navy(*game_manager.get_instance_manager(), game_manager.get_definition_manager());
	}

	if (ret && economy_report_days > 0) {
		InstanceManager& instance_manager = *game_manager.get_instance_manager();
		if (subsidise_all) {
			for (CountryInstance const& country : instance_manager.get_country_instance_manager().get_country_instances()) {
				instance_manager.queue_game_action<set_all_factories_subsidised_argument_t>(country.index, true);
			}
			instance_manager.force_tick_and_update();
		}
		print_economy_report(instance_manager);
		for (size_t day = 1; day <= economy_report_days; ++day) {
			instance_manager.force_tick_and_update();
			if (day % 30 == 0) {
				print_economy_report(instance_manager);
			}
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
		} else if (strcmp(arg, "-S") == 0) {
			subsidise_all = true;
		} else if (strcmp(arg, "-k") == 0) {
			if (++argn >= argc) {
				fmt::println(stderr, "Missing bookmark index after \"-k\".");
				return -1;
			}
			bookmark_index = static_cast<size_t>(std::max(0, std::atoi(argv[argn])));
		} else if (strcmp(arg, "-e") == 0) {
			if (++argn >= argc) {
				fmt::println(stderr, "Missing number of days after \"-e\".");
				return -1;
			}
			economy_report_days = static_cast<size_t>(std::max(0, std::atoi(argv[argn])));
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

	const bool ret = run_headless(root, mods, run_tests);

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

#include <cstdlib>
#include "InstanceManager.hpp"

#include "openvic-simulation/economy/production/InvestmentPlanner.hpp"

#include "openvic-simulation/ai/CountryAI.hpp"
#include "openvic-simulation/politics/PoliticsDynamics.hpp"
#include "openvic-simulation/research/ResearchDynamics.hpp"
#include "openvic-simulation/population/PopDynamics.hpp"

#include "openvic-simulation/DefinitionManager.hpp"
#include "openvic-simulation/console/ConsoleInstance.hpp"
#include "openvic-simulation/core/stl/containers/TypedSpan.hpp"
#include "openvic-simulation/economy/production/ArtisanalProducer.hpp"
#include "openvic-simulation/military/Division.hpp"
#include "openvic-simulation/military/SupplyDelivery.hpp"
#include "openvic-simulation/military/SupplyTerritory.hpp"
#include "openvic-simulation/misc/GameAction.hpp"
#include "openvic-simulation/utility/Logger.hpp"

using namespace OpenVic;

InstanceManager::InstanceManager(
	GameRulesManager const& new_game_rules_manager,
	DefinitionManager const& new_definition_manager,
	gamestate_updated_func_t gamestate_updated_callback
) : thread_pool { today },
	definition_manager { new_definition_manager },
	game_action_manager { *this },
	game_rules_manager { new_game_rules_manager },
	good_instance_manager {
		new_definition_manager.get_economy_manager().get_good_definition_manager(),
		new_game_rules_manager
	},
	market_instance {
		thread_pool,
		new_definition_manager.get_define_manager().get_country_defines(),
		good_instance_manager
	},
	artisanal_producer_deps {
		new_definition_manager.get_define_manager().get_economy_defines(),
		new_definition_manager.get_economy_manager().get_good_definition_manager().get_good_definitions(),
		new_definition_manager.get_modifier_manager().get_modifier_effect_cache()
	},
	country_instance_deps {
		new_definition_manager.get_economy_manager().get_building_type_manager().get_building_types(),
		new_definition_manager.get_define_manager().get_country_defines(),
		country_relation_manager,
		new_definition_manager.get_crime_manager().get_crime_modifiers(),
		new_definition_manager.get_define_manager().get_end_date(),
		new_definition_manager.get_define_manager().get_diplomacy_defines(),
		new_definition_manager.get_define_manager().get_economy_defines(),
		new_definition_manager.get_politics_manager().get_ideology_manager().get_ideologies(),
		new_definition_manager.get_research_manager().get_invention_manager().get_inventions(),
		new_game_rules_manager,
		good_instance_manager.get_good_instances(),
		good_instance_manager,
		new_definition_manager.get_politics_manager().get_government_type_manager().get_government_types(),
		market_instance,
		new_definition_manager.get_define_manager().get_military_defines(),
		new_definition_manager.get_modifier_manager().get_modifier_effect_cache(),
		PopsAggregateDeps{
			ideology_index_t(
				new_definition_manager.get_politics_manager().get_ideology_manager().get_ideology_count()
			),
			party_policy_index_t(
				new_definition_manager.get_politics_manager().get_issue_manager().get_party_policy_count()
			),
			pop_type_index_t(
				new_definition_manager.get_pop_manager().get_pop_type_count()
			),
			reform_index_t(
				new_definition_manager.get_politics_manager().get_issue_manager().get_reform_count()
			),
			strata_index_t(
				new_definition_manager.get_pop_manager().get_strata_count()
			)
		},
		new_definition_manager.get_pop_manager().get_pop_types(),
		new_definition_manager.get_politics_manager().get_issue_manager().get_reform_groups(),
		new_definition_manager.get_military_manager().get_unit_type_manager().get_regiment_types(),
		new_definition_manager.get_military_manager().get_unit_type_manager().get_ship_types(),
		new_definition_manager.get_pop_manager().get_stratas(),
		new_definition_manager.get_research_manager().get_technology_manager().get_technologies(),
		new_definition_manager.get_military_manager().get_unit_type_manager()
	},
	pops_aggregate_deps {
			ideology_index_t(
				new_definition_manager.get_politics_manager().get_ideology_manager().get_ideology_count()
			),
			party_policy_index_t(
				new_definition_manager.get_politics_manager().get_issue_manager().get_party_policy_count()
			),
			pop_type_index_t(
				new_definition_manager.get_pop_manager().get_pop_type_count()
			),
			reform_index_t(
				new_definition_manager.get_politics_manager().get_issue_manager().get_reform_count()
			),
			strata_index_t(
				new_definition_manager.get_pop_manager().get_strata_count()
			)
		},
	pop_deps {
		artisanal_producer_deps,
		market_instance,
		pops_aggregate_deps
	},
	rgo_deps {
		market_instance,
		new_definition_manager.get_modifier_manager().get_modifier_effect_cache(),
		pop_type_index_t(new_definition_manager.get_pop_manager().get_pop_type_count())
	},
	province_instance_deps {
		new_definition_manager.get_economy_manager().get_building_type_manager(),
		new_game_rules_manager,
		pops_aggregate_deps,
		rgo_deps,
		new_definition_manager.get_pop_manager().get_stratas()
	},
	global_flags { "global" },
	country_instance_manager {
		new_definition_manager.get_define_manager().get_country_defines(),
		new_definition_manager.get_country_definition_manager(),
		country_instance_deps,
		good_instance_manager,
		new_definition_manager.get_define_manager().get_pops_defines(),
		new_definition_manager.get_pop_manager().get_pop_types(),
		new_definition_manager.get_military_manager().get_unit_type_manager().get_regiment_types(),
		thread_pool
	},
	trade_network {
		new_definition_manager.get_define_manager().get_openvic_defines().trade,
		country_instance_manager.get_country_instances().size()
	},
	unit_instance_manager {
		new_definition_manager.get_pop_manager().get_culture_manager(),
		new_definition_manager.get_military_manager().get_leader_trait_manager(),
		new_definition_manager.get_define_manager().get_military_defines()
	},
	politics_instance_manager {
		*this,
		new_definition_manager.get_politics_manager().get_ideology_manager().get_ideologies()
	},
	map_instance {
		new_definition_manager.get_map_definition(),
		province_instance_deps,
		thread_pool
	},
	simulation_clock {
		[this]() -> void {
			queue_game_action<tick_argument_t>();
		},
		[this]() -> void {
			execute_game_actions();
			update_gamestate();
		}
	},
	console_instance { *this },
	gamestate_updated { gamestate_updated_callback ? std::move(gamestate_updated_callback) : []() {} } {
	trade_network.set_countries(country_instance_manager.get_country_instances());
	good_instance_manager.set_trade_network(&trade_network);
	war_manager.set_defines(definition_manager.get_define_manager().get_openvic_defines());
	UnitInstanceGroup::set_war_defines(definition_manager.get_define_manager().get_openvic_defines().war);
	ArtisanalProducer::set_openvic_defines(definition_manager.get_define_manager().get_openvic_defines().economy);
	credit_manager.set_defines(definition_manager.get_define_manager().get_openvic_defines().credit);
	FactoryInstance::set_defines(definition_manager.get_define_manager().get_openvic_defines().economy);
	CountryInstance::set_openvic_economy_defines(definition_manager.get_define_manager().get_openvic_defines().economy);
	CountryInstance::set_openvic_trade_defines(definition_manager.get_define_manager().get_openvic_defines().trade);
	CountryInstance::set_openvic_supply_defines(definition_manager.get_define_manager().get_openvic_defines().supply);
	ProvinceInstance::set_openvic_supply_defines(definition_manager.get_define_manager().get_openvic_defines().supply);
	SupplyContext::set(country_instance_manager, war_manager);
	Pop::set_openvic_population_defines(definition_manager.get_define_manager().get_openvic_defines().population);
	ResourceGatheringOperation::set_openvic_trade_defines(definition_manager.get_define_manager().get_openvic_defines().trade);
}

void InstanceManager::set_gamestate_needs_update() {
	if (!currently_updating_gamestate) {
		gamestate_needs_update = true;
	} else {
		spdlog::error_s("Attempted to queue a gamestate update already updating the gamestate!");
	}
}

void InstanceManager::update_gamestate() {
	if (currently_updating_gamestate) {
		spdlog::error_s("Attempted to update gamestate while already updating gamestate!");
		return;
	}

	if (!gamestate_needs_update) {
		return;
	}

	currently_updating_gamestate = true;

	SPDLOG_INFO("Update: {}", today);

	const auto update_start = TickProfiler::clock_t::now();
	{
		OV_PROFILE_SCOPE(tick_profiler, "update_gamestate");
		{
			OV_PROFILE_SCOPE(tick_profiler, "modifier_sums");
			update_modifier_sums();
		}

		// Update gamestate...
		{
			OV_PROFILE_SCOPE(tick_profiler, "map (provinces, states)");
			map_instance.update_gamestate(*this);
		}
		{
			OV_PROFILE_SCOPE(tick_profiler, "countries");
			country_instance_manager.update_gamestate(today, map_instance);
		}
		{
			OV_PROFILE_SCOPE(tick_profiler, "units");
			unit_instance_manager.update_gamestate();
		}

		{
			// What the game does with the new state (map colours, the interface), see GameSingleton
			OV_PROFILE_SCOPE(tick_profiler, "callback (colours and interface)");
			gamestate_updated();
		}
	}
	tick_profiler.record_update(
		std::chrono::duration<double, std::milli>(TickProfiler::clock_t::now() - update_start).count()
	);
	gamestate_needs_update = false;

	currently_updating_gamestate = false;
}

/* REQUIREMENTS:
 * SS-98, SS-101
 */
void InstanceManager::tick() {

	today++;

	SPDLOG_INFO("Tick: {}", today);

	// Tick...
	const auto tick_start = TickProfiler::clock_t::now();
	{
		OV_PROFILE_SCOPE(tick_profiler, "tick");
		{ OV_PROFILE_SCOPE(tick_profiler, "money ledger begin"); money_ledger.begin_day(*this); }
		{
			// The roads of the trade: built before the first day and rebuilt at the start of every month (derived from the state of the game)
			OV_PROFILE_SCOPE(tick_profiler, "trade network");
			if (trade_network.is_enabled() && (!trade_network.is_ready() || today.is_month_start())) {
				trade_network.update(*this);
			}
			trade_network.begin_day();
		}
		{
			// The regiments get into the divisions of their armies (the countries with the technology of the organisation of the army)
			OV_PROFILE_SCOPE(tick_profiler, "divisions");
			update_all_divisions(*this);
		}
		{
			// What the supply limits of the provinces let through to the armies (one thread, before the countries work out the cost)
			OV_PROFILE_SCOPE(tick_profiler, "army supply delivery");
			update_army_supply_delivery(*this);
		}
		{
			OV_PROFILE_SCOPE(tick_profiler, "countries before the map");
			country_instance_manager.country_manager_tick_before_map();
		}
		{ OV_PROFILE_SCOPE(tick_profiler, "money ledger phases"); money_ledger.end_phase(*this, "before_map"); }
		{
			OV_PROFILE_SCOPE(tick_profiler, "map_tick (provinces, pops)");
			map_instance.map_tick();
		}
		{ OV_PROFILE_SCOPE(tick_profiler, "money ledger phases"); money_ledger.end_phase(*this, "map_tick"); }
		{
			OV_PROFILE_SCOPE(tick_profiler, "factories");
			map_instance.factory_tick(market_instance, today);
		}
		{ OV_PROFILE_SCOPE(tick_profiler, "money ledger phases"); money_ledger.end_phase(*this, "factories"); }
		{
			OV_PROFILE_SCOPE(tick_profiler, "market (orders)");
			market_instance.execute_orders();
		}
		{ OV_PROFILE_SCOPE(tick_profiler, "money ledger phases"); money_ledger.end_phase(*this, "market"); }
		{
			OV_PROFILE_SCOPE(tick_profiler, "countries after the map");
			country_instance_manager.country_manager_tick_after_map();
		}
		{ OV_PROFILE_SCOPE(tick_profiler, "money ledger phases"); money_ledger.end_phase(*this, "after_map"); }
		{
			OV_PROFILE_SCOPE(tick_profiler, "units");
			unit_instance_manager.tick(today);
		}
		{
			OV_PROFILE_SCOPE(tick_profiler, "wars");
			war_manager.tick(*this);
		}
		{
			OV_PROFILE_SCOPE(tick_profiler, "credits");
			credit_manager.tick(*this);
		}
		{
			OV_PROFILE_SCOPE(tick_profiler, "colonisation");
			colonisation_manager.tick(*this);
		}
		{
			OV_PROFILE_SCOPE(tick_profiler, "investment");
			InvestmentPlanner::tick(*this);
		}
		{ OV_PROFILE_SCOPE(tick_profiler, "money ledger phases"); money_ledger.end_phase(*this, "units_wars_credit"); }

		if (today.is_month_start()) {
			OV_PROFILE_SCOPE(tick_profiler, "monthly");
			{
				OV_PROFILE_SCOPE(tick_profiler, "price history");
				market_instance.record_price_history();
			}
			{
				OV_PROFILE_SCOPE(tick_profiler, "factory takeovers");
				map_instance.monthly_factory_takeovers(market_instance);
			}
			{
				OV_PROFILE_SCOPE(tick_profiler, "pop dynamics");
				PopDynamics::monthly_update(*this, pop_deps);
			}
			{
				OV_PROFILE_SCOPE(tick_profiler, "politics");
				PoliticsDynamics::monthly_update(*this);
			}
			{
				OV_PROFILE_SCOPE(tick_profiler, "rebels");
				RebelDynamics::monthly_update(*this);
			}
			{
				OV_PROFILE_SCOPE(tick_profiler, "research");
				ResearchDynamics::monthly_update(*this);
			}
			{
				OV_PROFILE_SCOPE(tick_profiler, "country AI");
				CountryAI::monthly_update(*this);
			}
		}
		{ OV_PROFILE_SCOPE(tick_profiler, "money ledger phases"); money_ledger.end_phase(*this, "monthly"); }
		{
			OV_PROFILE_SCOPE(tick_profiler, "money ledger (checks)");
			money_ledger.end_day(*this);
		}
	}
	tick_profiler.record_tick(std::chrono::duration<double, std::milli>(TickProfiler::clock_t::now() - tick_start).count());
}

void InstanceManager::initialise_pop_savings() {
	const std::int32_t days = definition_manager.get_define_manager().get_openvic_defines().economy.pop_start_savings_days;
	if (days <= 0) {
		return; // the random test cash of setup_pop_test_values stays
	}
	const fixed_point_t base_consciousness = definition_manager.get_define_manager().get_pops_defines().get_pdef_base_con();

	for (ProvinceInstance& province : map_instance.get_province_instances()) {
		for (Pop& pop : province.get_mutable_pops()) {
			// The same quantities as the pop asks for each day (Pop::pop_tick) at the prices of the start of the game
			const fixed_point_t scalar = (fixed_point_t::_1 + 2 * pop.get_consciousness() / base_consciousness) * type_safe::get(pop.get_size());
			fixed_point_t daily_cost = 0;
			for (auto const* needs : { &pop.get_type().get_life_needs(), &pop.get_type().get_everyday_needs() }) {
				for (auto const& [good_index, quantity] : *needs) {
					GoodInstance const* const good = good_instance_manager.get_good_instance_by_index(good_index);
					if (good != nullptr && good->good_definition.is_available_from_start) {
						daily_cost += quantity * scalar / Pop::size_denominator * good->get_price();
					}
				}
			}
			pop.set_start_cash(daily_cost * days);
		}
	}
}

void InstanceManager::execute_game_actions() {
	if (currently_executing_game_actions) {
		spdlog::error_s("Attempted to execute game actions while already executing game actions!");
		return;
	}

	if (game_action_queue.empty()) {
		return;
	}

	currently_executing_game_actions = true;

	// Temporary gamestate/UI update trigger, to be replaced with a more sophisticated targeted system
	bool needs_gamestate_update = false;

	current_batch++;
	for (game_action_t const& game_action : game_action_queue) {
		if (Replay::is_recorded(game_action)) {
			action_history.push_back({ current_batch, game_action });
		}
		needs_gamestate_update |= game_action_manager.execute_game_action(game_action);
	}

	game_action_queue.clear();

	if (needs_gamestate_update) {
		set_gamestate_needs_update();
	}

	currently_executing_game_actions = false;
}

bool InstanceManager::setup() {
	if (is_game_instance_setup()) {
		spdlog::error_s("Cannot setup game instance - already set up!");
		return false;
	}

	thread_pool.initialise_threadpool(
		game_rules_manager,
		good_instance_manager,
		definition_manager.get_modifier_manager().get_modifier_effect_cache(),
		definition_manager.get_define_manager().get_pops_defines(),
		definition_manager.get_economy_manager().get_production_type_manager(),
		strata_index_t(definition_manager.get_pop_manager().get_strata_count()),
		good_instance_manager.get_good_instances(),
		country_instance_manager.get_country_instances(),
		map_instance.get_province_instances()
	);

	game_instance_setup = true;

	return true;
}

bool InstanceManager::load_bookmark(Bookmark const& new_bookmark) {
	if (is_bookmark_loaded()) {
		spdlog::error_s("Cannot load bookmark - already loaded!");
		return false;
	}

	if (!is_game_instance_setup()) {
		spdlog::error_s("Cannot load bookmark - game instance not set up!");
		return false;
	}

	bookmark = &new_bookmark;

	SPDLOG_INFO("Loading bookmark {} with start date {}", bookmark->get_name(), bookmark->date);

	if (!definition_manager.get_define_manager().in_game_period(bookmark->date)) {
		spdlog::warn_s("Bookmark date {} is not in the game's time period!", bookmark->date);
	}

	today = bookmark->date;

	// Pop generation uses rand(): a fixed seed keeps the start of every game identical (needed for replay saves)
	std::srand(20240229);
	politics_instance_manager.setup_starting_ideologies();
	bool ret = map_instance.apply_history_to_provinces(
		definition_manager.get_history_manager().get_province_manager(), today,
		country_instance_manager,
		definition_manager.get_define_manager().get_military_defines(),
		pop_deps,
		definition_manager.get_pop_manager().get_pop_types(),
		definition_manager.get_politics_manager().get_issue_manager().get_reforms()
	);

	// It is important that province history is applied before country history as province history includes
	// generating pops which then have stats like literacy and consciousness set by country history.

	ret &= country_instance_manager.apply_history_to_countries(*this);

	// The consciousness of the pops is known now, which the needs depend on
	initialise_pop_savings();

	ret &= map_instance.get_state_manager().generate_states(
		definition_manager.get_map_definition(),
		map_instance,
		pops_aggregate_deps,
		definition_manager.get_pop_manager().get_stratas(),
		definition_manager.get_pop_manager().get_pop_types()
	);

	bool all_has_state = true;
	for (ProvinceInstance const& province_instance : map_instance.get_province_instances()) {
		if (!province_instance.province_definition.is_water() && OV_unlikely(province_instance.get_state() == nullptr)) {
			all_has_state = false;
			spdlog::error_s("Province {} has no state.", province_instance);
			continue;
		}
	}
	OV_ERR_FAIL_COND_V_MSG(!all_has_state, false, "At least one land province has no state");

	update_modifier_sums();
	map_instance.initialise_for_new_game(*this);
	// The regiments of the start of the game are put into divisions (out of the template) and the armies of the countries without the
	// technology of the organisation of the army lose their generals
	update_all_divisions(*this);
	country_instance_manager.update_gamestate(today, map_instance);
	market_instance.execute_orders();

	return ret;
}

bool InstanceManager::start_game_session() {
	if (is_game_session_started()) {
		spdlog::error_s("Cannot start game session - already started!");
		return false;
	}

	session_start = time(nullptr);
	trade_network.prepare(*this);
	simulation_clock.reset();
	set_gamestate_needs_update();

	game_session_started = true;

	return true;
}

bool InstanceManager::update_clock() {
	if (!is_game_session_started()) {
		spdlog::error_s("Cannot update clock - game session not started!");
		return false;
	}

	simulation_clock.conditionally_advance_game();
	return true;
}

void InstanceManager::force_tick_and_update() {
	simulation_clock.force_advance_game();
}

bool InstanceManager::set_today_and_update(Date new_today) {
	if (!is_game_session_started()) {
		spdlog::error_s("Cannot update clock - game session not started!");
		return false;
	}

	today = new_today;
	gamestate_needs_update = true;
	update_gamestate();
	return true;
}

void InstanceManager::update_modifier_sums() {
	// Calculate national country modifier sums first, then local province modifier sums, adding province contributions
	// to controller countries' modifier sums if each province has a controller. This results in every country having a
	// full copy of all the modifiers affecting them in their modifier sum, but provinces only having their directly/locally
	// applied modifiers in their modifier sum, hence requiring owner country modifier effect values to be looked up when
	// determining the value of a global effect on the province.
	country_instance_manager.update_modifier_sums(
		today, definition_manager.get_modifier_manager().get_static_modifier_cache()
	);
	map_instance.update_modifier_sums(
		today, definition_manager.get_modifier_manager().get_static_modifier_cache()
	);
}

bool InstanceManager::replay_actions(std::vector<ReplayEntry> const& entries, std::function<void(size_t, size_t)> progress) {
	if (!is_game_session_started()) {
		spdlog::error_s("Cannot replay actions - game session not started!");
		return false;
	}
	size_t index = 0;
	while (index < entries.size()) {
		const std::uint32_t batch = entries[index].batch;
		while (index < entries.size() && entries[index].batch == batch) {
			game_action_queue.push_back(entries[index].action);
			index++;
		}
		execute_game_actions();
		update_gamestate();
		if (progress) {
			progress(index, entries.size());
		}
	}
	return true;
}

bool InstanceManager::queue_game_action(game_action_t&& game_action) {
	if (currently_executing_game_actions) {
		spdlog::error_s("Attempted to queue a game action while already executing game actions!");
		return false;
	}

	game_action_queue.emplace_back(std::move(game_action));
	return true;
}

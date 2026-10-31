#include "GameAction.hpp"

#include "openvic-simulation/military/Unlocks.hpp"
#include <algorithm>

#include "openvic-simulation/politics/PoliticsDynamics.hpp"

#include "openvic-simulation/core/Typedefs.hpp"
#include "openvic-simulation/DefinitionManager.hpp"
#include "openvic-simulation/InstanceManager.hpp"
#include "openvic-simulation/military/ArmyGroup.hpp"
#include "openvic-simulation/military/Division.hpp"
#include "openvic-simulation/diplomacy/DiplomaticAction.hpp"
#include "openvic-simulation/economy/BuildingType.hpp"
#include "openvic-simulation/economy/credit/Credit.hpp"
#include "openvic-simulation/defines/OpenVicDefines.hpp"
#include "openvic-simulation/economy/production/FactoryInstance.hpp"
#include "openvic-simulation/economy/production/ProductionType.hpp"
#include "openvic-simulation/map/State.hpp"
#include "openvic-simulation/military/UnitInstanceGroup.hpp"
#include "openvic-simulation/military/War.hpp"
#include "openvic-simulation/map/ProvinceInstance.hpp"
#include "openvic-simulation/military/Companies.hpp"
#include "openvic-simulation/research/ArmyResearch.hpp"

using namespace OpenVic;

bool GameActionManager::VariantVisitor::operator() (none_argument_t const& argument) const {
	return false;
}

// Core
bool GameActionManager::VariantVisitor::operator() (tick_argument_t const& argument) const {
	instance_manager.tick();
	return true;
}

bool GameActionManager::VariantVisitor::operator() (set_pause_argument_t const& new_is_paused) const {
	const bool old_pause = instance_manager.get_simulation_clock().is_paused();
	instance_manager.get_simulation_clock().set_paused(type_safe::get(new_is_paused));
	return old_pause != instance_manager.get_simulation_clock().is_paused();
}

bool GameActionManager::VariantVisitor::operator() (set_speed_argument_t const& new_speed) const {
	const SimulationClock::speed_t old_speed = instance_manager.get_simulation_clock().get_simulation_speed();
	instance_manager.get_simulation_clock().set_simulation_speed(type_safe::get(new_speed));
	return old_speed != instance_manager.get_simulation_clock().get_simulation_speed();
}

bool GameActionManager::VariantVisitor::operator() (set_ai_argument_t const& argument) const {
	const auto [country_index, new_is_ai] = argument;
	CountryInstance& country = instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index);

	const bool old_ai = country.is_ai();
	country.set_ai(new_is_ai);
	return old_ai != country.is_ai();
}

// Production
bool GameActionManager::VariantVisitor::operator() (expand_province_building_argument_t const& argument) const {
	const auto [country_index, province_index, province_building_index] = argument;
	CountryInstance& country = instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index);
	
	ProvinceInstance* province = instance_manager.get_map_instance().get_province_instance_by_index(province_index);

	if (OV_unlikely(province == nullptr)) {
		spdlog::error_s("GAME_ACTION_EXPAND_PROVINCE_BUILDING called with invalid province index: {}", province_index);
		return false;
	}

	return province->expand_building(
		instance_manager.definition_manager.get_modifier_manager().get_modifier_effect_cache(),
		province_building_index,
		country
	);
}

// Budget
bool GameActionManager::VariantVisitor::operator() (set_strata_tax_argument_t const& argument) const {
	const auto [country_index, strata_index, tax_rate] = argument;
	CountryInstance& country = instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index);

	Strata const* strata = instance_manager.definition_manager.get_pop_manager().get_strata_by_index(strata_index);

	if (OV_unlikely(strata == nullptr)) {
		spdlog::error_s("GAME_ACTION_SET_STRATA_TAX called with invalid strata index: {}", strata_index);
		return false;
	}

	country.set_strata_tax_rate_slider_value(*strata, tax_rate);
	return false;
}

bool GameActionManager::VariantVisitor::operator() (set_army_spending_argument_t const& argument) const {
	const auto [country_index, spending] = argument;
	CountryInstance& country = instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index);

	country.set_army_spending_slider_value(spending);
	return false;
}

bool GameActionManager::VariantVisitor::operator() (set_navy_spending_argument_t const& argument) const {
	const auto [country_index, spending] = argument;
	CountryInstance& country = instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index);

	country.set_navy_spending_slider_value(spending);
	return false;
}

bool GameActionManager::VariantVisitor::operator() (set_construction_spending_argument_t const& argument) const {
	const auto [country_index, spending] = argument;
	CountryInstance& country = instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index);

	country.set_construction_spending_slider_value(spending);
	return false;
}

bool GameActionManager::VariantVisitor::operator() (set_education_spending_argument_t const& argument) const {
	const auto [country_index, spending] = argument;
	CountryInstance& country = instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index);

	country.set_education_spending_slider_value(spending);
	return false;
}

bool GameActionManager::VariantVisitor::operator() (set_administration_spending_argument_t const& argument) const {
	const auto [country_index, spending] = argument;
	CountryInstance& country = instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index);

	country.set_administration_spending_slider_value(spending);
	return false;
}

bool GameActionManager::VariantVisitor::operator() (set_social_spending_argument_t const& argument) const {
	const auto [country_index, spending] = argument;
	CountryInstance& country = instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index);

	country.set_social_spending_slider_value(spending);
	return false;
}

bool GameActionManager::VariantVisitor::operator() (set_military_spending_argument_t const& argument) const {
	const auto [country_index, spending] = argument;
	CountryInstance& country = instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index);

	country.set_military_spending_slider_value(spending);
	return false;
}

bool GameActionManager::VariantVisitor::operator() (set_tariff_rate_argument_t const& argument) const {
	const auto [country_index, tariff_rate] = argument;
	CountryInstance& country = instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index);

	country.set_tariff_rate_slider_value(tariff_rate);
	return false;
}

// Technology
bool GameActionManager::VariantVisitor::operator() (start_research_argument_t const& argument) const {
	const auto [country_index, technology_index] = argument;
	CountryInstance& country = instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index);

	Technology const* technology = instance_manager.definition_manager
		.get_research_manager()
		.get_technology_manager()
		.get_technology_by_index(technology_index);

	if (OV_unlikely(technology == nullptr)) {
		spdlog::error_s("GAME_ACTION_START_RESEARCH called with invalid technology index: {}", technology_index);
		return false;
	}

	Technology const* old_research = country.get_current_research_untracked();

	country.start_research(*technology, instance_manager.get_today());

	return old_research != country.get_current_research_untracked();
}

// Politics

// Population

// Trade
bool GameActionManager::VariantVisitor::operator() (set_good_automated_argument_t const& argument) const {
	const auto [country_index, good_index, new_is_automated] = argument;
	CountryInstance& country = instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index);

	GoodInstance const* good = instance_manager.get_good_instance_manager().get_good_instance_by_index(good_index);

	if (OV_unlikely(good == nullptr)) {
		spdlog::error_s("GAME_ACTION_SET_GOOD_AUTOMATED called with invalid good index: {}", good_index);
		return false;
	}

	CountryInstance::good_data_t& good_data = country.get_good_data(*good);

	const bool old_automated = good_data.is_automated;

	good_data.is_automated = new_is_automated;

	return old_automated != good_data.is_automated;
}

bool GameActionManager::VariantVisitor::operator() (set_good_trade_order_argument_t const& argument) const {
	const auto [country_index, good_index, new_is_selling, new_cutoff] = argument;
	CountryInstance& country = instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index);

	GoodInstance const* good = instance_manager.get_good_instance_manager().get_good_instance_by_index(good_index);

	if (OV_unlikely(good == nullptr)) {
		spdlog::error_s("GAME_ACTION_SET_GOOD_TRADE_ORDER called with invalid good index: {}", good_index);
		return false;
	}

	CountryInstance::good_data_t& good_data = country.get_good_data(*good);

	if (OV_unlikely(good_data.is_automated)) {
		spdlog::error_s(
			"GAME_ACTION_SET_GOOD_TRADE_ORDER called for automated good! Country: {}, good: {}",
			country, *good
		);
		return false;
	}

	const bool old_is_selling = good_data.is_selling;
	const fixed_point_t old_stockpile_cutoff = good_data.stockpile_cutoff;

	good_data.is_selling = new_is_selling;
	good_data.stockpile_cutoff = new_cutoff;

	if (good_data.stockpile_cutoff.is_negative()) {
		spdlog::error_s(
			"GAME_ACTION_SET_GOOD_TRADE_ORDER called with negative stockpile cutoff {} for {} good \"{}\" in country \"{}\". Setting to 0.",
			good_data.stockpile_cutoff,
			*good,
			good_data.is_selling ? "selling" : "buying",
			country
		);
		good_data.stockpile_cutoff = 0;
	}

	return old_is_selling != good_data.is_selling || old_stockpile_cutoff != good_data.stockpile_cutoff;
}

// Diplomacy

// Military
bool GameActionManager::VariantVisitor::operator() (create_leader_argument_t const& argument) const {
	const auto [country_index, unit_branch] = argument;
	CountryInstance& country = instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index);

	if (country.get_create_leader_count() < 1) {
		spdlog::error_s(
			"GAME_ACTION_CREATE_LEADER called for country \"{}\" without enough leadership points ({:.2}) to create any leaders!",
			country, country.get_leadership_point_stockpile()
		);
		return false;
	}

	return instance_manager.get_unit_instance_manager().create_leader(
		country,
		unit_branch,
		instance_manager.get_today()
	);
}

bool GameActionManager::VariantVisitor::operator() (set_use_leader_argument_t const& argument) const {
	const auto [unique_id, new_should_use] = argument;

	LeaderInstance* leader = instance_manager.get_unit_instance_manager().get_leader_instance_by_unique_id(unique_id);

	if (OV_unlikely(leader == nullptr)) {
		spdlog::error_s("GAME_ACTION_SET_USE_LEADER called with invalid leader unique id: {}", unique_id);
		return false;
	}

	const bool old_use = leader->get_can_be_used();

	leader->set_can_be_used(new_should_use);

	return old_use != leader->get_can_be_used();
}

bool GameActionManager::VariantVisitor::operator() (set_auto_create_leaders_argument_t const& argument) const {
	const auto [country_index, new_should_auto_create] = argument;
	CountryInstance& country = instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index);

	const bool old_auto_create = country.get_auto_create_leaders();
	country.set_auto_create_leaders(new_should_auto_create);
	return old_auto_create != country.get_auto_create_leaders();
}

bool GameActionManager::VariantVisitor::operator() (set_auto_assign_leaders_argument_t const& argument) const {
	const auto [country_index, new_should_auto_assign] = argument;
	CountryInstance& country = instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index);

	const bool old_auto_assign = country.get_auto_assign_leaders();
	country.set_auto_assign_leaders(new_should_auto_assign);
	return old_auto_assign != country.get_auto_assign_leaders();
}

bool GameActionManager::VariantVisitor::operator() (set_mobilise_argument_t const& argument) const {
	const auto [country_index, new_is_mobilised] = argument;
	CountryInstance& country = instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index);

	if (new_is_mobilised && !country.has_unlock(UNLOCK_MECHANIC_MOBILISATION)) {
		return false;
	}

	const bool old_mobilise = country.is_mobilised();
	country.set_mobilised(new_is_mobilised);
	return old_mobilise != country.is_mobilised();
}

bool GameActionManager::VariantVisitor::operator() (start_land_unit_recruitment_argument_t const& argument) const {
	const auto [regiment_type_index, province_index, pop_id_in_province] = argument;

	RegimentType const* const regiment_type = instance_manager.definition_manager
		.get_military_manager()
		.get_unit_type_manager()
		.get_regiment_type_by_index(regiment_type_index);
	if (OV_unlikely(regiment_type == nullptr)) {
		spdlog::error_s("GAME_ACTION_START_LAND_UNIT_RECRUITMENT called with invalid regiment type index: {}", regiment_type_index);
		return false;
	}

	ProvinceInstance* province = instance_manager
		.get_map_instance()
		.get_province_instance_by_index(province_index);
	if (OV_unlikely(province == nullptr)) {
		spdlog::error_s("GAME_ACTION_START_LAND_UNIT_RECRUITMENT called with invalid province index: {}", province_index);
		return false;
	}

	Pop* pop = province->find_pop_by_id(pop_id_in_province);
	if (OV_unlikely(pop == nullptr)) {
		spdlog::error_s("GAME_ACTION_START_LAND_UNIT_RECRUITMENT called with invalid pop_id_in_province: {}", pop_id_in_province);
		return false;
	}

	CountryInstance* const country = province->get_owner();
	if (OV_unlikely(country == nullptr)) {
		return false;
	}

	return instance_manager.get_unit_instance_manager().start_recruitment(*country, *regiment_type, *province, *pop);
}

// Diplomacy
bool GameActionManager::VariantVisitor::operator() (diplomatic_action_argument_t const& argument) const {
	const auto [sender_index, receiver_index, action_type_index] = argument;
	CountryInstanceManager& country_manager = instance_manager.get_country_instance_manager();

	return instance_manager.definition_manager.get_diplomatic_action_manager().execute_diplomatic_action(
		action_type_index,
		&country_manager.get_country_instance_by_index(sender_index),
		&country_manager.get_country_instance_by_index(receiver_index),
		{},
		instance_manager
	);
}

// Wars and armies
namespace {
	CountryInstance& war_action_country(InstanceManager& instance_manager, const country_index_t index) {
		return instance_manager.get_country_instance_manager().get_country_instance_by_index(index);
	}
}

bool GameActionManager::VariantVisitor::operator() (move_army_argument_t const& argument) const {
	const auto [unique_id, province_index] = argument;

	UnitInstanceGroup* const group = instance_manager.get_unit_instance_manager().get_unit_instance_group_by_unique_id(unique_id);
	if (OV_unlikely(group == nullptr)) {
		spdlog::error_s("GAME_ACTION_MOVE_ARMY called with invalid army unique id: {}", unique_id);
		return false;
	}

	ProvinceInstance* const province = instance_manager.get_map_instance().get_province_instance_by_index(province_index);
	if (OV_unlikely(province == nullptr)) {
		spdlog::error_s("GAME_ACTION_MOVE_ARMY called with invalid province index: {}", province_index);
		return false;
	}

	// A manual order takes the army off its front so that the front does not send it back
	for (Front const& front : instance_manager.get_war_manager().get_fronts()) {
		if (front.country == &group->get_country()) {
			instance_manager.get_war_manager().set_front_army(
				instance_manager, group->get_country(), front.id, unique_id, false
			);
			break;
		}
	}

	return instance_manager.get_unit_instance_manager().order_move(*group, *province, instance_manager.get_map_instance());
}

bool GameActionManager::VariantVisitor::operator() (queue_army_move_argument_t const& argument) const {
	const auto [unique_id, province_index] = argument;

	UnitInstanceGroup* const group = instance_manager.get_unit_instance_manager().get_unit_instance_group_by_unique_id(unique_id);
	if (OV_unlikely(group == nullptr)) {
		spdlog::error_s("GAME_ACTION_QUEUE_ARMY_MOVE called with invalid army unique id: {}", unique_id);
		return false;
	}

	ProvinceInstance* const province = instance_manager.get_map_instance().get_province_instance_by_index(province_index);
	if (OV_unlikely(province == nullptr)) {
		spdlog::error_s("GAME_ACTION_QUEUE_ARMY_MOVE called with invalid province index: {}", province_index);
		return false;
	}

	// An order given by hand takes the army off its front
	for (Front const& front : instance_manager.get_war_manager().get_fronts()) {
		if (front.country == &group->get_country()) {
			instance_manager.get_war_manager().set_front_army(
				instance_manager, group->get_country(), front.id, unique_id, false
			);
			break;
		}
	}

	return instance_manager.get_unit_instance_manager().order_move(*group, *province, instance_manager.get_map_instance(), true);
}

bool GameActionManager::VariantVisitor::operator() (stop_army_argument_t const& argument) const {
	const unique_id_t unique_id = type_safe::get(argument);

	UnitInstanceGroup* const group = instance_manager.get_unit_instance_manager().get_unit_instance_group_by_unique_id(unique_id);
	if (OV_unlikely(group == nullptr)) {
		spdlog::error_s("GAME_ACTION_STOP_ARMY called with invalid army unique id: {}", unique_id);
		return false;
	}

	if (!group->is_moving()) {
		return false;
	}
	group->clear_path();
	return true;
}

bool GameActionManager::VariantVisitor::operator() (propose_peace_argument_t const& argument) const {
	const auto [country_index, war_id, peace_type] = argument;

	if (OV_unlikely(peace_type > static_cast<std::uint32_t>(WarManager::peace_type_t::GIVE_STATES))) {
		spdlog::error_s("GAME_ACTION_PROPOSE_PEACE called with invalid peace type: {}", peace_type);
		return false;
	}

	return instance_manager.get_war_manager().propose_peace(
		instance_manager, war_id, war_action_country(instance_manager, country_index),
		static_cast<WarManager::peace_type_t>(peace_type)
	);
}

bool GameActionManager::VariantVisitor::operator() (create_front_argument_t const& argument) const {
	const auto [country_index, enemy_index] = argument;
	CountryInstance& country = war_action_country(instance_manager, country_index);
	CountryInstance& enemy = war_action_country(instance_manager, enemy_index);

	// The same country as the enemy creates an empty front to draw by hand
	return instance_manager.get_war_manager().create_front(
		instance_manager, country, &enemy != &country ? &enemy : nullptr
	) != 0;
}

bool GameActionManager::VariantVisitor::operator() (delete_front_argument_t const& argument) const {
	const auto [country_index, front_id] = argument;
	return instance_manager.get_war_manager().delete_front(war_action_country(instance_manager, country_index), front_id);
}

bool GameActionManager::VariantVisitor::operator() (toggle_front_province_argument_t const& argument) const {
	const auto [country_index, front_id, province_index] = argument;

	ProvinceInstance* const province = instance_manager.get_map_instance().get_province_instance_by_index(province_index);
	if (OV_unlikely(province == nullptr)) {
		spdlog::error_s("GAME_ACTION_TOGGLE_FRONT_PROVINCE called with invalid province index: {}", province_index);
		return false;
	}

	return instance_manager.get_war_manager().toggle_front_province(
		war_action_country(instance_manager, country_index), front_id, *province
	);
}

bool GameActionManager::VariantVisitor::operator() (set_front_mode_argument_t const& argument) const {
	const auto [country_index, front_id, mode] = argument;
	return instance_manager.get_war_manager().set_front_mode(
		war_action_country(instance_manager, country_index), front_id, static_cast<Front::mode_t>(mode)
	);
}

bool GameActionManager::VariantVisitor::operator() (set_front_army_argument_t const& argument) const {
	const auto [country_index, front_id, army_id, assigned] = argument;
	return instance_manager.get_war_manager().set_front_army(
		instance_manager, war_action_country(instance_manager, country_index), front_id, army_id, assigned
	);
}

bool GameActionManager::VariantVisitor::operator() (assign_free_armies_argument_t const& argument) const {
	const auto [country_index, front_id] = argument;
	return instance_manager.get_war_manager().assign_free_armies(
		instance_manager, war_action_country(instance_manager, country_index), front_id
	);
}

bool GameActionManager::VariantVisitor::operator() (set_auto_plan_argument_t const& argument) const {
	const auto [country_index, enabled] = argument;
	CountryInstance& country = war_action_country(instance_manager, country_index);

	const bool old_enabled = instance_manager.get_war_manager().is_auto_plan_enabled(country);
	instance_manager.get_war_manager().set_auto_plan(country, enabled);
	return old_enabled != enabled;
}

// Factories
namespace {
	// Finds the factory building type and the state to build in, null if the action is not valid
	struct FactoryActionTarget {
		State* state = nullptr;
		BuildingType const* building_type = nullptr;
	};

	FactoryActionTarget find_factory_action_target(
		InstanceManager& instance_manager, CountryInstance& country, const province_index_t province_index,
		const building_type_index_t building_type_index
	) {
		FactoryActionTarget target;

		ProvinceInstance* province = instance_manager.get_map_instance().get_province_instance_by_index(province_index);
		if (OV_unlikely(province == nullptr)) {
			spdlog::error_s("Factory game action called with invalid province index: {}", province_index);
			return target;
		}

		State* state = province->get_state();
		if (state == nullptr || state->get_owner() != &country) {
			return target;
		}

		BuildingType const* building_type = instance_manager.definition_manager.get_economy_manager()
			.get_building_type_manager()
			.get_building_type_by_index(building_type_index);
		if (OV_unlikely(building_type == nullptr)) {
			spdlog::error_s("Factory game action called with invalid building type index: {}", building_type_index);
			return target;
		}

		ProductionType const* production_type = building_type->production_type;
		if (production_type == nullptr || production_type->template_type != ProductionType::template_type_t::FACTORY) {
			return target;
		}

		target.state = state;
		target.building_type = building_type;
		return target;
	}
}

bool GameActionManager::VariantVisitor::operator() (build_factory_argument_t const& argument) const {
	const auto [country_index, province_index, building_type_index] = argument;
	CountryInstance& country = instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index);

	const FactoryActionTarget target = find_factory_action_target(instance_manager, country, province_index, building_type_index);
	if (target.state == nullptr) {
		return false;
	}

	State& state = *target.state;
	BuildingType const& building_type = *target.building_type;

	if (!country.is_building_type_unlocked(building_type) || !building_type.production_type->is_valid_for_factory_in(state)) {
		return false;
	}

	FactoryInstance* factory = state.find_factory(building_type);
	if (factory != nullptr) {
		if (!factory->can_be_expanded()) {
			return false;
		}
	} else if (!state.has_free_factory_slot()) {
		return false;
	}

	// The player pays the money part of the cost at once, the goods are taken from the national stockpile while building
	const fixed_point_t cost = building_type.cost;
	if (country.get_cash_stockpile().load() < cost) {
		return false;
	}

	if (factory == nullptr) {
		factory = state.add_factory(building_type, building_level_t(0));
		if (factory != nullptr) {
			// A new factory of weapons makes the newest generation the country has (it can be changed)
			if (ProductionType const* const best = get_best_production_type(instance_manager, country, building_type); best != nullptr) {
				factory->request_production_type(*best);
			}
		}
		if (factory == nullptr) {
			return false;
		}
		// A factory built by the player belongs to the state
		factory->set_state_share(fixed_point_t::_1);
		factory->set_built_by_state(true);
	}

	country.spend_cash(cost);
	factory->start_construction(instance_manager.get_today(), true);
	return true;
}

bool GameActionManager::VariantVisitor::operator() (set_factory_subsidised_argument_t const& argument) const {
	const auto [country_index, province_index, building_type_index, new_subsidised] = argument;
	CountryInstance& country = instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index);

	const FactoryActionTarget target = find_factory_action_target(instance_manager, country, province_index, building_type_index);
	if (target.state == nullptr) {
		return false;
	}

	FactoryInstance* factory = target.state->find_factory(*target.building_type);
	if (factory == nullptr || factory->is_subsidised() == new_subsidised) {
		return false;
	}

	factory->set_subsidised(new_subsidised);
	return true;
}

bool GameActionManager::VariantVisitor::operator() (set_all_factories_open_argument_t const& argument) const {
	const auto [country_index, new_open] = argument;
	CountryInstance& country = instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index);

	bool changed = false;
	for (State* state : country.get_states()) {
		for (FactoryInstance& factory : state->get_mutable_factories()) {
			if (factory.get_level() > building_level_t(0) && factory.is_open() != new_open) {
				factory.set_open(new_open);
				changed = true;
			}
		}
	}
	return changed;
}

bool GameActionManager::VariantVisitor::operator() (set_all_factories_subsidised_argument_t const& argument) const {
	const auto [country_index, new_subsidised] = argument;
	CountryInstance& country = instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index);

	bool changed = false;
	for (State* state : country.get_states()) {
		for (FactoryInstance& factory : state->get_mutable_factories()) {
			if (factory.is_subsidised() != new_subsidised) {
				factory.set_subsidised(new_subsidised);
				changed = true;
			}
		}
	}
	return changed;
}

namespace {
	bool trade_factory_share(
		InstanceManager& instance_manager, const country_index_t country_index, const province_index_t province_index,
		const building_type_index_t building_type_index, const std::uint32_t percent, const bool state_buys
	) {
		CountryInstance& country = instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index);
		const FactoryActionTarget target = find_factory_action_target(instance_manager, country, province_index, building_type_index);
		if (target.state == nullptr || percent == 0 || percent > 100) {
			return false;
		}
		FactoryInstance* factory = target.state->find_factory(*target.building_type);
		if (factory == nullptr) {
			return false;
		}
		return factory->trade_share(
			*target.state, country, instance_manager.get_market_instance(),
			fixed_point_t(static_cast<int32_t>(percent)) / 100, state_buys
		);
	}
}

bool GameActionManager::VariantVisitor::operator() (buy_factory_share_argument_t const& argument) const {
	const auto [country_index, province_index, building_type_index, percent] = argument;
	return trade_factory_share(instance_manager, country_index, province_index, building_type_index, percent, true);
}

bool GameActionManager::VariantVisitor::operator() (sell_factory_share_argument_t const& argument) const {
	const auto [country_index, province_index, building_type_index, percent] = argument;
	return trade_factory_share(instance_manager, country_index, province_index, building_type_index, percent, false);
}

// Credit and the central bank (docs/CREDITS.md)
namespace {
	bool is_valid_loan_term(const std::uint32_t term) {
		return term <= static_cast<std::uint32_t>(Loan::term_t::LONG);
	}
}

bool GameActionManager::VariantVisitor::operator() (stage_credit_country_argument_t const& argument) const {
	const auto [lender_index, target_index, selected] = argument;
	CountryInstanceManager& country_manager = instance_manager.get_country_instance_manager();
	instance_manager.get_credit_manager().stage_target_country(
		country_manager.get_country_instance_by_index(lender_index), country_manager.get_country_instance_by_index(target_index),
		selected
	);
	return true;
}

bool GameActionManager::VariantVisitor::operator() (stage_credit_region_argument_t const& argument) const {
	const auto [lender_index, province_index, selected] = argument;
	ProvinceInstance* const province = instance_manager.get_map_instance().get_province_instance_by_index(province_index);
	if (OV_unlikely(province == nullptr || province->get_state() == nullptr)) {
		return false;
	}
	instance_manager.get_credit_manager().stage_target_state(
		instance_manager.get_country_instance_manager().get_country_instance_by_index(lender_index), *province->get_state(), selected
	);
	return true;
}

bool GameActionManager::VariantVisitor::operator() (offer_internal_credit_argument_t const& argument) const {
	const auto [lender_index, amount, rate, term, all_regions] = argument;
	if (OV_unlikely(!is_valid_loan_term(term))) {
		return false;
	}
	CountryInstance& lender = instance_manager.get_country_instance_manager().get_country_instance_by_index(lender_index);
	return instance_manager.get_credit_manager().offer_internal_credit(
		instance_manager, lender, amount, rate, static_cast<Loan::term_t>(term), all_regions
	) != nullptr;
}

bool GameActionManager::VariantVisitor::operator() (offer_external_credit_argument_t const& argument) const {
	const auto [lender_index, amount, rate, term, all_countries] = argument;
	if (OV_unlikely(!is_valid_loan_term(term))) {
		return false;
	}
	CountryInstance& lender = instance_manager.get_country_instance_manager().get_country_instance_by_index(lender_index);
	return instance_manager.get_credit_manager().offer_external_credit(
		instance_manager, lender, amount, rate, static_cast<Loan::term_t>(term), all_countries
	) != nullptr;
}

bool GameActionManager::VariantVisitor::operator() (withdraw_credit_offer_argument_t const& argument) const {
	const auto [lender_index, loan_id] = argument;
	return instance_manager.get_credit_manager().withdraw_offer(
		instance_manager, instance_manager.get_country_instance_manager().get_country_instance_by_index(lender_index), loan_id
	);
}

bool GameActionManager::VariantVisitor::operator() (take_credit_argument_t const& argument) const {
	const auto [borrower_index, loan_id] = argument;
	return instance_manager.get_credit_manager().take_credit(
		instance_manager, instance_manager.get_country_instance_manager().get_country_instance_by_index(borrower_index), loan_id
	);
}

bool GameActionManager::VariantVisitor::operator() (repay_loan_argument_t const& argument) const {
	const auto [country_index, loan_id, amount] = argument;
	return instance_manager.get_credit_manager().repay_loan_by_id(
		instance_manager, instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index),
		loan_id, amount
	);
}

bool GameActionManager::VariantVisitor::operator() (set_central_bank_rates_argument_t const& argument) const {
	const auto [country_index, internal_rate, external_rate] = argument;
	CountryInstance& country = instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index);
	// The rates the AI of the country offers its credits at: never below the minimum interest, never above the limit
	OpenVicCreditDefines const& credit_defines = instance_manager.get_credit_manager().get_defines();
	const fixed_point_t lowest = credit_defines.interest_min.fixed();
	const fixed_point_t highest = credit_defines.interest_limit.fixed();
	country.set_central_bank_rates(std::clamp(internal_rate, lowest, highest), std::clamp(external_rate, lowest, highest));
	return true;
}

bool GameActionManager::VariantVisitor::operator() (set_subsidy_mode_argument_t const& argument) const {
	const auto [country_index, mode] = argument;
	if (OV_unlikely(mode > static_cast<std::uint32_t>(CountryInstance::subsidy_mode_t::HARD_CRISIS))) {
		spdlog::error_s("GAME_ACTION_SET_SUBSIDY_MODE called with invalid mode: {}", mode);
		return false;
	}

	CountryInstance& country = instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index);
	const auto new_mode = static_cast<CountryInstance::subsidy_mode_t>(mode);
	if (country.get_subsidy_mode() == new_mode) {
		return false;
	}
	country.set_subsidy_mode(new_mode);
	return true;
}

bool GameActionManager::VariantVisitor::operator() (expand_all_factories_argument_t const& argument) const {
	CountryInstance& country = instance_manager.get_country_instance_manager().get_country_instance_by_index(
		type_safe::get(argument)
	);

	bool changed = false;
	for (State* state : country.get_states()) {
		for (FactoryInstance& factory : state->get_mutable_factories()) {
			if (!factory.can_be_expanded() || !country.is_building_type_unlocked(factory.building_type)) {
				continue;
			}
			// Like the expansion of a single factory: the money part is paid at once, the goods come from the stockpile
			const fixed_point_t cost = factory.building_type.cost;
			if (country.get_cash_stockpile().load() < cost) {
				continue;
			}
			country.spend_cash(cost);
			factory.start_construction(instance_manager.get_today(), true);
			changed = true;
		}
	}
	return changed;
}

bool GameActionManager::VariantVisitor::operator() (set_factory_open_argument_t const& argument) const {
	const auto [country_index, province_index, building_type_index, new_open] = argument;
	CountryInstance& country = instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index);

	const FactoryActionTarget target = find_factory_action_target(instance_manager, country, province_index, building_type_index);
	if (target.state == nullptr) {
		return false;
	}

	FactoryInstance* factory = target.state->find_factory(*target.building_type);
	if (factory == nullptr || factory->is_open() == new_open) {
		return false;
	}

	factory->set_open(new_open);
	return true;
}

bool GameActionManager::VariantVisitor::operator() (set_factory_priority_argument_t const& argument) const {
	const auto [country_index, province_index, building_type_index, priority] = argument;
	if (OV_unlikely(priority > 2)) {
		return false;
	}
	CountryInstance& country = instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index);

	const FactoryActionTarget target = find_factory_action_target(instance_manager, country, province_index, building_type_index);
	if (target.state == nullptr) {
		return false;
	}

	FactoryInstance* factory = target.state->find_factory(*target.building_type);
	if (factory == nullptr || factory->get_priority() == priority) {
		return false;
	}

	factory->set_priority(priority);
	return true;
}

bool GameActionManager::VariantVisitor::operator() (set_factory_production_type_argument_t const& argument) const {
	const auto [country_index, province_index, building_type_index, option_index] = argument;
	CountryInstance& country = instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index);

	const FactoryActionTarget target = find_factory_action_target(instance_manager, country, province_index, building_type_index);
	if (target.state == nullptr) {
		return false;
	}

	FactoryInstance* factory = target.state->find_factory(*target.building_type);
	if (factory == nullptr) {
		return false;
	}

	// The options of the building (what it can make) and the ones the country has the unlocks for
	const memory::vector<FactoryProductionOption> options = get_factory_production_options(instance_manager, *target.building_type);
	if (option_index >= options.size() || !is_production_option_available(country, options[option_index])) {
		return false;
	}
	ProductionType const* const wanted = options[option_index].production_type;
	if (wanted == factory->production_type && factory->get_pending_production_type() == nullptr) {
		return false;
	}
	factory->request_production_type(*wanted);
	return true;
}

bool GameActionManager::VariantVisitor::operator() (delete_factory_argument_t const& argument) const {
	const auto [country_index, province_index, building_type_index] = argument;
	CountryInstance& country = instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index);

	const FactoryActionTarget target = find_factory_action_target(instance_manager, country, province_index, building_type_index);
	if (target.state == nullptr) {
		return false;
	}

	FactoryInstance* factory = target.state->find_factory(*target.building_type);
	return factory != nullptr && target.state->remove_factory(*factory);
}

// Army management
namespace {
	// The land regiment and the army that it is in
	RegimentInstance* find_regiment(InstanceManager& instance_manager, const unique_id_t regiment_id, ArmyInstance*& army_out) {
		UnitInstance* const unit = instance_manager.get_unit_instance_manager().get_unit_instance_by_unique_id(regiment_id);
		if (unit == nullptr || unit->get_branch() != unit_branch_t::LAND) {
			return nullptr;
		}
		RegimentInstance& regiment = static_cast<RegimentInstance&>(*unit);
		army_out = instance_manager.get_unit_instance_manager().get_army_of_regiment(regiment);
		return army_out != nullptr ? &regiment : nullptr;
	}

	ArmyInstance* find_army(InstanceManager& instance_manager, const unique_id_t army_id) {
		UnitInstanceGroup* const group = instance_manager.get_unit_instance_manager().get_unit_instance_group_by_unique_id(army_id);
		if (group == nullptr || group->branch != unit_branch_t::LAND) {
			return nullptr;
		}
		return static_cast<ArmyInstance*>(group);
	}
}

bool GameActionManager::VariantVisitor::operator() (cancel_recruitment_argument_t const& argument) const {
	const auto [country_index, order_id] = argument;
	return instance_manager.get_unit_instance_manager().cancel_recruitment(
		war_action_country(instance_manager, country_index), order_id
	);
}

bool GameActionManager::VariantVisitor::operator() (split_regiment_argument_t const& argument) const {
	ArmyInstance* army = nullptr;
	RegimentInstance* const regiment = find_regiment(instance_manager, type_safe::get(argument), army);
	if (regiment == nullptr || army->is_in_combat()) {
		return false;
	}
	return instance_manager.get_unit_instance_manager().split_regiment(*army, *regiment) != nullptr;
}

bool GameActionManager::VariantVisitor::operator() (merge_armies_argument_t const& argument) const {
	const auto [target_id, source_id] = argument;
	ArmyInstance* const target = find_army(instance_manager, target_id);
	ArmyInstance* const source = find_army(instance_manager, source_id);
	if (target == nullptr || source == nullptr || target->is_in_combat() || source->is_in_combat()) {
		return false;
	}
	return instance_manager.get_unit_instance_manager().merge_armies(*target, *source);
}

bool GameActionManager::VariantVisitor::operator() (set_army_leader_argument_t const& argument) const {
	const auto [army_id, leader_id] = argument;
	// Generals lead armies, admirals lead fleets
	UnitInstanceGroup* const group = instance_manager.get_unit_instance_manager().get_unit_instance_group_by_unique_id(army_id);
	if (group == nullptr) {
		return false;
	}
	// Leader 0 takes the leader off the group
	if (leader_id == 0) {
		return group->get_leader() != nullptr && group->set_leader(nullptr);
	}
	LeaderInstance* const leader = instance_manager.get_unit_instance_manager().get_leader_instance_by_unique_id(leader_id);
	if (leader == nullptr || leader->branch != group->branch || &leader->country != &group->get_country() || !leader->get_can_be_used()) {
		return false;
	}
	// Generals command the armies of the countries with the technology of the organisation of the army only
	if (group->branch == unit_branch_t::LAND && !group->get_country().has_army_organisation()) {
		return false;
	}
	return group->set_leader(leader);
}

// The priority of the supply of one army: 0 standard, 1 increased, 2 maximum; 3 takes the own priority off (the one of the country applies)
bool GameActionManager::VariantVisitor::operator() (set_army_supply_priority_argument_t const& argument) const {
	const auto [army_id, value] = argument;
	ArmyInstance* const army = find_army(instance_manager, army_id);
	if (army == nullptr || value > 3) {
		return false;
	}
	if (value == 3) {
		const bool changed = army->is_supply_priority_custom();
		army->set_supply_priority_custom(false);
		army->set_supply_priority(supply_priority_t::STANDARD);
		return changed;
	}
	const supply_priority_t priority = static_cast<supply_priority_t>(value);
	if (army->is_supply_priority_custom() && army->get_supply_priority() == priority) {
		return false;
	}
	army->set_supply_priority_custom(true);
	army->set_supply_priority(priority);
	return true;
}

// The priority of the armies of a country that have no priority of their own
// Templates of the divisions: the number of regiments of the kinds infantry, cavalry, support, special
bool GameActionManager::VariantVisitor::operator() (create_division_template_argument_t const& argument) const {
	const auto [country_index, grid] = argument;
	CountryInstance& country = instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index);
	if (!country.has_unlock(UNLOCK_MECHANIC_DIVISION_TEMPLATES)) {
		return false;
	}
	return create_division_template(instance_manager, country, grid);
}

bool GameActionManager::VariantVisitor::operator() (update_division_template_argument_t const& argument) const {
	const auto [country_index, template_id, grid] = argument;
	CountryInstance& country = instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index);
	if (!country.has_unlock(UNLOCK_MECHANIC_DIVISION_TEMPLATES)) {
		return false;
	}
	return update_division_template(instance_manager, country, template_id, grid);
}

bool GameActionManager::VariantVisitor::operator() (delete_division_template_argument_t const& argument) const {
	const auto [country_index, template_id] = argument;
	CountryInstance& country = instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index);
	if (!country.has_unlock(UNLOCK_MECHANIC_DIVISION_TEMPLATES)) {
		return false;
	}
	return delete_division_template(country, template_id);
}

// The regiments of the army that are out of the template (of one division, if its id is not 0) make divisions of the template
bool GameActionManager::VariantVisitor::operator() (reform_army_to_template_argument_t const& argument) const {
	const auto [army_id, division_id, template_id] = argument;
	ArmyInstance* const army = find_army(instance_manager, army_id);
	return army != nullptr && reform_army_to_template(instance_manager, *army, division_id, template_id);
}

// Groups of armies and field marshals
bool GameActionManager::VariantVisitor::operator() (create_army_group_argument_t const& argument) const {
	CountryInstance& country = instance_manager.get_country_instance_manager().get_country_instance_by_index(type_safe::get(argument));
	return create_army_group(country);
}

bool GameActionManager::VariantVisitor::operator() (delete_army_group_argument_t const& argument) const {
	const auto [country_index, group_id] = argument;
	CountryInstance& country = instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index);
	return delete_army_group(instance_manager, country, group_id);
}

bool GameActionManager::VariantVisitor::operator() (set_army_group_argument_t const& argument) const {
	const auto [army_id, group_id] = argument;
	ArmyInstance* const army = find_army(instance_manager, army_id);
	return army != nullptr && set_army_group(
		army->get_country(), *army, group_id,
		static_cast<std::uint32_t>(instance_manager.definition_manager.get_define_manager().get_openvic_defines().war.marshal_max_attached)
	);
}

bool GameActionManager::VariantVisitor::operator() (set_army_group_marshal_argument_t const& argument) const {
	const auto [country_index, group_id, leader_id] = argument;
	CountryInstance& country = instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index);
	return set_army_group_marshal(instance_manager, country, group_id, leader_id);
}

bool GameActionManager::VariantVisitor::operator() (set_global_supply_priority_argument_t const& argument) const {
	const auto [country_index, value] = argument;
	if (value > 2) {
		return false;
	}
	CountryInstance& country = instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index);
	const supply_priority_t priority = static_cast<supply_priority_t>(value);
	if (country.get_army_supply_priority() == priority) {
		return false;
	}
	country.set_army_supply_priority(priority);
	return true;
}

bool GameActionManager::VariantVisitor::operator() (disband_regiment_argument_t const& argument) const {
	ArmyInstance* army = nullptr;
	RegimentInstance* const regiment = find_regiment(instance_manager, type_safe::get(argument), army);
	if (regiment == nullptr || army->is_in_combat()) {
		return false;
	}
	instance_manager.get_unit_instance_manager().disband_regiment(*army, *regiment);
	return true;
}

bool GameActionManager::VariantVisitor::operator() (disband_army_argument_t const& argument) const {
	ArmyInstance* const army = find_army(instance_manager, type_safe::get(argument));
	if (army == nullptr || army->is_in_combat()) {
		return false;
	}
	// The front keeps no reference to the disbanded army
	for (Front const& front : instance_manager.get_war_manager().get_fronts()) {
		if (front.country == &army->get_country()) {
			instance_manager.get_war_manager().set_front_army(
				instance_manager, army->get_country(), front.id, army->unique_id, false
			);
			break;
		}
	}
	instance_manager.get_unit_instance_manager().destroy_group(*army);
	return true;
}

// Ships and transport
bool GameActionManager::VariantVisitor::operator() (load_army_argument_t const& argument) const {
	const auto [army_id, navy_id] = argument;
	ArmyInstance* const army = find_army(instance_manager, army_id);
	UnitInstanceGroup* const group = instance_manager.get_unit_instance_manager().get_unit_instance_group_by_unique_id(navy_id);
	if (army == nullptr || group == nullptr || group->branch != unit_branch_t::NAVAL) {
		return false;
	}
	return static_cast<NavyInstance*>(group)->load_army(*army);
}

bool GameActionManager::VariantVisitor::operator() (unload_army_argument_t const& argument) const {
	ArmyInstance* const army = find_army(instance_manager, type_safe::get(argument));
	if (army == nullptr || army->get_transport_navy() == nullptr) {
		return false;
	}
	return const_cast<NavyInstance*>(army->get_transport_navy())->unload_army(*army);
}

bool GameActionManager::VariantVisitor::operator() (land_army_argument_t const& argument) const {
	const auto [army_id, province_index] = argument;
	ArmyInstance* const army = find_army(instance_manager, army_id);
	ProvinceInstance* const province = instance_manager.get_map_instance().get_province_instance_by_index(province_index);
	if (army == nullptr || province == nullptr || army->get_transport_navy() == nullptr) {
		return false;
	}
	return const_cast<NavyInstance*>(army->get_transport_navy())->land_army(*army, *province);
}

bool GameActionManager::VariantVisitor::operator() (start_ship_building_argument_t const& argument) const {
	const auto [ship_type_index, province_index] = argument;
	ShipType const* const ship_type = instance_manager.definition_manager
		.get_military_manager()
		.get_unit_type_manager()
		.get_ship_type_by_index(ship_type_index);
	ProvinceInstance* const province = instance_manager.get_map_instance().get_province_instance_by_index(province_index);
	if (OV_unlikely(ship_type == nullptr || province == nullptr)) {
		return false;
	}
	CountryInstance* const country = province->get_owner();
	if (country == nullptr) {
		return false;
	}
	return instance_manager.get_unit_instance_manager().start_ship_building(*country, *ship_type, *province);
}

// Peace negotiations
bool GameActionManager::VariantVisitor::operator() (declare_war_goal_argument_t const& argument) const {
	const auto [attacker_index, defender_index, goal_type, province_index] = argument;
	CountryInstance& attacker = war_action_country(instance_manager, attacker_index);
	CountryInstance& defender = war_action_country(instance_manager, defender_index);

	WarGoal goal;
	if (goal_type > static_cast<std::uint32_t>(WarGoal::type_t::REPARATIONS)) {
		return false;
	}
	goal.type = static_cast<WarGoal::type_t>(goal_type);
	if (goal.type == WarGoal::type_t::CONQUER_STATE) {
		goal.state_province = instance_manager.get_map_instance().get_province_instance_by_index(province_index);
		if (goal.state_province == nullptr) {
			return false;
		}
	}
	return instance_manager.get_war_manager().declare_war(instance_manager, attacker, defender, goal);
}

bool GameActionManager::VariantVisitor::operator() (set_peace_demand_argument_t const& argument) const {
	const auto [country_index, war_id, kind, province_index, enabled] = argument;
	CountryInstance& country = war_action_country(instance_manager, country_index);
	WarManager& manager = instance_manager.get_war_manager();

	if (manager.get_war_by_id(war_id) == nullptr) {
		return false;
	}
	PeaceTerms& draft = manager.get_peace_draft(country, war_id);

	switch (kind) {
	case 0: { // a state
		ProvinceInstance const* const province = instance_manager.get_map_instance().get_province_instance_by_index(province_index);
		if (province == nullptr) {
			return false;
		}
		const auto it = std::find_if(draft.states.begin(), draft.states.end(), [province](ProvinceInstance const* other) -> bool {
			return other == province || (other->get_state() != nullptr && other->get_state() == province->get_state());
		});
		if (enabled && it == draft.states.end()) {
			draft.states.push_back(province);
		} else if (!enabled && it != draft.states.end()) {
			draft.states.erase(it);
		}
		return true;
	}
	case 1:
		draft.reparations = enabled;
		return true;
	case 2:
		draft.humiliate = enabled;
		return true;
	case 3: // the side gives instead of demanding, the states listed make no sense then
		draft.concede = enabled;
		draft.states.clear();
		return true;
	case 4:
		manager.clear_peace_draft(country);
		return true;
	}
	return false;
}

bool GameActionManager::VariantVisitor::operator() (send_peace_offer_argument_t const& argument) const {
	const auto [country_index, war_id] = argument;
	CountryInstance& country = war_action_country(instance_manager, country_index);
	WarManager& manager = instance_manager.get_war_manager();

	PeaceTerms terms = manager.get_peace_draft(country, war_id);
	return manager.send_peace_offer(instance_manager, country, terms);
}

bool GameActionManager::VariantVisitor::operator() (answer_peace_offer_argument_t const& argument) const {
	const auto [country_index, war_id, accept] = argument;
	return instance_manager.get_war_manager().answer_peace_offer(
		instance_manager, war_action_country(instance_manager, country_index), war_id, accept
	);
}

// Politics
bool GameActionManager::VariantVisitor::operator() (enact_reform_argument_t const& argument) const {
	const auto [country_index, reform_index] = argument;
	CountryInstance& country = war_action_country(instance_manager, country_index);
	Reform const* const reform = instance_manager.definition_manager.get_politics_manager().get_issue_manager().get_reform_by_index(reform_index);
	if (reform == nullptr) {
		return false;
	}
	if (!PoliticsDynamics::get_reform_status(instance_manager, country, *reform).can_enact) {
		return false;
	}
	return country.add_reform(*reform);
}

// Colonisation
bool GameActionManager::VariantVisitor::operator() (start_colonisation_argument_t const& argument) const {
	const auto [country_index, province_index] = argument;
	ProvinceInstance* const province = instance_manager.get_map_instance().get_province_instance_by_index(province_index);
	if (province == nullptr) {
		return false;
	}
	return instance_manager.get_colonisation_manager().start_project(
		instance_manager, war_action_country(instance_manager, country_index), *province
	);
}

bool GameActionManager::VariantVisitor::operator() (cancel_colonisation_argument_t const& argument) const {
	const auto [country_index, province_index] = argument;
	ProvinceInstance* const province = instance_manager.get_map_instance().get_province_instance_by_index(province_index);
	if (province == nullptr) {
		return false;
	}
	return instance_manager.get_colonisation_manager().cancel_project(war_action_country(instance_manager, country_index), *province);
}

// A paid upgrade of one regiment (cavalry to hussars...): the goods come from the national stockpile
bool GameActionManager::VariantVisitor::operator() (upgrade_regiment_argument_t const& argument) const {
	const auto [country_index, regiment_id] = argument;
	CountryInstance& country = instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index);
	ArmyInstance* army = nullptr;
	RegimentInstance* const regiment = find_regiment(instance_manager, regiment_id, army);
	if (regiment == nullptr || army == nullptr) {
		return false;
	}
	RegimentType const* const target = UnitInstanceManager::get_upgrade_target(
		instance_manager.definition_manager.get_military_manager().get_unit_type_manager(), regiment->get_regiment_type()
	);
	if (target == nullptr) {
		return false;
	}
	return instance_manager.get_unit_instance_manager().upgrade_regiment(country, *army, *regiment, *target);
}

bool GameActionManager::VariantVisitor::operator() (set_army_group_orders_argument_t const& argument) const {
	const auto [country_index, group_id, execute] = argument;
	CountryInstance& country = instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index);
	return set_army_group_orders(country, group_id, execute);
}

// The support companies in the slots of a division template
bool GameActionManager::VariantVisitor::operator() (set_division_template_companies_argument_t const& argument) const {
	const auto [country_index, template_id, companies] = argument;
	CountryInstance& country = instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index);
	if (!country.has_unlock(UNLOCK_MECHANIC_DIVISION_TEMPLATES)) {
		return false;
	}
	return set_division_template_companies(instance_manager, country, template_id, companies);
}

// Research of the army technologies
bool GameActionManager::VariantVisitor::operator() (start_army_research_argument_t const& argument) const {
	const auto [country_index, technology_index] = argument;
	CountryInstance& country = instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index);
	return start_army_research(instance_manager, country, technology_index);
}

bool GameActionManager::VariantVisitor::operator() (cancel_army_research_argument_t const& argument) const {
	const auto [country_index, technology_index] = argument;
	CountryInstance& country = instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index);
	return cancel_army_research(instance_manager, country, technology_index);
}

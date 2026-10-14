#include "GameAction.hpp"

#include "openvic-simulation/core/Typedefs.hpp"
#include "openvic-simulation/DefinitionManager.hpp"
#include "openvic-simulation/InstanceManager.hpp"
#include "openvic-simulation/diplomacy/DiplomaticAction.hpp"
#include "openvic-simulation/economy/BuildingType.hpp"
#include "openvic-simulation/economy/credit/Credit.hpp"
#include "openvic-simulation/economy/production/FactoryInstance.hpp"
#include "openvic-simulation/economy/production/ProductionType.hpp"
#include "openvic-simulation/map/State.hpp"
#include "openvic-simulation/military/UnitInstanceGroup.hpp"
#include "openvic-simulation/military/War.hpp"
#include "openvic-simulation/map/ProvinceInstance.hpp"

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

// Credit and the central bank
namespace {
	bool is_valid_loan_term(const std::uint32_t term) {
		return term <= static_cast<std::uint32_t>(Loan::term_t::LONG);
	}
}

bool GameActionManager::VariantVisitor::operator() (take_external_loan_argument_t const& argument) const {
	const auto [borrower_index, lender_index, amount, term] = argument;
	if (OV_unlikely(!is_valid_loan_term(term))) {
		return false;
	}
	CountryInstanceManager& country_manager = instance_manager.get_country_instance_manager();
	return instance_manager.get_credit_manager().take_external_loan(
		instance_manager, country_manager.get_country_instance_by_index(borrower_index),
		country_manager.get_country_instance_by_index(lender_index), amount, static_cast<Loan::term_t>(term)
	);
}

bool GameActionManager::VariantVisitor::operator() (give_external_loans_argument_t const& argument) const {
	const auto [lender_index, group, amount, rate, term] = argument;
	if (OV_unlikely(!is_valid_loan_term(term) || group > static_cast<std::uint32_t>(CreditManager::target_group_t::SELECTED))) {
		return false;
	}
	CountryInstance& lender = instance_manager.get_country_instance_manager().get_country_instance_by_index(lender_index);
	return instance_manager.get_credit_manager().give_external_loans_to_group(
		instance_manager, lender, static_cast<CreditManager::target_group_t>(group), amount, rate, static_cast<Loan::term_t>(term)
	) > 0;
}

bool GameActionManager::VariantVisitor::operator() (stage_loan_target_argument_t const& argument) const {
	const auto [lender_index, target_index, selected] = argument;
	CountryInstanceManager& country_manager = instance_manager.get_country_instance_manager();
	instance_manager.get_credit_manager().stage_loan_target(
		country_manager.get_country_instance_by_index(lender_index), country_manager.get_country_instance_by_index(target_index),
		selected
	);
	return true;
}

bool GameActionManager::VariantVisitor::operator() (give_internal_loan_argument_t const& argument) const {
	const auto [country_index, province_index, amount, rate, term, policy] = argument;
	if (OV_unlikely(!is_valid_loan_term(term) || policy > static_cast<std::uint32_t>(Loan::maturity_policy_t::HARD))) {
		return false;
	}
	CountryInstance& country = instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index);
	ProvinceInstance* const province = instance_manager.get_map_instance().get_province_instance_by_index(province_index);
	if (OV_unlikely(province == nullptr || province->get_state() == nullptr)) {
		return false;
	}
	return instance_manager.get_credit_manager().give_internal_loan(
		instance_manager, country, *province->get_state(), amount, rate, static_cast<Loan::term_t>(term),
		static_cast<Loan::maturity_policy_t>(policy)
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
	// Rates are never lower than 1%
	country.set_central_bank_rates(
		CreditManager::clamp_rate(internal_rate), CreditManager::clamp_rate(external_rate)
	);
	return true;
}

bool GameActionManager::VariantVisitor::operator() (answer_alliance_request_argument_t const& argument) const {
	const auto [to_index, from_index, accept] = argument;
	CountryInstanceManager& country_manager = instance_manager.get_country_instance_manager();
	return instance_manager.get_credit_manager().answer_alliance_request(
		instance_manager, country_manager.get_country_instance_by_index(to_index),
		country_manager.get_country_instance_by_index(from_index), accept
	);
}

bool GameActionManager::VariantVisitor::operator() (set_subsidy_mode_argument_t const& argument) const {
	const auto [country_index, mode] = argument;
	if (OV_unlikely(mode > static_cast<std::uint32_t>(CountryInstance::subsidy_mode_t::BY_SHARE))) {
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
	ArmyInstance* const army = find_army(instance_manager, army_id);
	if (army == nullptr) {
		return false;
	}
	// Leader 0 takes the general off the army
	if (leader_id == 0) {
		return army->get_leader() != nullptr && army->set_leader(nullptr);
	}
	LeaderInstance* const leader = instance_manager.get_unit_instance_manager().get_leader_instance_by_unique_id(leader_id);
	if (leader == nullptr || leader->branch != unit_branch_t::LAND || &leader->country != &army->get_country() || !leader->get_can_be_used()) {
		return false;
	}
	return army->set_leader(leader);
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

#include "CountryInstance.hpp"

#include "openvic-simulation/misc/MoneyLedger.hpp"
#include "openvic-simulation/military/Unlocks.hpp"
#include "openvic-simulation/military/Equipment.hpp"
#include "openvic-simulation/military/UnitEquipment.hpp"
#include "openvic-simulation/economy/GoodDefinition.hpp"
#include "openvic-simulation/economy/production/FactoryInstance.hpp"
#include "openvic-simulation/map/State.hpp"

#include <algorithm>
#include <map>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>

#include <type_safe/strong_typedef.hpp>

#include "openvic-simulation/core/error/ErrorMacros.hpp"
#include "openvic-simulation/core/object/Date.hpp"
#include "openvic-simulation/core/object/FixedPoint.hpp"
#include "openvic-simulation/core/object/FixedPoint/Math.hpp"
#include "openvic-simulation/core/object/Timespan.hpp"
#include "openvic-simulation/core/Typedefs.hpp"
#include "openvic-simulation/country/CountryDefinition.hpp"
#include "openvic-simulation/country/SharedCountryValues.hpp"
#include "openvic-simulation/defines/CountryDefines.hpp"
#include "openvic-simulation/defines/DiplomacyDefines.hpp"
#include "openvic-simulation/defines/EconomyDefines.hpp"
#include "openvic-simulation/defines/MilitaryDefines.hpp"
#include "openvic-simulation/diplomacy/CountryRelation.hpp"
#include "openvic-simulation/economy/BuildingLevel.hpp"
#include "openvic-simulation/economy/BuildingType.hpp"
#include "openvic-simulation/economy/GoodInstance.hpp"
#include "openvic-simulation/economy/production/ProductionType.hpp"
#include "openvic-simulation/economy/trading/MarketInstance.hpp"
#include "openvic-simulation/history/CountryHistory.hpp"
#include "openvic-simulation/map/Crime.hpp"
#include "openvic-simulation/map/MapInstance.hpp"
#include "openvic-simulation/military/UnitInstanceGroup.hpp"
#include "openvic-simulation/military/UnitType.hpp"
#include "openvic-simulation/misc/GameRulesManager.hpp"
#include "openvic-simulation/military/SupplyDelivery.hpp"
#include "openvic-simulation/modifier/ModifierEffectCache.hpp"
#include "openvic-simulation/modifier/StaticModifierCache.hpp"
#include "openvic-simulation/politics/Government.hpp"
#include "openvic-simulation/politics/Ideology.hpp" // IWYU pragma: keep for HasIndex requirement of copy_values_from
#include "openvic-simulation/politics/NationalValue.hpp"
#include "openvic-simulation/politics/PartyPolicy.hpp"
#include "openvic-simulation/politics/Reform.hpp"
#include "openvic-simulation/politics/RuleSet.hpp"
#include "openvic-simulation/population/Culture.hpp"
#include "openvic-simulation/population/Pop.hpp"
#include "openvic-simulation/population/PopsAggregateDeps.hpp"
#include "openvic-simulation/population/PopSize.hpp"
#include "openvic-simulation/population/PopSum.hpp"
#include "openvic-simulation/population/PopType.hpp"
#include "openvic-simulation/research/Invention.hpp"
#include "openvic-simulation/research/Technology.hpp"
#include "openvic-simulation/types/ClampedValue.hpp"
#include "openvic-simulation/types/IndexedFlatMap.hpp"
#include "openvic-simulation/types/TypedIndices.hpp"
#include "openvic-simulation/types/UnitBranchType.hpp"
#include "openvic-simulation/utility/Logger.hpp"

#include "CountryInstanceDeps.hpp"
#include "CountryInstanceManager.hpp"

using namespace OpenVic;

using enum CountryInstance::country_status_t;

namespace {
	// What a unit uses of each of its supply goods in a day: the share a month (of the durable or of the consumable goods, of the army
	// or of the navy; for the coal and the fuel of a ship: at sea, on the move or in a port) of the amount that its construction needed,
	// divided by the days of the month, times the state of its group and the scale (the slider of the budget)
	template<typename Callback>
	void for_each_daily_supply(
		UnitType const& unit_type, const bool is_army, CountryInstance::supply_state_t const& state, const fixed_point_t scale,
		Callback&& callback, RegimentInstance const* regiment = nullptr
	) {
		OpenVicEconomyDefines const& defines = *CountryInstance::openvic_economy_defines;
		for (UnitType::supply_item_t const& item : unit_type.get_supply_items()) {
			fixed_point_t multiplier = state.multiplier;
			Rate const* share = nullptr;
			if (!is_army && item.engine_fuel) {
				share = state.moving ? &defines.navy_fuel_share_moving
					: state.in_port ? &defines.navy_fuel_share_in_port : &defines.navy_fuel_share_at_sea;
			} else {
				share = is_army
					? (item.consumable ? &defines.army_supply_consumable_share : &defines.army_supply_durable_share)
					: (item.consumable ? &defines.navy_supply_consumable_share : &defines.navy_supply_durable_share);
				if (state.moving) {
					multiplier = multiplier * defines.supply_moving_multiplier;
				}
			}
			const fixed_point_t amount =
				share->of(item.construction_amount) * multiplier * unit_type.supply_consumption * scale / defines.supply_days_per_month;
			// A regiment with a better weapon uses the good of that weapon instead of the good of the first generation of its line
			callback(
				regiment != nullptr && regiment->get_weapon_good() != nullptr && item.good == regiment->get_weapon_base_good()
					? *regiment->get_weapon_good() : *item.good,
				amount
			);
		}
	}
}

static constexpr size_t DAYS_OF_BALANCE_HISTORY = 30;
static constexpr colour_t ERROR_COLOUR = colour_t::from_integer(0xFF0000);

CountryInstance::CountryInstance(
	CountryDefinition const& new_country_definition,
	SharedCountryValues& new_shared_country_values,
	CountryInstanceDeps const& country_instance_deps
) : FlagStrings { "country" },
	HasIndex { new_country_definition.index },
	PopsAggregate {	country_instance_deps.pops_aggregate_deps },
	/* Main attributes */
	country_definition { new_country_definition },
	shared_country_values { new_shared_country_values },

	country_relations_manager { country_instance_deps.country_relations_manager },
	game_rules_manager { country_instance_deps.game_rules_manager },
	good_instance_manager { country_instance_deps.good_instance_manager },
	market_instance { country_instance_deps.market_instance },
	modifier_effect_cache { country_instance_deps.modifier_effect_cache },
	unit_type_manager { country_instance_deps.unit_type_manager },

	fallback_date_for_never_completing_research { country_instance_deps.fallback_date_for_never_completing_research },
	country_defines { country_instance_deps.country_defines },
	diplomacy_defines { country_instance_deps.diplomacy_defines },
	economy_defines { country_instance_deps.economy_defines },
	military_defines { country_instance_deps.military_defines },

	colour { ERROR_COLOUR },

	/* Production */
	building_type_unlock_levels { country_instance_deps.building_types },

	/* Budget */
	balance_history{DAYS_OF_BALANCE_HISTORY},
	taxable_income_by_pop_type { country_instance_deps.pop_types },
	effective_tax_rate_by_strata {
		country_instance_deps.stratas,
		[this](Strata const& strata)->auto {
			return [this,&strata](DependencyTracker& tracker)->fixed_point_t {
				return tax_efficiency.get(tracker) * tax_rate_slider_value_by_strata.at(strata).get_value(tracker);
			};
		}
	},
	tax_rate_slider_value_by_strata { country_instance_deps.stratas },

	/* Technology */
	technology_unlock_levels { country_instance_deps.technologies },
	invention_unlock_levels { country_instance_deps.inventions },

	/* Politics */
	upper_house_proportion_by_ideology { country_instance_deps.ideologies },
	reforms { country_instance_deps.reform_groups },
	flag_overrides_by_government_type { country_instance_deps.government_types },
	crime_unlock_levels { country_instance_deps.crimes },

	/* Trade */
	goods_data { country_instance_deps.good_instances },

	/* Diplomacy */

	/* Military */
	regiment_type_unlock_levels {
		regiment_type_index_t(country_instance_deps.regiment_types.size()),
		technology_unlock_level_t(0)
	},
	ship_type_unlock_levels {
		ship_type_index_t(country_instance_deps.ship_types.size()),
		technology_unlock_level_t(0)
	},

	/* DerivedState */
	flag_government_type { [this](DependencyTracker& tracker)->GovernmentType const* {
		GovernmentType const* current_government_type = government_type.get(tracker);
		if (current_government_type == nullptr) {
			return nullptr;
		}
		GovernmentType const* flag_override = flag_overrides_by_government_type.at(*current_government_type);
		return flag_override == nullptr
			? current_government_type
			: flag_override;
	}},
	total_score{[this](DependencyTracker& tracker)->fixed_point_t {
		return prestige.get(tracker) + industrial_power.get(tracker) + military_power.get(tracker);
	}},
	military_power{[this](DependencyTracker& tracker)->fixed_point_t {
		return military_power_from_land.get(tracker) + military_power_from_sea.get(tracker) + military_power_from_leaders.get(tracker);
	}},
	research_progress{[this](DependencyTracker& tracker)->fixed_point_t {
		const fixed_point_t current_research_cost_copy = current_research_cost.get(tracker);
		return current_research_cost_copy > 0
			? invested_research_points.get(tracker) / current_research_cost_copy
			: fixed_point_t::_0;
	}},
	desired_administrator_percentage{[this](DependencyTracker& tracker)->fixed_point_t {
		return country_defines.get_max_bureaucracy_percentage()
			+ total_administrative_multiplier.get(tracker) * country_defines.get_bureaucracy_percentage_increment();
	}},
	corruption_cost_multiplier{[this](DependencyTracker& tracker)->fixed_point_t {
		return 2 - administrative_efficiency_from_administrators.get(tracker);
	}},
	tariff_efficiency{[this](DependencyTracker& tracker)->fixed_point_t {
		return std::min(
			fixed_point_t::_1,
			administrative_efficiency_from_administrators.get(tracker) + country_defines.get_base_country_tax_efficiency()
		);
	}},
	effective_tariff_rate{[this](DependencyTracker& tracker)->fixed_point_t {
		return administrative_efficiency_from_administrators.get(tracker) * tariff_rate_slider_value.get_value(tracker);
	}},
	projected_administration_spending{[this](DependencyTracker& tracker)->fixed_point_t {
		return administration_spending_slider_value.get_value(tracker) * projected_administration_spending_unscaled_by_slider.get(tracker);
	}},
	projected_education_spending{[this](DependencyTracker& tracker)->fixed_point_t {
		return education_spending_slider_value.get_value(tracker) * projected_education_spending_unscaled_by_slider.get(tracker);
	}},
	projected_military_spending{[this](DependencyTracker& tracker)->fixed_point_t {
		return military_spending_slider_value.get_value(tracker) * projected_military_spending_unscaled_by_slider.get(tracker);
	}},
	projected_social_spending{[this](DependencyTracker& tracker)->fixed_point_t {
		return social_spending_slider_value.get_value(tracker) * projected_social_spending_unscaled_by_slider.get(tracker);
	}},
	projected_social_spending_unscaled_by_slider{[this](DependencyTracker& tracker)->fixed_point_t {
		return projected_pensions_spending_unscaled_by_slider.get(tracker)
			+ projected_unemployment_subsidies_spending_unscaled_by_slider.get(tracker);
	}},
	projected_import_subsidies{[this](DependencyTracker& tracker)->fixed_point_t {
		return has_import_subsidies.get(tracker)
			? -effective_tariff_rate.get(tracker) * get_yesterdays_import_value(tracker)
			: fixed_point_t::_0;
	}},
	projected_spending{[this](DependencyTracker& tracker)->fixed_point_t {
		return projected_administration_spending.get(tracker)
			+ projected_education_spending.get(tracker)
			+ projected_military_spending.get(tracker)
			+ projected_social_spending.get(tracker)
			+ projected_import_subsidies.get(tracker);
	}},
	has_import_subsidies{[this](DependencyTracker& tracker)->bool {
		return effective_tariff_rate.get(tracker) < 0;
	}} {
	modifier_sum.set_this_source(this);
	// Exclude PROVINCE (local) modifier effects from the country's modifier sum
	modifier_sum.set_this_excluded_targets(ModifierEffect::target_t::PROVINCE);

	// Some sliders need to have their max range limits temporarily set to 1 so they can start with a value of 0.5 or 1.0.
	// The range limits will be corrected on the first gamestate update, and the values will go to the closest valid point.
	for (ClampedValue& tax_rate_slider_value : tax_rate_slider_value_by_strata.get_values()) {
		tax_rate_slider_value.set_bounds(0, 1);
		tax_rate_slider_value.set_value(fixed_point_t::_0_50);
	}

	// army, navy and construction spending have minimums defined in EconomyDefines and always have an unmodified max (1.0).
	EconomyDefines const& economy_defines = country_instance_deps.economy_defines;
	army_spending_slider_value.set_bounds(economy_defines.get_minimum_army_spending_slider_value(), 1);
	army_spending_slider_value.set_value(1);

	navy_spending_slider_value.set_bounds(economy_defines.get_minimum_navy_spending_slider_value(), 1);
	navy_spending_slider_value.set_value(1);

	construction_spending_slider_value.set_bounds(0, 1);
	// The allocation to the investment fund starts at nothing: the player chooses how much of the budget goes to it, the AI
	// sets its allocation itself (InvestmentPlanner)
	construction_spending_slider_value.set_value(0);

	education_spending_slider_value.set_bounds(0, 1);
	education_spending_slider_value.set_value(fixed_point_t::_0_50);

	administration_spending_slider_value.set_bounds(0, 1);
	administration_spending_slider_value.set_value(fixed_point_t::_0_50);

	social_spending_slider_value.set_bounds(0, 1);
	social_spending_slider_value.set_value(1);

	military_spending_slider_value.set_bounds(0, 1);
	military_spending_slider_value.set_value(fixed_point_t::_0_50);

	tariff_rate_slider_value.set_bounds(0, 0);
	tariff_rate_slider_value.set_value(0);

	update_parties_for_votes(&new_country_definition);

	for (BuildingType const& building_type : building_type_unlock_levels.get_keys()) {
		if (building_type.is_enabled_by_default) {
			unlock_building_type(building_type);
		}
	}

	for (Crime const& crime : crime_unlock_levels.get_keys()) {
		if (crime.is_default_active) {
			unlock_crime(crime);
		}
	}

	for (RegimentType const& regiment_type : country_instance_deps.regiment_types) {
		if (regiment_type.starts_unlocked) {
			unlock_unit_type(regiment_type);
		}
	}

	for (ShipType const& ship_type : country_instance_deps.ship_types) {
		if (ship_type.starts_unlocked) {
			unlock_unit_type(ship_type);
		}
	}
}

std::string_view CountryInstance::get_identifier() const {
	return country_definition.get_identifier();
}

bool CountryInstance::exists() const {
	return !owned_provinces.empty();
}

bool CountryInstance::is_rebel_country() const {
	return country_definition.is_rebel_country();
}

bool CountryInstance::is_civilised() const {
	return country_status <= COUNTRY_STATUS_CIVILISED;
}

bool CountryInstance::can_colonise() const {
	return country_status <= COUNTRY_STATUS_SECONDARY_POWER;
}

bool CountryInstance::is_great_power() const {
	return country_status == COUNTRY_STATUS_GREAT_POWER;
}

bool CountryInstance::is_secondary_power() const {
	return country_status == COUNTRY_STATUS_SECONDARY_POWER;
}

bool CountryInstance::is_at_war() const {
	return !war_enemies.empty();
}

bool CountryInstance::is_neighbour(CountryInstance const& country) const {
	return neighbouring_countries.contains(&country);
}

bool CountryInstance::may_build_in(const BuildingRestrictionCategory restriction_category, ProvinceInstance const& location) const {
	CountryInstance const* const owner_ptr = location.get_owner();

	if (owner_ptr == nullptr) {
		//Can't build in uncolonised provinces
		return false;
	}
	CountryInstance const& owner = *owner_ptr;

	if (owner == *this) {
		switch(restriction_category) {
			case BuildingRestrictionCategory::UNRESTRICTED:
				return true;
			case BuildingRestrictionCategory::INFRASTRUCTURE:
				return rule_set.may_expand_infrastructure_domestically();
			case BuildingRestrictionCategory::FACTORY:
				return rule_set.may_build_factory_domestically();
		}
	}

	if (is_at_war_with(owner)) {
		//Not allowed to build in hostile lands.
		return false;
	}

	if (!owner.rule_set.foreigners_may_invest()) {
		return false;
	}

	switch(restriction_category) {
		case BuildingRestrictionCategory::UNRESTRICTED:
			return false; //For example you can't invest in foreign forts.
		case BuildingRestrictionCategory::INFRASTRUCTURE:
			return rule_set.may_invest_in_expanding_infrastructure_abroad();
		case BuildingRestrictionCategory::FACTORY:
			return rule_set.may_invest_in_building_factory_abroad();
	}

	OpenVic::unreachable();
}

CountryRelationManager::relation_value_type CountryInstance::get_relations_with(CountryInstance const& country) const {
	return country_relations_manager.get_country_relation(this, &country);
}

void CountryInstance::set_relations_with(CountryInstance& country, CountryRelationManager::relation_value_type relations) {
	country_relations_manager.set_country_relation(this, &country, relations);
}

bool CountryInstance::has_alliance_with(CountryInstance const& country) const {
	return country_relations_manager.get_country_alliance(this, &country);
}

void CountryInstance::set_alliance_with(CountryInstance& country, bool alliance) {
	country_relations_manager.set_country_alliance(this, &country, alliance);
}

bool CountryInstance::is_at_war_with(CountryInstance const& country) const {
	return war_enemies.contains(country);
}

void CountryInstance::set_at_war_with(CountryInstance& country, bool at_war) {
	country_relations_manager.set_at_war_with(this, &country, at_war);
	if (at_war) {
		war_enemies.emplace(country);
		country.war_enemies.emplace(*this);
	} else {
		war_enemies.unordered_erase(country);
		country.war_enemies.unordered_erase(*this);
	}
}

bool CountryInstance::has_military_access_to(CountryInstance const& country) const {
	return country_relations_manager.get_has_military_access_to(this, &country);
}

void CountryInstance::set_military_access_to(CountryInstance& country, bool access) {
	country_relations_manager.set_has_military_access_to(this, &country, access);
}

bool CountryInstance::is_sending_war_subsidy_to(CountryInstance const& country) const {
	return country_relations_manager.get_war_subsidies_to(this, &country);
}

void CountryInstance::set_sending_war_subsidy_to(CountryInstance& country, bool sending) {
	country_relations_manager.set_war_subsidies_to(this, &country, sending);
}

bool CountryInstance::is_commanding_units(CountryInstance const& country) const {
	return country_relations_manager.get_commands_units(this, &country);
}

void CountryInstance::set_commanding_units(CountryInstance& country, bool commanding) {
	country_relations_manager.set_commands_units(this, &country, commanding);
}

bool CountryInstance::has_vision_of(CountryInstance const& country) const {
	return country_relations_manager.get_has_vision(this, &country);
}

void CountryInstance::set_has_vision_of(CountryInstance& country, bool vision) {
	country_relations_manager.set_has_vision(this, &country, vision);
}

CountryRelationManager::OpinionType CountryInstance::get_opinion_of(CountryInstance const& country) const {
	return country_relations_manager.get_country_opinion(this, &country);
}

void CountryInstance::set_opinion_of(CountryInstance& country, CountryRelationManager::OpinionType opinion) {
	if (OV_unlikely(country.sphere_owner.get_untracked() != nullptr && opinion == CountryRelationManager::OpinionType::Sphere)) {
		spdlog::warn_s("Attempting to add '{}' to a sphere when it is already included in a sphere.", country);
	}
	country_relations_manager.set_country_opinion(this, &country, opinion);
	if (opinion == CountryRelationManager::OpinionType::Sphere) {
		country.sphere_owner.set(this);
	} else if (country.sphere_owner.get_untracked() == this) {
		country.sphere_owner.set(nullptr);
	}
}

void CountryInstance::increase_opinion_of(CountryInstance& country) {
	CountryRelationManager::OpinionType opinion = country_relations_manager.get_country_opinion(this, &country);
	opinion++;
	set_opinion_of(country, opinion);
}

void CountryInstance::decrease_opinion_of(CountryInstance& country) {
	CountryRelationManager::OpinionType opinion = country_relations_manager.get_country_opinion(this, &country);
	opinion--;
	set_opinion_of(country, opinion);
}

CountryRelationManager::influence_value_type CountryInstance::get_influence_with(CountryInstance const& country) const {
	return country_relations_manager.get_influence_with(this, &country);
}

void CountryInstance::set_influence_with(CountryInstance& country, CountryRelationManager::influence_value_type influence) {
	country_relations_manager.set_influence_with(this, &country, influence);
}

CountryRelationManager::influence_priority_value_type CountryInstance::get_influence_priority_with(CountryInstance const& country) const {
	return country_relations_manager.get_influence_priority_with(this, &country);
}

void CountryInstance::set_influence_priority_with(CountryInstance& country, CountryRelationManager::influence_priority_value_type influence) {
	country_relations_manager.set_influence_priority_with(this, &country, influence);
}

std::optional<Date> CountryInstance::get_decredited_from_date(CountryInstance const& country) const {
	return country_relations_manager.get_discredited_date(this, &country);
}

void CountryInstance::set_discredited_from(CountryInstance& country, Date until) {
	country_relations_manager.set_discredited_date(this, &country, until);
}

std::optional<Date> CountryInstance::get_embass_banned_from_date(CountryInstance const& country) const {
	return country_relations_manager.get_embassy_banned_date(this, &country);
}

void CountryInstance::set_embassy_banned_from(CountryInstance& country, Date until) {
	country_relations_manager.set_embassy_banned_date(this, &country, until);
}

bool CountryInstance::can_army_units_enter(CountryInstance const& country) const {
	// TODO: include war allies, puppets
	return *this == country || is_at_war_with(country) || has_military_access_to(country);
}

bool CountryInstance::can_navy_units_enter(CountryInstance const& country) const {
	// TODO: include war allies, puppets
	return *this == country || has_military_access_to(country);
}

fixed_point_t CountryInstance::get_script_variable(memory::string const& variable_name) const {
	const decltype(script_variables)::const_iterator it = script_variables.find(variable_name);

	if (it != script_variables.end()) {
		return it->second;
	} else {
		return 0;
	}
}

void CountryInstance::set_script_variable(memory::string const& variable_name, fixed_point_t value) {
	script_variables[variable_name] = value;
}

void CountryInstance::change_script_variable(memory::string const& variable_name, fixed_point_t value) {
	script_variables[variable_name] += value;
}

fixed_point_t CountryInstance::get_taxable_income_by_strata(Strata const& strata) const {
	fixed_point_t running_total = 0;
	for (auto const& [pop_type, taxable_income] : taxable_income_by_pop_type) {
		if (pop_type.strata == strata) {
			running_total += taxable_income;
		}
	}

	return running_total;
}
DerivedState<fixed_point_t>& CountryInstance::get_effective_tax_rate_by_strata(Strata const& strata) {
	return effective_tax_rate_by_strata.at(strata);
}

ReadOnlyClampedValue& CountryInstance::get_tax_rate_slider_value_by_strata(Strata const& strata) {
	return tax_rate_slider_value_by_strata.at(strata);
}
ReadOnlyClampedValue const& CountryInstance::get_tax_rate_slider_value_by_strata(Strata const& strata) const {
	return tax_rate_slider_value_by_strata.at(strata);
}

bool CountryInstance::add_owned_province(ProvinceInstance& new_item) {
	OV_ERR_FAIL_COND_V_MSG(
		!owned_provinces.emplace(&new_item).second, false,
		memory::fmt::format("owned_province \"{}\" already present in country {}", new_item, *this)
	);
	return true;
}

bool CountryInstance::remove_owned_province(ProvinceInstance const& item_to_remove) {
	OV_ERR_FAIL_COND_V_MSG(
		owned_provinces.erase(&item_to_remove) == 0, false,
		memory::fmt::format("owned_province \"{}\" not present in country {}", item_to_remove, *this)
	);
	return true;
}

bool CountryInstance::add_controlled_province(ProvinceInstance& new_item) {
	OV_ERR_FAIL_COND_V_MSG(
		!controlled_provinces.emplace(&new_item).second, false,
		memory::fmt::format("controlled_province \"{}\" already present in country {}", new_item, *this)
	);
	return true;
}

bool CountryInstance::remove_controlled_province(ProvinceInstance const& item_to_remove) {
	OV_ERR_FAIL_COND_V_MSG(
		controlled_provinces.erase(&item_to_remove) == 0, false,
		memory::fmt::format("controlled_province \"{}\" not present in country {}", item_to_remove, *this)
	);
	return true;
}

bool CountryInstance::add_core_province(ProvinceInstance& new_item) {
	OV_ERR_FAIL_COND_V_MSG(
		!core_provinces.emplace(&new_item).second, false,
		memory::fmt::format("core_province \"{}\" already present in country {}", new_item, *this)
	);
	return true;
}

bool CountryInstance::remove_core_province(ProvinceInstance const& item_to_remove) {
	OV_ERR_FAIL_COND_V_MSG(
		core_provinces.erase(&item_to_remove) == 0, false,
		memory::fmt::format("core_province \"{}\" not present in country {}", item_to_remove, *this)
	);
	return true;
}

bool CountryInstance::add_state(State& new_item) {
	OV_ERR_FAIL_COND_V_MSG(
		!states.emplace(&new_item).second, false,
		memory::fmt::format("state \"{}\" already present in country {}", new_item, *this)
	);
	return true;
}

bool CountryInstance::remove_state(State const& item_to_remove) {
	OV_ERR_FAIL_COND_V_MSG(
		states.erase(&item_to_remove) == 0, false,
		memory::fmt::format("state \"{}\" not present in country {}", item_to_remove, *this)
	);
	return true;
}

bool CountryInstance::add_accepted_culture(Culture const& new_item) {
	OV_ERR_FAIL_COND_V_MSG(
		!accepted_cultures.emplace(&new_item).second, false,
		memory::fmt::format("accepted_culture \"{}\" already present in country {}", new_item, *this)
	);
	return true;
}

bool CountryInstance::remove_accepted_culture(Culture const& item_to_remove) {
	OV_ERR_FAIL_COND_V_MSG(
		accepted_cultures.erase(&item_to_remove) == 0, false,
		memory::fmt::format("accepted_culture \"{}\" not present in country {}", item_to_remove, *this)
	);
	return true;
}

bool CountryInstance::set_ruling_party(CountryParty const& new_ruling_party) {
	if (ruling_party.get_untracked() != &new_ruling_party) {
		ruling_party.set(&new_ruling_party);

		return update_rule_set();
	} else {
		return true;
	}
}

bool CountryInstance::add_reform(Reform const& new_reform) {
	ReformGroup const& reform_group = new_reform.group;
	Reform const*& reform = reforms.at(reform_group);

	if (reform != &new_reform) {
		if (reform_group.is_administrative) {
			if (reform != nullptr) {
				total_administrative_multiplier -= reform->administrative_multiplier;
			}
			total_administrative_multiplier += new_reform.administrative_multiplier;
		}

		reform = &new_reform;

		// TODO - if new_reform.get_reform_group().is_uncivilised() ?
		// TODO - new_reform.get_on_execute_trigger() / new_reform.get_on_execute_effect() ?

		return update_rule_set();
	} else {
		return true;
	}
}

void CountryInstance::set_strata_tax_rate_slider_value(Strata const& strata, const fixed_point_t new_value) {
	tax_rate_slider_value_by_strata.at(strata).set_value(new_value);
}

void CountryInstance::set_army_spending_slider_value(const fixed_point_t new_value) {
	army_spending_slider_value.set_value(new_value);
}

void CountryInstance::set_navy_spending_slider_value(const fixed_point_t new_value) {
	navy_spending_slider_value.set_value(new_value);
}

void CountryInstance::set_construction_spending_slider_value(const fixed_point_t new_value) {
	construction_spending_slider_value.set_value(new_value);
}

void CountryInstance::set_education_spending_slider_value(const fixed_point_t new_value) {
	education_spending_slider_value.set_value(new_value);
}

void CountryInstance::set_administration_spending_slider_value(const fixed_point_t new_value) {
	administration_spending_slider_value.set_value(new_value);
}

void CountryInstance::set_social_spending_slider_value(const fixed_point_t new_value) {
	social_spending_slider_value.set_value(new_value);
}

void CountryInstance::set_military_spending_slider_value(const fixed_point_t new_value) {
	military_spending_slider_value.set_value(new_value);
}

void CountryInstance::set_tariff_rate_slider_value(const fixed_point_t new_value) {
	tariff_rate_slider_value.set_value(new_value);
}

void CountryInstance::change_war_exhaustion(fixed_point_t delta) {
	war_exhaustion = std::clamp(war_exhaustion + delta, fixed_point_t::_0, war_exhaustion_max);
}

bool CountryInstance::add_unit_instance_group(UnitInstanceGroup& group) {
	using enum unit_branch_t;

	switch (group.branch) {
	case LAND:
		armies.emplace_back(static_cast<ArmyInstance&>(group));
		return true;
	case NAVAL:
		navies.emplace_back(static_cast<NavyInstance&>(group));
		return true;
	default:
		spdlog::error_s(
			"Trying to add unit group \"{}\" with invalid branch {} to country {}",
			group.get_name(), static_cast<uint32_t>(group.branch), *this
		);
		return false;
	}
}

bool CountryInstance::remove_unit_instance_group(UnitInstanceGroup const& group) {
	const auto remove_from_vector = [this, &group]<unit_branch_t Branch>(
		memory::vector<std::reference_wrapper<UnitInstanceGroupBranched<Branch>>>& unit_instance_groups
	) -> bool {
		const auto it =	std::find(
			unit_instance_groups.begin(),
			unit_instance_groups.end(),
			group
		);

		if (it != unit_instance_groups.end()) {
			unit_instance_groups.erase(it);
			return true;
		} else {
			spdlog::error_s(
				"Trying to remove non-existent {} \"{}\" from country {}",
				get_branched_unit_group_name(Branch), group.get_name(), *this
			);
			return false;
		}
	};

	using enum unit_branch_t;

	switch (group.branch) {
	case LAND:
		return remove_from_vector(armies);
	case NAVAL:
		return remove_from_vector(navies);
	default:
		spdlog::error_s(
			"Trying to remove unit group \"{}\" with invalid branch {} from country {}",
			group.get_name(), static_cast<uint32_t>(group.branch), *this
		);
		return false;
	}
}

bool CountryInstance::add_leader(LeaderInstance& leader) {
	using enum unit_branch_t;

	switch (leader.branch) {
	case LAND:
		generals.push_back(leader);
		return true;
	case NAVAL:
		admirals.push_back(leader);
		return true;
	default:
		spdlog::error_s(
			"Trying to add leader \"{}\" with invalid branch {} to country {}",
			leader.get_name(), static_cast<uint32_t>(leader.branch), *this
		);
		return false;
	}
}

bool CountryInstance::remove_leader(LeaderInstance const& leader) {
	using enum unit_branch_t;

	memory::vector<std::reference_wrapper<LeaderInstance>>* leaders;

	switch (leader.branch) {
	case LAND:
		leaders = &generals;
		break;
	case NAVAL:
		leaders = &admirals;
		break;
	default:
		spdlog::error_s(
			"Trying to remove leader \"{}\" with invalid branch {} from country {}",
			leader.get_name(), static_cast<uint32_t>(leader.branch), *this
		);
		return false;
	}

	const auto it = std::find(leaders->begin(), leaders->end(), leader);

	if (it != leaders->end()) {
		leaders->erase(it);
		return true;
	} else {
		spdlog::error_s(
			"Trying to remove non-existent {} \"{}\" from country {}",
			 get_branched_leader_name(leader.branch), leader.get_name(), *this
		);
		return false;
	}
}

bool CountryInstance::has_leader_with_name(std::string_view name) const {
	const auto check_leaders = [name](
		memory::vector<std::reference_wrapper<LeaderInstance>> const& leaders
	) -> bool {
		for (LeaderInstance const& leader : leaders) {
			if (leader.get_name() == name) {
				return true;
			}
		}
		return false;
	};

	return check_leaders(generals) || check_leaders(admirals);
}

bool CountryInstance::modify_unit_type_unlock(const regiment_type_index_t regiment_type_index, technology_unlock_level_t unlock_level_change) {
	technology_unlock_level_t& unlock_level = regiment_type_unlock_levels[regiment_type_index];
	if(!validate_unit_unlock_change(regiment_type_index, unlock_level, unlock_level_change)) {
		return false;
	}
	
	const bool was_unlocked_before = is_unlocked(unlock_level);
	unlock_level += unlock_level_change;
	const bool is_unlocked_after = is_unlocked(unlock_level);

	if (was_unlocked_before != is_unlocked_after) {
		TypedSpan<regiment_type_index_t, const RegimentType> regiment_types = shared_country_values.regiment_types;
		if (was_unlocked_before) {
			//recalculate entirely
			allowed_regiment_cultures = regiment_allowed_cultures_t::NO_CULTURES;
			for (RegimentType const& regiment_type : regiment_types) {
				if (!is_unit_type_unlocked(regiment_type)) {
					continue;
				}

				allowed_regiment_cultures = RegimentType::allowed_cultures_get_most_permissive(
					allowed_regiment_cultures,
					regiment_type.allowed_cultures
				);
			}
		} else {
			allowed_regiment_cultures = RegimentType::allowed_cultures_get_most_permissive(
				allowed_regiment_cultures,
				regiment_types[regiment_type_index].allowed_cultures
			);
		}
	}
	return true;
}
bool CountryInstance::modify_unit_type_unlock(const ship_type_index_t ship_type_index, technology_unlock_level_t unlock_level_change) {
	technology_unlock_level_t& unlock_level = ship_type_unlock_levels[ship_type_index];
	if(!validate_unit_unlock_change(ship_type_index, unlock_level, unlock_level_change)) {
		return false;
	}
	unlock_level += unlock_level_change;
	return true;
}

bool CountryInstance::modify_unit_type_unlock(UnitType const& unit_type, technology_unlock_level_t unlock_level_change) {
	using enum unit_branch_t;

	switch (unit_type.branch) {
	case LAND:
		return modify_unit_type_unlock(static_cast<UnitTypeBranched<LAND> const&>(unit_type).index, unlock_level_change);
	case NAVAL:
		return modify_unit_type_unlock(static_cast<UnitTypeBranched<NAVAL> const&>(unit_type).index, unlock_level_change);
	default:
		spdlog::error_s(
			"Attempted to change unlock level for unit type \"{}\" with invalid branch {} for country {}",
			unit_type, static_cast<uint32_t>(unit_type.branch), *this
		);
		return false;
	}
}

bool CountryInstance::unlock_unit_type(UnitType const& unit_type) {
	return modify_unit_type_unlock(unit_type, technology_unlock_level_t { 1 });
}

bool CountryInstance::is_unit_type_unlocked(UnitType const& unit_type) const {
	using enum unit_branch_t;

	switch (unit_type.branch) {
	case LAND: {
		RegimentType const& regiment_type = static_cast<UnitTypeBranched<LAND> const&>(unit_type);
		// A unit type of OpenVic is open when it has no key or the country has the key (unit:<id>); the units of Vic2 follow the technologies of Vic2
		if (regiment_type.ov.is_ov) {
			return regiment_type.ov.unlock_key.empty() || has_unlock(regiment_type.ov.unlock_key);
		}
		return is_unit_type_unlocked(regiment_type.index);
	}
	case NAVAL:
		return is_unit_type_unlocked(static_cast<UnitTypeBranched<NAVAL> const&>(unit_type).index);
	default:
		spdlog::error_s(
			"Attempted to check if unit type \"{}\" with invalid branch {} is unlocked for country {}",
			unit_type, static_cast<uint32_t>(unit_type.branch), *this
		);
		return false;
	}
}

bool CountryInstance::modify_building_type_unlock(
	BuildingType const& building_type, technology_unlock_level_t tech_unlock_level_change
) {
	building_level_t& unlock_level = building_type_unlock_levels.at(building_type);
	building_level_t unlock_level_change = building_level_t(type_safe::get(tech_unlock_level_change));

	// This catches subtracting below 0 or adding above the int types maximum value
	if (unlock_level + unlock_level_change < building_level_t(0)) {
		spdlog::error_s(
			"Attempted to change unlock level for building type {} in country {} to invalid value: current level = {}, change = {}, invalid new value = {}",
			building_type, *this, unlock_level, unlock_level_change,
			unlock_level + unlock_level_change
		);
		return false;
	}

	unlock_level += unlock_level_change;

	if (building_type.production_type != nullptr) {
		good_instance_manager.enable_good(building_type.production_type->output_good);
	}

	return true;
}

bool CountryInstance::unlock_building_type(BuildingType const& building_type) {
	return modify_building_type_unlock(building_type, technology_unlock_level_t { 1 });
}

building_level_t const& CountryInstance::get_building_type_unlock_levels(BuildingType const& building_type) const {
	return building_type_unlock_levels.at(building_type);
}

bool CountryInstance::is_building_type_unlocked(BuildingType const& building_type) const {
	// A building with requires_unlock is built with one of its keys (the technologies of the army), not by the technologies of the base game
	if (!building_type.requires_unlock.empty()) {
		for (memory::string const& key : building_type.requires_unlock) {
			if (has_unlock(key)) {
				return true;
			}
		}
		return false;
	}
	return building_type_unlock_levels.at(building_type) > building_level_t(0);
}

bool CountryInstance::modify_crime_unlock(Crime const& crime, technology_unlock_level_t unlock_level_change) {
	technology_unlock_level_t& unlock_level = crime_unlock_levels.at(crime);

	// This catches subtracting below 0 or adding above the int types maximum value
	if (unlock_level + unlock_level_change < 0) {
		spdlog::error_s(
			"Attempted to change unlock level for crime {} in country {} to invalid value: current level = {}, change = {}, invalid new value = {}",
			crime, *this, unlock_level,
			unlock_level_change, unlock_level + unlock_level_change
		);
		return false;
	}

	unlock_level += unlock_level_change;

	return true;
}

bool CountryInstance::unlock_crime(Crime const& crime) {
	return modify_crime_unlock(crime, technology_unlock_level_t { 1 });
}

bool CountryInstance::is_crime_unlocked(Crime const& crime) const {
	return is_unlocked(crime_unlock_levels.at(crime));
}

bool CountryInstance::modify_gas_attack_unlock(technology_unlock_level_t unlock_level_change) {
	// This catches subtracting below 0 or adding above the int types maximum value
	if (gas_attack_unlock_level + unlock_level_change < 0) {
		spdlog::error_s(
			"Attempted to change unlock level for gas attack in country {} to invalid value: current level = {}, change = {}, invalid new value = {}",
			*this, gas_attack_unlock_level,
			unlock_level_change, gas_attack_unlock_level + unlock_level_change
		);
		return false;
	}

	gas_attack_unlock_level += unlock_level_change;

	return true;
}

bool CountryInstance::unlock_gas_attack() {
	return modify_gas_attack_unlock(technology_unlock_level_t { 1 });
}

bool CountryInstance::is_gas_attack_unlocked() const {
	return is_unlocked(gas_attack_unlock_level);
}

bool CountryInstance::modify_gas_defence_unlock(technology_unlock_level_t unlock_level_change) {
	// This catches subtracting below 0 or adding above the int types maximum value
	if (gas_defence_unlock_level + unlock_level_change < 0) {
		spdlog::error_s(
			"Attempted to change unlock level for gas defence in country {} to invalid value: current level = {}, change = {}, invalid new value = {}",
			*this, gas_defence_unlock_level,
			unlock_level_change, gas_defence_unlock_level + unlock_level_change
		);
		return false;
	}

	gas_defence_unlock_level += unlock_level_change;

	return true;
}

bool CountryInstance::unlock_gas_defence() {
	return modify_gas_defence_unlock(technology_unlock_level_t { 1 });
}

bool CountryInstance::is_gas_defence_unlocked() const {
	return is_unlocked(gas_defence_unlock_level);
}

bool CountryInstance::modify_unit_variant_unlock(unit_variant_t unit_variant, technology_unlock_level_t unlock_level_change) {
	if (unit_variant < 1) {
		spdlog::error_s("Trying to modify unlock level for default unit variant 0");
		return false;
	}

	if (unit_variant_unlock_levels.size() < unit_variant) {
		unit_variant_unlock_levels.resize(unit_variant);
	}

	technology_unlock_level_t& unlock_level = unit_variant_unlock_levels[unit_variant - 1];

	bool ret = true;

	// This catches subtracting below 0 or adding above the int types maximum value
	if (unlock_level + unlock_level_change < 0) {
		spdlog::error_s(
			"Attempted to change unlock level for unit variant {} in country {} to invalid value: current level = {}, change = {}, invalid new value = {}",
			static_cast<uint64_t>(unit_variant), *this, unlock_level,
			unlock_level_change, unlock_level + unlock_level_change
		);
		ret = false;
	} else {
		unlock_level += unlock_level_change;
	}

	while (!unit_variant_unlock_levels.empty() && unit_variant_unlock_levels.back() < 1) {
		unit_variant_unlock_levels.pop_back();
	}

	return ret;
}

bool CountryInstance::unlock_unit_variant(unit_variant_t unit_variant) {
	return modify_unit_variant_unlock(unit_variant, technology_unlock_level_t { 1 });
}

unit_variant_t CountryInstance::get_max_unlocked_unit_variant() const {
	return unit_variant_unlock_levels.size();
}

bool CountryInstance::modify_technology_unlock(
	Technology const& technology, technology_unlock_level_t unlock_level_change
) {
	technology_unlock_level_t& unlock_level = technology_unlock_levels.at(technology);

	// This catches subtracting below 0 or adding above the int types maximum value
	if (unlock_level + unlock_level_change < 0) {
		spdlog::error_s(
			"Attempted to change unlock level for technology {} in country {} to invalid value: current level = {}, change = {}, invalid new value = {}",
			technology, *this, unlock_level,
			unlock_level_change, unlock_level + unlock_level_change
		);
		return false;
	}

	unlock_level += unlock_level_change;

	bool ret = true;

	// TODO - bool unciv_military ?

	if (technology.get_unit_variant().has_value()) {
		ret &= modify_unit_variant_unlock(*technology.get_unit_variant(), unlock_level_change);
	}
	for (UnitType const* unit : technology.activated_units) {
		ret &= modify_unit_type_unlock(*unit, unlock_level_change);
	}
	for (BuildingType const* building : technology.activated_buildings) {
		ret &= modify_building_type_unlock(*building, unlock_level_change);
	}

	return ret;
}

bool CountryInstance::set_technology_unlock_level(
	Technology const& technology, technology_unlock_level_t unlock_level
) {
	const technology_unlock_level_t unlock_level_change = unlock_level - technology_unlock_levels.at(technology);
	return unlock_level_change != 0 ? modify_technology_unlock(technology, unlock_level_change) : true;
}

bool CountryInstance::unlock_technology(Technology const& technology) {
	return modify_technology_unlock(technology, technology_unlock_level_t { 1 });
}

bool CountryInstance::is_technology_unlocked(Technology const& technology) const {
	return is_unlocked(technology_unlock_levels.at(technology));
}

bool CountryInstance::modify_invention_unlock(
	Invention const& invention, technology_unlock_level_t unlock_level_change
) {
	technology_unlock_level_t& unlock_level = invention_unlock_levels.at(invention);

	// This catches subtracting below 0 or adding above the int types maximum value
	if (unlock_level + unlock_level_change < 0) {
		spdlog::error_s(
			"Attempted to change unlock level for invention {} in country {} to invalid value: current level = {}, change = {}, invalid new value = {}",
			invention, *this, unlock_level,
			unlock_level_change, unlock_level + unlock_level_change
		);
		return false;
	}

	const bool invention_was_unlocked = is_unlocked(unlock_level);
	unlock_level += unlock_level_change;
	if (invention_was_unlocked != is_unlocked(unlock_level)) {
		if (invention_was_unlocked) {
			inventions_count-=1;
		} else {
			inventions_count+=1;
		}
	}

	bool ret = true;

	// TODO - handle invention.is_news()

	for (UnitType const* unit : invention.activated_units) {
		ret &= modify_unit_type_unlock(*unit, unlock_level_change);
	}
	for (BuildingType const* building : invention.activated_buildings) {
		ret &= modify_building_type_unlock(*building, unlock_level_change);
	}
	for (Crime const* crime : invention.enabled_crimes) {
		ret &= modify_crime_unlock(*crime, unlock_level_change);
	}
	if (invention.unlocks_gas_attack) {
		ret &= modify_gas_attack_unlock(unlock_level_change);
	}
	if (invention.unlocks_gas_defence) {
		ret &= modify_gas_defence_unlock(unlock_level_change);
	}

	return ret;
}

bool CountryInstance::set_invention_unlock_level(
	Invention const& invention, technology_unlock_level_t unlock_level
) {
	const technology_unlock_level_t unlock_level_change = unlock_level - invention_unlock_levels.at(invention);
	return unlock_level_change != 0 ? modify_invention_unlock(invention, unlock_level_change) : true;
}

bool CountryInstance::unlock_invention(Invention const& invention) {
	return modify_invention_unlock(invention, technology_unlock_level_t { 1 });
}

bool CountryInstance::is_invention_unlocked(Invention const& invention) const {
	return is_unlocked(invention_unlock_levels.at(invention));
}

bool CountryInstance::is_primary_culture(Culture const& culture) const {
	return &culture == primary_culture;
}

bool CountryInstance::is_accepted_culture(Culture const& culture) const {
	return accepted_cultures.contains(&culture);
}

bool CountryInstance::is_primary_or_accepted_culture(Culture const& culture) const {
	return is_primary_culture(culture) || is_accepted_culture(culture);
}

fixed_point_t CountryInstance::calculate_research_cost(Technology const& technology) const {
	// TODO - what if research bonus is -100%? Divide by 0 -> infinite cost?
	return technology.cost / (fixed_point_t::_1 + get_modifier_effect_value(
		*modifier_effect_cache.get_research_bonus_effects(technology.area.folder)
	));
}

bool CountryInstance::can_research_tech(Technology const& technology, const Date today) const {
	Technology const* current_research_copy = current_research.get_untracked();
	if (
		technology.year > today.get_year()
		|| !is_civilised()
		|| is_technology_unlocked(technology)
		|| (current_research_copy && technology == *current_research_copy)
	) {
		return false;
	}

	const Technology::area_index_t index_in_area = technology.index_in_area;

	return index_in_area == 0 || is_technology_unlocked(technology.area.get_technologies()[index_in_area - 1]);
}

void CountryInstance::start_research(Technology const& technology, const Date today) {
	if (OV_unlikely(!can_research_tech(technology, today))) {
		spdlog::warn_s(
			"Attempting to start research for country \"{}\" on technology \"{}\" - cannot research this tech!",
			*this, technology
		);
		return;
	}

	current_research.set(&technology);
	invested_research_points.set(0);

	_update_current_tech(today);
}

void CountryInstance::apply_foreign_investments(
	fixed_point_map_t<CountryDefinition const*> const& investments, CountryInstanceManager const& country_instance_manager
) {
	for (auto const& [country, money_invested] : investments) {
		foreign_investments[country_instance_manager.get_country_instance_by_definition(*country)] = money_invested;
	}
}

bool CountryInstance::apply_history_to_country(
	CountryHistoryEntry const& entry,
	CountryInstanceManager const& country_instance_manager,
	FlagStrings& global_flags,
	MapInstance& map_instance
) {
	constexpr auto set_optional = []<typename T>(T& target, std::optional<T> const& source) {
		if (source) {
			target = *source;
		}
	};
	constexpr auto set_optional_state = []<typename T>(MutableState<T>& target, std::optional<T> const& source) {
		if (source) {
			target.set(*source);
		}
	};

	bool ret = true;

	set_optional(primary_culture, entry.get_primary_culture());
	for (auto const& [culture, add] : entry.get_accepted_cultures()) {
		if (add) {
			ret &= add_accepted_culture(*culture);
		} else {
			ret &= remove_accepted_culture(*culture);
		}
	}
	set_optional(religion, entry.get_religion());
	if (entry.get_ruling_party()) {
		ret &= set_ruling_party(**entry.get_ruling_party());
	}
	set_optional(last_election, entry.get_last_election());
	upper_house_proportion_by_ideology.copy_values_from(entry.get_upper_house_proportion_by_ideology());
	if (entry.get_capital()) {
		capital = &map_instance.get_province_instance_by_definition(**entry.get_capital());
	}
	set_optional_state(government_type, entry.get_government_type());
	set_optional_state(plurality, entry.get_plurality());
	set_optional_state(national_value, entry.get_national_value());
	if (entry.is_civilised()) {
		country_status = *entry.is_civilised() ? COUNTRY_STATUS_CIVILISED : COUNTRY_STATUS_UNCIVILISED;
	}
	set_optional_state(prestige, entry.get_prestige());
	for (Reform const* reform : entry.get_reforms()) {
		ret &= add_reform(*reform);
	}
	set_optional_state(tech_school, entry.get_tech_school());

	for (auto const& [technology, level] : entry.get_technologies()) {
		ret &= set_technology_unlock_level(*technology, level);
	}
	for (auto const& [invention, activated] : entry.get_inventions()) {
		ret &= set_invention_unlock_level(*invention, technology_unlock_level_t { static_cast<int8_t>(activated ? 1 : 0) });
	}

	apply_foreign_investments(entry.get_foreign_investment(), country_instance_manager);

	set_optional(releasable_vassal, entry.is_releasable_vassal());

	// TODO - entry.get_colonial_points();

	ret &= apply_flag_map(entry.get_country_flags(), true);
	ret &= global_flags.apply_flag_map(entry.get_global_flags(), true);
	flag_overrides_by_government_type.copy_values_from(entry.get_flag_overrides_by_government_type());
	for (Decision const* decision : entry.get_decisions()) {
		// TODO - take decision
	}

	return ret;
}

void CountryInstance::_update_production() {
	// Calculate industrial power from states and foreign investments
	fixed_point_t industrial_power_running_total = 0;
	industrial_power_from_states.clear();
	industrial_power_from_investments.clear();

	for (State const* state_ptr : states) {
		State const& state = *state_ptr;
		const fixed_point_t state_industrial_power = state.get_industrial_power();
		if (state_industrial_power != 0) {
			industrial_power += state_industrial_power;
			industrial_power_from_states.emplace_back(state, state_industrial_power);
		}
	}

	for (auto const& [country, money_invested] : foreign_investments) {
		if (country.get().exists()) {
			const fixed_point_t investment_industrial_power = fp::mul_div(
				money_invested,
				country_defines.get_country_investment_industrial_score_factor(),
				100
			);

			if (investment_industrial_power != 0) {
				industrial_power += investment_industrial_power;
				industrial_power_from_investments.emplace_back(country, investment_industrial_power);
			}
		}
	}

	industrial_power.set(industrial_power_running_total);

	std::stable_sort(
		industrial_power_from_states.begin(), industrial_power_from_states.end(),
		[](auto const& a, auto const& b) -> bool { return a.second > b.second; }
	);
	std::stable_sort(
		industrial_power_from_investments.begin(), industrial_power_from_investments.end(),
		[](auto const& a, auto const& b) -> bool { return a.second > b.second; }
	);
}

static inline constexpr fixed_point_t nonzero_or_one(fixed_point_t const& value) {
	return value == 0 ? fixed_point_t::_1 : value;
}

void CountryInstance::_update_budget() {
	const fixed_point_t min_tax = get_modifier_effect_value(*modifier_effect_cache.get_min_tax());
	const fixed_point_t max_tax = nonzero_or_one(get_modifier_effect_value(*modifier_effect_cache.get_max_tax()));

	for (ClampedValue& tax_rate_slider_value : tax_rate_slider_value_by_strata.get_values()) {
		tax_rate_slider_value.set_bounds(min_tax, max_tax);
	}

	// Education and administration spending sliders have no min/max modifiers

	social_spending_slider_value.set_bounds(
		get_modifier_effect_value(*modifier_effect_cache.get_min_social_spending()),
		nonzero_or_one(get_modifier_effect_value(*modifier_effect_cache.get_max_social_spending()))
	);
	military_spending_slider_value.set_bounds(
		get_modifier_effect_value(*modifier_effect_cache.get_min_military_spending()),
		nonzero_or_one(get_modifier_effect_value(*modifier_effect_cache.get_max_military_spending()))
	);
	tariff_rate_slider_value.set_bounds(
		get_modifier_effect_value(*modifier_effect_cache.get_min_tariff()),
		get_modifier_effect_value(*modifier_effect_cache.get_max_tariff())
	);

	// TODO - make sure we properly update everything dependent on these sliders' values,
	// as they might change if their sliders' bounds shrink past their previous values.

	tax_efficiency.set(
		country_defines.get_base_country_tax_efficiency()
		+ get_modifier_effect_value(*modifier_effect_cache.get_tax_efficiency())
		+ get_modifier_effect_value(*modifier_effect_cache.get_tax_eff()) / 100
	);

	TypedSpan<pop_type_index_t, const PopType> pop_types(
		shared_country_values.get_shared_pop_type_values().get_keys()
	);

	/*
	In Victoria 2, administration efficiency is updated in the UI immediately.
	However the corruption_cost_multiplier is only updated after 2 ticks.

	OpenVic immediately updates both.
	*/
	pop_sum_t total_non_colonial_population = 0;
	pop_sum_t administrators = 0;
	for (State const* const state_ptr : states) {
		if (state_ptr == nullptr) {
			continue;
		}

		State const& state = *state_ptr;
		if (state.is_colonial_state()) {
			continue;
		}

		{
			pop_type_index_t pop_type_index {};
			for (const pop_sum_t size : state.get_population_by_type()) {
				if (pop_types[pop_type_index].is_administrator) {
					administrators += size;
				}
				++pop_type_index;
			}
		}
		total_non_colonial_population += state.get_total_population();
	}

	if (total_non_colonial_population == 0) {
		administrative_efficiency_from_administrators.set(fixed_point_t::_1);
		administrator_percentage.set(fixed_point_t::_0);
	} else {
		administrator_percentage.set(fp::from_fraction<pop_sum_t>(administrators, total_non_colonial_population));

		const pop_sum_t desired_administrators = fp::multiply_truncate(
			total_non_colonial_population,
			desired_administrator_percentage.get_untracked()
		);
		const pop_sum_t effective_administrators = fp::multiply_truncate(
			administrators,
			fixed_point_t::_1 + get_modifier_effect_value(*modifier_effect_cache.get_administrative_efficiency())
		);
		const fixed_point_t administrative_efficiency_from_administrators_unclamped =
			desired_administrators == 0
			? fixed_point_t::_1
			: std::min(
				fp::from_fraction(effective_administrators, desired_administrators)
					* (fixed_point_t::_1 + get_modifier_effect_value(*modifier_effect_cache.get_administrative_efficiency_modifier())),
				fixed_point_t::_1
			);

		administrative_efficiency_from_administrators.set(
			game_rules_manager.get_prevent_negative_administration_efficiency()
			? std::max(fixed_point_t::_0, administrative_efficiency_from_administrators_unclamped)
			: administrative_efficiency_from_administrators_unclamped
		);
	}

	int64_t projected_administration_spending_unscaled_by_slider_running_total = 0;
	int64_t projected_education_spending_unscaled_by_slider_running_total = 0;
	int64_t projected_military_spending_unscaled_by_slider_running_total = 0;
	int64_t projected_pensions_spending_unscaled_by_slider_running_total = 0;
	int64_t projected_unemployment_subsidies_spending_unscaled_by_slider_running_total = 0;

	{
		// The same for every type of pop: looked up once, not for every type
		const fixed_point_t corruption_cost_multiplier_copy = corruption_cost_multiplier.get_untracked();
		const fixed_point_t pension_level = get_modifier_effect_value(*modifier_effect_cache.get_pension_level());
		const fixed_point_t unemployment_benefit = get_modifier_effect_value(*modifier_effect_cache.get_unemployment_benefit());

		pop_type_index_t pop_type_index {};
		for (const pop_sum_t pop_size : get_population_by_type()) {
			PopType const& pop_type = pop_types[pop_type_index];
			SharedPopTypeValues const& pop_type_values = shared_country_values.get_shared_pop_type_values(pop_type);
			const int64_t size = type_safe::get(pop_size);
			const fixed_point_t social_income_variant_base =
				corruption_cost_multiplier_copy * pop_type_values.get_social_income_variant_base_untracked();
			projected_administration_spending_unscaled_by_slider_running_total += size
				* (corruption_cost_multiplier_copy * pop_type_values.get_administration_salary_base_untracked()).get_raw_value();
			projected_education_spending_unscaled_by_slider_running_total += size
				* (corruption_cost_multiplier_copy * pop_type_values.get_education_salary_base_untracked()).get_raw_value();
			projected_military_spending_unscaled_by_slider_running_total += size
				* (corruption_cost_multiplier_copy * pop_type_values.get_military_salary_base_untracked()).get_raw_value();
			projected_pensions_spending_unscaled_by_slider_running_total += size
				* (pension_level * social_income_variant_base).get_raw_value();
			projected_unemployment_subsidies_spending_unscaled_by_slider_running_total += type_safe::get(
					get_unemployed_pops_by_type()[pop_type_index]
				)
				* (unemployment_benefit * social_income_variant_base).get_raw_value();
			++pop_type_index;
		}
	}

	const int64_t denominator = type_safe::get(Pop::size_denominator);
	projected_administration_spending_unscaled_by_slider.set(fixed_point_t::parse_raw(
		projected_administration_spending_unscaled_by_slider_running_total / denominator
	));
	projected_education_spending_unscaled_by_slider.set(fixed_point_t::parse_raw(
		projected_education_spending_unscaled_by_slider_running_total /	denominator
	));
	projected_military_spending_unscaled_by_slider.set(fixed_point_t::parse_raw(
		projected_military_spending_unscaled_by_slider_running_total / denominator
	));
	projected_pensions_spending_unscaled_by_slider.set(fixed_point_t::parse_raw(
		projected_pensions_spending_unscaled_by_slider_running_total / denominator
	));
	projected_unemployment_subsidies_spending_unscaled_by_slider.set(fixed_point_t::parse_raw(
		projected_unemployment_subsidies_spending_unscaled_by_slider_running_total / denominator
	));
}

fixed_point_t CountryInstance::get_administration_salary_base(PopType const& pop_type) {
	return corruption_cost_multiplier.get_untracked()
		* shared_country_values.get_shared_pop_type_values(pop_type).get_administration_salary_base_untracked();
}
fixed_point_t CountryInstance::get_education_salary_base(PopType const& pop_type) {
	return corruption_cost_multiplier.get_untracked()
		* shared_country_values.get_shared_pop_type_values(pop_type).get_education_salary_base_untracked();
}
fixed_point_t CountryInstance::get_military_salary_base(PopType const& pop_type) {
	return corruption_cost_multiplier.get_untracked()
		* shared_country_values.get_shared_pop_type_values(pop_type).get_military_salary_base_untracked();
}
fixed_point_t CountryInstance::get_social_income_variant_base(PopType const& pop_type) {
	return corruption_cost_multiplier.get_untracked()
		* shared_country_values.get_shared_pop_type_values(pop_type).get_social_income_variant_base_untracked();
}

fixed_point_t CountryInstance::calculate_pensions_base(PopType const& pop_type) {
	return get_modifier_effect_value(*modifier_effect_cache.get_pension_level())
		* get_social_income_variant_base(pop_type);
}
fixed_point_t CountryInstance::calculate_unemployment_subsidies_base(PopType const& pop_type) {
	return get_modifier_effect_value(*modifier_effect_cache.get_unemployment_benefit())
		* get_social_income_variant_base(pop_type);
}
fixed_point_t CountryInstance::calculate_minimum_wage_base(PopType const& pop_type) {
	if (pop_type.is_slave) {
		return 0;
	}
	return get_modifier_effect_value(*modifier_effect_cache.get_minimum_wage())
		* get_social_income_variant_base(pop_type);
}

void CountryInstance::_update_current_tech(const Date today) {
	Technology const* current_research_copy = current_research.get_untracked();
	if (current_research_copy == nullptr) {
		return;
	}

	current_research_cost.set(
		calculate_research_cost(*current_research_copy)
	);

	const fixed_point_t daily_research_points_copy = daily_research_points.get_untracked();
	if (daily_research_points_copy > 0) {
		expected_research_completion_date.set(
			today
			+ static_cast<Timespan>(
				(
					(current_research_cost.get_untracked() - invested_research_points.get_untracked())
					/ daily_research_points_copy
				).ceil<int64_t>()
			)
		);
	} else {
		expected_research_completion_date.set(fallback_date_for_never_completing_research);
	}
}

void CountryInstance::_update_technology(const Date today) {
	if (research_point_stockpile.get_untracked() < 0) {
		research_point_stockpile.set(0);
	}

	daily_research_points += get_modifier_effect_value(*modifier_effect_cache.get_research_points());
	daily_research_points *= fixed_point_t::_1 +
		get_modifier_effect_value(*modifier_effect_cache.get_research_points_modifier()) +
		get_modifier_effect_value(*modifier_effect_cache.get_increase_research());

	if (daily_research_points.get_untracked() < 0) {
		daily_research_points.set(0);
	}

	_update_current_tech(today);
}

void CountryInstance::_update_politics() {

}

void CountryInstance::_update_population() {
	clear_pops_aggregate();

	for (State* const state : states) {
		add_pops_aggregate(*state);
	}

	normalise_pops_aggregate();

	daily_research_points.set(0);
	monthly_leadership_points = 0;
	research_points_from_pop_types.clear();
	leadership_points_from_pop_types.clear();

	TypedSpan<pop_type_index_t, const PopType> pop_types(
		shared_country_values.get_shared_pop_type_values().get_keys()
	);
	{
		pop_type_index_t pop_type_index {};
		for (const pop_sum_t pop_size : get_population_by_type()) {
			PopType const& pop_type = pop_types[pop_type_index];
			if (pop_type.research_leadership_optimum > 0 && pop_size > 0) {
				const pop_sum_t optimum_size = fp::multiply_truncate(
					get_total_population(),
					pop_type.research_leadership_optimum
				);
				const fixed_point_t factor = optimum_size == 0
					? fixed_point_t::_1
					: std::min(
						fp::from_fraction(
							pop_size,
							optimum_size
						), fixed_point_t::_1
					);

				if (pop_type.research_points != 0) {
					const fixed_point_t research_points = pop_type.research_points * factor;
					research_points_from_pop_types[&pop_type] = research_points;
					daily_research_points += research_points;
				}

				if (pop_type.leadership_points != 0) {
					const fixed_point_t leadership_points = pop_type.leadership_points * factor;
					leadership_points_from_pop_types[&pop_type] = leadership_points;
					monthly_leadership_points = leadership_points;
				}
			}
			++pop_type_index;
		}
	}

	// TODO - update national focus capacity
}

void CountryInstance::_update_diplomacy() {
	// TODO - add prestige from modifiers
	// TODO - update diplomatic points and colonial power
}

void CountryInstance::_update_military() {
	regiment_count = 0;

	for (ArmyInstance const& army : armies) {
		regiment_count += army.get_unit_count();
	}

	ship_count = 0;
	total_consumed_ship_supply = 0;

	for (NavyInstance const& navy : navies) {
		ship_count += navy.get_unit_count();
		total_consumed_ship_supply += navy.get_total_consumed_supply();
	}

	// Calculate military power from land, sea, and leaders

	size_t deployed_non_mobilised_regiments = 0;
	for (ArmyInstance const& army : armies) {
		for (RegimentInstance const& regiment : army.get_regiment_instances()) {
			if (!regiment.is_mobilised()) {
				deployed_non_mobilised_regiments++;
			}
		}
	}

	supply_consumption = fixed_point_t::_1 + get_modifier_effect_value(*modifier_effect_cache.get_supply_consumption());

	// The supply limit of the provinces for the armies of this country
	supply_limit_global_base = get_modifier_effect_value(*modifier_effect_cache.get_supply_limit_global_base());
	supply_limit_multiplier = std::max(
		fixed_point_t::_0,
		fixed_point_t::_1 + get_modifier_effect_value(*modifier_effect_cache.get_supply_limit_global_percentage_change())
	) * get_supply_limit_research_multiplier();
	supply_home_bonus = fixed_point_t::_0;
	if (openvic_supply_defines != nullptr) {
		supply_home_bonus = openvic_supply_defines->supply_home_bonus.fixed();
		if (has_supply_home_bonus_technology()) {
			supply_home_bonus += openvic_supply_defines->supply_home_bonus_tech.fixed();
		}
	}

	const size_t regular_army_size = std::min(4 * deployed_non_mobilised_regiments, get_max_supported_regiment_count());

	fixed_point_t sum_of_regiment_type_stats = 0;
	for (RegimentType const& regiment_type : unit_type_manager.get_regiment_types()) {
		// TODO - apply country/tech modifiers to regiment stats
		sum_of_regiment_type_stats += (
			regiment_type.attack + regiment_type.defence /*+ land_attack_modifier + land_defense_modifier*/
		) * regiment_type.discipline;
	}

	military_power_from_land.set(
		supply_consumption * fp::mul_div(
			sum_of_regiment_type_stats,
			fixed_point_t::parse_raw(regular_army_size),
			fixed_point_t::parse_raw(7 * (1 + unit_type_manager.get_regiment_type_count()))
		)
	);

	if (disarmed) {
		military_power_from_land *= diplomacy_defines.get_disarmed_penalty();
	}

	fixed_point_t military_power_from_sea_running_total = 0;
	for (NavyInstance const& navy : navies) {
		for (ShipInstance const& ship : navy.get_ship_instances()) {
			ShipType const& ship_type = ship.get_ship_type();

			if (ship_type.is_capital) {

				// TODO - include gun power and hull modifiers + naval attack and defense modifiers

				military_power_from_sea_running_total += (ship_type.gun_power /*+ naval_attack_modifier*/)
					* (ship_type.hull /* + naval_defense_modifier*/);
			}
		}
	}
	military_power_from_sea.set(military_power_from_sea_running_total / 250);

	military_power_from_leaders.set(
		fixed_point_t::parse_capped(
			std::min(
				get_leader_count(),
				deployed_non_mobilised_regiments
			)
		)
	);

	// Mobilisation calculations
	mobilisation_impact = get_modifier_effect_value(*modifier_effect_cache.get_mobilization_impact());
	mobilisation_economy_impact = get_modifier_effect_value(*modifier_effect_cache.get_mobilisation_economy_impact_tech()) +
		get_modifier_effect_value(*modifier_effect_cache.get_mobilisation_economy_impact_country());

	// TODO - use country_defines.get_min_mobilize_limit(); (wiki: "lowest maximum of brigades you can mobilize. (by default 3)")

	mobilisation_max_regiment_count = regiment_count
		+ fp::multiply_truncate(regiment_count, mobilisation_impact);

	mobilisation_potential_regiment_count = 0; // TODO - calculate max regiments from poor citizens
	if (mobilisation_potential_regiment_count > mobilisation_max_regiment_count) {
		mobilisation_potential_regiment_count = mobilisation_max_regiment_count;
	}

	// Limit max war exhaustion to non-negative values. This technically diverges from the base game where
	// max war exhaustion can be negative, but in such cases the war exhaustion clamping behaviour is
	// very buggy and regardless it doesn't seem to make any modifier effect contributions.
	war_exhaustion_max = std::max(
		get_modifier_effect_value(*modifier_effect_cache.get_max_war_exhaustion()), fixed_point_t::_0
	);

	organisation_regain = fixed_point_t::_1 +
		get_modifier_effect_value(*modifier_effect_cache.get_org_regain()) +
		get_modifier_effect_value(*modifier_effect_cache.get_morale_global());

	land_organisation = fixed_point_t::_1 + get_modifier_effect_value(*modifier_effect_cache.get_land_organisation());
	naval_organisation = fixed_point_t::_1 + get_modifier_effect_value(*modifier_effect_cache.get_naval_organisation());

	land_unit_start_experience = get_modifier_effect_value(*modifier_effect_cache.get_regular_experience_level());
	naval_unit_start_experience = land_unit_start_experience;
	land_unit_start_experience += get_modifier_effect_value(*modifier_effect_cache.get_land_unit_start_experience());
	naval_unit_start_experience += get_modifier_effect_value(*modifier_effect_cache.get_naval_unit_start_experience());

	recruit_time = fixed_point_t::_1 + get_modifier_effect_value(*modifier_effect_cache.get_unit_recruitment_time());
	combat_width = fp::multiply_truncate(
		military_defines.get_base_combat_width(),
		get_modifier_effect_value(*modifier_effect_cache.get_combat_width_additive())
	);
	dig_in_cap = get_modifier_effect_value(*modifier_effect_cache.get_dig_in_cap()).floor<int32_t>();
	military_tactics = military_defines.get_base_military_tactics() +
		get_modifier_effect_value(*modifier_effect_cache.get_military_tactics());

	if (leadership_point_stockpile < 0) {
		leadership_point_stockpile = 0;
	}
	create_leader_count = (leadership_point_stockpile / military_defines.get_leader_recruit_cost()).floor<int32_t>();

	monthly_leadership_points += get_modifier_effect_value(*modifier_effect_cache.get_leadership());
	monthly_leadership_points *= fixed_point_t::_1 +
		get_modifier_effect_value(*modifier_effect_cache.get_leadership_modifier());

	if (monthly_leadership_points < 0) {
		monthly_leadership_points = 0;
	}

	// TODO - update max_ship_supply
}

bool CountryInstance::update_rule_set() {
	rule_set = {};
	CountryParty const* ruling_party_copy = ruling_party.get_untracked();
	if (ruling_party_copy != nullptr) {
		for (PartyPolicy const* party_policy : ruling_party_copy->get_policies()) {
			if (party_policy != nullptr) {
				rule_set.add_ruleset(party_policy->rules);
			}
		}
	}

	for (Reform const* reform : reforms.get_values()) {
		if (reform != nullptr) {
			rule_set.add_ruleset(reform->rules);
		}
	}

	return true;
}

static constexpr Modifier const& get_country_status_static_effect(
	CountryInstance::country_status_t country_status, StaticModifierCache const& static_modifier_cache
) {
	using enum CountryInstance::country_status_t;

	switch (country_status) {
	case COUNTRY_STATUS_GREAT_POWER:     return static_modifier_cache.get_great_power();
	case COUNTRY_STATUS_SECONDARY_POWER: return static_modifier_cache.get_secondary_power();
	case COUNTRY_STATUS_CIVILISED:       return static_modifier_cache.get_civilised();
	default:                             return static_modifier_cache.get_uncivilised();
	}
}

void CountryInstance::update_modifier_sum(Date today, StaticModifierCache const& static_modifier_cache) {
	// Update sum of national modifiers
	modifier_sum.clear();

	// Add static modifiers
	modifier_sum.add_modifier(static_modifier_cache.get_base_modifier());
	modifier_sum.add_modifier(get_country_status_static_effect(country_status, static_modifier_cache));
	if (is_disarmed()) {
		modifier_sum.add_modifier(static_modifier_cache.get_disarming());
	}
	modifier_sum.add_modifier(static_modifier_cache.get_war_exhaustion(), war_exhaustion);
	modifier_sum.add_modifier(static_modifier_cache.get_infamy(), infamy.get_untracked());
	modifier_sum.add_modifier(static_modifier_cache.get_literacy(), get_average_literacy());
	modifier_sum.add_modifier(static_modifier_cache.get_plurality(), plurality.get_untracked());
	modifier_sum.add_modifier(is_at_war() ? static_modifier_cache.get_war() : static_modifier_cache.get_peace());
	// TODO - difficulty modifiers, debt_default_to, bad_debtor, generalised_debt_default,
	//        total_occupation, total_blockaded, in_bankruptcy

	// TODO - handle triggered modifiers

	CountryParty const* ruling_party_copy = ruling_party.get_untracked();
	if (ruling_party_copy != nullptr) {
		for (PartyPolicy const* party_policy : ruling_party_copy->get_policies()) {
			// The ruling party's issues here could be null as they're stored in an FixedVector which has
			// values for every PartyPolicyGroup regardless of whether or not they have a policy set.
			if (party_policy != nullptr) {
				modifier_sum.add_modifier(*party_policy);
			}
		}
	}

	for (Reform const* reform : reforms.get_values()) {
		// The country's reforms here could be null as they're stored in an FixedVector which has
		// values for every ReformGroup regardless of whether or not they have a reform set.
		if (reform != nullptr) {
			modifier_sum.add_modifier(*reform);
		}
	}

	TechnologySchool const* tech_school_copy = tech_school.get_untracked();
	if (tech_school_copy != nullptr) {
		modifier_sum.add_modifier(*tech_school_copy);
	}

	for (Technology const& technology : technology_unlock_levels.get_keys()) {
		if (is_technology_unlocked(technology)) {
			modifier_sum.add_modifier(technology);
		}
	}

	for (Invention const& invention : invention_unlock_levels.get_keys()) {
		if (is_invention_unlocked(invention)) {
			modifier_sum.add_modifier(invention);
		}
	}

	for (Modifier const* const unlock_modifier : unlock_modifiers) {
		modifier_sum.add_modifier(*unlock_modifier);
	}

	// Erase expired event modifiers and add non-expired ones to the sum
	std::erase_if(event_modifiers, [this, today](ModifierInstance const& modifier) -> bool {
		if (today <= modifier.get_expiry_date()) {
			modifier_sum.add_modifier(*modifier.get_modifier());
			return false;
		} else {
			return true;
		}
	});

	NationalValue const* national_value_copy = national_value.get_untracked();
	if (national_value_copy != nullptr) {
		modifier_sum.add_modifier(*national_value_copy);
	}

	// TODO - calculate stats for each unit type (locked and unlocked)
}

void CountryInstance::make_room_for_province_modifier_sum(ModifierSum const& province_modifier_sum) {
	modifier_sum.make_room_for(province_modifier_sum);
}

void CountryInstance::contribute_province_modifier_sum(ModifierSum const& province_modifier_sum) {
	modifier_sum.add_modifier_sum(province_modifier_sum);
}

bool CountryInstance::grant_unlock(std::string_view key) {
	if (!is_valid_unlock_key(key)) {
		return false;
	}
	const auto it = std::lower_bound(
		unlock_keys.begin(), unlock_keys.end(), key, [](memory::string const& held, std::string_view value) -> bool {
			return std::string_view { held } < value;
		}
	);
	if (it != unlock_keys.end() && std::string_view { *it } == key) {
		return false;
	}
	unlock_keys.emplace(it, key);
	return true;
}

bool CountryInstance::revoke_unlock(std::string_view key) {
	const auto it = std::lower_bound(
		unlock_keys.begin(), unlock_keys.end(), key, [](memory::string const& held, std::string_view value) -> bool {
			return std::string_view { held } < value;
		}
	);
	if (it == unlock_keys.end() || std::string_view { *it } != key) {
		return false;
	}
	unlock_keys.erase(it);
	return true;
}

bool CountryInstance::has_unlock(std::string_view key) const {
	return std::binary_search(
		unlock_keys.begin(), unlock_keys.end(), key, [](auto const& a, auto const& b) -> bool {
			return std::string_view { a } < std::string_view { b };
		}
	);
}

bool CountryInstance::has_army_technology(const std::uint32_t technology_index) const {
	return std::binary_search(army_technologies_researched.begin(), army_technologies_researched.end(), technology_index);
}

bool CountryInstance::mark_army_technology_researched(const std::uint32_t technology_index) {
	const auto it = std::lower_bound(army_technologies_researched.begin(), army_technologies_researched.end(), technology_index);
	if (it != army_technologies_researched.end() && *it == technology_index) {
		return false;
	}
	army_technologies_researched.insert(it, technology_index);
	return true;
}

bool CountryInstance::unmark_army_technology_researched(const std::uint32_t technology_index) {
	const auto it = std::lower_bound(army_technologies_researched.begin(), army_technologies_researched.end(), technology_index);
	if (it == army_technologies_researched.end() || *it != technology_index) {
		return false;
	}
	army_technologies_researched.erase(it);
	return true;
}

bool CountryInstance::add_unlock_modifier(Modifier const& modifier) {
	if (std::find(unlock_modifiers.begin(), unlock_modifiers.end(), &modifier) != unlock_modifiers.end()) {
		return false;
	}
	unlock_modifiers.push_back(&modifier);
	return true;
}

bool CountryInstance::remove_unlock_modifier(Modifier const& modifier) {
	const auto it = std::find(unlock_modifiers.begin(), unlock_modifiers.end(), &modifier);
	if (it == unlock_modifiers.end()) {
		return false;
	}
	unlock_modifiers.erase(it);
	return true;
}

fixed_point_t CountryInstance::get_modifier_effect_value(ModifierEffect const& effect) const {
	return modifier_sum.get_modifier_effect_value(effect);
}

void CountryInstance::update_gamestate(const Date today, MapInstance& map_instance) {
	if (is_civilised()) {
		civilisation_progress = 0;
	} else {
		civilisation_progress = get_modifier_effect_value(*modifier_effect_cache.get_civilization_progress_modifier());

		if (civilisation_progress <= PRIMITIVE_CIVILISATION_PROGRESS) {
			country_status = COUNTRY_STATUS_PRIMITIVE;
		} else if (civilisation_progress <= UNCIVILISED_CIVILISATION_PROGRESS) {
			country_status = COUNTRY_STATUS_UNCIVILISED;
		} else {
			country_status = COUNTRY_STATUS_PARTIALLY_CIVILISED;
		}
	}

	owns_colonial_province = std::any_of(
		owned_provinces.begin(), owned_provinces.end(), std::bind_front(&ProvinceInstance::is_colonial_province)
	);

	{
		has_unowned_cores = false;
		owned_cores_controlled_proportion = 0;
		int32_t owned_core_province_count = 0;

		for (ProvinceInstance const* core_province : core_provinces) {
			if (core_province->get_owner() == this) {
				owned_core_province_count++;

				if (core_province->get_controller() == this) {
					owned_cores_controlled_proportion++;
				}
			} else {
				has_unowned_cores = true;
			}
		}

		if (owned_cores_controlled_proportion != 0) {
			owned_cores_controlled_proportion /= owned_core_province_count;
		}
	}

	occupied_provinces_proportion = 0;
	port_count = 0;
	neighbouring_countries.clear();

	Continent const* capital_continent = capital != nullptr ? capital->province_definition.get_continent() : nullptr;

	coastal = false;
	for (ProvinceInstance* province : owned_provinces) {
		ProvinceDefinition const& province_definition = province->province_definition;
		coastal |= province_definition.is_coastal();

		if (province->get_controller() != this) {
			occupied_provinces_proportion++;
		}

		if (province_definition.has_port()) {
			port_count++;
		}

		// TODO - ensure connected_to_capital and is_overseas are updated to false if a province becomes uncolonised
		province->set_connected_to_capital(false);
		province->set_is_overseas(province_definition.get_continent() != capital_continent);

		for (ProvinceDefinition::adjacency_t const& adjacency : province_definition.get_adjacencies()) {
			// TODO - should we limit based on adjacency type? Straits and impassable still work in game,
			// and water provinces don't have an owner so they'll get caught by the later checks anyway.
			CountryInstance* neighbour = map_instance.get_province_instance_by_definition(adjacency.get_to()).get_owner();
			if (neighbour != nullptr && neighbour != this) {
				neighbouring_countries.insert(neighbour);
			}
		}
	}

	if (occupied_provinces_proportion != 0) {
		assert(owned_provinces.size() <= std::numeric_limits<int32_t>::max());
		occupied_provinces_proportion /= static_cast<int32_t>(owned_provinces.size());
	}

	if (capital != nullptr) {
		capital->set_connected_to_capital(true);
		memory::vector<std::reference_wrapper<const ProvinceInstance>> province_checklist { *capital };

		for (size_t index = 0; index < province_checklist.size(); ++index) {
			ProvinceInstance const& province = province_checklist[index];

			for (ProvinceDefinition::adjacency_t const& adjacency : province.province_definition.get_adjacencies()) {
				ProvinceInstance& adjacent_province = map_instance.get_province_instance_by_definition(adjacency.get_to());

				if (adjacent_province.get_owner() == this && !adjacent_province.get_connected_to_capital()) {
					adjacent_province.set_connected_to_capital(true);
					adjacent_province.set_is_overseas(false);
					province_checklist.emplace_back(adjacent_province);
				}
			}
		}
	}

	// Order of updates might need to be changed/functions split up to account for dependencies
	// Updates population stats (including research and leadership points from pops)
	_update_population();
	// Calculates industrial power
	_update_production();
	// Calculates daily research points and predicts research completion date
	_update_technology(today);
	// Calculates national military modifiers, army and navy stats, daily leadership points
	_update_military();
	_update_budget();

	// These don't do anything yet
	_update_politics();
	_update_diplomacy();

	const CountryDefinition::government_colour_map_t::const_iterator it =
		country_definition.get_alternative_colours().find(government_type.get_untracked());

	if (it != country_definition.get_alternative_colours().end()) {
		colour = it.value();
	} else {
		colour = country_definition.get_colour();
	}
}

void CountryInstance::after_buy(void* actor, BuyResult const& buy_result) {
	const fixed_point_t quantity_bought = buy_result.quantity_bought;

	if (quantity_bought <= 0) {
		return;
	}

	CountryInstance& country = *static_cast<CountryInstance*>(actor);
	good_data_t& good_data = country.goods_data.at_index(buy_result.good_index);
	const fixed_point_t money_spent = buy_result.money_spent_total;
	country.cash_stockpile -= money_spent;
	MoneyLedger::record(MoneyLedger::label_t::country_stockpile_buys, -money_spent);
	country.actual_national_stockpile_spending += money_spent;
	good_data.stockpile_amount += quantity_bought;
	good_data.stockpile_change_yesterday += quantity_bought;
	good_data.quantity_traded_yesterday = quantity_bought;
	good_data.money_traded_yesterday = -money_spent;
}

void CountryInstance::after_sell(void* actor, SellResult const& sell_result, memory::vector<fixed_point_t>& reusable_vector) {
	const fixed_point_t quantity_sold = sell_result.quantity_sold;

	if (quantity_sold <= 0) {
		return;
	}

	CountryInstance& country = *static_cast<CountryInstance*>(actor);
	good_data_t& good_data = country.goods_data.at_index(sell_result.good_index);
	const fixed_point_t money_gained = sell_result.money_gained;
	MoneyLedger::record(MoneyLedger::label_t::sell_callbacks_countries, money_gained);
	country.cash_stockpile += money_gained;
	MoneyLedger::record(MoneyLedger::label_t::country_stockpile_sells, money_gained);
	country.actual_national_stockpile_income += money_gained;
	good_data.stockpile_amount -= quantity_sold;
	good_data.stockpile_change_yesterday -= quantity_sold;
	good_data.quantity_traded_yesterday = -quantity_sold;
	good_data.money_traded_yesterday = money_gained;
}

void CountryInstance::country_tick_before_map(
	const Date today,
	TypedSpan<good_index_t, char> reusable_goods_mask,
	forwardable_span<
		memory::vector<fixed_point_t>,
		VECTORS_FOR_COUNTRY_TICK
	> reusable_vectors,
	memory::vector<good_index_t>& reusable_good_index_vector
) {
	//TODO AI sliders
	// + reparations + war subsidies
	// + industrial subsidies
	// + loan interest

	// The modifiers do not change during the tick: the levels the pops of the country get their welfare by are looked up once, not for every pop
	pension_level_today = get_modifier_effect_value(*modifier_effect_cache.get_pension_level());
	unemployment_benefit_today = get_modifier_effect_value(*modifier_effect_cache.get_unemployment_benefit());

	fixed_point_t available_funds = cash_stockpile_start_of_tick = cash_stockpile;
	const fixed_point_t projected_administration_spending_copy = projected_administration_spending.get_untracked();
	const fixed_point_t projected_education_spending_copy = projected_education_spending.get_untracked();
	const fixed_point_t projected_military_spending_copy = projected_military_spending.get_untracked();
	const fixed_point_t projected_social_spending_copy = projected_social_spending.get_untracked();
	const fixed_point_t projected_import_subsidies_copy = projected_import_subsidies.get_untracked();
	//excluding national stockpile
	const fixed_point_t projected_total_spending = projected_spending.get_untracked();
	if (projected_total_spending <= available_funds) {
		actual_administration_budget = projected_administration_spending_copy;
		actual_education_budget = projected_education_spending_copy;
		actual_military_budget = projected_military_spending_copy;
		actual_social_budget = projected_social_spending_copy;
		actual_import_subsidies_budget = projected_import_subsidies_copy;
		available_funds -= projected_total_spending;
	} else {
		//TODO try take loan (callback?)
		//update available_funds with loan

		if (available_funds < projected_education_spending_copy) {
			actual_education_budget = available_funds;
			available_funds = 0;
			actual_administration_budget = 0;
			actual_military_budget = 0;
			actual_social_budget = 0;
			actual_import_subsidies_budget = 0;
		} else {
			available_funds -= projected_education_spending_copy;
			actual_education_budget = projected_education_spending_copy;

			if (available_funds < projected_administration_spending_copy) {
				actual_administration_budget = available_funds;
				available_funds = 0;
				actual_military_budget = 0;
				actual_social_budget = 0;
				actual_import_subsidies_budget = 0;
			} else {
				available_funds -= projected_administration_spending_copy;
				actual_administration_budget = projected_administration_spending_copy;

				if (available_funds < projected_social_spending_copy) {
					actual_social_budget = available_funds;
					available_funds = 0;
					actual_military_budget = 0;
					actual_import_subsidies_budget = 0;
				} else {
					available_funds -= projected_social_spending_copy;
					actual_social_budget = projected_social_spending_copy;

					if (available_funds < projected_military_spending_copy) {
						actual_military_budget = available_funds;
						available_funds = 0;
						actual_import_subsidies_budget = 0;
					} else {
						available_funds -= projected_military_spending_copy;
						actual_military_budget = projected_military_spending_copy;

						actual_import_subsidies_budget = std::min(available_funds, projected_import_subsidies_copy);
						available_funds -= actual_import_subsidies_budget;
					}
				}
			}
		}
	}

	was_administration_budget_cut_yesterday = actual_administration_budget < projected_administration_spending_copy;
	was_education_budget_cut_yesterday = actual_education_budget < projected_education_spending_copy;
	was_military_budget_cut_yesterday = actual_military_budget < projected_military_spending_copy;
	was_social_budget_cut_yesterday = actual_social_budget < projected_social_spending_copy;
	was_import_subsidies_budget_cut_yesterday = actual_import_subsidies_budget < projected_import_subsidies_copy;

	for (auto [good_instance, good_data] : goods_data) {
		good_data.clear_daily_recorded_data();
	}

	calculate_government_good_needs(today);

	manage_national_stockpile(
		reusable_goods_mask,
		reusable_vectors,
		reusable_good_index_vector,
		available_funds
	);

	// What is left is free to be spent on subsidies and loans during the day
	free_funds = available_funds;

	//TODO market maker orders

	taxable_income_by_pop_type.fill(0);
	actual_administration_spending
		= actual_education_spending
		= actual_military_spending
		= actual_pensions_spending
		= actual_unemployment_subsidies_spending
		= actual_import_subsidies_spending
		= actual_tariff_income
		= actual_transport_income
		= treasury_other_income
		= treasury_other_spending
		= treasury_subsidy_spending
		= actual_national_stockpile_spending
		= actual_national_stockpile_income
		= actual_tax_income
	 	= 0;
}

void CountryInstance::update_regiment_equipment() {
	if (equipment_manager == nullptr || good_definition_manager == nullptr || openvic_war_defines == nullptr
		|| equipment_manager->get_equipment().empty()) {
		return;
	}
	OpenVicWarDefines const& war = *openvic_war_defines;
	std::int32_t upgrades_left = war.equipment_upgrades_per_day;
	std::int32_t needs_left = war.equipment_upgrades_per_day; // the regiments whose weapons the government buys the goods of today

	// What a regiment type needs, found once a day for every type: the weapons it may have, what the country can give it
	struct type_cache_t {
		bool looked_up = false;
		memory::vector<EquipmentDefinition const*> allowed;
		UnitWeapon choice;
		GoodDefinition const* base_good = nullptr;
		fixed_point_t pieces = fixed_point_t::_0;
	};
	std::array<type_cache_t, 32> caches;

	for (ArmyInstance& army : armies) {
		for (RegimentInstance& regiment : army.get_regiment_instances()) {
			RegimentType const& regiment_type = regiment.get_regiment_type();
			const std::size_t kind = static_cast<std::size_t>(type_safe::get(regiment_type.index));
			if (kind >= caches.size()) {
				continue;
			}
			type_cache_t& cache = caches[kind];
			if (!cache.looked_up) {
				cache.looked_up = true;
				cache.allowed = get_allowed_weapons(*equipment_manager, regiment_type);
				cache.choice = choose_unit_weapon(*equipment_manager, *this, regiment_type, cache.allowed, war.equipment_below_min_penalty.fixed());
				if (cache.choice.initial != nullptr) {
					cache.base_good = good_definition_manager->get_good_definition_by_identifier(cache.choice.initial->good.identifier);
					// The pieces of the weapon a regiment needs: what its construction took of the good of the weapon it is built with
					for (auto const& [good, quantity] : regiment_type.get_build_cost()) {
						if (good == cache.base_good) {
							cache.pieces = quantity;
						}
					}
				}
			}
			if (cache.choice.initial == nullptr) {
				continue;
			}
			EquipmentDefinition const* const initial = cache.choice.initial;
			EquipmentDefinition const* const best = cache.choice.best; // nullptr: the country has none of the allowed weapons
			const std::uint32_t current_tier = regiment.get_weapon() != nullptr ? regiment.get_weapon()->tier : initial->tier;

			if (best != nullptr && current_tier < best->tier && cache.base_good != nullptr && cache.pieces > fixed_point_t::_0) {
				if (upgrades_left > 0) {
					// The best allowed generation the stockpile can give
					for (std::size_t i = cache.allowed.size(); i-- > 0;) {
						EquipmentDefinition const* const candidate = cache.allowed[i];
						if (candidate->tier <= current_tier || candidate->tier > best->tier || !has_unlock(candidate->unlock_key)) {
							continue;
						}
						GoodDefinition const* const good = good_definition_manager->get_good_definition_by_identifier(candidate->good.identifier);
						if (good == nullptr) {
							continue;
						}
						good_data_t& good_data = get_good_data(*good);
						if (good_data.stockpile_amount < cache.pieces) {
							continue;
						}
						good_data.stockpile_amount -= cache.pieces;
						good_data.stockpile_change_yesterday -= cache.pieces;
						regiment.set_weapon(candidate);
						regiment.set_weapon_base_good(cache.base_good);
						regiment.set_weapon_good(good);
						upgrades_left--;
						break;
					}
				}
				// The goods of the next regiments are bought at the market
				if ((regiment.get_weapon() == nullptr || regiment.get_weapon()->tier < best->tier) && needs_left > 0) {
					if (GoodDefinition const* const good = good_definition_manager->get_good_definition_by_identifier(best->good.identifier);
						good != nullptr) {
						good_data_t& good_data = get_good_data(*good);
						good_data.government_needs += cache.pieces;
						good_data.max_government_consumption += cache.pieces;
					}
					needs_left--;
				}
			}

			// What the weapon is worth in the battle: the multipliers of the weapon the regiment has (what was given to it, else the weapon it is built
			// with, which is the nearest lower generation with the penalty when the country does not have it)
			EquipmentDefinition const* equipped = regiment.get_weapon();
			fixed_point_t keep = fixed_point_t::_1;
			if (equipped == nullptr) {
				equipped = cache.choice.best != nullptr ? initial : cache.choice.effective;
				keep = cache.choice.best != nullptr ? fixed_point_t::_1 : cache.choice.keep;
			}
			regiment.set_weapon_attack_ratio(equipped->attack_mult * keep);
			regiment.set_weapon_defence_ratio(equipped->defense_mult * keep);
			regiment.set_weapon_breakthrough_ratio(equipped->breakthrough_mult * keep);
		}
	}
}

void CountryInstance::calculate_government_good_needs(const Date today) {
	// Days of supplies the government tries to keep in the national stockpile
	const int32_t DAYS_OF_SUPPLY = openvic_economy_defines->supply_reserve_days;

	// Army and navy: every unit uses every month a share of what its construction cost (see UnitType::supply_items and the
	// defines SUPPLY_*), spread over the days of the month, more when its group moves or stands on the land of an enemy, scaled by
	// the army/navy spending slider. The goods come out of the national stockpile and the government keeps a reserve of them.
	// TODO - construction (buildings, factories, recruitment) should add its goods needs here as well, see the pseudocode:
	//for each construction/recruitment
	//	for each good
	//		remainder = resources_total - already_consumed
	//		daily_needs = exact_vic_2_game_rule
	//			? slider * min(resources_total / spread_cost_days, remainder)
	//			: min(1, slider * days_since_start / spread_cost_days) * resources_total
	//		government_needs += daily_needs
	//		max_government_consumption += remainder
	// Every unit uses its supplies every day. What the stockpile is kept ready for and what is used up today is the same
	const auto add_unit_needs = [this](
		UnitInstance const& unit, supply_state_t const& state, const fixed_point_t scale, const bool is_army,
		RegimentInstance const* regiment = nullptr
	) -> void {
		if (scale <= fixed_point_t::_0) {
			return;
		}
		for_each_daily_supply(unit.unit_type, is_army, state, scale, [this, is_army](GoodDefinition const& good, const fixed_point_t daily_need) {
			good_data_t& good_data = goods_data.at_index(good.index);
			(is_army ? good_data.army_needs : good_data.navy_needs) += daily_need;
			(is_army ? good_data.army_due : good_data.navy_due) += daily_need;
		}, regiment);
	};

	update_regiment_equipment();

	const fixed_point_t army_slider = army_spending_slider_value.get_value_untracked();
	const fixed_point_t navy_slider = navy_spending_slider_value.get_value_untracked();
	const fixed_point_t reserve_capacity = get_supply_reserve_capacity();
	const fixed_point_t reserve_refill = openvic_supply_defines->supply_unit_reserve_refill_per_day;

	// The armies: what is sent is the need times what the priority sends (the delivery of the day was worked out by the supply of the
	// provinces, see military/SupplyDelivery.hpp), and a regiment whose reserve is not full and whose army gets all it needs fills it
	for (ArmyInstance& army : armies) {
		const supply_state_t state = get_supply_state(army);
		const bool full_delivery = army.get_supply_delivery() >= openvic_supply_defines->supply_full_threshold.fixed();
		for (RegimentInstance& regiment : army.get_regiment_instances()) {
			if (regiment.get_supply_reserve() < fixed_point_t::_0) {
				regiment.set_supply_reserve(reserve_capacity); // the units come supplied
			}
			add_unit_needs(regiment, state, army_slider * army.get_supply_send_multiplier(), true, &regiment);
			if (full_delivery && regiment.get_supply_reserve() < reserve_capacity) {
				const fixed_point_t refill = std::min(reserve_refill, reserve_capacity - regiment.get_supply_reserve());
				add_unit_needs(regiment, state, army_slider * refill, true, &regiment);
			}
		}
	}
	// The support companies of the divisions of the templates use their goods every day (the fallback type, when the goods of the type are not in the
	// stockpile at all, uses its own goods); how much the stockpile gave is worked out below, when the goods are used up
	memory::vector<std::size_t> company_works_as;
	if (company_type_manager != nullptr && company_type_manager->size() > 0) {
		const std::size_t company_count = company_type_manager->size();
		memory::vector<std::uint32_t> company_totals(company_count, 0);
		for (ArmyInstance const& army : armies) {
			for (DivisionInstance const& division : army.get_divisions()) {
				DivisionTemplate const* const division_template = division.template_id != 0 ? find_division_template(division.template_id) : nullptr;
				if (division_template == nullptr) {
					continue;
				}
				const memory::vector<std::uint32_t> counts = count_template_companies(*company_type_manager, *division_template);
				for (std::size_t index = 0; index < company_count; ++index) {
					company_totals[index] += counts[index];
				}
			}
		}
		company_works_as.resize(company_count);
		for (std::size_t index = 0; index < company_count; ++index) {
			CompanyType const& company = company_type_manager->at(index);
			company_works_as[index] = index;
			if (company.fallback_index != SIZE_MAX && !company.goods.empty()) {
				bool any_in_stock = false;
				for (auto const& [good, amount] : company.goods) {
					any_in_stock |= goods_data.at_index(good->index).stockpile_amount > fixed_point_t::_0;
				}
				if (!any_in_stock) {
					company_works_as[index] = company.fallback_index;
				}
			}
			if (company_totals[index] == 0) {
				continue;
			}
			for (auto const& [good, amount] : company_type_manager->at(company_works_as[index]).goods) {
				good_data_t& good_data = goods_data.at_index(good->index);
				const fixed_point_t need = amount * static_cast<std::int32_t>(company_totals[index]) * army_slider;
				good_data.army_needs += need;
				good_data.army_due += need;
			}
		}
	}
	for (NavyInstance const& navy : navies) {
		const supply_state_t state = get_supply_state(navy);
		for (ShipInstance const& ship : navy.get_ship_instances()) {
			add_unit_needs(ship, state, navy_slider, false);
		}
	}

	// Factories under construction use goods of the stockpile (the construction slider is the allocation to the
	// investment fund, it does not slow the construction down)
	{
		for (State const* state : states) {
			for (FactoryInstance const& factory : state->get_factories()) {
				if (!factory.is_construction_using_stockpile()) {
					continue;
				}
				for (auto const& [good, quantity] : factory.building_type.goods_cost) {
					good_data_t& good_data = goods_data.at_index(good->index);
					// Consumed by the factory when it builds, so it only has to be kept in the stockpile
					const fixed_point_t daily_need = factory.get_daily_construction_goods(*good);
					good_data.government_needs += daily_need * openvic_economy_defines->construction_reserve_days;
					good_data.max_government_consumption += daily_need * openvic_economy_defines->construction_reserve_days;
				}
			}
		}
	}

	fixed_point_t army_needed_total = 0;
	fixed_point_t army_supplied_total = 0;
	fixed_point_t navy_needed_total = 0;
	fixed_point_t navy_supplied_total = 0;

	std::map<std::size_t, fixed_point_t> supplied_share; // by the index of the good; a good that is not here was given in full
	for (auto [good_instance, good_data] : goods_data) {
		const fixed_point_t daily_need = good_data.army_needs + good_data.navy_needs;
		const fixed_point_t due = good_data.army_due + good_data.navy_due;

		// The supplies of the day are used up
		if (due > fixed_point_t::_0) {
			const fixed_point_t consumed = std::min(due, good_data.stockpile_amount);
			supplied_share[static_cast<std::size_t>(type_safe::get(good_instance.index))] = consumed / due;
			good_data.stockpile_amount -= consumed;
			good_data.stockpile_change_yesterday -= consumed;

			// What the units got of what they needed
			army_needed_total += good_data.army_due;
			army_supplied_total += consumed * good_data.army_due / due;
			navy_needed_total += good_data.navy_due;
			navy_supplied_total += consumed * good_data.navy_due / due;
		}

		if (daily_need > fixed_point_t::_0) {
			good_data.government_needs += daily_need * DAYS_OF_SUPPLY;
			good_data.max_government_consumption += daily_need * DAYS_OF_SUPPLY;
		}
	}

	// The companies: what each type works as today and how much of its goods it got (the slider of the army counts as it does for the regiments)
	if (!company_works_as.empty()) {
		const fixed_point_t company_paid_share = std::clamp(army_slider, fixed_point_t::_0, fixed_point_t::_1);
		company_states.resize(company_works_as.size());
		for (std::size_t index = 0; index < company_works_as.size(); ++index) {
			fixed_point_t supplied = company_paid_share;
			for (auto const& [good, amount] : company_type_manager->at(company_works_as[index]).goods) {
				const auto share_it = supplied_share.find(static_cast<std::size_t>(type_safe::get(good->index)));
				if (share_it != supplied_share.end()) {
					supplied = std::min(supplied, share_it->second);
				}
			}
			company_states[index] = { company_works_as[index], supplied };
		}
	}

	// The share of the goods that the stockpile could give (the same for all armies)
	const fixed_point_t goods_share = army_needed_total > fixed_point_t::_0
		? std::min(fixed_point_t::_1, army_supplied_total / army_needed_total) : fixed_point_t::_1;
	if (army_needed_total > fixed_point_t::_0) {
		army_supply_ratio = goods_share;
	}
	if (navy_needed_total > fixed_point_t::_0) {
		navy_supply_ratio = navy_supplied_total / navy_needed_total;
	}

	// The supply level goes slowly to its target
	const fixed_point_t level_step = openvic_economy_defines->supply_level_change_per_day.of(fixed_point_t::_1);
	const auto move_level = [level_step](fixed_point_t& level, const fixed_point_t target) -> void {
		level = level < target ? std::min(target, level + level_step) : std::max(target, level - level_step);
	};

	// The armies: what arrived is the delivery (the supply limit and the priority) times what the stockpile gave times the slider; what did
	// not arrive is taken from the reserves of the regiments, and when a reserve is used up the supply level of the army goes down
	const fixed_point_t paid_share = std::clamp(army_slider, fixed_point_t::_0, fixed_point_t::_1);
	fixed_point_t level_sum = 0;
	fixed_point_t ratio_sum = 0;
	std::int32_t regiment_total = 0;
	for (ArmyInstance& army : armies) {
		const std::int32_t count = static_cast<std::int32_t>(army.get_unit_count());
		if (count == 0) {
			continue;
		}
		const fixed_point_t arrived = std::clamp(army.get_supply_delivery() * goods_share * paid_share, fixed_point_t::_0, fixed_point_t::_1);
		// An army that gets almost all it needs is fully supplied (the stockpile is seldom exactly enough)
		const bool full = arrived >= openvic_supply_defines->supply_full_threshold.fixed();
		const fixed_point_t shortfall = full ? fixed_point_t::_0 : fixed_point_t::_1 - arrived;
		fixed_point_t deficit_sum = 0;
		fixed_point_t reserve_sum = 0;
		for (RegimentInstance& regiment : army.get_regiment_instances()) {
			// What the regiment gets to fill its reserve is what the stockpile gave of it
			const fixed_point_t refill = full ? reserve_refill * goods_share : fixed_point_t::_0;
			const SupplyReserveResult result = step_supply_reserve(regiment.get_supply_reserve(), shortfall, refill, reserve_capacity);
			regiment.set_supply_reserve(result.reserve);
			deficit_sum += result.deficit;
			reserve_sum += result.reserve;
		}
		army.set_supply_ratio(arrived);
		army.set_supply_reserve_average(reserve_sum / count);
		fixed_point_t level = army.get_supply_level();
		move_level(level, std::clamp(fixed_point_t::_1 - deficit_sum / count, fixed_point_t::_0, fixed_point_t::_1));
		army.set_supply_level(level);

		level_sum += level * count;
		ratio_sum += arrived * count;
		regiment_total += count;
	}
	// The values of the country (the budget, the tooltips): the average of the armies
	if (regiment_total > 0) {
		army_supply_level = level_sum / regiment_total;
		army_supply_ratio = ratio_sum / regiment_total;
	} else {
		army_supply_level = fixed_point_t::_1;
		army_supply_ratio = fixed_point_t::_1;
	}
	move_level(navy_supply_level, navy_slider * navy_supply_ratio);
}

fixed_point_t CountryInstance::get_supply_reserve_capacity() const {
	if (openvic_supply_defines == nullptr) {
		return fixed_point_t::_0;
	}
	// TODO research: a stub, the days come from the technologies of the organisation of the army and of the deep strikes and assaults
	fixed_point_t days = fixed_point_t::_0;
	if (has_army_organisation()) {
		days += openvic_supply_defines->supply_unit_reserve_days_organisation;
	}
	if (has_deep_assault_technology()) {
		days += openvic_supply_defines->supply_unit_reserve_days_deep_assault;
	}
	if (has_late_supply_reserve_technology()) {
		days += openvic_supply_defines->supply_unit_reserve_days_late;
	}
	return days;
}

void CountryInstance::manage_national_stockpile(
	TypedSpan<good_index_t, char> reusable_goods_mask,
	forwardable_span<
		memory::vector<fixed_point_t>,
		VECTORS_FOR_COUNTRY_TICK
	> reusable_vectors,
	memory::vector<good_index_t>& reusable_good_index_vector,
	fixed_point_t& available_funds
) {
	TypedSpan<good_index_t, char> wants_more_mask = reusable_goods_mask;
	const size_t mask_size = type_safe::get(wants_more_mask.size());

	reusable_vectors[0].resize(mask_size, 0);
	TypedSpan<good_index_t, fixed_point_t> max_quantity_to_buy_per_good { reusable_vectors[0] };

	reusable_vectors[1].resize(mask_size, 0);
	TypedSpan<good_index_t, fixed_point_t> max_costs_per_good { reusable_vectors[1] };

	reusable_vectors[2].resize(mask_size, 0);
	TypedSpan<good_index_t, fixed_point_t> weights { reusable_vectors[2] };

	memory::vector<good_index_t>& good_indices_to_buy = reusable_good_index_vector;
	fixed_point_t weights_sum = 0;

	for (auto [good_instance, good_data] : goods_data) {
		const good_index_t good_index = good_instance.index;

		// Automated (AI): keep only what the government needs (nothing while no needs are calculated), sell the excess
		// and buy up what is missing. Manual: buy up to the stockpile level of the slider, or sell down to it.
		const fixed_point_t target_stockpile = good_data.is_automated
			? good_data.government_needs
			: good_data.stockpile_cutoff;
		const bool buys = good_data.is_automated
			? good_data.stockpile_amount < target_stockpile
			: !good_data.is_selling;

		if (buys) {
			const fixed_point_t quantity_to_allocate_for = target_stockpile - good_data.stockpile_amount;
			const fixed_point_t max_quantity_to_buy = good_data.is_automated
				? std::max(good_data.max_government_consumption, target_stockpile) - good_data.stockpile_amount
				: quantity_to_allocate_for;
			if (max_quantity_to_buy <= 0 || quantity_to_allocate_for <= 0 || available_funds <= 0) {
				continue;
			}

			good_indices_to_buy.push_back(good_index);
			max_quantity_to_buy_per_good[good_index] = max_quantity_to_buy;

			const fixed_point_t max_money_to_spend = max_costs_per_good[good_index] = market_instance.get_max_money_to_allocate_to_buy_quantity(
				good_index,
				quantity_to_allocate_for
			);
			if (max_money_to_spend <= 0) {
				good_indices_to_buy.pop_back();
				continue;
			}
			wants_more_mask[good_index] = true;
			const fixed_point_t weight = weights[good_index] = fixed_point_t::usable_max / max_money_to_spend;
			weights_sum += weight;
		} else {
			const fixed_point_t quantity_to_sell = good_data.stockpile_amount - target_stockpile;
			if (quantity_to_sell <= 0 || !market_instance.get_is_available(good_index)) {
				continue;
			}
			market_instance.place_market_sell_order(
				{
					good_index,
					index,
					quantity_to_sell,
					this,
					after_sell,
					capital != nullptr ? capital->get_freight_rate() : fixed_point_t::_0,
				},
				reusable_vectors[3] //temporarily used here and later used as money_to_spend_per_good
			);
		}
	}

	if (weights_sum > 0) {
		reusable_vectors[3].resize(mask_size, 0);
		TypedSpan<good_index_t, fixed_point_t> money_to_spend_per_good { reusable_vectors[3] };

		fixed_point_t cash_left_to_spend_draft = available_funds;
		bool needs_redistribution = true;
		while (needs_redistribution) {
			needs_redistribution = false;
			for (const good_index_t good_index : good_indices_to_buy) {
				char& wants_more = wants_more_mask[good_index];
				if (!wants_more) {
					continue;
				}

				const fixed_point_t weight = weights[good_index];
				const fixed_point_t max_costs = max_costs_per_good[good_index];

				fixed_point_t cash_available_for_good = fp::mul_div(
					cash_left_to_spend_draft,
					weight,
					weights_sum
				);

				if (cash_available_for_good >= max_costs) {
					cash_left_to_spend_draft -= max_costs;
					money_to_spend_per_good[good_index] = max_costs;
					weights_sum -= weight;
					wants_more = false;
					needs_redistribution = weights_sum > 0;
					break;
				}

				const fixed_point_t max_possible_quantity_bought = cash_available_for_good / market_instance.get_min_next_price(good_index);
				if (max_possible_quantity_bought < fixed_point_t::epsilon) {
					money_to_spend_per_good[good_index] = 0;
				} else {
					money_to_spend_per_good[good_index] = cash_available_for_good;
				}
			}
		}

		for (const good_index_t good_index : good_indices_to_buy) {
			const fixed_point_t max_quantity_to_buy = max_quantity_to_buy_per_good[good_index];
			const fixed_point_t money_to_spend = money_to_spend_per_good[good_index];
			if (money_to_spend <= 0) {
				continue;
			}

			available_funds -= money_to_spend;
			market_instance.place_buy_up_to_order(
				{
					good_index,
					index,
					max_quantity_to_buy,
					money_to_spend,
					this,
					after_buy
				}
			);
		}
	}

	std::fill(reusable_goods_mask.begin(), reusable_goods_mask.end(), 0);
	for (auto& reusable_vector : reusable_vectors) {
		reusable_vector.clear();
	}
	reusable_good_index_vector.clear();
}

void CountryInstance::publish_need_consumption() {
	// While verifying, the map was filled by the reports (the old way) and is only compared with the flat sums
	const bool verify = verify_need_consumption;
	auto const& pop_types = shared_country_values.get_shared_pop_type_values().get_keys();
	for (auto [good_instance, good_data] : goods_data) {
		if (!good_data.has_need_consumption_reports) {
			continue;
		}
		std::size_t pop_types_with_consumption = 0;
		bool is_different = false;
		for (std::size_t i = 0; i < good_data.need_consumption_by_pop_type_index.size(); ++i) {
			const fixed_point_t quantity = good_data.need_consumption_by_pop_type_index[i];
			// Every report was more than zero
			if (quantity == fixed_point_t::_0) {
				continue;
			}
			pop_types_with_consumption++;
			if (verify) {
				const auto it = good_data.need_consumption_per_pop_type.find(&pop_types[i]);
				is_different |= it == good_data.need_consumption_per_pop_type.end() || it->second != quantity;
			} else {
				good_data.need_consumption_per_pop_type[&pop_types[i]] = quantity;
			}
		}
		if (verify) {
			is_different |= good_data.need_consumption_per_pop_type.size() != pop_types_with_consumption;
			if (is_different) {
				spdlog::error_s(
					"The consumption of the pops that country {} recorded for good {} is not the sum of its reports.", *this,
					good_instance.get_identifier()
				);
				++need_consumption_mismatch_count;
			}
		}
	}
}

void CountryInstance::country_tick_after_map(const Date today) {
	// The consumption of the pops of the day is complete: the market is over
	publish_need_consumption();

	// Gain daily research points
	research_point_stockpile += daily_research_points.get_untracked();

	Technology const* current_research_copy = current_research.get_untracked();
	if (current_research_copy != nullptr) {
		const fixed_point_t research_points_spent = std::min(
			std::min(
				research_point_stockpile.get_untracked(),
				current_research_cost.get_untracked() - invested_research_points.get_untracked()
			),
			economy_defines.get_max_daily_research()
		);

		research_point_stockpile -= research_points_spent;
		invested_research_points += research_points_spent;

		if (invested_research_points.get_untracked() >= current_research_cost.get_untracked()) {
			unlock_technology(*current_research_copy);
			current_research.set(nullptr);
			invested_research_points.set(0);
			current_research_cost.set(0);
		}
	}

	// Apply maximum research point stockpile limit
	const fixed_point_t max_research_point_stockpile = is_civilised()
		? daily_research_points.get_untracked() * static_cast<int32_t>(Date::DAYS_IN_YEAR)
		: country_defines.get_max_research_points();
	if (research_point_stockpile.get_untracked() > max_research_point_stockpile) {
		research_point_stockpile.set(max_research_point_stockpile);
	}

	// Gain monthly leadership points
	if (today.is_month_start()) {
		leadership_point_stockpile += monthly_leadership_points;
	}

	// TODO - auto create and auto assign leaders

	// Apply maximum leadership point stockpile limit
	const fixed_point_t max_leadership_point_stockpile = military_defines.get_max_leadership_point_stockpile();
	if (leadership_point_stockpile > max_leadership_point_stockpile) {
		leadership_point_stockpile = max_leadership_point_stockpile;
	}

	fixed_point_t total_gold_production = 0;
	for (auto const& [good_instance, data] : goods_data) {
		GoodDefinition const& good_definition = good_instance.good_definition;
		if (!good_definition.is_money) {
			continue;
		}

		for (auto const& [production_type, produced_quantity] : data.production_per_production_type) {
			if (production_type->template_type != ProductionType::template_type_t::RGO) {
				continue;
			}
			total_gold_production += produced_quantity;
		}
	}

	const fixed_point_t actual_administration_spending_copy = actual_administration_spending.load();
	if (OV_unlikely(actual_administration_spending_copy > actual_administration_budget)) {
		spdlog::error_s(
			"Country {} has overspend on administration. Spending {} instead of the allocated {}. This indicates a severe bug in the economy code.",
			*this, actual_administration_spending_copy, actual_administration_budget
		);
	}
	cash_stockpile -= actual_administration_spending;
	MoneyLedger::record(MoneyLedger::label_t::budget_administration, -actual_administration_spending_copy);

	const fixed_point_t actual_education_spending_copy = actual_education_spending.load();
	if (OV_unlikely(actual_education_spending_copy > actual_education_budget)) {
		spdlog::error_s(
			"Country {} has overspend on education. Spending {} instead of the allocated {}. This indicates a severe bug in the economy code.",
			*this, actual_education_spending_copy, actual_education_budget
		);
	}
	cash_stockpile -= actual_education_spending;
	MoneyLedger::record(MoneyLedger::label_t::budget_education, -actual_education_spending_copy);

	const fixed_point_t actual_military_spending_copy = actual_military_spending.load();
	if (OV_unlikely(actual_military_spending_copy > actual_military_budget)) {
		spdlog::error_s(
			"Country {} has overspend on military. Spending {} instead of the allocated {}. This indicates a severe bug in the economy code.",
			*this, actual_military_spending_copy, actual_military_budget
		);
	}
	cash_stockpile -= actual_military_spending;
	MoneyLedger::record(MoneyLedger::label_t::budget_military, -actual_military_spending_copy);

	const fixed_point_t actual_social_spending_copy = actual_pensions_spending.load() + actual_unemployment_subsidies_spending.load();
	if (OV_unlikely(actual_social_spending_copy > actual_social_budget)) {
		spdlog::error_s(
			"Country {} has overspend on pensions and/or unemployment subsidies. "
			"Spending {} on pensions and {} on unemployment subsidies instead of the total allocated {}. "
			"This indicates a severe bug in the economy code.",
			*this, actual_pensions_spending.load(), actual_unemployment_subsidies_spending.load(), actual_social_budget
		);
	}
	cash_stockpile -= actual_pensions_spending;
	cash_stockpile -= actual_unemployment_subsidies_spending;
	MoneyLedger::record(MoneyLedger::label_t::budget_pensions, -actual_pensions_spending.load());
	MoneyLedger::record(MoneyLedger::label_t::budget_unemployment_subsidies, -actual_unemployment_subsidies_spending.load());

	const fixed_point_t actual_import_subsidies_spending_copy = actual_import_subsidies_spending.load();
	if (OV_unlikely(actual_import_subsidies_spending_copy > actual_import_subsidies_budget)) {
		spdlog::error_s(
			"Country {} has overspend on import subsidies. Spending {} instead of the allocated {}. This indicates a severe bug in the economy code.",
			*this, actual_import_subsidies_spending_copy, actual_import_subsidies_budget
		);
	}
	cash_stockpile -= actual_import_subsidies_spending;
	MoneyLedger::record(MoneyLedger::label_t::budget_import_subsidies, -actual_import_subsidies_spending_copy);

	const fixed_point_t cash_stockpile_copy = cash_stockpile.load();
	if (OV_unlikely(cash_stockpile_copy < 0)) {
		spdlog::error_s(
			"Country {} has overspend resulting in a cash stockpile of {}. This indicates a severe bug in the economy code.",
			*this, cash_stockpile_copy
		);
	}

	cash_stockpile += actual_tariff_income;
	MoneyLedger::record(MoneyLedger::label_t::tariff_income, actual_tariff_income.load());
	// The freight of the sellers: a share goes to the investment fund (the state builds railroads and ports from it), the rest to the treasury
	const fixed_point_t transport_total = actual_transport_income.load();
	const fixed_point_t transport_to_fund = openvic_trade_defines->freight_fund_share.of(transport_total);
	const fixed_point_t transport_to_treasury = transport_total - transport_to_fund;
	cash_stockpile += transport_to_treasury;
	transport_to_treasury_last = transport_to_treasury;
	investment_fund += transport_to_fund;
	MoneyLedger::record(MoneyLedger::label_t::transport_fee_income, transport_total);

	const fixed_point_t gold_income_value = country_defines.get_gold_to_cash_rate() * total_gold_production;;
	gold_income.set(gold_income_value);
	cash_stockpile += gold_income_value;
	MoneyLedger::record(MoneyLedger::label_t::gold_income, gold_income_value);

	// The budget of the day, and the part of it that the construction slider sets aside for the investment fund
	income_yesterday = actual_tax_income.load() + actual_tariff_income.load() + transport_to_treasury + gold_income_value + actual_national_stockpile_income.load();
	investment_deposit_yesterday = fixed_point_t::_0;
	const fixed_point_t allocation = get_investment_allocation();
	// An AI country starts to set money aside once it knows its own balance (it sets its allocation from it)
	if (allocation > fixed_point_t::_0 && !(is_ai() && balance_samples == 0)) {
		// The money of the allocation is a part of the budget like the other expenses, the treasury cannot go below zero
		const fixed_point_t deposit = std::min(allocation, cash_stockpile.load());
		if (deposit > fixed_point_t::_0) {
			cash_stockpile -= deposit;
			investment_fund += deposit;
			investment_deposit_yesterday = deposit;
		}
	}
	// The money of the fund that nobody uses goes back to the treasury (and on to the pops by the budget): a fund that holds more than a
	// few months of the income of the country is not a plan any more but a hoard, and the pops are short of the money
	{
		const fixed_point_t fund_limit = income_yesterday * openvic_economy_defines->investment_fund_max_days;
		if (investment_fund > fund_limit) {
			const fixed_point_t released = openvic_economy_defines->investment_fund_release_share.of(investment_fund - fund_limit);
			if (released > fixed_point_t::_0) {
				investment_fund -= released;
				cash_stockpile += released;
			}
		}
	}
	free_funds = cash_stockpile.load();
	const fixed_point_t yesterdays_balance = cash_stockpile - cash_stockpile_start_of_tick;
	balance_history.push_back(yesterdays_balance);

	// The average balance without the deposit to the investment fund (the first days count fully)
	const std::int32_t average_days = openvic_economy_defines->investment_ai_average_days;
	if (balance_samples < average_days) {
		balance_samples++;
	}
	balance_before_investment_average += (yesterdays_balance + investment_deposit_yesterday - balance_before_investment_average) / balance_samples;
}

fixed_point_t CountryInstance::get_investment_allocation(const fixed_point_t slider_value) const {
	if (slider_value <= fixed_point_t::_0 || income_yesterday <= fixed_point_t::_0) {
		return fixed_point_t::_0;
	}
	return openvic_economy_defines->investment_budget_share.of(income_yesterday) * slider_value;
}

CountryInstance::good_data_t::good_data_t()
	: mutex { memory::make_unique<spin_mutex>() }
	{ }

void CountryInstance::good_data_t::clear_daily_recorded_data() {
	const std::lock_guard<spin_mutex> lock_guard { *mutex };
	stockpile_change_yesterday
		= quantity_traded_yesterday
		= money_traded_yesterday
		= exported_amount
		= max_government_consumption
		= government_needs
		= army_needs
		= navy_needs
		= army_due
		= navy_due
		= overseas_maintenance
		= factory_demand
		= pop_demand
		= available_amount
		= 0;
	need_consumption_per_pop_type.clear();
	has_need_consumption_reports = false;
	input_consumption_per_production_type.clear();
	production_per_production_type.clear();
}

void CountryInstance::report_pop_income_tax(PopType const& pop_type, const fixed_point_t gross_income, const fixed_point_t paid_as_tax) {
	const std::lock_guard<spin_mutex> lock_guard { taxable_income_mutex };
	taxable_income_by_pop_type.at(pop_type) += gross_income;
	cash_stockpile += paid_as_tax;
	actual_tax_income += paid_as_tax;
	MoneyLedger::record(MoneyLedger::label_t::tax_income, paid_as_tax);
}

void CountryInstance::report_pop_need_consumption(PopType const& pop_type, const good_index_t good_index, const fixed_point_t quantity) {
	good_data_t& good_data = goods_data.at_index(good_index);
	const std::lock_guard<spin_mutex> lock_guard { *good_data.mutex };
	if (OV_unlikely(!good_data.has_need_consumption_reports)) {
		good_data.has_need_consumption_reports = true;
		good_data.need_consumption_by_pop_type_index.assign(
			shared_country_values.get_shared_pop_type_values().get_keys().size(), fixed_point_t::_0
		);
	}
	good_data.need_consumption_by_pop_type_index[static_cast<std::size_t>(type_safe::get(pop_type.index))] += quantity;
	if (OV_unlikely(verify_need_consumption)) {
		good_data.need_consumption_per_pop_type[&pop_type] += quantity;
	}
}
void CountryInstance::report_pop_need_demand(PopType const& pop_type, const good_index_t good_index, const fixed_point_t quantity) {
	good_data_t& good_data = goods_data.at_index(good_index);
	const std::lock_guard<spin_mutex> lock_guard { *good_data.mutex };
	good_data.pop_demand += quantity;
}
void CountryInstance::report_pop_need_demand_total(const good_index_t good_index, const fixed_point_t quantity) {
	good_data_t& good_data = goods_data.at_index(good_index);
	const std::lock_guard<spin_mutex> lock_guard { *good_data.mutex };
	good_data.pop_demand += quantity;
}
void CountryInstance::report_input_consumption(ProductionType const& production_type, const good_index_t good_index, const fixed_point_t quantity) {
	good_data_t& good_data = goods_data.at_index(good_index);
	const std::lock_guard<spin_mutex> lock_guard { *good_data.mutex };
	good_data.input_consumption_per_production_type[&production_type] += quantity;
}
void CountryInstance::report_input_demand(ProductionType const& production_type, const good_index_t good_index, const fixed_point_t quantity) {
	good_data_t& good_data = goods_data.at_index(good_index);

	if (production_type.template_type == ProductionType::template_type_t::ARTISAN) {
		switch (game_rules_manager.get_artisanal_input_demand_category()) {
			case demand_category::FactoryNeeds: break;
			case demand_category::PopNeeds: {
				const std::lock_guard<spin_mutex> lock_guard { *good_data.mutex };
				good_data.pop_demand += quantity;
				return;
			}
			default: return; //demand_category::None
		}
	}

	const std::lock_guard<spin_mutex> lock_guard { *good_data.mutex };
	good_data.factory_demand += quantity;
}
void CountryInstance::report_output(ProductionType const& production_type, const fixed_point_t quantity) {
	good_data_t& good_data = get_good_data(production_type.output_good);
	const std::lock_guard<spin_mutex> lock_guard { *good_data.mutex };
	good_data.production_per_production_type[&production_type] += quantity;
}

void CountryInstance::request_salaries_and_welfare_and_import_subsidies(Pop& pop) {
	PopType const& pop_type = pop.get_type();
	const pop_size_t pop_size = pop.get_size();
	SharedPopTypeValues const& pop_type_values = shared_country_values.get_shared_pop_type_values(pop_type);
	// Not changed while the pops are ticked, and the same for all the pops of the country
	const fixed_point_t corruption_cost_multiplier_copy = corruption_cost_multiplier.get_untracked();

	if (actual_administration_budget > 0) {
		const fixed_point_t administration_salary = fp::mul_div(
			pop_size * (corruption_cost_multiplier_copy * pop_type_values.get_administration_salary_base_untracked()),
			actual_administration_budget,
			projected_administration_spending_unscaled_by_slider.get_untracked()
		) / Pop::size_denominator;
		if (administration_salary > 0) {
			pop.add_government_salary_administration(administration_salary);
			actual_administration_spending += administration_salary;
		}
	}

	if (actual_education_budget > 0) {
		const fixed_point_t education_salary = fp::mul_div(
			pop_size * (corruption_cost_multiplier_copy * pop_type_values.get_education_salary_base_untracked()),
			actual_education_budget,
			projected_education_spending_unscaled_by_slider.get_untracked()
		) / Pop::size_denominator;
		if (education_salary > 0) {
			pop.add_government_salary_education(education_salary);
			actual_education_spending += education_salary;
		}
	}

	if (actual_military_budget > 0) {
		const fixed_point_t military_salary = fp::mul_div(
			pop_size * (corruption_cost_multiplier_copy * pop_type_values.get_military_salary_base_untracked()),
			actual_military_budget,
			projected_military_spending_unscaled_by_slider.get_untracked()
		) / Pop::size_denominator;
		if (military_salary > 0) {
			pop.add_government_salary_military(military_salary);
			actual_military_spending += military_salary;
		}
	}

	if (actual_social_budget > 0) {
		const fixed_point_t social_income_variant_base =
			corruption_cost_multiplier_copy * pop_type_values.get_social_income_variant_base_untracked();
		const fixed_point_t pension_income = fp::mul_div(
			pop_size * (pension_level_today * social_income_variant_base),
			actual_social_budget,
			projected_social_spending_unscaled_by_slider.get_untracked()
		) / Pop::size_denominator;
		if (pension_income > 0) {
			pop.add_pensions(pension_income);
			actual_pensions_spending += pension_income;
		}

		const fixed_point_t unemployment_subsidies = fp::mul_div(
			pop.get_unemployed() * (unemployment_benefit_today * social_income_variant_base),
			actual_social_budget,
			projected_social_spending_unscaled_by_slider.get_untracked()
		) / Pop::size_denominator;
		if (unemployment_subsidies > 0) {
			pop.add_unemployment_subsidies(unemployment_subsidies);
			actual_unemployment_subsidies_spending += unemployment_subsidies;
		}
	}

	if (actual_import_subsidies_budget > 0) {
		const fixed_point_t import_subsidies = fp::mul_div(
			effective_tariff_rate.get_untracked() // < 0
				* pop.get_yesterdays_import_value().get_copy_of_value(),
			actual_import_subsidies_budget, // < 0
			projected_import_subsidies.get_untracked() // > 0
		); //effective_tariff_rate * actual_net_tariffs cancel out the negative
		pop.add_import_subsidies(import_subsidies);
		actual_import_subsidies_spending += import_subsidies;
	}
}

fixed_point_t CountryInstance::apply_tariff(const fixed_point_t money_spent_on_imports) {
	const fixed_point_t effective_tariff_rate_value = effective_tariff_rate.get_untracked();
	if (effective_tariff_rate_value <= 0) {
		return 0;
	}

	const fixed_point_t tariff = effective_tariff_rate_value * money_spent_on_imports;
	actual_tariff_income += tariff;
	return tariff;
}

CountryInstance::supply_state_t CountryInstance::get_supply_state(UnitInstanceGroup const& group) const {
	ProvinceInstance const& location = group.get_location();
	supply_state_t state { get_supply_research_multiplier(), group.is_moving(), false };
	// A group that stands on land (a fleet in a port) is in a port
	state.in_port = !state.moving && !location.province_definition.is_water();
	// The land of an enemy: its owner (or the one who holds it) is at war with this country
	CountryInstance const* const owner = location.get_owner();
	CountryInstance const* const controller = location.get_controller();
	if ((owner != nullptr && is_at_war_with(*owner)) || (controller != nullptr && is_at_war_with(*controller))) {
		state.multiplier = state.multiplier * openvic_economy_defines->supply_enemy_land_multiplier;
	}
	return state;
}

fixed_point_t CountryInstance::calculate_army_supply_cost_per_day() const {
	fixed_point_t total = fixed_point_t::_0;
	for (ArmyInstance const& army : armies) {
		const supply_state_t state = get_supply_state(army);
		for (RegimentInstance const& regiment : army.get_regiment_instances()) {
			for_each_daily_supply(regiment.unit_type, true, state, fixed_point_t::_1, [this, &total](GoodDefinition const& good, const fixed_point_t daily_need) {
				total += daily_need * market_instance.get_good_instance(good.index).get_price();
			});
		}
	}
	return total;
}

fixed_point_t CountryInstance::calculate_navy_supply_cost_per_day() const {
	fixed_point_t total = fixed_point_t::_0;
	for (NavyInstance const& navy : navies) {
		const supply_state_t state = get_supply_state(navy);
		for (ShipInstance const& ship : navy.get_ship_instances()) {
			for_each_daily_supply(ship.unit_type, false, state, fixed_point_t::_1, [this, &total](GoodDefinition const& good, const fixed_point_t daily_need) {
				total += daily_need * market_instance.get_good_instance(good.index).get_price();
			});
		}
	}
	return total;
}

CountryInstance::good_data_t& CountryInstance::get_good_data(GoodInstance const& good_instance) {
	return goods_data.at(good_instance);
}
CountryInstance::good_data_t const& CountryInstance::get_good_data(GoodInstance const& good_instance) const {
	return goods_data.at(good_instance);
}
CountryInstance::good_data_t& CountryInstance::get_good_data(GoodDefinition const& good_definition) {
	return goods_data.at_index(good_definition.index);
}
CountryInstance::good_data_t const& CountryInstance::get_good_data(GoodDefinition const& good_definition) const {
	return goods_data.at_index(good_definition.index);
}

void CountryInstance::set_upper_house_proportion(Ideology const& ideology, const fixed_point_t value) {
	upper_house_proportion_by_ideology.at(ideology) = value;
}

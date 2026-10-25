#include "StateHash.hpp"

#include "openvic-simulation/InstanceManager.hpp"
#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/economy/GoodInstance.hpp"
#include "openvic-simulation/economy/production/FactoryInstance.hpp"
#include "openvic-simulation/map/MapInstance.hpp"
#include "openvic-simulation/map/ProvinceInstance.hpp"
#include "openvic-simulation/map/State.hpp"
#include "openvic-simulation/military/UnitInstanceGroup.hpp"
#include "openvic-simulation/military/War.hpp"
#include "openvic-simulation/population/Pop.hpp"

using namespace OpenVic;

namespace {
	struct Hasher {
		std::uint64_t value = 14695981039346656037ULL;

		void add(const std::uint64_t number) {
			for (int i = 0; i < 8; ++i) {
				value ^= (number >> (8 * i)) & 0xFF;
				value *= 1099511628211ULL;
			}
		}
		void add(fixed_point_t const number) {
			add(static_cast<std::uint64_t>(number.get_raw_value()));
		}
		void add_signed(const std::int64_t number) {
			add(static_cast<std::uint64_t>(number));
		}
	};
}

std::uint64_t StateHash::compute(InstanceManager& instance_manager) {
	Hasher hasher;
	hasher.add_signed((instance_manager.get_today() - Date {}).to_int());

	for (ProvinceInstance& province : instance_manager.get_map_instance().get_province_instances()) {
		CountryInstance const* const owner = province.get_owner();
		CountryInstance const* const controller = province.get_controller();
		hasher.add(owner != nullptr ? static_cast<std::uint64_t>(type_safe::get(owner->index)) + 1 : 0);
		hasher.add(controller != nullptr ? static_cast<std::uint64_t>(type_safe::get(controller->index)) + 1 : 0);
		for (Pop& pop : province.get_mutable_pops()) {
			hasher.add_signed(type_safe::get(pop.get_size()));
			hasher.add(pop.get_cash().get_copy_of_value());
			hasher.add(pop.get_militancy());
			hasher.add(pop.get_consciousness());
			hasher.add(pop.get_literacy());
		}
		for (ArmyInstance const& army : province.get_armies()) {
			hasher.add(army.unique_id);
			hasher.add(army.get_unit_count());
			hasher.add(army.get_total_strength());
			hasher.add(army.get_total_organisation());
		}
	}

	for (CountryInstance const& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		hasher.add(country.get_cash_stockpile().load());
		hasher.add(country.get_investment_fund());
		hasher.add(country.get_income_yesterday());
		hasher.add(country.get_balance_before_investment_average());
		hasher.add(country.get_prestige_untracked());
		hasher.add(country.is_at_war() ? 1 : 0);
		hasher.add(country.get_owned_provinces().size());
	}

	for (StateSet const& state_set : instance_manager.get_map_instance().get_state_manager().get_state_sets()) {
		for (State const& state : state_set.get_states()) {
			hasher.add(state.get_capitalist_fund());
			for (FactoryInstance const& factory : state.get_factories()) {
				hasher.add_signed(type_safe::get(factory.get_level()));
				hasher.add(factory.get_budget());
				hasher.add(factory.get_expansion_fund());
				hasher.add(factory.is_open() ? 1 : 0);
			}
		}
	}

	for (GoodInstance const& good : instance_manager.get_good_instance_manager().get_good_instances()) {
		hasher.add(good.get_price());
	}

	hasher.add(instance_manager.get_war_manager().get_wars().size());
	hasher.add(instance_manager.get_colonisation_manager().get_projects().size());
	hasher.add(instance_manager.get_notification_log().get_entries().size());
	return hasher.value;
}

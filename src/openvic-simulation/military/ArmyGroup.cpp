#include "ArmyGroup.hpp"

#include <algorithm>

#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/defines/OpenVicDefines.hpp"
#include "openvic-simulation/InstanceManager.hpp"
#include "openvic-simulation/military/Leader.hpp"
#include "openvic-simulation/military/LeaderTrait.hpp"
#include "openvic-simulation/military/UnitInstanceGroup.hpp"
#include "openvic-simulation/modifier/ModifierEffectCache.hpp"

using namespace OpenVic;

fixed_point_t OpenVic::calculate_leader_supply_bonus(
	LeaderInstance const& leader, ModifierEffectCache const& modifier_effect_cache, OpenVicSupplyDefines const& defines
) {
	fixed_point_t bonus = fixed_point_t::_0;
	if (modifier_effect_cache.get_attrition_leader() != nullptr) {
		for (LeaderTrait const* trait : { leader.get_personality(), leader.get_background() }) {
			if (trait != nullptr) {
				// The attrition of a trait is a penalty: negative is good
				bonus -= trait->get_effect(*modifier_effect_cache.get_attrition_leader()) * defines.supply_leader_attrition_factor;
			}
		}
	}
	bonus += leader.get_prestige() * defines.supply_leader_prestige_bonus.fixed();
	return bonus;
}

bool OpenVic::create_army_group(CountryInstance& country) {
	if (!country.has_army_organisation() || country.get_army_groups().size() >= MAX_ARMY_GROUPS) {
		return false;
	}
	country.get_mutable_army_groups().push_back({ country.take_army_group_id(), 0 });
	return true;
}

namespace {
	void free_marshal(InstanceManager& instance_manager, ArmyGroup& group) {
		if (group.marshal_id != 0) {
			LeaderInstance* const marshal = instance_manager.get_unit_instance_manager().get_leader_instance_by_unique_id(group.marshal_id);
			if (marshal != nullptr) {
				marshal->set_marshal(false);
			}
			group.marshal_id = 0;
		}
	}
}

bool OpenVic::delete_army_group(InstanceManager& instance_manager, CountryInstance& country, const std::uint32_t group_id) {
	memory::vector<ArmyGroup>& groups = country.get_mutable_army_groups();
	const auto it = std::find_if(groups.begin(), groups.end(), [group_id](ArmyGroup const& group) -> bool {
		return group.id == group_id;
	});
	if (it == groups.end()) {
		return false;
	}
	free_marshal(instance_manager, *it);
	groups.erase(it);
	for (ArmyInstance& army : country.get_armies()) {
		if (army.get_army_group_id() == group_id) {
			army.set_army_group_id(0);
		}
	}
	return true;
}

bool OpenVic::set_army_group(CountryInstance const& country, ArmyInstance& army, const std::uint32_t group_id, const std::uint32_t max_attached) {
	if (!country.has_army_organisation() || army.get_army_group_id() == group_id) {
		return false;
	}
	if (group_id != 0 && country.find_army_group(group_id) == nullptr) {
		return false;
	}
	if (group_id != 0) {
		std::uint32_t attached = 0;
		for (ArmyInstance const& other : country.get_armies()) {
			attached += other.get_army_group_id() == group_id;
		}
		if (attached >= max_attached) {
			return false;
		}
	}
	army.set_army_group_id(group_id);
	return true;
}

bool OpenVic::set_army_group_orders(CountryInstance& country, const std::uint32_t group_id, const bool execute) {
	if (!country.has_army_organisation()) {
		return false;
	}
	for (ArmyGroup& group : country.get_mutable_army_groups()) {
		if (group.id == group_id) {
			if (group.execute_orders == execute) {
				return false;
			}
			group.execute_orders = execute;
			return true;
		}
	}
	return false;
}

bool OpenVic::set_army_group_marshal(
	InstanceManager& instance_manager, CountryInstance& country, const std::uint32_t group_id, const unique_id_t leader_id
) {
	if (!country.has_army_organisation()) {
		return false;
	}
	ArmyGroup* group = nullptr;
	for (ArmyGroup& candidate : country.get_mutable_army_groups()) {
		if (candidate.id == group_id) {
			group = &candidate;
		}
	}
	if (group == nullptr || group->marshal_id == leader_id) {
		return false;
	}
	if (leader_id == 0) {
		free_marshal(instance_manager, *group);
		return true;
	}
	LeaderInstance* const leader = instance_manager.get_unit_instance_manager().get_leader_instance_by_unique_id(leader_id);
	if (leader == nullptr || leader->branch != unit_branch_t::LAND || &leader->country != &country || !leader->get_can_be_used()
		|| leader->is_marshal()) {
		return false;
	}
	// A general that leads an army leaves it (the army stays without a general until the auto assignment or the player gives it one)
	if (leader->get_unit_instance_group() != nullptr) {
		leader->get_unit_instance_group()->set_leader(nullptr);
	}
	free_marshal(instance_manager, *group);
	group->marshal_id = leader_id;
	leader->set_marshal(true);
	return true;
}

void OpenVic::update_army_groups(InstanceManager& instance_manager, CountryInstance& country) {
	memory::vector<ArmyGroup>& groups = country.get_mutable_army_groups();
	if (!country.has_army_organisation()) {
		// No technology, no groups (the generals stay, they are only free)
		for (ArmyGroup& group : groups) {
			free_marshal(instance_manager, group);
		}
		groups.clear();
		for (ArmyInstance& army : country.get_armies()) {
			army.set_army_group_id(0);
		}
		return;
	}
	for (ArmyGroup& group : groups) {
		if (group.marshal_id != 0 && instance_manager.get_unit_instance_manager().get_leader_instance_by_unique_id(group.marshal_id) == nullptr) {
			group.marshal_id = 0;
		}
	}
	for (ArmyInstance& army : country.get_armies()) {
		if (army.get_army_group_id() != 0 && country.find_army_group(army.get_army_group_id()) == nullptr) {
			army.set_army_group_id(0);
		}
	}
}

#pragma once

#include <cstdint>

#include "openvic-simulation/core/object/FixedPoint.hpp"
#include "openvic-simulation/types/UniqueId.hpp"
#include "openvic-simulation/types/UnitBranchType.hpp"

namespace OpenVic {
	struct CountryInstance;
	struct InstanceManager;
	struct LeaderInstance;
	struct ModifierEffectCache;
	struct OpenVicSupplyDefines;
	struct UnitInstanceManager;

	// Groups of armies and the field marshals (docs/PLAN_ARMY_SUPPLY_FRONTS.md, sections 1 and 4): a country with the technology of the
	// organisation of the army joins its armies into groups; a group has a field marshal, a general (that does not lead an army) that
	// gives a share of his bonus to the supply of all the armies of the group. A country without the technology has no groups.
	inline constexpr std::uint32_t MAX_ARMY_GROUPS = 12;

	struct ArmyGroup {
		std::uint32_t id = 0;
		unique_id_t marshal_id = 0; // the unique id of the general, 0 if there is none
	};

	// The bonus of a general to the supply of the army he leads (the third factor of the supply limit): the traits that reduce the attrition
	// (the attrition of a trait is negative when it is good) times SUPPLY_LEADER_ATTRITION_FACTOR plus the prestige times SUPPLY_LEADER_PRESTIGE_BONUS
	[[nodiscard]] fixed_point_t calculate_leader_supply_bonus(
		LeaderInstance const& leader, ModifierEffectCache const& modifier_effect_cache, OpenVicSupplyDefines const& defines
	);
	// The temporary debuffs of the supply (not in the game yet)
	[[nodiscard]] inline fixed_point_t calculate_temporary_supply_debuff(CountryInstance const&) {
		return fixed_point_t::_0;
	}

	// The commands (false if nothing changed or the command is not allowed)
	bool create_army_group(CountryInstance& country);
	bool delete_army_group(InstanceManager& instance_manager, CountryInstance& country, std::uint32_t group_id);
	// group_id 0 takes the army out of its group
	bool set_army_group(CountryInstance const& country, ArmyInstance& army, std::uint32_t group_id);
	// leader_id 0 takes the marshal off; the general must not be a marshal already and must be usable; if he leads an army he leaves it
	bool set_army_group_marshal(
		InstanceManager& instance_manager, CountryInstance& country, std::uint32_t group_id, unique_id_t leader_id
	);

	// Every day: the countries without the technology have no groups and no marshals; the groups lose what does not exist any more
	void update_army_groups(InstanceManager& instance_manager, CountryInstance& country);
}

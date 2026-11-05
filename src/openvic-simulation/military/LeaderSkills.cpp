#include "LeaderSkills.hpp"

#include <algorithm>

#include <fmt/format.h>

#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/DefinitionManager.hpp"
#include "openvic-simulation/defines/OpenVicDefines.hpp"
#include "openvic-simulation/InstanceManager.hpp"
#include "openvic-simulation/map/TerrainType.hpp"
#include "openvic-simulation/military/ArmyGroup.hpp"
#include "openvic-simulation/military/Leader.hpp"
#include "openvic-simulation/military/UnitInstanceGroup.hpp"

using namespace OpenVic;
using namespace OpenVic::leader_skills;

std::string_view leader_skills::get_skill_identifier(const skill_t skill, TerrainType const* const terrain) {
	switch (skill) {
	case skill_t::ATTACK: return "attack";
	case skill_t::DEFENCE: return "defence";
	case skill_t::SUPPLY: return "supply";
	case skill_t::PLANNING: return "planning";
	case skill_t::TERRAIN: return terrain != nullptr ? std::string_view { terrain->get_identifier() } : std::string_view { "terrain" };
	}
	return {};
}

fixed_point_t leader_skills::experience_for_level(const std::uint32_t level, const std::int32_t per_level) {
	const std::int32_t n = static_cast<std::int32_t>(level);
	return fixed_point_t(per_level * n * (n + 1) / 2);
}

std::uint32_t leader_skills::level_of(const fixed_point_t experience, const std::int32_t per_level, const std::int32_t max_level) {
	std::uint32_t level = 0;
	while (static_cast<std::int32_t>(level) < max_level && experience >= experience_for_level(level + 1, per_level)) {
		++level;
	}
	return level;
}

fixed_point_t leader_skills::get_experience(LeaderInstance const& leader, const skill_t skill, TerrainType const* const terrain) {
	switch (skill) {
	case skill_t::ATTACK: return leader.get_attack_experience();
	case skill_t::DEFENCE: return leader.get_defence_experience();
	case skill_t::SUPPLY: return leader.get_supply_experience();
	case skill_t::PLANNING: return leader.get_planning_experience();
	case skill_t::TERRAIN: return terrain != nullptr ? leader.get_terrain_experience_of(*terrain) : fixed_point_t::_0;
	}
	return fixed_point_t::_0;
}

std::uint32_t leader_skills::get_level(
	OpenVicWarDefines const& defines, LeaderInstance const& leader, const skill_t skill, TerrainType const* const terrain
) {
	return is_marshal_skill(skill)
		? level_of(get_experience(leader, skill, terrain), defines.marshal_xp_per_level, defines.marshal_skill_max_level)
		: level_of(get_experience(leader, skill, terrain), defines.general_xp_per_level, defines.general_skill_max_level);
}

std::uint32_t leader_skills::get_active_level(
	OpenVicWarDefines const& defines, LeaderInstance const* const leader, const skill_t skill, TerrainType const* const terrain
) {
	if (leader == nullptr || leader->is_marshal() != is_marshal_skill(skill)) {
		return 0;
	}
	return get_level(defines, *leader, skill, terrain);
}

void leader_skills::add_experience(
	InstanceManager& instance_manager, LeaderInstance& leader, const skill_t skill, const fixed_point_t amount, TerrainType const* const terrain
) {
	if (amount <= fixed_point_t::_0 || (skill == skill_t::TERRAIN && terrain == nullptr)) {
		return;
	}
	OpenVicWarDefines const& defines = instance_manager.definition_manager.get_define_manager().get_openvic_defines().war;
	const std::uint32_t before = get_level(defines, leader, skill, terrain);
	switch (skill) {
	case skill_t::ATTACK: leader.set_attack_experience(leader.get_attack_experience() + amount); break;
	case skill_t::DEFENCE: leader.set_defence_experience(leader.get_defence_experience() + amount); break;
	case skill_t::SUPPLY: leader.set_supply_experience(leader.get_supply_experience() + amount); break;
	case skill_t::PLANNING: leader.set_planning_experience(leader.get_planning_experience() + amount); break;
	case skill_t::TERRAIN: leader.add_terrain_experience(*terrain, amount); break;
	}
	const std::uint32_t after = get_level(defines, leader, skill, terrain);
	if (after <= before) {
		return;
	}
	// Every new level n gives n x the prestige of a level (a field marshal more)
	const fixed_point_t per_level = is_marshal_skill(skill) ? defines.marshal_level_prestige : defines.general_level_prestige;
	fixed_point_t prestige = fixed_point_t::_0;
	for (std::uint32_t level = before + 1; level <= after; ++level) {
		prestige += per_level * static_cast<std::int32_t>(level);
	}
	// The country of the leader is the one the game changes (the leader only keeps it as const)
	CountryInstance& country = const_cast<CountryInstance&>(leader.country);
	country.add_prestige(prestige);
	instance_manager.get_notification_log().add(
		instance_manager.get_today(), &country, "leader_skill_up",
		fmt::format("{}|{}|{}", std::string_view { leader.get_name() }, get_skill_identifier(skill, terrain), after)
	);
}

fixed_point_t leader_skills::scale_terrain_effect(const fixed_point_t effect, const fixed_point_t step, const std::uint32_t level) {
	if (level == 0 || effect == fixed_point_t::_0) {
		return effect;
	}
	const fixed_point_t change = step * static_cast<std::int32_t>(level);
	return effect > fixed_point_t::_0 ? effect * (fixed_point_t::_1 + change) : effect * std::max(fixed_point_t::_0, fixed_point_t::_1 - change);
}

LeaderInstance* leader_skills::get_group_marshal(InstanceManager& instance_manager, ArmyGroup const& group) {
	return group.marshal_id != 0 ? instance_manager.get_unit_instance_manager().get_leader_instance_by_unique_id(group.marshal_id) : nullptr;
}

LeaderInstance const* leader_skills::get_army_marshal(InstanceManager& instance_manager, ArmyInstance const& army) {
	if (army.get_army_group_id() == 0) {
		return nullptr;
	}
	ArmyGroup const* const group = army.get_country().find_army_group(army.get_army_group_id());
	return group != nullptr ? get_group_marshal(instance_manager, *group) : nullptr;
}

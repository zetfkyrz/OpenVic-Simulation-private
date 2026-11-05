#pragma once

#include <cstdint>
#include <string_view>

#include "openvic-simulation/core/object/FixedPoint.hpp"
#include "openvic-simulation/types/UnitBranchType.hpp"

namespace OpenVic {
	struct ArmyGroup;

	struct InstanceManager;
	struct LeaderInstance;
	struct OpenVicWarDefines;
	struct TerrainType;

	// The skills of the leaders (docs/WAR_ASSUMPTIONS.md, section 7; the numbers are in ov_war): a general has the attack, the defence and a skill for
	// every terrain (GENERAL_SKILL_MAX_LEVEL levels), a field marshal the supply and the planning (MARSHAL_SKILL_MAX_LEVEL levels). A skill has a level
	// by its experience; the role decides which skills work and grow (a general made a marshal keeps the experience of a general for later)
	namespace leader_skills {
		enum struct skill_t : std::uint8_t { ATTACK, DEFENCE, TERRAIN, SUPPLY, PLANNING };

		[[nodiscard]] constexpr bool is_marshal_skill(const skill_t skill) {
			return skill == skill_t::SUPPLY || skill == skill_t::PLANNING;
		}
		// The identifier of a skill for the messages and the interface ("attack", "defence", "supply", "planning"; a terrain: its identifier)
		[[nodiscard]] std::string_view get_skill_identifier(skill_t skill, TerrainType const* terrain);

		// The total experience level n needs: per_level x (1 + 2 + ... + n)
		[[nodiscard]] fixed_point_t experience_for_level(std::uint32_t level, std::int32_t per_level);
		// The level an experience reaches (at most max_level)
		[[nodiscard]] std::uint32_t level_of(fixed_point_t experience, std::int32_t per_level, std::int32_t max_level);

		[[nodiscard]] fixed_point_t get_experience(LeaderInstance const& leader, skill_t skill, TerrainType const* terrain = nullptr);
		[[nodiscard]] std::uint32_t get_level(
			OpenVicWarDefines const& defines, LeaderInstance const& leader, skill_t skill, TerrainType const* terrain = nullptr
		);
		// The level of a skill that works: of a general only while he is not a marshal, of a marshal only while he is one
		[[nodiscard]] std::uint32_t get_active_level(
			OpenVicWarDefines const& defines, LeaderInstance const* leader, skill_t skill, TerrainType const* terrain = nullptr
		);

		// Adds the experience of a skill (of the terrain for TERRAIN); a new level gives the country of the leader the prestige of it (the number of
		// the level x GENERAL_LEVEL_PRESTIGE or MARSHAL_LEVEL_PRESTIGE) and a message "leader_skill_up" (subject: name|skill|level)
		void add_experience(
			InstanceManager& instance_manager, LeaderInstance& leader, skill_t skill, fixed_point_t amount, TerrainType const* terrain = nullptr
		);

		// The effect of a terrain (a share: +0.1 is a bonus of 10 %, -0.2 a penalty of 20 %) for a general with a level of that terrain: the bonus
		// grows by step x level of itself, the penalty shrinks by it (never below 0)
		[[nodiscard]] fixed_point_t scale_terrain_effect(fixed_point_t effect, fixed_point_t step, std::uint32_t level);

		// The field marshal of the group of an army (nullptr: no group, no marshal)
		[[nodiscard]] LeaderInstance const* get_army_marshal(InstanceManager& instance_manager, ArmyInstance const& army);
		[[nodiscard]] LeaderInstance* get_group_marshal(InstanceManager& instance_manager, ArmyGroup const& group);
	}
}

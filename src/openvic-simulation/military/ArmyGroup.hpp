#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "openvic-simulation/core/memory/Vector.hpp"
#include "openvic-simulation/core/object/Colour.hpp"
#include "openvic-simulation/core/object/FixedPoint.hpp"
#include "openvic-simulation/dataloader/NodeTools.hpp"
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

	// The command of a field marshal (B1 of the command system): the armies (with their generals) attached to the group are at most
	// get_marshal_max_attached (ov_war.MARSHAL_MAX_ATTACHED plus the modifier attached_generals_add of the country, never more than
	// ov_war.MARSHAL_MAX_ATTACHED_LIMIT). Every group has a colour of the palette (common/ov_command/palette.txt): the marshal, his generals
	// (the armies of the group) and, on the map, their marks have it; the armies without a group are grey.
	struct ArmyGroup {
		std::uint32_t id = 0;
		unique_id_t marshal_id = 0; // the unique id of the general, 0 if there is none
		bool execute_orders = true; // the armies of the group follow the orders of the marshal (the switch of the panel of the commanders)
		std::uint32_t colour_index = 0; // the index in the palette of the command
	};

	// The palette of the colours of the field marshals (ov_data/common/ov_command/palette.txt: colour = { r g b } lines). A new group takes the first
	// colour of the palette that no other group of the country has, so the colours are the same in every run. A palette that is missing or empty
	// is replaced by the built-in one of 12 colours.
	struct CommandPalette {
	private:
		memory::vector<colour_t> colours;

	public:
		// Returns the number of the colours
		std::size_t load_file(std::string_view source, ast::NodeCPtr root);
		void use_builtin_colours();
		[[nodiscard]] std::span<const colour_t> get_colours() const {
			return colours;
		}
		[[nodiscard]] std::size_t size() const {
			return colours.size();
		}
	};

	// How many armies (generals) a country can attach to a field marshal: the base of the defines plus the modifier attached_generals_add
	[[nodiscard]] std::uint32_t get_marshal_max_attached(InstanceManager const& instance_manager, CountryInstance const& country);

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
	bool create_army_group(CountryInstance& country, std::size_t palette_size);
	// The colour of the group (an index of the palette): the player recolours his field marshal
	bool set_army_group_colour(CountryInstance& country, std::uint32_t group_id, std::uint32_t colour_index, std::size_t palette_size);
	bool delete_army_group(InstanceManager& instance_manager, CountryInstance& country, std::uint32_t group_id);
	// group_id 0 takes the army out of its group
	// A group holds at most max_attached armies
	bool set_army_group(CountryInstance const& country, ArmyInstance& army, std::uint32_t group_id, std::uint32_t max_attached);
	bool set_army_group_orders(CountryInstance& country, std::uint32_t group_id, bool execute);
	// leader_id 0 takes the marshal off; the general must not be a marshal already and must be usable; if he leads an army he leaves it
	bool set_army_group_marshal(
		InstanceManager& instance_manager, CountryInstance& country, std::uint32_t group_id, unique_id_t leader_id
	);

	// Every day: the countries without the technology have no groups and no marshals; the groups lose what does not exist any more
	void update_army_groups(InstanceManager& instance_manager, CountryInstance& country);
}

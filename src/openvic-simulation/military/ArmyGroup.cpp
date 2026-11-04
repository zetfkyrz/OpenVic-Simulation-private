#include "ArmyGroup.hpp"

#include <algorithm>
#include <limits>

#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/DefinitionManager.hpp"
#include "openvic-simulation/defines/OpenVicDefines.hpp"
#include "openvic-simulation/InstanceManager.hpp"
#include "openvic-simulation/military/Leader.hpp"
#include "openvic-simulation/map/MapInstance.hpp"
#include "openvic-simulation/map/ProvinceInstance.hpp"
#include "openvic-simulation/military/LeaderTrait.hpp"
#include "openvic-simulation/military/MilitaryManager.hpp"
#include "openvic-simulation/military/War.hpp"
#include "openvic-simulation/military/UnitInstanceGroup.hpp"
#include "openvic-simulation/modifier/ModifierEffectCache.hpp"
#include "openvic-simulation/modifier/ModifierManager.hpp"
#include "openvic-simulation/utility/Logger.hpp"

using namespace OpenVic;
using namespace OpenVic::NodeTools;

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

std::size_t CommandPalette::load_file(std::string_view source, ast::NodeCPtr root) {
	colours.clear();
	const bool parsed = expect_dictionary_keys_and_default(
		[source](std::string_view key, ast::NodeCPtr) -> bool {
			spdlog::warn_s("Unknown key \"{}\" in the palette of the command {}", key, source);
			return true;
		},
		"colour", ZERO_OR_MORE, expect_colour([this](colour_t colour) -> bool {
			colours.push_back(colour);
			return true;
		})
	)(root);
	if (!parsed) {
		spdlog::warn_s("The palette of the command {} is broken, what could be read is loaded", source);
	}
	return colours.size();
}

void CommandPalette::use_builtin_colours() {
	colours.clear();
	// Distinct hues that read on the map and on the dark panel
	static constexpr std::uint8_t builtin[][3] = {
		{ 214, 62, 62 }, { 62, 120, 214 }, { 70, 170, 84 }, { 224, 168, 40 }, { 150, 84, 190 }, { 54, 176, 176 },
		{ 224, 112, 40 }, { 200, 80, 150 }, { 130, 170, 40 }, { 90, 90, 200 }, { 170, 120, 70 }, { 120, 200, 140 }
	};
	for (auto const& rgb : builtin) {
		colours.push_back(colour_t::from_rgb((static_cast<std::uint32_t>(rgb[0]) << 16) | (static_cast<std::uint32_t>(rgb[1]) << 8) | rgb[2]));
	}
}

std::uint32_t OpenVic::get_marshal_max_attached(InstanceManager const& instance_manager, CountryInstance const& country) {
	OpenVicWarDefines const& war = instance_manager.definition_manager.get_define_manager().get_openvic_defines().war;
	ModifierEffectCache const& cache = instance_manager.definition_manager.get_modifier_manager().get_modifier_effect_cache();
	std::int32_t value = war.marshal_max_attached;
	if (cache.get_attached_generals_add() != nullptr) {
		value += country.get_modifier_effect_value(*cache.get_attached_generals_add()).floor<std::int32_t>();
	}
	return static_cast<std::uint32_t>(std::clamp(value, 1, std::max(1, war.marshal_max_attached_limit)));
}

namespace {
	// The first colour of the palette that is used the least by the given colours (the first of them): the same in every run
	std::uint32_t pick_free_colour(std::span<const std::uint32_t> used, const std::size_t palette_size) {
		std::uint32_t colour_index = 0;
		std::uint32_t best_uses = std::numeric_limits<std::uint32_t>::max();
		for (std::uint32_t candidate = 0; candidate < palette_size; ++candidate) {
			std::uint32_t uses = 0;
			for (const std::uint32_t value : used) {
				uses += value == candidate;
			}
			if (uses < best_uses) {
				best_uses = uses;
				colour_index = candidate;
			}
		}
		return colour_index;
	}

	// The main theatre is made when a country with the organisation of the army needs one
	void ensure_main_theatre(CountryInstance& country, const std::size_t palette_size) {
		if (country.get_theatres().empty()) {
			Theatre theatre;
			theatre.id = country.take_theatre_id();
			theatre.colour_index = pick_free_colour({}, palette_size);
			country.get_mutable_theatres().push_back(theatre);
		}
	}
}

std::uint32_t OpenVic::get_main_theatre_id(CountryInstance const& country) {
	return country.get_theatres().empty() ? 0 : country.get_theatres().front().id;
}

bool OpenVic::create_theatre(CountryInstance& country, const std::size_t palette_size, const std::size_t max_theatres) {
	if (!country.has_army_command()) {
		return false;
	}
	ensure_main_theatre(country, palette_size);
	if (country.get_theatres().size() >= max_theatres) {
		return false;
	}
	std::vector<std::uint32_t> used;
	for (Theatre const& theatre : country.get_theatres()) {
		used.push_back(theatre.colour_index);
	}
	Theatre theatre;
	theatre.id = country.take_theatre_id();
	theatre.colour_index = pick_free_colour(used, palette_size);
	country.get_mutable_theatres().push_back(theatre);
	return true;
}

bool OpenVic::delete_theatre(CountryInstance& country, const std::uint32_t theatre_id) {
	memory::vector<Theatre>& theatres = country.get_mutable_theatres();
	if (theatres.empty() || theatres.front().id == theatre_id) {
		return false;
	}
	const auto it = std::find_if(theatres.begin(), theatres.end(), [theatre_id](Theatre const& theatre) -> bool {
		return theatre.id == theatre_id;
	});
	if (it == theatres.end()) {
		return false;
	}
	const std::uint32_t main_id = theatres.front().id;
	theatres.erase(it);
	for (ArmyGroup& group : country.get_mutable_army_groups()) {
		if (group.theatre_id == theatre_id) {
			group.theatre_id = main_id;
		}
	}
	return true;
}

bool OpenVic::rename_theatre(CountryInstance& country, const std::uint32_t theatre_id, theatre_name_t const& name) {
	for (Theatre& theatre : country.get_mutable_theatres()) {
		if (theatre.id == theatre_id) {
			if (theatre.name == name) {
				return false;
			}
			theatre.name = name;
			theatre.name.back() = 0; // always ends
			return true;
		}
	}
	return false;
}

bool OpenVic::set_theatre_colour(CountryInstance& country, const std::uint32_t theatre_id, const std::uint32_t colour_index, const std::size_t palette_size) {
	if (colour_index >= palette_size) {
		return false;
	}
	for (Theatre& theatre : country.get_mutable_theatres()) {
		if (theatre.id == theatre_id) {
			if (theatre.colour_index == colour_index) {
				return false;
			}
			theatre.colour_index = colour_index;
			return true;
		}
	}
	return false;
}

bool OpenVic::set_theatre_focus(InstanceManager& instance_manager, CountryInstance& country, const std::uint32_t theatre_id, const std::uint32_t province_number) {
	if (province_number != 0) {
		if (province_number > std::numeric_limits<std::uint16_t>::max()) {
			return false;
		}
		ProvinceInstance const* const province = instance_manager.get_map_instance().get_province_instance_from_number(static_cast<std::uint16_t>(province_number));
		if (province == nullptr || province->province_definition.is_water()) {
			return false;
		}
	}
	for (Theatre& theatre : country.get_mutable_theatres()) {
		if (theatre.id == theatre_id) {
			if (theatre.focus_province == province_number) {
				return false;
			}
			theatre.focus_province = province_number;
			return true;
		}
	}
	return false;
}

bool OpenVic::set_army_group_theatre(CountryInstance& country, const std::uint32_t group_id, const std::uint32_t theatre_id) {
	if (country.find_theatre(theatre_id) == nullptr) {
		return false;
	}
	for (ArmyGroup& group : country.get_mutable_army_groups()) {
		if (group.id == group_id) {
			if (group.theatre_id == theatre_id) {
				return false;
			}
			group.theatre_id = theatre_id;
			return true;
		}
	}
	return false;
}

bool OpenVic::create_army_group(CountryInstance& country, const std::size_t palette_size) {
	if (!country.has_army_command() || country.get_army_groups().size() >= MAX_ARMY_GROUPS) {
		return false;
	}
	// The first colour of the palette that no other group of the country has (all taken: the one that is used the least, the first of them)
	std::uint32_t colour_index = 0;
	if (palette_size > 0) {
		std::uint32_t best_uses = std::numeric_limits<std::uint32_t>::max();
		for (std::uint32_t candidate = 0; candidate < palette_size; ++candidate) {
			std::uint32_t uses = 0;
			for (ArmyGroup const& group : country.get_army_groups()) {
				uses += group.colour_index == candidate;
			}
			if (uses < best_uses) {
				best_uses = uses;
				colour_index = candidate;
			}
		}
	}
	ensure_main_theatre(country, palette_size);
	ArmyGroup group { country.take_army_group_id(), 0, true, colour_index };
	group.theatre_id = get_main_theatre_id(country);
	country.get_mutable_army_groups().push_back(group);
	return true;
}

namespace {
	ArmyGroup* find_mutable_group(CountryInstance& country, const std::uint32_t group_id) {
		for (ArmyGroup& group : country.get_mutable_army_groups()) {
			if (group.id == group_id) {
				return &group;
			}
		}
		return nullptr;
	}

	// The plan stops: the armies of its fronts stop where they are
	void stop_plan(InstanceManager& instance_manager, CountryInstance const& country, ArmyGroup& group) {
		if (group.plan_active) {
			WarManager& war_manager = instance_manager.get_war_manager();
			for (Front const& front : war_manager.get_fronts()) {
				if (front.country == &country && front.group_id == group.id) {
					war_manager.stop_front_armies(instance_manager, front.id);
				}
			}
			group.plan_active = false;
		}
	}

	// The fronts and the lines of the plans of a group which is gone go with it
	void delete_orphan_lines(InstanceManager& instance_manager, CountryInstance const& country) {
		WarManager& war_manager = instance_manager.get_war_manager();
		std::vector<std::uint32_t> orphans;
		for (Front const& front : war_manager.get_fronts()) {
			if (front.country == &country && front.group_id != 0 && country.find_army_group(front.group_id) == nullptr
				&& std::find(orphans.begin(), orphans.end(), front.group_id) == orphans.end()) {
				orphans.push_back(front.group_id);
			}
		}
		for (PlanLine const& line : war_manager.get_plan_lines()) {
			if (line.country == &country && country.find_army_group(line.group_id) == nullptr
				&& std::find(orphans.begin(), orphans.end(), line.group_id) == orphans.end()) {
				orphans.push_back(line.group_id);
			}
		}
		for (const std::uint32_t group_id : orphans) {
			war_manager.delete_group_lines(instance_manager, country, group_id);
		}
	}
}

bool OpenVic::set_army_group_front(
	InstanceManager& instance_manager, CountryInstance& country, const std::uint32_t group_id, const std::uint32_t front_id, const bool held
) {
	ArmyGroup* const group = country.has_army_command() ? find_mutable_group(country, group_id) : nullptr;
	WarManager& war_manager = instance_manager.get_war_manager();
	Front const* const front = war_manager.get_front_by_id(front_id);
	if (group == nullptr || front == nullptr || front->country != &country) {
		return false;
	}
	if (held) {
		return front->group_id == 0 && war_manager.set_front_group(country, front_id, group_id);
	}
	if (front->group_id != group_id) {
		return false;
	}
	if (group->plan_active) {
		war_manager.stop_front_armies(instance_manager, front_id);
	}
	return war_manager.set_front_group(country, front_id, 0);
}

bool OpenVic::set_army_group_modes(CountryInstance& country, const std::uint32_t group_id, const std::uint32_t aggression, const std::uint32_t integrity) {
	ArmyGroup* const group = country.has_army_command() ? find_mutable_group(country, group_id) : nullptr;
	if (group == nullptr || aggression < 1 || aggression > 3 || integrity > 2 || (group->aggression == aggression && group->integrity == integrity)) {
		return false;
	}
	group->aggression = aggression;
	group->integrity = integrity;
	return true;
}

bool OpenVic::set_army_group_plan_active(
	InstanceManager& instance_manager, CountryInstance& country, const std::uint32_t group_id, const bool active
) {
	ArmyGroup* const group = country.has_army_command() ? find_mutable_group(country, group_id) : nullptr;
	if (group == nullptr || group->plan_active == active) {
		return false;
	}
	if (!active) {
		stop_plan(instance_manager, country, *group);
		return true;
	}
	// The armies of the group are on the order already (WarManager::update_fronts puts them there every day); the execution lets them attack
	if (instance_manager.get_war_manager().find_front_of_group(country, group_id) == nullptr || group->marshal_id == 0) {
		return false;
	}
	group->plan_active = true;
	return true;
}

bool OpenVic::set_army_group_colour(CountryInstance& country, const std::uint32_t group_id, const std::uint32_t colour_index, const std::size_t palette_size) {
	if (!country.has_army_command() || colour_index >= palette_size) {
		return false;
	}
	for (ArmyGroup& group : country.get_mutable_army_groups()) {
		if (group.id == group_id) {
			if (group.colour_index == colour_index) {
				return false;
			}
			group.colour_index = colour_index;
			return true;
		}
	}
	return false;
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
	stop_plan(instance_manager, country, *it);
	groups.erase(it);
	delete_orphan_lines(instance_manager, country);
	for (ArmyInstance& army : country.get_armies()) {
		if (army.get_army_group_id() == group_id) {
			army.set_army_group_id(0);
		}
	}
	return true;
}

bool OpenVic::set_army_group(CountryInstance const& country, ArmyInstance& army, const std::uint32_t group_id, const std::uint32_t max_attached) {
	if (!country.has_army_command() || army.get_army_group_id() == group_id) {
		return false;
	}
	if (group_id != 0 && country.find_army_group(group_id) == nullptr) {
		return false;
	}
	if (group_id != 0) {
		std::uint32_t attached = 0;
		for (ArmyInstance const& other : country.get_armies()) {
			attached += other.get_army_group_id() == group_id && other.get_parent_army_id() == 0;
		}
		if (attached >= max_attached && army.get_parent_army_id() == 0) {
			return false;
		}
	}
	// A division of the army of a general moved on its own becomes an army of its own; the divisions of an army go with it
	army.set_parent_army_id(0);
	army.set_army_group_id(group_id);
	for (ArmyInstance& other : const_cast<CountryInstance&>(country).get_armies()) {
		if (other.get_parent_army_id() == army.unique_id) {
			other.set_army_group_id(group_id);
		}
	}
	return true;
}

bool OpenVic::set_army_group_orders(CountryInstance& country, const std::uint32_t group_id, const bool execute) {
	if (!country.has_army_command()) {
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
	if (!country.has_army_command()) {
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
	if (!country.has_army_command()) {
		// No technology, no groups (the generals stay, they are only free)
		for (ArmyGroup& group : groups) {
			free_marshal(instance_manager, group);
		}
		for (ArmyGroup& group : groups) {
			stop_plan(instance_manager, country, group);
		}
		groups.clear();
		country.get_mutable_theatres().clear();
		for (ArmyInstance& army : country.get_armies()) {
			army.set_army_group_id(0);
		}
		delete_orphan_lines(instance_manager, country);
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
	// A colour out of the palette (an old save, another palette) is replaced by the first colour of the palette; the armies over the limit of
	// the field marshal (the limit fell) leave the group, the last armies of the country first
	// The execution of a plan needs a front of the group and a field marshal
	for (ArmyGroup& group : groups) {
		if (!group.plan_active) {
			continue;
		}
		if (instance_manager.get_war_manager().find_front_of_group(country, group.id) == nullptr) {
			group.plan_active = false; // the fronts are gone, there are no armies to stop
		} else if (group.marshal_id == 0) {
			stop_plan(instance_manager, country, group);
		}
	}
	delete_orphan_lines(instance_manager, country);
	const std::size_t palette_size = instance_manager.definition_manager.get_military_manager().get_command_palette().size();
	const std::uint32_t max_attached = get_marshal_max_attached(instance_manager, country);
	ensure_main_theatre(country, palette_size);
	for (Theatre& theatre : country.get_mutable_theatres()) {
		if (palette_size > 0 && theatre.colour_index >= palette_size) {
			theatre.colour_index = 0;
		}
	}
	const std::uint32_t main_theatre_id = get_main_theatre_id(country);
	for (ArmyGroup& group : groups) {
		if (country.find_theatre(group.theatre_id) == nullptr) {
			group.theatre_id = main_theatre_id; // a group always belongs to one theatre
		}
		if (palette_size > 0 && group.colour_index >= palette_size) {
			group.colour_index = 0;
		}
		std::uint32_t attached = 0;
		for (ArmyInstance& army : country.get_armies()) {
			if (army.get_army_group_id() == group.id && army.get_parent_army_id() == 0 && ++attached > max_attached) {
				army.set_army_group_id(0);
			}
		}
	}
	// A division follows the army of its general (into a group, out of it); without that army it is an army of its own
	UnitInstanceManager& unit_manager = instance_manager.get_unit_instance_manager();
	for (ArmyInstance& army : country.get_armies()) {
		if (army.get_parent_army_id() == 0) {
			continue;
		}
		UnitInstanceGroup const* const parent = unit_manager.get_unit_instance_group_by_unique_id(army.get_parent_army_id());
		if (parent == nullptr || parent->branch != unit_branch_t::LAND || &parent->get_country() != &country) {
			army.set_parent_army_id(0);
		} else {
			army.set_army_group_id(static_cast<ArmyInstance const*>(parent)->get_army_group_id());
			if (army.get_army_group_id() == 0) {
				army.set_parent_army_id(0);
			}
		}
	}
}

#include "CountryRelation.hpp"

#include <cassert>

#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/InstanceManager.hpp"

using namespace OpenVic;
using CRM = CountryRelationManager;
using relation_value_type = OpenVic::CountryRelationManager::relation_value_type;
using influence_value_type = OpenVic::CountryRelationManager::influence_value_type;
using OpinionType = OpenVic::CountryRelationManager::OpinionType;
using influence_priority_value_type = OpenVic::CountryRelationManager::influence_priority_value_type;

relation_value_type CRM::get_country_relation( //
	CountryInstance const* country, CountryInstance const* recipient
) const {
	decltype(relations)::const_iterator it = relations.find({ country, recipient });
	if (it == relations.end()) {
		return {};
	}
	return it->second;
}

relation_value_type& CRM::assign_or_get_country_relation( //
	CountryInstance* country, CountryInstance* recipient, relation_value_type default_value
) {
	decltype(relations)::iterator it = relations.find({ country, recipient });
	if (it == relations.end()) {
		it = relations.insert({ { country, recipient }, default_value }).first;
	}
	return it.value();
}

bool CRM::set_country_relation( //
	CountryInstance* country, CountryInstance* recipient, relation_value_type value
) {
	decltype(relations)::iterator it = relations.find({ country, recipient });
	if (it == relations.end()) {
		it = relations.insert({ { country, recipient }, value }).first;
	} else {
		it.value() = value;
	}
	return true;
}

decltype(CRM::relations)::values_container_type const& CRM::get_country_relation_values() const {
	return relations.values_container();
}

bool CRM::get_country_alliance( //
	CountryInstance const* country, CountryInstance const* recipient
) const {
	decltype(alliances)::const_iterator it = alliances.find({ country, recipient });
	if (it == alliances.end()) {
		return {};
	}
	return it->second;
}

bool& CRM::assign_or_get_country_alliance( //
	CountryInstance* country, CountryInstance* recipient, bool default_value
) {
	decltype(alliances)::iterator it = alliances.find({ country, recipient });
	if (it == alliances.end()) {
		it = alliances.insert({ { country, recipient }, default_value }).first;
	}
	return it.value();
}

bool CRM::set_country_alliance( //
	CountryInstance* country, CountryInstance* recipient, bool value
) {
	decltype(alliances)::iterator it = alliances.find({ country, recipient });
	if (it == alliances.end()) {
		it = alliances.insert({ { country, recipient }, value }).first;
	} else {
		it.value() = value;
	}
	return true;
}

decltype(CRM::alliances)::values_container_type const& CRM::get_country_alliance_values() const {
	return alliances.values_container();
}

bool CRM::get_at_war_with( //
	CountryInstance const* country, CountryInstance const* recipient
) const {
	decltype(at_war)::const_iterator it = at_war.find({ country, recipient });
	if (it == at_war.end()) {
		return {};
	}
	return it->second;
}

bool& CRM::assign_or_get_at_war_with( //
	CountryInstance* country, CountryInstance* recipient, bool default_value
) {
	decltype(at_war)::iterator it = at_war.find({ country, recipient });
	if (it == at_war.end()) {
		it = at_war.insert({ { country, recipient }, default_value }).first;
	}
	return it.value();
}

bool CRM::set_at_war_with( //
	CountryInstance* country, CountryInstance* recipient, bool value
) {
	decltype(at_war)::iterator it = at_war.find({ country, recipient });
	if (it == at_war.end()) {
		it = at_war.insert({ { country, recipient }, value }).first;
	} else {
		it.value() = value;
	}
	return true;
}

decltype(CRM::at_war)::values_container_type const& CRM::get_at_war_with_values() const {
	return at_war.values_container();
}

bool CRM::get_has_military_access_to( //
	CountryInstance const* country, CountryInstance const* recipient
) const {
	decltype(military_access)::const_iterator it = military_access.find({ country, recipient });
	if (it == military_access.end()) {
		return {};
	}
	return it->second;
}

bool& CRM::assign_or_get_has_military_access_to( //
	CountryInstance* country, CountryInstance const* recipient, bool default_value
) {
	decltype(military_access)::iterator it = military_access.find({ country, recipient });
	if (it == military_access.end()) {
		it = military_access.insert({ { country, recipient }, default_value }).first;
	}
	return it.value();
}

bool CRM::set_has_military_access_to( //
	CountryInstance* country, CountryInstance const* recipient, bool value
) {
	decltype(military_access)::iterator it = military_access.find({ country, recipient });
	if (it == military_access.end()) {
		it = military_access.insert({ { country, recipient }, value }).first;
	} else {
		it.value() = value;
	}
	return true;
}

decltype(CRM::military_access)::values_container_type const& CRM::get_has_military_access_to_values() const {
	return military_access.values_container();
}

bool CRM::get_war_subsidies_to( //
	CountryInstance const* country, CountryInstance const* recipient
) const {
	decltype(war_subsidies)::const_iterator it = war_subsidies.find({ country, recipient });
	if (it == war_subsidies.end()) {
		return {};
	}
	return it->second;
}

bool& CRM::assign_or_get_war_subsidies_to( //
	CountryInstance* country, CountryInstance const* recipient, bool default_value
) {
	decltype(war_subsidies)::iterator it = war_subsidies.find({ country, recipient });
	if (it == war_subsidies.end()) {
		it = war_subsidies.insert({ { country, recipient }, default_value }).first;
	}
	return it.value();
}

bool CRM::set_war_subsidies_to( //
	CountryInstance* country, CountryInstance const* recipient, bool value
) {
	decltype(war_subsidies)::iterator it = war_subsidies.find({ country, recipient });
	if (it == war_subsidies.end()) {
		it = war_subsidies.insert({ { country, recipient }, value }).first;
	} else {
		it.value() = value;
	}
	return true;
}

decltype(CRM::war_subsidies)::values_container_type const& CRM::get_war_subsidies_to_values() const {
	return war_subsidies.values_container();
}

bool CRM::get_commands_units( //
	CountryInstance const* country, CountryInstance const* recipient
) const {
	decltype(command_units)::const_iterator it = command_units.find({ country, recipient });
	if (it == command_units.end()) {
		return {};
	}
	return it->second;
}

bool& CRM::assign_or_get_commands_units( //
	CountryInstance* country, CountryInstance const* recipient, bool default_value
) {
	decltype(command_units)::iterator it = command_units.find({ country, recipient });
	if (it == command_units.end()) {
		it = command_units.insert({ { country, recipient }, default_value }).first;
	}
	return it.value();
}

bool CRM::set_commands_units( //
	CountryInstance* country, CountryInstance const* recipient, bool value
) {
	decltype(command_units)::iterator it = command_units.find({ country, recipient });
	if (it == command_units.end()) {
		it = command_units.insert({ { country, recipient }, value }).first;
	} else {
		it.value() = value;
	}
	return true;
}

decltype(CRM::command_units)::values_container_type const& CRM::get_commands_units_values() const {
	return command_units.values_container();
}

bool CRM::get_has_vision( //
	CountryInstance const* country, CountryInstance const* recipient
) const {
	decltype(vision)::const_iterator it = vision.find({ country, recipient });
	if (it == vision.end()) {
		return {};
	}
	return it->second;
}

bool& CRM::assign_or_get_has_vision( //
	CountryInstance* country, CountryInstance const* recipient, bool default_value
) {
	decltype(vision)::iterator it = vision.find({ country, recipient });
	if (it == vision.end()) {
		it = vision.insert({ { country, recipient }, default_value }).first;
	}
	return it.value();
}

bool CRM::set_has_vision( //
	CountryInstance* country, CountryInstance const* recipient, bool value
) {
	decltype(vision)::iterator it = vision.find({ country, recipient });
	if (it == vision.end()) {
		it = vision.insert({ { country, recipient }, value }).first;
	} else {
		it.value() = value;
	}
	return true;
}

decltype(CRM::vision)::values_container_type const& CRM::get_has_vision_values() const {
	return vision.values_container();
}

OpinionType CRM::get_country_opinion( //
	CountryInstance const* country, CountryInstance const* recipient
) const {
	decltype(opinions)::const_iterator it = opinions.find({ country, recipient });
	if (it == opinions.end()) {
		return {};
	}
	return it->second;
}

OpinionType& CRM::assign_or_get_country_opinion( //
	CountryInstance* country, CountryInstance const* recipient, OpinionType default_value
) {
	decltype(opinions)::iterator it = opinions.find({ country, recipient });
	if (it == opinions.end()) {
		it = opinions.insert({ { country, recipient }, default_value }).first;
	}
	return it.value();
}

bool CRM::set_country_opinion( //
	CountryInstance* country, CountryInstance const* recipient, OpinionType value
) {
	decltype(opinions)::iterator it = opinions.find({ country, recipient });
	if (it == opinions.end()) {
		it = opinions.insert({ { country, recipient }, value }).first;
	} else {
		it.value() = value;
	}
	return true;
}

decltype(CRM::opinions)::values_container_type const& CRM::get_country_opinion_values() const {
	return opinions.values_container();
}

influence_value_type CRM::get_influence_with( //
	CountryInstance const* country, CountryInstance const* recipient
) const {
	decltype(influence)::const_iterator it = influence.find({ country, recipient });
	if (it == influence.end()) {
		return {};
	}
	return it->second;
}

influence_value_type& CRM::assign_or_get_influence_with(
	CountryInstance* country, CountryInstance const* recipient, influence_value_type default_value
) {
	decltype(influence)::iterator it = influence.find({ country, recipient });
	if (it == influence.end()) {
		it = influence.insert({ { country, recipient }, default_value }).first;
	}
	return it.value();
}

bool CRM::set_influence_with(CountryInstance* country, CountryInstance const* recipient, influence_value_type value) {
	decltype(influence)::iterator it = influence.find({ country, recipient });
	if (it == influence.end()) {
		it = influence.insert({ { country, recipient }, value }).first;
	} else {
		it.value() = value;
	}
	return true;
}

decltype(CRM::influence)::values_container_type const& CRM::get_influence_with_values() const {
	return influence.values_container();
}

influence_priority_value_type CRM::get_influence_priority_with( //
	CountryInstance const* country, CountryInstance const* recipient
) const {
	decltype(influence_priority)::const_iterator it = influence_priority.find({ country, recipient });
	if (it == influence_priority.end()) {
		return {};
	}
	return it->second;
}

influence_priority_value_type& CRM::assign_or_get_influence_priority_with(
	CountryInstance* country, CountryInstance const* recipient, influence_priority_value_type default_value
) {
	decltype(influence_priority)::iterator it = influence_priority.find({ country, recipient });
	if (it == influence_priority.end()) {
		it = influence_priority.insert({ { country, recipient }, default_value }).first;
	}
	return it.value();
}

bool CRM::set_influence_priority_with(CountryInstance* country, CountryInstance const* recipient, influence_priority_value_type value) {
	decltype(influence_priority)::iterator it = influence_priority.find({ country, recipient });
	if (it == influence_priority.end()) {
		it = influence_priority.insert({ { country, recipient }, value }).first;
	} else {
		it.value() = value;
	}
	return true;
}

decltype(CRM::influence_priority)::values_container_type const& CRM::get_influence_priority_with_values() const {
	return influence_priority.values_container();
}

std::optional<Date> CRM::get_discredited_date( //
	CountryInstance const* country, CountryInstance const* recipient
) const {
	decltype(discredits)::const_iterator it = discredits.find({ country, recipient });
	if (it == discredits.end()) {
		return {};
	}
	return it->second;
}

Date& CRM::assign_or_get_discredited_date( //
	CountryInstance* country, CountryInstance const* recipient, Date default_value
) {
	decltype(discredits)::iterator it = discredits.find({ country, recipient });
	if (it == discredits.end()) {
		it = discredits.insert({ { country, recipient }, default_value }).first;
	}
	return it.value();
}

bool CRM::set_discredited_date( //
	CountryInstance* country, CountryInstance const* recipient, Date value
) {
	decltype(discredits)::iterator it = discredits.find({ country, recipient });
	if (it == discredits.end()) {
		it = discredits.insert({ { country, recipient }, value }).first;
	} else {
		it.value() = value;
	}
	return true;
}

decltype(CRM::discredits.values_container()) CRM::get_discredited_date_values() const {
	return discredits.values_container();
}

std::optional<Date> CRM::get_embassy_banned_date( //
	CountryInstance const* country, CountryInstance const* recipient
) const {
	decltype(embassy_bans)::const_iterator it = embassy_bans.find({ country, recipient });
	if (it == embassy_bans.end()) {
		return {};
	}
	return it->second;
}

Date& CRM::assign_or_get_embassy_banned_date( //
	CountryInstance* country, CountryInstance const* recipient, Date default_value
) {
	decltype(embassy_bans)::iterator it = embassy_bans.find({ country, recipient });
	if (it == embassy_bans.end()) {
		it = embassy_bans.insert({ { country, recipient }, default_value }).first;
	}
	return it.value();
}

bool CRM::set_embassy_banned_date( //
	CountryInstance* country, CountryInstance const* recipient, Date value
) {
	decltype(embassy_bans)::iterator it = embassy_bans.find({ country, recipient });
	if (it == embassy_bans.end()) {
		it = embassy_bans.insert({ { country, recipient }, value }).first;
	} else {
		it.value() = value;
	}
	return true;
}

decltype(CRM::embassy_bans.values_container()) CRM::get_embassy_banned_date_values() const {
	return embassy_bans.values_container();
}

// ---- The relations (version 90, docs/DIPLOMACY.md)

fixed_point_t CRM::get_war_scar(CountryInstance const* country, CountryInstance const* recipient) const {
	decltype(war_scars)::const_iterator it = war_scars.find({ country, recipient });
	return it == war_scars.end() ? fixed_point_t::_0 : it->second;
}

void CRM::add_war_scar(CountryInstance* country, CountryInstance* recipient) {
	if (diplomacy_defines == nullptr || country == recipient || diplomacy_defines->war_relation_penalty <= 0) {
		return;
	}
	const fixed_point_t scar = fixed_point_t(-diplomacy_defines->war_relation_penalty);
	decltype(war_scars)::iterator it = war_scars.find({ country, recipient });
	if (it == war_scars.end()) {
		war_scars.insert({ { country, recipient }, scar });
	} else if (it->second > scar) {
		it.value() = scar;
	}
	// a war ends the processes of the buttons between the two
	relation_processes.erase({ country, recipient });
	relation_processes.erase({ recipient, country });
}

relation_value_type CRM::get_effective_relation(CountryInstance const* country, CountryInstance const* recipient) const {
	std::int32_t value = static_cast<std::int32_t>(get_country_relation(country, recipient))
		+ get_war_scar(country, recipient).floor<std::int32_t>();
	// The leader of a sphere and its members (version 92, docs/PLAN_SPHERES.md): +SPHERE_LEADER_RELATION, never under SPHERE_RELATION_FLOOR
	if (diplomacy_defines != nullptr && country != nullptr && recipient != nullptr
		&& (country->get_sphere_owner_untracked() == recipient || recipient->get_sphere_owner_untracked() == country)) {
		value = std::max(value + diplomacy_defines->sphere_leader_relation, diplomacy_defines->sphere_relation_floor);
	} else if (diplomacy_defines != nullptr && country != nullptr && recipient != nullptr
		&& (country->shares_sphere_of_kind(*recipient, sphere_kind_t::IMPERIAL) || country->shares_sphere_of_kind(*recipient, sphere_kind_t::IDEOLOGICAL))) {
		// two members of an imperial sphere or an ideological union (version 93): +SPHERE_MEMBER_RELATION
		value += diplomacy_defines->sphere_member_relation;
	}
	return static_cast<relation_value_type>(std::clamp(value, -200, 200));
}

std::int8_t CRM::get_relation_process(CountryInstance const* sender, CountryInstance const* receiver) const {
	decltype(relation_processes)::const_iterator it = relation_processes.find({ sender, receiver });
	return it == relation_processes.end() ? std::int8_t { 0 } : it->second;
}

void CRM::set_relation_process(CountryInstance* sender, CountryInstance const* receiver, const std::int8_t direction) {
	if (direction == 0) {
		relation_processes.erase({ sender, receiver });
		return;
	}
	decltype(relation_processes)::iterator it = relation_processes.find({ sender, receiver });
	if (it == relation_processes.end()) {
		relation_processes.insert({ { sender, receiver }, direction });
	} else {
		it.value() = direction;
	}
}

std::int32_t CRM::get_relation_limit(CountryInstance const* country, CountryInstance const* recipient) const {
	if (diplomacy_defines == nullptr) {
		return 200;
	}
	// an alliance or a sphere (the leader and a member, version 92) allow the relation above RELATION_MAX
	const bool sphere = country != nullptr && recipient != nullptr
		&& (country->get_sphere_owner_untracked() == recipient || recipient->get_sphere_owner_untracked() == country);
	return get_country_alliance(country, recipient) || sphere ? diplomacy_defines->relation_allied_max : diplomacy_defines->relation_max;
}

std::size_t CRM::get_alliance_count(CountryInstance const* country) const {
	std::size_t count = 0;
	for (auto const& [pair, allied] : alliances) {
		if (allied && (pair.first == country || pair.second == country)) {
			++count;
		}
	}
	return count;
}

memory::vector<std::pair<CountryInstance const*, std::int8_t>> CRM::get_relation_processes_of(CountryInstance const* sender) const {
	memory::vector<std::pair<CountryInstance const*, std::int8_t>> result;
	for (auto const& [pair, direction] : relation_processes) {
		if (pair.first == sender) {
			result.emplace_back(pair.second, direction);
		}
	}
	return result;
}

fixed_point_t CRM::get_diplomatic_points_gain(CountryInstance const& country) {
	if (diplomacy_defines == nullptr) {
		return fixed_point_t::_0;
	}
	OpenVicDiplomacyDefines const& defines = *diplomacy_defines;
	const std::size_t rank = country.get_total_rank();
	if (!country.is_civilised()) {
		return defines.diplomatic_points_uncivilised;
	}
	if (rank > 0 && rank <= static_cast<std::size_t>(std::max(0, defines.great_power_rank))) {
		return defines.diplomatic_points_great_power;
	}
	if (rank > 0 && rank <= static_cast<std::size_t>(std::max(0, defines.secondary_power_rank))) {
		return defines.diplomatic_points_secondary;
	}
	return defines.diplomatic_points_other;
}

void CRM::monthly_update(InstanceManager& instance_manager) {
	if (diplomacy_defines == nullptr) {
		return;
	}
	OpenVicDiplomacyDefines const& defines = *diplomacy_defines;

	// The diplomatic points of the month, by the rank of the country
	for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		if (!country.exists() || country.is_rebel_country()) {
			continue;
		}
		const fixed_point_t gain = get_diplomatic_points_gain(country);
		country.set_diplomatic_points(std::min(defines.diplomatic_points_max, country.get_diplomatic_points() + gain));
	}

	// The processes of the buttons, in the order they were started
	memory::vector<CountryInstancePair> finished;
	for (auto const& [pair, direction] : relation_processes) {
		CountryInstance* const sender = const_cast<CountryInstance*>(pair.first);
		CountryInstance* const receiver = const_cast<CountryInstance*>(pair.second);
		if (sender == nullptr || receiver == nullptr || !sender->exists() || !receiver->exists() || get_at_war_with(sender, receiver)) {
			finished.push_back(pair);
			continue;
		}
		const std::int32_t high = get_relation_limit(sender, receiver);
		const std::int32_t low = defines.relation_min;
		std::int32_t relation = get_country_relation(sender, receiver);
		if ((direction > 0 && relation >= high) || (direction < 0 && relation <= low)) {
			finished.push_back(pair);
			continue;
		}
		// the diplomacy between the members of an imperial sphere or an Alliance is free (version 93)
		const fixed_point_t cost = sender->shares_sphere_of_kind(*receiver, sphere_kind_t::IMPERIAL)
			|| sender->shares_sphere_of_kind(*receiver, sphere_kind_t::ALLIANCE) ? fixed_point_t::_0 : defines.relation_process_cost;
		if (sender->get_diplomatic_points() < cost) {
			continue; // it waits for the points
		}
		sender->set_diplomatic_points(sender->get_diplomatic_points() - cost);
		relation = direction > 0 ? std::min(high, relation + defines.relation_improve_per_month)
			: std::max(low, relation - defines.relation_worsen_per_month);
		set_country_relation(sender, receiver, static_cast<relation_value_type>(relation));
		if ((direction > 0 && relation >= high) || (direction < 0 && relation <= low)) {
			finished.push_back(pair);
		}
	}
	for (CountryInstancePair const& pair : finished) {
		relation_processes.erase(pair);
	}

	// The relations stay in their limits (from the history, the credits, an alliance that ended)
	for (auto it = relations.begin(); it != relations.end(); ++it) {
		CountryInstance const* const first = it->first.first;
		CountryInstance const* const second = it->first.second;
		const std::int32_t high = get_relation_limit(first, second);
		const std::int32_t value = std::clamp<std::int32_t>(it->second, defines.relation_min, high);
		if (value != it->second) {
			it.value() = static_cast<relation_value_type>(value);
		}
	}

	// The scars of the wars heal after the peace: WAR_RELATION_HEAL_PER_MONTH a month (version 91: 1, decision of the owner 2026-11-14)
	const fixed_point_t heal = fixed_point_t(std::max(0, defines.war_relation_heal_per_month));
	memory::vector<UnorderedCountryInstancePair> healed;
	for (auto it = war_scars.begin(); it != war_scars.end(); ++it) {
		if (get_at_war_with(it->first.first, it->first.second)) {
			continue;
		}
		const fixed_point_t value = it->second + heal;
		if (value >= fixed_point_t::_0) {
			healed.push_back(it->first);
		} else {
			it.value() = value;
		}
	}
	for (UnorderedCountryInstancePair const& pair : healed) {
		war_scars.erase(pair);
	}
}

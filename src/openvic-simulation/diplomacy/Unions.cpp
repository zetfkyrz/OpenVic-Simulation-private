#include "Unions.hpp"

#include <algorithm>

#include "openvic-simulation/DefinitionManager.hpp"
#include "openvic-simulation/InstanceManager.hpp"
#include "openvic-simulation/country/CountryDefinition.hpp"
#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/country/Sphere.hpp"
#include "openvic-simulation/defines/OpenVicDefines.hpp"
#include "openvic-simulation/diplomacy/CountryRelation.hpp"
#include "openvic-simulation/diplomacy/DiplomaticAction.hpp"
#include "openvic-simulation/diplomacy/Subjects.hpp"
#include "openvic-simulation/map/MapInstance.hpp"
#include "openvic-simulation/map/ProvinceInstance.hpp"
#include "openvic-simulation/map/State.hpp"
#include "openvic-simulation/military/War.hpp"
#include "openvic-simulation/misc/NotificationLog.hpp"
#include "openvic-simulation/population/Culture.hpp"
#include "openvic-simulation/research/Technology.hpp"
#include "openvic-simulation/utility/Logger.hpp"

using namespace OpenVic;

namespace {
	OpenVicDiplomacyDefines const& defines_of(InstanceManager const& instance_manager) {
		return instance_manager.definition_manager.get_define_manager().get_openvic_defines().diplomacy;
	}

	CultureGroup const* group_of(CountryInstance const& country) {
		return country.get_primary_culture() != nullptr ? &country.get_primary_culture()->group : nullptr;
	}

	// A deterministic dice in [0, 100) from two countries, the date and a salt
	int dice(CountryInstance const& a, CountryInstance const& b, const Date date, const std::uint64_t salt) {
		std::uint64_t x = (static_cast<std::uint64_t>(type_safe::get(a.index)) + 1) * 0x9E3779B97F4A7C15ULL;
		x ^= (static_cast<std::uint64_t>(type_safe::get(b.index)) + 7) * 0xC2B2AE3D27D4EB4FULL;
		x ^= static_cast<std::uint64_t>((date - Date {}).to_int()) * 0xBF58476D1CE4E5B9ULL;
		x ^= salt * 0x94D049BB133111EBULL;
		x ^= x >> 29;
		x *= 0xBF58476D1CE4E5B9ULL;
		x ^= x >> 32;
		return static_cast<int>(x % 100);
	}

	// The province of the capital of the country is in the state
	bool has_capital(State const& state, CountryInstance const& country) {
		ProvinceInstance const* const capital = country.get_capital();
		return capital != nullptr && capital->get_state() == &state;
	}
}

CountryDefinition const* Unions::get_union_definition(CountryInstance const& country) {
	CultureGroup const* const group = group_of(country);
	return group != nullptr ? group->union_country : nullptr;
}

CountryInstance& Unions::get_union_instance(InstanceManager& instance_manager, CountryDefinition const& union_definition) {
	return instance_manager.get_country_instance_manager().get_country_instance_by_definition(union_definition);
}

CountryInstance* Unions::get_union_holder(InstanceManager& instance_manager, CountryDefinition const& union_definition) {
	CountryInstance& union_instance = get_union_instance(instance_manager, union_definition);
	if (union_instance.union_holder != nullptr && union_instance.union_holder->exists()) {
		return union_instance.union_holder;
	}
	return union_instance.exists() ? &union_instance : nullptr;
}

bool Unions::is_union_state(State const& state, CountryInstance const& union_instance) {
	for (ProvinceInstance const& province : state.get_provinces()) {
		if (province.get_cores().contains(const_cast<CountryInstance*>(&union_instance))) {
			return true;
		}
	}
	return false;
}

memory::vector<Unions::requirement_t> Unions::get_form_requirements(InstanceManager& instance_manager, CountryInstance& country) {
	memory::vector<requirement_t> result;
	CountryDefinition const* const definition = get_union_definition(country);
	if (definition == nullptr) {
		result.push_back({ "У нашей культурной группы нет союза наций", false });
		return result;
	}
	OpenVicDiplomacyDefines const& defines = defines_of(instance_manager);
	CountryInstance& union_instance = get_union_instance(instance_manager, *definition);
	result.push_back({ "Союз ещё не образован", get_union_holder(instance_manager, *definition) == nullptr && &union_instance != &country });
	result.push_back({ "Мы — великая держава", country.is_great_power() });
	const fixed_point_t prestige = country.get_prestige_untracked();
	result.push_back({ memory::fmt::format("Престиж не ниже {} (сейчас {})", defines.union_form_prestige, prestige.floor<std::int32_t>()),
		prestige >= fixed_point_t(defines.union_form_prestige) });
	result.push_back({ "Мы не воюем", !country.is_at_war() });
	result.push_back({ "Мы не чей-то субъект", country.overlord == nullptr });
	// the land of the union cores: ours, of the members of our sphere or of our subjects
	std::size_t ours = 0;
	std::size_t missing = 0;
	CountryInstance const* example = nullptr;
	for (ProvinceInstance const& province : instance_manager.get_map_instance().get_province_instances()) {
		if (!province.get_cores().contains(&union_instance)) {
			continue;
		}
		CountryInstance const* const owner = province.get_owner();
		if (owner == &country) {
			ours++;
		} else if (owner == nullptr || owner->get_sphere_owner_untracked() == &country || owner->overlord == &country) {
			continue;
		} else {
			missing++;
			if (example == nullptr) {
				example = owner;
			}
		}
	}
	memory::string text = "Все земли с ядрами союза — наши, нашей сферы или наших субъектов";
	if (missing > 0) {
		text += memory::fmt::format(" (ещё {} пров.)", missing);
	} else if (ours == 0) {
		text += " (у нас нет таких земель)";
	}
	result.push_back({ text, missing == 0 && ours > 0, example });
	return result;
}

memory::string Unions::get_form_block_reason(InstanceManager& instance_manager, CountryInstance& country) {
	for (requirement_t const& requirement : get_form_requirements(instance_manager, country)) {
		if (!requirement.met) {
			return requirement.text;
		}
	}
	return {};
}

bool Unions::form_union(InstanceManager& instance_manager, CountryInstance& country) {
	if (!get_form_block_reason(instance_manager, country).empty()) {
		return false;
	}
	OpenVicDiplomacyDefines const& defines = defines_of(instance_manager);
	CountryDefinition const& definition = *get_union_definition(country);
	CountryInstance& union_instance = get_union_instance(instance_manager, definition);
	const memory::string old_identifier { country.get_display_identifier() };
	SPDLOG_INFO("Unions: {} forms {}", country.get_identifier(), definition.get_identifier());
	country.union_identity = &definition;
	union_instance.union_holder = &country;
	// the cores of the union are its cores
	for (ProvinceInstance& province : instance_manager.get_map_instance().get_province_instances()) {
		if (province.get_cores().contains(&union_instance) && !province.get_cores().contains(&country)) {
			province.add_core(country, false);
		}
	}
	// all the cultures of the group are accepted
	CultureGroup const* const group = group_of(country);
	for (Culture const& culture : instance_manager.definition_manager.get_pop_manager().get_culture_manager().get_cultures()) {
		if (&culture.group == group && !country.is_primary_or_accepted_culture(culture)) {
			country.add_accepted_culture(culture);
		}
	}
	country.add_prestige(fixed_point_t(defines.union_form_prestige_gain));
	instance_manager.get_notification_log().add(instance_manager.get_today(), &country, "union_formed", old_identifier, nullptr);
	// the small countries of the group in our sphere and our subjects are asked (as the event 11101 of Vic2)
	DiplomaticActionManager const& actions = instance_manager.definition_manager.get_diplomatic_action_manager();
	const std::optional<std::size_t> index = actions.get_action_index("join_union");
	CountryRelationManager& relations = instance_manager.get_country_relation_manager();
	for (CountryInstance& member : instance_manager.get_country_instance_manager().get_country_instances()) {
		if (!may_join(instance_manager, country, member)) {
			continue;
		}
		if (!member.is_ai()) {
			if (index.has_value()) {
				actions.propose(*index, &country, &member, instance_manager, false);
			}
		} else if (dice(country, member, instance_manager.get_today(), 0x11101) < defines.union_join_chance) {
			join_union(instance_manager, country, member);
		} else {
			relations.change_relation(&member, &country, -100);
			member.add_prestige(fixed_point_t::_1);
			SPDLOG_INFO("Unions: {} refuses to join {}", member.get_identifier(), definition.get_identifier());
		}
	}
	return true;
}

bool Unions::may_join(InstanceManager& instance_manager, CountryInstance const& holder, CountryInstance const& member) {
	if (&member == &holder || !member.exists() || member.is_great_power() || member.is_rebel_country() || holder.union_identity == nullptr
		|| group_of(member) != group_of(holder) || member.is_at_war() || holder.is_at_war()) {
		return false;
	}
	return member.get_sphere_owner_untracked() == &holder || member.overlord == &holder;
}

void Unions::join_union(InstanceManager& instance_manager, CountryInstance& holder, CountryInstance& member) {
	SPDLOG_INFO("Unions: {} joins {}", member.get_identifier(), holder.get_identifier());
	instance_manager.get_notification_log().add(instance_manager.get_today(), &holder, "union_joined", member.get_identifier(), &member);
	Subjects::absorb(instance_manager, holder, member);
}

std::string_view Unions::get_unification_block_reason(InstanceManager& instance_manager, CountryInstance const& attacker, CountryInstance const& target) {
	CountryDefinition const* const definition = get_union_definition(attacker);
	if (definition == nullptr) {
		return "У нашей культурной группы нет союза наций";
	}
	CountryInstance const* const holder = get_union_holder(instance_manager, *definition);
	if (holder != nullptr && holder != &attacker) {
		return "Союз уже образован другой страной";
	}
	if (holder != &attacker && !attacker.is_great_power()) {
		return "Объединяет только великая держава или сам союз";
	}
	if (CountryInstance::are_vic2_technologies_enabled()) {
		Technology const* const technology = instance_manager.definition_manager.get_research_manager().get_technology_manager()
			.get_technology_by_identifier("nationalism_n_imperialism");
		if (technology != nullptr && !attacker.is_technology_unlocked(*technology)) {
			return "Нужна технология «Национализм и империализм»";
		}
	}
	if (group_of(target) == group_of(attacker)) {
		return "Страну нашей культурной группы присоединяют, а не завоёвывают";
	}
	if (target.overlord == &attacker) {
		return "Это наш субъект";
	}
	CountryInstance const& union_instance = get_union_instance(instance_manager, *definition);
	for (State const* state : target.get_states()) {
		if (state != nullptr && !has_capital(*state, target) && is_union_state(*state, union_instance)) {
			return {};
		}
	}
	return "У этой страны нет земель с ядрами союза";
}

memory::vector<CountryInstance*> Unions::get_hegemony_members(
	InstanceManager& instance_manager, CountryInstance const& winner, CountryInstance const& loser
) {
	memory::vector<CountryInstance*> result;
	CultureGroup const* const group = group_of(winner);
	if (group == nullptr || group->union_country == nullptr) {
		return result;
	}
	Sphere const* const sphere = instance_manager.get_sphere_manager().find_led_by(&loser);
	if (sphere == nullptr) {
		return result;
	}
	for (CountryInstance* member : sphere->members) {
		if (member != nullptr && member != &winner && group_of(*member) == group) {
			result.push_back(member);
		}
	}
	return result;
}

void Unions::ai_monthly(InstanceManager& instance_manager) {
	OpenVicDiplomacyDefines const& defines = defines_of(instance_manager);
	const Date today = instance_manager.get_today();
	DiplomaticActionManager const& actions = instance_manager.definition_manager.get_diplomatic_action_manager();
	const std::optional<std::size_t> join_index = actions.get_action_index("join_union");
	CountryInstanceManager& countries = instance_manager.get_country_instance_manager();
	for (CountryInstance& country : countries.get_country_instances()) {
		if (!country.is_ai() || !country.exists() || country.is_rebel_country() || get_union_definition(country) == nullptr) {
			continue;
		}
		// the decision (ai_will_do = 1 in Vic2)
		if (country.union_identity == nullptr && get_form_block_reason(instance_manager, country).empty()) {
			form_union(instance_manager, country);
			continue;
		}
		// the union invites the members that like it
		if (country.union_identity != nullptr && join_index.has_value()) {
			for (CountryInstance& member : countries.get_country_instances()) {
				if (may_join(instance_manager, country, member)
					&& instance_manager.get_country_relation_manager().get_effective_relation(&member, &country) >= defines.union_join_relation) {
					actions.execute_diplomatic_action(*join_index, &country, &member, {}, instance_manager);
					break;
				}
			}
		}
		// a war of unification against the weakest country that holds the union land
		if (country.is_at_war() || dice(country, country, today, 0x8d8) >= defines.ai_unification_war_chance) {
			continue;
		}
		CountryInstance* target = nullptr;
		fixed_point_t target_power = fixed_point_t::_0;
		const fixed_point_t own_power = country.military_power.get_untracked();
		for (CountryInstance& candidate : countries.get_country_instances()) {
			if (&candidate == &country || !candidate.exists() || candidate.is_rebel_country()
				|| !instance_manager.get_war_manager().can_declare_war(country, candidate, today)
				|| !get_unification_block_reason(instance_manager, country, candidate).empty()) {
				continue;
			}
			const fixed_point_t power = candidate.military_power.get_untracked();
			if (power * 100 > own_power * defines.ai_unification_max_power_pct) {
				continue;
			}
			if (target == nullptr || power < target_power) {
				target = &candidate;
				target_power = power;
			}
		}
		if (target != nullptr && instance_manager.get_war_manager().declare_war_as_country(
			instance_manager, country, *target, WarGoal { WarGoal::type_t::UNIFICATION, nullptr }
		)) {
			SPDLOG_INFO("AI: {} fights a war of unification against {}", country.get_identifier(), target->get_identifier());
		}
	}
}

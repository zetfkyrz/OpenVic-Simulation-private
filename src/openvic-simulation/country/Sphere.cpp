#include "Sphere.hpp"

#include <algorithm>

#include "openvic-simulation/DefinitionManager.hpp"
#include "openvic-simulation/InstanceManager.hpp"
#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/economy/GoodDefinition.hpp"
#include "openvic-simulation/economy/credit/Credit.hpp"
#include "openvic-simulation/economy/trading/TradeNetwork.hpp"
#include "openvic-simulation/map/MapInstance.hpp"
#include "openvic-simulation/map/ProvinceInstance.hpp"
#include "openvic-simulation/map/State.hpp"
#include "openvic-simulation/population/PopType.hpp"
#include "openvic-simulation/utility/Logger.hpp"

using namespace OpenVic;
using namespace OpenVic::NodeTools;

std::string_view OpenVic::sphere_kind_identifier(const sphere_kind_t kind) {
	switch (kind) {
	case sphere_kind_t::CUSTOMS_UNION: return "customs_union";
	case sphere_kind_t::IMPERIAL: return "imperial";
	case sphere_kind_t::MILITARY: return "military";
	case sphere_kind_t::IDEOLOGICAL: return "ideological";
	case sphere_kind_t::ALLIANCE: return "alliance";
	default: return "none";
	}
}

std::string_view OpenVic::principle_effect_identifier(const principle_effect_t effect) {
	switch (effect) {
	case principle_effect_t::OUTPUT: return "output";
	case principle_effect_t::RESEARCH: return "research";
	case principle_effect_t::LITERACY: return "literacy";
	case principle_effect_t::FREIGHT: return "freight";
	case principle_effect_t::LOAN_INTEREST: return "loan_interest";
	case principle_effect_t::MILITANCY: return "militancy";
	default: return "none";
	}
}

std::uint32_t SpherePrincipleDefinitions::find(std::string_view identifier) const {
	for (std::size_t index = 0; index < principles.size(); ++index) {
		if (principles[index].identifier == identifier) {
			return static_cast<std::uint32_t>(index);
		}
	}
	return Sphere::NO_PRINCIPLE;
}

std::size_t SpherePrincipleDefinitions::load_file(std::string_view source, ast::NodeCPtr root, GoodDefinitionManager const& good_manager) {
	std::size_t loaded = 0;
	const std::size_t good_count = good_manager.get_good_definition_count();
	const bool parsed = expect_dictionary([this, source, &good_manager, good_count, &loaded](std::string_view key, ast::NodeCPtr value) -> bool {
		principle_t principle;
		principle.identifier = key;
		bool effect_ok = false;
		const auto mark = [&principle, good_count](GoodDefinition const& good) {
			principle.goods.resize(good_count, 0);
			const std::size_t index = type_safe::get(good.index);
			if (index < principle.goods.size()) {
				principle.goods[index] = 1;
			}
		};
		const bool ok = expect_dictionary_keys_and_default(
			[source, key](std::string_view inner, ast::NodeCPtr) -> bool {
				spdlog::warn_s("Unknown key \"{}\" in the sphere principle {} ({})", inner, key, source);
				return true;
			},
			"effect", ONE_EXACTLY, expect_identifier([&principle, &effect_ok](std::string_view identifier) -> bool {
				for (std::size_t i = 0; i < static_cast<std::size_t>(principle_effect_t::COUNT); ++i) {
					if (principle_effect_identifier(static_cast<principle_effect_t>(i)) == identifier) {
						principle.effect = static_cast<principle_effect_t>(i);
						effect_ok = true;
					}
				}
				return true;
			}),
			"value", ONE_EXACTLY, expect_fixed_point([&principle](fixed_point_t amount) -> bool {
				principle.value = amount;
				return true;
			}),
			"goods", ZERO_OR_ONE, expect_list(expect_identifier_or_string([&good_manager, &mark, key, source](std::string_view identifier) -> bool {
				GoodDefinition const* const good = good_manager.get_good_definition_by_identifier(identifier);
				if (good == nullptr) {
					spdlog::warn_s("The good \"{}\" of the sphere principle {} does not exist ({})", identifier, key, source);
					return true;
				}
				mark(*good);
				return true;
			})),
			"good_categories", ZERO_OR_ONE, expect_list(expect_identifier_or_string([&good_manager, &mark, key, source](std::string_view identifier) -> bool {
				GoodCategory const* const category = good_manager.get_good_category_by_identifier(identifier);
				if (category == nullptr) {
					spdlog::warn_s("The good category \"{}\" of the sphere principle {} does not exist ({})", identifier, key, source);
					return true;
				}
				for (GoodDefinition const& good : category->get_good_definitions()) {
					mark(good);
				}
				return true;
			}))
		)(value);
		if (!ok || !effect_ok || (principle.effect == principle_effect_t::OUTPUT && principle.goods.empty())) {
			spdlog::warn_s("The sphere principle {} is skipped: it has errors, an unknown effect or no goods ({})", key, source);
			return true;
		}
		// A later file (a mod) with the same identifier replaces the principle
		for (principle_t& existing : principles) {
			if (existing.identifier == principle.identifier) {
				existing = std::move(principle);
				loaded++;
				return true;
			}
		}
		principles.push_back(std::move(principle));
		loaded++;
		return true;
	})(root);
	if (!parsed) {
		spdlog::warn_s("The sphere principles file {} is broken, what could be read is used", source);
	}
	return loaded;
}

namespace {
	OpenVicDiplomacyDefines const& defines_of(InstanceManager const& instance_manager) {
		return instance_manager.definition_manager.get_define_manager().get_openvic_defines().diplomacy;
	}

	bool is_active(CountryInstance const& country) {
		return country.exists() && !country.is_rebel_country() && !country.get_owned_provinces().empty();
	}

	bool is_great_power(OpenVicDiplomacyDefines const& defines, CountryInstance const& country) {
		const std::size_t rank = country.get_total_rank();
		return rank > 0 && rank <= static_cast<std::size_t>(std::max(0, defines.great_power_rank));
	}

	// The two share a land border
	bool land_neighbours(InstanceManager const& instance_manager, CountryInstance const& a, CountryInstance const& b) {
		MapInstance const& map_instance = instance_manager.get_map_instance();
		for (ProvinceInstance const* province : a.get_owned_provinces()) {
			for (ProvinceDefinition::adjacency_t const& adjacency : province->province_definition.get_adjacencies()) {
				if (adjacency.get_type() == ProvinceDefinition::adjacency_t::type_t::LAND && !adjacency.get_to().is_water()
					&& map_instance.get_province_instance_by_definition(adjacency.get_to()).get_owner() == &b) {
					return true;
				}
			}
		}
		return false;
	}

	// The income of the pops of the country of a day (the national income, the GDP of a day)
	fixed_point_t daily_income(CountryInstance const& country) {
		fixed_point_t sum = fixed_point_t::_0;
		for (auto const& [pop_type, income] : country.get_taxable_income_by_pop_type()) {
			sum += income;
		}
		return sum;
	}

	// The kind of the sphere an AI great power founds (version 93): a colonial empire (a quarter of its provinces in colonial states) an imperial
	// sphere, a country whose army ranks above its industry a military union, else a customs union
	sphere_kind_t ai_sphere_kind(CountryInstance const& country) {
		std::size_t colonial = 0, all = 0;
		for (ProvinceInstance const* province : country.get_owned_provinces()) {
			++all;
			if (province->get_state() != nullptr && province->get_state()->is_colonial_state()) {
				++colonial;
			}
		}
		if (all > 0 && colonial * 4 >= all) {
			return sphere_kind_t::IMPERIAL;
		}
		const std::size_t military = country.get_military_rank(), industrial = country.get_industrial_rank();
		if (military > 0 && industrial > 0 && military < industrial) {
			return sphere_kind_t::MILITARY;
		}
		return sphere_kind_t::CUSTOMS_UNION;
	}

	// The percent of the influence by the relation (decision of the owner 2026-11-14)
	std::int32_t relation_percent(OpenVicDiplomacyDefines const& defines, const std::int32_t relation) {
		if (relation < -100) {
			return defines.sphere_relation_hostile_pct;
		}
		if (relation <= 0) {
			return defines.sphere_relation_negative_pct;
		}
		if (relation <= 100) {
			return defines.sphere_relation_positive_pct;
		}
		return defines.sphere_relation_high_pct;
	}
}

std::size_t StartSphereDefinitions::load_file(std::string_view source, ast::NodeCPtr root) {
	std::size_t loaded = 0;
	const bool parsed = expect_dictionary([this, source, &loaded](std::string_view key, ast::NodeCPtr value) -> bool {
		entry_t entry;
		entry.leader = key;
		entry.from = Date {};
		entry.until = Date { 10000 };
		bool kind_ok = true;
		const bool ok = expect_dictionary_keys_and_default(
			[source, key](std::string_view inner, ast::NodeCPtr) -> bool {
				spdlog::warn_s("Unknown key \"{}\" in the start sphere of {} ({})", inner, key, source);
				return true;
			},
			"kind", ONE_EXACTLY, expect_identifier([&entry, &kind_ok](std::string_view identifier) -> bool {
				kind_ok = false;
				for (std::size_t i = 0; i < static_cast<std::size_t>(sphere_kind_t::COUNT); ++i) {
					if (sphere_kind_identifier(static_cast<sphere_kind_t>(i)) == identifier) {
						entry.kind = static_cast<sphere_kind_t>(i);
						kind_ok = true;
					}
				}
				return true;
			}),
			"members", ONE_EXACTLY, expect_list(expect_identifier_or_string([&entry](std::string_view tag) -> bool {
				entry.members.emplace_back(tag);
				return true;
			})),
			"from", ZERO_OR_ONE, expect_date([&entry](Date date) -> bool {
				entry.from = date;
				return true;
			}),
			"until", ZERO_OR_ONE, expect_date([&entry](Date date) -> bool {
				entry.until = date;
				return true;
			})
		)(value);
		if (!ok || !kind_ok || entry.members.empty()) {
			spdlog::warn_s("The start sphere of {} is skipped: it has errors, an unknown kind or no members ({})", key, source);
			return true;
		}
		// A later file (a mod) with the same leader replaces the entry
		for (entry_t& existing : entries) {
			if (existing.leader == entry.leader) {
				existing = std::move(entry);
				loaded++;
				return true;
			}
		}
		entries.push_back(std::move(entry));
		loaded++;
		return true;
	})(root);
	if (!parsed) {
		spdlog::warn_s("The start spheres file {} is broken, what could be read is used", source);
	}
	return loaded;
}

bool SphereManager::is_protected(CountryInstance const* country, const Date today) const {
	const auto it = protected_until.find(country);
	return it != protected_until.end() && today < it->second;
}

void SphereManager::setup_start_spheres(InstanceManager& instance_manager, StartSphereDefinitions const& definitions) {
	OpenVicDiplomacyDefines const& defines = defines_of(instance_manager);
	const Date today = instance_manager.get_today();
	const Date protection = today + Timespan::from_years(std::max(0, defines.sphere_start_protection_years));
	CountryInstanceManager& countries = instance_manager.get_country_instance_manager();
	for (StartSphereDefinitions::entry_t const& entry : definitions.get_entries()) {
		if (today < entry.from || today >= entry.until) {
			continue;
		}
		CountryInstance* const leader = countries.get_country_instance_by_identifier(entry.leader);
		if (leader == nullptr || !is_active(*leader) || find_of(leader) != nullptr) {
			spdlog::warn_s("The start sphere of {} is skipped: no such country now, or it already is in a sphere", entry.leader);
			continue;
		}
		Sphere& sphere = spheres.emplace_back();
		sphere.id = next_id++;
		sphere.leader = leader;
		sphere.kind = entry.kind;
		// the colour of the country is computed with the first update of the game state, after this: the colour of its definition
		sphere.colour = leader->country_definition.get_colour();
		sphere.founded = today;
		sphere.kind_changed = today;
		sphere.protected_until = protection;
		leader->set_leads_sphere(true);
		leader->set_sphere_kind(sphere.kind);
		leader->set_sphere_colour(sphere.colour);
		for (memory::string const& tag : entry.members) {
			CountryInstance* const member = countries.get_country_instance_by_identifier(tag);
			// a country that does not exist at this date (Tripoli, taken back by the Ottomans in 1835) is simply not there
			if (member == nullptr || member == leader || !is_active(*member) || find_of(member) != nullptr) {
				continue;
			}
			sphere.members.push_back(member);
			set_membership(*member, leader, sphere.kind);
			protected_until.insert({ member, protection });
		}
		SPDLOG_INFO("Sphere: {} starts with a {} of {} members", leader->get_identifier(), sphere_kind_identifier(sphere.kind), sphere.members.size());
	}
}

Sphere const* SphereManager::find_led_by(CountryInstance const* leader) const {
	for (Sphere const& sphere : spheres) {
		if (sphere.leader == leader) {
			return &sphere;
		}
	}
	return nullptr;
}

Sphere const* SphereManager::find_of(CountryInstance const* country) const {
	for (Sphere const& sphere : spheres) {
		if (sphere.leader == country || std::find(sphere.members.begin(), sphere.members.end(), country) != sphere.members.end()) {
			return &sphere;
		}
	}
	return nullptr;
}

SphereManager::influence_t SphereManager::get_influence(CountryInstance const* leader, CountryInstance const* country) const {
	const auto it = influence.find({ leader, country });
	return it == influence.end() ? influence_t {} : it->second;
}

void SphereManager::set_membership(CountryInstance& country, CountryInstance const* leader, const sphere_kind_t kind) {
	country.set_sphere_owner_country(leader);
	country.set_sphere_kind(leader != nullptr ? kind : sphere_kind_t::COUNT);
}

std::string_view SphereManager::found_block_reason(InstanceManager const& instance_manager, CountryInstance const& country, sphere_kind_t kind) const {
	OpenVicDiplomacyDefines const& defines = defines_of(instance_manager);
	if (kind >= sphere_kind_t::COUNT) {
		return "no such kind of sphere";
	}
	if (kind == sphere_kind_t::ALLIANCE) {
		return "the Alliance needs the technology of the multilateral alliances";
	}
	if (!is_active(country) || !is_great_power(defines, country)) {
		return "only a great power founds a sphere";
	}
	if (find_of(&country) != nullptr) {
		return "the country already is in a sphere";
	}
	return {};
}

bool SphereManager::found(InstanceManager& instance_manager, CountryInstance& country, sphere_kind_t kind) {
	if (!found_block_reason(instance_manager, country, kind).empty()) {
		return false;
	}
	Sphere& sphere = spheres.emplace_back();
	sphere.id = next_id++;
	sphere.leader = &country;
	sphere.kind = kind;
	sphere.colour = country.get_colour();
	sphere.founded = instance_manager.get_today();
	sphere.kind_changed = sphere.founded;
	country.set_leads_sphere(true);
	country.set_sphere_kind(kind);
	country.set_sphere_colour(sphere.colour);
	instance_manager.get_notification_log().add(instance_manager.get_today(), &country, "sphere_founded", sphere_kind_identifier(kind));
	SPDLOG_INFO("Sphere: {} founds a {}", country.get_identifier(), sphere_kind_identifier(kind));
	return true;
}

std::string_view SphereManager::invite_block_reason(InstanceManager const& instance_manager, CountryInstance const& leader, CountryInstance const& country) const {
	OpenVicDiplomacyDefines const& defines = defines_of(instance_manager);
	Sphere const* const sphere = find_led_by(&leader);
	if (sphere == nullptr) {
		return "the country leads no sphere";
	}
	if (&leader == &country || !is_active(country)) {
		return "no such country";
	}
	if (sphere->kind != sphere_kind_t::ALLIANCE && is_great_power(defines, country)) {
		return "only one great power in a sphere (the Alliance allows more)";
	}
	if (find_of(&country) == sphere) {
		return "the country already is a member";
	}
	if (leader.is_at_war_with(country)) {
		return "at war";
	}
	return {};
}

bool SphereManager::accepts(InstanceManager const& instance_manager, CountryInstance const& leader, CountryInstance const& country) const {
	OpenVicDiplomacyDefines const& defines = defines_of(instance_manager);
	const fixed_point_t value = get_influence(&leader, &country).value;
	if (value < fixed_point_t(defines.sphere_join_influence) || country.get_relations_with(leader) < 0) {
		return false;
	}
	// a member of another sphere moves only when the new leader has clearly more influence
	if (Sphere const* const current = find_of(&country); current != nullptr && current->leader != &leader) {
		if (is_protected(&country, instance_manager.get_today())) {
			return false;
		}
		return value >= get_influence(current->leader, &country).value + fixed_point_t(defines.sphere_switch_margin);
	}
	return true;
}

bool SphereManager::invite(InstanceManager& instance_manager, CountryInstance& leader, CountryInstance& country) {
	if (!invite_block_reason(instance_manager, leader, country).empty()) {
		return false;
	}
	// the AI answers by the influence and the relation; a player is not asked yet (the AI does not invite players)
	if (country.is_ai() && !accepts(instance_manager, leader, country)) {
		return false;
	}
	if (find_of(&country) != nullptr) {
		leave(instance_manager, country);
	}
	for (Sphere& sphere : spheres) {
		if (sphere.leader == &leader) {
			sphere.members.push_back(&country);
			set_membership(country, &leader, sphere.kind);
			instance_manager.get_notification_log().add(instance_manager.get_today(), &country, "sphere_joined", "", &leader);
			SPDLOG_INFO("Sphere: {} joins the sphere of {}", country.get_identifier(), leader.get_identifier());
			refresh_principles(instance_manager);
			return true;
		}
	}
	return false;
}

bool SphereManager::leave(InstanceManager& instance_manager, CountryInstance& country) {
	for (Sphere& sphere : spheres) {
		const auto it = std::find(sphere.members.begin(), sphere.members.end(), &country);
		if (it != sphere.members.end()) {
			sphere.members.erase(it);
			set_membership(country, nullptr, sphere_kind_t::COUNT);
			protected_until.erase(&country);
			instance_manager.get_notification_log().add(instance_manager.get_today(), &country, "sphere_left", "", sphere.leader);
			SPDLOG_INFO("Sphere: {} leaves the sphere of {}", country.get_identifier(), sphere.leader->get_identifier());
			refresh_principles(instance_manager);
			return true;
		}
	}
	return false;
}

bool SphereManager::set_look(CountryInstance const& leader, const colour_t colour, const std::uint32_t icon) {
	for (Sphere& sphere : spheres) {
		if (sphere.leader == &leader) {
			sphere.colour = colour;
			sphere.icon = icon;
			sphere.leader->set_sphere_colour(colour);
			return true;
		}
	}
	return false;
}

Date SphereManager::get_kind_change_date(InstanceManager const& instance_manager, Sphere const& sphere) const {
	return sphere.kind_changed + Timespan::from_months(std::max(0, defines_of(instance_manager).sphere_kind_change_months));
}

std::string_view SphereManager::change_kind_block_reason(
	InstanceManager const& instance_manager, CountryInstance const& leader, const sphere_kind_t kind
) const {
	Sphere const* const sphere = find_led_by(&leader);
	if (sphere == nullptr) {
		return "the country leads no sphere";
	}
	if (kind >= sphere_kind_t::COUNT) {
		return "no such kind of sphere";
	}
	if (kind == sphere->kind) {
		return "the sphere already is of this kind";
	}
	if (kind == sphere_kind_t::ALLIANCE || sphere->kind == sphere_kind_t::ALLIANCE) {
		return "the Alliance needs the technology of the multilateral alliances";
	}
	if (instance_manager.get_today() < get_kind_change_date(instance_manager, *sphere)) {
		return "the kind was changed not long ago";
	}
	// the military union forbids the wars between the members: not while two of them fight each other
	if (kind == sphere_kind_t::MILITARY) {
		for (CountryInstance const* member : sphere->members) {
			if (leader.is_at_war_with(*member)) {
				return "members are at war with each other";
			}
			for (CountryInstance const* other : sphere->members) {
				if (member != other && member->is_at_war_with(*other)) {
					return "members are at war with each other";
				}
			}
		}
	}
	return {};
}

bool SphereManager::change_kind(InstanceManager& instance_manager, CountryInstance& leader, const sphere_kind_t kind) {
	if (!change_kind_block_reason(instance_manager, leader, kind).empty()) {
		return false;
	}
	for (Sphere& sphere : spheres) {
		if (sphere.leader == &leader) {
			sphere.kind = kind;
			sphere.kind_changed = instance_manager.get_today();
			leader.set_sphere_kind(kind);
			for (CountryInstance* member : sphere.members) {
				member->set_sphere_kind(kind);
			}
			instance_manager.get_notification_log().add(instance_manager.get_today(), &leader, "sphere_kind_changed", sphere_kind_identifier(kind));
			SPDLOG_INFO("Sphere: {} makes its sphere a {}", leader.get_identifier(), sphere_kind_identifier(kind));
			return true;
		}
	}
	return false;
}

void SphereManager::daily_update(InstanceManager& instance_manager) {
	OpenVicDiplomacyDefines const& defines = defines_of(instance_manager);
	for (Sphere& sphere : spheres) {
		sphere.tribute_yesterday = fixed_point_t::_0;
		if (sphere.kind != sphere_kind_t::IMPERIAL || sphere.leader == nullptr || defines.sphere_imperial_tribute.numerator <= 0) {
			continue;
		}
		// The members pay a share of the income of their treasury of yesterday (not more than they have). Later the subjects that already
		// pay their overlord (the plan of the subjects) will be left out
		for (CountryInstance* member : sphere.members) {
			const fixed_point_t income = member->get_income_yesterday();
			if (income <= fixed_point_t::_0) {
				continue;
			}
			const fixed_point_t tribute = std::min(defines.sphere_imperial_tribute.of(income), member->get_cash_stockpile().load());
			if (tribute <= fixed_point_t::_0) {
				continue;
			}
			member->spend_cash(tribute);
			sphere.leader->add_cash(tribute);
			sphere.tribute_yesterday += tribute;
		}
	}
}

void SphereManager::dissolve(InstanceManager& instance_manager, const std::size_t index) {
	Sphere& sphere = spheres[index];
	for (CountryInstance* member : sphere.members) {
		set_membership(*member, nullptr, sphere_kind_t::COUNT);
	}
	if (sphere.leader != nullptr) {
		sphere.leader->set_leads_sphere(false);
		sphere.leader->set_sphere_kind(sphere_kind_t::COUNT);
		instance_manager.get_notification_log().add(instance_manager.get_today(), sphere.leader, "sphere_dissolved", sphere_kind_identifier(sphere.kind));
		SPDLOG_INFO("Sphere: the sphere of {} ends", sphere.leader->get_identifier());
	}
	spheres.erase(spheres.begin() + static_cast<std::ptrdiff_t>(index));
	refresh_principles(instance_manager);
}

std::size_t SphereManager::get_principle_slots(InstanceManager const& instance_manager, Sphere const& sphere) const {
	OpenVicDiplomacyDefines const& defines = defines_of(instance_manager);
	const std::int32_t countries = static_cast<std::int32_t>(sphere.members.size()) + 1;
	std::size_t slots = 1;
	if (countries >= defines.sphere_principle_slot2_countries) {
		slots = 2;
	}
	if (countries >= defines.sphere_principle_slot3_countries) {
		slots = 3;
	}
	return std::min(slots, Sphere::MAX_PRINCIPLE_SLOTS);
}

std::int32_t SphereManager::get_principle_level(InstanceManager const& instance_manager, Sphere::principle_slot_t const& slot) const {
	if (slot.principle == Sphere::NO_PRINCIPLE) {
		return 0;
	}
	OpenVicDiplomacyDefines const& defines = defines_of(instance_manager);
	const Date today = instance_manager.get_today();
	if (today >= slot.since + Timespan::from_years(std::max(0, defines.sphere_principle_level3_years))) {
		return 3;
	}
	if (today >= slot.since + Timespan::from_years(std::max(0, defines.sphere_principle_level2_years))) {
		return 2;
	}
	return 1;
}

fixed_point_t SphereManager::get_principle_bonus(InstanceManager const& instance_manager, Sphere::principle_slot_t const& slot) const {
	SpherePrincipleDefinitions const& definitions = instance_manager.definition_manager.get_sphere_principle_definitions();
	if (slot.principle >= definitions.get_principles().size()) {
		return fixed_point_t::_0;
	}
	OpenVicDiplomacyDefines const& defines = defines_of(instance_manager);
	const std::int32_t level = get_principle_level(instance_manager, slot);
	const std::int32_t percent = level >= 3 ? defines.sphere_principle_level3_pct : level == 2 ? defines.sphere_principle_level2_pct
		: defines.sphere_principle_level1_pct;
	return definitions.get_principles()[slot.principle].value * percent / 100;
}

Date SphereManager::get_principle_next_level_date(InstanceManager const& instance_manager, Sphere::principle_slot_t const& slot) const {
	OpenVicDiplomacyDefines const& defines = defines_of(instance_manager);
	switch (get_principle_level(instance_manager, slot)) {
	case 1: return slot.since + Timespan::from_years(std::max(0, defines.sphere_principle_level2_years));
	case 2: return slot.since + Timespan::from_years(std::max(0, defines.sphere_principle_level3_years));
	default: return Date {};
	}
}

std::string_view SphereManager::set_principle_block_reason(
	InstanceManager const& instance_manager, CountryInstance const& leader, const std::size_t slot, const std::uint32_t principle
) const {
	Sphere const* const sphere = find_led_by(&leader);
	if (sphere == nullptr) {
		return "the country leads no sphere";
	}
	if (slot >= get_principle_slots(instance_manager, *sphere)) {
		return "the sphere has no such slot (more countries open more slots)";
	}
	if (principle == Sphere::NO_PRINCIPLE) {
		return sphere->principles[slot].principle == Sphere::NO_PRINCIPLE ? "the slot is empty" : "";
	}
	if (principle >= instance_manager.definition_manager.get_sphere_principle_definitions().get_principles().size()) {
		return "no such principle";
	}
	for (std::size_t other = 0; other < Sphere::MAX_PRINCIPLE_SLOTS; ++other) {
		if (sphere->principles[other].principle == principle) {
			return other == slot ? "the principle already is in this slot" : "the principle already is in another slot";
		}
	}
	return {};
}

bool SphereManager::set_principle(InstanceManager& instance_manager, CountryInstance& leader, const std::size_t slot, const std::uint32_t principle) {
	if (!set_principle_block_reason(instance_manager, leader, slot, principle).empty()) {
		return false;
	}
	for (Sphere& sphere : spheres) {
		if (sphere.leader == &leader) {
			sphere.principles[slot] = { principle, instance_manager.get_today() };
			SPDLOG_INFO("Sphere: {} puts the principle {} into the slot {}", leader.get_identifier(), principle == Sphere::NO_PRINCIPLE ? "none"
				: std::string_view { instance_manager.definition_manager.get_sphere_principle_definitions().get_principles()[principle].identifier }, slot + 1);
			refresh_principles(instance_manager);
			return true;
		}
	}
	return false;
}

void SphereManager::refresh_principles(InstanceManager& instance_manager) {
	for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		for (std::size_t effect = 0; effect < static_cast<std::size_t>(principle_effect_t::COUNT); ++effect) {
			country.set_sphere_principle_bonus(static_cast<principle_effect_t>(effect), fixed_point_t::_0);
		}
		country.get_sphere_output_bonus_mutable().clear();
	}
	SpherePrincipleDefinitions const& definitions = instance_manager.definition_manager.get_sphere_principle_definitions();
	for (Sphere& sphere : spheres) {
		const std::size_t slots = get_principle_slots(instance_manager, sphere);
		for (std::size_t slot = 0; slot < Sphere::MAX_PRINCIPLE_SLOTS; ++slot) {
			Sphere::principle_slot_t& principle_slot = sphere.principles[slot];
			// fewer countries, fewer slots: the principles of the closed slots are taken out
			if (slot >= slots || principle_slot.principle >= definitions.get_principles().size()) {
				principle_slot = {};
				continue;
			}
			SpherePrincipleDefinitions::principle_t const& principle = definitions.get_principles()[principle_slot.principle];
			const fixed_point_t bonus = get_principle_bonus(instance_manager, principle_slot);
			const auto apply = [&principle, bonus](CountryInstance& country) {
				if (principle.effect == principle_effect_t::OUTPUT) {
					memory::vector<fixed_point_t>& output = country.get_sphere_output_bonus_mutable();
					output.resize(std::max(output.size(), principle.goods.size()), fixed_point_t::_0);
					for (std::size_t good = 0; good < principle.goods.size(); ++good) {
						if (principle.goods[good] != 0) {
							output[good] += bonus;
						}
					}
				} else {
					country.set_sphere_principle_bonus(principle.effect, country.get_sphere_principle_bonus(principle.effect) + bonus);
				}
			};
			apply(*sphere.leader);
			for (CountryInstance* member : sphere.members) {
				apply(*member);
			}
		}
	}
}

void SphereManager::update_influence(InstanceManager& instance_manager) {
	OpenVicDiplomacyDefines const& defines = defines_of(instance_manager);
	TradeNetwork const& trade = instance_manager.get_trade_network();
	CountryInstanceManager& countries = instance_manager.get_country_instance_manager();
	CreditManager const& credits = instance_manager.get_credit_manager();

	for (CountryInstance& leader : countries.get_country_instances()) {
		if (!is_active(leader) || !is_great_power(defines, leader)) {
			continue;
		}
		for (CountryInstance& country : countries.get_country_instances()) {
			if (&country == &leader || !is_active(country)) {
				continue;
			}
			const fixed_point_t income = daily_income(country);
			influence_t parts {};
			if (income > fixed_point_t::_0) {
				// the trade: what the country bought from the leader yesterday, a share of its income of a day; 1 % = 10 points
				if (trade.is_ready()) {
					parts.trade = trade.get_pair_flow_yesterday(leader.index, country.index) * 1000 / income;
				}
				// the investments: the credits of the leader the country still owes, a share of its yearly income; 1 % = 10 points
				fixed_point_t owed = fixed_point_t::_0;
				for (auto const& loan : credits.get_loans()) {
					if (loan != nullptr && loan->lender == &leader && loan->borrower == &country && loan->status == Loan::status_t::ACTIVE) {
						owed += loan->principal_left;
					}
				}
				parts.investment = owed * 1000 / (income * 365);
			}
			std::int32_t percent = relation_percent(defines, country.get_relations_with(leader));
			if (land_neighbours(instance_manager, leader, country)) {
				percent += defines.sphere_neighbour_pct;
			}
			parts.multiplier = fixed_point_t(percent) / 100;
			const fixed_point_t target = std::max(fixed_point_t::_0, (parts.trade + parts.investment) * (fixed_point_t::_1 + parts.multiplier));
			auto it = influence.find({ &leader, &country });
			const fixed_point_t old_value = it != influence.end() ? it->second.value : fixed_point_t::_0;
			parts.value = old_value + defines.sphere_influence_smoothing.of(target - old_value);
			if (it == influence.end()) {
				if (parts.value > fixed_point_t::_0 || parts.trade > fixed_point_t::_0 || parts.investment > fixed_point_t::_0) {
					influence.insert({ { &leader, &country }, parts });
				}
			} else {
				it.value() = parts;
			}
		}
	}
}

void SphereManager::monthly_update(InstanceManager& instance_manager) {
	OpenVicDiplomacyDefines const& defines = defines_of(instance_manager);
	const Date today = instance_manager.get_today();
	update_influence(instance_manager);

	// The end of the spheres: the leader is gone, or no great power for SPHERE_GRACE_MONTHS; the members that are gone or became great powers
	for (std::size_t index = spheres.size(); index-- > 0;) {
		Sphere& sphere = spheres[index];
		if (sphere.leader == nullptr || !is_active(*sphere.leader)) {
			dissolve(instance_manager, index);
			continue;
		}
		if (is_great_power(defines, *sphere.leader) || today < sphere.protected_until) {
			sphere.leader_lost_rank = Date {};
		} else if (sphere.leader_lost_rank == Date {}) {
			sphere.leader_lost_rank = today;
		} else if (today >= sphere.leader_lost_rank + Timespan::from_months(std::max(0, defines.sphere_grace_months))) {
			dissolve(instance_manager, index);
			continue;
		}
		memory::vector<CountryInstance*> gone;
		for (CountryInstance* member : sphere.members) {
			if (!is_active(*member) || (sphere.kind != sphere_kind_t::ALLIANCE && is_great_power(defines, *member)) || sphere.leader->is_at_war_with(*member)) {
				gone.push_back(member);
			}
		}
		for (CountryInstance* member : gone) {
			leave(instance_manager, *member);
		}
	}

	// The AI: a great power with the goal of economic control founds a sphere (its kind by its goals); the leaders invite; the members leave
	for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		if (!country.is_ai() || !is_active(country)) {
			continue;
		}
		if (find_of(&country) == nullptr && is_great_power(defines, country)
			&& country.get_ai_goal() == CountryInstance::ai_goal_t::ECONOMIC_CONTROL) {
			found(instance_manager, country, ai_sphere_kind(country));
		}
		if (Sphere const* const sphere = find_of(&country); sphere != nullptr && sphere->leader != &country) {
			const fixed_point_t value = get_influence(sphere->leader, &country).value;
			if (!is_protected(&country, today) && (value < fixed_point_t(defines.sphere_leave_influence) || country.get_relations_with(*sphere->leader) < 0)) {
				leave(instance_manager, country);
			}
		}
	}
	for (std::size_t index = 0; index < spheres.size(); ++index) {
		CountryInstance& leader = *spheres[index].leader;
		if (!leader.is_ai()) {
			continue;
		}
		// the countries of the most influence first
		memory::vector<std::pair<fixed_point_t, CountryInstance*>> candidates;
		for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
			if (country.is_ai() && invite_block_reason(instance_manager, leader, country).empty() && accepts(instance_manager, leader, country)) {
				candidates.emplace_back(get_influence(&leader, &country).value, &country);
			}
		}
		std::stable_sort(candidates.begin(), candidates.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
		for (std::size_t i = 0; i < candidates.size() && static_cast<std::int32_t>(i) < defines.sphere_ai_invites_per_month; ++i) {
			invite(instance_manager, leader, *candidates[i].second);
		}
	}

	// The AI leaders fill the empty slots of the principles: the ones that suit the kind first, then any (version 94)
	SpherePrincipleDefinitions const& definitions = instance_manager.definition_manager.get_sphere_principle_definitions();
	for (std::size_t index = 0; index < spheres.size(); ++index) {
		CountryInstance& leader = *spheres[index].leader;
		if (!leader.is_ai()) {
			continue;
		}
		std::array<std::string_view, 3> preferred {};
		switch (spheres[index].kind) {
		case sphere_kind_t::CUSTOMS_UNION: preferred = { "trading_posts", "banking_union", "specialists_exchange" }; break;
		case sphere_kind_t::IMPERIAL: preferred = { "banking_union", "mutual_police", "teachers_exchange" }; break;
		case sphere_kind_t::MILITARY: preferred = { "military_industry", "specialists_exchange", "mutual_police" }; break;
		case sphere_kind_t::IDEOLOGICAL: preferred = { "teachers_exchange", "mutual_police", "specialists_exchange" }; break;
		default: preferred = { "specialists_exchange", "military_industry", "trading_posts" }; break;
		}
		memory::vector<std::uint32_t> order;
		for (std::string_view identifier : preferred) {
			const std::uint32_t principle = definitions.find(identifier);
			if (principle != Sphere::NO_PRINCIPLE) {
				order.push_back(principle);
			}
		}
		for (std::uint32_t principle = 0; principle < definitions.get_principles().size(); ++principle) {
			if (std::find(order.begin(), order.end(), principle) == order.end()) {
				order.push_back(principle);
			}
		}
		const std::size_t slots = get_principle_slots(instance_manager, spheres[index]);
		for (std::size_t slot = 0; slot < slots; ++slot) {
			if (spheres[index].principles[slot].principle != Sphere::NO_PRINCIPLE) {
				continue;
			}
			for (const std::uint32_t principle : order) {
				if (set_principle_block_reason(instance_manager, leader, slot, principle).empty()) {
					set_principle(instance_manager, leader, slot, principle);
					break;
				}
			}
		}
	}
	// the levels grow with the time
	refresh_principles(instance_manager);
}

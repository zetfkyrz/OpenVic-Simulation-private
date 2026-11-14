#include "Sphere.hpp"

#include <algorithm>

#include "openvic-simulation/DefinitionManager.hpp"
#include "openvic-simulation/InstanceManager.hpp"
#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/economy/credit/Credit.hpp"
#include "openvic-simulation/economy/trading/TradeNetwork.hpp"
#include "openvic-simulation/map/MapInstance.hpp"
#include "openvic-simulation/map/ProvinceInstance.hpp"
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
		sphere.protected_until = protection;
		leader->set_leads_sphere(true);
		leader->set_sphere_colour(sphere.colour);
		for (memory::string const& tag : entry.members) {
			CountryInstance* const member = countries.get_country_instance_by_identifier(tag);
			// a country that does not exist at this date (Tripoli, taken back by the Ottomans in 1835) is simply not there
			if (member == nullptr || member == leader || !is_active(*member) || find_of(member) != nullptr) {
				continue;
			}
			sphere.members.push_back(member);
			set_membership(*member, leader);
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

void SphereManager::set_membership(CountryInstance& country, CountryInstance const* leader) {
	country.set_sphere_owner_country(leader);
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
	country.set_leads_sphere(true);
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
			set_membership(country, &leader);
			instance_manager.get_notification_log().add(instance_manager.get_today(), &country, "sphere_joined", "", &leader);
			SPDLOG_INFO("Sphere: {} joins the sphere of {}", country.get_identifier(), leader.get_identifier());
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
			set_membership(country, nullptr);
			protected_until.erase(&country);
			instance_manager.get_notification_log().add(instance_manager.get_today(), &country, "sphere_left", "", sphere.leader);
			SPDLOG_INFO("Sphere: {} leaves the sphere of {}", country.get_identifier(), sphere.leader->get_identifier());
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

void SphereManager::dissolve(InstanceManager& instance_manager, const std::size_t index) {
	Sphere& sphere = spheres[index];
	for (CountryInstance* member : sphere.members) {
		set_membership(*member, nullptr);
	}
	if (sphere.leader != nullptr) {
		sphere.leader->set_leads_sphere(false);
		instance_manager.get_notification_log().add(instance_manager.get_today(), sphere.leader, "sphere_dissolved", sphere_kind_identifier(sphere.kind));
		SPDLOG_INFO("Sphere: the sphere of {} ends", sphere.leader->get_identifier());
	}
	spheres.erase(spheres.begin() + static_cast<std::ptrdiff_t>(index));
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
			found(instance_manager, country, sphere_kind_t::CUSTOMS_UNION);
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
}

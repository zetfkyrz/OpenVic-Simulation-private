#include "LawDynamics.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "openvic-simulation/DefinitionManager.hpp"
#include "openvic-simulation/InstanceManager.hpp"
#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/country/CountryInstanceManager.hpp"
#include "openvic-simulation/map/ProvinceInstance.hpp"
#include "openvic-simulation/politics/Ideology.hpp"
#include "openvic-simulation/politics/Law.hpp"
#include "openvic-simulation/population/Pop.hpp"
#include "openvic-simulation/population/PopType.hpp"
#include "openvic-simulation/utility/Logger.hpp"

using namespace OpenVic;

namespace {
	// A deterministic draw for the month (the day, the country, the law): 0 .. 9999
	std::uint32_t draw(const std::int64_t day, const std::size_t country, const std::size_t law) {
		std::uint64_t x = static_cast<std::uint64_t>(day) * 0x9E3779B97F4A7C15ULL ^ (static_cast<std::uint64_t>(country) << 20) ^ static_cast<std::uint64_t>(law);
		x ^= x >> 30;
		x *= 0xBF58476D1CE4E5B9ULL;
		x ^= x >> 27;
		x *= 0x94D049BB133111EBULL;
		x ^= x >> 31;
		return static_cast<std::uint32_t>(x % 10000);
	}

	// The voters of a country by their ideologies: the men of every ideology times the weight of the vote of their strata (the distribution of power)
	std::vector<fixed_point_t> voters_by_ideology(CountryInstance const& country, const std::size_t ideology_count) {
		fixed_point_t vote_poor = country.get_law_effect(law_effect_t::vote_poor);
		fixed_point_t vote_middle = country.get_law_effect(law_effect_t::vote_middle);
		fixed_point_t vote_rich = country.get_law_effect(law_effect_t::vote_rich);
		if (vote_poor <= fixed_point_t::_0 && vote_middle <= fixed_point_t::_0 && vote_rich <= fixed_point_t::_0) {
			vote_rich = fixed_point_t::_1;
		}
		std::vector<fixed_point_t> voters(ideology_count, fixed_point_t::_0);
		for (ProvinceInstance const* province : country.get_owned_provinces()) {
			for (Pop const& pop : province->get_pops()) {
				std::string_view const strata = pop.get_type().strata.get_identifier();
				const fixed_point_t weight = strata == "rich" ? vote_rich : strata == "middle" ? vote_middle : vote_poor;
				if (weight <= fixed_point_t::_0) {
					continue;
				}
				auto const& by_ideology = pop.get_supporter_equivalents_by_ideology();
				std::size_t index = 0;
				for (fixed_point_t const& men : by_ideology) {
					if (index < ideology_count && men > fixed_point_t::_0) {
						// (in thousands, so that the sums of millions fit)
						voters[index] += men * weight / 1000;
					}
					index++;
				}
			}
		}
		return voters;
	}

	// The support of the variant over the law in force: (the voters that like it more) - (the ones that like it less), -1..1
	fixed_point_t support_of(
		LawManager const& laws, std::vector<fixed_point_t> const& voters, LawVariant const& variant, LawVariant const* current, const fixed_point_t status_quo
	) {
		fixed_point_t total = fixed_point_t::_0, balance = fixed_point_t::_0;
		for (std::size_t ideology = 0; ideology < voters.size(); ++ideology) {
			if (voters[ideology] <= fixed_point_t::_0) {
				continue;
			}
			total += voters[ideology];
			const fixed_point_t now = current != nullptr ? laws.get_approval(ideology, *current) + status_quo : fixed_point_t::_0_50;
			balance += voters[ideology] * (laws.get_approval(ideology, variant) - now);
		}
		return total > fixed_point_t::_0 ? std::clamp(balance / total, -fixed_point_t::_1, fixed_point_t::_1) : fixed_point_t::_0;
	}
}

bool LawDynamics::may_pass(CountryInstance const& country, LawVariant const& variant, LawVariant const* current) {
	if (current == nullptr || variant.lean >= current->lean) {
		return true;
	}
	// Back towards the reaction: only a reactionary or fascist government
	CountryParty const* const ruling = country.get_ruling_party_untracked();
	const std::string_view ideology = ruling != nullptr && ruling->ideology != nullptr ? std::string_view { ruling->ideology->get_identifier() } : std::string_view {};
	return ideology == "reactionary" || ideology == "fascist";
}

void LawDynamics::monthly_update(InstanceManager& instance_manager) {
	LawManager const& laws = instance_manager.definition_manager.get_politics_manager().get_law_manager();
	if (laws.empty()) {
		return;
	}
	OpenVicPoliticsDefines const& defines = instance_manager.definition_manager.get_define_manager().get_openvic_defines().politics;
	OpenVicDiplomacyDefines const& diplomacy_defines = instance_manager.definition_manager.get_define_manager().get_openvic_defines().diplomacy;
	const std::size_t ideology_count = instance_manager.definition_manager.get_politics_manager().get_ideology_manager().get_ideologies().size();
	const Date today = instance_manager.get_today();
	const std::int64_t day = static_cast<std::int64_t>(today.get_year()) * 400 + static_cast<std::int64_t>(today.get_day_of_year());
	const bool ai_month = defines.ai_law_month > 0 && static_cast<std::int32_t>(today.get_month()) == defines.ai_law_month;
	const fixed_point_t status_quo = defines.law_status_quo.fixed();

	for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		if (!country.exists() || country.is_rebel_country() || country.get_laws().empty()) {
			continue;
		}
		const bool ai_proposes = ai_month && country.is_ai() && country.get_law_proposal() == nullptr;
		if (!ai_proposes && country.get_law_proposal() == nullptr) {
			continue;
		}
		const std::vector<fixed_point_t> voters = voters_by_ideology(country, ideology_count);

		// The AI proposes the change its voters like the most (when they like it enough)
		if (ai_proposes) {
			LawVariant const* best = nullptr;
			fixed_point_t best_support = defines.ai_law_min_support.fixed();
			for (std::size_t group_index = 0; group_index < laws.get_groups().size(); ++group_index) {
				LawVariant const* const current = country.get_law(group_index);
				for (const std::size_t variant_index : laws.get_groups()[group_index].variants) {
					LawVariant const& variant = laws.get_variants()[variant_index];
					if (&variant == current) {
						continue;
					}
					if (!may_pass(country, variant, current)) {
						continue;
					}
					const fixed_point_t support = support_of(laws, voters, variant, current, status_quo);
					if (support > best_support) {
						best = &variant;
						best_support = support;
					}
				}
			}
			if (best != nullptr) {
				country.set_law_proposal(best);
				country.set_law_stall_months(0);
				SPDLOG_INFO("Law: {} proposes {} (support {:.2f})", country.get_identifier(), best->get_identifier(), static_cast<double>(best_support));
			}
		}

		LawVariant const* const proposal = country.get_law_proposal();
		if (proposal == nullptr) {
			continue;
		}
		LawVariant const* const current = country.get_law(proposal->group_index);
		if (current == proposal) {
			country.set_law_proposal(nullptr);
			continue;
		}
		const fixed_point_t support = support_of(laws, voters, *proposal, current, status_quo);
		country.set_law_support(support);
		if (support > fixed_point_t::_0 && may_pass(country, *proposal, current)) {
			// (the presidential republic passes the laws faster)
			// (the members of an ideological union, version 93: +SPHERE_IDEOLOGICAL_LAW_CHANCE)
			const fixed_point_t sphere_bonus = country.get_sphere_kind() == sphere_kind_t::IDEOLOGICAL
				? diplomacy_defines.sphere_ideological_law_chance.fixed() : fixed_point_t::_0;
			const fixed_point_t chance = std::min(fixed_point_t::_1, (defines.law_pass_base.fixed() + defines.law_pass_per_support.fixed() * support)
				* std::max(fixed_point_t::_0, fixed_point_t::_1 + country.get_law_effect(law_effect_t::law_pass_speed)) + sphere_bonus);
			const std::uint32_t needed = static_cast<std::uint32_t>(std::clamp((chance * 10000).ceil<std::int32_t>(), 0, 10000));
			if (draw(day, type_safe::get(country.index), proposal->index) < needed) {
				SPDLOG_INFO("Law: {} passes {} (support {:.2f})", country.get_identifier(), proposal->get_identifier(), static_cast<double>(support));
				country.set_law(*proposal);
				country.set_law_proposal(nullptr);
				country.set_law_stall_months(0);
			}
			continue;
		}
		// A stall: no support this month
		country.set_law_stall_months(country.get_law_stall_months() + 1);
		if (country.get_law_stall_months() >= static_cast<std::uint32_t>(defines.law_stall_months)) {
			SPDLOG_INFO("Law: {} gives up {} (no support for {} months)", country.get_identifier(), proposal->get_identifier(), country.get_law_stall_months());
			country.set_law_proposal(nullptr);
			country.set_law_stall_months(0);
		}
	}
}

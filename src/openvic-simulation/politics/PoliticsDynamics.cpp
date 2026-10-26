#include "PoliticsDynamics.hpp"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <vector>

#include "openvic-simulation/DefinitionManager.hpp"
#include "openvic-simulation/InstanceManager.hpp"
#include "openvic-simulation/country/CountryDefinition.hpp"
#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/country/CountryParty.hpp"
#include "openvic-simulation/map/ProvinceInstance.hpp"
#include "openvic-simulation/modifier/ModifierEffectCache.hpp"
#include "openvic-simulation/politics/Government.hpp"
#include "openvic-simulation/politics/Ideology.hpp"
#include "openvic-simulation/politics/IssueManager.hpp"
#include "openvic-simulation/politics/PoliticsManager.hpp"
#include "openvic-simulation/population/Pop.hpp"
#include "openvic-simulation/population/PopManager.hpp"
#include "openvic-simulation/population/PopType.hpp"
#include "openvic-simulation/utility/Logger.hpp"

using namespace OpenVic;

namespace {
	// Place of an ideology on the line from conservative (0) to radical (1)
	double ideology_position(OpenVicPoliticsDefines const& d, std::string_view identifier) {
		if (identifier == "reactionary") return d.position_reactionary;
		if (identifier == "conservative") return d.position_conservative;
		if (identifier == "fascist") return d.position_fascist;
		if (identifier == "liberal") return d.position_liberal;
		if (identifier == "anarcho_liberal") return d.position_anarcho_liberal;
		if (identifier == "socialist") return d.position_socialist;
		if (identifier == "communist") return d.position_communist;
		return d.position_other;
	}

	struct IdeologyWeight {
		std::string_view ideology;
		double weight;
	};

	// Starting leanings of the pop types, scaled by the state of the pop below
	std::vector<IdeologyWeight> base_leaning(OpenVicPoliticsDefines const& d, std::string_view pop_type, std::string_view strata) {
		if (pop_type == "aristocrats") return { { "reactionary", d.leaning_aristocrats_reactionary }, { "conservative", d.leaning_aristocrats_conservative }, { "liberal", d.leaning_aristocrats_liberal } };
		if (pop_type == "capitalists") return { { "reactionary", d.leaning_capitalists_reactionary }, { "conservative", d.leaning_capitalists_conservative }, { "liberal", d.leaning_capitalists_liberal } };
		if (pop_type == "bureaucrats") return { { "reactionary", d.leaning_bureaucrats_reactionary }, { "conservative", d.leaning_bureaucrats_conservative }, { "liberal", d.leaning_bureaucrats_liberal } };
		if (pop_type == "clergymen") return { { "reactionary", d.leaning_clergymen_reactionary }, { "conservative", d.leaning_clergymen_conservative }, { "liberal", d.leaning_clergymen_liberal } };
		if (pop_type == "officers") return { { "reactionary", d.leaning_officers_reactionary }, { "conservative", d.leaning_officers_conservative }, { "fascist", d.leaning_officers_fascist }, { "liberal", d.leaning_officers_liberal } };
		if (pop_type == "soldiers") return { { "reactionary", d.leaning_soldiers_reactionary }, { "conservative", d.leaning_soldiers_conservative }, { "liberal", d.leaning_soldiers_liberal } };
		if (pop_type == "farmers") return { { "reactionary", d.leaning_farmers_reactionary }, { "conservative", d.leaning_farmers_conservative }, { "liberal", d.leaning_farmers_liberal } };
		if (pop_type == "labourers") return { { "conservative", d.leaning_labourers_conservative }, { "liberal", d.leaning_labourers_liberal }, { "socialist", d.leaning_labourers_socialist } };
		if (pop_type == "craftsmen" || pop_type == "artisans") return { { "conservative", d.leaning_craftsmen_conservative }, { "liberal", d.leaning_craftsmen_liberal }, { "socialist", d.leaning_craftsmen_socialist } };
		if (pop_type == "clerks") return { { "conservative", d.leaning_clerks_conservative }, { "liberal", d.leaning_clerks_liberal }, { "socialist", d.leaning_clerks_socialist } };
		if (pop_type == "slaves") return { { "conservative", d.leaning_slaves_conservative }, { "liberal", d.leaning_slaves_liberal }, { "socialist", d.leaning_slaves_socialist } };
		if (strata == "rich") return { { "reactionary", d.leaning_strata_rich_reactionary }, { "conservative", d.leaning_strata_rich_conservative }, { "liberal", d.leaning_strata_rich_liberal } };
		if (strata == "middle") return { { "conservative", d.leaning_strata_middle_conservative }, { "liberal", d.leaning_strata_middle_liberal }, { "socialist", d.leaning_strata_middle_socialist } };
		return { { "reactionary", d.leaning_strata_other_reactionary }, { "conservative", d.leaning_strata_other_conservative }, { "liberal", d.leaning_strata_other_liberal }, { "socialist", d.leaning_strata_other_socialist } };
	}

	// Close places support each other
	double closeness(OpenVicPoliticsDefines const& defines, const double first, const double second) {
		const double d = (first - second) / defines.closeness_width;
		return 1.0 / (1.0 + d * d);
	}

	double to_double(fixed_point_t const value) {
		return static_cast<double>(value);
	}
}

namespace {
	// The ideologies the monthly update treats in its own way, found once from the identifiers (not for every pop)
	enum struct IdeologyKind : std::uint8_t {
		OTHER, REACTIONARY, CONSERVATIVE, LIBERAL, SOCIALIST, COMMUNIST, ANARCHO_LIBERAL, FASCIST
	};

	IdeologyKind ideology_kind(std::string_view identifier) {
		if (identifier == "reactionary") return IdeologyKind::REACTIONARY;
		if (identifier == "conservative") return IdeologyKind::CONSERVATIVE;
		if (identifier == "liberal") return IdeologyKind::LIBERAL;
		if (identifier == "socialist") return IdeologyKind::SOCIALIST;
		if (identifier == "communist") return IdeologyKind::COMMUNIST;
		if (identifier == "anarcho_liberal") return IdeologyKind::ANARCHO_LIBERAL;
		if (identifier == "fascist") return IdeologyKind::FASCIST;
		return IdeologyKind::OTHER;
	}

	// A group of options (the policies of a party policy group or the reforms of a reform group) the support of a pop is spread over.
	// closeness holds the closeness of every ideology to every option (option-major), it depends only on the numbers of the options.
	struct OptionGroup {
		size_t option_count = 0;
		bool is_policy = false;
		std::vector<size_t> targets;
		std::vector<double> closeness;
	};
}

void PoliticsDynamics::monthly_update(InstanceManager& instance_manager) {
	DefinitionManager const& definitions = instance_manager.definition_manager;
	OpenVicPoliticsDefines const& defines = definitions.get_define_manager().get_openvic_defines().politics;
	PoliticsManager const& politics = definitions.get_politics_manager();
	IssueManager const& issues = politics.get_issue_manager();
	PopManager const& pop_manager = definitions.get_pop_manager();
	ModifierEffectCache const& cache = definitions.get_modifier_manager().get_modifier_effect_cache();
	const Date today = instance_manager.get_today();

	auto const& ideologies = politics.get_ideology_manager().get_ideologies();
	const size_t ideology_count = ideologies.size();
	const size_t policy_count = issues.get_party_policies().size();
	const size_t reform_count = issues.get_reforms().size();

	std::vector<double> positions;
	std::vector<IdeologyKind> kinds(ideology_count, IdeologyKind::OTHER);
	for (Ideology const& ideology : ideologies) {
		positions.push_back(ideology_position(defines, ideology.get_identifier()));
		kinds[static_cast<size_t>(type_safe::get(ideology.index))] = ideology_kind(ideology.get_identifier());
	}

	// Groups that take part in the distribution of the support of a pop
	size_t counted_groups = issues.get_party_policy_groups().size();
	for (ReformGroup const& group : issues.get_reform_groups()) {
		if (!group.is_civilizing()) {
			++counted_groups;
		}
	}
	counted_groups = std::max<size_t>(counted_groups, 1);

	// Tables that only depend on the definitions: the leanings of every pop type and the closeness of the ideologies to the options
	const size_t pop_type_count = pop_manager.get_pop_type_count();
	std::vector<std::vector<std::pair<size_t, double>>> leanings_by_type(pop_type_count);
	std::vector<char> type_is_rich(pop_type_count, 0);
	for (size_t t = 0; t < pop_type_count; ++t) {
		PopType const* const type = pop_manager.get_pop_type_by_index(static_cast<pop_type_index_t>(static_cast<uint32_t>(t)));
		if (type == nullptr) {
			continue;
		}
		for (IdeologyWeight const& leaning : base_leaning(defines, type->get_identifier(), type->strata.get_identifier())) {
			for (Ideology const& ideology : ideologies) {
				if (ideology.get_identifier() == leaning.ideology) {
					leanings_by_type[t].emplace_back(static_cast<size_t>(type_safe::get(ideology.index)), leaning.weight);
				}
			}
		}
		type_is_rich[t] = type->strata.get_identifier() == "rich";
	}

	std::vector<OptionGroup> option_groups;
	const auto add_group = [&](const bool is_policy, const size_t option_count, auto&& option_index_at) -> void {
		OptionGroup group;
		group.option_count = option_count;
		group.is_policy = is_policy;
		group.targets.reserve(option_count);
		group.closeness.reserve(option_count * ideology_count);
		for (size_t option = 0; option < option_count; ++option) {
			group.targets.push_back(option_index_at(option));
			const double position = option_count > 1 ? static_cast<double>(option) / static_cast<double>(option_count - 1) : 0.5;
			for (size_t i = 0; i < ideology_count; ++i) {
				group.closeness.push_back(closeness(defines, positions[i], position));
			}
		}
		option_groups.push_back(std::move(group));
	};
	for (PartyPolicyGroup const& group : issues.get_party_policy_groups()) {
		auto const& options = group.get_party_policies();
		add_group(true, options.size(), [&](size_t k) { return static_cast<size_t>(type_safe::get(options[k].get().index)); });
	}
	for (ReformGroup const& group : issues.get_reform_groups()) {
		if (group.is_civilizing()) {
			continue;
		}
		auto const& options = group.get_reforms();
		add_group(false, options.size(), [&](size_t k) { return static_cast<size_t>(type_safe::get(options[k].get().index)); });
	}

	const size_t strata_count = pop_manager.get_strata_count();

	// The countries are independent (each one only writes its own pops, provinces and values), so they are worked out in parallel;
	// what is shared (the winners of the elections, then the notifications) is applied afterwards in the order of the countries
	std::vector<CountryParty const*> election_winners(instance_manager.get_country_instance_manager().get_country_instances().size(), nullptr);

	const auto update_country = [&](CountryInstance& country) -> void {
		if (!country.exists() || country.is_rebel_country() || country.get_owned_provinces().empty()) {
			return;
		}
		// Reused for every pop and every group of this country instead of being made again each time
		std::vector<fixed_point_t> ideology_support(ideology_count), policy_support(policy_count), reform_support(reform_count);
		std::vector<double> shares(ideology_count, 0.0);
		std::vector<double> weights;
		fixed_point_map_t<CountryParty const*> votes;
		std::vector<double> eligible_by_strata(strata_count, 0.0);
		const bool civilised = country.is_civilised();

		// Ideologies that exist for the country now
		std::vector<bool> ideology_active(ideology_count, true);
		for (Ideology const& ideology : ideologies) {
			const size_t i = static_cast<size_t>(type_safe::get(ideology.index));
			if (ideology.spawn_date.has_value() && today < *ideology.spawn_date) {
				ideology_active[i] = false;
			}
			if (!civilised && !ideology.is_allowed_for_uncivilised) {
				ideology_active[i] = false;
			}
		}

		// Active parties, and how many there are for each ideology
		std::vector<CountryParty const*> active_parties;
		std::vector<size_t> parties_per_ideology(ideology_count, 0);
		for (CountryParty const& party : country.country_definition.get_parties()) {
			if (party.ideology != nullptr && party.start_date <= today && today <= party.end_date) {
				active_parties.push_back(&party);
				++parties_per_ideology[static_cast<size_t>(type_safe::get(party.ideology->index))];
			}
		}
		// The ideologies without a party (their voters spread over all parties), in the order of the ideologies
		std::vector<size_t> ideologies_without_party;
		for (size_t i = 0; i < ideology_count; ++i) {
			if (parties_per_ideology[i] == 0) {
				ideologies_without_party.push_back(i);
			}
		}

		// The share of each strata that may vote by the reforms (the same for all its pops of the country)
		for (size_t s = 0; s < strata_count; ++s) {
			Strata const* const strata = pop_manager.get_strata_by_index(static_cast<strata_index_t>(static_cast<uint32_t>(s)));
			eligible_by_strata[s] = strata == nullptr ? 0.0 : std::clamp(
				to_double(country.get_modifier_effect_value(*cache.get_strata_effects(*strata).get_vote())), 0.0, 1.0
			);
		}

		// Who may vote: a share of each strata by the reforms, and the cultures the voting rules allow
		RuleSet const& rules = country.get_rule_set();
		const bool all_cultures = rules.is_all_voting_set();
		const bool accepted_cultures = rules.is_culture_voting_set();
		const bool primary_only = rules.is_primary_culture_voting_set() && !all_cultures && !accepted_cultures;

		std::vector<fixed_point_t> upper_house(ideology_count, fixed_point_t::_0);
		fixed_point_t upper_house_total = fixed_point_t::_0;
		const bool rich_only = rules.is_rich_only_set();

		fixed_point_map_t<CountryParty const*> party_totals;

		for (ProvinceInstance* province : country.get_owned_provinces()) {
			for (Pop& pop : province->get_mutable_pops()) {
				const double size = static_cast<double>(type_safe::get(pop.get_size()));
				if (size <= 0.0 || pop.get_rebel_type() != nullptr) {
					continue;
				}

				PopType const& type = pop.get_type();
				const size_t type_index = static_cast<size_t>(type_safe::get(type.index));
				const double consciousness = std::clamp(to_double(pop.get_consciousness()) / 10.0, 0.0, 1.0);
				const double militancy = std::clamp(to_double(pop.get_militancy()) / 10.0, 0.0, 1.0);
				const double literacy = std::clamp(to_double(pop.get_literacy()), 0.0, 1.0);

				// ---- ideologies of the pop
				std::fill(shares.begin(), shares.end(), 0.0);
				double share_total = 0.0;
				for (auto const& [ideology_index, weight] : leanings_by_type[type_index]) {
					shares[ideology_index] += weight;
				}
				for (Ideology const& ideology : ideologies) {
					const size_t i = static_cast<size_t>(type_safe::get(ideology.index));
					double w = shares[i] + defines.ideology_base_weight;
					switch (kinds[i]) {
					case IdeologyKind::REACTIONARY: w *= 1.0 - defines.reactionary_consciousness_penalty * consciousness; break;
					case IdeologyKind::CONSERVATIVE: w *= 1.0 - defines.conservative_consciousness_penalty * consciousness; break;
					case IdeologyKind::LIBERAL: w *= 1.0 + defines.liberal_consciousness_bonus * consciousness + defines.liberal_literacy_bonus * literacy; break;
					case IdeologyKind::SOCIALIST: w *= defines.socialist_base + defines.socialist_consciousness_bonus * consciousness + defines.socialist_militancy_bonus * militancy; break;
					case IdeologyKind::COMMUNIST: w *= defines.communist_base + defines.communist_consciousness_militancy_bonus * consciousness * militancy; break;
					case IdeologyKind::ANARCHO_LIBERAL: w *= defines.anarcho_liberal_base + defines.anarcho_liberal_militancy_bonus * militancy; break;
					case IdeologyKind::FASCIST: w *= defines.fascist_base + defines.fascist_militancy_bonus * militancy * (1.0 - consciousness); break;
					case IdeologyKind::OTHER: break;
					}
					if (!ideology_active[i]) w = 0.0;
					shares[i] = w;
					share_total += w;
				}
				if (share_total <= 0.0) {
					share_total = 1.0;
				}
				for (double& share : shares) {
					share /= share_total;
				}
				for (size_t i = 0; i < ideology_count; ++i) {
					ideology_support[i] = fixed_point_t::parse_unsafe(static_cast<float>(shares[i] * size));
				}

				// ---- support of the options of the reforms and of the party policies
				std::fill(policy_support.begin(), policy_support.end(), fixed_point_t::_0);
				std::fill(reform_support.begin(), reform_support.end(), fixed_point_t::_0);
				for (OptionGroup const& group : option_groups) {
					weights.assign(group.option_count, 0.0);
					double weight_total = 0.0;
					for (size_t option = 0; option < group.option_count; ++option) {
						double const* const row = group.closeness.data() + option * ideology_count;
						for (size_t i = 0; i < ideology_count; ++i) {
							weights[option] += shares[i] * row[i];
						}
						weight_total += weights[option];
					}
					if (weight_total <= 0.0) {
						continue;
					}
					std::vector<fixed_point_t>& target = group.is_policy ? policy_support : reform_support;
					for (size_t option = 0; option < group.option_count; ++option) {
						target[group.targets[option]] = fixed_point_t::parse_unsafe(
							static_cast<float>(weights[option] / weight_total * size / static_cast<double>(counted_groups))
						);
					}
				}
				pop.set_political_support(ideology_support, policy_support, reform_support);

				// ---- votes
				double eligible = eligible_by_strata[static_cast<size_t>(type_safe::get(type.strata.index))];
				const auto status = pop.get_culture_status();
				if (primary_only && status != Pop::culture_status_t::PRIMARY) {
					eligible = 0.0;
				} else if (!all_cultures && accepted_cultures && status == Pop::culture_status_t::UNACCEPTED) {
					eligible = 0.0;
				} else if (!all_cultures && !accepted_cultures && !primary_only && status == Pop::culture_status_t::UNACCEPTED) {
					eligible = 0.0;
				}

				votes.clear();
				if (!active_parties.empty() && eligible > 0.0) {
					double no_party_share = 0.0;
					for (const size_t i : ideologies_without_party) {
						no_party_share += shares[i];
					}
					for (CountryParty const* party : active_parties) {
						const size_t i = static_cast<size_t>(type_safe::get(party->ideology->index));
						double share = shares[i] / static_cast<double>(parties_per_ideology[i]);
						// voters of ideologies without a party spread over all parties
						share += no_party_share / static_cast<double>(active_parties.size());
						const fixed_point_t amount = fixed_point_t::parse_unsafe(static_cast<float>(share * size * eligible));
						votes[party] = amount;
						party_totals[party] += amount;
					}
				}
				pop.set_party_votes(votes);

				// ---- upper house (the same amounts as the ideology support above)
				if (!rich_only || type_is_rich[type_index]) {
					for (size_t i = 0; i < ideology_count; ++i) {
						const fixed_point_t part = ideology_support[i];
						upper_house[i] += part;
						upper_house_total += part;
					}
				}
			}
		}

		// ---- the upper house
		if (rules.is_same_as_ruling_party_set() && country.get_ruling_party_untracked() != nullptr
			&& country.get_ruling_party_untracked()->ideology != nullptr) {
			for (Ideology const& ideology : ideologies) {
				country.set_upper_house_proportion(
					ideology, &ideology == country.get_ruling_party_untracked()->ideology ? fixed_point_t::_1 : fixed_point_t::_0
				);
			}
		} else if (upper_house_total > fixed_point_t::_0) {
			for (Ideology const& ideology : ideologies) {
				country.set_upper_house_proportion(ideology, upper_house[static_cast<size_t>(type_safe::get(ideology.index))] / upper_house_total);
			}
		}

		// ---- elections
		GovernmentType const* const government = country.get_government_type_untracked();
		if (government != nullptr && government->holds_elections && !active_parties.empty()) {
			if (country.get_last_election() == Date {}) {
				country.set_last_election(today);
			} else if (today - country.get_last_election() >= government->term_duration) {
				CountryParty const* winner = nullptr;
				fixed_point_t best = fixed_point_t::_0;
				for (CountryParty const* party : active_parties) {
					const auto it = party_totals.find(party);
					const fixed_point_t total = it != party_totals.end() ? it->second : fixed_point_t::_0;
					if (winner == nullptr || total > best) {
						winner = party;
						best = total;
					}
				}
				country.set_last_election(today);
				if (winner != nullptr && country.get_ruling_party_untracked() != winner) {
					election_winners[static_cast<size_t>(type_safe::get(country.index))] = winner;
				}
			}
		}
	};

	instance_manager.get_country_instance_manager().for_each_country_parallel(update_country);

	for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		CountryParty const* const winner = election_winners[static_cast<size_t>(type_safe::get(country.index))];
		if (winner != nullptr) {
			SPDLOG_INFO("Election in {}: {} comes to power", country.get_identifier(), winner->get_identifier());
			instance_manager.get_notification_log().add(today, &country, "election", winner->get_identifier());
			country.set_ruling_party(*winner);
		}
	}
}

void PoliticsDynamics::get_group_support(CountryInstance const& country, ReformGroup const& group, fixed_point_t* out_supports) {
	auto const& options = group.get_reforms();
	fixed_point_t total = fixed_point_t::_0;
	for (size_t k = 0; k < options.size(); ++k) {
		out_supports[k] = country.get_supporter_equivalents_by_reform()[options[k].get().index];
		total += out_supports[k];
	}
	for (size_t k = 0; k < options.size(); ++k) {
		out_supports[k] = total > fixed_point_t::_0 ? out_supports[k] / total : fixed_point_t::_0;
	}
}

PoliticsDynamics::ReformStatus PoliticsDynamics::get_reform_status(
	InstanceManager const& instance_manager, CountryInstance const& country, Reform const& reform
) {
	OpenVicPoliticsDefines const& defines = instance_manager.definition_manager.get_define_manager().get_openvic_defines().politics;
	ReformStatus status;
	ReformGroup const& group = reform.group;
	auto const& options = group.get_reforms();

	Reform const* const current = country.get_reforms().at(group);
	if (current == &reform) {
		status.reason = "Уже действует";
		return status;
	}

	size_t reform_position = 0, current_position = 0;
	for (size_t k = 0; k < options.size(); ++k) {
		if (&options[k].get() == &reform) {
			reform_position = k;
		}
		if (&options[k].get() == current) {
			current_position = k;
		}
	}

	std::vector<fixed_point_t> supports(options.size(), fixed_point_t::_0);
	get_group_support(country, group, supports.data());
	status.support = supports[reform_position];
	const fixed_point_t current_support = current != nullptr ? supports[current_position] : fixed_point_t::_0;

	if (group.is_civilizing() && !country.is_civilised()) {
		status.reason = "Нужна вестернизация";
		return status;
	}
	if (group.is_ordered && current != nullptr) {
		const size_t distance = reform_position > current_position ? reform_position - current_position : current_position - reform_position;
		if (distance != 1) {
			status.reason = "Можно переходить только на соседний шаг";
			return status;
		}
	}
	if (current != nullptr && reform_position < current_position) {
		// Taking a reform back needs a government of the reaction
		CountryParty const* const ruling = country.get_ruling_party_untracked();
		const std::string_view ideology = ruling != nullptr && ruling->ideology != nullptr ? ruling->ideology->get_identifier() : "";
		if (ideology != "reactionary" && ideology != "fascist") {
			status.reason = "Отмена реформы возможна только при реакционном правительстве";
			return status;
		}
	}
	if (status.support <= current_support) {
		status.reason = "Поддержка людей ниже, чем у действующего варианта";
		return status;
	}

	// Political reforms also need the upper house
	if (std::string_view { group.reform_type.get_identifier() }.find("political") != std::string_view::npos) {
		auto const& ideologies = instance_manager.definition_manager.get_politics_manager().get_ideology_manager().get_ideologies();
		const double count = static_cast<double>(options.size());
		const double target_position = count > 1.0 ? static_cast<double>(reform_position) / (count - 1.0) : 0.5;
		const double current_pos = count > 1.0 ? static_cast<double>(current_position) / (count - 1.0) : 0.5;
		double accept_new = 0.0, accept_current = 0.0;
		for (Ideology const& ideology : ideologies) {
			const double share = to_double(country.get_upper_house_proportion_by_ideology().at(ideology));
			accept_new += share * closeness(defines, ideology_position(defines, ideology.get_identifier()), target_position);
			accept_current += share * closeness(defines, ideology_position(defines, ideology.get_identifier()), current_pos);
		}
		status.upper_house = fixed_point_t::parse_unsafe(static_cast<float>(accept_new));
		if (accept_new <= accept_current) {
			status.reason = "Верхняя палата против";
			return status;
		}
	}

	status.can_enact = true;
	return status;
}

// ---- Unrest and rebels ----
#include <unordered_map>
#include <unordered_set>

#include "openvic-simulation/military/UnitInstanceGroup.hpp"

namespace {
	CountryInstance* find_rebel_country(InstanceManager& instance_manager) {
		for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
			if (country.is_rebel_country()) {
				return &country;
			}
		}
		return nullptr;
	}

	ArmyInstance* rebel_army_in(ProvinceInstance& province) {
		for (ArmyInstance& army : province.get_armies()) {
			if (army.get_country().is_rebel_country()) {
				return &army;
			}
		}
		return nullptr;
	}
}

memory::vector<RebelDynamics::Uprising> RebelDynamics::get_uprisings(InstanceManager const& instance_manager, CountryInstance const& country) {
	(void)instance_manager;
	memory::vector<Uprising> result;
	for (ProvinceInstance* province : country.get_owned_provinces()) {
		for (ArmyInstance const& army : province->get_armies()) {
			if (army.get_country().is_rebel_country()) {
				result.push_back({ province, static_cast<std::int32_t>(army.get_unit_count()), army.get_strength_proportion() });
			}
		}
	}
	return result;
}

void RebelDynamics::monthly_update(InstanceManager& instance_manager) {
	State& state = instance_manager.get_rebel_state();
	OpenVicPoliticsDefines const& defines = instance_manager.definition_manager.get_define_manager().get_openvic_defines().politics;
	CountryInstance* const rebels = find_rebel_country(instance_manager);
	UnitInstanceManager& unit_manager = instance_manager.get_unit_instance_manager();
	const Date today = instance_manager.get_today();

	for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		if (!country.exists() || country.is_rebel_country() || country.get_owned_provinces().empty()) {
			continue;
		}

		fixed_point_t exhaustion = fixed_point_t::_0;
		if (country.get_war_exhaustion_max() > fixed_point_t::_0) {
			exhaustion = country.get_war_exhaustion() / country.get_war_exhaustion_max();
		}

		bool has_uprising = false;
		for (ProvinceInstance* province : country.get_owned_provinces()) {
			if (province->province_definition.is_water()) {
				continue;
			}

			// ---- militancy and consciousness of the pops
			double militant_people = 0.0;
			double all_people = 0.0;
			for (Pop& pop : province->get_mutable_pops()) {
				const double size = static_cast<double>(type_safe::get(pop.get_size()));
				if (size <= 0.0 || pop.get_rebel_type() != nullptr) {
					continue;
				}
				const double life = to_double(pop.get_life_needs_fulfilled());
				double pressure = std::max(0.0, defines.unrest_life_needs_threshold - life) * defines.unrest_life_needs_weight
					+ to_double(pop.get_unemployment_fraction()) * defines.unrest_unemployment_weight;
				pressure += to_double(exhaustion) * defines.unrest_war_exhaustion_weight;
				double militancy = to_double(pop.get_militancy()) + (pressure - defines.militancy_pressure_offset) * defines.militancy_growth;
				militancy = std::clamp(militancy, 0.0, 10.0);
				pop.set_militancy(fixed_point_t::parse_unsafe(static_cast<float>(militancy)));
				double consciousness = to_double(pop.get_consciousness()) + to_double(pop.get_literacy()) * defines.consciousness_literacy_growth - defines.consciousness_decay;
				pop.set_consciousness(fixed_point_t::parse_unsafe(static_cast<float>(std::clamp(consciousness, 0.0, 10.0))));

				all_people += size;
				if (militancy > defines.militant_level) {
					militant_people += size * (militancy - defines.militant_level) / (10.0 - defines.militant_level);
				}
			}

			ArmyInstance* const rebel_army = rebel_army_in(*province);
			if (rebel_army != nullptr) {
				// The government of an AI country puts the rising down after some time
				if (country.is_ai() && state.uprising_started.contains(province)
					&& (today - state.uprising_started[province]).to_int() >= defines.ai_suppression_days) {
					unit_manager.destroy_group(*rebel_army);
					state.uprising_ended[province] = today;
				} else {
					has_uprising = true;
					continue;
				}
			}
			// Provinces that have risen lately are calm for a while
			if (const auto ended = state.uprising_ended.find(province);
				ended != state.uprising_ended.end() && (today - ended->second).to_int() < defines.uprising_cooldown_days) {
				continue;
			}

			// ---- a rising
			if (rebels != nullptr && province->get_controller() == &country && militant_people >= defines.min_militant_people
				&& all_people > 0.0 && militant_people * 100.0 >= all_people * (defines.min_militant_share * 100.0)) {
				RegimentType const* regiment_type = nullptr;
				for (RegimentType const& type : instance_manager.definition_manager.get_military_manager().get_unit_type_manager().get_regiment_types()) {
					if (country.is_unit_type_unlocked(type)) {
						regiment_type = &type;
						break;
					}
				}
				if (regiment_type != nullptr) {
					const size_t count = static_cast<size_t>(std::clamp<double>(
						militant_people / defines.people_per_rebel_regiment, 1.0, defines.max_rebel_regiments
					));
					if (unit_manager.create_army_with_regiments(*rebels, *province, *regiment_type, count, "Rebels") != nullptr) {
						rebels->set_at_war_with(country, true);
						state.active_uprisings.insert(province);
						state.uprising_started[province] = today;
						has_uprising = true;
						SPDLOG_INFO("Uprising in {} ({}): {} rebel regiments", province->get_identifier(), country.get_identifier(), count);
						instance_manager.get_notification_log().add(today, &country, "uprising", province->get_identifier());
					}
				}
			}
		}

		// ---- the war with the rebels ends when there are no rebel armies left in the country
		if (!has_uprising && rebels != nullptr && country.is_at_war_with(*rebels)) {
			rebels->set_at_war_with(country, false);
		}
	}

	// ---- beaten uprisings: the province returns to the owner, the people calm down
	for (auto it = state.active_uprisings.begin(); it != state.active_uprisings.end();) {
		ProvinceInstance* province = const_cast<ProvinceInstance*>(*it);
		if (rebel_army_in(*province) == nullptr) {
			if (province->get_owner() != nullptr && province->get_controller() != province->get_owner()
				&& province->get_controller() != nullptr && province->get_controller()->is_rebel_country()) {
				province->set_controller(province->get_owner());
			}
			for (Pop& pop : province->get_mutable_pops()) {
				const double militancy = std::max(0.0, to_double(pop.get_militancy()) - defines.uprising_end_militancy_drop);
				pop.set_militancy(fixed_point_t::parse_unsafe(static_cast<float>(militancy)));
			}
			state.uprising_ended[province] = today;
			it = state.active_uprisings.erase(it);
		} else {
			++it;
		}
	}
	(void)today;
}
